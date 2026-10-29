//
// modgameboy.c - MicroPython bindings for the Game Boy emulator.
//
// Python side is shared/gameboy.py. Typical use:
//
//   emu = _gameboy.Emulator()
//   emu.rom_begin(size)
//   emu.rom_write(offset, chunk) ...      # whole ROM
//   title, ram_size, slots = emu.start(6)
//   emu.cart_ram()[:] = saved_data        # memoryview
//   emu.keymap(bytes(18)); emu.video(mode, (c0, c1, c2, c3), border)
//   while True: rv = emu.run() ...
//   emu.close()
//
// Save states: state_header() + state_view() + cart_ram() written out, and
// state_load() to restore.
//
// Pokemon trades: link_trade(name, theirs, mine) plugs in a partner for
// the Cable Club (gb_link.h); link_status() says how it went.
//
#include <string.h>

#include "py/obj.h"
#include "py/objarray.h"
#include "py/runtime.h"
#include "py/mperrno.h"

#include "gb_core.h"
#include "gb_hal.h"

typedef struct {
    mp_obj_base_t base;

    ccgb_t *core;
    uint8_t *rom;
    uint32_t rom_size;
    uint8_t *bank0;
    uint8_t *slots[GB_MAX_SLOTS];
    int n_slots;
    uint8_t *cart_ram;
    uint32_t cart_ram_size;
} gb_obj_t;

const mp_obj_type_t gb_emulator_type;

STATIC gb_obj_t *get_started(mp_obj_t self_in)
{
    gb_obj_t *self = MP_OBJ_TO_PTR(self_in);

    if(!self->core) {
        mp_raise_ValueError(MP_ERROR_TEXT("not started"));
    }

    return self;
}

STATIC mp_obj_t gb_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args)
{
    mp_arg_check_num(n_args, n_kw, 0, 0, false);

    gb_obj_t *self = m_new0(gb_obj_t, 1);
    self->base.type = &gb_emulator_type;

    return MP_OBJ_FROM_PTR(self);
}

