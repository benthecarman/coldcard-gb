#!/usr/bin/env bash
#
# Build COLDCARD Q firmware with the Game Boy emulator, signed with key
# zero (the "external contributor" key every Q accepts), as
# stm32/gameboy-q.dfu.
#
# The build runs in Coinkite's reproducible-build Docker image (Alpine,
# GCC 11), the toolchain their releases are built and checked with. It
# builds the committed HEAD, not the working tree.
#
# Do not flash firmware built with the host compiler. A GCC 15 build of
# this tree boot-looped a Q before the PIN screen, and a Q can't be
# recovered from that in the field (see docs/install-q.md).
#
# Needs: docker, and the venv in ENV/ (for signit). The first time, run
# `make -f Q1-Makefile setup` in stm32/ (see unix/README.md for the venv).
#
set -euo pipefail

cd "$(dirname "$0")/../../stm32"
export PATH="$PWD/../ENV/bin:$PATH"

if ! git diff --quiet --ignore-submodules=dirty HEAD; then
    echo "Uncommitted changes: the Docker build only sees HEAD. Commit first." >&2
    exit 1
fi

mkdir -p built
rm -f built/dev.dfu
make -f Q1-Makefile repro

# The repro step also compares the build with the official release of
# this version; that comparison fails for this fork, as it should.
cp built/dev.dfu gameboy-q.dfu

signit check gameboy-q.dfu
sha256sum gameboy-q.dfu
