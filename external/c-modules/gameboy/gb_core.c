//
// gb_core.c - Game Boy emulator core for COLDCARD Q, built on Peanut-GB.
//
#include <string.h>

#define GB_CORE_IMPL
#include "gb_core.h"
#include "gb_hal.h"

// One Game Boy frame: 70224 clocks at 4.194304 MHz
#define FRAME_NS            16742706ULL

// Give up catching up if this far behind, and just carry on from now.
#define MAX_BEHIND_NS       (100 * 1000 * 1000ULL)

// When emulation is behind, still show at least every Nth frame.
#define MAX_UNSHOWN         3

// Output groups per hal_lcd_write()
#define GROUPS_PER_WRITE    4

// When to write cart RAM out, in frames since the last write to it:
// soon after the game disables RAM (games do that after saving), later
// if it stays enabled, and anyway once it has been dirty for a while.
#define SAVE_AFTER_DISABLE  30
#define SAVE_AFTER_IDLE     120
#define SAVE_AT_MOST        600

// cur_bank when no bank is selected yet
#define NO_BANK             0xffff

static uint8_t *bank_mem(ccgb_t *c, uint16_t bank)
{
    if(!c->n_slots) {
        // no cache: read the image directly
        return (uint8_t *)c->rom + (uint32_t)bank * GB_BANK_SIZE;
    }

    c->use_clock++;

    int oldest = 0;
    for(int i = 0; i < c->n_slots; i++) {
        if(c->slot_bank[i] == bank) {
            c->slot_used[i] = c->use_clock;
            c->stats.cache_hits++;
            return c->slot_mem[i];
        }
        if(c->slot_used[i] < c->slot_used[oldest]) oldest = i;
    }

    c->stats.cache_misses++;
    memcpy(c->slot_mem[oldest], c->rom + (uint32_t)bank * GB_BANK_SIZE, GB_BANK_SIZE);
    c->slot_bank[oldest] = bank;
    c->slot_used[oldest] = c->use_clock;

    return c->slot_mem[oldest];
}

static uint8_t rom_read(struct gb_s *gb, const uint_fast32_t addr)
{
    ccgb_t *c = gb->direct.priv;

    if(addr < GB_BANK_SIZE) {
        return c->bank0[addr];
    }

    // Peanut-GB passes bank * 0x4000 + offset for the switchable area
    uint16_t bank = addr / GB_BANK_SIZE;
    if(bank != c->cur_bank) {
        if(bank >= c->rom_banks) return 0xff;

        c->cur_mem = bank_mem(c, bank);
        c->cur_bank = bank;
    }

    return c->cur_mem[addr & (GB_BANK_SIZE - 1)];
}

static uint8_t cart_ram_read(struct gb_s *gb, const uint_fast32_t addr)
{
    ccgb_t *c = gb->direct.priv;

    return (addr < c->cart_ram_size) ? c->cart_ram[addr] : 0xff;
}

static void cart_ram_write(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val)
{
    ccgb_t *c = gb->direct.priv;

    if(addr >= c->cart_ram_size || c->cart_ram[addr] == val) return;

    c->cart_ram[addr] = val;
    if(!c->ram_dirty) {
        c->ram_dirty = true;
        c->ram_first_write = c->stats.frames;
    }
    c->ram_last_write = c->stats.frames;
}

static void gb_error(struct gb_s *gb, const enum gb_error_e err, const uint16_t addr)
{
    ccgb_t *c = gb->direct.priv;

    if(!c->error) {
        c->error = (int)err + 1;
        c->error_addr = addr;
    }
}

static void serial_tx(struct gb_s *gb, const uint8_t tx)
{
    ccgb_t *c = gb->direct.priv;

    if(c->serial_len < (int)sizeof(c->serial) - 1) {
        c->serial[c->serial_len++] = tx;
        c->serial[c->serial_len] = 0;
    }
}

static enum gb_serial_rx_ret_e serial_rx(struct gb_s *gb, uint8_t *rx)
{
    (void)gb;
    *rx = 0xff;
    return GB_SERIAL_RX_NO_CONNECTION;
}