// rom_begin(size): get ROM storage ready for `size` bytes
STATIC mp_obj_t gb_rom_begin(mp_obj_t self_in, mp_obj_t size_in)
{
    gb_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_int_t size = mp_obj_get_int(size_in);

    if(self->core) {
        mp_raise_ValueError(MP_ERROR_TEXT("already started"));
    }
    if(size < 0x150 || size > 8 * 1024 * 1024) {
        mp_raise_ValueError(MP_ERROR_TEXT("bad ROM size"));
    }

    // whole 16K banks, padded with 0xff
    uint32_t padded = (size + GB_BANK_SIZE - 1) & ~(GB_BANK_SIZE - 1);
    self->rom = hal_rom_alloc(padded);
    if(!self->rom) {
        mp_raise_OSError(MP_ENOMEM);
    }
    self->rom_size = padded;

    if(padded > (uint32_t)size) {
        static const uint8_t ff[64] = {
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        };
        // padding starts at a 16K boundary minus the ROM's tail; round down
        // to a word so the Q's PSRAM sees aligned writes
        for(uint32_t pos = size & ~3; pos < padded; pos += sizeof(ff)) {
            uint32_t n = padded - pos;
            hal_rom_write(self->rom + pos, ff, n < sizeof(ff) ? n : sizeof(ff));
        }
    }

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(gb_rom_begin_obj, gb_rom_begin);

// rom_write(offset, data): copy part of the ROM file into place
STATIC mp_obj_t gb_rom_write(mp_obj_t self_in, mp_obj_t offset_in, mp_obj_t data_in)
{
    gb_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_int_t offset = mp_obj_get_int(offset_in);
    mp_buffer_info_t buf;
    mp_get_buffer_raise(data_in, &buf, MP_BUFFER_READ);

    if(!self->rom || self->core) {
        mp_raise_ValueError(MP_ERROR_TEXT("call rom_begin first"));
    }
    if(offset < 0 || (offset & 3) || offset + buf.len > self->rom_size) {
        mp_raise_ValueError(MP_ERROR_TEXT("bad offset"));
    }

    hal_rom_write(self->rom + offset, buf.buf, buf.len);

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_3(gb_rom_write_obj, gb_rom_write);

STATIC void free_buffers(gb_obj_t *self)
{
    for(int i = 0; i < self->n_slots; i++) {
        m_del(uint8_t, self->slots[i], GB_BANK_SIZE);
        self->slots[i] = NULL;
    }
    self->n_slots = 0;

    if(self->bank0) {
        m_del(uint8_t, self->bank0, GB_BANK_SIZE);
        self->bank0 = NULL;
    }
    if(self->cart_ram) {
        m_del(uint8_t, self->cart_ram, self->cart_ram_size);
        self->cart_ram = NULL;
        self->cart_ram_size = 0;
    }
    if(self->core) {
        m_del(ccgb_t, self->core, 1);
        self->core = NULL;
    }
}

// start(slots) -> (title, cart_ram_size, slots_used)
// Uses up to `slots` 16K bank cache buffers, fewer if memory is short.
STATIC mp_obj_t gb_start(mp_obj_t self_in, mp_obj_t slots_in)
{
    gb_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_int_t want = mp_obj_get_int(slots_in);

    if(!self->rom || self->core) {
        mp_raise_ValueError(MP_ERROR_TEXT("call rom_begin first"));
    }
    if(want < 0) want = 0;
    if(want > GB_MAX_SLOTS) want = GB_MAX_SLOTS;

    self->core = m_new0(ccgb_t, 1);
    self->bank0 = m_new(uint8_t, GB_BANK_SIZE);

    // no point caching more banks than the ROM has (bank 0 is separate)
    int banks = self->rom_size / GB_BANK_SIZE;
    if(want > banks - 1) want = banks - 1;

    while(self->n_slots < want) {
        uint8_t *p = m_new_maybe(uint8_t, GB_BANK_SIZE);
        if(!p) break;
        self->slots[self->n_slots++] = p;
    }

    int rv = ccgb_init(self->core, self->rom, self->rom_size,
                       self->bank0, self->slots, self->n_slots);
    if(rv != GB_INIT_NO_ERROR) {
        free_buffers(self);
        if(rv == GB_INIT_CARTRIDGE_UNSUPPORTED) {
            mp_raise_ValueError(MP_ERROR_TEXT("unsupported cartridge type"));
        }
        mp_raise_ValueError(MP_ERROR_TEXT("bad ROM header checksum"));
    }

    uint32_t ram = ccgb_cart_ram_size(self->core);
    if(ram) {
        self->cart_ram = m_new(uint8_t, ram);
        memset(self->cart_ram, 0xff, ram);
        self->cart_ram_size = ram;
    }
    ccgb_set_cart_ram(self->core, self->cart_ram, ram);

    char title[17] = { 0 };
    ccgb_title(self->core, title);

    mp_obj_t tup[3] = {
        mp_obj_new_str(title, strlen(title)),
        mp_obj_new_int(ram),
        mp_obj_new_int(self->n_slots),
    };

    return mp_obj_new_tuple(3, tup);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(gb_start_obj, gb_start);

// cart_ram() -> writable memoryview of cartridge RAM (None if the game has none)
STATIC mp_obj_t gb_cart_ram(mp_obj_t self_in)
{
    gb_obj_t *self = get_started(self_in);

    if(!self->cart_ram) return mp_const_none;

    return mp_obj_new_memoryview(MP_OBJ_ARRAY_TYPECODE_FLAG_RW | 'B',
                                 self->cart_ram_size, self->cart_ram);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(gb_cart_ram_obj, gb_cart_ram);

// dirty() -> True if cart RAM changed since saved() was last called
STATIC mp_obj_t gb_dirty(mp_obj_t self_in)
{
    gb_obj_t *self = get_started(self_in);

    return mp_obj_new_bool(self->core->ram_dirty);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(gb_dirty_obj, gb_dirty);

STATIC mp_obj_t gb_saved(mp_obj_t self_in)
{
    gb_obj_t *self = get_started(self_in);

    ccgb_saved(self->core);

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(gb_saved_obj, gb_saved);

// keymap(map): bytes of key numbers, 2 per button (A B SELECT START
// RIGHT LEFT UP DOWN), then the menu and fast-forward keys; 0xff = none
STATIC mp_obj_t gb_keymap(mp_obj_t self_in, mp_obj_t map_in)
{
    gb_obj_t *self = get_started(self_in);
    mp_buffer_info_t buf;
    mp_get_buffer_raise(map_in, &buf, MP_BUFFER_READ);

    if(buf.len != GB_KEYMAP_BYTES) {
        mp_raise_ValueError(MP_ERROR_TEXT("keymap length"));
    }

    ccgb_set_keymap(self->core, buf.buf);

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(gb_keymap_obj, gb_keymap);

// video(mode, (rgb0, rgb1, rgb2, rgb3), border_rgb): colours are 0xRRGGBB
STATIC mp_obj_t gb_video(size_t n_args, const mp_obj_t *args)
{
    gb_obj_t *self = get_started(args[0]);
    mp_int_t mode = mp_obj_get_int(args[1]);
    size_t n;
    mp_obj_t *items;
    uint32_t rgb[4];

    mp_obj_get_array(args[2], &n, &items);
    if(n != 4) {
        mp_raise_ValueError(MP_ERROR_TEXT("need 4 colours"));
    }
    for(int i = 0; i < 4; i++) {
        rgb[i] = mp_obj_get_int(items[i]);
    }

    ccgb_set_video(self->core, mode, rgb, mp_obj_get_int(args[3]));

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gb_video_obj, 4, 4, gb_video);

// run(max_frames=0, redraw=False) -> RUN_* code
STATIC mp_obj_t gb_run(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
    enum { ARG_max_frames, ARG_redraw };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_max_frames, MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_redraw, MP_ARG_BOOL, {.u_bool = false} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);

    gb_obj_t *self = get_started(pos_args[0]);

    int rv = ccgb_run(self->core, args[ARG_max_frames].u_int, args[ARG_redraw].u_bool);

    return MP_OBJ_NEW_SMALL_INT(rv);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(gb_run_obj, 1, gb_run);

// stats(reset=False) -> (frames, shown, skipped, groups_sent, cache_hits,
//                        cache_misses, emu_us, lcd_us, idle_us)
STATIC mp_obj_t gb_stats(size_t n_args, const mp_obj_t *args)
{
    gb_obj_t *self = get_started(args[0]);
    gb_stats_t *s = &self->core->stats;

    mp_obj_t tup[9] = {
        mp_obj_new_int_from_uint(s->frames),
        mp_obj_new_int_from_uint(s->shown),
        mp_obj_new_int_from_uint(s->skipped),
        mp_obj_new_int_from_uint(s->lines_sent),
        mp_obj_new_int_from_uint(s->cache_hits),
        mp_obj_new_int_from_uint(s->cache_misses),
        mp_obj_new_int_from_ull(s->emu_us),
        mp_obj_new_int_from_ull(s->lcd_us),
        mp_obj_new_int_from_ull(s->idle_us),
    };
    mp_obj_t rv = mp_obj_new_tuple(9, tup);

    if(n_args > 1 && mp_obj_is_true(args[1])) {
        // keep the frame counter: save timing depends on it
        uint32_t frames = s->frames;
        memset(s, 0, sizeof(*s));
        s->frames = frames;
    }

    return rv;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gb_stats_obj, 1, 2, gb_stats);

// state_header() -> bytes that start a save state file for this ROM/build
STATIC mp_obj_t gb_state_header(mp_obj_t self_in)
{
    gb_obj_t *self = get_started(self_in);
    uint8_t hdr[GB_STATE_HDR];

    ccgb_state_header(self->core, hdr);

    return mp_obj_new_bytes(hdr, sizeof(hdr));
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(gb_state_header_obj, gb_state_header);

// state_view() -> read-only memoryview of the emulator state; a save state
// is state_header() + this + cart_ram()
STATIC mp_obj_t gb_state_view(mp_obj_t self_in)
{
    gb_obj_t *self = get_started(self_in);

    return mp_obj_new_memoryview('B', sizeof(struct gb_s), &self->core->gb);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(gb_state_view_obj, gb_state_view);

// state_load(state, ram): restore from the parts after the header; ram is
// None if the game has no cart RAM
STATIC mp_obj_t gb_state_load(mp_obj_t self_in, mp_obj_t state_in, mp_obj_t ram_in)
{
    gb_obj_t *self = get_started(self_in);
    mp_buffer_info_t st, ram = { 0 };

    mp_get_buffer_raise(state_in, &st, MP_BUFFER_READ);
    if(ram_in != mp_const_none) {
        mp_get_buffer_raise(ram_in, &ram, MP_BUFFER_READ);
    }
    if(st.len != sizeof(struct gb_s) || ram.len != self->cart_ram_size) {
        mp_raise_ValueError(MP_ERROR_TEXT("state size"));
    }

    if(ram.len) memcpy(self->cart_ram, ram.buf, ram.len);
    ccgb_state_load(self->core, st.buf);

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_3(gb_state_load_obj, gb_state_load);

// error() -> None, or (code, address) of the first emulation error
STATIC mp_obj_t gb_error_info(mp_obj_t self_in)
{
    gb_obj_t *self = get_started(self_in);

    if(!self->core->error) return mp_const_none;

    mp_obj_t tup[2] = {
        MP_OBJ_NEW_SMALL_INT(self->core->error - 1),
        MP_OBJ_NEW_SMALL_INT(self->core->error_addr),
    };

    return mp_obj_new_tuple(2, tup);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(gb_error_info_obj, gb_error_info);

// reset(): turn the Game Boy off and on; cart RAM stays
STATIC mp_obj_t gb_reset_game(mp_obj_t self_in)
{
    gb_obj_t *self = get_started(self_in);

    ccgb_reset(self->core);

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(gb_reset_obj, gb_reset_game);

// wram() -> read-only memoryview of work RAM (0xC000..0xDFFF)
STATIC mp_obj_t gb_wram(mp_obj_t self_in)
{
    gb_obj_t *self = get_started(self_in);

    return mp_obj_new_memoryview('B', WRAM_SIZE, self->core->gb.wram);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(gb_wram_obj, gb_wram);

// link_trade(name, theirs, mine): plug in a Pokemon trade partner (see
// gb_link.h) called name, offering theirs for mine; each Pokemon is
// party struct + OT name + nickname. link_trade(None) unplugs it.
STATIC mp_obj_t gb_link_trade(size_t n_args, const mp_obj_t *args)
{
    gb_obj_t *self = get_started(args[0]);
    gb_link_t *link = &self->core->link;

    if(args[1] == mp_const_none) {
        gb_link_off(link);
        return mp_const_none;
    }
    if(n_args != 4) {
        mp_raise_TypeError(MP_ERROR_TEXT("name, theirs, mine"));
    }

    mp_buffer_info_t name, theirs, mine;
    mp_get_buffer_raise(args[1], &name, MP_BUFFER_READ);
    mp_get_buffer_raise(args[2], &theirs, MP_BUFFER_READ);
    mp_get_buffer_raise(args[3], &mine, MP_BUFFER_READ);
    if(name.len != GB_LINK_NAME || theirs.len != sizeof(gb_link_mon_t)
            || mine.len != sizeof(gb_link_mon_t)) {
        mp_raise_ValueError(MP_ERROR_TEXT("trade data length"));
    }

    gb_link_setup(link, name.buf, theirs.buf, mine.buf);

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gb_link_trade_obj, 2, 4, gb_link_trade);

// link_status() -> (state, traded, refused): LINK_* state, whether the
// trade happened, and how many times the wrong Pokemon was offered
STATIC mp_obj_t gb_link_status(mp_obj_t self_in)
{
    gb_obj_t *self = get_started(self_in);
    gb_link_t *link = &self->core->link;

    mp_obj_t tup[3] = {
        MP_OBJ_NEW_SMALL_INT(link->state),
        mp_obj_new_bool(link->traded),
        MP_OBJ_NEW_SMALL_INT(link->refused),
    };

    return mp_obj_new_tuple(3, tup);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(gb_link_status_obj, gb_link_status);

// close(): free everything; the object can't be used again
STATIC mp_obj_t gb_close(mp_obj_t self_in)
{
    gb_obj_t *self = MP_OBJ_TO_PTR(self_in);

    free_buffers(self);
    if(self->rom) {
        hal_rom_free();
        self->rom = NULL;
        self->rom_size = 0;
    }

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(gb_close_obj, gb_close);

STATIC const mp_rom_map_elem_t gb_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_rom_begin), MP_ROM_PTR(&gb_rom_begin_obj) },
    { MP_ROM_QSTR(MP_QSTR_rom_write), MP_ROM_PTR(&gb_rom_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&gb_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_cart_ram), MP_ROM_PTR(&gb_cart_ram_obj) },
    { MP_ROM_QSTR(MP_QSTR_dirty), MP_ROM_PTR(&gb_dirty_obj) },
    { MP_ROM_QSTR(MP_QSTR_saved), MP_ROM_PTR(&gb_saved_obj) },
    { MP_ROM_QSTR(MP_QSTR_keymap), MP_ROM_PTR(&gb_keymap_obj) },
    { MP_ROM_QSTR(MP_QSTR_video), MP_ROM_PTR(&gb_video_obj) },
    { MP_ROM_QSTR(MP_QSTR_run), MP_ROM_PTR(&gb_run_obj) },
    { MP_ROM_QSTR(MP_QSTR_stats), MP_ROM_PTR(&gb_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_state_header), MP_ROM_PTR(&gb_state_header_obj) },
    { MP_ROM_QSTR(MP_QSTR_state_view), MP_ROM_PTR(&gb_state_view_obj) },
    { MP_ROM_QSTR(MP_QSTR_state_load), MP_ROM_PTR(&gb_state_load_obj) },
    { MP_ROM_QSTR(MP_QSTR_error), MP_ROM_PTR(&gb_error_info_obj) },
    { MP_ROM_QSTR(MP_QSTR_reset), MP_ROM_PTR(&gb_reset_obj) },
    { MP_ROM_QSTR(MP_QSTR_wram), MP_ROM_PTR(&gb_wram_obj) },
    { MP_ROM_QSTR(MP_QSTR_link_trade), MP_ROM_PTR(&gb_link_trade_obj) },
    { MP_ROM_QSTR(MP_QSTR_link_status), MP_ROM_PTR(&gb_link_status_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&gb_close_obj) },
};
STATIC MP_DEFINE_CONST_DICT(gb_locals_dict, gb_locals_dict_table);

const mp_obj_type_t gb_emulator_type = {
    { &mp_type_type },
    .name = MP_QSTR_Emulator,
    .make_new = gb_make_new,
    .locals_dict = (void *)&gb_locals_dict,
};

// keys() -> bit mask of Q keys held now, bit N for key number N
STATIC mp_obj_t gb_keys(void)
{
    return mp_obj_new_int_from_ull(hal_keys());
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(gb_keys_obj, gb_keys);

#ifdef GB_HAL_UNIX
extern const mp_obj_fun_builtin_var_t gb_sim_setup_obj;
extern const mp_obj_fun_builtin_fixed_t gb_sim_keys_obj;
#endif

STATIC const mp_rom_map_elem_t mp_module_gameboy_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR__gameboy) },
    { MP_ROM_QSTR(MP_QSTR_Emulator), MP_ROM_PTR(&gb_emulator_type) },
    { MP_ROM_QSTR(MP_QSTR_keys), MP_ROM_PTR(&gb_keys_obj) },
#ifdef GB_HAL_UNIX
    { MP_ROM_QSTR(MP_QSTR_sim_setup), MP_ROM_PTR(&gb_sim_setup_obj) },
    { MP_ROM_QSTR(MP_QSTR_sim_keys), MP_ROM_PTR(&gb_sim_keys_obj) },
#endif

    { MP_ROM_QSTR(MP_QSTR_RUN_FRAMES), MP_ROM_INT(GB_RUN_FRAMES) },
    { MP_ROM_QSTR(MP_QSTR_RUN_MENU), MP_ROM_INT(GB_RUN_MENU) },
    { MP_ROM_QSTR(MP_QSTR_RUN_SAVE), MP_ROM_INT(GB_RUN_SAVE) },
    { MP_ROM_QSTR(MP_QSTR_RUN_ERROR), MP_ROM_INT(GB_RUN_ERROR) },

    { MP_ROM_QSTR(MP_QSTR_VIDEO_1X), MP_ROM_INT(VIDEO_1X) },
    { MP_ROM_QSTR(MP_QSTR_VIDEO_15_SMOOTH), MP_ROM_INT(VIDEO_15_SMOOTH) },
    { MP_ROM_QSTR(MP_QSTR_VIDEO_15_SHARP), MP_ROM_INT(VIDEO_15_SHARP) },
    { MP_ROM_QSTR(MP_QSTR_VIDEO_2X_CROP), MP_ROM_INT(VIDEO_2X_CROP) },
    { MP_ROM_QSTR(MP_QSTR_VIDEO_OLED_FILL), MP_ROM_INT(VIDEO_OLED_FILL) },
    { MP_ROM_QSTR(MP_QSTR_VIDEO_OLED_FIT), MP_ROM_INT(VIDEO_OLED_FIT) },

    { MP_ROM_QSTR(MP_QSTR_LINK_OFF), MP_ROM_INT(GB_LINK_OFF) },
    { MP_ROM_QSTR(MP_QSTR_LINK_WAITING), MP_ROM_INT(GB_LINK_WAITING) },
    { MP_ROM_QSTR(MP_QSTR_LINK_READY), MP_ROM_INT(GB_LINK_READY) },
    { MP_ROM_QSTR(MP_QSTR_LINK_BLOCKS), MP_ROM_INT(GB_LINK_BLOCKS) },
    { MP_ROM_QSTR(MP_QSTR_LINK_SELECT), MP_ROM_INT(GB_LINK_SELECT) },
    { MP_ROM_QSTR(MP_QSTR_LINK_CONFIRM), MP_ROM_INT(GB_LINK_CONFIRM) },

    { MP_ROM_QSTR(MP_QSTR_NO_KEY), MP_ROM_INT(GB_NO_KEY) },
    { MP_ROM_QSTR(MP_QSTR_KEYMAP_BYTES), MP_ROM_INT(GB_KEYMAP_BYTES) },
};
STATIC MP_DEFINE_CONST_DICT(mp_module_gameboy_globals, mp_module_gameboy_globals_table);

const mp_obj_module_t mp_module_gameboy = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mp_module_gameboy_globals,
};

MP_REGISTER_MODULE(MP_QSTR__gameboy, mp_module_gameboy, 1);
