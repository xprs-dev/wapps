// See hevc_dec.h. libde265 runs strictly single-threaded here (no worker
// threads are started); wasi_st.h inside vendor/de265 stubs the std threading
// primitives the unmodified sources reference.

#include "hevc_dec.h"

#include "de265/libde265/de265.h"

#include <stdlib.h>
#include <string.h>

struct HevcDec {
  de265_decoder_context *ctx;
  int lenSize; // NAL length-prefix bytes in mp4 samples (from hvcC)
  // Scratch planes for Main10 → 8-bit downshift (grown on demand).
  uint8_t *shift[3];
  int shiftCap[3];
  // Sample pts values pushed but not yet emitted, kept sorted ascending.
  // Pictures come out in display order while samples go in decode order
  // (B-frames), so the true display time of the next output picture is the
  // SMALLEST pending input pts — pop it per emitted frame.
  double *pend;
  int pendLen, pendCap;
};

static void pend_push(HevcDec *d, double pts) {
  if (d->pendLen == d->pendCap) {
    int n = d->pendCap ? d->pendCap * 2 : 64;
    double *g = (double *)realloc(d->pend, n * sizeof(double));
    if (!g) return; // drop tracking; frames fall back to their input pts
    d->pend = g;
    d->pendCap = n;
  }
  int i = d->pendLen++;
  while (i > 0 && d->pend[i - 1] > pts) {
    d->pend[i] = d->pend[i - 1];
    i--;
  }
  d->pend[i] = pts;
}

static int pend_pop(HevcDec *d, double *pts) {
  if (d->pendLen == 0) return 0;
  *pts = d->pend[0];
  d->pendLen--;
  memmove(d->pend, d->pend + 1, d->pendLen * sizeof(double));
  return 1;
}

// Parse the raw HVCCDecoderConfigurationRecord: extract lengthSizeMinusOne
// and push every parameter-set NAL (VPS/SPS/PPS arrays) into the decoder.
static bool push_parameter_sets(HevcDec *d, const uint8_t *p, int n) {
  if (n < 23 || p[0] != 1) return false; // configurationVersion must be 1
  d->lenSize = (p[21] & 3) + 1;
  int numArrays = p[22];
  int i = 23;
  for (int a = 0; a < numArrays; a++) {
    if (i + 3 > n) return false;
    i++; // array_completeness(1) + reserved(1) + NAL_unit_type(6)
    int numNalus = (p[i] << 8) | p[i + 1];
    i += 2;
    for (int u = 0; u < numNalus; u++) {
      if (i + 2 > n) return false;
      int nl = (p[i] << 8) | p[i + 1];
      i += 2;
      if (nl <= 0 || i + nl > n) return false;
      de265_push_NAL(d->ctx, p + i, nl, 0, nullptr);
      i += nl;
    }
  }
  return true;
}

extern "C" HevcDec *hevc_open(const uint8_t *hvcc, int hvcc_len,
                              int *nal_len_size) {
  HevcDec *d = (HevcDec *)calloc(1, sizeof(HevcDec));
  if (!d) return nullptr;
  d->ctx = de265_new_decoder();
  if (!d->ctx) { free(d); return nullptr; }
  d->lenSize = 4;
  if (!push_parameter_sets(d, hvcc, hvcc_len)) {
    de265_free_decoder(d->ctx);
    free(d);
    return nullptr;
  }
  if (nal_len_size) *nal_len_size = d->lenSize;
  return d;
}

extern "C" void hevc_push_sample(HevcDec *d, const uint8_t *data, int len,
                                 double ptsMs) {
  if (!d) return;
  pend_push(d, ptsMs);
  int i = 0;
  const int ls = d->lenSize;
  while (i + ls <= len) {
    uint32_t nl = 0;
    for (int b = 0; b < ls; b++) nl = (nl << 8) | data[i + b];
    i += ls;
    if (nl == 0 || i + (int)nl > len) break;
    de265_push_NAL(d->ctx, data + i, (int)nl, (de265_PTS)(ptsMs + 0.5),
                   nullptr);
    i += (int)nl;
  }
}

extern "C" void hevc_flush(HevcDec *d) {
  if (d) de265_flush_data(d->ctx);
}

// Copy a >8-bit plane downshifted to 8 bits into the scratch buffer.
static const uint8_t *downshift(HevcDec *d, int ch, const uint8_t *src,
                                int srcStride, int w, int h, int bpp,
                                int *outStride) {
  int need = w * h;
  if (d->shiftCap[ch] < need) {
    free(d->shift[ch]);
    d->shift[ch] = (uint8_t *)malloc(need);
    d->shiftCap[ch] = d->shift[ch] ? need : 0;
    if (!d->shift[ch]) return nullptr;
  }
  int sh = bpp - 8;
  for (int y = 0; y < h; y++) {
    const uint16_t *in = (const uint16_t *)(src + (size_t)y * srcStride);
    uint8_t *out = d->shift[ch] + (size_t)y * w;
    for (int x = 0; x < w; x++) out[x] = (uint8_t)(in[x] >> sh);
  }
  *outStride = w;
  return d->shift[ch];
}

extern "C" int hevc_pull(HevcDec *d, HevcFrame *out) {
  if (!d) return 0;
  for (;;) {
    const de265_image *img = de265_get_next_picture(d->ctx);
    if (img) {
      if (de265_get_chroma_format(img) != de265_chroma_420) continue; // skip
      out->w = de265_get_image_width(img, 0);
      out->h = de265_get_image_height(img, 0);
      if (!pend_pop(d, &out->ptsMs)) {
        out->ptsMs = (double)de265_get_image_PTS(img);
      }
      for (int ch = 0; ch < 3; ch++) {
        int stride = 0;
        const uint8_t *p = de265_get_image_plane(img, ch, &stride);
        int bpp = de265_get_bits_per_pixel(img, ch);
        if (bpp > 8) {
          p = downshift(d, ch, p, stride,
                        de265_get_image_width(img, ch),
                        de265_get_image_height(img, ch), bpp, &stride);
          if (!p) return 0;
        }
        out->planes[ch] = p;
        out->strides[ch] = stride;
      }
      return 1;
    }
    int more = 0;
    de265_error err = de265_decode(d->ctx, &more);
    if (err != DE265_OK && err != DE265_ERROR_WAITING_FOR_INPUT_DATA) {
      // Non-fatal decode errors (missing refs etc.) — keep going while the
      // decoder says there may be more output.
      if (!more) return 0;
      continue;
    }
    if (err == DE265_ERROR_WAITING_FOR_INPUT_DATA || !more) return 0;
  }
}

extern "C" void hevc_close(HevcDec *d) {
  if (!d) return;
  de265_free_decoder(d->ctx);
  for (int ch = 0; ch < 3; ch++) free(d->shift[ch]);
  free(d->pend);
  free(d);
}
