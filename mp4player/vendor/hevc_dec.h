// HEVC (H.265) video backend: libde265 compiled to WebAssembly, wrapped in a
// small C API mirroring webm_dec.h. Input is mp4-framed samples (length-
// prefixed NALs, framing from the track's hvcC record); output is 8-bit I420
// planes — Main10 (HDR phone clips) is downshifted to 8-bit internally. The
// host stays codec-free.
#ifndef PLAYER_HEVC_DEC_H
#define PLAYER_HEVC_DEC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HevcDec HevcDec;

// One decoded picture. Plane pointers stay valid until the next hevc_pull /
// hevc_close (they point into the decoder's picture or a scratch buffer).
typedef struct {
  const uint8_t *planes[3]; // I420 Y/U/V, 8-bit
  int strides[3];
  int w, h;
  double ptsMs;
} HevcFrame;

// Open a decoder from the track's raw HVCCDecoderConfigurationRecord (the
// hvcC box payload). Pushes the VPS/SPS/PPS parameter sets. Returns NULL if
// the record is malformed. *nal_len_size gets the sample NAL length-prefix
// size (1/2/4 bytes) declared by the record.
HevcDec *hevc_open(const uint8_t *hvcc, int hvcc_len, int *nal_len_size);

// Feed one mp4 sample (length-prefixed NALs) with its presentation time.
void hevc_push_sample(HevcDec *d, const uint8_t *data, int len, double ptsMs);

// Signal end of stream so buffered (reordered) pictures drain via hevc_pull.
void hevc_flush(HevcDec *d);

// Pull the next decoded picture in display order. Returns 1 and fills [out],
// or 0 if no picture is ready yet (feed more samples / flush).
int hevc_pull(HevcDec *d, HevcFrame *out);

void hevc_close(HevcDec *d);

#ifdef __cplusplus
}
#endif

#endif
