#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <stdlib.h>
#include <string.h>

#include "lib/flipper.h"
#include "game/Game.h"
#include "game/Platform.h"
#include "lib/EEPROM.h"

#define TARGET_FRAMERATE 30
#define HOLD_TIME_MS 300

FlipperState* g_state = NULL;

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
    for(size_t i = 0; i < BUFFER_SIZE; i++) {
        data[i] = src[i] ^ 0xFF;
    }

    furi_mutex_release(state->fb_mutex);
}

#define SET_FLAG_ON_PRESS_RELEASE(key, flag)              \
    case key:                                             \
        if(event->type == InputTypePress)                 \
            state->input_state |= (flag);                 \
        else if(event->type == InputTypeRelease)          \
            state->input_state &= ~(flag);                \
        break;

#define BACK_PRESS()                                      \
    do {                                                  \
        state->back_hold_active = true;                   \
        state->back_hold_handled = false;                 \
        state->back_hold_start = furi_get_tick();         \
    } while(0)

#define BACK_RELEASE()                                    \
    do {                                                  \
        state->back_hold_active = false;                  \
        state->back_hold_handled = false;                 \
    } while(0)

static void input_events_callback(const void* value, void* ctx) {
    FlipperState* state = (FlipperState*)ctx;
    if(!state || !value) return;

    const InputEvent* event = (const InputEvent*)value;

    if(event->type == InputTypeRepeat &&
       event->key == InputKeyBack &&
       state->back_hold_active &&
       !state->back_hold_handled) {

        if((furi_get_tick() - state->back_hold_start) >= HOLD_TIME_MS) {
            state->back_hold_handled = true;
            if(Game::InMenu())
                state->exit_requested = true;
            else
                Game::GoToMenu();
        }
        return;
    }

    switch(event->key) {
        SET_FLAG_ON_PRESS_RELEASE(InputKeyUp,    INPUT_UP)
        SET_FLAG_ON_PRESS_RELEASE(InputKeyDown,  INPUT_DOWN)
        SET_FLAG_ON_PRESS_RELEASE(InputKeyLeft,  INPUT_LEFT)
        SET_FLAG_ON_PRESS_RELEASE(InputKeyRight, INPUT_RIGHT)
        SET_FLAG_ON_PRESS_RELEASE(InputKeyOk,    INPUT_B)

        case InputKeyBack:
            if(event->type == InputTypePress) {
                BACK_PRESS();
            } else if(event->type == InputTypeRelease) {
                BACK_RELEASE();
            }
            break;

        default:
            break;
    }
}

extern "C" int32_t arduboy3d_app(void* p) {
    UNUSED(p);

    g_state = (FlipperState*)malloc(sizeof(FlipperState));
    if(!g_state) return -1;
    memset(g_state, 0, sizeof(FlipperState));

    g_state->fb_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    if(!g_state->fb_mutex) {
        free(g_state);
        return -1;
    }

    memset(g_state->back_buffer, 0x00, BUFFER_SIZE);
    memset(g_state->front_buffer, 0x00, BUFFER_SIZE);

    EEPROM.begin();
    furi_delay_ms(50);
    Platform::SetAudioEnabled(EEPROM.read(2) != 0);
    Game::menu.ReadScore();

    g_state->gui = (Gui*)furi_record_open(RECORD_GUI);
    gui_add_framebuffer_callback(g_state->gui, framebuffer_commit_callback, g_state);
    g_state->canvas = gui_direct_draw_acquire(g_state->gui);

    g_state->input_events = (FuriPubSub*)furi_record_open(RECORD_INPUT_EVENTS);
    g_state->input_sub =
        furi_pubsub_subscribe(g_state->input_events, input_events_callback, g_state);

    const uint32_t tick_hz = furi_kernel_get_tick_frequency();
    uint32_t period_ticks = (tick_hz + (TARGET_FRAMERATE / 2)) / TARGET_FRAMERATE;
    if(period_ticks == 0) period_ticks = 1;

    uint32_t next_tick = furi_get_tick();

    while(!g_state->exit_requested) {
        uint32_t now = furi_get_tick();
        if((int32_t)(now - next_tick) < 0) {
            uint32_t dt_ticks = next_tick - now;
            uint32_t dt_ms = (dt_ticks * 1000u) / tick_hz;
            if(dt_ms) furi_delay_ms(dt_ms);
            continue;
        }
        next_tick += period_ticks;

        Game::Tick();
        Game::Draw();

        furi_mutex_acquire(g_state->fb_mutex, FuriWaitForever);
        memcpy(g_state->front_buffer, g_state->back_buffer, BUFFER_SIZE);
        furi_mutex_release(g_state->fb_mutex);

        canvas_commit(g_state->canvas);
    }

    canvas_commit(g_state->canvas);
    furi_delay_ms(40);

    gui_remove_framebuffer_callback(g_state->gui, framebuffer_commit_callback, g_state);
    Platform::SetAudioEnabled(false);

    if(g_state->input_sub)
        furi_pubsub_unsubscribe(g_state->input_events, g_state->input_sub);

    furi_record_close(RECORD_INPUT_EVENTS);

    gui_direct_draw_release(g_state->gui);
    furi_record_close(RECORD_GUI);

    furi_mutex_free(g_state->fb_mutex);
    free(g_state);
    g_state = NULL;

    return 0;
}
