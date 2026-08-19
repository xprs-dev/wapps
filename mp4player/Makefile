# mp4player — wasm H.264 video player wapp.
#
# Unlike the plain-C wapps (sdk/Makefile.common), this one bundles a C++
# decoder (openh264) + an mp4 demuxer (minimp4) compiled to wasm, so it
# needs clang++ and a larger source set. The host stays codec-free; this
# wapp pushes decoded RGBA via the hal_video_* sink.
#
# Build:  cd wapps && WASI_SDK_PATH=$HOME/wasi-sdk make -C mp4player
#   (or)  cd wapps/mp4player && WASI_SDK_PATH=$HOME/wasi-sdk make

WASI_SDK_PATH ?= /opt/wasi-sdk
CXX  := $(WASI_SDK_PATH)/bin/clang++
CC   := $(WASI_SDK_PATH)/bin/clang
HAL  := ../hal
OH   := vendor/openh264/codec
FDK  := vendor/fdkaac
VPX  := vendor/vpx
NEG  := vendor/nestegg
DE265 := vendor/de265

OUTPUT := app.wasm

# -Ivendor/sjlj is FIRST so its setjmp.h shadows the wasi one for libvpx (which
# uses setjmp/longjmp for error recovery; wasi has no real setjmp). See sjlj/.
INCLUDES := \
	-Ivendor/sjlj \
	-I$(HAL) \
	-Ivendor \
	-I$(OH)/api \
	-I$(OH)/api/wels \
	-I$(OH)/common/inc \
	-I$(OH)/decoder/core/inc \
	-I$(OH)/decoder/plus/inc \
	-I$(FDK)/libAACdec/include \
	-I$(FDK)/libFDK/include \
	-I$(FDK)/libSYS/include \
	-I$(FDK)/libMpegTPDec/include \
	-I$(FDK)/libPCMutils/include \
	-I$(FDK)/libSBRdec/include \
	-I$(FDK)/libDRCdec/include \
	-I$(FDK)/libSACdec/include \
	-I$(FDK)/libArithCoding/include \
	-Ivendor/opuslib/include \
	-Ivendor/opuslib/celt \
	-Ivendor/opuslib/silk \
	-Ivendor/opuslib/silk/float \
	-Ivendor/opuslib/ogg/include \
	-I$(VPX) \
	-I$(NEG)/include \
	-I$(DE265)

# -msimd128: wasm SIMD via LLVM autovectorization only (the vendored codec
# trees carry no SIMD sources) — measured ~1.2-2x, not native-asm speed. The
# REAL fast paths are the native binaries in bin/ and Android's MediaCodec;
# this narrows the gap for the wasm fallback.
CXXFLAGS := \
	--target=wasm32-wasi \
	-O2 \
	-msimd128 \
	-fno-exceptions -fno-rtti \
	-ffunction-sections -fdata-sections \
	-DNDEBUG -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PTHREAD \
	-DHAVE_POSIX_MEMALIGN \
	$(INCLUDES)

# C flags for the vendored decoder libraries (dr_libs, stb_vorbis): plain C,
# no stdio (we decode from memory). No -fno-rtti/-fno-exceptions (C-only).
CFLAGS := \
	--target=wasm32-wasi \
	-O2 \
	-ffunction-sections -fdata-sections \
	-DNDEBUG -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PTHREAD \
	-DSTB_VORBIS_NO_STDIO \
	-DOPUS_BUILD -DVAR_ARRAYS \
	$(INCLUDES)

LDFLAGS := \
	--target=wasm32-wasi \
	-nostartfiles \
	-lwasi-emulated-signal \
	-lwasi-emulated-pthread \
	-Wl,--no-entry \
	-Wl,--gc-sections \
	-Wl,-z,stack-size=5242880 \
	-Wl,--initial-memory=134217728 \
	-Wl,--max-memory=134217728 \
	-Wl,--export=module_init \
	-Wl,--export=module_tick \
	-Wl,--export=module_handle_event \
	-Wl,--export=module_destroy \
	-Wl,--export=module_tick_interval_ms \
	-Wl,--strip-all

