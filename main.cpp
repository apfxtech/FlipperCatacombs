#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <stdlib.h>
#include <string.h>

#include "game/Game.h"
#include "game/Draw.h"
#include "game/FixedMath.h"
#include "game/Platform.h"
#include "game/Defines.h"

#define DISPLAY_WIDTH 128
#define DISPLAY_HEIGHT 64
#define BUFFER_SIZE (DISPLAY_WIDTH * DISPLAY_HEIGHT / 8)

typedef struct {
    FuriMutex* mutex;
    FuriTimer* timer;
    uint32_t last_tick;
    int16_t tick_accum;
    
    uint8_t screen_buffer[BUFFER_SIZE];
    uint8_t xbm_buffer[BUFFER_SIZE];
    uint8_t input_state;
    bool audio_enabled;
    bool exit_requested;
    bool redraw_needed;
} FlipperState;

static FlipperState* flipper_state = NULL;

/* ================= INPUT ================= */

uint8_t Platform::GetInput() {
    return flipper_state ? flipper_state->input_state : 0;
}

/* ================= AUDIO ================= */

void Platform::PlaySound(const uint16_t* pattern) {
    if(!flipper_state || !flipper_state->audio_enabled || !pattern) return;
    if(pattern[0] && pattern[1]) {
        furi_hal_speaker_start(pattern[0], 0.5f);
        furi_delay_ms(pattern[1]);
        furi_hal_speaker_stop();
    }
}

bool Platform::IsAudioEnabled() {
    return flipper_state && flipper_state->audio_enabled;
}

void Platform::SetAudioEnabled(bool e) {
    if(flipper_state) flipper_state->audio_enabled = e;
}

void Platform::ExpectLoadDelay() {
    if(flipper_state) {
        flipper_state->last_tick = furi_get_tick();
        flipper_state->tick_accum = 0;
    }
}

/* ================= LED ================= */

void Platform::SetLED(uint8_t r, uint8_t g, uint8_t b) {
    NotificationApp* n = (NotificationApp*)furi_record_open(RECORD_NOTIFICATION);
    if(r) notification_message(n, &sequence_set_red_255); else notification_message(n, &sequence_reset_red);
    if(g) notification_message(n, &sequence_set_green_255); else notification_message(n, &sequence_reset_green);
    if(b) notification_message(n, &sequence_set_blue_255); else notification_message(n, &sequence_reset_blue);
    furi_record_close(RECORD_NOTIFICATION);
}

/* ================= FRAMEBUFFER ================= */

static inline void arduboy_putpixel(int16_t x, int16_t y, bool color) {
    if(x < 0 || y < 0 || x >= DISPLAY_WIDTH || y >= DISPLAY_HEIGHT) return;
    uint16_t idx = x + (y >> 3) * DISPLAY_WIDTH;
    uint8_t mask = 1 << (y & 7);
    if(!color) 
        flipper_state->screen_buffer[idx] |= mask;
    else  
        flipper_state->screen_buffer[idx] &= ~mask;
}

void Platform::PutPixel(uint8_t x, uint8_t y, uint8_t color) {
    arduboy_putpixel(x, y, color);
}

void Platform::FillScreen(uint8_t color) {
    memset(flipper_state->screen_buffer, color ? 0xFF : 0x00, BUFFER_SIZE);
}

uint8_t* Platform::GetScreenBuffer() {
    return flipper_state->screen_buffer;
}

/* ================= DRAW ================= */

void Platform::DrawVLine(uint8_t x, int8_t y0, int8_t y1, uint8_t pattern) {
    if(y0 > y1) {
        int8_t temp = y0;
        y0 = y1;
        y1 = temp;
    }
    
    for(int y = y0; y <= y1; y++) {
        if(pattern & (1 << (y & 7))) {
            arduboy_putpixel(x, y, 1);
        }
    }
}

