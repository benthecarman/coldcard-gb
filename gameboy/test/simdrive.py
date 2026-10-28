#!/usr/bin/env python3
#
# simdrive.py - drive the desktop simulator running on an X server (Xvfb):
# press keys through XTEST and grab the simulated LCD.
#
#   simdrive.py key NAME [HOLD_MS]      # X keysym name: Return, Escape, Down, k ...
#   simdrive.py keys NAME,NAME... [HOLD_MS]   # hold several at once
#   simdrive.py shot OUT.png            # save the 320x240 LCD
#
# Needs python-xlib (pip install python-xlib) and ImageMagick's `import`.
# Uses $DISPLAY. The simulator window opens at (100, 100) and its LCD is at
# (95, 96) inside it, see LCDSimulator.TOPLEFT in unix/simulator.py.
#
import subprocess, sys, time

LCD_X, LCD_Y = 100 + 95, 100 + 96


def press(names, hold_ms):
    from Xlib import X, XK, display
    from Xlib.ext import xtest

    d = display.Display()

    # keyboard focus follows the pointer when there's no window manager
    xtest.fake_input(d, X.MotionNotify, x=LCD_X + 160, y=LCD_Y + 120)

    codes = []
    for name in names:
        sym = XK.string_to_keysym(name)
        if not sym:
            sys.exit('unknown key: ' + name)
        codes.append(d.keysym_to_keycode(sym))

    for code in codes:
        xtest.fake_input(d, X.KeyPress, code)
    d.sync()
    time.sleep(hold_ms / 1000)
    for code in reversed(codes):
        xtest.fake_input(d, X.KeyRelease, code)
    d.sync()


def shot(path):
    subprocess.check_call(['import', '-window', 'root',
                           '-crop', '320x240+%d+%d' % (LCD_X, LCD_Y), '+repage', path])


def main():
    cmd = sys.argv[1]
    if cmd == 'key':
        press([sys.argv[2]], int(sys.argv[3]) if len(sys.argv) > 3 else 80)
    elif cmd == 'keys':
        press(sys.argv[2].split(','), int(sys.argv[3]) if len(sys.argv) > 3 else 80)
    elif cmd == 'shot':
        shot(sys.argv[2])
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
