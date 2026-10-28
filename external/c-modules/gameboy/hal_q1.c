//
// hal_q1.c - gb_hal.h for the COLDCARD Q (STM32L4S5).
//
// LCD: ST7789 on SPI1 at 60 MHz. Pixel rows go out by DMA while the CPU
// emulates; Python must take the SPI bus from the GPU co-processor first
// (gpu.take_spi()). Pins and commands as in stm32/COLDCARD_Q1/modlcd.c.
//
// Keys: the 6x10 matrix is scanned here directly. The firmware's keyboard
// driver (shared/keyboard.py) runs from soft interrupts, which don't run
// while a game does.
//
// ROM: lower half of the PSRAM, memory mapped by the bootloader. Writes
// must be whole 32-bit words.
//
#include <string.h>

#include "py/mphal.h"
#include "py/runtime.h"
#include "spi.h"
#include "dma.h"

#include "gb_hal.h"

const bool hal_lcd_big_endian = true;

#define PIN_LCD_CS          pin_A4
#define PIN_LCD_DATA_CMD    pin_A8

#define CASET               0x2a
#define RASET               0x2b
#define RAMWR               0x2c

#define LCD_SPI             (&spi_obj[0])       // SPI1

#define PSRAM_BASE          ((uint8_t *)0x90000000)
#define PSRAM_ROM_MAX       0x400000            // lower half; Virtual Disk has the top

static DMA_HandleTypeDef lcd_dma;
static bool lcd_dma_on;

// key matrix, as in stm32/COLDCARD_Q1/pins.csv (Q1_ROWn, Q1_COLn)
static const pin_obj_t *const key_rows[6] = {
    pin_D8, pin_D9, pin_D10, pin_D11, pin_D12, pin_D7,
};
static const pin_obj_t *const key_cols[10] = {
    pin_B0, pin_B1, pin_B2, pin_B5, pin_B8, pin_B9, pin_B10, pin_D13, pin_D14, pin_D15,
};

// Wait for the last pixel DMA to finish and the SPI to go idle.
static void lcd_wait(void)
{
    SPI_HandleTypeDef *spi = LCD_SPI->spi;
    uint32_t t0 = HAL_GetTick();

    while(spi->State != HAL_SPI_STATE_READY) {
        if(HAL_GetTick() - t0 > 100) {
            // lost an interrupt? don't hang the game
            HAL_SPI_Abort(spi);
            break;
        }
    }
}

static void lcd_cmd(uint8_t cmd, const uint8_t *args, int len)
{
    SPI_HandleTypeDef *spi = LCD_SPI->spi;

    mp_hal_pin_write(PIN_LCD_CS, 1);
    mp_hal_pin_write(PIN_LCD_DATA_CMD, 0);
    mp_hal_pin_write(PIN_LCD_CS, 0);
    HAL_SPI_Transmit(spi, &cmd, 1, 10);

    if(len) {
        mp_hal_pin_write(PIN_LCD_DATA_CMD, 1);
        HAL_SPI_Transmit(spi, (uint8_t *)args, len, 10);
    }
}

void hal_lcd_begin(void)
{
    const spi_t *s = LCD_SPI;

    // DMA stays set up for the whole run, not per transfer as spi_transfer() does
    dma_init(&lcd_dma, s->tx_dma_descr, DMA_MEMORY_TO_PERIPH, s->spi);
    s->spi->hdmatx = &lcd_dma;
    s->spi->hdmarx = NULL;
    lcd_dma_on = true;
}

void hal_lcd_end(void)
{
    const spi_t *s = LCD_SPI;

    hal_lcd_sync();
    if(lcd_dma_on) {
        dma_deinit(s->tx_dma_descr);
        s->spi->hdmatx = NULL;
        lcd_dma_on = false;
    }
}

void hal_lcd_window(int x, int y, int w, int h)
{
    uint8_t col[4] = { x >> 8, x, (x + w - 1) >> 8, x + w - 1 };
    uint8_t row[4] = { y >> 8, y, (y + h - 1) >> 8, y + h - 1 };

    lcd_wait();
    lcd_cmd(CASET, col, 4);
    lcd_cmd(RASET, row, 4);
    lcd_cmd(RAMWR, NULL, 0);

    // pixel data follows
    mp_hal_pin_write(PIN_LCD_DATA_CMD, 1);
}

void hal_lcd_write(const uint16_t *px, int count)
{
    SPI_HandleTypeDef *spi = LCD_SPI->spi;

    lcd_wait();
    if(!lcd_dma_on || HAL_SPI_Transmit_DMA(spi, (uint8_t *)px, count * 2) != HAL_OK) {
        HAL_SPI_Transmit(spi, (uint8_t *)px, count * 2, 100);
    }
}

void hal_lcd_sync(void)
{
    lcd_wait();
    mp_hal_pin_write(PIN_LCD_CS, 1);
}

void hal_lcd_fill(int x, int y, int w, int h, uint16_t px)
{
    static uint16_t line[HAL_LCD_W];

    if(w <= 0 || h <= 0) return;

    hal_lcd_window(x, y, w, h);

    lcd_wait();             // line[] may still be going out
    for(int i = 0; i < w; i++) line[i] = px;
    for(int r = 0; r < h; r++) {
        hal_lcd_write(line, w);
    }
    hal_lcd_sync();
}

void hal_oled_write(const uint8_t *buf)
{
    // the Q has no OLED; Python only offers the LCD video modes here
    (void)buf;
}

uint64_t hal_keys(void)
{
    uint64_t held = 0;

    for(int r = 0; r < 6; r++) {
        // open drain: 0 pulls the row low, 1 lets it float high
        for(int i = 0; i < 6; i++) {
            mp_hal_pin_write(key_rows[i], i != r);
        }

        // column pull-ups through the 1K series resistors need a moment
        mp_hal_delay_us(10);

        for(int c = 0; c < 10; c++) {
            if(!mp_hal_pin_read(key_cols[c])) {
                held |= 1ULL << (r * 10 + c);
            }
        }
    }

    // all rows low: how keyboard.py waits for any key
    for(int i = 0; i < 6; i++) {
        mp_hal_pin_write(key_rows[i], 0);
    }

    return held;
}

uint64_t hal_micros(void)
{
    // mp_hal_ticks_us() is 32 bits (wraps after 71 minutes); extend it
    static uint32_t last;
    static uint64_t high;

    uint32_t now = mp_hal_ticks_us();
    if(now < last) high += 1ULL << 32;
    last = now;

    return high | now;
}

void hal_wait_until(uint64_t when_us)
{
    // sleep to the next SysTick (1 ms) while there's time, then spin
    while(hal_micros() + 1100 < when_us) {
        __WFI();
    }
    while(hal_micros() < when_us) { }
}

uint8_t *hal_rom_alloc(uint32_t size)
{
    return (size <= PSRAM_ROM_MAX) ? PSRAM_BASE : NULL;
}

void hal_rom_write(uint8_t *dst, const uint8_t *src, size_t len)
{
    volatile uint32_t *d = (volatile uint32_t *)dst;
    size_t i = 0;

    for(; i + 4 <= len; i += 4) {
        uint32_t w;
        memcpy(&w, src + i, 4);
        *d++ = w;
    }
    if(i < len) {
        uint32_t w = *d;
        memcpy(&w, src + i, len - i);
        *d = w;
    }
}

void hal_rom_free(void)
{
    // nothing: the PSRAM is just scratch space
}
