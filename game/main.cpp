#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>

#include <stdlib.h>
#include <string.h>

#include "game/Flipper.h"
#include "game/Game.h"
#include "game/Platform.h"

// Icons live compressed in flash and are unpacked by the firmware only while drawing
extern "C" {
#include <catacombs_icons.h>
}

#define TARGET_FRAMERATE 30

// Title logo position inside the 128x64 artwork
#define LOGO_X 28
#define LOGO_Y 11

FlipperState* g_state = NULL;

typedef struct {
    FlipperState* state;
    FuriMessageQueue* input_queue;
    ViewPort* view_port;
    Gui* gui;
} CatacombsApp;

static void draw_callback(Canvas* canvas, void* context) {
    CatacombsApp* app = (CatacombsApp*)context;

    uint8_t* target = canvas_get_buffer(canvas);
    size_t size = canvas_get_buffer_size(canvas);
    if(size > BUFFER_SIZE) size = BUFFER_SIZE;

    furi_mutex_acquire(app->state->mutex, FuriWaitForever);

    const uint8_t* source = app->state->framebuffer;
    for(size_t i = 0; i < size; i++) {
        target[i] = (uint8_t)~source[i];
    }

    const bool title = (Game::menu.GetSplashPhase() == Menu::SplashPhase::Title);

    furi_mutex_release(app->state->mutex);

    if(!title) return;

    canvas_set_bitmap_mode(canvas, true);

    canvas_set_color(canvas, ColorWhite);
    canvas_draw_frame(canvas, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    canvas_draw_icon(canvas, LOGO_X, LOGO_Y, &I_logo_fill);

    canvas_set_color(canvas, ColorBlack);
    canvas_draw_frame(canvas, 1, 1, DISPLAY_WIDTH - 2, DISPLAY_HEIGHT - 2);
    canvas_draw_icon(canvas, LOGO_X, LOGO_Y, &I_logo_ink);

    canvas_set_bitmap_mode(canvas, false);
}

static void input_callback(InputEvent* event, void* context) {
    CatacombsApp* app = (CatacombsApp*)context;
    furi_message_queue_put(app->input_queue, event, 0);
}

static uint8_t button_from_key(InputKey key) {
    switch(key) {
    case InputKeyUp:
        return INPUT_UP;
    case InputKeyDown:
        return INPUT_DOWN;
    case InputKeyLeft:
        return INPUT_LEFT;
    case InputKeyRight:
        return INPUT_RIGHT;
    case InputKeyOk:
        return INPUT_A | INPUT_B;
    default:
        return 0;
    }
}

static void input_apply(CatacombsApp* app, const InputEvent* event) {
    if(event->key == InputKeyBack) {
        if(event->type == InputTypeLong) {
            if(Game::InMenu())
                app->state->exit_requested = true;
            else
                Game::GoToMenu();
        }
        return;
    }

    const uint8_t bit = button_from_key(event->key);
    if(!bit) return;

    if((event->type == InputTypePress) || (event->type == InputTypeRepeat)) {
        app->state->input_state |= bit;
    } else if(event->type == InputTypeRelease) {
        app->state->input_state &= (uint8_t)~bit;
    }
}

static void frame_advance(CatacombsApp* app) {
    furi_mutex_acquire(app->state->mutex, FuriWaitForever);

    Game::Tick();
    Game::Draw();

    furi_mutex_release(app->state->mutex);
}

extern "C" int32_t arduboy3d_app(void* p) {
    UNUSED(p);

    CatacombsApp* app = (CatacombsApp*)malloc(sizeof(CatacombsApp));
    memset(app, 0, sizeof(CatacombsApp));

    app->state = (FlipperState*)malloc(sizeof(FlipperState));
    memset(app->state, 0, sizeof(FlipperState));
    g_state = app->state;

    app->state->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->input_queue = furi_message_queue_alloc(16, sizeof(InputEvent));

    Platform::SetAudioEnabled(!furi_hal_rtc_is_flag_set(FuriHalRtcFlagStealthMode));
    Game::menu.ReadSave();

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, draw_callback, app);
    view_port_input_callback_set(app->view_port, input_callback, app);

    app->gui = (Gui*)furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    const uint32_t period = furi_kernel_get_tick_frequency() / TARGET_FRAMERATE;
    uint32_t next_frame = furi_get_tick() + period;

    while(!app->state->exit_requested) {
        int32_t remaining = (int32_t)(next_frame - furi_get_tick());

        InputEvent event;
        if(remaining > 0 &&
           furi_message_queue_get(app->input_queue, &event, (uint32_t)remaining) == FuriStatusOk) {
            input_apply(app, &event);
            continue;
        }

        next_frame += period;
        if((int32_t)(furi_get_tick() - next_frame) > (int32_t)period) {
            next_frame = furi_get_tick() + period;
        }

        frame_advance(app);
        view_port_update(app->view_port);
    }

    gui_remove_view_port(app->gui, app->view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(app->view_port);

    Game::menu.WriteSave();

    Platform::SetAudioEnabled(false);

    furi_message_queue_free(app->input_queue);
    furi_mutex_free(app->state->mutex);

    free(app->state);
    g_state = NULL;
    free(app);

    return 0;
}
