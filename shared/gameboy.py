# gameboy.py - Game Boy (DMG) emulator, Q only.
#
# Emulation runs in C (external/c-modules/gameboy); this file picks the
# ROM, loads it, and runs the pause menu between calls to Emulator.run().
#
import gc, utime, uos
import _gameboy as core
from glob import dis
from ux import ux_show_story, the_ux
from menu import MenuSystem, MenuItem
from files import CardSlot

# Bank cache: 16K buffers of switchable ROM banks kept in SRAM
CACHE_SLOTS = const(6)
CHUNK = const(16384)

# Q key numbers (row * 10 + col), see shared/charcodes.py
KN_LEFT = const(3)
KN_UP = const(4)
KN_DOWN = const(5)
KN_RIGHT = const(6)
KN_CANCEL = const(7)
KN_ENTER = const(8)
KN_W = const(21)
KN_A = const(30)
KN_S = const(31)
KN_D = const(32)
KN_J = const(36)
KN_K = const(37)
KN_SPACE = const(52)

NO = core.NO_KEY

# Two Q keys per button, in this order: A B SELECT START RIGHT LEFT UP DOWN,
# then the menu key and the fast-forward key.
DEFAULT_KEYMAP = bytes([
    KN_K, NO,
    KN_J, NO,
    KN_SPACE, NO,
    KN_ENTER, NO,
    KN_D, KN_RIGHT,
    KN_A, KN_LEFT,
    KN_W, KN_UP,
    KN_S, KN_DOWN,
    KN_CANCEL,
    NO,
])

PALETTES = [
    ('Green', (0xe0f8d0, 0x88c070, 0x346856, 0x081820)),
    ('Pea Soup', (0x9bbc0f, 0x8bac0f, 0x306230, 0x0f380f)),
    ('Grey', (0xffffff, 0xaaaaaa, 0x555555, 0x000000)),
]
BORDER = 0x000000

def show_rom_error(fn, exc):
    return ux_show_story('Unable to start %s\n\n%s' % (fn.split('/')[-1], exc),
                         title='Game Boy')

def load_rom(emu, fn):
    # Copy ROM file into emulator memory (PSRAM on Q).
    with CardSlot(readonly=True) as card:
        size = uos.stat(fn)[6]
        emu.rom_begin(size)

        buf = bytearray(CHUNK)
        pos = 0
        with open(fn, 'rb') as fd:
            while pos < size:
                n = fd.readinto(buf)
                if not n:
                    raise OSError('short read')
                emu.rom_write(pos, memoryview(buf)[0:n])
                pos += n
                dis.progress_bar_show(pos / size)

    del buf
    gc.collect()

    return emu.start(CACHE_SLOTS)

async def pause_menu(emu):
    # Returns True to quit.
    picked = []

    async def pick(_1, _2, item):
        picked.append(item.arg)
        the_ux.pop()

    m = MenuSystem([
        MenuItem('Resume', f=pick, arg='resume'),
        MenuItem('Quit Game', f=pick, arg='quit'),
    ])
    the_ux.push(m)
    await m.interact()

    return picked and picked[0] == 'quit'

def screen_back():
    # Python's display code tracks what's on the LCD; the game drew over it.
    dis._full_redraw = True
    dis.clear()

def keys_back():
    # The game consumed key events; don't let idle-logout think nobody's here.
    from glob import numpad
    numpad.last_event_time = utime.ticks_ms()

def log_stats(emu, last):
    # Frame rate and time split since the last call, to the console. Returns
    # the frame counter for next time.
    frames, shown, skipped, groups, hits, misses, emu_us, lcd_us, idle_us = emu.stats(True)
    n = frames - last
    total = emu_us + lcd_us + idle_us
    if n and total:
        print('gameboy: %d frames, %.2f fps, %d shown, %d skipped; '
              'emu %d%% lcd %d%% idle %d%%; bank cache %d/%d' % (
              n, n * 1e6 / total, shown, skipped,
              emu_us * 100 // total, lcd_us * 100 // total, idle_us * 100 // total,
              hits, hits + misses))
    return frames

async def play(emu):
    redraw = True
    last = 0
    while True:
        rv = emu.run(redraw=redraw)
        keys_back()
        redraw = False

        if rv == core.RUN_SAVE:
            # TODO (phase 3): write the .sav file
            emu.saved()
            continue

        last = log_stats(emu, last)
        screen_back()

        if rv == core.RUN_ERROR:
            code, addr = emu.error()
            await ux_show_story('Emulation stopped: error %d at 0x%04x.' % (code, addr),
                                title='Game Boy')
            return

        if rv == core.RUN_MENU:
            if await pause_menu(emu):
                return
            redraw = True

async def start_gameboy(*a):
    from actions import file_picker

    fn = await file_picker(suffix='.gb', min_size=0x8000, max_size=8*1024*1024,
                           none_msg='Copy a Game Boy ROM (.gb file) to the card.')
    if not fn:
        return

    if hasattr(core, 'sim_setup'):
        import sys
        core.sim_setup(int(sys.argv[1]), int(sys.argv[2]))

    emu = core.Emulator()
    try:
        dis.fullscreen('Loading...')
        try:
            title, ram_size, slots = load_rom(emu, fn)
        except Exception as exc:
            await show_rom_error(fn, exc)
            return

        emu.keymap(DEFAULT_KEYMAP)
        emu.video(core.VIDEO_15_SMOOTH, PALETTES[0][1], BORDER)

        await play(emu)
    finally:
        emu.close()
        screen_back()
        gc.collect()

# EOF
