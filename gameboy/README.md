# Game Boy emulator for COLDCARD Q

Unofficial fork of [Coldcard/firmware](https://github.com/Coldcard/firmware)
that adds a Game Boy (DMG) emulator to the Q, under **Advanced/Tools > Game Boy**.
The emulator is [Peanut-GB](https://github.com/deltabeard/Peanut-GB) in C.

## Where things are

| Path | What |
|---|---|
| `external/c-modules/gameboy/` | C module `_gameboy`: core, video, HALs, MicroPython bindings |
| `external/c-modules/gameboy/peanut_gb.h` | Peanut-GB, vendored unmodified at `d0bcca77` (2026-09-21, v1.3.0) |
| `shared/gameboy.py` | ROM picker, loading, pause menu |
| `shared/flow.py` | menu hooks (two `MenuItem` lines + a lazy-import wrapper) |
| `unix/simulator.py` | numpy blit for raw pixel messages, and reads big display updates in full |
| `gameboy/test/` | headless test runner (host gcc) |
| `gameboy/scripts/build-sim.sh` | builds the simulator with the flags GCC 15 needs |
| `gameboy/scripts/fetch-test-roms.sh` | downloads pinned, freely licensed test ROMs to `gameboy/roms/` |
| `gameboy/docs/` | phase reports |

## Tests

```
cd gameboy/test
make test
```

This fetches the test ROMs, then runs Blargg's `cpu_instrs` (with a 6-slot
and a 1-slot bank cache) and `instr_timing`, and renders `dmg-acid2` through
the full video path and compares it with the reference image.

Other runner commands:

```
build/gbtest shot ROM FRAMES MODE OUT.png [grey|green]   # MODE: 1x 1.5x 1.5x-sharp 2x
build/gbtest bench ROM FRAMES
```

## Simulator

```
gameboy/scripts/build-sim.sh
cp gameboy/roms/*.gb unix/work/MicroSD/
cd unix && ./simulator.py --q1
```

Then go to Advanced/Tools > Game Boy. Esc (CANCEL) opens the pause menu.
When it opens, a line with the frame rate goes to the console.

On a machine without a display, `gameboy/test/sim-xvfb.sh start` runs the
simulator on Xvfb with the console in `/tmp/gb-sim/console.log`. Use
`gameboy/test/simdrive.py` to press keys and save LCD screenshots.

## Licenses

The firmware is MIT plus the Commons Clause (see `LICENSE`): it may be shared
but not sold. Peanut-GB is MIT, with parts from SameBoy (MIT). Test ROMs are
downloaded, not committed. Never commit commercial ROMs or their saves
(`gameboy/.gitignore` blocks `*.gb`, `*.gbc`, `*.sav`).