static void draw_line(struct gb_s *gb, const uint8_t *pixels, const uint_fast8_t line)
{
    ccgb_t *c = gb->direct.priv;

    if(line >= GB_H) return;

    uint8_t *d = c->fb[line];
    for(int i = 0; i < GB_LINE_BYTES; i++, pixels += 4) {
        d[i] = (pixels[0] & 3) | ((pixels[1] & 3) << 2)
                | ((pixels[2] & 3) << 4) | ((pixels[3] & 3) << 6);
    }
}

    int
ccgb_init(ccgb_t *c, const uint8_t *rom, uint32_t rom_size,
          uint8_t *bank0, uint8_t **slots, int n_slots)
{
    if(rom_size < 0x150) return GB_INIT_INVALID_CHECKSUM;
    if(n_slots > GB_MAX_SLOTS) n_slots = GB_MAX_SLOTS;

    c->rom = rom;
    c->rom_size = rom_size;
    c->rom_banks = rom_size / GB_BANK_SIZE;
    if(!c->rom_banks) c->rom_banks = 1;

    c->bank0 = bank0;
    memcpy(bank0, rom, rom_size < GB_BANK_SIZE ? rom_size : GB_BANK_SIZE);

    c->n_slots = n_slots;
    for(int i = 0; i < n_slots; i++) {
        c->slot_mem[i] = slots[i];
        c->slot_bank[i] = 0;        // bank 0 is never looked up here: empty
        c->slot_used[i] = 0;
    }
    c->cur_bank = NO_BANK;
    c->cur_mem = NULL;

    int rv = gb_init(&c->gb, rom_read, cart_ram_read, cart_ram_write, gb_error, c);
    if(rv != GB_INIT_NO_ERROR) return rv;

    gb_init_lcd(&c->gb, draw_line);
    gb_init_serial(&c->gb, serial_tx, serial_rx);

    c->full_redraw = true;

    return GB_INIT_NO_ERROR;
}

    uint32_t
ccgb_cart_ram_size(ccgb_t *c)
{
    size_t sz = 0;

    if(gb_get_save_size_s(&c->gb, &sz) != 0) return 0;

    return sz;
}

    void
ccgb_set_cart_ram(ccgb_t *c, uint8_t *ram, uint32_t size)
{
    c->cart_ram = ram;
    c->cart_ram_size = ram ? size : 0;
    c->ram_dirty = false;
}

    void
ccgb_saved(ccgb_t *c)
{
    c->ram_dirty = false;
}

    void
ccgb_set_keymap(ccgb_t *c, const uint8_t map[GB_KEYMAP_BYTES])
{
    memcpy(&c->keymap, map, GB_KEYMAP_BYTES);
}

    void
ccgb_set_video(ccgb_t *c, int mode, const uint32_t rgb[4], uint32_t border)
{
    gb_video_setup(&c->video, mode, rgb, border, hal_lcd_big_endian);
    c->full_redraw = true;
}

    void
ccgb_title(ccgb_t *c, char title[17])
{
    gb_get_rom_name(&c->gb, title);
}

    void
ccgb_state_header(ccgb_t *c, uint8_t hdr[GB_STATE_HDR])
{
    uint32_t state_size = sizeof(struct gb_s);

    memcpy(hdr, "CCGBST01", 8);
    memcpy(hdr + 8, &state_size, 4);
    memcpy(hdr + 12, &c->cart_ram_size, 4);

    // cartridge header: title (0x134..0x143), then header and global
    // checksums (0x14d..0x14f), plus a pad byte
    memcpy(hdr + 16, c->bank0 + 0x134, 16);
    memcpy(hdr + 32, c->bank0 + 0x14d, 3);
    hdr[35] = 0;
}

    void
