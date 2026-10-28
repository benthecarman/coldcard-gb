//
// gbtest.c - headless runner for the COLDCARD Q Game Boy core.
//
// Builds the same gb_core.c / gb_video.c as the firmware, with hal_test.c
// standing in for the hardware. Commands:
//
//   gbtest [-s SLOTS] serial ROM MAX_FRAMES
//       Run until the serial port shows "Passed" or "Failed" (Blargg's
//       test ROMs). Exit status 0 only on "Passed".
//
//   gbtest [-s SLOTS] shot ROM FRAMES MODE OUT.png [grey|green]
//       Run FRAMES frames and save the 320x240 LCD as a PNG.
//       MODE: 1x, 1.5x, 1.5x-sharp, 2x, oled-top, oled-middle, oled-bottom,
//       oled-fit, oled-zoom
//
//   gbtest frame IN.pgm MODE OUT.png [PALETTE]
//       Show a 160x144 greyscale PGM (white is shade 0) in an OLED mode,
//       without running a ROM.
//
//   gbtest oled
//       Checks of the OLED renderer on made-up pictures.
//
//   gbtest view ROM 0
//       Checks of the view key, running ROM.
//
//   gbtest [-s SLOTS] bench ROM FRAMES
//       Run FRAMES frames as fast as possible; print host speed and
//       bank cache stats.
//
//   gbtest [-s SLOTS] state ROM FRAMES
//       Run FRAMES frames, save state, load it into a second emulator,
//       then run both on and check they stay identical.
//
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

#include "gb_core.h"
#include "hal_test.h"

static const uint32_t PAL_GREY[4] = { 0xffffff, 0xaaaaaa, 0x555555, 0x000000 };
static const uint32_t PAL_GREEN[4] = { 0xe0f8d0, 0x88c070, 0x346856, 0x081820 };
static const uint32_t PAL_MONO[4] = { 0xffffff, 0xffffff, 0x000000, 0x000000 };
static const uint32_t PAL_INVERSE[4] = { 0x000000, 0x555555, 0xaaaaaa, 0xffffff };

static const uint32_t *parse_palette(const char *s)
{
    if(!strcmp(s, "grey")) return PAL_GREY;
    if(!strcmp(s, "green")) return PAL_GREEN;
    if(!strcmp(s, "mono")) return PAL_MONO;
    if(!strcmp(s, "inverse")) return PAL_INVERSE;
    test_fail("unknown palette %s", s);
}

void test_fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FAIL: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

static ccgb_t *start(const uint8_t *rom, uint32_t padded, int n_slots);

static ccgb_t *load(const char *path, int n_slots)
{
    FILE *f = fopen(path, "rb");
    if(!f) test_fail("can't open %s", path);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    // ROM area is whole 16K banks, padded with 0xff
    uint32_t padded = (size + GB_BANK_SIZE - 1) & ~(GB_BANK_SIZE - 1);
    uint8_t *rom = hal_rom_alloc(padded);
    uint8_t *tmp = malloc(padded);
    memset(tmp, 0xff, padded);
    if(fread(tmp, 1, size, f) != (size_t)size) test_fail("short read");
    fclose(f);
    hal_rom_write(rom, tmp, padded);
    free(tmp);

    return start(rom, padded, n_slots);
}

// Emulator for a ROM image already in place
static ccgb_t *start(const uint8_t *rom, uint32_t padded, int n_slots)
{
    ccgb_t *c = calloc(1, sizeof(ccgb_t));
    uint8_t *bank0 = malloc(GB_BANK_SIZE);
    uint8_t *slots[GB_MAX_SLOTS];
    for(int i = 0; i < n_slots; i++) slots[i] = malloc(GB_BANK_SIZE);

    int rv = ccgb_init(c, rom, padded, bank0, slots, n_slots);
    if(rv) test_fail("ccgb_init: %d", rv);

    uint32_t ram = ccgb_cart_ram_size(c);
    if(ram) {
        uint8_t *mem = malloc(ram);
        memset(mem, 0xff, ram);
        ccgb_set_cart_ram(c, mem, ram);
    }

    // menu = Q's CANCEL; no keys are pressed in these tests
    uint8_t km[GB_KEYMAP_BYTES];
    memset(km, GB_NO_KEY, sizeof(km));
    km[offsetof(gb_keymap_t, menu)] = 7;
    ccgb_set_keymap(c, km);

    return c;
}

