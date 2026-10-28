#!/usr/bin/env bash
#
# Build the desktop simulator (unix/coldcard-mpy) with the extra flags this
# toolchain (GCC 15, NixOS) needs. Arguments go to make, e.g. `clean`.
#
#  - GCC 15 promotes some warnings in old MicroPython code to errors.
#  - MicroPython's x64 assembly NLR (py/nlrx64.c) breaks under GCC 15 at
#    -O0: every raised exception crashes. The setjmp NLR works.
#
set -euo pipefail

# NixOS session variables (pkg-config paths for libffi etc.) for shells
# that didn't get them; the file reads unset vars
if [ -f /etc/set-environment ]; then set +u; source /etc/set-environment; set -u; fi

cd "$(dirname "$0")/../../unix"
make CFLAGS_EXTRA="-Wno-error=dangling-pointer -Wno-error=enum-int-mismatch \
    -Wno-unterminated-string-initialization -DMICROPY_NLR_SETJMP=1" "$@"
