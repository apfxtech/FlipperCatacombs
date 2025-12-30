#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <stdlib.h>
#include <string.h>

#define COLOUR_WHITE 0
#define COLOUR_BLACK 1

#include "game/Game.h"
#include "game/Draw.h"
#include "game/FixedMath.h"
#include "game/Platform.h"
#include "game/Defines.h"
#include "game/Sounds.h"
#include "lib/EEPROM.h"

#define DISPLAY_WIDTH    128
#define DISPLAY_HEIGHT   64
#define BUFFER_SIZE      (DISPLAY_WIDTH * DISPLAY_HEIGHT / 8)
#define TARGET_FRAMERATE 30

typedef struct {
    uint8_t back_buffer[BUFFER_SIZE];
    uint8_t front_buffer[BUFFER_SIZE];

    Gui* gui;
    Canvas* canvas;
    FuriTimer* timer;

    FuriMutex* fb_mutex;

    volatile uint8_t input_state;
    volatile bool exit_requested;
    volatile bool audio_enabled;

    volatile bool invert_frame;
    volatile bool in_frame;

    // back-hold логика
    bool back_hold_active;
    uint16_t back_hold_start;
    bool back_hold_handled;

    // input pubsub
    FuriPubSub* input_events;
    FuriPubSubSubscription* input_sub;
} FlipperState;

static FlipperState* g_state = NULL;

// ---------------- SOUND ----------------

typedef struct {
    const uint16_t* pattern;
} SoundRequest;

static FuriMessageQueue* g_sound_queue = NULL;
static FuriThread* g_sound_thread = NULL;
static volatile bool g_sound_thread_running = false;
static const float kSoundVolume = 1.0f;
static const uint32_t kToneTickHz = 780;

static inline uint32_t arduboy_ticks_to_ms(uint16_t ticks) {
    return (uint32_t)((ticks * 1000u + (kToneTickHz / 2)) / kToneTickHz);
}

