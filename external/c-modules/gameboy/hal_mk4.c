//
// hal_mk4.c - gb_hal.h for the COLDCARD Mk4 (STM32L4S5).
//
// OLED: SSD1309, 128x64, on SPI1; pins and commands as in shared/display.py
// and shared/ssd1306.py. Python sets the bus up for the OLED before a game
// runs. Frames are 1 KB, sent without DMA (about 0.2 ms at 40 MHz).
//
// Keys: the 4x3 membrane matrix is scanned here directly. The firmware's
// keypad driver (shared/mempad.py) scans from a timer and pin interrupts,
// which Python turns off while a game runs.
//
// Time and ROM storage are in hal_stm32.c, shared with the Q.
//
#include "py/mphal.h"
#include "py/runtime.h"
#include "spi.h"

#include "gb_hal.h"

const bool hal_lcd_big_endian = false;

#define PIN_OLED_CS         pin_A4
#define PIN_OLED_DATA_CMD   pin_A8

#define SET_COL_ADDR        0x21
#define SET_PAGE_ADDR       0x22

#define OLED_SPI            (&spi_obj[0])       // SPI1

// keypad matrix, as in stm32/COLDCARD_MK4/pins.csv (M2_ROWn, M2_COLn);
// key number is row * 3 + col, as in shared/mempad.py
static const pin_obj_t *const key_rows[4] = {
    pin_D8, pin_D9, pin_D10, pin_D11,
};
static const pin_obj_t *const key_cols[3] = {
    pin_B0, pin_B1, pin_B2,
};

// The Mk4 has no colour LCD: the core only calls these in LCD video
// modes, which Python doesn't offer here.
void hal_lcd_begin(void) { }
void hal_lcd_end(void) { }
void hal_lcd_sync(void) { }
void hal_lcd_window(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
void hal_lcd_write(const uint16_t *px, int count) { (void)px; (void)count; }
void hal_lcd_fill(int x, int y, int w, int h, uint16_t px) { (void)x; (void)y; (void)w; (void)h; (void)px; }

// One command byte, framed by chip select like SSD1306_SPI.write_cmd()
static void oled_cmd(uint8_t cmd)
{
    mp_hal_pin_write(PIN_OLED_CS, 1);
    mp_hal_pin_write(PIN_OLED_DATA_CMD, 0);
    mp_hal_pin_write(PIN_OLED_CS, 0);
    HAL_SPI_Transmit(OLED_SPI->spi, &cmd, 1, 10);
    mp_hal_pin_write(PIN_OLED_CS, 1);
}

void hal_oled_write(const uint8_t *buf)
{
    // whole screen, as SSD1306.show() does
    oled_cmd(SET_COL_ADDR);
    oled_cmd(0);
    oled_cmd(127);
    oled_cmd(SET_PAGE_ADDR);
    oled_cmd(0);
    oled_cmd(7);

    mp_hal_pin_write(PIN_OLED_CS, 1);
    mp_hal_pin_write(PIN_OLED_DATA_CMD, 1);
    mp_hal_pin_write(PIN_OLED_CS, 0);
    HAL_SPI_Transmit(OLED_SPI->spi, (uint8_t *)buf, 1024, 10);
    mp_hal_pin_write(PIN_OLED_CS, 1);
}

uint64_t hal_keys(void)
{
    uint64_t held = 0;

    for(int r = 0; r < 4; r++) {
        // open drain: 0 pulls the row low, 1 lets it float high
        for(int i = 0; i < 4; i++) {
            mp_hal_pin_write(key_rows[i], i != r);
        }

        // let the column pull-ups settle
        mp_hal_delay_us(10);

        for(int c = 0; c < 3; c++) {
            if(!mp_hal_pin_read(key_cols[c])) {
                held |= 1ULL << (r * 3 + c);
            }
        }
    }

    // all rows low: how mempad.py waits for any key
    for(int i = 0; i < 4; i++) {
        mp_hal_pin_write(key_rows[i], 0);
    }

    return held;
}
