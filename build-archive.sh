#!/bin/sh
# Build every wapp source folder in this repo and package each as a
# .wapp ZIP under binaries/. Wapp source folders are top-level
# directories containing a manifest.json (any other top-level dir
# — sdk/, hal/, modules/, binaries/ — is skipped automatically).
#
# Usage:
#   ./build-archive.sh              # build and package all
#   ./build-archive.sh maps         # build and package one
#   ./build-archive.sh clean        # remove binaries/
#
# Output layout:
#   binaries/
#     maps/
#       maps-1.0.0.wapp
#     terminal/
#       terminal-1.0.0.wapp
#     index.json              ← version index for update detection
#
# Environment:
#   WASI_SDK_PATH  — path to wasi-sdk (default: ~/wasi-sdk)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WAPPS_DIR="$SCRIPT_DIR"
OUTPUT_DIR="$SCRIPT_DIR/binaries"
WASI_SDK_PATH="${WASI_SDK_PATH:-$HOME/wasi-sdk}"

export WASI_SDK_PATH

# Verify wasi-sdk
if [ ! -x "$WASI_SDK_PATH/bin/clang" ]; then
    echo "wasi-sdk not found at $WASI_SDK_PATH"
    echo "Run: ./install-wasi-sdk.sh"
    exit 1
fi

# Clean mode
if [ "${1:-}" = "clean" ]; then
    echo "Removing $OUTPUT_DIR..."
    rm -rf "$OUTPUT_DIR"
    echo "Done."
    exit 0
fi

mkdir -p "$OUTPUT_DIR"

# Read a JSON string field from a file: json_field <file> <key>
json_field() {
    grep -o "\"$2\"[[:space:]]*:[[:space:]]*\"[^\"]*\"" "$1" \
        | head -1 | sed "s/.*\"$2\"[[:space:]]*:[[:space:]]*\"//;s/\"//"
}

# Build and package a single wapp directory.
build_wapp() {
    dir="$1"
    name=$(basename "$dir")

    [ -f "$dir/Makefile" ] || return 1
    [ -f "$dir/manifest.json" ] || return 1

    version=$(json_field "$dir/manifest.json" version)
    [ -z "$version" ] && version="0.0.0"

    echo "[$name] compiling..."
    if ! make -C "$dir" --no-print-directory 2>&1; then
        echo "[$name] FAILED to compile"
        return 1
    fi

    [ -f "$dir/app.wasm" ] || { echo "[$name] no app.wasm after build"; return 1; }

    # Build tests.wasm if a tests/ folder is present. Test failures
    # don't break the package — they leave tests.wasm out so the
    # consumer just gets "no tests" when running them. See
    # wapp-interfaces.md §20.
    if [ -d "$dir/tests" ]; then
        echo "[$name] compiling tests..."
        if ! make -C "$dir" --no-print-directory tests 2>&1; then
            echo "[$name] tests FAILED — packaging without tests.wasm"
            rm -f "$dir/tests.wasm"
        fi
    fi

    mkdir -p "$OUTPUT_DIR/$name"
    wapp_file="$OUTPUT_DIR/$name/$name-$version.wapp"
    echo "[$name] packaging $name-$version.wapp..."
    rm -f "$wapp_file"

    # ZIP from inside the wapp dir so paths are at the root.
    # tests.wasm and tests/ source go in only when present.
    # main.c is bundled too — it lets the App Creator load existing
    # wapps for editing (read_source primitive) and keeps install
    # archives self-describing.
    (
        cd "$dir"
        zip -q -r "$wapp_file" \
            app.wasm \
            manifest.json \
            $([ -f main.c ] && echo main.c) \
            $([ -f Makefile ] && echo Makefile) \
            $([ -d screens ] && echo screens) \
            $([ -d media ] && echo media) \
            $([ -d web ] && echo web) \
            $([ -d lang ] && echo lang) \
            $([ -d bin ] && echo bin) \
            $([ -f tests.wasm ] && echo tests.wasm) \
            $([ -d tests ] && echo tests)
    )

    size=$(wc -c < "$wapp_file" | tr -d ' ')
    if [ "$size" -lt 1024 ]; then
        human="${size}B"
    else
        human="$(echo "$size" | awk '{printf "%.1fKB", $1/1024}')"
    fi
    echo "[$name] → $name/$name-$version.wapp ($human)"
    return 0
}

