//
// gbpy.c - the core as a shared library, for test scripts in Python
// (see gbpy.py). Same gb_core.c and hal_test.c as gbtest.
//
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gb_core.h"
#include "hal_test.h"

void test_fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FAIL: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    abort();
}

// The ROM image, shared by every emulator made after it
static uint8_t *rom;
static uint32_t rom_size;

int gbpy_rom(const uint8_t *data, uint32_t size)
{
    uint32_t padded = (size + GB_BANK_SIZE - 1) & ~(GB_BANK_SIZE - 1);
    uint8_t *tmp = malloc(padded);

    memset(tmp, 0xff, padded);
    memcpy(tmp, data, size);
    rom = hal_rom_alloc(padded);
    hal_rom_write(rom, tmp, padded);
    rom_size = padded;
    free(tmp);

    return 0;
}

// New emulator; buttons are key numbers 0..7 (A B SELECT START RIGHT
// LEFT UP DOWN), so keys for gbpy_run are a bit mask of buttons.
ccgb_t *gbpy_new(void)
{
    ccgb_t *c = calloc(1, sizeof(ccgb_t));
    uint8_t *bank0 = malloc(GB_BANK_SIZE);
    uint8_t *slots[6];
    for(int i = 0; i < 6; i++) slots[i] = malloc(GB_BANK_SIZE);

    if(ccgb_init(c, rom, rom_size, bank0, slots, 6)) return NULL;

    uint32_t ram = ccgb_cart_ram_size(c);
    if(ram) {
        uint8_t *mem = malloc(ram);
        memset(mem, 0xff, ram);
        ccgb_set_cart_ram(c, mem, ram);
    }

    uint8_t km[GB_KEYMAP_BYTES];
    memset(km, GB_NO_KEY, sizeof(km));
    for(int b = 0; b < GB_NUM_BTN; b++) km[b * GB_KEYS_PER_BTN] = b;
    km[offsetof(gb_keymap_t, menu)] = 63;
    ccgb_set_keymap(c, km);

    static const uint32_t grey[4] = { 0xffffff, 0xaaaaaa, 0x555555, 0x000000 };
    ccgb_set_video(c, VIDEO_1X, grey, 0);

    return c;
}

// Run frames holding these buttons. Returns 0, or the error code + 1.
int gbpy_run(ccgb_t *c, uint32_t frames, uint32_t buttons)
{
    uint32_t start = c->stats.frames;

    test_keys_at_start = true;
    test_keys = buttons;
    while(c->stats.frames - start < frames) {
        int rv = ccgb_run(c, frames - (c->stats.frames - start), false);
        if(rv == GB_RUN_ERROR) return c->error;
        if(rv == GB_RUN_SAVE) ccgb_saved(c);
    }
    test_keys = 0;

    return 0;
}

uint8_t *gbpy_wram(ccgb_t *c) { return c->gb.wram; }
uint8_t *gbpy_hram(ccgb_t *c) { return c->gb.hram_io; }
uint8_t *gbpy_cart_ram(ccgb_t *c) { return c->cart_ram; }
uint32_t gbpy_cart_ram_size(ccgb_t *c) { return c->cart_ram_size; }

void gbpy_reset(ccgb_t *c) { ccgb_reset(c); }

// Last frame drawn, one byte per pixel: shade 0 (white) .. 3
void gbpy_frame(ccgb_t *c, uint8_t *out)
{
    for(int y = 0; y < GB_H; y++) {
        for(int x = 0; x < GB_W; x++) {
            *out++ = (c->fb[y][x / 4] >> (2 * (x % 4))) & 3;
        }
    }
}

void gbpy_link_setup(ccgb_t *c, const uint8_t *name, const uint8_t *theirs, const uint8_t *mine)
{
    gb_link_mon_t t, m;

    memcpy(&t, theirs, sizeof(t));
    memcpy(&m, mine, sizeof(m));
    gb_link_setup(&c->link, name, &t, &m);
}

void gbpy_link_off(ccgb_t *c) { gb_link_off(&c->link); }

// state, traded, refused, party_ok
void gbpy_link_status(ccgb_t *c, int *out)
{
    out[0] = c->link.state;
    out[1] = c->link.traded;
    out[2] = c->link.refused;
    out[3] = c->link.party_ok;
}

int gbpy_link_mon_bytes(void) { return sizeof(gb_link_mon_t); }

// Save states: struct gb_s, the link, then cart RAM
uint32_t gbpy_state_size(ccgb_t *c)
{
    return sizeof(struct gb_s) + sizeof(gb_link_t) + c->cart_ram_size;
}

void gbpy_state_get(ccgb_t *c, uint8_t *out)
{
    memcpy(out, &c->gb, sizeof(struct gb_s));
    memcpy(out + sizeof(struct gb_s), &c->link, sizeof(gb_link_t));
    memcpy(out + sizeof(struct gb_s) + sizeof(gb_link_t), c->cart_ram, c->cart_ram_size);
}

void gbpy_state_set(ccgb_t *c, const uint8_t *in)
{
    memcpy(&c->link, in + sizeof(struct gb_s), sizeof(gb_link_t));
    memcpy(c->cart_ram, in + sizeof(struct gb_s) + sizeof(gb_link_t), c->cart_ram_size);
    ccgb_state_load(c, in);
}
