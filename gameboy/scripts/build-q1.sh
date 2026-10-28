#!/usr/bin/env bash
#
# Build COLDCARD Q firmware with the Game Boy emulator, signed with key
# zero (the "external contributor" key every Q accepts), as
# stm32/gameboy-q.dfu. Arguments go to make, e.g. `clean`.
#
# Needs: arm-none-eabi-gcc, and the venv in ENV/ (for signit). The first
# time, run `make -f Q1-Makefile setup` in stm32/ (see unix/README.md for
# the venv).
#
set -euo pipefail

cd "$(dirname "$0")/../../stm32"
export PATH="$PWD/../ENV/bin:$PATH"

# GCC 15 (newer than the official build's) promotes some warnings in old
# MicroPython code to errors. From the environment, so the board's own
# CFLAGS_EXTRA additions still apply.
export CFLAGS_EXTRA="-Wno-error=dangling-pointer -Wno-error=enum-int-mismatch \
    -Wno-unterminated-string-initialization"

if [ $# -gt 0 ]; then
    exec make -f Q1-Makefile "$@"
fi

make -f Q1-Makefile -j"$(nproc)"
rm -f firmware-signed.bin firmware-signed.dfu
make -f Q1-Makefile firmware-signed.dfu
mv firmware-signed.dfu gameboy-q.dfu

signit check gameboy-q.dfu
sha256sum gameboy-q.dfu