# Generate binaries/index.json from all .wapp files present.
generate_index() {
    index="$OUTPUT_DIR/index.json"
    printf '[\n' > "$index"
    first=1
    for wapp in "$OUTPUT_DIR"/*/*.wapp; do
        [ -f "$wapp" ] || continue
        manifest=$(unzip -p "$wapp" manifest.json 2>/dev/null) || continue
        wapp_dir=$(basename "$(dirname "$wapp")")
        fname=$(basename "$wapp")
        size=$(wc -c < "$wapp" | tr -d ' ')
        wapp_id=$(echo "$manifest" | grep -o '"id"[[:space:]]*:[[:space:]]*"[^"]*"' \
            | head -1 | sed 's/.*"id"[[:space:]]*:[[:space:]]*"//;s/"//')
        version=$(echo "$manifest" | grep -o '"version"[[:space:]]*:[[:space:]]*"[^"]*"' \
            | head -1 | sed 's/.*"version"[[:space:]]*:[[:space:]]*"//;s/"//')
        title=$(echo "$manifest" | grep -o '"title"[[:space:]]*:[[:space:]]*"[^"]*"' \
            | head -1 | sed 's/.*"title"[[:space:]]*:[[:space:]]*"//;s/"//')
        description=$(echo "$manifest" | grep -o '"description"[[:space:]]*:[[:space:]]*"[^"]*"' \
            | head -1 | sed 's/.*"description"[[:space:]]*:[[:space:]]*"//;s/"//')

        # Legacy schema migration: pre-title manifests put the launcher
        # label in `description`. If a manifest lacks `title` but has a
        # `description`, treat the description as the title and emit no
        # one-liner so the host doesn't double-print it.
        if [ -z "$title" ] && [ -n "$description" ]; then
            title="$description"
            description=""
        fi

        [ "$first" = 1 ] && first=0 || printf ',\n' >> "$index"
        printf '  {"file":"%s/%s","id":"%s","version":"%s","size":%s,"title":"%s","description":"%s"}' \
            "$wapp_dir" "$fname" "$wapp_id" "$version" "$size" "$title" "$description" >> "$index"
    done
    printf '\n]\n' >> "$index"
}

# Single wapp mode (skip directories that are part of the build
# infrastructure, not actual wapps).
if [ -n "${1:-}" ] && [ -d "$WAPPS_DIR/$1" ] && [ -f "$WAPPS_DIR/$1/manifest.json" ]; then
    build_wapp "$WAPPS_DIR/$1"
    generate_index
    exit $?
fi

# Build all wapps.
echo "Building all wapps..."
echo "  WASI_SDK_PATH=$WASI_SDK_PATH"
echo "  Output: $OUTPUT_DIR/"
echo ""

TOTAL=0
BUILT=0
FAILED=0

for dir in "$WAPPS_DIR"/*/; do
    # Only consider directories that look like wapps — i.e. have a
    # manifest.json. Skip sdk/, hal/, modules/, binaries/, dist/, etc.
    [ -f "$dir/manifest.json" ] || continue
    [ -f "$dir/Makefile" ] || continue
    TOTAL=$((TOTAL + 1))

    if build_wapp "$dir"; then
        BUILT=$((BUILT + 1))
    else
        FAILED=$((FAILED + 1))
    fi
    echo ""
done

echo "Results: $BUILT/$TOTAL built"
[ "$FAILED" -gt 0 ] && echo "  $FAILED failed" && exit 1

generate_index

# Summary
echo ""
echo "Packages:"
printf "  %-30s %s\n" "FILE" "SIZE"
printf "  %-30s %s\n" "------------------------------" "--------"
for wapp in "$OUTPUT_DIR"/*/*.wapp; do
    [ -f "$wapp" ] || continue
    wapp_dir=$(basename "$(dirname "$wapp")")
    fname=$(basename "$wapp")
    size=$(wc -c < "$wapp" | tr -d ' ')
    if [ "$size" -lt 1024 ]; then
        human="${size}B"
    else
        human="$(echo "$size" | awk '{printf "%.1fKB", $1/1024}')"
    fi
    printf "  %-30s %s\n" "$wapp_dir/$fname" "$human"
done
