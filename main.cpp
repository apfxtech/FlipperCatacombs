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

#include "game/Game.h"
#include "game/Draw.h"
#include "game/FixedMath.h"
#include "game/Platform.h"
#include "game/Defines.h"
#include "game/Sounds.h"

#define DISPLAY_WIDTH 128
#define DISPLAY_HEIGHT 64
#define BUFFER_SIZE (DISPLAY_WIDTH * DISPLAY_HEIGHT / 8)
#define FRAME_TIME_MS (1000 / TARGET_FRAMERATE)
#define MAX_FRAME_SKIP 3

typedef struct {
    // Мьютекс для GAME/INPUT состояния (Tick/Draw и input_state/exit)
    FuriMutex* game_mutex;

    // Мьютекс только для быстрого swap указателя кадра
    FuriMutex* swap_mutex;

    FuriTimer* timer;
    uint32_t last_tick;
    int16_t tick_accum;

    // Буфер игры в "page" формате (как у тебя)
    uint8_t screen_buffer[BUFFER_SIZE];

    // Двойная буферизация XBM (то, что реально рисуется canvas_draw_xbm)
    uint8_t xbm_a[BUFFER_SIZE];
    uint8_t xbm_b[BUFFER_SIZE];

    // Указатель на текущий front XBM (GUI читает только его)
    volatile uint8_t* xbm_front;

    uint8_t input_state;
    bool inverted;
    bool audio_enabled;
    volatile bool exit_requested;

    // Флаг: есть смысл перерисовать/обновить кадр
    volatile bool game_updated;

    // Защита от реэнтерабельности timer callback (на всякий случай)
    volatile bool in_timer;
} FlipperState;

static FlipperState* g_state = NULL;

// ---------------- Platform ----------------

uint8_t Platform::GetInput() {
    return g_state ? g_state->input_state : 0;
}
// --- Add near top of main.cpp (after includes) ---

typedef struct {
    const uint16_t* pattern;
} SoundRequest;

static FuriMessageQueue* g_sound_queue = NULL;
static FuriThread* g_sound_thread = NULL;

// Можно подстроить громкость
static const float kSoundVolume = 1.0f;

static const uint32_t kToneTickHz = 800;

static inline uint32_t arduboy_ticks_to_ms(uint16_t ticks) {
    return (uint32_t)((ticks * 1000u + (kToneTickHz/2)) / kToneTickHz);
}

static int32_t sound_thread_fn(void* /*ctx*/) {
    SoundRequest req;

    while(true) {
        // Если игра завершается — выходим
        if(g_state && g_state->exit_requested) break;

        // Ждём запрос на звук
        if(furi_message_queue_get(g_sound_queue, &req, 50) != FuriStatusOk) {
            continue;
        }

        // Если звук выключен — просто пропускаем
        if(!g_state || !g_state->audio_enabled || !req.pattern) {
            continue;
        }

        // Пытаемся захватить динамик
        if(!furi_hal_speaker_acquire(50)) {
            continue;
        }

        const uint16_t* p = req.pattern;

        while(true) {
            // Возможность прервать текущий звук новым
            SoundRequest new_req;
            if(furi_message_queue_get(g_sound_queue, &new_req, 0) == FuriStatusOk) {
                // переключаемся на новый паттерн
                p = new_req.pattern ? new_req.pattern : p;
            }

            uint16_t freq = *p++;

            // Конец паттерна
            if(freq == TONES_END) {
                break;
            }

            uint16_t dur_ticks = *p++;
            uint32_t dur_ms = arduboy_ticks_to_ms(dur_ticks);

            // Защита от нулевой длительности
            if(dur_ms == 0) dur_ms = 1;

            // Если во время проигрывания звук выключили — выходим
            if(!g_state || !g_state->audio_enabled) {
                break;
            }

            if(freq == 0) {
                // Пауза
                furi_hal_speaker_stop();
                furi_delay_ms(dur_ms);
            } else {
                // Нота
                furi_hal_speaker_start((float)freq, kSoundVolume);
                furi_delay_ms(dur_ms);
                furi_hal_speaker_stop();
            }
        }

        furi_hal_speaker_stop();
        furi_hal_speaker_release();
    }

    // На всякий случай
    if(furi_hal_speaker_is_mine()) {
        furi_hal_speaker_stop();
        furi_hal_speaker_release();
    }

    return 0;
}

static void sound_system_init_once() {
    if(g_sound_queue) return;

    // Очередь маленькая: нам важнее "последний звук", чем накопление
    g_sound_queue = furi_message_queue_alloc(4, sizeof(SoundRequest));

    g_sound_thread = furi_thread_alloc();
    furi_thread_set_name(g_sound_thread, "GameSound");
    furi_thread_set_stack_size(g_sound_thread, 1024);
    furi_thread_set_priority(g_sound_thread, FuriThreadPriorityNormal);
    furi_thread_set_callback(g_sound_thread, sound_thread_fn);
    furi_thread_start(g_sound_thread);
}


