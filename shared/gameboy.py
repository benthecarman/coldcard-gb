# gameboy.py - Game Boy (DMG) emulator, Q only.
#
# Emulation runs in C (external/c-modules/gameboy); this file picks the
# ROM, loads it, keeps the .sav file and save state, and runs the pause
# menu between calls to Emulator.run().
#
import gc, utime, uos
import _gameboy as core
from glob import dis, settings
from ux import ux_show_story, the_ux
from menu import MenuSystem, MenuItem
from files import CardSlot, CardMissingError
from uasyncio import sleep_ms
from charcodes import DECODER

# Bank cache: 16K buffers of switchable ROM banks kept in SRAM
CACHE_SLOTS = const(6)
CHUNK = const(16384)

# settings key: {'v': video mode index, 'p': palette index, 'k': keymap list}
SETTINGS_KEY = 'gb'

# the desktop simulator, see unix/variant/touch.py
SIM = hasattr(core, 'sim_setup')

# Q key numbers (row * 10 + col), see shared/charcodes.py
KN_LEFT = const(3)
KN_UP = const(4)
KN_DOWN = const(5)
KN_RIGHT = const(6)
KN_CANCEL = const(7)
KN_ENTER = const(8)
KN_W = const(21)
KN_O = const(28)
KN_A = const(30)
KN_S = const(31)
KN_D = const(32)
KN_J = const(36)
KN_K = const(37)
KN_SPACE = const(52)

NO = core.NO_KEY

# Keymap: two Q keys per button, in this order: A B SELECT START RIGHT LEFT
# UP DOWN, then the menu key and the fast-forward key.
BUTTONS = ('A', 'B', 'Select', 'Start', 'Right', 'Left', 'Up', 'Down')
FUNCS = BUTTONS + ('Menu', 'Fast-forward')
POS_MENU = const(16)
POS_FAST = const(17)

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
    KN_O,
])

KEY_NAMES = {0: 'NFC', 1: 'TAB', 2: 'QR', 3: 'LEFT', 4: 'UP', 5: 'DOWN',
             6: 'RIGHT', 7: 'CANCEL', 8: 'ENTER', 50: 'LAMP', 51: 'SHIFT',
             52: 'SPACE', 53: 'SYM', 54: 'DEL'}

VIDEO_MODES = (
    ('1x', core.VIDEO_1X),
    ('1.5x Smooth', core.VIDEO_15_SMOOTH),
    ('1.5x Sharp', core.VIDEO_15_SHARP),
    ('2x Cropped', core.VIDEO_2X_CROP),
)
DEFAULT_VIDEO = const(1)

PALETTES = (
    ('Green', (0xe0f8d0, 0x88c070, 0x346856, 0x081820)),
    ('Pea Soup', (0x9bbc0f, 0x8bac0f, 0x306230, 0x0f380f)),
    ('Grey', (0xffffff, 0xaaaaaa, 0x555555, 0x000000)),
)
BORDER = 0x000000

def key_name(kn):
    if kn == NO:
        return 'none'
    return KEY_NAMES.get(kn) or DECODER[kn].upper()

def func_keys(km, f):
    # Q keys mapped to function f (index into FUNCS)
    if f < len(BUTTONS):
        return [k for k in km[2*f:2*f+2] if k != NO]
    k = km[POS_MENU if f == len(BUTTONS) else POS_FAST]
    return [k] if k != NO else []

def keymap_ok(km):
    return (len(km) == core.KEYMAP_BYTES and km[POS_MENU] != NO
            and all(k == NO or 0 <= k < 60 for k in km))

# The Q keyboard is a 6x10 matrix without diodes: holding three keys at
# three corners of a rectangle makes the fourth corner read as pressed too.

