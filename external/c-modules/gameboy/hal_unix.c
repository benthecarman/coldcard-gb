//
// hal_unix.c - gb_hal.h for the desktop simulator (unix/simulator.py).
//
// Pixels go down the simulator's display pipe as 'r' (raw RGB565) and 'f'
// (fill) messages, same as unix/variant/st7788.py. Key state comes from the
// simulator's key pipe: 5-byte reports of held Q key numbers, 255 = none.
//
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "gb_hal.h"

const bool hal_lcd_big_endian = false;

static int display_fd = -1, keys_fd = -1;
static int win_x, win_y, win_w, win_h, win_row;
static uint64_t keys_held;
static uint8_t *rom_mem;

// _gameboy.sim_setup(display_fd, keys_fd)
STATIC mp_obj_t gb_sim_setup(mp_obj_t disp_in, mp_obj_t keys_in)
{
    display_fd = mp_obj_get_int(disp_in);
    keys_fd = mp_obj_get_int(keys_in);

    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(gb_sim_setup_obj, gb_sim_setup);

static void write_all(const void *data, size_t len)
{
    const uint8_t *p = data;

    while(len && display_fd >= 0) {
        ssize_t n = write(display_fd, p, len);
        if(n < 0) {
            if(errno == EINTR) continue;
            return;
        }
        p += n;
        len -= n;
    }
}

// header: struct.pack('<s6H', mode, x, y, w, h, count, arg)
static void send_header(char mode, int x, int y, int w, int h, int count)
{
    uint8_t hdr[13];
    uint16_t v[6] = { x, y, w, h, count, 0 };

    hdr[0] = mode;
    for(int i = 0; i < 6; i++) {
        hdr[1 + 2*i] = v[i] & 0xff;
        hdr[2 + 2*i] = v[i] >> 8;
    }
    write_all(hdr, sizeof(hdr));
}

void hal_lcd_begin(void) { }
void hal_lcd_end(void) { }
void hal_lcd_sync(void) { }

void hal_lcd_window(int x, int y, int w, int h)
{
    win_x = x; win_y = y; win_w = w; win_h = h;
    win_row = 0;
}

void hal_lcd_write(const uint16_t *px, int count)
{
    int rows = count / win_w;

    if(rows <= 0 || win_row + rows > win_h) return;

    // count field is 16 bits: callers send at most a few rows at a time
    send_header('r', win_x, win_y + win_row, win_w, rows, count * 2);
    write_all(px, count * 2);
    win_row += rows;
}

void hal_lcd_fill(int x, int y, int w, int h, uint16_t px)
{
    send_header('f', x, y, w, h, 2);
    write_all(&px, 2);
}

uint64_t hal_keys(void)
{
    struct pollfd pfd = { .fd = keys_fd, .events = POLLIN };

    while(keys_fd >= 0 && poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLIN)) {
        uint8_t rpt[5];
        if(read(keys_fd, rpt, sizeof(rpt)) != sizeof(rpt)) break;

        // 0xfe is the simulator's USB plug toggle, not a key
        if(memchr(rpt, 0xfe, sizeof(rpt))) continue;

        keys_held = 0;
        for(int i = 0; i < 5; i++) {
            if(rpt[i] < 64) keys_held |= 1ULL << rpt[i];
        }
    }

    return keys_held;
}

uint64_t hal_micros(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

void hal_wait_until(uint64_t when_us)
{
    uint64_t now = hal_micros();

    if(when_us > now) {
        uint64_t dt = when_us - now;
        struct timespec ts = { dt / 1000000, (dt % 1000000) * 1000 };
        nanosleep(&ts, NULL);
    }
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
