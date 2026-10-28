# Install on a COLDCARD Q

This firmware is for a **dev unit with no funds on it**. It is signed with
key zero, the key Coinkite publishes for custom builds. The Q accepts it, but
shows a warning on every boot.

## Read this first

Custom firmware that fails before the PIN screen can't be removed. The
bootloader starts any correctly signed image. Its microSD recovery mode only
reinstalls the image that was already installed, and retail units have no
USB DFU or debug port (`stm32/mk4-bootloader/main.c`, `sdcard.c`,
`docs/dev-access.md`). Official firmware can only be installed from a
firmware that reaches the PIN screen.

On 2026-10-28 a build of this tree made with the host compiler (GCC 15, from
upstream master) boot-looped a Q on the warning screen, and the Q could not
be recovered. So:

- Only flash firmware built with `gameboy/scripts/build-q1.sh`. It builds
  in Coinkite's Docker image, from the release tag this branch is based on.
- On a new unit, first flash a key-zero build of the *unmodified* release,
  and check that it boots, logs in, and can go back to official firmware.
  Only then flash the Game Boy build.

## Build

One-time setup (see also `unix/README.md` for the venv):

```
cd stm32
make -f Q1-Makefile setup
```

Then commit your changes and run:

```
gameboy/scripts/build-q1.sh
```

This builds HEAD in Docker (Alpine, GCC 11), signs it with key zero, and
writes `stm32/gameboy-q.dfu`. The script prints `signit check` (model `Q1`,
`pubkey_num: 0`) and the file's SHA-256. The Docker step also compares the
build with the official release and reports a mismatch; that is expected for
this fork.

To check the toolchain, build the unmodified release tag the same way
(`make -f Q1-Makefile repro` in a clean clone at the tag). It must report
"bit-for-bit identical" with Coinkite's release.

## Load

1. Copy `gameboy-q.dfu` to a microSD card. Also keep the official firmware
   on the card for recovery (from coldcard.com; check its hash against
   `releases/signatures.txt`).
2. On the Q: log in (a main PIN must be set; no seed is needed), then
   **Advanced/Tools > Upgrade Firmware > From MicroSD**, and pick
   `gameboy-q.dfu`.
3. The Q warns that the firmware is "signed by an EXTERNAL CONTRIBUTOR".
   Accept it to install. After that, every boot shows a custom-firmware
   warning and a delay.

To go back, install the official DFU the same way.

## Play

1. Copy a `.gb` ROM to the card; its `.sav` goes next to it, with the same
   name. To bring a save from another emulator or a cartridge dumper, give
   it the ROM's name with `.sav`.
2. **Advanced/Tools > Game Boy**, then pick the ROM.
3. CANCEL opens the pause menu: Resume, save and load state, screen scale,
   palette, controls, **Speed**, and quit.

Default controls:

| Game Boy | Q keys |
|---|---|
| D-pad | W A S D, or the arrow keys |
| A | K |
| B | J |
| Start | ENTER |
| Select | SPACE |
| Pause menu | CANCEL |
| Fast-forward (hold) | O |

## Saves

- The game's own save (cart RAM) goes to `NAME.sav` when the game saves,
  when the pause menu opens, and on quit. It is plain cart RAM, the format
  other emulators and cartridge tools use.
- The first write of each session moves the old file to `NAME.sav.bak`, so
  you always have the save from before the session. Writes go through
  `NAME.sav.tmp` first, so an interrupted write can't corrupt the save.
- Save states (`NAME.state`) only work with the firmware build that wrote
  them.

## What to report back

Open **Speed** in the pause menu after a minute of play. It shows the game
and screen frame rates, how the CPU time splits between emulation, LCD and
idle, the bank cache hit rate, the ROM load time and free memory. Please
note these for each screen scale. Also report any tearing, missed or stuck
keys, and anything that crashes.

If the emulator crashes, the Q shows the usual error screen. The Game Boy
code only runs when you pick it in the menu, so a crash can't stop the Q
from booting or reaching Upgrade Firmware.
