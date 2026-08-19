// Unified raw-audio decoder API for the Player wapp: mp3/wav/flac/ogg-vorbis
// to interleaved 16-bit PCM. Backed by dr_libs + stb_vorbis (single-header,
// public domain), isolated in audio_pcm.c / stb_vorbis.c so their
// IMPLEMENTATION macros live in exactly one translation unit each.
#ifndef PLAYER_AUDIO_PCM_H
#define PLAYER_AUDIO_PCM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  AUDIO_FMT_NONE = 0,
  AUDIO_FMT_MP3 = 1,
  AUDIO_FMT_WAV = 2,
  AUDIO_FMT_FLAC = 3,
  AUDIO_FMT_VORBIS = 4,
};

typedef struct {
  int fmt;
  void* h;
  int rate;
  int channels;
  long long total_frames; // 0 if unknown (mp3) → unknown duration
} AudioDec;

// Open [data]/[len] as [fmt]. Returns 1 on success (fills rate/channels/
// total_frames), 0 on failure. [data] must stay valid until audio_close.
int audio_open(AudioDec* d, const uint8_t* data, int len, int fmt);

// Decode up to [max_frames] interleaved s16 frames into [out]
// (out must hold max_frames*channels shorts). Returns frames decoded, 0 = EOF.
int audio_read(AudioDec* d, int16_t* out, int max_frames);

void audio_close(AudioDec* d);

#ifdef __cplusplus
}
#endif

#endif
