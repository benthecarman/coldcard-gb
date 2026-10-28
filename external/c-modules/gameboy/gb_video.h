//
// gb_video.h - Game Boy frame buffer to LCD: palettes and scalers.
//
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define GB_W            160
#define GB_H            144
// Frame buffers hold 2-bit shades, 4 pixels per byte, pixel 0 in bits 1-0.
#define GB_LINE_BYTES   (GB_W / 4)

enum {
    VIDEO_1X = 0,           // 160x144, centered
    VIDEO_15_SMOOTH,        // 240x216, centered, blended
    VIDEO_15_SHARP,         // 240x216, centered, nearest neighbour
    VIDEO_2X_CROP,          // 320x240 from the middle 160x120
    VIDEO_NUM_MODES
};

// Most source lines one group covers, and most output pixels one group makes.
#define VIDEO_MAX_GROUP_PX  (3 * 240)

typedef struct {
    int mode;

    // output rectangle on the LCD
    int x, y, w, h;

    // Output is made in groups: `src_lines` GB lines become `out_rows` LCD
    // rows. `src_first` is the first GB line shown, `groups` how many.
    int src_first, src_lines, out_rows, groups;

    // colours, already in LCD wire byte order
    uint16_t pal[4];
    uint16_t mix2[16];      // blend of shades a,b: index a | b<<2
    uint16_t mix4[256];     // blend of 4 shades: a | b<<2 | c<<4 | d<<6
    uint16_t border;
} gb_video_t;

// Set mode and colours. Palette entries are 0xRRGGBB, shade 0 (lightest)
// first. Border is 0xRRGGBB too.
void gb_video_setup(gb_video_t *v, int mode, const uint32_t rgb[4],
                    uint32_t border_rgb, bool big_endian);

// Render one output group (v->out_rows rows of v->w pixels) into dst.
void gb_video_render_group(const gb_video_t *v,
                           const uint8_t fb[GB_H][GB_LINE_BYTES],
                           int group, uint16_t *dst);
