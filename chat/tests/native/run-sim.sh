#!/bin/sh
# Multi-node chat simulator: many instances of the REAL wapp in one process,
# wired to each other through a stand-in core (sim.c installs hal_mock.c's
# network hooks). Tests 1:1, closed groups and Local end to end, no device.
#   sh tests/native/run-sim.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../.."
SQLIB="$(ldconfig -p | grep -m1 'libsqlite3.so' | awk '{print $NF}')"
rm -rf /tmp/chat_sim
cc -O0 -g -I"$SRC" -I"$SRC/../hal" -Wno-attributes -Wall \
  "$HERE/sim.c" "$HERE/hal_mock.c" \
  "$SRC/main.c" "$SRC/room.c" "$SRC/db.c" "$SRC/thread.c" "$SRC/xprs.c" \
  "$SQLIB" -o /tmp/chat_sim_bin
exec /tmp/chat_sim_bin