static int32_t sound_thread_fn(void* ctx) {
    UNUSED(ctx);
    SoundRequest req;

    while(g_sound_thread_running) {
        if(furi_message_queue_get(g_sound_queue, &req, 50) != FuriStatusOk) continue;
        if(!g_state || !g_state->audio_enabled || !req.pattern) continue;
        if(!furi_hal_speaker_acquire(50)) continue;

        const uint16_t* p = req.pattern;
        while(g_sound_thread_running && g_state && g_state->audio_enabled) {
            SoundRequest new_req;
            if(furi_message_queue_get(g_sound_queue, &new_req, 0) == FuriStatusOk) {
                if(new_req.pattern) p = new_req.pattern;
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
    if(enabled && !was_enabled)
        sound_system_init();
    else if(!enabled && was_enabled)
        sound_system_deinit();
}

// ---------------- INPUT / LED ----------------

uint8_t Platform::GetInput() {
    return g_state ? g_state->input_state : 0;
}

void Platform::ExpectLoadDelay() {
}

void Platform::SetLED(uint8_t r, uint8_t g, uint8_t b) {
    NotificationApp* n = (NotificationApp*)furi_record_open(RECORD_NOTIFICATION);
    if(r)
        notification_message(n, &sequence_set_red_255);
    else
        notification_message(n, &sequence_reset_red);

    if(g)
        notification_message(n, &sequence_set_green_255);
    else
        notification_message(n, &sequence_reset_green);

    if(b)
        notification_message(n, &sequence_set_blue_255);
    else
        notification_message(n, &sequence_reset_blue);

    furi_record_close(RECORD_NOTIFICATION);
}

// ---------------- DRAW (в back_buffer) ----------------

static inline void set_pixel(int16_t x, int16_t y, bool color) {
    if(!g_state) return;
    if(x < 0 || y < 0 || x >= DISPLAY_WIDTH || y >= DISPLAY_HEIGHT) return;

    uint8_t* buf = g_state->back_buffer;
    uint16_t idx = (uint16_t)(x + (y >> 3) * DISPLAY_WIDTH);
    uint8_t mask = (uint8_t)(1u << (y & 7));

    if(color)
        buf[idx] |= mask;
    else
        buf[idx] &= (uint8_t)~mask;
}

void Platform::PutPixel(uint8_t x, uint8_t y, uint8_t color) {
    set_pixel(x, y, color);
}

void Platform::FillScreen(uint8_t color) {
    if(!g_state) return;
    memset(g_state->back_buffer, color ? 0xFF : 0x00, BUFFER_SIZE);
}

uint8_t* Platform::GetScreenBuffer() {
    return g_state ? g_state->back_buffer : NULL;
}

void Platform::DrawVLine(uint8_t x, int8_t y0, int8_t y1, uint8_t pattern) {
    if(y0 > y1) {
        int8_t t = y0;
        y0 = y1;
        y1 = t;
    }
    for(int y = y0; y <= y1; y++) {
        if(pattern & (1 << (y & 7))) set_pixel(x, y, 1);
    }
}

void Platform::DrawBitmap(int16_t x, int16_t y, const uint8_t* bmp) {
    if(!bmp) return;
    uint8_t w = bmp[0], h = bmp[1];
    const uint8_t* data = bmp + 2;
    uint8_t pages = (h + 7) >> 3;
    for(uint8_t page = 0; page < pages; page++) {
        for(uint8_t i = 0; i < w; i++) {
            uint8_t byte = data[i + page * w];
            for(uint8_t b = 0; b < 8; b++) {
                uint8_t py = (uint8_t)(page * 8 + b);
                if(py >= h) break;
                if(byte & (1 << b)) set_pixel(x + i, y + py, COLOUR_WHITE);
            }
        }
    }
}

void Platform::DrawSolidBitmap(int16_t x, int16_t y, const uint8_t* bmp) {
    if(!bmp) return;
    uint8_t w = bmp[0], h = bmp[1];
    const uint8_t* data = bmp + 2;
    uint8_t pages = (h + 7) >> 3;
    for(uint8_t page = 0; page < pages; page++) {
        for(uint8_t i = 0; i < w; i++) {
            uint8_t byte = data[i + page * w];
            for(uint8_t b = 0; b < 8; b++) {
                uint8_t py = (uint8_t)(page * 8 + b);
                if(py >= h) break;
                bool pixel = (byte & (1 << b)) != 0;
                set_pixel(x + i, y + py, pixel ? COLOUR_WHITE : COLOUR_BLACK);
            }
        }
    }
}

void Platform::DrawSprite(int16_t x, int16_t y, const uint8_t* bmp, uint8_t frame) {
    if(!bmp) return;
    uint8_t w = bmp[0], h = bmp[1];
    uint8_t pages = (h + 7) >> 3;
    uint16_t frame_size = (uint16_t)(w * pages);
    const uint8_t* data = bmp + 2 + (uint32_t)frame * frame_size * 2;
    for(uint8_t page = 0; page < pages; page++) {
        for(uint8_t i = 0; i < w; i++) {
            uint16_t idx = (uint16_t)((i + page * w) * 2);
            uint8_t s = data[idx + 0];
            uint8_t m = data[idx + 1];
            for(uint8_t b = 0; b < 8; b++) {
                uint8_t py = (uint8_t)(page * 8 + b);
                if(py >= h) break;
                uint8_t bit = (uint8_t)(1u << b);
                if(m & bit) set_pixel(x + i, y + py, (s & bit) ? COLOUR_BLACK : COLOUR_WHITE);
            }
        }
    }
}

// ---------------- DISPLAY OUTPUT ----------------
static void framebuffer_commit_callback(
    uint8_t* data,
    size_t size,
    CanvasOrientation orientation,
    void* context) {
    FlipperState* state = (FlipperState*)context;
    if(!state || !data) return;
    if(size < BUFFER_SIZE) return;
    (void)orientation;
    if(furi_mutex_acquire(state->fb_mutex, 0) != FuriStatusOk) return;
    const uint8_t* src = state->front_buffer;
    bool inv = state->invert_frame;
    if(!inv) {
        memcpy(data, src, BUFFER_SIZE);
    } else {
        for(size_t i = 0; i < BUFFER_SIZE; i++) {
            data[i] = (uint8_t)(src[i] ^ 0xFF);
        }
    }

    furi_mutex_release(state->fb_mutex);
}

// ---------------- INPUT PUBSUB ----------------

static void input_events_callback(const void* value, void* ctx) {
    FlipperState* state = (FlipperState*)ctx;
    if(!state || !value) return;

    const InputEvent* event = (const InputEvent*)value;
    const uint32_t HOLD_TIME_MS = 300;

    if(event->type == InputTypePress) {
        switch(event->key) {
        case InputKeyUp:
            state->input_state |= INPUT_UP;
            break;
        case InputKeyDown:
            state->input_state |= INPUT_DOWN;
            break;
        case InputKeyLeft:
            state->input_state |= INPUT_LEFT;
            break;
        case InputKeyRight:
            state->input_state |= INPUT_RIGHT;
            break;
        case InputKeyOk:
            state->input_state |= INPUT_B;
            break;
        case InputKeyBack:
            state->back_hold_active = true;
            state->back_hold_handled = false;
            state->back_hold_start = furi_get_tick();
            break;
        default:
            break;
        }
    } else if(event->type == InputTypeRepeat) {
        if(event->key == InputKeyBack && state->back_hold_active && !state->back_hold_handled) {
            uint32_t held_ms = furi_get_tick() - state->back_hold_start;
            if(held_ms >= HOLD_TIME_MS) {
                state->back_hold_handled = true;
                if(Game::IsInMenu())
                    state->exit_requested = true;
                else
                    Game::GoToMenu();
            }
        }
    } else if(event->type == InputTypeRelease) {
        switch(event->key) {
        case InputKeyUp:
            state->input_state &= ~INPUT_UP;
            break;
        case InputKeyDown:
            state->input_state &= ~INPUT_DOWN;
            break;
        case InputKeyLeft:
            state->input_state &= ~INPUT_LEFT;
            break;
        case InputKeyRight:
            state->input_state &= ~INPUT_RIGHT;
            break;
        case InputKeyOk:
            state->input_state &= ~INPUT_B;
            break;
        case InputKeyBack:
            state->back_hold_active = false;
            state->back_hold_handled = false;
            break;
        default:
            break;
        }
    }
}

static void timer_callback(void* ctx) {
    FlipperState* state = (FlipperState*)ctx;
    if(!state) return;

    if(state->in_frame) return;
    state->in_frame = true;

    Game::Tick();
    Game::Draw();

    bool inv = !Game::IsInMenu();

    // back -> front
    furi_mutex_acquire(state->fb_mutex, FuriWaitForever);
    memcpy(state->front_buffer, state->back_buffer, BUFFER_SIZE);
    state->invert_frame = inv;
    furi_mutex_release(state->fb_mutex);

    canvas_commit(state->canvas);

    state->in_frame = false;
}

extern "C" int32_t arduboy3d_app(void* p) {
    UNUSED(p);

    g_state = (FlipperState*)malloc(sizeof(FlipperState));
    if(!g_state) return -1;
    memset(g_state, 0, sizeof(FlipperState));

    g_state->fb_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    if(!g_state->fb_mutex) {
        free(g_state);
        g_state = NULL;
        return -1;
    }

    memset(g_state->back_buffer, 0x00, BUFFER_SIZE);
    memset(g_state->front_buffer, 0x00, BUFFER_SIZE);
    EEPROM.begin();
    furi_delay_ms(50);
    if(EEPROM.read(2)){
        Platform::SetAudioEnabled(true);
    } else {
        Platform::SetAudioEnabled(false);
    }
    Game::menu.ReadScore();

    g_state->gui = (Gui*)furi_record_open(RECORD_GUI);
    gui_add_framebuffer_callback(g_state->gui, framebuffer_commit_callback, g_state);
    g_state->canvas = gui_direct_draw_acquire(g_state->gui);
    g_state->input_events = (FuriPubSub*)furi_record_open(RECORD_INPUT_EVENTS);
    g_state->input_sub =
        furi_pubsub_subscribe(g_state->input_events, input_events_callback, g_state);
    g_state->timer = furi_timer_alloc(timer_callback, FuriTimerTypePeriodic, g_state);
    uint32_t tick_hz = furi_kernel_get_tick_frequency();
    uint32_t period = (tick_hz + (TARGET_FRAMERATE / 2)) / TARGET_FRAMERATE;
    if(period == 0) period = 1;
    furi_timer_start(g_state->timer, period);

    while(!g_state->exit_requested) {
        furi_delay_ms(50);
    }

    // Cleanup
    Platform::SetAudioEnabled(false);
    if(g_state->timer) {
        furi_timer_stop(g_state->timer);
        furi_timer_free(g_state->timer);
        g_state->timer = NULL;
    }

    if(g_state->input_sub) {
        furi_pubsub_unsubscribe(g_state->input_events, g_state->input_sub);
        g_state->input_sub = NULL;
    }

    if(g_state->input_events) {
        furi_record_close(RECORD_INPUT_EVENTS);
        g_state->input_events = NULL;
    }
    
    if(g_state->gui) {
        gui_direct_draw_release(g_state->gui);
        gui_remove_framebuffer_callback(g_state->gui, framebuffer_commit_callback, g_state);
        furi_record_close(RECORD_GUI);
        g_state->gui = NULL;
    }

    if(g_state->fb_mutex) {
        furi_mutex_free(g_state->fb_mutex);
        g_state->fb_mutex = NULL;
    }

    free(g_state);
    g_state = NULL;
    return 0;
}