static int parse_mode(const char *s)
{
    if(!strcmp(s, "1x")) return VIDEO_1X;
    if(!strcmp(s, "1.5x")) return VIDEO_15_SMOOTH;
    if(!strcmp(s, "1.5x-sharp")) return VIDEO_15_SHARP;
    if(!strcmp(s, "2x")) return VIDEO_2X_CROP;
    if(!strcmp(s, "oled-top")) return VIDEO_OLED_TOP;
    if(!strcmp(s, "oled-middle")) return VIDEO_OLED_MIDDLE;
    if(!strcmp(s, "oled-bottom")) return VIDEO_OLED_BOTTOM;
    if(!strcmp(s, "oled-fit")) return VIDEO_OLED_FIT;
    if(!strcmp(s, "oled-zoom")) return VIDEO_OLED_ZOOM;
    test_fail("unknown mode %s", s);
}

// Run frames, stopping only for errors; saves just get acknowledged.
static void run_frames(ccgb_t *c, uint32_t frames)
{
    uint32_t start = c->stats.frames;
    bool redraw = true;

    while(c->stats.frames - start < frames) {
        int rv = ccgb_run(c, frames - (c->stats.frames - start), redraw);
        redraw = false;
        if(rv == GB_RUN_ERROR) {
            test_fail("emulation error %d at 0x%04x", c->error - 1, c->error_addr);
        }
        if(rv == GB_RUN_SAVE) ccgb_saved(c);
    }
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static void png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
    uint8_t hdr[8];
    put32(hdr, len);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if(len) fwrite(data, 1, len, f);

    uint32_t crc = crc32(0, hdr + 4, 4);
    if(len) crc = crc32(crc, data, len);   // crc32(crc, NULL, 0) returns 0
    uint8_t tail[4];
    put32(tail, crc);
    fwrite(tail, 1, 4, f);
}

