#!/usr/bin/env python3
#
# Host checks for the key matrix ghost logic in shared/gameboy.py, run
# under CPython with stand-ins for the firmware modules.
#
#   python3 test_keys.py
#
import builtins, os, sys, types

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '../../shared'))

builtins.const = lambda x: x

def stub(name, **attrs):
    m = types.ModuleType(name)
    m.__dict__.update(attrs)
    sys.modules[name] = m

stub('_gameboy', NO_KEY=0xff, KEYMAP_BYTES=19, VIDEO_1X=0, VIDEO_15_SMOOTH=1,
     VIDEO_15_SHARP=2, VIDEO_2X_CROP=3, VIDEO_OLED_TOP=16, VIDEO_OLED_MIDDLE=17,
     VIDEO_OLED_BOTTOM=18, VIDEO_OLED_FIT=19, VIDEO_OLED_ZOOM=20)
stub('glob', dis=None, settings=None)
stub('ux', ux_show_story=None, the_ux=None)
stub('menu', MenuSystem=None, MenuItem=None)
stub('files', CardSlot=None, CardMissingError=Exception)
stub('uasyncio', sleep_ms=None)
stub('utime')
stub('uos')

def load(qwerty):
    # gameboy.py picks its Q or Mk4 tables when imported
    stub('version', has_qwerty=qwerty)
    sys.modules.pop('gameboy', None)
    import gameboy
    return gameboy

gb = load(True)

def check(cond, msg):
    if not cond:
        sys.exit('FAIL: ' + msg)

# three corners of a rectangle: W(2,1) S(3,1) D(3,2) -> E(2,2) = 22
check(gb.ghosts([21, 31, 32]) == {22}, 'basic ghost')
check(gb.ghosts([21, 32]) == set(), 'two keys never ghost')
check(gb.ghosts([30, 31, 32, 36, 37]) == set(), 'one row never ghosts')

# defaults must be clean (ghosts of the menu key are fine)
problems = gb.ghost_problems(gb.DEFAULT_KEYMAP)
check(problems == [], 'default keymap: %r' % problems)
check(gb.keymap_ok(gb.DEFAULT_KEYMAP), 'default keymap valid')

# a bad map is caught: A=Q(2,0) B=W(2,1) Down=A(3,0) -> S(3,1)
km = bytearray(gb.DEFAULT_KEYMAP)
km[0], km[1] = 20, 0xff
km[2], km[3] = 21, 0xff
km[14], km[15] = 30, 0xff       # Down = 'a'
km[10], km[11] = 32, 0xff       # Left = 'd', away from 's'
km[12], km[13] = 31, 0xff       # Up = 's'
problems = gb.ghost_problems(km)
check(any('Down+A+B: also Up' in p for p in problems), 'bad map: %r' % problems)

names = [gb.key_name(k) for k in (3, 7, 8, 21, 39, 52, 0xff)]
check(names == ['LEFT', 'CANCEL', 'ENTER', 'W', "'", 'SPACE', 'none'], repr(names))

# Mk4: 4x3 keypad, key number = position in 'y0x987654321'
gb = load(False)
mk = gb.MK4_KEYS.index
check(gb.ghosts([mk('6'), mk('3'), mk('2')]) == {mk('5')}, 'mk4 basic ghost')
check(gb.keymap_ok(gb.DEFAULT_KEYMAP), 'mk4 default keymap valid')
check(len(gb.DEFAULT_KEYMAP) == 19 and gb.DEFAULT_KEYMAP[gb.POS_VIEW] == mk('2'), 'mk4 view key')

# D-pad on the arrow keys
dpad = [gb.DEFAULT_KEYMAP[2 * f] for f in range(4, 8)]
check(dpad == [mk('9'), mk('7'), mk('5'), mk('8')], 'mk4 D-pad: %r' % dpad)

# the default layout's ghosts all need a diagonal to the right, with A
problems = gb.ghost_problems(gb.DEFAULT_KEYMAP)
check(len(problems) == 8 and
      all(p.startswith(('Up+Right+A', 'Down+Right+A')) for p in problems),
      'mk4 default keymap: %r' % problems)

# settings with the old default get the new one; others are kept
saved = {}
gb.settings = types.SimpleNamespace(get=lambda k: saved.get(k))
saved['gb'] = dict(v=1, p=0, k=list(gb.OLD_DEFAULT_KEYMAPS[0]))
check(bytes(gb.Config().keymap) == gb.DEFAULT_KEYMAP, 'mk4 old default keymap not replaced')
mine = bytearray(gb.DEFAULT_KEYMAP)
mine[0] = mk('y')
saved['gb'] = dict(v=1, p=0, k=list(mine))
check(gb.Config().keymap == mine, 'mk4 own keymap not kept')

names = [gb.key_name(k) for k in (mk('y'), mk('x'), mk('5'), 0xff)]
check(names == ['OK', 'X', '5', 'none'], repr(names))

print('key tests passed')
