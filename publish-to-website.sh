#!/bin/sh
# =============================================================================
# publish-to-website.sh — mirror the built wapp catalog (binaries/) to the
# xprs.dev website repo so the in-app wapp store can fetch it from
# https://xprs.dev/wapps (no github.com dependency).
#
# Usage:
#   ./build-archive.sh                       # build + package all wapps first
#   ./publish-to-website.sh [SITE_DIR]       # mirror binaries/ -> SITE_DIR/wapps
#
# SITE_DIR defaults to ../website (the local xprs-dev/xprs-dev.github.io
# checkout, which serves xprs.dev). After running, commit & push the website
# repo.
# =============================================================================
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$SCRIPT_DIR/binaries"
SITE="${1:-$SCRIPT_DIR/../website}"
DEST="$SITE/wapps"

if [ ! -f "$SRC/index.json" ]; then
  echo "error: $SRC/index.json not found — run ./build-archive.sh first" >&2
  exit 1
fi
if [ ! -d "$SITE" ]; then
  echo "error: website dir not found: $SITE" >&2
  exit 1
fi

mkdir -p "$DEST"
if command -v rsync >/dev/null 2>&1; then
  rsync -a --delete --exclude 'README.md' "$SRC/" "$DEST/"
else
  # Fallback: wipe (keeping README.md) and copy.
  find "$DEST" -mindepth 1 -not -name 'README.md' -delete 2>/dev/null || true
  cp -r "$SRC/." "$DEST/"
fi

echo ">> mirrored $(grep -c '"file"' "$DEST/index.json") wapps to $DEST"
echo ">> now commit & push the website repo to deploy to xprs.dev/wapps"
