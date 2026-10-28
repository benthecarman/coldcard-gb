//
// gb_oled.h - Game Boy frame buffer to the Mk4's 128x64 one-bit OLED.
//
// The OLED always shows the whole 160x144 picture, compressed to fit, in
// one of two ways:
//
// - Fill: squashed to 128x64, so columns shrink by 4/5 and lines by 4/9.
//   Where pixels merge, the darkest one wins, so thin dark lines (text
//   strokes, outlines) survive.
// - Fit: scaled by 4/9 both ways, to 71x64 in the middle of the OLED, so
//   shapes keep their proportions. Each OLED pixel covers about 2x2 Game
//   Boy pixels; their brightness is averaged, with extra weight on the
//   darkest so outlines stay visible, and then dithered.
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
    VIDEO_OLED_FILL = 16,   // 128x64, squashed
    VIDEO_OLED_FIT,         // 71x64, in proportion
    VIDEO_OLED_END
};

#define VIDEO_IS_OLED(m)    ((m) >= VIDEO_OLED_FILL && (m) < VIDEO_OLED_END)

typedef struct {
    int mode;
    int dst_x, dst_w;           // OLED columns drawn
    uint8_t lit[4];             // per shade: 4 bits, one per 2x2 dither cell
    uint8_t lum[4];             // per shade: brightness, 0-255
    uint32_t lum4[256];         // Fit: a packed byte as 4 brightnesses
    uint8_t col_a[OLED_W];      // GB columns merged into each OLED column
    uint8_t col_b[OLED_W];      //   drawn, from dst_x
    uint8_t row_a[OLED_H];      // GB lines merged into each OLED row
    uint8_t row_n[OLED_H];
} gb_oled_t;

// Set mode and shading. Palette entries are 0xRRGGBB, shade 0 first.
void gb_oled_setup(gb_oled_t *o, int mode, const uint32_t rgb[4]);

// Render the whole OLED image into out.
void gb_oled_render(const gb_oled_t *o, const uint8_t fb[GB_H][GB_LINE_BYTES],
                    uint8_t out[OLED_BYTES]);
