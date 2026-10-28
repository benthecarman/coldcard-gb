//
// hal_test.c - gb_hal.h for the headless test runner.
//
// The LCD is an in-memory 320x240 RGB565 screen; OLED frames are drawn
// on it at 2x. Time is virtual: waiting moves the clock forward, so runs
// are fast and deterministic.
//
#include <stdlib.h>
#include <string.h>
#include "gb_hal.h"
#include "hal_test.h"

const bool hal_lcd_big_endian = false;

uint16_t test_screen[HAL_LCD_H][HAL_LCD_W];
uint64_t test_keys;
int test_lcd_writes;

static int win_x, win_y, win_w, win_h, win_pos;
static uint64_t now_us;
static uint8_t *rom_mem;

void hal_lcd_begin(void) { }
void hal_lcd_end(void) { }
void hal_lcd_sync(void) { }

void hal_lcd_window(int x, int y, int w, int h)
{
    if(x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > HAL_LCD_W || y + h > HAL_LCD_H) {
        test_fail("bad LCD window %d,%d %dx%d", x, y, w, h);
    }
    win_x = x; win_y = y; win_w = w; win_h = h; win_pos = 0;
}

void hal_lcd_write(const uint16_t *px, int count)
{
    if(count % win_w) test_fail("LCD write of %d px is not whole rows of %d", count, win_w);
    if(win_pos + count > win_w * win_h) test_fail("LCD write overflows window");

    for(int i = 0; i < count; i++, win_pos++) {
        test_screen[win_y + win_pos / win_w][win_x + win_pos % win_w] = px[i];
    }
    test_lcd_writes++;
}

void hal_lcd_fill(int x, int y, int w, int h, uint16_t px)
{
    hal_lcd_window(x, y, w, h);
    for(int r = 0; r < h; r++) {
        for(int i = 0; i < w; i++) test_screen[y + r][x + i] = px;
    }
}

void hal_oled_write(const uint8_t *buf)
{
    // lit pixels in the simulator's OLED colour (#ccf), on near black
    const uint16_t on = 0xce7f, off = 0x1082;
    const int x0 = (HAL_LCD_W - 2 * 128) / 2, y0 = (HAL_LCD_H - 2 * 64) / 2;

    for(int y = 0; y < 64; y++) {
        for(int x = 0; x < 128; x++) {
            uint16_t px = ((buf[(y / 8) * 128 + x] >> (y & 7)) & 1) ? on : off;
            for(int k = 0; k < 4; k++) {
                test_screen[y0 + 2 * y + k / 2][x0 + 2 * x + k % 2] = px;
            }
        }
    }
    test_lcd_writes++;
}

uint64_t hal_keys(void)
{
    return test_keys;
}

uint64_t hal_micros(void)
{
    return now_us;
}

void hal_wait_until(uint64_t when_us)
{
    if(when_us > now_us) now_us = when_us;
}

uint8_t *hal_rom_alloc(uint32_t size)
{
    free(rom_mem);
    rom_mem = malloc(size);
    return rom_mem;
}

void hal_rom_write(uint8_t *dst, const uint8_t *src, size_t len)
{
    memcpy(dst, src, len);
}

void hal_rom_free(void)
{
    free(rom_mem);
    rom_mem = NULL;
}
