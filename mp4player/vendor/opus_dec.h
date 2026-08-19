// Streaming ogg-opus decoder: demuxes ogg pages (libogg) and decodes Opus
// packets (libopus) to interleaved 48 kHz s16 PCM. For .opus / ogg-Opus files.
#ifndef PLAYER_OPUS_DEC_H
#define PLAYER_OPUS_DEC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct OpusDec OpusDec;

// Open ogg-opus from [data]/[len] (must stay valid until close). Fills rate
// (always 48000), channels. Returns an opaque handle or NULL on failure.
OpusDec* opus_dec_open(const uint8_t* data, int len, int* rate, int* channels);

// Decode up to [max_frames] interleaved s16 frames into [out]
// (out holds max_frames*channels shorts). Returns frames, 0 = EOF.
int opus_dec_read(OpusDec* d, int16_t* out, int max_frames);

void opus_dec_close(OpusDec* d);

#ifdef __cplusplus
}
#endif

#endif
