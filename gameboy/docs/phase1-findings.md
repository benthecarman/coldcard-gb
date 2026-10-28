# Phase 1: COLDCARD Q hardware findings and Game Boy design plan

Base: `Coldcard/firmware` master at `a772156d` (2026-10-23), Q version string
`1.5.3Q`. All findings come from the repo (schematic `hardware/schematic-q1d.png`,
BOM `hardware/bom-q1d.xlsx`, and the driver code). Items marked **(verify on
hardware)** cannot be confirmed from source alone.

## 1. MCU, clock, memory

| Item | Finding | Source |
|---|---|---|
| MCU | STM32L4S5VIT6 (Cortex-M4F, LQFP100) | BOM U1, `mpconfigboard.mk` `CMSIS_MCU = STM32L4S5xx` |
| Clock | 120 MHz SYSCLK (8 MHz HSE, PLL M=2 N=60 R=2), set by bootloader | `mk4-bootloader/clocks.c`, `COLDCARD_Q1/clocks.c` |
| Flash | 2 MB internal. Firmware code region 16K ISR + 1392K text; 512K LFS settings fs | `COLDCARD_Q1/layout.ld` |
| Flash headroom | Official v1.5.3Q DFU is 1,175,861 bytes, limit is 1,441,792, so about 260 KB free | coldcard.com download size |
| SRAM | 640 KB (SRAM1+2+3 contiguous). 632 KB for firmware (top 8 KB is bootloader). Layout: .data/.bss, then MicroPython GC heap, 16 KB stack, 8 KB fs cache | `layout.ld`, `docs/memory-map.md` |
| PSRAM | **Yes: 8 MB ESP-PSRAM64H** (U7), quad-SPI on OCTOSPI1, memory-mapped at `0x9000_0000`. Quad read `0xEB`, 6 dummy cycles, **60 MHz** (prescaler 2). Set up by bootloader, not cached by the CPU | `mk4-bootloader/psram.c`, schematic U7 |
| PSRAM use | Lower 4 MB: `glob.PSRAM` scratch (PSBT, firmware-upgrade staging). Upper 4 MB: Virtual Disk (USB MSC ram disk). Both are idle while a game runs | `shared/psram.py`, `COLDCARD_Q1/psramdisk.c` |
| Watchdog | None on the MCU (only in the secure elements) | grep |

Implications:
- A ROM fits in PSRAM (Pokémon R/B = 1 MB). I will put it at the start of the lower half.
- `psram.py` says writes must be word-aligned, so ROM loading uses aligned 32-bit writes.
- Random PSRAM reads are slow. One byte costs a full QSPI command (cmd + addr + 6 dummy + data), about 250–300 ns or 30–40 CPU cycles. Fetching every opcode from PSRAM would use about 1/3 of the CPU. **Design: an SRAM bank cache** (see below).

## 2. Display

| Item | Finding | Source |
|---|---|---|
| Resolution | 320×240, landscape via MADCTL `0x60` (MV=1). The panel is native portrait | `q1-bootloader/lcd.c` |
| Controller | Sitronix ST7789-family (init comments say "ST7789SS"; the firmware names the driver `st7788`). The panel is on FFC J5 and is not in the main BOM | `lcd.c`, `st7788.py` |
| Interface | 4-wire SPI, write-only (no MISO): SCLK PA5, MOSI PA7, CS PA4, D/C PA8, RESET PA6, **TEAR PB11** (TE output enabled, `TEON`) | `pins.csv`, schematic J5 |
| SPI speed | SPI1 at **60 MHz** (APB2 120 MHz / 2, the hardware max) | `st7788.py`, `lcd.c` |
| Pixel format | RGB565, 16 bpp, sent MSB first (code byte-swaps) | `lcd.c` COLMOD `0x05`, `modlcd.c` |
| Panel refresh | FRCTR2 = `0x0F` (60 Hz in ST7789 tables). Code comments measure TEAR at about 61 Hz | `lcd.c`, `keyboard.py` |
| DMA | MicroPython `spi_transfer()` uses DMA for large writes but **blocks until done**. `modlcd.c` uses it that way. The CPU and SPI do not overlap today | `ports/stm32/spi.c` @ `3d227fec` |
| Shared bus | The SPI pins are shared with a **"GPU" co-processor** (STM32C011, U10) that draws the busy bar and cursor. The main MCU must call `gpu.take_spi()` (G_CTRL high, wait for G_BUSY low) before driving the LCD | `shared/gpu.py`, `misc/gpu/README.md` |

