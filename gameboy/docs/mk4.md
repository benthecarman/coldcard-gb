# Game Boy on a COLDCARD Mk4

**Status: simulator only. Do not flash the Mk4 Game Boy build yet.**

The Mk4 port runs in the desktop simulator (`--mk4`). The Mk4 hardware
code (`external/c-modules/gameboy/hal_mk4.c`) compiles but has not run on
a device. The Q lesson in `install-q.md` applies here too: firmware that
fails before the PIN screen can't be removed in the field.

master includes the Mk4 release tag `2026-09-30T2026-v5.6.3`, so Mk4
builds come from released code.

## The screen

The Mk4 has a 128x64 one-bit OLED, and the Game Boy screen is 160x144. So
the 160 columns are squashed to 128, and the OLED shows one **view**:

| View | Game Boy lines |
|---|---|
| Top | 0-63 |
| Middle (default) | 40-103 |
| Bottom | 80-143 |
| Whole | all 144, squashed to 64 |

Where pixels merge, the darkest wins, so thin dark lines such as text
survive. The view key (5) steps through the views while a game runs. The
pause menu also has **View**, and the last view used is saved.

**Shades** sets how the four Game Boy shades become on/off pixels:

| Shades | Result |
|---|---|
| Two-Tone (default) | light shades off, dark shades on |
| Dithered | four levels, as a 2x2 dither pattern |
| Inverse | Two-Tone, reversed |
| Dither Inverse | Dithered, reversed |

## Controls

```
 1 Select    2 Up       3 Start
 4 Left      5 View     6 Right
 7 Fast      8 Down     9 -
 X Menu      0 B        OK A
```

Fast-forward runs while you hold 7. X opens the pause menu. Change the
controls in the pause menu under **Controls**.

The keypad is a 4x3 matrix: if you hold three keys at the corners of a
rectangle, it reads the fourth key as held too. No layout of 11 controls
on 12 keys avoids this. With the default layout, it only happens when you
hold A and B together with Up, or with a direction and Fast-forward. If
you change a control and the change adds such a combo, the firmware lists
it and asks you to keep or undo the change.

## Simulator

```
gameboy/scripts/build-sim.sh
cp gameboy/roms/*.gb unix/work/MicroSD/
cd unix && ./simulator.py --mk4
```

Then go to Advanced/Tools > Game Boy. On a machine without a display, use
`gameboy/test/sim-xvfb.sh start --mk4 --eff` and
`gameboy/test/simdrive.py oled OUT.png` for screenshots.

The simulator's keypad sends one key at a time. When you hold two keys
and let go of one, the game still sees both as held until you let go of
both. The hardware scan doesn't do this.

## First step on a device: the plain release round trip

Before any Game Boy build goes on a Mk4, check that the unit takes a
key-zero build and goes back to official firmware. Use a key-zero build
of the *unmodified* 5.6.3 release from Coinkite's Docker image. It must
match the official release byte for byte, except for the signature:

```
git clone https://github.com/Coldcard/firmware.git && cd firmware
git checkout 2026-09-30T2026-v5.6.3
git submodule update --init
cd stm32 && make -f MK-Makefile setup && make -f MK-Makefile repro
```

The repro step must print "bit-for-bit identical copy of Coldcard
firmware for v5.6.3". The key-zero file is `stm32/built/dev.dfu`.

Install it with **Advanced/Tools > Upgrade Firmware > From MicroSD**, then
install the official 5.6.3 DFU the same way. The custom build has a later
timestamp, so the firmware warns about a downgrade on the way back;
accept it. Never use **Danger Zone > Set High-Water**: it makes the
bootloader refuse firmware older than the installed one.
