# Game Boy on a COLDCARD Mk4

**Status: runs on a Mk4 dev unit with no funds on it.** The plain release
round trip (below) passed on a Mk4 on 2026-10-28, and the first Game Boy
build ran games on it the same day.

The Q lesson in `install-q.md` applies here too: firmware that fails
before the PIN screen can't be removed in the field. So only flash Mk4
builds made with `gameboy/scripts/build-mk4.sh`, in Coinkite's Docker
image.

master includes the Mk4 release tag `2026-09-30T2026-v5.6.3`, so Mk4
builds come from released code.

## The screen

The Mk4 has a 128x64 one-bit OLED, and the Game Boy screen is 160x144: it
can't show all of it at a readable size. So it shows one **view**:

| View | What you see |
|---|---|
| Top | lines 0-79, all 160 columns, scaled to 4/5 |
| Middle (default) | lines 32-111, scaled to 4/5 |
| Bottom | lines 64-143, scaled to 4/5 |
| Whole | the whole screen at 4/9, 71x64 in the middle of the OLED |
| Zoom | 128x64 at full size, from the middle of the screen |

Top, Middle and Bottom keep the picture's proportions and show more than
half of it. Where pixels merge, the darkest wins, so thin dark lines such
as text survive. In most games, Bottom shows the text box and Middle the
player. Whole is too small to read text, but shows where everything is.
It averages each pixel's area, weighted towards the darkest pixel so
outlines stay visible. Zoom is sharp, but shows the least.

**Tap** the view key (2) for the next view. **Hold** it and press the
D-pad to move the view: up and down in Top, Middle and Bottom, any way in
Zoom. The game doesn't see the keys while you hold the view key. The pause
menu also has **View**, and the last view used is saved; where you moved
it to isn't.

**Shades** sets how the four Game Boy shades become on/off pixels:

| Shades | Result |
|---|---|
| Two-Tone (default) | light shades off, dark shades on |
| Dithered | four levels, as a 2x2 dither pattern |
| Inverse | Two-Tone, reversed |
| Dither Inverse | Dithered, reversed |

## Controls

```
 1 Start     2 View     3 B
 4 Select    5 Up       6 A
 7 Left      8 Down     9 Right
 X Menu      0 Fast     OK -
```

The D-pad is on the arrow keys, as in the firmware's menus. Fast-forward
runs while you hold 0. X opens the pause menu. Change the controls in the
pause menu under **Controls**. If you never changed them, settings from
an earlier build move to this layout by themselves.

The keypad is a 4x3 matrix: if you hold three keys at the corners of a
rectangle, it reads the fourth key as held too. No layout of 11 controls
on 12 keys avoids this. Of all layouts with the D-pad on the arrow keys,
this one has the fewest such combos: they all need a diagonal to the
right (Up+Right or Down+Right) with A. If you change a control and the
change adds such a combo, the firmware lists it and asks you to keep or
undo the change.

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

## Build

```
gameboy/scripts/build-mk4.sh
```

This builds the committed HEAD in Docker (Alpine, GCC 11), signs it with
key zero, and writes `stm32/gameboy-mk4.dfu`. The Docker step also
compares the build with the official release and reports a mismatch;
that is expected for this fork.

## Install and go back

1. Copy `gameboy-mk4.dfu` and the official 5.6.3 DFU to a microSD card.
2. Log in, then **Advanced/Tools > Upgrade Firmware > From MicroSD**, and
   pick `gameboy-mk4.dfu`. Accept the EXTERNAL CONTRIBUTOR warning.
3. Every boot then shows the custom-firmware warning and a delay before
   the PIN screen.
4. To go back, install the official DFU the same way and accept the
   downgrade warning.

The Game Boy code only runs when you pick **Advanced/Tools > Game Boy**.
Nothing starts it at boot, so if a game hangs or the keys don't respond,
unplug the Mk4 and plug it back in: it boots to the PIN screen as usual,
and Upgrade Firmware is still there.
