// Minimal AAC-LC/HE decode wrapper over fdk-aac (RAW transport: the mp4
// AudioSpecificConfig is supplied once, then each AAC access unit decodes to
// interleaved 16-bit PCM). Used for AAC audio inside mp4/m4a so videos get
// sound and m4a plays.
#ifndef PLAYER_AAC_DEC_H
#define PLAYER_AAC_DEC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  void* h; // HANDLE_AACDECODER
} AacDec;

// Open with the AudioSpecificConfig (mp4 dsi). Returns 1 on success.
int aac_open(AacDec* d, const uint8_t* asc, int asc_len);

// Decode one AAC access unit into [out] (interleaved s16, capacity out_max
// shorts). Returns frames-per-channel decoded (0 = need more, -1 = error) and
// fills *out_ch / *out_rate (rate can differ from the ASC for HE-AAC/SBR).
int aac_decode(AacDec* d, const uint8_t* au, int au_len, int16_t* out,
               int out_max, int* out_ch, int* out_rate);

void aac_close(AacDec* d);

#ifdef __cplusplus
}
#endif

#endif
