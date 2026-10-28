//
// gb_video.c - Game Boy frame buffer to LCD: palettes and scalers.
//
#include <string.h>
#include "gb_video.h"
#include "gb_hal.h"

static uint16_t rgb565(uint32_t r, uint32_t g, uint32_t b, bool big_endian)
{
    uint16_t px = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);

    return big_endian ? (uint16_t)((px >> 8) | (px << 8)) : px;
}

// Average of n 0xRRGGBB colours, as RGB565.
static uint16_t blend(const uint32_t *cols, int n, bool big_endian)
{
    uint32_t r = 0, g = 0, b = 0;

    for(int i = 0; i < n; i++) {
        r += (cols[i] >> 16) & 0xff;
        g += (cols[i] >> 8) & 0xff;
        b += cols[i] & 0xff;
    }

    return rgb565((r + n/2) / n, (g + n/2) / n, (b + n/2) / n, big_endian);
}

    void
gb_video_setup(gb_video_t *v, int mode, const uint32_t rgb[4],
               uint32_t border_rgb, bool big_endian)
{
    memset(v, 0, sizeof(*v));
    if(mode < 0 || mode >= VIDEO_NUM_MODES) mode = VIDEO_15_SMOOTH;
    v->mode = mode;

    switch(mode) {
        case VIDEO_1X:
            v->w = GB_W; v->h = GB_H;
            v->src_first = 0; v->src_lines = 1; v->out_rows = 1;
            break;

        case VIDEO_15_SMOOTH:
        case VIDEO_15_SHARP:
            v->w = 240; v->h = 216;
            v->src_first = 0; v->src_lines = 2; v->out_rows = 3;
            break;

        case VIDEO_2X_CROP:
            // 160x120 source window, vertically centered
            v->w = HAL_LCD_W; v->h = HAL_LCD_H;
            v->src_first = (GB_H - 120) / 2; v->src_lines = 1; v->out_rows = 2;
            break;
    }
    v->x = (HAL_LCD_W - v->w) / 2;
    v->y = (HAL_LCD_H - v->h) / 2;
    v->groups = v->h / v->out_rows;

    for(int i = 0; i < 4; i++) {
        v->pal[i] = blend(&rgb[i], 1, big_endian);
    }
    for(int i = 0; i < 16; i++) {
        uint32_t c[2] = { rgb[i & 3], rgb[(i >> 2) & 3] };
        v->mix2[i] = blend(c, 2, big_endian);
    }
    for(int i = 0; i < 256; i++) {
        uint32_t c[4] = { rgb[i & 3], rgb[(i >> 2) & 3],
                          rgb[(i >> 4) & 3], rgb[(i >> 6) & 3] };
        v->mix4[i] = blend(c, 4, big_endian);
    }
    v->border = blend(&border_rgb, 1, big_endian);
}

// Unpack one 2-bit packed line into one shade per byte.
static void unpack_line(const uint8_t *packed, uint8_t *out)
{
    for(int i = 0; i < GB_LINE_BYTES; i++) {
        uint8_t b = packed[i];
        out[0] = b & 3;
        out[1] = (b >> 2) & 3;
        out[2] = (b >> 4) & 3;
        out[3] = b >> 6;
        out += 4;
    }
}

    void
gb_video_render_group(const gb_video_t *v,
                      const uint8_t fb[GB_H][GB_LINE_BYTES],
                      int group, uint16_t *dst)
{
    uint8_t s0[GB_W], s1[GB_W];
    const uint16_t *pal = v->pal;
    int line = v->src_first + group * v->src_lines;

    unpack_line(fb[line], s0);

    switch(v->mode) {
        case VIDEO_1X:
            for(int x = 0; x < GB_W; x++) {
                dst[x] = pal[s0[x]];
            }
            break;

        case VIDEO_2X_CROP: {
            // middle 160 source pixels are all shown; each one doubled
            uint16_t *r1 = dst + v->w;
            for(int x = 0; x < GB_W; x++) {
                uint16_t px = pal[s0[x]];
                dst[2*x] = dst[2*x + 1] = px;
            }
            memcpy(r1, dst, v->w * sizeof(uint16_t));
            break;
        }

        case VIDEO_15_SHARP: {
            // nearest neighbour: 2 pixels -> [a, b, b], 2 rows -> [r0, r1, r1]
            unpack_line(fb[line + 1], s1);
            uint16_t *r1 = dst + v->w, *r2 = r1 + v->w;
            for(int x = 0, o = 0; x < GB_W; x += 2, o += 3) {
                dst[o] = pal[s0[x]];
                dst[o + 1] = dst[o + 2] = pal[s0[x + 1]];
                r1[o] = pal[s1[x]];
                r1[o + 1] = r1[o + 2] = pal[s1[x + 1]];
            }
            memcpy(r2, r1, v->w * sizeof(uint16_t));
            break;
        }

        case VIDEO_15_SMOOTH: {
            // 2x2 source pixels -> 3x3 output pixels; the middle row and
            // column are blends of their neighbours.
            unpack_line(fb[line + 1], s1);
            uint16_t *r1 = dst + v->w, *r2 = r1 + v->w;
            for(int x = 0, o = 0; x < GB_W; x += 2, o += 3) {
                uint8_t a0 = s0[x], b0 = s0[x + 1];
                uint8_t a1 = s1[x], b1 = s1[x + 1];

                dst[o] = pal[a0];
                dst[o + 1] = v->mix2[a0 | (b0 << 2)];
                dst[o + 2] = pal[b0];

                r1[o] = v->mix2[a0 | (a1 << 2)];
                r1[o + 1] = v->mix4[a0 | (b0 << 2) | (a1 << 4) | (b1 << 6)];
                r1[o + 2] = v->mix2[b0 | (b1 << 2)];

                r2[o] = pal[a1];
                r2[o + 1] = v->mix2[a1 | (b1 << 2)];
                r2[o + 2] = pal[b1];
            }
            break;
        }
    }
}
