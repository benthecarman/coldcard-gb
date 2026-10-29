# gameboy.py - Game Boy (DMG) emulator, for the Q and the Mk4.
#
# Emulation runs in C (external/c-modules/gameboy); this file picks the
# ROM, loads it, keeps the .sav file and save state, and runs the pause
# menu between calls to Emulator.run().
#
import gc, utime, uos, version
import _gameboy as core
from glob import dis, settings
from ux import ux_show_story, the_ux
from menu import MenuSystem, MenuItem
from files import CardSlot, CardMissingError
from uasyncio import sleep_ms

# Bank cache: 16K buffers of switchable ROM banks kept in SRAM
CACHE_SLOTS = const(6)
CHUNK = const(16384)

# settings key: {'v': video mode index, 'p': palette index, 'k': keymap list}
SETTINGS_KEY = 'gb'

# the desktop simulator, see unix/variant/touch.py
SIM = hasattr(core, 'sim_setup')

# Mk4: 128x64 one-bit OLED and a 12-key keypad. Q: colour LCD, keyboard.
MK4 = not version.has_qwerty

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

# Keymap: two keys per button, in this order: A B SELECT START RIGHT LEFT
# UP DOWN, then the menu and fast-forward keys.
BUTTONS = ('A', 'B', 'Select', 'Start', 'Right', 'Left', 'Up', 'Down')
FUNC_MENU = const(8)
FUNC_FAST = const(9)
POS_MENU = const(16)
POS_FAST = const(17)
FUNC_POS = {FUNC_MENU: POS_MENU, FUNC_FAST: POS_FAST}

if not MK4:
    FUNCS = BUTTONS + ('Menu', 'Fast-forward')

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

    # key numbers are row * 10 + col of the keyboard matrix
    ROW_KEYS = 10
    NUM_KEYS = 60

    KEY_NAMES = {0: 'NFC', 1: 'TAB', 2: 'QR', 3: 'LEFT', 4: 'UP', 5: 'DOWN',
                 6: 'RIGHT', 7: 'CANCEL', 8: 'ENTER', 50: 'LAMP', 51: 'SHIFT',
                 52: 'SPACE', 53: 'SYM', 54: 'DEL'}
    OK_NAME, CANCEL_NAME = 'ENTER', 'CANCEL'
    KEYBOARD = 'The Q keyboard'

    VIDEO_LABEL = 'Screen'
    VIDEO_MODES = (
        ('1x', core.VIDEO_1X),
        ('1.5x Smooth', core.VIDEO_15_SMOOTH),
        ('1.5x Sharp', core.VIDEO_15_SHARP),
        ('2x Cropped', core.VIDEO_2X_CROP),
    )
    DEFAULT_VIDEO = 1

    PALETTE_LABEL = 'Palette'
    PALETTES = (
        ('Green', (0xe0f8d0, 0x88c070, 0x346856, 0x081820)),
        ('Pea Soup', (0x9bbc0f, 0x8bac0f, 0x306230, 0x0f380f)),
        ('Grey', (0xffffff, 0xaaaaaa, 0x555555, 0x000000)),
    )
    SCREEN_NAME = 'LCD'
else:
    FUNCS = BUTTONS + ('Menu', 'Fast-forward')

    # key numbers are positions in mempad.py's DECODER: row * 3 + col
    MK4_KEYS = 'y0x987654321'
    mk = MK4_KEYS.index

    # D-pad on the arrow keys: 5 up, 7 left, 8 down, 9 right, as in the
    # firmware's menus. With the D-pad there, this layout has the fewest
    # matrix ghosts of all: they all need a diagonal to the right, with A.
    DEFAULT_KEYMAP = bytes([
        mk('6'), NO,        # A
        mk('3'), NO,        # B
        mk('4'), NO,        # Select
        mk('1'), NO,        # Start
        mk('9'), NO,        # Right
        mk('7'), NO,        # Left
        mk('5'), NO,        # Up
        mk('8'), NO,        # Down
        mk('x'),            # menu
        mk('0'),            # fast-forward
    ])
    del mk

    ROW_KEYS = 3
    NUM_KEYS = 12

    KEY_NAMES = {0: 'OK', 2: 'X'}
    OK_NAME, CANCEL_NAME = 'OK', 'X'
    KEYBOARD = 'The keypad'

    # the whole Game Boy screen either way
    VIDEO_LABEL = 'Screen'
    VIDEO_MODES = (
        ('Fill', core.VIDEO_OLED_FILL),
        ('Keep Shape', core.VIDEO_OLED_FIT),
    )
    DEFAULT_VIDEO = 0

    # On the OLED a shade lights up as many pixels as its brightness says:
    # white and black are sharp, the greys come out dithered.
    PALETTE_LABEL = 'Shades'
    PALETTES = (
        ('Two-Tone', (0xffffff, 0xffffff, 0x000000, 0x000000)),
        ('Dithered', (0xffffff, 0xaaaaaa, 0x555555, 0x000000)),
        ('Inverse', (0x000000, 0x000000, 0xffffff, 0xffffff)),
        ('Dither Inverse', (0x000000, 0x555555, 0xaaaaaa, 0xffffff)),
    )
    SCREEN_NAME = 'OLED'

