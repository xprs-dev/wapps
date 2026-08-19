// See av1_dec.h. dav1d runs with n_threads = 1 — strictly single-threaded
// inside the wapp (the pthread symbols it links against are the same wasi
// stubs the other decoders use; none are exercised at runtime).

#include "av1_dec.h"

#include "dav1d/include/dav1d/dav1d.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct Av1Dec {
  Dav1dContext *ctx;
  Dav1dPicture held; // last pulled picture; planes point into it
  int hasHeld;
  // Scratch planes for 10-bit → 8-bit downshift (grown on demand).
  uint8_t *shift[3];
  int shiftCap[3];
};

static void push_bytes(Av1Dec *d, const uint8_t *bytes, int len, double ptsMs) {
  if (len <= 0) return;
  Dav1dData data;
  memset(&data, 0, sizeof(data));
  uint8_t *buf = dav1d_data_create(&data, (size_t)len);
  if (!buf) return;
  memcpy(buf, bytes, (size_t)len);
  data.m.timestamp = (int64_t)(ptsMs + 0.5);
  int res = dav1d_send_data(d->ctx, &data);
  if (res < 0) dav1d_data_unref(&data); // EAGAIN etc. — drop, caller re-pulls
}

extern "C" Av1Dec *av1_open(const uint8_t *config, int config_len) {
  Av1Dec *d = (Av1Dec *)calloc(1, sizeof(Av1Dec));
  if (!d) return nullptr;
  Dav1dSettings s;
  dav1d_default_settings(&s);
  s.n_threads = 1;
  s.max_frame_delay = 1; // output as soon as decoded (low latency, less RAM)
  if (dav1d_open(&d->ctx, &s) < 0) {
    free(d);
    return nullptr;
  }
  // av1C box payload: 4 config bytes, then optional configOBUs (sequence
  // header). Feed those so the first sample decodes even when the stream
  // doesn't repeat the sequence header.
  if (config && config_len > 4) {
    push_bytes(d, config + 4, config_len - 4, 0);
  }
  return d;
}

extern "C" void av1_push(Av1Dec *d, const uint8_t *data, int len,
                         double ptsMs) {
  if (d) push_bytes(d, data, len, ptsMs);
}

// Copy a 10-bit (word) plane downshifted to 8 bits into scratch.
static const uint8_t *downshift(Av1Dec *d, int ch, const uint8_t *src,
                                ptrdiff_t srcStride, int w, int h, int bpc,
                                int *outStride) {
  int need = w * h;
  if (d->shiftCap[ch] < need) {
    free(d->shift[ch]);
    d->shift[ch] = (uint8_t *)malloc(need);
    d->shiftCap[ch] = d->shift[ch] ? need : 0;
    if (!d->shift[ch]) return nullptr;
  }
  int sh = bpc - 8;
  for (int y = 0; y < h; y++) {
    const uint16_t *in = (const uint16_t *)(src + (size_t)y * srcStride);
    uint8_t *out = d->shift[ch] + (size_t)y * w;
    for (int x = 0; x < w; x++) out[x] = (uint8_t)(in[x] >> sh);
  }
  *outStride = w;
  return d->shift[ch];
}

extern "C" int av1_pull(Av1Dec *d, Av1Frame *out) {
  if (!d) return 0;
  Dav1dPicture pic;
  memset(&pic, 0, sizeof(pic));
  int res = dav1d_get_picture(d->ctx, &pic);
  if (res < 0) return 0; // EAGAIN (needs data / end) or decode error
  if (d->hasHeld) {
    dav1d_picture_unref(&d->held);
    d->hasHeld = 0;
  }
  d->held = pic;
  d->hasHeld = 1;
  if (pic.p.layout != DAV1D_PIXEL_LAYOUT_I420) {
    return 0; // 4:2:2/4:4:4/mono — rare; skip the frame rather than garble
  }
  out->w = pic.p.w;
  out->h = pic.p.h;
  out->ptsMs = (double)pic.m.timestamp;
  const int cw = (pic.p.w + 1) >> 1, chh = (pic.p.h + 1) >> 1;
  for (int ch = 0; ch < 3; ch++) {
    const uint8_t *p = (const uint8_t *)pic.data[ch];
    int stride = (int)pic.stride[ch == 0 ? 0 : 1];
    if (pic.p.bpc > 8) {
      int w = ch == 0 ? pic.p.w : cw, h = ch == 0 ? pic.p.h : chh;
      p = downshift(d, ch, p, pic.stride[ch == 0 ? 0 : 1], w, h, pic.p.bpc,
                    &stride);
      if (!p) return 0;
    }
    out->planes[ch] = p;
    out->strides[ch] = stride;
  }
  return 1;
}

extern "C" void av1_close(Av1Dec *d) {
  if (!d) return;
  if (d->hasHeld) dav1d_picture_unref(&d->held);
  dav1d_close(&d->ctx);
  for (int ch = 0; ch < 3; ch++) free(d->shift[ch]);
  free(d);
}
