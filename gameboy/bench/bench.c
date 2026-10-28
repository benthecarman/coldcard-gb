//
// bench.c - run the Game Boy core bare-metal on QEMU's Cortex-M4 board
// (mps2-an386) and count instructions per frame.
//
// QEMU with `-icount shift=0` advances its virtual clock 1 ns per guest
// instruction, and SysTick runs from that clock, so time measured here is
// an instruction count. It is not a cycle count: on the Q (120 MHz, flash
// with 5 wait states behind the ART cache) each instruction takes about
// 1.3-2 cycles. See gameboy/README.md for how to read the numbers.
//
// The LCD is not modelled; bytes sent to it are counted instead.
//
#include <stdint.h>
#include <string.h>

#include "gb_core.h"
#include "gb_hal.h"

#ifndef BENCH_FRAMES
# define BENCH_FRAMES   1800
#endif

extern const uint8_t rom_image[], rom_image_end[];

// ---- startup

extern uint32_t _etext, _sdata, _edata, _sbss, _ebss, _estack;
void reset(void);
static void fault(void) { for(;;) { } }

#ifdef PROFILE
void systick_isr(void);
#else
# define systick_isr fault
#endif

__attribute__((section(".vectors"), used))
static const void *const vectors[16] = {
    &_estack, reset, fault, fault, fault, fault, fault, 0, 0, 0, 0, fault, 0, 0, fault, systick_isr,
};

int main(void);

void reset(void)
{
    memcpy(&_sdata, &_etext, (uint8_t *)&_edata - (uint8_t *)&_sdata);
    memset(&_sbss, 0, (uint8_t *)&_ebss - (uint8_t *)&_sbss);

    // FPU on (the firmware's hard-float ABI may use its registers)
    *(volatile uint32_t *)0xE000ED88 |= 0xF << 20;

    main();
    fault();
}

// ---- semihosting console

static int semihost(int op, const void *arg)
{
    register int r0 __asm__("r0") = op;
    register const void *r1 __asm__("r1") = arg;
    __asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}

static void out(const char *s) { semihost(0x04, s); }

static void out_u(uint64_t v)
{
    char buf[24], *p = buf + sizeof(buf) - 1;
    *p = 0;
    do { *--p = '0' + v % 10; v /= 10; } while(v);
    out(p);
}

static void out_kv(const char *k, uint64_t v) { out(k); out_u(v); }

static void quit(void)
{
    semihost(0x18, (const void *)0x20026);      // ADP_Stopped_ApplicationExit
}

// ---- clock: SysTick counting down from the virtual clock

#define SYST_CSR    (*(volatile uint32_t *)0xE000E010)
#define SYST_RVR    (*(volatile uint32_t *)0xE000E014)
#define SYST_CVR    (*(volatile uint32_t *)0xE000E018)

// mps2-an386 SysTick: 25 MHz processor clock, so 40 ns (instructions) per tick
#define NS_PER_TICK 40

#ifndef PROFILE

static uint64_t ticks;
static uint32_t last_cvr;

static void clock_init(void)
{
    SYST_RVR = 0xffffff;
    SYST_CVR = 0;
    SYST_CSR = 5;           // enable, processor clock, no interrupt
    last_cvr = SYST_CVR;
}

// virtual nanoseconds = instructions run
static uint64_t insns(void)
{
    uint32_t v = SYST_CVR;
    ticks += (last_cvr - v) & 0xffffff;
    last_cvr = v;
    return ticks * NS_PER_TICK;
}

#else

// Sampling profiler: SysTick interrupts every PROFILE_TICKS ticks and
// records the interrupted PC, in 16-byte buckets of the first 64K of code.
#define PROFILE_TICKS   1000
static volatile uint32_t wraps;
static uint32_t samples[0x10000 / 16];

static void clock_init(void)
{
    SYST_RVR = PROFILE_TICKS - 1;
    SYST_CVR = 0;
    SYST_CSR = 7;           // enable, processor clock, interrupt
}

static uint64_t insns(void)
{
    uint32_t w, v;
    do {
        w = wraps;
        v = SYST_CVR;
    } while(w != wraps);
    return ((uint64_t)w * PROFILE_TICKS + (PROFILE_TICKS - 1 - v)) * NS_PER_TICK;
}

void systick_c(const uint32_t *frame)
{
    uint32_t pc = frame[6];
    wraps++;
    if(pc < 0x10000) samples[pc / 16]++;
}

__attribute__((naked)) void systick_isr(void)
{
    __asm__ volatile("mrs r0, msp\n b systick_c\n");
}

static void dump_profile(void)
{
    for(uint32_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        if(samples[i]) {
            out_kv("pc=", i * 16);
            out_kv(" n=", samples[i]);
            out("\n");
        }
    }
}

#endif

// ---- HAL

const bool hal_lcd_big_endian = true;

static uint64_t lcd_bytes, lcd_windows;
static uint32_t frame_no;

void hal_lcd_begin(void) { }
void hal_lcd_end(void) { }
void hal_lcd_sync(void) { }
void hal_lcd_window(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; lcd_windows++; }
void hal_lcd_write(const uint16_t *px, int count) { (void)px; lcd_bytes += count * 2; }
void hal_lcd_fill(int x, int y, int w, int h, uint16_t px) { (void)x; (void)y; (void)px; lcd_bytes += w * h * 2; }
void hal_oled_write(const uint8_t *buf) { (void)buf; lcd_bytes += 1024; }