Realistic full-frame rates at 60 Mbit/s (pixel data only; window commands add little):

| Mode | Bytes/frame | Transfer time | Max fps if every line changes |
|---|---|---|---|
| 1× (160×144) | 46,080 | 6.1 ms | ~160 |
| 1.5× (240×216) | 103,680 | 13.8 ms | ~70 (83% of the SPI budget at 59.7 fps) |
| 2× cropped (320×240 from 160×120 source) | 153,600 | 20.5 ms | ~48: **cannot do 60 fps full-frame**; needs frame skip or dirty lines |
| full screen 320×240 | 153,600 | 20.5 ms | ~48 |

So 1.5× at full speed is possible only if the SPI transfer overlaps emulation (async DMA). The blocking path leaves too little CPU time.

Tearing **(verify on hardware)**: the panel is native portrait and landscape comes from MV=1. The refresh sweep most likely moves across the screen horizontally in our view, not top to bottom. Row-order writes synced to TEAR may still tear. If this shows up, send pixels in column order (native scan order). That costs nothing extra with the scaler, because it can emit columns as easily as rows.

## 3. Keyboard

- **Hardware:** a 6 row × 10 column matrix on its own board via FFC. Rows PD8–PD12, PD7 (open-drain outputs). Columns PB0, PB1, PB2, PB5, PB8, PB9, PB10, PD13–PD15 (inputs with pull-ups). 1K series resistors, **no diodes, so ghosting is possible**: with 3 keys held at three corners of a rectangle, the 4th corner reads as pressed. Power button is separate (PB12). Long-press power-off is handled in hardware (XC6192).
- **Driver** (`shared/keyboard.py`): Python. A column edge interrupt starts scanning. Each full scan runs from the LCD TEAR interrupt (soft IRQ, about 61 Hz) and needs 3 matching samples (about 50 ms debounce). The output is a *character* event queue with a single "all keys up" event. It does **not** report per-key hold and release, so it cannot drive a game. Its latency is also 50 ms or more.
- **Plan:** the C module scans the matrix itself, once per emulated frame (60 GPIO ops, a few µs), and reports raw held state. The Python scanner's IRQs are detached during play and reattached on exit.
- **Available keys** (row, col): row 0 `NFC TAB QR LEFT UP DOWN RIGHT CANCEL(X) ENTER(OK)`; row 1 `1–0`; row 2 `q–p`; row 3 `a s d f g h j k l '`; row 4 `z x c v b n m , . /`; row 5 `LAMP SHIFT SPACE SYMBOL DELETE`. 55 keys. The arrows are in **one row**, not a cross, so WASD will feel better as a D-pad.
- **Simulator:** it already sends raw Q key state (up to 5 held key numbers per report) over a pipe, so held keys and releases work in the sim.

## 4. microSD

- Two slots (A top, B) muxed onto **SDMMC1, 4-bit** (TS3A27518E mux on `SD_MUX` PC13, detect pins PD3/PD4). MicroPython `pyb.SDCard` with DMA, FAT via oofatfs.
- Firmware access is `with CardSlot() as card:`, then normal `open()`/`read()`/`write()` under `/sd` (`shared/files.py`). There is a `file_picker()` helper in `shared/actions.py` with suffix and size filters. Q1 also has Virtual Disk (USB drive in PSRAM), so you could also copy ROMs over USB.
- **Large reads:** fine. Python reads 16 KB chunks and a C call copies each into PSRAM. 1 MB should take about 1–2 s **(verify on hardware)**.
- **Small writes:** fine. Python writes the cart-RAM buffer, exposed as a memoryview, to `name.sav`. The card is mounted only for each save, as the rest of the firmware does.
- No C-level FAT access is needed.