ccgb_state_load(ccgb_t *c, const uint8_t *state)
{
    struct gb_s *gb = &c->gb;

    // keep this run's pointers: the saved ones may be from another boot
    // (only these fields; all of gb_s is too big for the stack)
    typeof(gb->gb_rom_read) rom_read = gb->gb_rom_read;
    typeof(gb->gb_cart_ram_read) ram_read = gb->gb_cart_ram_read;
    typeof(gb->gb_cart_ram_write) ram_write = gb->gb_cart_ram_write;
    typeof(gb->gb_error) error = gb->gb_error;
    typeof(gb->gb_serial_tx) serial_tx = gb->gb_serial_tx;
    typeof(gb->gb_serial_rx) serial_rx = gb->gb_serial_rx;
    typeof(gb->gb_bootrom_read) bootrom_read = gb->gb_bootrom_read;
    typeof(gb->display.lcd_draw_line) draw_line = gb->display.lcd_draw_line;
    void *priv = gb->direct.priv;
    uint8_t joypad = gb->direct.joypad;

    memcpy(gb, state, sizeof(*gb));

    gb->gb_rom_read = rom_read;
    gb->gb_cart_ram_read = ram_read;
    gb->gb_cart_ram_write = ram_write;
    gb->gb_error = error;
    gb->gb_serial_tx = serial_tx;
    gb->gb_serial_rx = serial_rx;
    gb->gb_bootrom_read = bootrom_read;
    gb->display.lcd_draw_line = draw_line;
    gb->direct.priv = priv;
    gb->direct.joypad = joypad;

    c->cur_bank = NO_BANK;
    c->cur_mem = NULL;
    c->error = 0;
    c->full_redraw = true;

    if(c->cart_ram_size) {
        c->ram_dirty = true;
        c->ram_first_write = c->ram_last_write = c->stats.frames;
    }
}

static inline uint64_t key_bit(uint8_t kn)
{
    return (kn < 64) ? (1ULL << kn) : 0;
}

static uint8_t joypad_from_keys(const gb_keymap_t *km, uint64_t keys)
{
    uint8_t pressed = 0;

    for(int b = 0; b < GB_NUM_BTN; b++) {
        for(int k = 0; k < GB_KEYS_PER_BTN; k++) {
            if(keys & key_bit(km->btn[b][k])) pressed |= (1 << b);
        }
    }

    // a real D-pad can't press opposite directions at once; some games crash
    if((pressed & JOYPAD_LEFT) && (pressed & JOYPAD_RIGHT)) {
        pressed &= ~(JOYPAD_LEFT | JOYPAD_RIGHT);
    }
    if((pressed & JOYPAD_UP) && (pressed & JOYPAD_DOWN)) {
        pressed &= ~(JOYPAD_UP | JOYPAD_DOWN);
    }

    // Peanut-GB: bit clear = pressed
    return ~pressed;
}

static void draw_border(ccgb_t *c)
{
    const gb_video_t *v = &c->video;
    int right = v->x + v->w, bottom = v->y + v->h;

    if(v->y) hal_lcd_fill(0, 0, HAL_LCD_W, v->y, v->border);
    if(bottom < HAL_LCD_H) hal_lcd_fill(0, bottom, HAL_LCD_W, HAL_LCD_H - bottom, v->border);
    if(v->x) hal_lcd_fill(0, v->y, v->x, v->h, v->border);
    if(right < HAL_LCD_W) hal_lcd_fill(right, v->y, HAL_LCD_W - right, v->h, v->border);
}

static bool group_changed(const ccgb_t *c, int group)
{
    if(c->full_redraw) return true;

    const gb_video_t *v = &c->video;
    int line = v->src_first + group * v->src_lines;

    return memcmp(c->fb[line], c->fb_shown[line], v->src_lines * GB_LINE_BYTES) != 0;
}