// press Start now and then to get past title screens; hold Right in between
uint64_t hal_keys(void)
{
    uint32_t f = frame_no++;

    if(f % 300 >= 280 && f % 300 < 290) return 1ULL << 8;      // ENTER = Start
    if(f % 300 < 200) return 1ULL << 6;                         // RIGHT
    return 0;
}

// "microseconds" here are thousands of instructions
uint64_t hal_micros(void) { return insns() / 1000; }
void hal_wait_until(uint64_t when_us) { (void)when_us; }

uint8_t *hal_rom_alloc(uint32_t size) { (void)size; return NULL; }
void hal_rom_write(uint8_t *dst, const uint8_t *src, size_t len) { memcpy(dst, src, len); }
void hal_rom_free(void) { }

// ---- benchmark

static ccgb_t core;
static uint8_t bank0[GB_BANK_SIZE];
static uint8_t slot_mem[6][GB_BANK_SIZE];
static uint8_t cart_ram[0x8000];

static const uint32_t PAL[4] = { 0xe0f8d0, 0x88c070, 0x346856, 0x081820 };

static const struct { int mode; const char *name; } MODES[] = {
    { VIDEO_15_SMOOTH, "1.5x-smooth" },
    { VIDEO_1X, "1x" },
    { VIDEO_2X_CROP, "2x-crop" },
    { VIDEO_OLED_FILL, "oled-fill" },
    { VIDEO_OLED_FIT, "oled-fit" },
};

static void bench_mode(int mode, const char *name)
{
    ccgb_t *c = &core;
    uint8_t *slots[6];
    for(int i = 0; i < 6; i++) slots[i] = slot_mem[i];

    memset(c, 0, sizeof(*c));
    uint32_t size = rom_image_end - rom_image;
    if(ccgb_init(c, rom_image, size, bank0, slots, 6)) {
        out("ccgb_init failed\n");
        return;
    }
    uint32_t ram = ccgb_cart_ram_size(c);
    if(ram > sizeof(cart_ram)) ram = sizeof(cart_ram);
    memset(cart_ram, 0xff, sizeof(cart_ram));
    ccgb_set_cart_ram(c, ram ? cart_ram : NULL, ram);

    // default Q keymap from shared/gameboy.py
    static const uint8_t km[GB_KEYMAP_BYTES] = {
        37, 0xff, 36, 0xff, 52, 0xff, 8, 0xff,
        32, 6, 30, 3, 21, 4, 31, 5, 7, 28, 0xff,
    };
    ccgb_set_keymap(c, km);
    ccgb_set_video(c, mode, PAL, 0);

    frame_no = 0;
    lcd_bytes = lcd_windows = 0;

    // per-frame worst cases, after the first second (boot logos)
    uint64_t worst_emu = 0, worst_lcd = 0, worst_bytes = 0;
    uint64_t sum_emu = 0, sum_lcd = 0, sum_bytes = 0;
    uint32_t counted = 0, misses0 = 0;

    for(uint32_t f = 0; f < BENCH_FRAMES; f++) {
        uint64_t e0 = c->stats.emu_us, l0 = c->stats.lcd_us, b0 = lcd_bytes;
        if(f == 60) misses0 = c->stats.cache_misses;

        int rv = ccgb_run(c, 1, f == 0);
        if(rv == GB_RUN_SAVE) ccgb_saved(c);
        if(rv == GB_RUN_ERROR) {
            out("emulation error\n");
            return;
        }

        if(f < 60) continue;
        uint64_t e = c->stats.emu_us - e0, l = c->stats.lcd_us - l0, b = lcd_bytes - b0;
        sum_emu += e; sum_lcd += l; sum_bytes += b; counted++;
        if(e > worst_emu) worst_emu = e;
        if(l > worst_lcd) worst_lcd = l;
        if(b > worst_bytes) worst_bytes = b;
    }

    // one line per mode
    out("mode="); out(name);
    out_kv(" frames=", counted);
    out_kv(" emu_k_avg=", sum_emu / counted);
    out_kv(" emu_k_max=", worst_emu);
    out_kv(" lcd_k_avg=", sum_lcd / counted);
    out_kv(" lcd_k_max=", worst_lcd);
    out_kv(" spi_bytes_avg=", sum_bytes / counted);
    out_kv(" spi_bytes_max=", worst_bytes);
    out_kv(" cache_misses=", c->stats.cache_misses - misses0);
    out_kv(" cache_lookups=", c->stats.cache_hits + c->stats.cache_misses);
    out("\n");
}

int main(void)
{
    clock_init();

    // calibration: a loop of known length (2 instructions per pass)
    uint64_t t0 = insns();
    for(volatile uint32_t n = 0; n < 1; n++) { }
    __asm__ volatile(
        "mov r0, #0x100000\n"
        "1: subs r0, r0, #1\n"
        "bne 1b\n" ::: "r0", "cc");
    out_kv("calibrate: 2097152 insns measured as ", insns() - t0);
    out("\n");

#ifdef PROFILE
#ifndef PROFILE_MODE
#define PROFILE_MODE 0      // index into MODES
#endif
    bench_mode(MODES[PROFILE_MODE].mode, MODES[PROFILE_MODE].name);
    dump_profile();
#else
    for(unsigned i = 0; i < sizeof(MODES) / sizeof(MODES[0]); i++) {
        bench_mode(MODES[i].mode, MODES[i].name);
    }
#endif

    quit();
    return 0;
}