## 5. Audio

**None.** There is no speaker, buzzer or DAC output. PA4/PA5, the DAC pins on this MCU, are LCD CS/SCLK. The only "BEEPER" net is a test point on the QR-scanner module connector (J6), which the scanner module controls. Build with `ENABLE_SOUND=0`, which also saves CPU.

## Blockers and risks

**Blockers: none.** Decisions from review:

1. **Tooling:** packages go into Ben's NixOS config (`~/projects/nixos-config-dev-server/modules/development.nix`), not a project flake. Ben runs the switch.
2. **Repo layout:** this repo *is* the firmware fork. Upstream Coldcard is the `upstream` remote (push disabled), everything is on `master`, and tooling and docs live under `gameboy/`.

Risks (not blockers):

- **CPU budget (main risk).** Rough estimate per second: CPU core ~35–65 M cycles, PPU line renderer ~20–35 M, scaling/palette ~10 M. That is 65–110 M of 120 M. Peanut-GB's README claims full speed on an RP2040, but that is a different core and likely overclocked, so it is not proof for this chip. Mitigations, in order: `-O2` for the emulator files only; a ROM-read fast path (a small marked patch to `__gb_read` that uses a bank pointer instead of the per-byte callback); Peanut's built-in frame skip, which skips PPU work on alternate frames; dirty-line updates; hot functions in SRAM if flash wait states (5 WS at 120 MHz) hurt. Required behaviour stays: emulation at 59.73 Hz, display drops frames.
- **dmg-acid2.** Peanut-GB's README claims Blargg cpu_instrs and instr_timing, but says nothing about dmg-acid2. It does default `PEANUT_GB_HIGH_LCD_ACCURACY=1`, which handles sprite sorting and the 10-per-line limit. If it fails, the fix is a local, clearly marked patch (and could go upstream). Switching cores is the last resort.
- **SRAM for the emulator.** About 170 KB comes from the GC heap at game start (table below). Free heap on a running Q is unknown until Phase 4 (`gc.mem_free()`). The design degrades gracefully: fewer cache slots.
- **Keyboard ghosting.** Handled by key-map choice plus a validator and a "menu key must be the only key held" rule (details below).
- **2× cropped** cannot hit 60 fps when everything changes. It will show ~30–48 fps while emulation stays at full speed.
- **Battery life** at 100% CPU with the backlight on 3×AAA will be short. USB power is fine.

## Custom firmware procedure (Coinkite guide, 2026-09-23)

