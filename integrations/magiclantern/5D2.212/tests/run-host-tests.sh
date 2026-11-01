#!/bin/sh
set -eu
if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
    printf '%s\n' 'Usage: run-host-tests.sh /absolute/sample.CR2 /absolute/sample.JPG [empty-workdir]' >&2
    exit 2
fi
tests=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH= cd -- "$tests/../../../.." && pwd)
case "$1:$2" in /*:/*) ;; *) printf '%s\n' 'Sample paths must be absolute.' >&2; exit 2;; esac
work=${3:-$(mktemp -d "${TMPDIR:-/tmp}/soup-5d2-test.XXXXXX")}
mkdir -p "$work"
if [ -n "$(ls -A "$work")" ]; then
    printf '%s\n' 'The host-test work directory must be empty.' >&2
    exit 2
fi
cc=${CC:-cc}
"$cc" -std=c99 -O1 -g -Wall -Wextra -DSOUP_HOST_TEST \
    -I"$tests" -I"$tests/../src" -I"$repo/src/engine" -I"$repo/src/vendor/magiclantern/src" \
    "$tests/test-5d2.c" "$tests/../src/film_develop.c" \
    "$tests/../src/film_transaction.c" "$tests/../src/film_tiff.c" \
    "$repo/src/vendor/magiclantern/src/crc32.c" \
    "$repo/src/engine/se.c" "$repo/src/engine/cr2.c" \
    "$repo/src/engine/jpeg_dec.c" "$repo/src/engine/jpeg_enc.c" \
    "$repo/src/engine/look.c" "$repo/src/engine/stock.c" \
    "$repo/src/engine/wb.c" "$repo/src/engine/color.c" \
    "$repo/src/engine/grade.c" "$repo/src/engine/lut.c" \
    -lm -o "$work/test-5d2"
"$work/test-5d2" "$1" "$2" "$repo/src/stocks/acros.fs" "$work"
printf '%s\n' "Host test files: $work"