# C++ sources: main + openh264 (video) + fdk-aac (AAC audio) + aac wrapper +
# webm backend (drives libvpx + libnestegg + libopus) + hevc backend
# (libde265; .cc sources listed separately below).
CXX_SRCS := \
	main.cpp \
	vendor/aac_dec.cpp \
	vendor/webm_dec.cpp \
	vendor/hevc_dec.cpp \
	vendor/av1_dec.cpp \
	$(wildcard $(OH)/common/src/*.cpp) \
	$(wildcard $(OH)/decoder/core/src/*.cpp) \
	$(wildcard $(OH)/decoder/plus/src/*.cpp) \
	$(wildcard $(FDK)/libAACdec/src/*.cpp) \
	$(wildcard $(FDK)/libFDK/src/*.cpp) \
	$(wildcard $(FDK)/libSYS/src/*.cpp) \
	$(wildcard $(FDK)/libMpegTPDec/src/*.cpp) \
	$(wildcard $(FDK)/libPCMutils/src/*.cpp) \
	$(wildcard $(FDK)/libSBRdec/src/*.cpp) \
	$(wildcard $(FDK)/libDRCdec/src/*.cpp) \
	$(wildcard $(FDK)/libSACdec/src/*.cpp) \
	$(wildcard $(FDK)/libArithCoding/src/*.cpp)

# C sources: vendored audio decoders + stubs + libvpx (VP8/VP9) + nestegg +
# setjmp shim (compiled as C).
C_SRCS := \
	vendor/audio_pcm.c \
	vendor/stb_vorbis.c \
	vendor/wasi_thread_stubs.c \
	vendor/opus_dec.c \
	vendor/opuslib/ogg/framing.c \
	vendor/opuslib/ogg/bitwise.c \
	$(wildcard vendor/opuslib/celt/*.c) \
	$(wildcard vendor/opuslib/silk/*.c) \
	$(wildcard vendor/opuslib/silk/float/*.c) \
	$(wildcard vendor/opuslib/src/*.c) \
	vendor/sjlj/sjlj_shim.c \
	$(NEG)/src/nestegg.c \
	$(VPX)/vpx_config.c \
	$(wildcard $(VPX)/vpx/src/*.c) \
	$(wildcard $(VPX)/vpx_mem/*.c) \
	$(wildcard $(VPX)/vpx_scale/*.c) \
	$(wildcard $(VPX)/vpx_scale/generic/*.c) \
	$(wildcard $(VPX)/vpx_dsp/*.c) \
	$(wildcard $(VPX)/vpx_util/*.c) \
	$(wildcard $(VPX)/vp8/*.c) \
	$(wildcard $(VPX)/vp8/common/*.c) \
	$(wildcard $(VPX)/vp8/common/generic/*.c) \
	$(wildcard $(VPX)/vp8/decoder/*.c) \
	$(wildcard $(VPX)/vp9/*.c) \
	$(wildcard $(VPX)/vp9/common/*.c) \
	$(wildcard $(VPX)/vp9/decoder/*.c)

# libde265 (HEVC) — upstream uses .cc; runs single-threaded via its wasi_st.h
# shim (see vendor/de265/libde265/wasi_st.h).
CC_SRCS := $(wildcard $(DE265)/libde265/*.cc)

OBJS := $(CXX_SRCS:.cpp=.o) $(CC_SRCS:.cc=.o) $(C_SRCS:.c=.o)

.PHONY: all clean
all: $(OUTPUT)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

%.o: %.cc
	$(CXX) $(CXXFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# dav1d (AV1) links as a prebuilt wasm32-wasi static lib — meson-only build,
# see vendor/dav1d/README.md for how to regenerate it.
DAV1D_LIB := vendor/dav1d/lib/libdav1d.a

$(OUTPUT): $(OBJS) $(DAV1D_LIB)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(DAV1D_LIB)
	@echo "Built $(OUTPUT) ($$(wc -c < $(OUTPUT)) bytes)"

clean:
	rm -f $(OBJS) $(OUTPUT)
