//
// gb_oled.c - Game Boy frame buffer to the Mk4's one-bit OLED.
//
#include <string.h>

#include "gb_oled.h"

// 2x2 ordered dither: cell (x & 1) + 2 * (y & 1) lights up when fewer
// than this many of the four cells should be lit
static const uint8_t BAYER2[4] = { 0, 2, 3, 1 };

// 4x4 ordered dither for Fit: 17 levels
static const uint8_t BAYER4[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 },
};

// Fit: how far each pixel moves from the average brightness towards the
// darkest it covers, in quarters
#ifndef OLED_DARK_WEIGHT
#define OLED_DARK_WEIGHT    3
#endif

#define FIT_W               71          // 160x144 at 4/9

    void
gb_oled_setup(gb_oled_t *o, int mode, const uint32_t rgb[4])
{
    if(!VIDEO_IS_OLED(mode)) mode = VIDEO_OLED_FILL;
    o->mode = mode;
    o->dst_w = (mode == VIDEO_OLED_FIT) ? FIT_W : OLED_W;
    o->dst_x = (OLED_W - o->dst_w) / 2;

    // each OLED row covers 2 or 3 whole GB lines, and each GB line goes
    // to exactly one row; likewise for columns (1 or 2 in Fill, 2 or 3 in
    // Fit)
    for(int y = 0; y < OLED_H; y++) {
        int a = y * GB_H / OLED_H;
        int b = (y + 1) * GB_H / OLED_H;
        o->row_a[y] = a;
        o->row_n[y] = b - a;
    }
    for(int x = 0; x < o->dst_w; x++) {
        o->col_a[x] = x * GB_W / o->dst_w;
        o->col_b[x] = (x + 1) * GB_W / o->dst_w - 1;
    }

    // shading: brightness of each shade, and how many of 4 cells it lights
    for(int s = 0; s < 4; s++) {
        uint32_t c = rgb[s];
        uint32_t lum = (((c >> 16) & 0xff) * 3 + ((c >> 8) & 0xff) * 6 + (c & 0xff)) / 10;
        int n = (lum * 4 + 127) / 255;

        uint8_t bits = 0;
        for(int cell = 0; cell < 4; cell++) {
            if(BAYER2[cell] < n) bits |= 1 << cell;
        }
        o->lit[s] = bits;
        o->lum[s] = lum;
    }

    // brightness of the 4 pixels in a frame buffer byte, first in the low
    // byte, as they lie in memory (little endian)
    for(int b = 0; b < 256; b++) {
        o->lum4[b] = o->lum[b & 3] | (o->lum[(b >> 2) & 3] << 8)
                   | (o->lum[(b >> 4) & 3] << 16) | ((uint32_t)o->lum[b >> 6] << 24);
    }

}

// One GB line as a shade per pixel
static void unpack(const uint8_t *line, uint8_t out[GB_W])
{
    for(int i = 0; i < GB_LINE_BYTES; i++) {
        uint8_t b = line[i];
        out[4*i] = b & 3;
        out[4*i + 1] = (b >> 2) & 3;
        out[4*i + 2] = (b >> 4) & 3;
        out[4*i + 3] = b >> 6;
    }
}

// Fill: darkest shade of the pixels covered, dithered
static void render_darkest(const gb_oled_t *o, const uint8_t fb[GB_H][GB_LINE_BYTES],
                           uint8_t out[OLED_BYTES])
{
    uint8_t row[GB_W], more[GB_W];

    for(int y = 0; y < OLED_H; y++) {
        // darkest shade (highest number) of the GB lines in this row
        unpack(fb[o->row_a[y]], row);
        for(int k = 1; k < o->row_n[y]; k++) {
            unpack(fb[o->row_a[y] + k], more);
            for(int x = 0; x < GB_W; x++) {
                if(more[x] > row[x]) row[x] = more[x];
            }
        }

        uint8_t *dst = out + (y >> 3) * OLED_W + o->dst_x;
        const uint8_t bit = 1 << (y & 7);
        const int cell_y = (y & 1) * 2;

        for(int x = 0; x < o->dst_w; x++) {
            uint8_t s = row[o->col_a[x]];
            uint8_t t = row[o->col_b[x]];
            if(t > s) s = t;

            if((o->lit[s] >> (cell_y + ((o->dst_x + x) & 1))) & 1) dst[x] |= bit;
        }
    }
}

// Fit: brightness averaged over the pixels covered, pulled towards the
// darkest of them, dithered. Columns are merged first, line by line, then
// the lines of each row.
static void render_average(const gb_oled_t *o, const uint8_t fb[GB_H][GB_LINE_BYTES],
                           uint8_t out[OLED_BYTES])
{
    uint32_t lum_words[GB_LINE_BYTES];
    const uint8_t *lum = (const uint8_t *)lum_words;
    uint16_t sum[OLED_W];
    uint8_t low[OLED_W];
    uint8_t width[OLED_W];

    for(int x = 0; x < o->dst_w; x++) {
        width[x] = o->col_b[x] - o->col_a[x] + 1;
    }

    for(int y = 0; y < OLED_H; y++) {
        for(int x = 0; x < o->dst_w; x++) {
            sum[x] = 0;
            low[x] = 0xff;
        }

        for(int k = 0; k < o->row_n[y]; k++) {
            const uint8_t *line = fb[o->row_a[y] + k];
            for(int i = 0; i < GB_LINE_BYTES; i++) {
                lum_words[i] = o->lum4[line[i]];
            }

            // each OLED column covers 2 or 3 GB columns
            for(int x = 0; x < o->dst_w; x++) {
                const uint8_t *p = lum + o->col_a[x];
                uint8_t v0 = p[0], v1 = p[1];
                uint16_t t = v0 + v1;
                uint8_t lo = (v1 < v0) ? v1 : v0;
                if(width[x] > 2) {
                    t += p[2];
                    if(p[2] < lo) lo = p[2];
                }
                sum[x] += t;
                if(lo < low[x]) low[x] = lo;
            }
        }

        uint8_t *dst = out + (y >> 3) * OLED_W + o->dst_x;
        const uint8_t bit = 1 << (y & 7);
        const uint8_t *bayer = BAYER4[y & 3];

        for(int x = 0; x < o->dst_w; x++) {
            int avg = sum[x] / (width[x] * o->row_n[y]);
            int v = avg - (avg - low[x]) * OLED_DARK_WEIGHT / 4;

            // lit when v is above the cell's threshold: 0 lights none of
            // the 16 cells, 255 all of them
            if(v * 32 > (2 * bayer[(o->dst_x + x) & 3] + 1) * 255) dst[x] |= bit;
        }
    }
}

    void
gb_oled_render(const gb_oled_t *o, const uint8_t fb[GB_H][GB_LINE_BYTES],
               uint8_t out[OLED_BYTES])
{
    memset(out, 0, OLED_BYTES);

    if(o->mode == VIDEO_OLED_FIT) {
        render_average(o, fb, out);
    } else {
        render_darkest(o, fb, out);
    }
}
