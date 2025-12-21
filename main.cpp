#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <stdlib.h>
#include <string.h>

#define COLOUR_WHITE 1
#define COLOUR_BLACK 0
#define TONES_END 0x8000

#include "game/Game.h"
#include "game/Draw.h"
#include "game/FixedMath.h"
#include "game/Platform.h"
#include "game/Defines.h"
#include "game/Sounds.h"

#define DISPLAY_WIDTH 128
#define DISPLAY_HEIGHT 64
#define BUFFER_SIZE (DISPLAY_WIDTH * DISPLAY_HEIGHT / 8)
#define TARGET_FRAMERATE 30
#define DISPLAY_FPS 15

typedef struct {
    uint8_t screen_buffer[BUFFER_SIZE];
    uint8_t xbm_buffers[2][BUFFER_SIZE];
    FuriMutex* mutex;
    ViewPort* view_port;
    FuriTimer* timer;
    volatile uint8_t xbm_read_idx;
    volatile uint8_t input_state;
    volatile bool exit_requested;
    volatile bool audio_enabled;
} FlipperState;

static FlipperState* g_state = NULL;

typedef struct {
    const uint16_t* pattern;
} SoundRequest;

static FuriMessageQueue* g_sound_queue = NULL;
static FuriThread* g_sound_thread = NULL;
static volatile bool g_sound_thread_running = false;
static const float kSoundVolume = 1.0f;
static const uint32_t kToneTickHz = 850;

static inline uint32_t arduboy_ticks_to_ms(uint16_t ticks) {
    return (uint32_t)((ticks * 1000u + (kToneTickHz/2)) / kToneTickHz);
}

static int32_t sound_thread_fn(void* ctx) {
    UNUSED(ctx);
    SoundRequest req;
    while(g_sound_thread_running) {
        if(furi_message_queue_get(g_sound_queue, &req, 50) != FuriStatusOk) {
            continue;
        }
        if(!g_state || !g_state->audio_enabled || !req.pattern) {
            continue;
        }
        if(!furi_hal_speaker_acquire(50)) {
            continue;
        }
        const uint16_t* p = req.pattern;
        while(g_sound_thread_running && g_state && g_state->audio_enabled) {
            SoundRequest new_req;
            if(furi_message_queue_get(g_sound_queue, &new_req, 0) == FuriStatusOk) {
                p = new_req.pattern ? new_req.pattern : p;
            }
            uint16_t freq = *p++;
            if(freq == TONES_END) break;
            uint16_t dur_ticks = *p++;
            uint32_t dur_ms = arduboy_ticks_to_ms(dur_ticks);
            if(dur_ms == 0) dur_ms = 1;
            if(freq == 0) {
                furi_hal_speaker_stop();
                furi_delay_ms(dur_ms);
            } else {
                furi_hal_speaker_start((float)freq, kSoundVolume);
                furi_delay_ms(dur_ms);
                furi_hal_speaker_stop();
            }
        }
        furi_hal_speaker_stop();
        furi_hal_speaker_release();
    }
    if(furi_hal_speaker_is_mine()) {
        furi_hal_speaker_stop();
        furi_hal_speaker_release();
    }
    return 0;
}

static void sound_system_init() {
    if(g_sound_queue || g_sound_thread) return;
    g_sound_queue = furi_message_queue_alloc(4, sizeof(SoundRequest));
    g_sound_thread = furi_thread_alloc();
    furi_thread_set_name(g_sound_thread, "GameSound");
    furi_thread_set_stack_size(g_sound_thread, 1024);
    furi_thread_set_priority(g_sound_thread, FuriThreadPriorityNormal);
    furi_thread_set_callback(g_sound_thread, sound_thread_fn);
    g_sound_thread_running = true;
    furi_thread_start(g_sound_thread);
}

static void sound_system_deinit() {
    if(!g_sound_thread) return;
    g_sound_thread_running = false;
    furi_thread_join(g_sound_thread);
    furi_thread_free(g_sound_thread);
    g_sound_thread = NULL;
    if(g_sound_queue) {
        furi_message_queue_free(g_sound_queue);
        g_sound_queue = NULL;
    }
}

void Platform::PlaySound(const uint16_t* audioPattern) {
    if(!g_state || !g_state->audio_enabled || !audioPattern || !g_sound_queue) return;
    SoundRequest req = {.pattern = audioPattern};
    if(furi_message_queue_put(g_sound_queue, &req, 0) != FuriStatusOk) {
        SoundRequest dummy;
        (void)furi_message_queue_get(g_sound_queue, &dummy, 0);
        (void)furi_message_queue_put(g_sound_queue, &req, 0);
    }
}

