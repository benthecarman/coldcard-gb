//
// gb_hal.h - platform layer for the Game Boy emulator.
//
// Implemented by hal_q1.c (COLDCARD Q hardware), hal_unix.c (desktop
// simulator) and gameboy/test/hal_test.c (headless test runner).
//
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// LCD is 320x240, RGB565.
#define HAL_LCD_W   320
#define HAL_LCD_H   240

// True if hal_lcd_write() expects pixel words with the high byte first in
// memory (what the Q's SPI LCD wants). Palette tables are built to match.
extern const bool hal_lcd_big_endian;

// Take / release the display (the Q shares its SPI bus with the GPU micro).
void hal_lcd_begin(void);
void hal_lcd_end(void);

// Set the drawing window; pixel data written next fills it row by row.
void hal_lcd_window(int x, int y, int w, int h);

// Send pixels into the current window. Count is always whole rows of the
// window. May return before the transfer is done: the caller must not touch
// the buffer until the next hal_lcd_write(), hal_lcd_window(), hal_lcd_fill()
// or hal_lcd_sync() call returns.
void hal_lcd_write(const uint16_t *px, int count);

// Wait until any pixel transfer has finished.
void hal_lcd_sync(void);

// Fill a rectangle with one colour (pixel already in wire byte order).
void hal_lcd_fill(int x, int y, int w, int h, uint16_t px);

// Keyboard: bit N set when Q key number N (row*10 + col, as in
// shared/charcodes.py) is held right now.
uint64_t hal_keys(void);

// Monotonic microsecond clock, and a wait until it reaches a value.
uint64_t hal_micros(void);
void hal_wait_until(uint64_t when_us);

// ROM image storage (PSRAM on the Q). Writes go through hal_rom_write()
// because Q PSRAM needs word-aligned writes.
uint8_t *hal_rom_alloc(uint32_t size);
void hal_rom_write(uint8_t *dst, const uint8_t *src, size_t len);
void hal_rom_free(void);
