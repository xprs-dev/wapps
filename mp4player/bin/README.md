# Native decoder binaries (bundled in the wapp)

Minimal decode-only static FFmpeg builds, shipped INSIDE this wapp (see
`manifest.json` → `provides.native_binaries`). The host stays codec-free:
these travel with the signed wapp and are spawned per playback (same trust
boundary as `hal_process_exec`). They exist because wasm decode loses the
codecs' SIMD assembly and threading (5-10× slower) — the wasm module in
`app.wasm` remains the universal fallback.

Contents:

- `ffmpeg-linux-x86_64` — static ELF, ~11 MB
- `ffmpeg-win-x86_64.exe` — static PE, ~10 MB

## Build recipe (reproducible)

Source: FFmpeg `release/7.1` (https://git.ffmpeg.org/ffmpeg.git) +
dav1d 1.4.3 (https://code.videolan.org/videolan/dav1d.git) for AV1
(FFmpeg's builtin `av1` decoder is hwaccel-only).

dav1d (native asm ON — this is a real binary, unlike the wasm build):

    meson setup build --default-library=static --prefix=<prefix> \
      -Denable_tools=false -Denable_tests=false -Denable_examples=false
    ninja -C build install
    # Windows: add --cross-file with x86_64-w64-mingw32-{gcc,g++,ar,strip}

FFmpeg (identical for both targets; Windows adds
`--arch=x86_64 --target-os=mingw32 --cross-prefix=x86_64-w64-mingw32-`
and drops `png` from encoders + `--enable-zlib`):

    PKG_CONFIG_PATH=<dav1d prefix>/lib/.../pkgconfig ./configure \
      --disable-everything --disable-doc --disable-debug \
      --disable-network --disable-autodetect \
      --disable-ffprobe --disable-ffplay --disable-shared --enable-static \
      --enable-protocol=file,pipe \
      --enable-demuxer=mov,matroska,avi,mpegts,mpegps,ogg,image2 \
      --enable-decoder=h264,hevc,vp8,vp9,libdav1d,mpeg4,mpeg2video,mjpeg,theora,aac,mp3,opus,vorbis,flac,pcm_s16le \
      --enable-parser=h264,hevc,vp8,vp9,av1,aac,mpegaudio,opus,vorbis \
      --enable-muxer=rawvideo,image2,image2pipe,pcm_s16le \
      --enable-encoder=bmp,png,rawvideo,pcm_s16le \
      --enable-filter=scale,format,aformat,aresample,anull,null \
      --enable-zlib --enable-libdav1d \
      --extra-ldflags='-static' --pkg-config-flags='--static'
    make -j6 && strip ffmpeg

License: LGPL 2.1+ (this configuration enables no GPL components — the
configure summary prints "License: LGPL version 2.1 or later"). dav1d is
BSD-2. FFmpeg source: https://ffmpeg.org — this project redistributes
unmodified builds of the above configuration.

sha256:
- ffmpeg-linux-x86_64: 46fba3210c8b209ce11f03bdfa5426ae3823c871e18793e611c7980979cd8eae
- ffmpeg-win-x86_64.exe: 341452e2f6241e9d105d40738b76607bfe3dfca6a21af0dcdb92b81c54d1de5a

Measured on a desktop x86_64: 1080x1920 HEVC decodes at ~180 fps (6×
realtime) vs the wasm fallback's sub-realtime — the whole point.
