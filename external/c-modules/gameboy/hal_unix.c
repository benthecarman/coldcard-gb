//
// hal_unix.c - gb_hal.h for the desktop simulator (unix/simulator.py).
//
// Q: pixels go down the simulator's display pipe as 'r' (raw RGB565) and
// 'f' (fill) messages, same as unix/variant/st7788.py. Key state comes from
// the simulator's key pipe: 5-byte reports of held Q key numbers, 255 = none.
//
// Mk4: OLED frames go down the display pipe as 1024 bytes, as from
// unix/variant/ssd1306.py. The key pipe carries a key's character when it
// goes down and a NUL when all keys are up (unix/variant/touch.py), so a
// key released while another is held stays down until both are up.
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

// Mk4 key numbers are positions in this string (shared/mempad.py)
static const char MK4_KEYS[] = "y0x987654321";

static int display_fd = -1, keys_fd = -1;
static bool is_mk4;
static int win_x, win_y, win_w, win_h, win_row;
static uint64_t keys_held;
static uint8_t *rom_mem;

// _gameboy.sim_setup(display_fd, keys_fd, mk4=False)
STATIC mp_obj_t gb_sim_setup(size_t n_args, const mp_obj_t *args)
{
    display_fd = mp_obj_get_int(args[0]);
    keys_fd = mp_obj_get_int(args[1]);
    is_mk4 = (n_args > 2) && mp_obj_is_true(args[2]);

    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gb_sim_setup_obj, 2, 3, gb_sim_setup);

// _gameboy.sim_keys(keynums): set which keys are held. The firmware's own
// key reader (unix/variant/touch.py) reads the same pipe while the game
// is paused, so the reports it took are passed over this way.
STATIC mp_obj_t gb_sim_keys(mp_obj_t keys_in)
{
    mp_buffer_info_t buf;
    mp_get_buffer_raise(keys_in, &buf, MP_BUFFER_READ);

    keys_held = 0;
    for(size_t i = 0; i < buf.len; i++) {
        uint8_t kn = ((const uint8_t *)buf.buf)[i];
        if(kn < 64) keys_held |= 1ULL << kn;
    }

    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(gb_sim_keys_obj, gb_sim_keys);

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

void hal_oled_write(const uint8_t *buf)
{
    // one write: the simulator keeps only the last whole frame it reads
    write_all(buf, 1024);
}

static void mk4_keys(void)
{
    struct pollfd pfd = { .fd = keys_fd, .events = POLLIN };

    while(keys_fd >= 0 && poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLIN)) {
        char ch;
        if(read(keys_fd, &ch, 1) != 1) break;

        if(ch == 0) {
            keys_held = 0;
        } else {
            const char *p = strchr(MK4_KEYS, ch);
            if(p) keys_held |= 1ULL << (p - MK4_KEYS);
        }
    }
}

uint64_t hal_keys(void)
{
    struct pollfd pfd = { .fd = keys_fd, .events = POLLIN };

    if(is_mk4) {
        mk4_keys();
        return keys_held;
    }

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
