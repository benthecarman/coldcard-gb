//
// gb_oled.h - Game Boy frame buffer to the Mk4's 128x64 one-bit OLED.
//
// The 160x144 picture doesn't fit, so there are three kinds of view:
//
// - Top, Middle, Bottom: 80 lines of the screen, all 160 columns, scaled
//   by 4/5 both ways to fill the OLED. Shapes keep their proportions.
//   Where pixels merge, the darkest one wins, so thin dark lines (text
//   strokes) survive.
// - Whole: the whole screen, scaled by 4/9 to 71x64 in the middle of the
//   OLED. Each OLED pixel covers about 2x2 Game Boy pixels; their
//   brightness is averaged, with extra weight on the darkest so outlines
//   stay visible, and then dithered.
// - Zoom: a 128x64 window at full size, one OLED pixel per Game Boy pixel.
//
// The windowed views can be moved (see gb_oled_pan()).
//
// Shades become on/off pixels by ordered dithering. How many pixels light
// up for each shade comes from the palette's brightness, so a black and
// white palette gives sharp two-tone output, a grey ramp gives dithered
// greys, and a reversed ramp gives inverse video.
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
    VIDEO_OLED_TOP = 16,    // GB lines 0-79, scaled 4/5
    VIDEO_OLED_MIDDLE,      // GB lines 32-111, scaled 4/5
    VIDEO_OLED_BOTTOM,      // GB lines 64-143, scaled 4/5
    VIDEO_OLED_FIT,         // the whole screen, 71x64
    VIDEO_OLED_ZOOM,        // 128x64 at full size, from the middle
    VIDEO_OLED_END
};

#define VIDEO_IS_OLED(m)    ((m) >= VIDEO_OLED_TOP && (m) < VIDEO_OLED_END)

typedef struct {
    int mode;
    int pan_x, pan_y;           // top left of the window, in GB pixels
    int max_x, max_y;           // how far the window can move
    int first, lines;           // GB lines used
    int dst_x, dst_w;           // OLED columns drawn
    bool average;               // Whole: average, don't take the darkest
    uint8_t lit[4];             // per shade: 4 bits, one per 2x2 dither cell
    uint8_t lum[4];             // per shade: brightness, 0-255
    uint32_t lum4[256];         // Whole: a packed byte as 4 brightnesses
    uint8_t col_a[OLED_W];      // GB columns merged into each OLED column
    uint8_t col_b[OLED_W];      //   drawn, from dst_x
    uint8_t row_a[OLED_H];      // GB lines merged into each OLED row
    uint8_t row_n[OLED_H];
} gb_oled_t;

// Set mode and shading, with the window where the mode puts it. Palette
// entries are 0xRRGGBB, shade 0 first.
void gb_oled_setup(gb_oled_t *o, int mode, const uint32_t rgb[4]);

// Move the window by dx, dy GB pixels, as far as it goes. Returns true if
// it moved.
bool gb_oled_pan(gb_oled_t *o, int dx, int dy);

// Render the whole OLED image into out.
void gb_oled_render(const gb_oled_t *o, const uint8_t fb[GB_H][GB_LINE_BYTES],
                    uint8_t out[OLED_BYTES]);
