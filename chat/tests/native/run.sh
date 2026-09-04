#!/bin/sh
# Native tests for the chat wapp: a real sqlite-backed mock HAL drives the
# whole module (module_init / module_handle_event) and room.c directly.
# Not part of the wasm build — run manually:  sh tests/native/run.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../.."
SQLIB="$(ldconfig -p | grep -m1 'libsqlite3.so' | awk '{print $NF}')"
rm -rf /tmp/chat_native_test
cc -O0 -g -I"$SRC" -I"$SRC/../hal" -Wno-attributes -Wall \
  "$HERE/test_room.c" "$HERE/hal_mock.c" \
  "$SRC/main.c" "$SRC/room.c" "$SRC/db.c" "$SRC/thread.c" "$SRC/xprs.c" \
  "$SQLIB" -o /tmp/chat_native_test_bin
exec /tmp/chat_native_test_bin