void Platform::DrawBitmap(int16_t x, int16_t y, const uint8_t* bmp) {
    if(!bmp) return;
    
    uint8_t w = bmp[0];
    uint8_t h = bmp[1];
    const uint8_t* data = bmp + 2;
    
    for(uint8_t j = 0; j < h; j++) {
        for(uint8_t i = 0; i < w; i++) {
            uint8_t byte = data[i + (j / 8) * w];
            if(byte & (1 << (j & 7))) {
                arduboy_putpixel(x + i, y + j, 1);
            }
        }
    }
}

void Platform::DrawSolidBitmap(int16_t x, int16_t y, const uint8_t* bmp) {
    if(!bmp) return;
    
    uint8_t w = bmp[0];
    uint8_t h = bmp[1];
    const uint8_t* data = bmp + 2;
    
    for(uint8_t j = 0; j < h; j++) {
        for(uint8_t i = 0; i < w; i++) {
            uint8_t byte = data[i + (j / 8) * w];
            if(byte & (1 << (j & 7))) {
                arduboy_putpixel(x + i, y + j, 1);
            } else {
                arduboy_putpixel(x + i, y + j, 0);
            }
        }
    }
}

void Platform::DrawSprite(int16_t x, int16_t y, const uint8_t* bmp, uint8_t) {
    DrawBitmap(x, y, bmp);
}

void Platform::DrawSprite(int16_t x, int16_t y, const uint8_t* bmp, const uint8_t*, uint8_t, uint8_t) {
    DrawBitmap(x, y, bmp);
}

/* ================= TIMER ================= */

static void timer_callback(void* ctx) {
    FlipperState* state = (FlipperState*)ctx;
    
    if(furi_mutex_acquire(state->mutex, 0) != FuriStatusOk) {
        return; // Пропускаем тик если мьютекс занят
    }
    
    uint32_t now = furi_get_tick();
    uint32_t delta_ticks = now - state->last_tick;
    state->last_tick = now;
    
    // Конвертируем тики в миллисекунды
    int16_t delta_ms = delta_ticks * 1000 / furi_kernel_get_tick_frequency();
    state->tick_accum += delta_ms;
    
    // Обрабатываем игровые тики с фиксированной частотой
    const int16_t frame_time = 1000 / TARGET_FRAMERATE;
    
    // Ограничиваем максимальное накопление (не более 3 кадров)
    if(state->tick_accum > frame_time * 3) {
        state->tick_accum = frame_time;
    }
    
    bool updated = false;
    while(state->tick_accum >= frame_time) {
        Game::Tick();
        state->tick_accum -= frame_time;
        updated = true;
    }
    
    if(updated) {
        state->redraw_needed = true;
    }
    
    furi_mutex_release(state->mutex);
}

/* ================= INPUT ================= */

static void input_callback(InputEvent* event, void* ctx) {
    FlipperState* state = (FlipperState*)ctx;
    
    furi_mutex_acquire(state->mutex, FuriWaitForever);
    
    if(event->type == InputTypePress || event->type == InputTypeRepeat) {
        switch(event->key) {
        case InputKeyUp:    state->input_state |= INPUT_UP; break;
        case InputKeyDown:  state->input_state |= INPUT_DOWN; break;
        case InputKeyLeft:  state->input_state |= INPUT_LEFT; break;
        case InputKeyRight: state->input_state |= INPUT_RIGHT; break;
        case InputKeyOk:    state->input_state |= INPUT_B; break;
        case InputKeyBack:  state->exit_requested = true; break;
        default: break;
        }
        state->redraw_needed = true; // Ввод требует перерисовки
    }
    
    if(event->type == InputTypeRelease) {
        switch(event->key) {
        case InputKeyUp:    state->input_state &= ~INPUT_UP; break;
        case InputKeyDown:  state->input_state &= ~INPUT_DOWN; break;
        case InputKeyLeft:  state->input_state &= ~INPUT_LEFT; break;
        case InputKeyRight: state->input_state &= ~INPUT_RIGHT; break;
        case InputKeyOk:    state->input_state &= ~INPUT_B; break;
        default: break;
        }
    }
    
    furi_mutex_release(state->mutex);
}

