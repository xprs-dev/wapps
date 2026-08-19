#!/usr/bin/env bash
# Build and run the APRS-IS live integration test against the real
# server. Requires internet access to rotate.aprs2.net:14580.
# Exits 0 on pass (or SKIP when offline), non-zero on failure.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
HAL="${HAL_DIR:-/home/brito/code/xprs/wapps/hal}"
CC="${CC:-cc}"
OUT="$(mktemp -d)/it_aprs"
"$CC" -O1 -I"$HAL" -o "$OUT" "$HERE/it_aprs_is.c"
"$OUT"
