# dav1d (AV1 decoder) — prebuilt for wasm32-wasi

`lib/libdav1d.a` is dav1d 1.4.3 built for wasm32-wasi (pure C, no asm, no
worker threads used at runtime — the player opens it with `n_threads = 1`).
Unlike the other vendored decoders it is not built from source by the wapp
Makefile because dav1d's build is meson-only (it generates config headers and
compiles the 8/16-bit template sources twice).

To regenerate:

    git clone --depth 1 --branch 1.4.3 https://code.videolan.org/videolan/dav1d.git
    pip install meson ninja   # any recent versions
    cat > wasi-cross.ini <<'EOF'
    # IMPORTANT: meson's threads dependency adds -pthread, which makes clang
    # emit wasm atomics/shared-memory opcodes that wasm_run (wasmtime) rejects
    # ("threads support is not enabled"). Use a wrapper that strips it:
    cat > clang-nopthread <<'EOF2'
    #!/bin/bash
    args=(); for a in "$@"; do [ "$a" = "-pthread" ] && continue; args+=("$a"); done
    exec <WASI_SDK>/bin/clang "${args[@]}"
    EOF2
    chmod +x clang-nopthread

    [binaries]
    c = '<path to clang-nopthread wrapper>'
    cpp = '<WASI_SDK>/bin/clang++'
    ar = '<WASI_SDK>/bin/llvm-ar'
    strip = '<WASI_SDK>/bin/llvm-strip'
    ranlib = '<WASI_SDK>/bin/llvm-ranlib'
    [built-in options]
    c_args = ['--target=wasm32-wasi', '-O2', '-msimd128', '-D_GNU_SOURCE', '-D_WASI_EMULATED_SIGNAL', '-D_WASI_EMULATED_PTHREAD', '-ffunction-sections', '-fdata-sections']
    c_link_args = ['--target=wasm32-wasi']
    default_library = 'static'
    [host_machine]
    system = 'wasi'
    cpu_family = 'wasm32'
    cpu = 'wasm32'
    endian = 'little'
    EOF
    cd dav1d
    meson setup build-wasi --cross-file ../wasi-cross.ini \
      -Denable_asm=false -Denable_tools=false -Denable_tests=false \
      -Denable_examples=false -Dlogging=false
    ninja -C build-wasi
    cp build-wasi/src/libdav1d.a       <here>/lib/
    cp include/dav1d/*.h               <here>/include/dav1d/
    cp build-wasi/include/dav1d/version.h <here>/include/dav1d/

License: BSD-2-Clause (see COPYING).
