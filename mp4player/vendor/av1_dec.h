// AV1 video backend: dav1d compiled to WebAssembly (prebuilt static lib in
// vendor/dav1d/), wrapped in a small C API mirroring hevc_dec.h. Input is a
// raw OBU temporal unit per sample/packet (mp4 av01 samples and WebM/mkv AV1
// block payloads are already in that form — no reframing needed); output is
// 8-bit I420 planes (10-bit is downshifted internally). The host stays
// codec-free.
#ifndef PLAYER_AV1_DEC_H
#define PLAYER_AV1_DEC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Av1Dec Av1Dec;

// One decoded picture. Plane pointers stay valid until the next av1_pull /
// av1_close.
typedef struct {
  const uint8_t *planes[3]; // I420 Y/U/V, 8-bit
  int strides[3];
  int w, h;
  double ptsMs;
} Av1Frame;

// Open a decoder. [config]/[config_len] may pass the mp4 av1C box payload
// (its trailing configOBUs — typically the sequence header — are fed to the
// decoder); pass NULL/0 when the stream carries its own sequence header
// (WebM, and most mp4s repeat it in the first sample too).
Av1Dec *av1_open(const uint8_t *config, int config_len);

// Feed one temporal unit (raw OBUs) with its presentation time.
void av1_push(Av1Dec *d, const uint8_t *data, int len, double ptsMs);

// Pull the next decoded picture. Returns 1 and fills [out], or 0 if none
// ready yet (feed more data; at end of stream keep pulling until 0).
int av1_pull(Av1Dec *d, Av1Frame *out);

void av1_close(Av1Dec *d);

#ifdef __cplusplus
}
#endif

#endif
