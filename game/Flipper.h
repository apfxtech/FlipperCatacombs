#pragma once

#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <stdbool.h>
#include <stdint.h>

#define DISPLAY_WIDTH  128
#define DISPLAY_HEIGHT 64
#define BUFFER_SIZE    (DISPLAY_WIDTH * DISPLAY_HEIGHT / 8)

typedef struct {
    uint8_t framebuffer[BUFFER_SIZE];

    FuriMutex* mutex;

    volatile uint8_t input_state;
    volatile bool exit_requested;
    volatile bool audio_enabled;
} FlipperState;

extern FlipperState* g_state;