// писал chatGPT 
static void render_callback(Canvas* canvas, void* ctx) {
    FlipperState* state = (FlipperState*)ctx;
    
    if(furi_mutex_acquire(state->mutex, 0) != FuriStatusOk) {
        return; // Пропускаем рендер если мьютекс занят
    }
    
    if(state->redraw_needed) {
        // Даем игре отрисовать кадр
        Game::Draw();
        
        // Быстро обнуляем XBM-буфер
        memset(state->xbm_buffer, 0, BUFFER_SIZE);
        
        // Параметры для трансформации
        const int dst_stride = DISPLAY_WIDTH >> 3;           // количество байт в строке XBM (128/8 = 16)
        const int vert_blocks = DISPLAY_HEIGHT >> 3;         // количество блоков по 8 строк (64/8 = 8)
        
        // Итерация по столбцам и вертикальным байтам исходного буфера
        for(int x = 0; x < DISPLAY_WIDTH; ++x) {
            const uint8_t mask = (uint8_t)(1u << (x & 7));   // бит в целевом XBM-байте
            const int x_div8 = x >> 3;                       // смещение внутри XBM-строки
            
            // Для каждого вертикального байта (по 8 пикселей по Y)
            // src_idx = x + by * DISPLAY_WIDTH
            for(int by = 0; by < vert_blocks; ++by) {
                const uint16_t src_idx = (uint16_t)(x + by * DISPLAY_WIDTH);
                const uint8_t b = state->screen_buffer[src_idx];
                if(!b) continue; // пропускаем нулевой байт
                
                // Вычисляем базовый индекс в XBM для y = by*8
                // base = ((by*8)*DISPLAY_WIDTH + x) >> 3
                // Упрощается до:
                const uint16_t base = (uint16_t)(by * DISPLAY_WIDTH + x_div8);
                
                // Для каждого из 8 битов исходного байта устанавливаем соответствующий XBM-байт
                // dst indices: base + k * dst_stride  (k = 0..7)
                uint16_t dst = base;
                uint8_t bitmask = 1;
                for(int k = 0; k < 8; ++k) {
                    if(b & bitmask) {
                        state->xbm_buffer[dst] |= mask;
                    }
                    dst += dst_stride;
                    bitmask <<= 1;
                }
            }
        }
        
        state->redraw_needed = false;
    }
    
    // Рисуем XBM буфер на canvas
    canvas_draw_xbm(canvas, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, state->xbm_buffer);
    
    furi_mutex_release(state->mutex);
}
/* ================= ENTRY ================= */

extern "C" int32_t arduboy3d_app(void* p) {
    UNUSED(p);
    
    // Инициализация состояния
    flipper_state = (FlipperState*)malloc(sizeof(FlipperState));
    memset(flipper_state, 0, sizeof(FlipperState));
    
    flipper_state->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    if(!flipper_state->mutex) {
        free(flipper_state);
        return -1;
    }
    
    flipper_state->audio_enabled = false;
    flipper_state->last_tick = furi_get_tick();
    flipper_state->tick_accum = 0;
    flipper_state->redraw_needed = true; // Первая отрисовка
    
    // Инициализация игры
    Game::Init();
    
    // Настройка GUI
    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, render_callback, flipper_state);
    view_port_input_callback_set(view_port, input_callback, flipper_state);
    
    Gui* gui = (Gui*)furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);
    
    flipper_state->timer = furi_timer_alloc(timer_callback, FuriTimerTypePeriodic, flipper_state);
    furi_timer_start(flipper_state->timer, furi_kernel_get_tick_frequency() / TARGET_FRAMERATE);
    
    // Главный цикл
    while(!flipper_state->exit_requested) {
        view_port_update(view_port);
        furi_delay_ms(100);
    }
    
    // Очистка
    furi_timer_stop(flipper_state->timer);
    furi_timer_free(flipper_state->timer);
    
    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_record_close(RECORD_GUI);
    
    furi_mutex_free(flipper_state->mutex);
    free(flipper_state);
    flipper_state = NULL;
    
    return 0;
}