// Send the lines that changed since the last frame shown.
static void present(ccgb_t *c)
{
    const gb_video_t *v = &c->video;
    const int group_px = v->w * v->out_rows;

    for(int g = 0; g < v->groups; ) {
        if(!group_changed(c, g)) {
            g++;
            continue;
        }

        int end = g + 1;
        while(end < v->groups && group_changed(c, end)) end++;

        hal_lcd_window(v->x, v->y + g * v->out_rows, v->w, (end - g) * v->out_rows);

        while(g < end) {
            int n = end - g;
            if(n > GROUPS_PER_WRITE) n = GROUPS_PER_WRITE;

            uint16_t *dst = c->out_buf[c->out_next];
            for(int k = 0; k < n; k++, g++) {
                int line = v->src_first + g * v->src_lines;

                gb_video_render_group(v, (const uint8_t (*)[GB_LINE_BYTES])c->fb,
                                      g, dst + k * group_px);
                memcpy(c->fb_shown[line], c->fb[line], v->src_lines * GB_LINE_BYTES);
            }

            hal_lcd_write(dst, n * group_px);
            c->out_next ^= 1;
            c->stats.lines_sent += n;
        }
    }

    c->full_redraw = false;
}

static bool save_due(const ccgb_t *c)
{
    if(!c->ram_dirty) return false;

    uint32_t idle = c->stats.frames - c->ram_last_write;
    uint32_t age = c->stats.frames - c->ram_first_write;

    if(!c->gb.enable_cart_ram && idle >= SAVE_AFTER_DISABLE) return true;

    return idle >= SAVE_AFTER_IDLE || age >= SAVE_AT_MOST;
}

    int
ccgb_run(ccgb_t *c, uint32_t max_frames, bool redraw)
{
    const gb_keymap_t *km = &c->keymap;
    const uint64_t menu_bit = key_bit(km->menu);
    const uint64_t fast_bit = key_bit(km->fast);
    uint32_t count = 0;
    int rv;

    hal_lcd_begin();
    if(redraw) {
        draw_border(c);
        c->full_redraw = true;
    }

    // Keys already held (like the one that picked "resume") are ignored
    // until released, so they don't leak into the game.
    uint64_t masked = hal_keys();
    c->last_keys = masked;

    c->deadline_ns = hal_micros() * 1000;

    for(;;) {
        uint64_t keys = hal_keys();
        masked &= keys;

        // Menu key only counts alone: a matrix ghost always comes with
        // three other keys held.
        if((keys & menu_bit) && !(c->last_keys & menu_bit) && keys == menu_bit) {
            c->last_keys = keys;
            rv = GB_RUN_MENU;
            break;
        }
        c->last_keys = keys;

        bool fast = (keys & fast_bit) != 0;
        c->gb.direct.joypad = joypad_from_keys(km, keys & ~masked);

        uint64_t t0 = hal_micros();
        gb_run_frame(&c->gb);
        c->stats.frames++;

        if(!(c->gb.hram_io[IO_LCDC] & LCDC_ENABLE)) {
            // LCD off shows blank (lightest shade)
            memset(c->fb, 0, sizeof(c->fb));
        }

        uint64_t t1 = hal_micros();
        c->stats.emu_us += t1 - t0;

        if(c->error) {
            rv = GB_RUN_ERROR;
            break;
        }

        // Show this frame unless emulation is already late for the next one.
        c->deadline_ns += FRAME_NS;
        bool behind = (t1 * 1000) > c->deadline_ns;
        if((!behind && !fast) || c->frames_unshown >= MAX_UNSHOWN) {
            present(c);
            c->stats.shown++;
            c->frames_unshown = 0;
        } else {
            c->stats.skipped++;
            c->frames_unshown++;
        }

        uint64_t t2 = hal_micros();
        c->stats.lcd_us += t2 - t1;

        uint64_t now_ns = t2 * 1000;
        if(fast) {
            c->deadline_ns = now_ns;
        } else if(c->deadline_ns > now_ns) {
            hal_wait_until(c->deadline_ns / 1000);
            c->stats.idle_us += hal_micros() - t2;
        } else if(now_ns - c->deadline_ns > MAX_BEHIND_NS) {
            c->deadline_ns = now_ns;
        }

        if(save_due(c)) {
            rv = GB_RUN_SAVE;
            break;
        }

        if(max_frames && ++count >= max_frames) {
            rv = GB_RUN_FRAMES;
            break;
        }
    }

    hal_lcd_sync();
    hal_lcd_end();

    return rv;
}