bool Platform::IsAudioEnabled() {
    return g_state && g_state->audio_enabled;
}

void Platform::SetAudioEnabled(bool enabled) {
    if(!g_state) return;
    bool was_enabled = g_state->audio_enabled;
    g_state->audio_enabled = enabled;
    if(enabled && !was_enabled) {
        sound_system_init();
    } else if(!enabled && was_enabled) {
        sound_system_deinit();
    }
}

uint8_t Platform::GetInput() {
    return g_state ? g_state->input_state : 0;
}

void Platform::ExpectLoadDelay() {
}

void Platform::SetLED(uint8_t r, uint8_t g, uint8_t b) {
    NotificationApp* n = (NotificationApp*)furi_record_open(RECORD_NOTIFICATION);
    if(r) notification_message(n, &sequence_set_red_255);
    else notification_message(n, &sequence_reset_red);
    if(g) notification_message(n, &sequence_set_green_255);
    else notification_message(n, &sequence_reset_green);
    if(b) notification_message(n, &sequence_set_blue_255);
    else notification_message(n, &sequence_reset_blue);
    furi_record_close(RECORD_NOTIFICATION);
}

static inline void set_pixel(int16_t x, int16_t y, bool color) {
    if(!g_state || x < 0 || y < 0 || x >= DISPLAY_WIDTH || y >= DISPLAY_HEIGHT) return;
    uint16_t idx = x + (y >> 3) * DISPLAY_WIDTH;
    uint8_t mask = 1 << (y & 7);
    if(!color) g_state->screen_buffer[idx] |= mask;
    else g_state->screen_buffer[idx] &= ~mask;
}

void Platform::PutPixel(uint8_t x, uint8_t y, uint8_t color) {
    set_pixel(x, y, color);
}

void Platform::FillScreen(uint8_t color) {
    if(!g_state) return;
    memset(g_state->screen_buffer, color ? 0xFF : 0x00, BUFFER_SIZE);
}

uint8_t* Platform::GetScreenBuffer() {
    return g_state ? g_state->screen_buffer : NULL;
}

void Platform::DrawVLine(uint8_t x, int8_t y0, int8_t y1, uint8_t pattern) {
    if(y0 > y1) {
        int8_t temp = y0;
        y0 = y1;
        y1 = temp;
    }
    for(int y = y0; y <= y1; y++) {
        if(pattern & (1 << (y & 7))) {
            set_pixel(x, y, 1);
        }
    }
}

void Platform::DrawBitmap(int16_t x, int16_t y, const uint8_t* bmp) {
    if(!bmp) return;
    uint8_t w = bmp[0];
    uint8_t h = bmp[1];
    const uint8_t* data = bmp + 2;
    uint8_t pages = (h + 7) >> 3;
    for(uint8_t page = 0; page < pages; page++) {
        for(uint8_t i = 0; i < w; i++) {
            uint8_t byte = data[i + page * w];
            for(uint8_t b = 0; b < 8; b++) {
                uint8_t py = page * 8 + b;
                if(py >= h) break;
                if(byte & (1 << b)) {
                    set_pixel(x + i, y + py, COLOUR_WHITE);
                }
            }
        }
    }
}

void Platform::DrawSolidBitmap(int16_t x, int16_t y, const uint8_t* bmp) {
    if(!bmp) return;
    uint8_t w = bmp[0];
    uint8_t h = bmp[1];
    const uint8_t* data = bmp + 2;
    uint8_t pages = (h + 7) >> 3;
    for(uint8_t page = 0; page < pages; page++) {
        for(uint8_t i = 0; i < w; i++) {
            uint8_t byte = data[i + page * w];
            for(uint8_t b = 0; b < 8; b++) {
                uint8_t py = page * 8 + b;
                if(py >= h) break;
                bool pixel = byte & (1 << b);
                set_pixel(x + i, y + py, pixel ? COLOUR_WHITE : COLOUR_BLACK);
            }
        }
    }
}

