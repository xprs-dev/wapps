#!/bin/sh
# Native integration test for the circles wapp: a real sqlite-backed mock HAL
# drives module_handle_event through the folder flow (open / add / enter / post).
# Not part of the wasm build — run manually:  sh tests/native/run.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../.."
SQLIB="$(ldconfig -p | grep -m1 'libsqlite3.so' | awk '{print $NF}')"
cc -O0 -g -I"$SRC" -I"$SRC/../hal" -Wno-attributes \
  "$HERE/test_folders.c" "$HERE/hal_mock.c" \
  "$SRC/main.c" "$SRC/util.c" "$SRC/db.c" "$SRC/circle.c" \
  "$SQLIB" -o /tmp/circles_cttest
exec /tmp/circles_cttest
