#!/usr/bin/env bash
#
# Build COLDCARD Mk4 firmware with the Game Boy emulator, signed with key
# zero (the "external contributor" key every Mk4 accepts), as
# stm32/gameboy-mk4.dfu.
#
# The build runs in Coinkite's reproducible-build Docker image (Alpine,
# GCC 11), the toolchain their releases are built and checked with. It
# builds the committed HEAD, not the working tree.
#
# Do not flash firmware built with the host compiler (see
# docs/install-q.md for why).
#
# Needs: docker, and the venv in ENV/ (for signit). The first time, run
# `make -f MK-Makefile setup` in stm32/ (see unix/README.md for the venv).
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
make -f MK-Makefile repro

# The repro step also compares the build with the official release of
# this version; that comparison fails for this fork, as it should.
cp built/dev.dfu gameboy-mk4.dfu

signit check gameboy-mk4.dfu
sha256sum gameboy-mk4.dfu
