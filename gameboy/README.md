# Game Boy emulator for COLDCARD Q and Mk4

Unofficial fork of [Coldcard/firmware](https://github.com/Coldcard/firmware)
that adds a Game Boy (DMG) emulator to the Q and the Mk4, under
**Advanced/Tools > Game Boy**.
The emulator is [Peanut-GB](https://github.com/deltabeard/Peanut-GB) in C.

master is based on the Q release tag `2026-09-30T2025-v1.5.3Q` and includes
the Mk4 release tag `2026-09-30T2026-v5.6.3`, not upstream master, so the
firmware is built from released code.

The Mk4 port is ready for a first test on a dev unit; see
`docs/mk4.md`.

## Where things are

| Path | What |
|---|---|
| `external/c-modules/gameboy/` | C module `_gameboy`: core, LCD and OLED video, HALs, MicroPython bindings |
| `external/c-modules/gameboy/hal_q1.c`, `hal_mk4.c`, `hal_stm32.c` | hardware: Q screen and keys, Mk4 screen and keys, and the clock and ROM storage both share |
| `external/c-modules/gameboy/gb_link.c` | the other Game Boy on the link cable, for Pokemon trades |
| `external/c-modules/gameboy/peanut_gb.h` | Peanut-GB `d0bcca77` (2026-09-21, v1.3.0), with a faster background and window drawer; changes are marked `CCGB:` |
| `shared/gameboy.py` | ROM picker, loading, saves, pause menu, settings, trade menu |
| `shared/gbtrade.py` | Pokemon trade codes (QR text) and reading the party from WRAM; Q only |
| `shared/manifest_q1.py`, `manifest_mk4.py` | freeze `gameboy.py` (and on the Q `gbtrade.py`) into the firmware |
| `shared/flow.py` | menu hooks (two `MenuItem` lines + a lazy-import wrapper) |
| `unix/simulator.py` | numpy blit for raw pixel messages, and reads big display updates in full |
| `gameboy/test/` | headless test runner (host gcc), and `gbpy.py` to drive the core from Python |
| `gameboy/bench/` | instruction counts on an emulated Cortex-M4 (QEMU) |
| `gameboy/scripts/build-q1.sh` | builds the Q firmware in Coinkite's Docker image and signs it with key zero (`stm32/gameboy-q.dfu`); read `docs/install-q.md` before flashing |
| `gameboy/scripts/build-mk4.sh` | the same for the Mk4 (`stm32/gameboy-mk4.dfu`); read `docs/mk4.md` first |
| `gameboy/scripts/build-sim.sh` | builds the simulator with the flags GCC 15 needs |
| `gameboy/scripts/fetch-test-roms.sh` | downloads pinned, freely licensed test ROMs to `gameboy/roms/` |
| `gameboy/docs/` | design notes, Q install guide, Mk4 notes |

## Tests

```
cd gameboy/test
make test
```

This fetches the test ROMs, then runs Blargg's `cpu_instrs` (with a 6-slot
and a 1-slot bank cache) and `instr_timing`, renders `dmg-acid2` through
the full video path and compares it with the reference image, and checks the
OLED renderer and the key ghost logic.

Pokemon trades need a ROM that can't be in the repo:

```
make trade ROM=path/to/red.gb
```

This needs Pokemon Red (USA, Europe). It plays two games, has them swap
trade codes, and trades at the Cable Club: a wrong Pokemon is refused,
then a Kadabra and a Machoke are traded and both evolve. Then it checks
the save and a reset. Screenshots go to `build/trade/`.

Other runner commands:

```
build/gbtest shot ROM FRAMES MODE OUT.png [PALETTE]
    # MODE: 1x 1.5x 1.5x-sharp 2x oled-fill oled-fit
    # PALETTE: grey green mono inverse
build/gbtest frame IN.pgm MODE OUT.png [PALETTE]   # a 160x144 picture, no ROM
build/gbtest bench ROM FRAMES
```

## Speed bench

```
cd gameboy/bench
make run ROM=../roms/tobu.gb
make profile
```

This runs the core bare-metal on QEMU's Cortex-M4 board with the firmware's
compiler flags, and prints the instructions per frame (in thousands) for
emulation and screen work in each screen mode. The Q runs 2.0M cycles per
frame (120 MHz at 59.7 fps), and each instruction takes about 1.3 to 2
cycles, so 650K instructions per frame is roughly half the CPU. `profile`
lists where the instructions go, by function and source line, for one
mode (`MODE=N`, an index into `MODES` in `bench.c`). Needs
`qemu-system-arm` and `arm-none-eabi-gcc`.

## Simulator

```
gameboy/scripts/build-sim.sh
cp gameboy/roms/*.gb unix/work/MicroSD/
cd unix && ./simulator.py --q1     # or --mk4
```

Then go to Advanced/Tools > Game Boy. Esc (CANCEL, or X on the Mk4) opens
the pause menu.
When it opens, a line with the frame rate goes to the console.

On a machine without a display, `gameboy/test/sim-xvfb.sh start` runs the
simulator on Xvfb with the console in `/tmp/gb-sim/console.log`. Use
`gameboy/test/simdrive.py` to press keys and save screenshots (`shot` for
the Q's LCD, `oled` for the Mk4).

## Licenses

The firmware is MIT plus the Commons Clause (see `LICENSE`): it may be shared
but not sold. Peanut-GB is MIT, with parts from SameBoy (MIT). Test ROMs are
downloaded, not committed. Never commit commercial ROMs or their saves
(the root `.gitignore` blocks `*.gb`, `*.gbc`, `*.sav` and states everywhere).