BORDER = 0x000000

def key_name(kn):
    if kn == NO:
        return 'none'
    if MK4:
        return KEY_NAMES.get(kn) or MK4_KEYS[kn]
    from charcodes import DECODER       # Q keyboard only
    return KEY_NAMES.get(kn) or DECODER[kn].upper()

def func_keys(km, f):
    # keys mapped to function f (index into FUNCS)
    if f < len(BUTTONS):
        return [k for k in km[2*f:2*f+2] if k != NO]
    k = km[FUNC_POS[f]]
    return [k] if k != NO else []

def keymap_ok(km):
    return (len(km) == core.KEYMAP_BYTES and km[POS_MENU] != NO
            and all(k == NO or 0 <= k < NUM_KEYS for k in km))

# Both keyboards are matrices without diodes: holding three keys at three
# corners of a rectangle makes the fourth corner read as pressed too.

def ghosts(held):
    # keys the matrix reports as down, beyond those really held
    R = ROW_KEYS
    out = set(held)
    grow = True
    while grow:
        grow = False
        for p in list(out):
            for q in list(out):
                if q == p or q // R != p // R:
                    continue
                # p and q share a row: a key under q puts a ghost under p
                for s in list(out):
                    if s % R == q % R and s // R != q // R:
                        g = (s // R) * R + p % R
                        if g not in out:
                            out.add(g)
                            grow = True
    return out - set(held)

def ghost_problems(km):
    # Button combos people hold that would also press another game key.
    # Returns lines of text; empty if none. The menu key only
    # counts when held alone, so ghosts of it are harmless.
    owner = {}
    for f in range(len(FUNCS)):
        if f == FUNC_MENU:
            continue
        for k in func_keys(km, f):
            owner[k] = f

    R, L, U, D = 4, 5, 6, 7
    combos = []
    for dirs in ([R], [L], [U], [D], [U, R], [U, L], [D, R], [D, L]):
        for extra in ([], [0], [1], [0, 1]):
            combos.append(dirs + extra)
            if km[POS_FAST] != NO:
                combos.append(dirs + extra + [FUNC_FAST])
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
            # none yet, or from a build with other controls and screen
            # modes: its screen choice meant something else too
            km = bytearray(DEFAULT_KEYMAP)
            self.video = DEFAULT_VIDEO
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
        self.frames_seen = 0
        self.speed = None           # see speed_report()
        self.load_info = ''
        self.title = ''
        self.dest = 0               # header 0x14a: 0 is Japan

        # Pokemon trade (see trade_menu): our name and the Pokemon we
        # give, and their name and Pokemon, both as gbtrade bytes
        self.give = None
        self.get = None
        self.traded = False
        self.refused_seen = 0

    def load(self):
        # Copy ROM file into emulator memory (PSRAM on Q), then .sav.
        emu = self.emu
        with CardSlot(readonly=True) as card:
            size = uos.stat(self.fn)[6]
            self.rom_size = size

            # the ROM goes in PSRAM: anything staged there is gone (see auth.py)
            import glob
            if getattr(glob, 'PSRAM', None):
                glob.PSRAM.psram_write_count += 1
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
                    if pos == 0 and n > 0x14a:
                        self.dest = buf[0x14a]
                    emu.rom_write(pos, memoryview(buf)[0:n])
                    pos += n
                    dis.progress_bar_show(pos / size)

            del buf
            gc.collect()

            title, self.ram_size, slots = emu.start(CACHE_SLOTS)
            self.title = title
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
            with card.open(tmp, 'wb') as fd:
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

    def can_trade(self):
        # Pokemon trades by QR code: the Q has the scanner (and gbtrade.py)
        if not version.has_qr:
            return False
        import gbtrade
        return gbtrade.can_trade(self.title, self.dest)

    def trade_link(self):
        # Plug the trade partner in once both Pokemon are known. Only
        # when they change: this starts the partner over. After a trade
        # it stays, until a reset: the game keeps talking to it.
        if self.give and self.get:
            self.emu.link_trade(self.get[0], self.get[1], self.give[1])
        else:
            self.emu.link_trade(None)

    def trade_clear(self):
        self.give = self.get = None
        self.traded = False
        self.refused_seen = 0
        self.trade_link()

    def write_state(self):
        emu = self.emu
        with CardSlot() as card:
            self.check_card()

            tmp = self.state + '.tmp'
            with card.open(tmp, 'wb') as fd:
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
    # Q: keys held according to the firmware's keyboard code
    from glob import numpad
    return bytes(kn for kn, dn in enumerate(numpad.is_pressed) if dn)

def keys_held():
    # bit mask of keys held right now
    if MK4:
        # mempad.py tracks one key at a time; enough to wait for all up.
        # (The simulator's keypad never sets it: presses come as events.)
        from glob import numpad
        return 1 if numpad.key_pressed else 0
    if SIM:
        m = 0
        for kn in py_keys():
            m |= 1 << kn
        return m
    return core.keys()

def before_run():
    from glob import numpad
    if MK4:
        if not SIM:
            # mempad.py scans from a timer and column interrupts; they
            # would fight the C scan. Set the bus up for the OLED.
            numpad.timer.deinit()
            for c in numpad.cols:
                c.irq(None)
            dis.dis._setup_spi()
        else:
            core.sim_keys(b'')
        return

    # The game draws straight to the LCD; take the SPI bus from the GPU.
    dis.gpu.take_spi()
    if SIM:
        core.sim_keys(py_keys())

def after_run():
    from glob import numpad
    if MK4:
        if not SIM:
            from machine import Pin
            for c in numpad.cols:
                c.irq(numpad.anypress_irq, Pin.IRQ_FALLING | Pin.IRQ_RISING)
            numpad._wait_any()
    elif SIM:
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

def wrap_px(text, font):
    # Mk4: split text into lines that fit the OLED's width in this font
    lines, cur = [], ''
    for word in text.split():
        trial = (cur + ' ' + word) if cur else word
        if cur and dis.width(trial, font) > dis.WIDTH:
            lines.append(cur)
            cur = word
        else:
            cur = trial
    if cur:
        lines.append(cur)
    return lines

def prompt(msg, line2=None):
    # Show a message, with a smaller hint under it, while waiting for a key
    if not MK4:
        dis.fullscreen(msg, line2=line2)
        return

    # the Mk4's fullscreen() is one line of large text; these need more
    from zevvpeep import FontSmall, FontTiny
    dis.clear()
    y = 0
    for ln in wrap_px(msg, FontSmall):
        dis.text(0, y, ln, font=FontSmall)
        y += FontSmall.height
    y += 3
    for ln in wrap_px(line2 or '', FontTiny):
        dis.text(0, y, ln, font=FontTiny)
        y += FontTiny.height + 1
    dis.show()

async def capture_key(msg, line2):
    # Show a prompt and return the number of the next key pressed.
    prompt(msg, line2)
    await keys_settle()
    if MK4:
        from glob import numpad
        while True:
            ch = await numpad.get()
            if ch and ch in MK4_KEYS:
                break
        await keys_settle()
        return MK4_KEYS.index(ch)
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

    if f == FUNC_FAST:
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
    elif f == FUNC_MENU:
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
        km[FUNC_POS[f]] = new[0] if new else NO

    # only warn about new problems: the Mk4's default layout has a few
    # ghosts already (12 keys for 11 controls)
    before = ghost_problems(old)
    problems = [p for p in ghost_problems(km) if p not in before]
    if problems:
        msg = ('%s reads some key combinations wrong. '
               'With these controls:\n\n' % KEYBOARD + '\n'.join(problems[:6]))
        if len(problems) > 6:
            msg += '\n(and %d more)' % (len(problems) - 6)
        msg += '\n\nPress %s to keep them, %s to undo.' % (OK_NAME, CANCEL_NAME)
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
    if game.can_trade():
        await trade_news(game)

    cursor = 0
    while True:
        items = [
            ('Resume', 'resume'),
            ('Save State', 'save_state'),
            ('Load State', 'load_state'),
            ('%s: %s' % (VIDEO_LABEL, VIDEO_MODES[cfg.video][0]), 'video'),
            ('%s: %s' % (PALETTE_LABEL, PALETTES[cfg.palette][0]), 'palette'),
            ('Controls', 'controls'),
            ('Speed', 'speed'),
            ('Reset Game', 'reset'),
            ('Quit Game', 'quit'),
        ]
        if game.can_trade():
            items.insert(3, (trade_label(game), 'trade'))
        idx = await menu_pick([l for l, _ in items], cursor=cursor)
        what = items[idx][1] if idx is not None else 'resume'
        cursor = idx or 0

        if what == 'resume':
            return False

        if what == 'quit':
            return await quit_ok(game)

        if what == 'reset':
            ch = await ux_show_story('Turn the Game Boy off and on again? Progress since '
                                     'the game last saved is lost.', title='Reset Game')
            if ch != 'y':
                continue
            emu.reset()
            # the partner starts over too; after a trade, it's gone
            if game.traded:
                game.trade_clear()
            elif game.can_trade():
                game.trade_link()
            return False

        if what == 'trade':
            await trade_menu(game)
            continue

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

        elif what == 'speed':
            await ux_show_story((game.speed or 'Play a bit first.') + '\n\n' + game.load_info,
                                title='Speed')
            continue

        cfg.save()
        cfg.apply(emu)

# Pokemon trades by QR code (Red, Blue, Yellow; see gbtrade.py)

TRADE_HELP = """\
Both players do this on their own COLDCARD.

1. Pick the Pokemon you will give. Your COLDCARD shows it as a QR code: let the other player scan it.

2. Scan the other player's code.

3. In the game, go to a Pokemon Center and talk to the woman at the right of the counter (Cable Club). Sit at the table and trade the Pokemon you picked.

4. After the trade, use Reset Game: in these games that is the only way out of the Trade Center.

The game needs you to have the Pokedex first. Only the Pokemon you picked can go, and only once."""

def trade_label(game):
    if game.traded:
        return 'Trade Pokemon: Done'
    if game.give and game.get:
        return 'Trade Pokemon: Ready'
    return 'Trade Pokemon'

async def trade_news(game):
    # Tell how the trade went since the last pause
    import gbtrade
    _, traded, refused = game.emu.link_status()

    if traded and not game.traded:
        game.traded = True
        await ux_show_story('You gave %s and got %s. The game saved your party.\n\n'
                            'Use Reset Game to leave the Trade Center.'
                            % (gbtrade.label(game.give[1]), gbtrade.label(game.get[1])),
                            title='Trade Done')
    elif refused > game.refused_seen:
        game.refused_seen = refused
        await ux_show_story("The trade was canceled: that isn't the Pokemon you promised. "
                            "Trade %s." % gbtrade.label(game.give[1]), title='Trade')

async def trade_show(game):
    import gbtrade
    from ux import show_qr_code
    name, mon = game.give
    await show_qr_code(gbtrade.encode(name, mon), is_alnum=True, msg=gbtrade.label(mon),
                       allow_nfc=False)

async def trade_pick(game):
    # Pick the Pokemon to give, from the party in the game's memory
    import gbtrade
    got = gbtrade.read_party(game.emu.wram())
    if not got:
        await ux_show_story("Can't find your party. Start or continue a game, with at "
                            "least one Pokemon.", title='Trade')
        return False
    name, party = got

    chosen = None
    if game.give:
        for i, m in enumerate(party):
            if gbtrade.same(m, game.give[1]):
                chosen = i
    idx = await menu_pick([gbtrade.label(m) for m in party], chosen=chosen)
    if idx is None:
        return False

    game.give = (name, party[idx])
    return True

async def trade_scan(game):
    # Scan the other player's code; True if one came in
    import gbtrade
    from ux_q1 import QRScannerInteraction

    problem = None
    while True:
        # BBQr would collect in PSRAM, over the ROM: take it as text
        got = await QRScannerInteraction.scan("Scan the other player's code", line2=problem,
                                              bbqr_ok=False)
        if got is None:
            return False
        try:
            game.get = gbtrade.decode(got)
            return True
        except ValueError as exc:
            problem = str(exc)

async def trade_menu(game):
    import gbtrade

    while True:
        if game.traded:
            await ux_show_story('This trade is done. Use Reset Game to leave the Trade '
                                'Center; then you can trade again.', title='Trade')
            return

        items = [('Give: ' + (gbtrade.label(game.give[1]) if game.give else 'pick one'), 'give')]
        if game.give:
            items.append(('Show My Code', 'show'))
        items.append(('Get: ' + (gbtrade.label(game.get[1]) if game.get else 'scan code'), 'get'))
        items.append(('How To Trade', 'help'))
        if game.give or game.get:
            items.append(('Cancel Trade', 'cancel'))

        idx = await menu_pick([l for l, _ in items])
        if idx is None:
            return
        what = items[idx][1]
        before = (game.give, game.get)

        if what == 'give':
            if await trade_pick(game):
                await trade_show(game)
        elif what == 'show':
            await trade_show(game)
        elif what == 'get':
            if await trade_scan(game):
                await ux_show_story('%s offers %s.' % (gbtrade.text(game.get[0]),
                                                      gbtrade.label(game.get[1])),
                                    title='Trade')
        elif what == 'help':
            await ux_show_story(TRADE_HELP, title='How To Trade')
        elif what == 'cancel':
            game.trade_clear()

        if (game.give, game.get) == before:
            continue
        game.trade_link()
        if game.give and game.get:
            await ux_show_story('You give %s, %s gives %s.\n\nNow, in the game, go to the '
                                'Cable Club in a Pokemon Center.'
                                % (gbtrade.label(game.give[1]), gbtrade.text(game.get[0]),
                                   gbtrade.label(game.get[1])), title='Ready to Trade')

async def quit_ok(game):
    # Last chance to write the .sav. Returns True to quit.
    while game.needs_save():
        dis.fullscreen('Saving...')
        try:
            game.write_sav()
            break
        except Exception as exc:
            ch = await ux_show_story("Couldn't write the save file.\n\n%s\n\n"
                                     "Press %s to try again, or %s to quit "
                                     "and lose progress since the last save."
                                     % (err_text(exc), OK_NAME, CANCEL_NAME),
                                     title='Game Boy')
            if ch != 'y':
                break
    return True

def speed_report(game):
    # Frame rates and time split since the last call (the pause menu's
    # Speed screen shows the latest); also printed to the console.
    frames, shown, skipped, groups, hits, misses, emu_us, lcd_us, idle_us = \
        game.emu.stats(True)
    n = frames - game.frames_seen
    game.frames_seen = frames
    total = emu_us + lcd_us + idle_us
    if not (n and total):
        return

    pct = lambda t: t * 100 // total
    lookups = hits + misses
    game.speed = ('Game: %.1f fps\nScreen: %.1f fps (%d of %d frames)\n\n'
                  'Emulation %d%%, %s %d%%, idle %d%%\n'
                  'Bank cache hits: %d%% of %d' % (
                  n * 1e6 / total, shown * 1e6 / total, shown, n,
                  pct(emu_us), SCREEN_NAME, pct(lcd_us), pct(idle_us),
                  (hits * 100 // lookups) if lookups else 100, lookups))
    print('gameboy: ' + game.speed.replace('\n\n', '; ').replace('\n', '; '))

async def play(game, cfg):
    emu = game.emu
    redraw = True
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

        speed_report(game)
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
        core.sim_setup(int(sys.argv[1]), int(sys.argv[2]), MK4)

    emu = core.Emulator()
    try:
        dis.fullscreen('Loading...')
        game = Game(emu, fn)
        try:
            t0 = utime.ticks_ms()
            title = game.load()
            gc.collect()
            game.load_info = 'ROM: %s, %dK, loaded in %d ms\nFree memory: %dK' % (
                title, game.rom_size // 1024, utime.ticks_diff(utime.ticks_ms(), t0),
                gc.mem_free() // 1024)
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