The guide is ["Build and Run Your Own COLDCARD Firmware"](https://blog.coinkite.com/build-run-custom-coldcard-firmware/). Page metadata says 2026-09-23; the blog index lists it under 2026-09-30. It was checked against master `948dc108`. For Q:

```
cd stm32
make clean
make -f Q1-Makefile setup
make -f Q1-Makefile firmware-signed.dfu     # signs with key zero by default
mv firmware-signed.dfu gameboy-q.dfu
signit check gameboy-q.dfu                  # confirm model, version, key #0, sig OK
sha256sum gameboy-q.dfu
```

To load: copy to microSD, unlock, then **Advanced/Tools > Upgrade Firmware > From MicroSD**. A main PIN must be set; do not create a seed. Keep the official 1.5.3Q DFU (hash in `releases/signatures.txt`) on the card for recovery. Expect a forced warning and delay on every boot. Since commit `5c13be46` (2026-09-28, newer than the guide), the install screen also says "signed by an EXTERNAL CONTRIBUTOR". Downgrade protection compares header timestamps, and `signit` stamps build time, so our builds pass.

Brick risk is low with this design. Nothing runs at boot or login. `gameboy.py` is imported only when the menu item is chosen, so a crash in the emulator cannot prevent reaching Upgrade Firmware.

## Design plan

### Where the code goes (minimal hooks)

New files:

```
external/c-modules/gameboy/        # built into both the Q1 firmware and the unix sim
  micropython.mk                   # adds sources; -O2 for these files only
  peanut_gb.h                      # vendored at a pinned commit; local patches marked /* CCGB: */
  gb_main.c                        # MicroPython bindings (module `_gameboy`) + run loop
  gb_cart.c                        # ROM bank cache (PSRAM -> SRAM), cart RAM, dirty tracking
  gb_video.c                       # palette, 1x/1.5x/2x scalers, dirty lines
  hal.h
  hal_q1.c                         # SPI1 async DMA to LCD, GPU handoff, matrix scan, DWT µs timer
  hal_unix.c                       # sim: pixels to display pipe, keys from key pipe, clock_gettime
shared/gameboy.py                  # UI: ROM picker, pause menu, settings, .sav I/O
```

Hooks into existing files:
- `shared/manifest_q1.py`: +1 line (`gameboy.py`).
- `shared/flow.py`: +1 `MenuItem('Game Boy', f=..., predicate=version.has_qwerty)` in `AdvancedPinnedVirginMenu` (the menu a dev unit with PIN and no seed sees) and in `AdvancedNormalMenu`.
- `unix/simulator.py`: a small fast-blit path. The existing raw-pixel mode unpacks one pixel at a time in Python and is too slow for live video.

Host-side test harness (`gameboy/test/`): a headless runner that builds `peanut_gb.h` + `gb_cart.c` + `gb_video.c` with a null HAL. It runs a ROM for N frames, then (a) checks for Blargg's "Passed" on the serial port/screen, and (b) dumps a PNG for dmg-acid2 and diffs it against the reference image. This makes the correctness gates fast and repeatable. A script fetches the test ROMs into an ignored directory; nothing commercial, no boot ROM.

### Memory plan

| Item | Where | Size |
|---|---|---|
| ROM image | PSRAM `0x9000_0000` | ROM size (1 MB for Pokémon R/B) |
| Bank 0 (fixed) | SRAM | 16 KB |
| Switchable-bank LRU cache | SRAM | 6 × 16 KB = 96 KB (fewer if the heap is tight) |
| `struct gb_s` (WRAM, VRAM, OAM, HRAM) | SRAM | ~17 KB |
| Cart RAM | SRAM | up to 32 KB (Pokémon: 32 KB, MBC3) |
| 2 GB framebuffers, 2-bit packed (dirty compare + DMA source) | SRAM | 2 × 5.6 KB |
| DMA line buffers (ping-pong) | SRAM | ~3 KB |

Total about 175 KB. It is allocated as Python-owned bytearrays at game start (after `gc.collect()`) and passed to C, so the GC keeps it alive between `run()` calls and frees it on quit. No permanent `.bss` cost to the wallet.

A bank switch is a pointer swap on a cache hit and a 16 KB copy from PSRAM (~1 ms) on a miss. Pokémon switches banks a few times per frame (audio engine in VBlank, farcalls) across a small working set, so the hit rate should be high. The cache logs hits and misses for tuning.

### Run loop

- Python calls `gameboy.run()`. C loops: scan keys, `gb_run_frame()`, present, pace to 59.73 Hz (16.743 ms) with the DWT cycle counter (no audio clock). It returns to Python only for: menu key, save pending, or error.
- Pacing uses its own timer, not TEAR. TEAR is about 61 Hz, which would run games 2% fast. TEAR is used only to time DMA start for tearing.
- **Saves.** C marks cart RAM dirty on write. When the game disables cart RAM (MBC RAMG = 0, which games do after saving), or 1 s after the last write, `run()` returns `SAVE`. Python writes `<rom>.sav` next to the ROM and resumes, a ~50 ms hitch. Also saved on quit. On SD errors, the dirty flag stays set and the pause menu shows a warning. RTC (MBC3 timer) is not needed for Red/Blue; it is a stretch item for Gold/Silver.
- **Display.** `lcd_draw_line` writes 2-bit pixels into the back framebuffer. At frame end, changed lines are found by comparing against the front buffer. Changed runs are sent with async SPI DMA. The DMA-complete IRQ converts the next chunk (palette LUT + scaler) into the free ping-pong buffer, so the CPU keeps emulating the next frame. If the previous frame's DMA is still running at frame end, that frame is skipped for display (emulation continues) and the dirty set accumulates.
- **Scalers.** 1× centered. 1.5×: 2 source px to 3 output px per axis, `[a, (a+b)/2, b]`; the centre pixel is the 4-way average. With 4 shades, every blend comes from a precomputed RGB565 LUT. 2× crop: pixel doubling, 160×120 source window centered (the crop position may become adjustable later).
- **Palette.** Default green-tint `#E0F8D0 #88C070 #346856 #081820`, plus a "DMG pea-soup" and a grey option.

### Controls

Defaults below; every button is rebindable in the pause menu, and settings are saved.

| GB | Default | Alternate preset |
|---|---|---|
| D-pad | WASD and the arrow keys | arrows only |
| A | K | X |
| B | J | Z |
| Start | ENTER | ENTER |
| Select | SPACE | SPACE |
| Pause/menu | CANCEL (X key) | CANCEL |
| (stretch) fast-forward | TAB | TAB |

Ghosting check of the default set: the only mapped-key ghost found is RIGHT + J + K giving CANCEL. The rule "CANCEL counts only when it is the only key held" covers it, because a ghost always needs 3 other keys held. A key-map validator runs on rebinding and warns when a mapping can produce a ghost on another mapped key.

Pause menu (normal firmware UX, rendered by Python): Resume, Save now, Scale (1×, 1.5× smooth, 1.5× sharp, 2× crop), Palette, Controls, Quit (saves, then returns to the firmware menu). On quit: release SPI back to the GPU, reattach keyboard IRQs, free buffers, redraw the menu.

### Phase gates

2. Sim: host harness passes `cpu_instrs` (all 11) and `dmg-acid2`; homebrew ROM runs in the simulator.
3. Sim: input, `.sav` round trip (homebrew with battery RAM), pause menu, settings.
4. Hardware: key-zero signed DFU, loaded via microSD; report fps, free heap, ROM load time.
5. Tuning: async DMA, dirty lines, cache stats, SRAM hot paths; report fps per scale mode.

## License notes (for publishing the fork)

- **Coldcard firmware** (`LICENSE`, `COPYING-CC`): MIT **plus Commons Clause**. You can publish and share the fork and DFU, but you may not "Sell" it: no fee-based product or service, including paid hosting or support, whose value comes substantially from this software. Keep the copyright notice **and the Commons Clause notice** in copies. This is source-available, not OSI open source.
- **Hardware files** (schematics, BOMs): Coinkite copyright, research and testing use only, no commercial licence. Don't redistribute them commercially. The fork doesn't need to touch them.
- **Peanut-GB:** MIT (© Mahyar Koshkouei), with parts from SameBoy (MIT, © Lior Halphon). Keep both notices in the vendored header.
- **MicroPython:** MIT.
- **Test ROMs:** dmg-acid2 is MIT. Blargg's tests have no explicit licence but are widely redistributed. Safest is to fetch them with a script and not commit them.
- **Never commit** commercial ROMs, `.sav` files from them, or a Nintendo boot ROM. Peanut-GB doesn't need one. Add `*.gb *.gbc *.sav` to `.gitignore`.
- **Trademarks:** "COLDCARD" belongs to Coinkite and "Game Boy"/"Pokémon" to Nintendo. A public fork should be clearly unofficial, e.g. not branded as a COLDCARD release, and must not ship ROMs. Every user also sees the "EXTERNAL CONTRIBUTOR" and custom-firmware warnings.
