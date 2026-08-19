#!/bin/sh
# Native integration test for the mesh wapp: a canned mock HAL drives
# module_tick / module_handle_event and asserts the emitted messages.
# Not part of the wasm build — run manually:  sh tests/native/run.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../.."
cc -O0 -g -I"$SRC" -I"$SRC/../hal" -Wno-attributes \
  "$HERE/test_mesh.c" "$HERE/hal_mock.c" "$SRC/main.c" \
  -o /tmp/mesh_cttest
exec /tmp/mesh_cttest
