#!/bin/sh
# Build local experimental files only; never copy anything to a camera/card.
set -eu
src=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
repo=$(dirname "$src")
overlay="$repo/integrations/magiclantern/5D2.212"
ml="$src/vendor/magiclantern"
pin=$(awk '/^Commit / { print $2; exit }' "$overlay/SOUP-PIN.txt")
: "${ARM_BINPATH:?Set ARM_BINPATH to the directory containing arm-none-eabi-gcc}"
test -x "$ARM_BINPATH/arm-none-eabi-gcc"
if [ ! -d "$ml/.git" ]; then
    mkdir -p "$ml"
    git -C "$ml" init -q
    git -C "$ml" remote add origin https://github.com/reticulatedpines/magiclantern_simplified
    git -C "$ml" fetch --depth 1 origin "$pin"
    git -C "$ml" checkout --detach FETCH_HEAD
fi
if [ "$(git -C "$ml" rev-parse HEAD)" != "$pin" ]; then
    printf '%s\n' 'Wrong Magic Lantern revision; use the pinned checkout from SOUP-PIN.txt.' >&2
    exit 1
fi
cp "$overlay/platform/5D2.212/Makefile" "$ml/platform/5D2.212/Makefile"
cp "$overlay/src/film_menu.c" "$overlay/src/film_develop.c" "$ml/src/"
if [ "$(uname -s)" = Darwin ]; then
    set -- 'STAT_CMD=stat -f "%N: %z bytes"'
else
    set --
fi
make -C "$ml/platform/5D2.212" \
    "ARM_BINPATH=$ARM_BINPATH" "READELF=$ARM_BINPATH/arm-none-eabi-readelf" \
    "$@" build/autoexec.bin build/5D2_212.sym
printf '%s\n' "Experimental build: $ml/platform/5D2.212/build/autoexec.bin" \
    "Matching symbols: $ml/platform/5D2.212/build/5D2_212.sym" \
    'Not validated on camera. No card files were changed.'