static void save_png(const char *path)
{
    const int w = HAL_LCD_W, h = HAL_LCD_H;
    uLong raw_len = h * (1 + w * 3);
    uint8_t *raw = malloc(raw_len), *p = raw;

    for(int y = 0; y < h; y++) {
        *p++ = 0;       // filter: none
        for(int x = 0; x < w; x++) {
            uint16_t px = test_screen[y][x];
            uint8_t r = (px >> 11) & 0x1f, g = (px >> 5) & 0x3f, b = px & 0x1f;
            *p++ = (r << 3) | (r >> 2);
            *p++ = (g << 2) | (g >> 4);
            *p++ = (b << 3) | (b >> 2);
        }
    }

    uLong zlen = compressBound(raw_len);
    uint8_t *z = malloc(zlen);
    if(compress(z, &zlen, raw, raw_len) != Z_OK) test_fail("compress");

    FILE *f = fopen(path, "wb");
    if(!f) test_fail("can't write %s", path);
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ihdr[13];
    put32(ihdr, w);
    put32(ihdr + 4, h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;   // 8-bit RGB
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", z, zlen);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);

    free(raw);
    free(z);
}

// OLED renderer checks on made-up frame buffers
static int oled_lit(const uint8_t *buf, int x, int y)
{
    return (buf[(y / 8) * OLED_W + x] >> (y & 7)) & 1;
}

static int oled_count(const uint8_t *buf)
{
    int n = 0;
    for(int y = 0; y < OLED_H; y++) {
        for(int x = 0; x < OLED_W; x++) n += oled_lit(buf, x, y);
    }
    return n;
}

static void fb_fill(uint8_t fb[GB_H][GB_LINE_BYTES], int shade)
{
    memset(fb, shade * 0x55, GB_H * GB_LINE_BYTES);
}

static void fb_set(uint8_t fb[GB_H][GB_LINE_BYTES], int x, int y, int shade)
{
    uint8_t *b = &fb[y][x / 4];
    int sh = (x & 3) * 2;
    *b = (*b & ~(3 << sh)) | (shade << sh);
}

static int oled_checks(void)
{
    static uint8_t fb[GB_H][GB_LINE_BYTES];
    static uint8_t out[OLED_BYTES];
    gb_oled_t o;
    const int all = OLED_W * OLED_H;

    // grey ramp: shades light up 4, 3, 1 and 0 of every 4 pixels
    gb_oled_setup(&o, VIDEO_OLED_MIDDLE, PAL_GREY);
    const int want[4] = { all, all * 3 / 4, all / 4, 0 };
    for(int s = 0; s < 4; s++) {
        fb_fill(fb, s);
        gb_oled_render(&o, (const uint8_t (*)[GB_LINE_BYTES])fb, out);
        if(oled_count(out) != want[s]) test_fail("shade %d: %d lit", s, oled_count(out));
    }

    // two-tone and inverse
    gb_oled_setup(&o, VIDEO_OLED_TOP, PAL_MONO);
    fb_fill(fb, 1);
    gb_oled_render(&o, (const uint8_t (*)[GB_LINE_BYTES])fb, out);
    if(oled_count(out) != all) test_fail("mono: light grey not lit");
    gb_oled_setup(&o, VIDEO_OLED_TOP, PAL_INVERSE);
    fb_fill(fb, 3);
    gb_oled_render(&o, (const uint8_t (*)[GB_LINE_BYTES])fb, out);
    if(oled_count(out) != all) test_fail("inverse: black not lit");

    // the windowed views keep a one-pixel dark line, in either direction,
    // on every GB column and line they show
    const int modes[4] = { VIDEO_OLED_TOP, VIDEO_OLED_MIDDLE, VIDEO_OLED_BOTTOM, VIDEO_OLED_ZOOM };
    for(int m = 0; m < 4; m++) {
        gb_oled_setup(&o, modes[m], PAL_MONO);
        int cols = (modes[m] == VIDEO_OLED_ZOOM) ? OLED_W : GB_W;
        for(int gx = o.pan_x; gx < o.pan_x + cols; gx++) {
            fb_fill(fb, 0);
            for(int y = 0; y < GB_H; y++) fb_set(fb, gx, y, 3);
            gb_oled_render(&o, (const uint8_t (*)[GB_LINE_BYTES])fb, out);
            if(oled_count(out) != all - OLED_H) test_fail("mode %d: column %d", modes[m], gx);
        }
        for(int gy = o.first; gy < o.first + o.lines; gy++) {
            fb_fill(fb, 0);
            for(int x = 0; x < GB_W; x++) fb_set(fb, x, gy, 3);
            gb_oled_render(&o, (const uint8_t (*)[GB_LINE_BYTES])fb, out);
            if(oled_count(out) != all - OLED_W) test_fail("mode %d: line %d", modes[m], gy);
        }
    }

    // the scaled views cover 80 lines: bottom is lines 64-143
    gb_oled_setup(&o, VIDEO_OLED_BOTTOM, PAL_MONO);
    fb_fill(fb, 0);
    fb_set(fb, 0, 64, 3);
    fb_set(fb, 159, 143, 3);
    gb_oled_render(&o, (const uint8_t (*)[GB_LINE_BYTES])fb, out);
    if(oled_lit(out, 0, 0) || oled_lit(out, 127, 63) || oled_count(out) != all - 2) {
        test_fail("bottom view corners");
    }

    // moving the middle view down as far as it goes gives the bottom view
    gb_oled_setup(&o, VIDEO_OLED_MIDDLE, PAL_MONO);
    if(!gb_oled_pan(&o, 0, 1000) || o.first != 64 || gb_oled_pan(&o, 0, 1)) {
        test_fail("middle view pan: first %d", o.first);
    }
    if(gb_oled_pan(&o, 1, 0)) test_fail("scaled views move sideways");

    // zoom is 1:1 and moves both ways, within the screen
    gb_oled_setup(&o, VIDEO_OLED_ZOOM, PAL_MONO);
    gb_oled_pan(&o, -1000, -1000);
    fb_fill(fb, 0);
    fb_set(fb, 0, 0, 3);
    fb_set(fb, 127, 63, 3);
    fb_set(fb, 128, 64, 3);
    gb_oled_render(&o, (const uint8_t (*)[GB_LINE_BYTES])fb, out);
    if(oled_lit(out, 0, 0) || oled_lit(out, 127, 63) || oled_count(out) != all - 2) {
        test_fail("zoom corners");
    }
    gb_oled_pan(&o, 1000, 1000);
    if(o.pan_x != GB_W - OLED_W || o.pan_y != GB_H - OLED_H) test_fail("zoom pan limits");

    // whole: 71 columns in the middle; the sides stay dark
    gb_oled_setup(&o, VIDEO_OLED_FIT, PAL_GREY);
    const int fit = o.dst_w * OLED_H;
    fb_fill(fb, 0);
    gb_oled_render(&o, (const uint8_t (*)[GB_LINE_BYTES])fb, out);
    if(o.dst_w != 71 || oled_count(out) != fit || oled_lit(out, o.dst_x - 1, 0)
       || oled_lit(out, o.dst_x + o.dst_w, 0)) {
        test_fail("whole view: %d columns, %d lit", o.dst_w, oled_count(out));
    }

    // whole: shades get darker in order, and a one-pixel dark line
    // anywhere darkens the picture
    int last = fit + 1;
    for(int s = 0; s < 4; s++) {
        fb_fill(fb, s);
        gb_oled_render(&o, (const uint8_t (*)[GB_LINE_BYTES])fb, out);
        if(oled_count(out) >= last) test_fail("whole view: shade %d not darker", s);
        last = oled_count(out);
    }
    if(last != 0) test_fail("whole view: black lit");
    for(int gx = 0; gx < GB_W; gx++) {
        fb_fill(fb, 0);
        for(int y = 0; y < GB_H; y++) fb_set(fb, gx, y, 3);
        gb_oled_render(&o, (const uint8_t (*)[GB_LINE_BYTES])fb, out);
        if(oled_count(out) >= fit) test_fail("whole view: column %d lost", gx);
    }
    for(int gy = 0; gy < GB_H; gy++) {
        fb_fill(fb, 0);
        for(int x = 0; x < GB_W; x++) fb_set(fb, x, gy, 3);
        gb_oled_render(&o, (const uint8_t (*)[GB_LINE_BYTES])fb, out);
        if(oled_count(out) >= fit) test_fail("whole view: line %d lost", gy);
    }

    printf("oled checks passed\n");
    return 0;
}

// A 160x144 PGM (P5, maxval 255) to the OLED, as a 320x240 PNG
static int show_frame(const char *in, int mode, const char *out_png, const uint32_t *pal)
{
    static uint8_t fb[GB_H][GB_LINE_BYTES];
    static uint8_t out[OLED_BYTES];
    int w, h, maxval;

    FILE *f = fopen(in, "rb");
    if(!f) test_fail("can't read %s", in);
    if(fscanf(f, "P5 %d %d %d", &w, &h, &maxval) != 3 || w != GB_W || h != GB_H
       || maxval != 255 || fgetc(f) == EOF) {
        test_fail("%s: want a 160x144 P5 PGM", in);
    }
    for(int y = 0; y < GB_H; y++) {
        for(int x = 0; x < GB_W; x++) {
            int v = fgetc(f);
            if(v == EOF) test_fail("%s: cut short", in);
            fb_set(fb, x, y, 3 - v * 4 / 256);
        }
    }
    fclose(f);

    gb_oled_t o;
    gb_oled_setup(&o, mode, pal);
    gb_oled_render(&o, (const uint8_t (*)[GB_LINE_BYTES])fb, out);
    hal_oled_write(out);
    save_png(out_png);
    return 0;
}

// One ccgb_run() with keys changing each frame: keys[0] is what is held
// when it starts
static void play_keys(ccgb_t *c, const uint64_t *keys, int n)
{
    test_key_seq = keys;
    test_key_len = n;
    run_frames(c, n - 1);
    test_key_seq = NULL;
}

// View key: a tap steps to the next view, held with the D-pad it moves
// the view, and a ghost of it (pressed with other keys) does nothing.
static int view_checks(ccgb_t *c)
{
    enum { K_A = 20, K_UP, K_DOWN, K_VIEW };
    uint8_t km[GB_KEYMAP_BYTES];
    memset(km, GB_NO_KEY, sizeof(km));
    km[offsetof(gb_keymap_t, btn[GB_BTN_A][0])] = K_A;
    km[offsetof(gb_keymap_t, btn[GB_BTN_UP][0])] = K_UP;
    km[offsetof(gb_keymap_t, btn[GB_BTN_DOWN][0])] = K_DOWN;
    km[offsetof(gb_keymap_t, menu)] = 7;
    km[offsetof(gb_keymap_t, view)] = K_VIEW;
    ccgb_set_keymap(c, km);
    ccgb_set_video(c, VIDEO_OLED_MIDDLE, PAL_MONO, 0);
    run_frames(c, 2);

    const uint64_t view = 1ULL << K_VIEW, down = 1ULL << K_DOWN, a = 1ULL << K_A;

    // tap; while the view key is held the game sees no keys
    const uint64_t tap[] = { 0, view, view, view };
    play_keys(c, tap, 4);
    if(c->gb.direct.joypad != 0xff) test_fail("view key held: game sees keys");
    ccgb_set_video(c, VIDEO_OLED_MIDDLE, PAL_MONO, 0);
    const uint64_t tap_up[] = { 0, view, view, 0 };
    play_keys(c, tap_up, 4);
    if(ccgb_video_mode(c) != VIDEO_OLED_BOTTOM) test_fail("tap: mode %d", ccgb_video_mode(c));

    // hold and move: no view change on letting go, and Down held after
    // it stays away from the game
    ccgb_set_video(c, VIDEO_OLED_MIDDLE, PAL_MONO, 0);
    const uint64_t pan[] = { 0, view, view | down, view | down, view | down, view | down,
                             view | down, down, down };
    play_keys(c, pan, 9);
    if(c->oled.pan_y != 32 + 5) test_fail("pan: first line %d", c->oled.pan_y);
    if(ccgb_video_mode(c) != VIDEO_OLED_MIDDLE) test_fail("pan then let go: mode changed");
    if(c->gb.direct.joypad != 0xff) test_fail("pan then let go: Down leaked to the game");

    // pressed together with other keys (as a matrix ghost is): ignored
    ccgb_set_video(c, VIDEO_OLED_MIDDLE, PAL_MONO, 0);
    const uint64_t ghost[] = { 0, view | a | down, view | a | down };
    play_keys(c, ghost, 3);
    if(c->gb.direct.joypad == 0xff) test_fail("ghost view key: game sees no keys");
    const uint64_t off[] = { view | a | down, 0, 0 };
    play_keys(c, off, 3);
    if(ccgb_video_mode(c) != VIDEO_OLED_MIDDLE || c->oled.pan_y != 32) {
        test_fail("ghost view key: view changed");
    }

    printf("view key checks passed\n");
    return 0;
}

static void usage(void)
{
    fprintf(stderr, "usage: gbtest [-s SLOTS] serial ROM MAX_FRAMES\n"
                    "       gbtest [-s SLOTS] shot ROM FRAMES MODE OUT.png [grey|green|mono|inverse]\n"
                    "       gbtest frame IN.pgm MODE OUT.png [PALETTE]\n"
                    "       gbtest oled\n"
                    "       gbtest view ROM 0\n"
                    "       gbtest [-s SLOTS] bench ROM FRAMES\n"
                    "       gbtest [-s SLOTS] state ROM FRAMES\n");
    exit(2);
}

int main(int argc, char **argv)
{
    int n_slots = 6;

    if(argc > 2 && !strcmp(argv[1], "-s")) {
        n_slots = atoi(argv[2]);
        argv += 2;
        argc -= 2;
    }
    if(argc == 2 && !strcmp(argv[1], "oled")) {
        return oled_checks();
    }
    if(argc >= 5 && !strcmp(argv[1], "frame")) {
        return show_frame(argv[2], parse_mode(argv[3]), argv[4],
                          (argc > 5) ? parse_palette(argv[5]) : PAL_GREY);
    }
    if(argc < 4) usage();

    const char *cmd = argv[1];
    ccgb_t *c = load(argv[2], n_slots);
    uint32_t frames = strtoul(argv[3], NULL, 0);

    if(!strcmp(cmd, "serial")) {
        ccgb_set_video(c, VIDEO_1X, PAL_GREY, 0);
        for(uint32_t f = 0; f < frames; f += 60) {
            run_frames(c, 60);
            if(strstr(c->serial, "Passed") || strstr(c->serial, "Failed")) break;
        }
        printf("%s\n", c->serial);
        printf("frames: %u\n", c->stats.frames);
        return strstr(c->serial, "Passed") && !strstr(c->serial, "Failed") ? 0 : 1;
    }

    if(!strcmp(cmd, "view")) {
        return view_checks(c);
    }

    if(!strcmp(cmd, "shot")) {
        if(argc < 6) usage();
        const uint32_t *pal = (argc > 6) ? parse_palette(argv[6]) : PAL_GREY;
        ccgb_set_video(c, parse_mode(argv[4]), pal, 0x202020);
        run_frames(c, frames);
        save_png(argv[5]);
        printf("frames: %u shown: %u groups sent: %u\n",
               c->stats.frames, c->stats.shown, c->stats.lines_sent);
        return 0;
    }

    if(!strcmp(cmd, "bench")) {
        ccgb_set_video(c, VIDEO_15_SMOOTH, PAL_GREEN, 0x202020);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        run_frames(c, frames);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        printf("%u frames in %.3f s: %.0f fps on this host\n", frames, secs, frames / secs);
        printf("groups sent: %u, cache hits: %u, misses: %u\n",
               c->stats.lines_sent, c->stats.cache_hits, c->stats.cache_misses);
        return 0;
    }

    if(!strcmp(cmd, "state")) {
        const int after = 600;

        ccgb_set_video(c, VIDEO_1X, PAL_GREY, 0);
        run_frames(c, frames);

        // what a state file holds: header, gb_s, cart RAM
        uint8_t hdr[GB_STATE_HDR];
        ccgb_state_header(c, hdr);
        uint8_t *st = malloc(sizeof(struct gb_s));
        memcpy(st, &c->gb, sizeof(struct gb_s));

        ccgb_t *d = start(c->rom, c->rom_size, n_slots);
        uint8_t hdr2[GB_STATE_HDR];
        ccgb_state_header(d, hdr2);
        if(memcmp(hdr, hdr2, sizeof(hdr))) test_fail("state headers differ");

        if(c->cart_ram_size) memcpy(d->cart_ram, c->cart_ram, c->cart_ram_size);
        ccgb_state_load(d, st);
        ccgb_set_video(d, VIDEO_1X, PAL_GREY, 0);

        for(int i = 0; i < after; i++) {
            run_frames(c, 1);
            run_frames(d, 1);
            if(memcmp(c->fb, d->fb, sizeof(c->fb))) test_fail("frame %d differs", i);
        }
        if(memcmp(c->gb.wram, d->gb.wram, sizeof(c->gb.wram))
           || memcmp(&c->gb.cpu_reg, &d->gb.cpu_reg, sizeof(c->gb.cpu_reg))) {
            test_fail("state differs after %d frames", after);
        }
        if(c->cart_ram_size && memcmp(c->cart_ram, d->cart_ram, c->cart_ram_size)) {
            test_fail("cart RAM differs");
        }

        printf("state: %zu + %u bytes; restored run matches for %d frames\n",
               sizeof(struct gb_s), c->cart_ram_size, after);
        return 0;
    }

    usage();
}
