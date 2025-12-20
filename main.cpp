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

#define DISPLAY_WIDTH 128
#define DISPLAY_HEIGHT 64
#define BUFFER_SIZE (DISPLAY_WIDTH * DISPLAY_HEIGHT / 8)
#define FRAME_TIME_MS (1000 / TARGET_FRAMERATE)
#define MAX_FRAME_SKIP 3

typedef struct {
    FuriMutex* mutex;
    FuriTimer* timer;
    uint32_t last_tick;
    int16_t tick_accum;
    
    uint8_t screen_buffer[BUFFER_SIZE];
    uint8_t xbm_buffer[BUFFER_SIZE];
    uint8_t input_state;
    bool inverted;
    bool audio_enabled;
    bool exit_requested;
    bool game_updated;
} FlipperState;

static FlipperState* g_state = NULL;

uint8_t Platform::GetInput() {
    return g_state ? g_state->input_state : 0;
}

void Platform::PlaySound(const uint16_t* pattern) {
    (void)pattern;
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
    if(x < 0 || y < 0 || x >= DISPLAY_WIDTH || y >= DISPLAY_HEIGHT) return;
    uint16_t idx = x + (y >> 3) * DISPLAY_WIDTH;
    uint8_t mask = 1 << (y & 7);
    if(!color) 
        g_state->screen_buffer[idx] |= mask;
    else  
        g_state->screen_buffer[idx] &= ~mask;
}

void Platform::PutPixel(uint8_t x, uint8_t y, uint8_t color) {
    set_pixel(x, y, color);
}

void Platform::FillScreen(uint8_t color) {
    memset(g_state->screen_buffer, color ? 0xFF : 0x00, BUFFER_SIZE);
}

uint8_t* Platform::GetScreenBuffer() {
    return g_state->screen_buffer;
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
    
    // Количество страниц по 8 пикселей в высоту
    uint8_t pages = (h + 7) >> 3;
    
    for(uint8_t page = 0; page < pages; page++) {
        for(uint8_t i = 0; i < w; i++) {
            uint8_t byte = data[i + page * w];  // Правильная индексация!
            
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
    uint16_t frame_size = w * pages; // размер только bitmap-части (без маски)

    // PLUS_MASK: (spriteByte, maskByte) * frame_size
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

                // В Arduboy Sprites маска "бит-флипнута" по смыслу,
                // но твоя логика уже ожидает m&bit => "рисуем тут".
                if(m & bit) {
                    set_pixel(x + i, y + py, (s & bit) ? COLOUR_WHITE : COLOUR_BLACK);
                }
            }
        }
    }
}

void Platform::DrawSprite(int16_t x, int16_t y,
                          const uint8_t* bmp, const uint8_t* mask,
                          uint8_t frame, uint8_t mask_frame) {
    if(!bmp) return;

    uint8_t w = bmp[0];
    uint8_t h = bmp[1];
    uint8_t pages = (h + 7) >> 3;
    uint16_t frame_size = w * pages;

    const uint8_t* sprite_data = bmp + 2 + (uint32_t)frame * frame_size;

    if(mask) {
        // ВАЖНО: external mask в Arduboy Sprites обычно БЕЗ заголовка w/h
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
        // если вдруг реально зовётся с mask==NULL, обрабатываем как PLUS_MASK
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

static void convert_buffer_to_xbm(FlipperState* state) {
    memset(state->xbm_buffer, 0x00, BUFFER_SIZE);
    
    for(int page = 0; page < DISPLAY_HEIGHT / 8; page++) {
        int y_base = page << 3;
        int page_offset = page * DISPLAY_WIDTH;
        
        for(int x = 0; x < DISPLAY_WIDTH; x++) {
            uint8_t byte = state->screen_buffer[x + page_offset];
            
            for(int b = 0; b < 8; b++) {
                bool bit = (byte >> b) & 0x01;
                bool draw = state->inverted ? !bit : bit;
                
                if(draw) {
                    int y = y_base + b;
                    int xbm_idx = (y * DISPLAY_WIDTH + x) >> 3;
                    state->xbm_buffer[xbm_idx] |= 1 << (x & 7);
                }
            }
        }
    }
}

static void timer_callback(void* ctx) {
    FlipperState* state = (FlipperState*)ctx;
    
    if(furi_mutex_acquire(state->mutex, 0) != FuriStatusOk) return;
    
    uint32_t now = furi_get_tick();
    uint32_t delta_ticks = now - state->last_tick;
    state->last_tick = now;
    
    int16_t delta_ms = delta_ticks * 1000 / furi_kernel_get_tick_frequency();
    state->tick_accum += delta_ms;
    
    if(state->tick_accum > FRAME_TIME_MS * MAX_FRAME_SKIP) {
        state->tick_accum = FRAME_TIME_MS;
    }
    
    while(state->tick_accum >= FRAME_TIME_MS) {
        Game::Tick();
        state->tick_accum -= FRAME_TIME_MS;
        state->game_updated = true;
    }
    
    furi_mutex_release(state->mutex);
}

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
    } else if(event->type == InputTypeRelease) {
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

static void render_callback(Canvas* canvas, void* ctx) {
    FlipperState* state = (FlipperState*)ctx;

    if(furi_mutex_acquire(state->mutex, 0) != FuriStatusOk) return;

    if(state->game_updated) {
        Game::Draw();
        convert_buffer_to_xbm(state);
        state->game_updated = false;
    }

    canvas_draw_xbm(canvas, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, state->xbm_buffer);
    furi_mutex_release(state->mutex);
}

extern "C" int32_t arduboy3d_app(void* p) {
    UNUSED(p);
    
    g_state = (FlipperState*)malloc(sizeof(FlipperState));
    memset(g_state, 0, sizeof(FlipperState));
    
    g_state->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    if(!g_state->mutex) {
        free(g_state);
        return -1;
    }
    
    g_state->audio_enabled = false;
    g_state->last_tick = furi_get_tick();
    g_state->tick_accum = 0;
    g_state->game_updated = true;
    g_state->inverted = true;
    
    Game::Init();
    
    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, render_callback, g_state);
    view_port_input_callback_set(view_port, input_callback, g_state);
    
    Gui* gui = (Gui*)furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);
    
    g_state->timer = furi_timer_alloc(timer_callback, FuriTimerTypePeriodic, g_state);
    furi_timer_start(g_state->timer, furi_kernel_get_tick_frequency() / TARGET_FRAMERATE);
    
    while(!g_state->exit_requested) {
        view_port_update(view_port);
        furi_delay_ms(100);
    }
    
    furi_timer_stop(g_state->timer);
    furi_timer_free(g_state->timer);
    
    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_record_close(RECORD_GUI);
    
    furi_mutex_free(g_state->mutex);
    free(g_state);
    g_state = NULL;
    
    return 0;
}