void Platform::DrawSprite(int16_t x, int16_t y, const uint8_t* bmp, uint8_t frame) {
    if(!bmp) return;
    uint8_t w = bmp[0];
    uint8_t h = bmp[1];
    uint8_t pages = (h + 7) >> 3;
    uint16_t frame_size = w * pages;
    const uint8_t* data = bmp + 2 + (uint32_t)frame * frame_size * 2;
    for(uint8_t page = 0; page < pages; page++) {
        for(uint8_t i = 0; i < w; i++) {
            uint16_t idx = (i + page * w) * 2;
            uint8_t s = data[idx + 0];
            uint8_t m = data[idx + 1];
            for(uint8_t b = 0; b < 8; b++) {
                uint8_t py = page * 8 + b;
                if(py >= h) break;
                uint8_t bit = 1 << b;
                if(m & bit) {
                    set_pixel(x + i, y + py, (s & bit) ? COLOUR_WHITE : COLOUR_BLACK);
                }
            }
        }
    }
}

void Platform::DrawSprite(int16_t x, int16_t y, const uint8_t* bmp, const uint8_t* mask, uint8_t frame, uint8_t mask_frame) {
    if(!bmp) return;
    uint8_t w = bmp[0];
    uint8_t h = bmp[1];
    uint8_t pages = (h + 7) >> 3;
    uint16_t frame_size = w * pages;
    const uint8_t* sprite_data = bmp + 2 + (uint32_t)frame * frame_size;
    if(mask) {
        const uint8_t* mask_data = mask + (uint32_t)mask_frame * frame_size;
        for(uint8_t page = 0; page < pages; page++) {
            for(uint8_t i = 0; i < w; i++) {
                uint8_t s = sprite_data[i + page * w];
                uint8_t m = mask_data[i + page * w];
                for(uint8_t b = 0; b < 8; b++) {
                    uint8_t py = page * 8 + b;
                    if(py >= h) break;
                    uint8_t bit = 1 << b;
                    if(m & bit) {
                        set_pixel(x + i, y + py, (s & bit) ? COLOUR_WHITE : COLOUR_BLACK);
                    }
                }
            }
        }
    } else {
        const uint8_t* data = bmp + 2 + (uint32_t)frame * frame_size * 2;
        for(uint8_t page = 0; page < pages; page++) {
            for(uint8_t i = 0; i < w; i++) {
                uint16_t idx = (i + page * w) * 2;
                uint8_t s = data[idx + 0];
                uint8_t m = data[idx + 1];
                for(uint8_t b = 0; b < 8; b++) {
                    uint8_t py = page * 8 + b;
                    if(py >= h) break;
                    uint8_t bit = 1 << b;
                    if(m & bit) {
                        set_pixel(x + i, y + py, (s & bit) ? COLOUR_WHITE : COLOUR_BLACK);
                    }
                }
            }
        }
    }
}

static void convert_screen_to_xbm_into(const uint8_t* screen, uint8_t* dst) {
    constexpr int XBM_STRIDE = DISPLAY_WIDTH / 8;
    for(int page = 0; page < DISPLAY_HEIGHT / 8; page++) {
        const int page_offset = page * DISPLAY_WIDTH;
        const int y_base = page * 8;

        for(int x = 0; x < DISPLAY_WIDTH; x += 8) {
            uint8_t c0 = screen[page_offset + x + 0] ^ 0x00;
            uint8_t c1 = screen[page_offset + x + 1] ^ 0x00;
            uint8_t c2 = screen[page_offset + x + 2] ^ 0x00;
            uint8_t c3 = screen[page_offset + x + 3] ^ 0x00;
            uint8_t c4 = screen[page_offset + x + 4] ^ 0x00;
            uint8_t c5 = screen[page_offset + x + 5] ^ 0x00;
            uint8_t c6 = screen[page_offset + x + 6] ^ 0x00;
            uint8_t c7 = screen[page_offset + x + 7] ^ 0x00;

            const int dst_xbyte = x / 8;
            int d = (y_base * XBM_STRIDE) + dst_xbyte;

            for(int b = 0; b < 8; b++) {
                uint8_t out =
                    (((c0 >> b) & 1) << 0) |
                    (((c1 >> b) & 1) << 1) |
                    (((c2 >> b) & 1) << 2) |
                    (((c3 >> b) & 1) << 3) |
                    (((c4 >> b) & 1) << 4) |
                    (((c5 >> b) & 1) << 5) |
                    (((c6 >> b) & 1) << 6) |
                    (((c7 >> b) & 1) << 7);

                dst[d] = out;
                d += XBM_STRIDE;
            }
        }
    }
}