def ghosts(held):
    # keys the matrix reports as down, beyond those really held
    out = set(held)
    grow = True
    while grow:
        grow = False
        for p in list(out):
            for q in list(out):
                if q == p or q // 10 != p // 10:
                    continue
                # p and q share a row: a key under q puts a ghost under p
                for s in list(out):
                    if s % 10 == q % 10 and s // 10 != q // 10:
                        g = (s // 10) * 10 + p % 10
                        if g not in out:
                            out.add(g)
                            grow = True
    return out - set(held)

def ghost_problems(km):
    # Button combos people hold that would also press another game key.
    # Returns lines of text; empty if none. The menu key only counts when
    # held alone, so ghosts of it are harmless.
    owner = {}
    for f in range(len(FUNCS)):
        if f == len(BUTTONS):
            continue        # menu
        for k in func_keys(km, f):
            owner[k] = f

    R, L, U, D = 4, 5, 6, 7
    combos = []
    for dirs in ([R], [L], [U], [D], [U, R], [U, L], [D, R], [D, L]):
        for extra in ([], [0], [1], [0, 1]):
            combos.append(dirs + extra)
            if km[POS_FAST] != NO:
                combos.append(dirs + extra + [9])
    combos.append([0, 1, 2, 3])
    combos.append([2, 3])

    problems = []
    for combo in combos:
        choices = [func_keys(km, f) for f in combo]
        if not all(choices):
            continue

        # every way of holding the combo with the keys each button has
        held_sets = [[]]
        for opts in choices:
            held_sets = [h + [k] for h in held_sets for k in opts]

        for held in held_sets:
            for g in sorted(ghosts(held)):
                f = owner.get(g)
                if f is None or f in combo:
                    continue
                msg = '%s: also %s' % ('+'.join(FUNCS[x] for x in combo), FUNCS[f])
                if msg not in problems:
                    problems.append(msg)
    return problems

class Config:
    # Screen, palette and controls; shared by all games
    def __init__(self):
        d = settings.get(SETTINGS_KEY) or {}

        self.video = d.get('v', DEFAULT_VIDEO)
        if not (0 <= self.video < len(VIDEO_MODES)):
            self.video = DEFAULT_VIDEO

        self.palette = d.get('p', 0)
        if not (0 <= self.palette < len(PALETTES)):
            self.palette = 0

        try:
            km = bytearray(d.get('k'))
            assert keymap_ok(km)
        except:
            km = bytearray(DEFAULT_KEYMAP)
        self.keymap = km

    def save(self):
        settings.put(SETTINGS_KEY, dict(v=self.video, p=self.palette, k=list(self.keymap)))

    def apply(self, emu):
        emu.keymap(bytes(self.keymap))
        emu.video(VIDEO_MODES[self.video][1], PALETTES[self.palette][1], BORDER)

def err_text(exc):
    if isinstance(exc, CardMissingError):
        return 'There is no card in the slot.'
    return str(exc) or type(exc).__name__

def exists(fn):
    try:
        uos.stat(fn)
        return True
    except OSError:
        return False

def remove(fn):
    try:
        uos.remove(fn)
    except OSError:
        pass

class Game:
    # One loaded ROM and its files next to it on the card:
    #   NAME.sav       cart RAM (battery save), same format as other emulators
    #   NAME.sav.bak   the .sav as it was before this session's first write
    #   NAME.state     save state (only for this firmware build)
    def __init__(self, emu, fn):
        self.emu = emu
        self.fn = fn
        base = fn.rsplit('.', 1)[0]
        self.sav = base + '.sav'
        self.state = base + '.state'
        self.rom_size = 0
        self.ram_size = 0
        self.sav_tail = b''         # bytes after cart RAM in the .sav, kept
        self.backed_up = False
        self.save_pending = False   # a .sav write failed: try again later

    def load(self):
        # Copy ROM file into emulator memory (PSRAM on Q), then .sav.
        emu = self.emu
        with CardSlot(readonly=True) as card:
            size = uos.stat(self.fn)[6]
            self.rom_size = size
            emu.rom_begin(size)

            buf = bytearray(CHUNK)
            pos = 0
            with open(self.fn, 'rb') as fd:
                while pos < size:
                    n = fd.readinto(buf)
                    if not n:
                        raise OSError('short read')
                    if pos == 0 and n > 0x143 and buf[0x143] == 0xc0:
                        raise ValueError('This game needs a Game Boy Color.')
                    emu.rom_write(pos, memoryview(buf)[0:n])
                    pos += n
                    dis.progress_bar_show(pos / size)

            del buf
            gc.collect()

            title, self.ram_size, slots = emu.start(CACHE_SLOTS)
            if self.ram_size:
                self.read_sav()

        return title

    def read_sav(self):
        # Cart RAM from the .sav. Falls back to the files a write leaves
        # if it was cut short: .tmp is only renamed into place once whole.
        ram = self.emu.cart_ram()
        for fn in (self.sav, self.sav + '.tmp', self.sav + '.bak'):
            try:
                size = uos.stat(fn)[6]
            except OSError:
                continue
            if fn.endswith('.tmp') and size < self.ram_size:
                continue
            with open(fn, 'rb') as fd:
                n = fd.readinto(ram)
                # other emulators may add a clock footer: keep it
                self.sav_tail = fd.read() or b''
            print('gameboy: %s: %d bytes' % (fn, n))
            break

    def check_card(self):
        # Is the card with the ROM in? Don't write saves onto another card.
        try:
            ok = uos.stat(self.fn)[6] == self.rom_size
        except OSError:
            ok = False
        if not ok:
            raise OSError("The card with the game isn't in.")

    def write_sav(self):
        emu = self.emu
        with CardSlot() as card:
            self.check_card()

            tmp = self.sav + '.tmp'
            with open(tmp, 'wb') as fd:
                fd.write(emu.cart_ram())
                if self.sav_tail:
                    fd.write(self.sav_tail)

            if not self.backed_up and exists(self.sav):
                remove(self.sav + '.bak')
                uos.rename(self.sav, self.sav + '.bak')
            else:
                remove(self.sav)
            uos.rename(tmp, self.sav)
            self.backed_up = True

        emu.saved()
        self.save_pending = False

    def needs_save(self):
        return self.ram_size and (self.emu.dirty() or self.save_pending)

    async def save_or_complain(self):
        # Write the .sav if anything changed. Returns False if that failed.
        if not self.needs_save():
            return True
        try:
            self.write_sav()
            return True
        except Exception as exc:
            self.emu.saved()        # stop asking every frame; retried later
            self.save_pending = True
            screen_back()
            await keys_settle()
            await ux_show_story("Couldn't write the save file.\n\n%s\n\n"
                                "It will be tried again when you pause or quit."
                                % err_text(exc),
                                title='Game Boy')
            return False

    def write_state(self):
        emu = self.emu
        with CardSlot() as card:
            self.check_card()

            tmp = self.state + '.tmp'
            with open(tmp, 'wb') as fd:
                fd.write(emu.state_header())
                fd.write(emu.state_view())
                if self.ram_size:
                    fd.write(emu.cart_ram())
            remove(self.state)
            uos.rename(tmp, self.state)

    def read_state(self):
        emu = self.emu
        size = len(emu.state_view())
        with CardSlot(readonly=True) as card:
            try:
                fd = open(self.state, 'rb')
            except OSError:
                raise ValueError('No saved state for this game yet.')
            with fd:
                hdr = fd.read(len(emu.state_header()))
                if hdr != emu.state_header():
                    raise ValueError('The state was saved by another game '
                                     'or firmware version.')
                st = fd.read(size)
                ram = fd.read(self.ram_size) if self.ram_size else None

        if len(st) != size or (ram is not None and len(ram) != self.ram_size):
            raise ValueError('The state file is cut short.')

        emu.state_load(st, ram)

# Keys. While a game runs, C reads the keys; between runs the firmware's
# own keyboard code does. These hand over between the two.

def py_keys():
    # keys held according to the firmware's keyboard code
    from glob import numpad
    return bytes(kn for kn, dn in enumerate(numpad.is_pressed) if dn)

def keys_held():
    # bit mask of Q keys held right now
    if SIM:
        m = 0
        for kn in py_keys():
            m |= 1 << kn
        return m
    return core.keys()

def before_run():
    # The game draws straight to the LCD; take the SPI bus from the GPU.
    dis.gpu.take_spi()
    if SIM:
        core.sim_keys(py_keys())

def after_run():
    from glob import numpad
    if SIM:
        k = core.keys()
        for kn in range(len(numpad.is_pressed)):
            numpad.is_pressed[kn] = (k >> kn) & 1
    # don't let idle-logout think nobody's here
    numpad.last_event_time = utime.ticks_ms()

async def keys_settle():
    # Wait for all keys up, then drop what the keyboard code queued, so
    # keys meant for the game (or a key being set up) don't leak into menus.
    from glob import numpad
    while keys_held():
        await sleep_ms(20)
    await sleep_ms(100)     # the keyboard code debounces over 3 scans
    while not numpad.empty():
        numpad.get_nowait()
    numpad.clear_pressed()
    numpad.last_event_time = utime.ticks_ms()

async def capture_key(msg, line2):
    # Show a prompt and return the number of the next Q key pressed.
    dis.fullscreen(msg, line2=line2)
    await keys_settle()
    while True:
        k = keys_held()
        if k:
            break
        await sleep_ms(20)
    kn = 0
    while not (k >> kn) & 1:
        kn += 1
    await keys_settle()
    return kn

def screen_back():
    # Python's display code tracks what's on the LCD; the game drew over it.
    dis._full_redraw = True
    dis.clear()

async def menu_pick(labels, chosen=None, cursor=None):
    # Simple menu: returns the index picked, or None for CANCEL. chosen
    # gets a check mark; cursor just starts there.
    picked = []

    async def pick(_1, idx, _2):
        picked.append(idx)
        the_ux.pop()

    m = MenuSystem([MenuItem(l, f=pick) for l in labels], chosen=chosen)
    if cursor is not None:
        m.goto_idx(cursor)
    the_ux.push(m)
    await m.interact()

    return picked[0] if picked else None

async def set_func_keys(cfg, f):
    # Ask for new keys for FUNCS[f]. Returns False if nothing changed.
    km = cfg.keymap
    name = FUNCS[f]
    menu_key = km[POS_MENU]
    is_button = f < len(BUTTONS)

    if f == len(FUNCS) - 1:
        line2 = 'Hold it to run the game fast. Press the menu key (%s) for none.' \
                    % key_name(menu_key)
    else:
        line2 = None
    first = await capture_key('Press the key for %s' % name, line2)

    new = [first]
    if is_button:
        if first == menu_key:
            await ux_show_story('%s is the menu key. Pick another menu key first.'
                                % key_name(first), title='Controls')
            return False
        second = await capture_key('Press a 2nd key for %s' % name,
                                   'Or press %s again to use just one key.' % key_name(first))
        if second != first and second != menu_key:
            new.append(second)
    elif f == len(BUTTONS):
        pass        # menu: may take a key from a button, below
    elif first == menu_key:
        new = []    # fast-forward: none

    old = bytes(km)

    # a key does one thing: take it away from anything else
    for pos in range(len(km)):
        if km[pos] in new:
            km[pos] = NO

    if is_button:
        km[2*f] = new[0]
        km[2*f+1] = new[1] if len(new) > 1 else NO
    else:
        km[POS_MENU if f == len(BUTTONS) else POS_FAST] = new[0] if new else NO

    problems = ghost_problems(km)
    if problems:
        msg = ('The Q keyboard reads some key combinations wrong. '
               'With these controls:\n\n' + '\n'.join(problems[:6]))
        if len(problems) > 6:
            msg += '\n(and %d more)' % (len(problems) - 6)
        msg += '\n\nPress ENTER to keep them, CANCEL to undo.'
        ch = await ux_show_story(msg, title='Controls')
        if ch != 'y':
            km[:] = old
            return False

    return bytes(km) != old

async def controls_menu(cfg, emu):
    cursor = 0
    while True:
        km = cfg.keymap
        labels = ['%s: %s' % (FUNCS[f], ', '.join(key_name(k) for k in func_keys(km, f)) or 'none')
                  for f in range(len(FUNCS))]
        labels.append('Reset to Defaults')

        f = await menu_pick(labels, cursor=cursor)
        if f is None:
            return
        cursor = f

        if f == len(FUNCS):
            cfg.keymap = bytearray(DEFAULT_KEYMAP)
        elif not await set_func_keys(cfg, f):
            continue

        cfg.save()
        cfg.apply(emu)

async def pause_menu(game, cfg):
    # Returns True to quit.
    emu = game.emu
    await keys_settle()
    await game.save_or_complain()

    cursor = 0
    while True:
        items = [
            ('Resume', 'resume'),
            ('Save State', 'save_state'),
            ('Load State', 'load_state'),
            ('Screen: ' + VIDEO_MODES[cfg.video][0], 'video'),
            ('Palette: ' + PALETTES[cfg.palette][0], 'palette'),
            ('Controls', 'controls'),
            ('Quit Game', 'quit'),
        ]
        idx = await menu_pick([l for l, _ in items], cursor=cursor)
        what = items[idx][1] if idx is not None else 'resume'
        cursor = idx or 0

        if what == 'resume':
            return False

        if what == 'quit':
            return await quit_ok(game)

        if what in ('save_state', 'load_state'):
            dis.fullscreen('Saving...' if what == 'save_state' else 'Loading...')
            try:
                if what == 'save_state':
                    game.write_state()
                else:
                    game.read_state()
                return False
            except Exception as exc:
                await ux_show_story(err_text(exc), title='Game Boy')

        elif what == 'video':
            pick = await menu_pick([l for l, _ in VIDEO_MODES], chosen=cfg.video)
            if pick is not None:
                cfg.video = pick

        elif what == 'palette':
            pick = await menu_pick([l for l, _ in PALETTES], chosen=cfg.palette)
            if pick is not None:
                cfg.palette = pick

        elif what == 'controls':
            await controls_menu(cfg, emu)

        cfg.save()
        cfg.apply(emu)

async def quit_ok(game):
    # Last chance to write the .sav. Returns True to quit.
    while game.needs_save():
        dis.fullscreen('Saving...')
        try:
            game.write_sav()
            break
        except Exception as exc:
            ch = await ux_show_story("Couldn't write the save file.\n\n%s\n\n"
                                     "Press ENTER to try again, or CANCEL to quit "
                                     "and lose progress since the last save."
                                     % err_text(exc),
                                     title='Game Boy')
            if ch != 'y':
                break
    return True

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

async def play(game, cfg):
    emu = game.emu
    redraw = True
    last = 0
    while True:
        before_run()
        rv = emu.run(redraw=redraw)
        after_run()
        redraw = False

        if rv == core.RUN_SAVE:
            # the game saved: write the .sav, then straight back in
            if not await game.save_or_complain():
                redraw = True
            continue

        last = log_stats(emu, last)
        screen_back()

        if rv == core.RUN_ERROR:
            code, addr = emu.error()
            await game.save_or_complain()
            await ux_show_story('Emulation stopped: error %d at 0x%04x.' % (code, addr),
                                title='Game Boy')
            return

        if rv == core.RUN_MENU:
            if await pause_menu(game, cfg):
                return
            redraw = True

async def start_gameboy(*a):
    from actions import file_picker

    fn = await file_picker(suffix=['.gb', '.gbc'], min_size=0x8000, max_size=8*1024*1024,
                           none_msg='Copy a Game Boy ROM (.gb file) to the card.')
    if not fn:
        return

    if SIM:
        import sys
        core.sim_setup(int(sys.argv[1]), int(sys.argv[2]))

    emu = core.Emulator()
    try:
        dis.fullscreen('Loading...')
        game = Game(emu, fn)
        try:
            game.load()
        except Exception as exc:
            await ux_show_story('Unable to start %s\n\n%s' % (fn.split('/')[-1], err_text(exc)),
                                title='Game Boy')
            return

        cfg = Config()
        cfg.apply(emu)

        await keys_settle()
        await play(game, cfg)
    finally:
        emu.close()
        screen_back()
        gc.collect()

# EOF
