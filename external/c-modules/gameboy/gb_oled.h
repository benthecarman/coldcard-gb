//
// gb_oled.h - Game Boy frame buffer to the Mk4's 128x64 one-bit OLED.
//
// The 160x144 picture doesn't fit, so there are four views: a 64-line
// window at the top, middle or bottom of the screen at full height, or
// the whole screen squashed to 64 lines. Every view squashes 160 columns
// to 128. Where pixels are merged, the darkest one wins, so thin dark
// lines (text strokes) survive.
//
// Shades become on/off pixels by 2x2 ordered dithering. How many of the
// four pixels light up for each shade comes from the palette's brightness,
// so a black and white palette gives sharp two-tone output, a grey ramp
// gives dithered greys, and a reversed ramp gives inverse video.
//
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "gb_video.h"

#define OLED_W          128
#define OLED_H          64
// SSD1309 page layout (MONO_VLSB): byte [page * 128 + x], bit y & 7
#define OLED_BYTES      (OLED_W * OLED_H / 8)

// Video modes for the OLED; numbered after the LCD modes in gb_video.h.
enum {
    VIDEO_OLED_TOP = 16,    // GB lines 0-63
    VIDEO_OLED_MIDDLE,      // GB lines 40-103
    VIDEO_OLED_BOTTOM,      // GB lines 80-143
    VIDEO_OLED_FIT,         // all 144 lines in 64
    VIDEO_OLED_END
};

#define VIDEO_IS_OLED(m)    ((m) >= VIDEO_OLED_TOP && (m) < VIDEO_OLED_END)

typedef struct {
    int mode;
    int first, lines;           // GB lines used
    uint8_t lit[4];             // per shade: 4 bits, one per 2x2 dither cell
    uint8_t col_a[OLED_W];      // GB columns merged into each OLED column
    uint8_t col_b[OLED_W];
    uint8_t row_a[OLED_H];      // GB lines merged into each OLED row
    uint8_t row_n[OLED_H];
} gb_oled_t;

// Set mode and shading. Palette entries are 0xRRGGBB, shade 0 first.
void gb_oled_setup(gb_oled_t *o, int mode, const uint32_t rgb[4]);

// Render the whole OLED image into out.
void gb_oled_render(const gb_oled_t *o, const uint8_t fb[GB_H][GB_LINE_BYTES],
                    uint8_t out[OLED_BYTES]);
