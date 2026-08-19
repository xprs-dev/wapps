// WebM/Matroska backend: libnestegg demuxes the container, libvpx decodes the
// VP8/VP9 video track, and libopus decodes the Opus audio track. Output is raw
// I420 video planes + interleaved s16 audio — the host stays codec-free.
//
// Vorbis-in-WebM audio is not decoded (rare next to Opus; would need libvorbis
// with the 3 setup headers). Such files still play video; audio is dropped.
#ifndef PLAYER_WEBM_DEC_H
#define PLAYER_WEBM_DEC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WebmDec WebmDec;

// One decoded unit pulled from the stream (video frame or audio block).
typedef struct {
  int type;                 // 0 = nothing this packet, 1 = video, 2 = audio
  const uint8_t *planes[3]; // video: I420 Y/U/V (valid until next webm_consume)
  int strides[3];           // video: plane strides
  int vw, vh;               // video: frame dimensions
  int aframes;              // audio: interleaved s16 frames written to apcm
  double ptsMs;             // presentation timestamp (ms)
} WebmUnit;

// Open WebM from [data]/[len] (must stay valid until close). NULL if not a
// supported WebM (no VP8/VP9 video and no Opus audio).
WebmDec *webm_open(const uint8_t *data, int len);

int webm_has_video(WebmDec *d);
void webm_video_info(WebmDec *d, int *w, int *h);
int webm_has_audio(WebmDec *d); // Opus only (decodable)
void webm_audio_info(WebmDec *d, int *rate, int *channels);
long long webm_duration_ms(WebmDec *d);

// Timestamp (ms) of the next pending packet without consuming it. Returns 1 and
// sets *ms, or 0 at end of stream.
int webm_next_tstamp(WebmDec *d, double *ms);

// Decode the pending packet into [out]. For audio, PCM lands in [apcm] (room for
// apcm_max_frames * channels shorts). Returns 1 if a unit was produced, 0 at EOF.
// out->type may be 0 (e.g. a track we ignore) — caller should just continue.
int webm_consume(WebmDec *d, WebmUnit *out, int16_t *apcm, int apcm_max_frames);

void webm_close(WebmDec *d);

#ifdef __cplusplus
}
#endif

#endif