static void render_callback(Canvas* canvas, void* ctx) {
    FlipperState* state = (FlipperState*)ctx;
    if(!state || !canvas) return;
    uint8_t idx = state->xbm_read_idx;
    canvas_draw_xbm(canvas, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, state->xbm_buffers[idx]);
}

static void input_callback(InputEvent* event, void* ctx) {
    FlipperState* state = (FlipperState*)ctx;
    if(!state || !event) return;
    if(event->type == InputTypePress || event->type == InputTypeRepeat) {
        switch(event->key) {
        case InputKeyUp: state->input_state |= INPUT_UP; break;
        case InputKeyDown: state->input_state |= INPUT_DOWN; break;
        case InputKeyLeft: state->input_state |= INPUT_LEFT; break;
        case InputKeyRight: state->input_state |= INPUT_RIGHT; break;
        case InputKeyOk: state->input_state |= INPUT_B; break;
        case InputKeyBack: state->exit_requested = true; break;
        default: break;
        }
    } else if(event->type == InputTypeRelease) {
        switch(event->key) {
        case InputKeyUp: state->input_state &= ~INPUT_UP; break;
        case InputKeyDown: state->input_state &= ~INPUT_DOWN; break;
        case InputKeyLeft: state->input_state &= ~INPUT_LEFT; break;
        case InputKeyRight: state->input_state &= ~INPUT_RIGHT; break;
        case InputKeyOk: state->input_state &= ~INPUT_B; break;
        default: break;
        }
    }
}

static void timer_callback(void* ctx) {
    FlipperState* state = (FlipperState*)ctx;
    if(!state) return;
    static uint32_t logic_counter = 0;
    const uint32_t convert_ratio = TARGET_FRAMERATE / DISPLAY_FPS;
    if(furi_mutex_acquire(state->mutex, 0) != FuriStatusOk) return;
    Game::Tick();
    Game::Draw();
    logic_counter++;
    if(logic_counter >= convert_ratio) {
        uint8_t read_idx = state->xbm_read_idx;
        uint8_t write_idx = 1 - read_idx;
        convert_screen_to_xbm_into(state->screen_buffer, state->xbm_buffers[write_idx]);
        state->xbm_read_idx = write_idx;
        view_port_update(state->view_port);
        logic_counter = 0;
    }
    furi_mutex_release(state->mutex);
}

extern "C" int32_t arduboy3d_app(void* p) {
    UNUSED(p);
    g_state = (FlipperState*)malloc(sizeof(FlipperState));
    if(!g_state) return -1;
    memset(g_state, 0, sizeof(FlipperState));
    g_state->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    if(!g_state->mutex) {
        free(g_state);
        g_state = NULL;
        return -1;
    }
    g_state->exit_requested = false;
    g_state->audio_enabled = false;
    g_state->xbm_read_idx = 0;
    memset(g_state->screen_buffer, 0x00, BUFFER_SIZE);
    memset(g_state->xbm_buffers, 0x00, sizeof(g_state->xbm_buffers));
    Game::Init();
    furi_mutex_acquire(g_state->mutex, FuriWaitForever);
    Game::Draw();
    convert_screen_to_xbm_into(g_state->screen_buffer, g_state->xbm_buffers[0]);
    furi_mutex_release(g_state->mutex);
    g_state->view_port = view_port_alloc();
    view_port_draw_callback_set(g_state->view_port, render_callback, g_state);
    view_port_input_callback_set(g_state->view_port, input_callback, g_state);
    Gui* gui = (Gui*)furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, g_state->view_port, GuiLayerFullscreen);
    g_state->timer = furi_timer_alloc(timer_callback, FuriTimerTypePeriodic, g_state);
    const uint32_t tick_hz = furi_kernel_get_tick_frequency();
    uint32_t period = tick_hz / TARGET_FRAMERATE;
    if(period == 0) period = 1;
    furi_timer_start(g_state->timer, period);
    while(!g_state->exit_requested) {
        furi_delay_ms(100);
    }
    Platform::SetAudioEnabled(false);
    if(g_state->timer) {
        furi_timer_stop(g_state->timer);
        furi_timer_free(g_state->timer);
    }
    gui_remove_view_port(gui, g_state->view_port);
    view_port_free(g_state->view_port);
    furi_record_close(RECORD_GUI);
    if(g_state->mutex) furi_mutex_free(g_state->mutex);
    free(g_state);
    g_state = NULL;
    return 0;
}