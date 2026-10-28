#!/usr/bin/env bash
#
# Download the freely licensed test and homebrew ROMs used by the Game Boy
# tests into gameboy/roms/ (not committed). Every file is pinned to a
# commit or release and checked against a SHA-256.
#
# - cpu_instrs, instr_timing: Blargg's test ROMs (via retrio/gb-test-roms)
# - dmg-acid2 + reference image: Matt Currie, MIT
# - 2048.gb: Sanqui, zlib (via the gbdev Homebrew Hub database)
# - tobu.gb: Tobu Tobu Girl, Tangram Games, MIT (via the gbdev database)
#
set -euo pipefail

DEST="$(cd "$(dirname "$0")/.." && pwd)/roms"
mkdir -p "$DEST"

RAW=https://raw.githubusercontent.com
BLARGG=$RAW/retrio/gb-test-roms/c240dd7d700e5c0b00a7bbba52b53e4ee67b5f15
ACID=$RAW/mattcurrie/dmg-acid2/8a98ce731f96dde032ffb22ec36dc985d78fdb18
HHDB=$RAW/gbdev/database/94368e51470f14fe5c495e6e65c994f2c3b6c661/entries

# name  sha256  url
FILES=(
  "cpu_instrs.gb 8c5e12f41e0ba5bbca796944f92ffe6de28809198682c4332e38d1b3cf56fcf2 $BLARGG/cpu_instrs/cpu_instrs.gb"
  "instr_timing.gb 646067b3d6c79fda810e9c3f1cb7c0efd5abb0a7ac06437c54e65720c15d9925 $BLARGG/instr_timing/instr_timing.gb"
  "dmg-acid2.gb 464e14b7d42e7feea0b7ede42be7071dc88913f75b9ffa444299424b63d1dff1 https://github.com/mattcurrie/dmg-acid2/releases/download/v1.0/dmg-acid2.gb"
  "dmg-acid2-reference.png ca966d50895c7efef05838590d148c2cbfd7fba57dab986f25b35b4da71abb57 $ACID/img/reference-dmg.png"
  "2048.gb 3ea2376b15b34bd26b10e6b31d2753bf8b086098dbdb5ee84d9c0f03d66d3d7f $HHDB/2048gb/2048.gb"
  "tobu.gb 5d3871cae77db2287807e8914fd21f76203e73aa3fec20955489d45cb4571d8f $HHDB/tobutobugirl/tobu.gb"
)

for entry in "${FILES[@]}"; do
    read -r name sum url <<< "$entry"
    out="$DEST/$name"
    if [ -f "$out" ] && echo "$sum  $out" | sha256sum --quiet -c - 2>/dev/null; then
        continue
    fi
    echo "fetch $name"
    curl -sfL -o "$out.tmp" "$url"
    if ! echo "$sum  $out.tmp" | sha256sum --quiet -c -; then
        echo "checksum mismatch: $name" >&2
        rm -f "$out.tmp"
        exit 1
    fi
    mv "$out.tmp" "$out"
done

echo "ROMs ready in $DEST"
