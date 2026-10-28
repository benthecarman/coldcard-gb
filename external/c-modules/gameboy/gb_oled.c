//
// gb_oled.c - Game Boy frame buffer to the Mk4's one-bit OLED.
//
#include <string.h>

#include "gb_oled.h"

// 2x2 ordered dither: cell (x & 1) + 2 * (y & 1) lights up when fewer
// than this many of the four cells should be lit
static const uint8_t BAYER[4] = { 0, 2, 3, 1 };

    void
gb_oled_setup(gb_oled_t *o, int mode, const uint32_t rgb[4])
{
    if(!VIDEO_IS_OLED(mode)) mode = VIDEO_OLED_MIDDLE;
    o->mode = mode;

    if(mode == VIDEO_OLED_FIT) {
        o->first = 0;
        o->lines = GB_H;
    } else {
        o->first = (mode - VIDEO_OLED_TOP) * 40;
        o->lines = OLED_H;
    }

    // rows: each OLED row covers lines/64 GB lines (1, or 2.25 for fit)
    for(int y = 0; y < OLED_H; y++) {
        int a = o->first + y * o->lines / OLED_H;
        int b = o->first + (y + 1) * o->lines / OLED_H;
        o->row_a[y] = a;
        o->row_n[y] = (b > a) ? b - a : 1;
    }

    // columns: 160 into 128 is 5 into 4; each OLED column covers 1 or 2,
    // and each GB column goes to exactly one OLED column
    for(int x = 0; x < OLED_W; x++) {
        int a = x * GB_W / OLED_W;
        int b = (x + 1) * GB_W / OLED_W - 1;
        o->col_a[x] = a;
        o->col_b[x] = b;
    }

    // shading: lit cells out of 4 from the palette brightness
    for(int s = 0; s < 4; s++) {
        uint32_t c = rgb[s];
        uint32_t lum = (((c >> 16) & 0xff) * 3 + ((c >> 8) & 0xff) * 6 + (c & 0xff)) / 10;
        int n = (lum * 4 + 127) / 255;

        uint8_t bits = 0;
        for(int cell = 0; cell < 4; cell++) {
            if(BAYER[cell] < n) bits |= 1 << cell;
        }
        o->lit[s] = bits;
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

    void
gb_oled_render(const gb_oled_t *o, const uint8_t fb[GB_H][GB_LINE_BYTES],
               uint8_t out[OLED_BYTES])
{
    uint8_t row[GB_W], more[GB_W];

    memset(out, 0, OLED_BYTES);

    for(int y = 0; y < OLED_H; y++) {
        // darkest shade (highest number) of the GB lines in this row
        unpack(fb[o->row_a[y]], row);
        for(int k = 1; k < o->row_n[y]; k++) {
            unpack(fb[o->row_a[y] + k], more);
            for(int x = 0; x < GB_W; x++) {
                if(more[x] > row[x]) row[x] = more[x];
            }
        }

        uint8_t *dst = out + (y >> 3) * OLED_W;
        const uint8_t bit = 1 << (y & 7);
        const int cell_y = (y & 1) * 2;

        for(int x = 0; x < OLED_W; x++) {
            uint8_t s = row[o->col_a[x]];
            uint8_t t = row[o->col_b[x]];
            if(t > s) s = t;

            if((o->lit[s] >> (cell_y + (x & 1))) & 1) dst[x] |= bit;
        }
    }
}