// --- Replace your stub Platform::PlaySound in main.cpp with this: ---

void Platform::PlaySound(const uint16_t* audioPattern) {
    if(!g_state) return;
    if(!g_state->audio_enabled) return;
    if(!audioPattern) return;

    sound_system_init_once();

    SoundRequest req = {.pattern = audioPattern};

    // Важно: не блокируем игру. Если очередь полная — выкидываем самый старый.
    if(furi_message_queue_put(g_sound_queue, &req, 0) != FuriStatusOk) {
        SoundRequest dummy;
        // освободим 1 слот
        (void)furi_message_queue_get(g_sound_queue, &dummy, 0);
        (void)furi_message_queue_put(g_sound_queue, &req, 0);
    }
}

bool Platform::IsAudioEnabled() {
    return g_state && g_state->audio_enabled;
}

void Platform::SetAudioEnabled(bool e) {
    if(g_state) g_state->audio_enabled = e;
}

void Platform::ExpectLoadDelay() {
    if(g_state) {
        g_state->last_tick = furi_get_tick();
        g_state->tick_accum = 0;
    }
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
    if(!g_state) return;
    if(x < 0 || y < 0 || x >= DISPLAY_WIDTH || y >= DISPLAY_HEIGHT) return;

    uint16_t idx = x + (y >> 3) * DISPLAY_WIDTH;
    uint8_t mask = 1 << (y & 7);

    // У тебя логика: "white" очищает бит, "black" ставит бит
    if(!color)
        g_state->screen_buffer[idx] |= mask;
    else
        g_state->screen_buffer[idx] &= ~mask;
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

void Platform::DrawSprite(
    int16_t x,
    int16_t y,
    const uint8_t* bmp,
    const uint8_t* mask,
    uint8_t frame,
    uint8_t mask_frame) {
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

// ---------------- Fast convert (page buffer -> XBM) ----------------
//
// dst: XBM buffer (строки, ширина 128 => 16 байт/строку)
// screen: page-формат (как у тебя): idx = x + page*128, бит = y%8
//
static void convert_screen_to_xbm_into(const uint8_t* screen, uint8_t* dst, bool inverted) {
    const uint8_t inv = inverted ? 0xFF : 0x00;
    constexpr int XBM_STRIDE = DISPLAY_WIDTH / 8;

    // Полностью перезаписываем dst, memset не нужен
    for(int page = 0; page < DISPLAY_HEIGHT / 8; page++) {
        const int page_offset = page * DISPLAY_WIDTH;
        const int y_base = page * 8;

        for(int x = 0; x < DISPLAY_WIDTH; x += 8) {
            uint8_t c0 = screen[page_offset + x + 0] ^ inv;
            uint8_t c1 = screen[page_offset + x + 1] ^ inv;
            uint8_t c2 = screen[page_offset + x + 2] ^ inv;
            uint8_t c3 = screen[page_offset + x + 3] ^ inv;
            uint8_t c4 = screen[page_offset + x + 4] ^ inv;
            uint8_t c5 = screen[page_offset + x + 5] ^ inv;
            uint8_t c6 = screen[page_offset + x + 6] ^ inv;
            uint8_t c7 = screen[page_offset + x + 7] ^ inv;

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

// ---------------- Callbacks ----------------

static void timer_callback(void* ctx) {
    FlipperState* state = (FlipperState*)ctx;
    if(!state) return;

    // Защита от реэнтерабельности (на всякий случай)
    if(state->in_timer) return;
    state->in_timer = true;

    // Обновление времени — без мьютекса (это только локальные переменные таймера)
    uint32_t now = furi_get_tick();
    uint32_t delta_ticks = now - state->last_tick;
    state->last_tick = now;

    int16_t delta_ms = (int16_t)(delta_ticks * 1000 / furi_kernel_get_tick_frequency());
    state->tick_accum += delta_ms;

    if(state->tick_accum > FRAME_TIME_MS * MAX_FRAME_SKIP) {
        state->tick_accum = FRAME_TIME_MS;
    }

    // 1) Быстро/неблокирующе берём game_mutex
    if(furi_mutex_acquire(state->game_mutex, 0) != FuriStatusOk) {
        state->in_timer = false;
        return;
    }

    // 2) Tick (может сделать несколько тиков)
    bool did_tick = false;
    while(state->tick_accum >= FRAME_TIME_MS) {
        Game::Tick();
        state->tick_accum -= FRAME_TIME_MS;
        did_tick = true;
    }

    // 3) Если было изменение (тик/принудительный флаг), делаем Draw в screen_buffer
    bool need_draw = did_tick || state->game_updated;
    if(need_draw) {
        Game::Draw();
        state->game_updated = false;
    }

    // Важно: дальше игра нам не нужна — отпускаем game_mutex ДО конвертации
    furi_mutex_release(state->game_mutex);

    // 4) Конвертируем в BACK XBM (без мьютекса)
    if(need_draw) {
        uint8_t* front = (uint8_t*)state->xbm_front;
        uint8_t* back = (front == state->xbm_a) ? state->xbm_b : state->xbm_a;

        convert_screen_to_xbm_into(state->screen_buffer, back, state->inverted);

        // 5) Короткий swap указателя (без долгих блокировок)
        if(furi_mutex_acquire(state->swap_mutex, 0) == FuriStatusOk) {
            state->xbm_front = back;
            furi_mutex_release(state->swap_mutex);
        } else {
            // если не смогли — просто пропускаем swap (лучше, чем блокировать)
        }
    }

    state->in_timer = false;
}

static void input_callback(InputEvent* event, void* ctx) {
    FlipperState* state = (FlipperState*)ctx;
    if(!state || !event) return;

    // Здесь можно оставить FuriWaitForever — событие ввода редкое
    furi_mutex_acquire(state->game_mutex, FuriWaitForever);

    if(event->type == InputTypePress || event->type == InputTypeRepeat) {
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
            state->exit_requested = true;
            break;
        default:
            break;
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
        default:
            break;
        }
    }

    furi_mutex_release(state->game_mutex);
}

static void render_callback(Canvas* canvas, void* ctx) {
    FlipperState* state = (FlipperState*)ctx;
    if(!state || !canvas) return;

    // Берём только указатель на готовый кадр — максимально коротко
    const uint8_t* frame;

    if(furi_mutex_acquire(state->swap_mutex, 0) == FuriStatusOk) {
        frame = (const uint8_t*)state->xbm_front;
        furi_mutex_release(state->swap_mutex);
    } else {
        // если не смогли — рисуем текущий указатель (скорее всего валиден)
        frame = (const uint8_t*)state->xbm_front;
    }

    canvas_draw_xbm(canvas, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, frame);
}

// ---------------- App entry ----------------

extern "C" int32_t arduboy3d_app(void* p) {
    UNUSED(p);

    g_state = (FlipperState*)malloc(sizeof(FlipperState));
    if(!g_state) return -1;
    memset(g_state, 0, sizeof(FlipperState));

    g_state->game_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    g_state->swap_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    if(!g_state->game_mutex || !g_state->swap_mutex) {
        if(g_state->game_mutex) furi_mutex_free(g_state->game_mutex);
        if(g_state->swap_mutex) furi_mutex_free(g_state->swap_mutex);
        free(g_state);
        g_state = NULL;
        return -1;
    }

    g_state->audio_enabled = false;
    g_state->last_tick = furi_get_tick();
    g_state->tick_accum = 0;
    g_state->game_updated = true;
    g_state->inverted = true;
    g_state->in_timer = false;

    // Инициализация front/back
    memset(g_state->xbm_a, 0x00, BUFFER_SIZE);
    memset(g_state->xbm_b, 0x00, BUFFER_SIZE);
    g_state->xbm_front = g_state->xbm_a;

    // Важно: init игры до запуска таймера
    Game::Init();

    // Первый кадр (чтобы не было пустоты до первого тика)
    // Делается под game_mutex, но один раз при старте.
    furi_mutex_acquire(g_state->game_mutex, FuriWaitForever);
    Game::Draw();
    furi_mutex_release(g_state->game_mutex);
    convert_screen_to_xbm_into(g_state->screen_buffer, (uint8_t*)g_state->xbm_front, g_state->inverted);

    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, render_callback, g_state);
    view_port_input_callback_set(view_port, input_callback, g_state);

    Gui* gui = (Gui*)furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    g_state->timer = furi_timer_alloc(timer_callback, FuriTimerTypePeriodic, g_state);
    furi_timer_start(g_state->timer, furi_kernel_get_tick_frequency() / TARGET_FRAMERATE);

    while(!g_state->exit_requested) {
        view_port_update(view_port);
        furi_delay_ms(100); // не трогаем
    }

    furi_timer_stop(g_state->timer);
    furi_timer_free(g_state->timer);

    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_record_close(RECORD_GUI);

    furi_mutex_free(g_state->swap_mutex);
    furi_mutex_free(g_state->game_mutex);
    free(g_state);
    g_state = NULL;

    return 0;
}
