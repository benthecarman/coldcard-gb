//
// hal_stm32.c - the parts of gb_hal.h the COLDCARD Q and Mk4 share: the
// clock, and ROM storage.
//
// ROM: lower half of the PSRAM, memory mapped by the bootloader. Writes
// must be whole 32-bit words.
//
#include <string.h>

#include "py/mphal.h"

#include "gb_hal.h"

#define PSRAM_BASE          ((uint8_t *)0x90000000)
#define PSRAM_ROM_MAX       0x400000            // lower half; Virtual Disk has the top

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
