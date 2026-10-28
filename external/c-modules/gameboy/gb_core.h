//
// gb_core.h - Game Boy emulator core for COLDCARD Q, built on Peanut-GB.
//
// Platform independent: talks to hardware only through gb_hal.h, and has no
// MicroPython dependency (see modgameboy.c for the Python bindings).
//
#pragma once

#include <stdint.h>
#include <stdbool.h>

// Peanut-GB configuration; must match in every file including peanut_gb.h
#define ENABLE_SOUND                0
#define ENABLE_LCD                  1
#define PEANUT_GB_12_COLOUR         0
#define PEANUT_GB_HIGH_LCD_ACCURACY 1

#ifndef GB_CORE_IMPL
# define PEANUT_GB_HEADER_ONLY
#endif
#include "peanut_gb.h"

#include "gb_video.h"

#define GB_BANK_SIZE        0x4000
#define GB_MAX_SLOTS        16

// ccgb_run() results
enum {
    GB_RUN_FRAMES = 0,      // ran the requested number of frames
    GB_RUN_MENU,            // menu key pressed
    GB_RUN_SAVE,            // cart RAM changed and should be written out
    GB_RUN_ERROR,           // emulation error, see ccgb_t.error
};

// Game Boy buttons, in Peanut-GB's JOYPAD_* bit order.
enum {
    GB_BTN_A = 0, GB_BTN_B, GB_BTN_SELECT, GB_BTN_START,
    GB_BTN_RIGHT, GB_BTN_LEFT, GB_BTN_UP, GB_BTN_DOWN,
    GB_NUM_BTN
};

#define GB_KEYS_PER_BTN     2
#define GB_NO_KEY           0xff

// Q key numbers (row*10 + col) for each button, then the menu and
// fast-forward keys. Unused entries are GB_NO_KEY.
typedef struct {
    uint8_t btn[GB_NUM_BTN][GB_KEYS_PER_BTN];
    uint8_t menu;
    uint8_t fast;
} gb_keymap_t;

#define GB_KEYMAP_BYTES     (GB_NUM_BTN * GB_KEYS_PER_BTN + 2)

typedef struct {
    uint32_t frames;            // emulated
    uint32_t shown;             // sent to the LCD
    uint32_t skipped;           // not sent because emulation was behind
    uint32_t lines_sent;        // output groups sent
    uint32_t cache_hits, cache_misses;
    uint64_t emu_us, lcd_us, idle_us;   // time spent in each part
} gb_stats_t;

typedef struct ccgb_s {
    struct gb_s gb;

    // ROM: whole image (slow memory on the Q), bank 0 copy, bank cache
    const uint8_t *rom;
    uint32_t rom_size;
    uint16_t rom_banks;
    uint8_t *bank0;
    uint8_t *slot_mem[GB_MAX_SLOTS];
    uint16_t slot_bank[GB_MAX_SLOTS];
    uint32_t slot_used[GB_MAX_SLOTS];
    int n_slots;
    uint16_t cur_bank;
    const uint8_t *cur_mem;
    uint32_t use_clock;

    // cartridge RAM, and when it last changed (in frames)
    uint8_t *cart_ram;
    uint32_t cart_ram_size;
    bool ram_dirty;
    uint32_t ram_first_write, ram_last_write;

    // frame buffers: being drawn, and what the LCD shows now; a line is
    // dirty when the two differ
    uint8_t fb[GB_H][GB_LINE_BYTES] __attribute__((aligned(4)));
    uint8_t fb_shown[GB_H][GB_LINE_BYTES] __attribute__((aligned(4)));
    bool line_dirty[GB_H];
    bool full_redraw;
    gb_video_t video;
    uint16_t out_buf[2][4 * VIDEO_MAX_GROUP_PX];
    int out_next;               // which out_buf to fill next

    gb_keymap_t keymap;
    uint64_t last_keys;

    // pacing, in nanoseconds of the monotonic clock
    uint64_t deadline_ns;
    uint32_t frames_unshown;

    gb_stats_t stats;

    int error;                  // enum gb_error_e + 1, or 0
    uint16_t error_addr;

    // serial port output (Blargg's test ROMs print results there)
    char serial[512];
    int serial_len;
} ccgb_t;

// Start emulation. ROM image and cart RAM buffers belong to the caller;
// bank0 must be GB_BANK_SIZE bytes, slots[] n_slots buffers of GB_BANK_SIZE.
// Returns enum gb_init_error_e.
int ccgb_init(ccgb_t *c, const uint8_t *rom, uint32_t rom_size,
              uint8_t *bank0, uint8_t **slots, int n_slots);

// Cart RAM size the loaded ROM wants (call after ccgb_init).
uint32_t ccgb_cart_ram_size(ccgb_t *c);
void ccgb_set_cart_ram(ccgb_t *c, uint8_t *ram, uint32_t size);

void ccgb_set_keymap(ccgb_t *c, const uint8_t map[GB_KEYMAP_BYTES]);
void ccgb_set_video(ccgb_t *c, int mode, const uint32_t rgb[4], uint32_t border);

// Cart RAM was written out: clear the dirty flag.
void ccgb_saved(ccgb_t *c);

// Run until something needs the caller (see GB_RUN_*), or max_frames
// have run (0 = no limit). Set redraw if something else used the LCD.
int ccgb_run(ccgb_t *c, uint32_t max_frames, bool redraw);

// ROM title from the cartridge header (17 bytes incl. NUL)
void ccgb_title(ccgb_t *c, char title[17]);

// Save states: the emulator state (struct gb_s) plus cart RAM. A state
// file starts with a header (GB_STATE_HDR bytes) that names the ROM and
// this build's state layout; a state loads only if its header matches.
// The layout differs between builds for different CPUs.
#define GB_STATE_HDR        36

void ccgb_state_header(ccgb_t *c, uint8_t hdr[GB_STATE_HDR]);

// Load emulator state saved from &c->gb (sizeof(struct gb_s) bytes). The
// cart RAM is the caller's to restore; it is marked as needing a save.
void ccgb_state_load(ccgb_t *c, const uint8_t *state);
