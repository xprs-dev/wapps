// WebM backend implementation — see webm_dec.h.
#include "webm_dec.h"
#include "av1_dec.h"

#include <stdlib.h>
#include <string.h>

extern "C" {
#include "nestegg/nestegg.h"
#include "vpx/vpx_decoder.h"
#include "vpx/vp8dx.h"
#include "opus.h"
}

struct WebmDec {
  const uint8_t *data;
  int len;
  int pos; // read cursor for the in-memory IO

  nestegg *ne;

  // video
  int v_track;     // -1 if none
  int v_is_vp9;    // 0 = VP8, 1 = VP9
  int v_is_av1;    // AV1 track → decoded by dav1d (av1), not libvpx
  int vw, vh;
  vpx_codec_ctx_t vpx;
  int vpx_ready;
  Av1Dec *av1;     // non-null when the AV1 decoder opened

  // audio (Opus)
  int a_track;     // -1 if none
  int a_rate, a_ch;
  OpusDecoder *opus;
  int skip_left;   // pre-skip samples (per channel) to drop

  long long dur_ms;

  // pending packet (peeked but not yet consumed)
  nestegg_packet *pending;
};

// ── in-memory nestegg IO ────────────────────────────────────────────────
static int64_t io_read(void *buf, size_t length, void *u) {
  WebmDec *d = (WebmDec *)u;
  int64_t avail = d->len - d->pos;
  if (avail <= 0) return 0; // EOF
  int64_t n = (int64_t)length < avail ? (int64_t)length : avail;
  memcpy(buf, d->data + d->pos, (size_t)n);
  d->pos += (int)n;
  return n;
}
static int io_seek(int64_t offset, int whence, void *u) {
  WebmDec *d = (WebmDec *)u;
  int64_t np;
  if (whence == NESTEGG_SEEK_SET) np = offset;
  else if (whence == NESTEGG_SEEK_CUR) np = d->pos + offset;
  else /* NESTEGG_SEEK_END */ np = d->len + offset;
  if (np < 0 || np > d->len) return -1;
  d->pos = (int)np;
  return 0;
}
static int64_t io_tell(void *u) { return ((WebmDec *)u)->pos; }

static void log_cb(nestegg *ctx, unsigned int sev, char const *fmt, ...) {
  (void)ctx; (void)sev; (void)fmt; // silent
}

WebmDec *webm_open(const uint8_t *data, int len) {
  if (!data || len < 4) return NULL;
  // Quick EBML magic check (0x1A45DFA3).
  if (!(data[0] == 0x1A && data[1] == 0x45 && data[2] == 0xDF && data[3] == 0xA3))
    return NULL;

  WebmDec *d = (WebmDec *)calloc(1, sizeof(WebmDec));
  if (!d) return NULL;
  d->data = data; d->len = len; d->pos = 0;
  d->v_track = -1; d->a_track = -1;

  nestegg_io io;
  io.read = io_read; io.seek = io_seek; io.tell = io_tell; io.userdata = d;
  if (nestegg_init(&d->ne, io, log_cb, -1) != 0) { free(d); return NULL; }

  unsigned int ntracks = 0;
  nestegg_track_count(d->ne, &ntracks);
  for (unsigned int i = 0; i < ntracks; i++) {
    int type = nestegg_track_type(d->ne, i);
    int codec = nestegg_track_codec_id(d->ne, i);
    if (type == NESTEGG_TRACK_VIDEO && d->v_track < 0 &&
        (codec == NESTEGG_CODEC_VP8 || codec == NESTEGG_CODEC_VP9 ||
         codec == NESTEGG_CODEC_AV1)) {
      nestegg_video_params vp; memset(&vp, 0, sizeof(vp));
      if (nestegg_track_video_params(d->ne, i, &vp) == 0) {
        d->v_track = (int)i;
        d->v_is_vp9 = (codec == NESTEGG_CODEC_VP9);
        d->v_is_av1 = (codec == NESTEGG_CODEC_AV1);
        d->vw = (int)vp.width;
        d->vh = (int)vp.height;
      }
    } else if (type == NESTEGG_TRACK_AUDIO && d->a_track < 0 &&
               codec == NESTEGG_CODEC_OPUS) {
      nestegg_audio_params ap; memset(&ap, 0, sizeof(ap));
      if (nestegg_track_audio_params(d->ne, i, &ap) == 0) {
        d->a_track = (int)i;
        d->a_rate = 48000; // Opus always decodes at 48k
        d->a_ch = (int)ap.channels;
        // codec_delay (ns) → pre-skip samples to drop at 48 kHz.
        d->skip_left = (int)((ap.codec_delay * 48000ULL) / 1000000000ULL);
      }
    }
  }

  if (d->v_track < 0 && d->a_track < 0) { nestegg_destroy(d->ne); free(d); return NULL; }

  // Init the video decoder: dav1d for AV1, libvpx for VP8/VP9. Failure →
  // video unplayable, fall back to audio-only if present.
  if (d->v_track >= 0) {
    if (d->v_is_av1) {
      d->av1 = av1_open(NULL, 0);
      if (!d->av1) d->v_track = -1;
    } else {
      vpx_codec_iface_t *iface = d->v_is_vp9 ? vpx_codec_vp9_dx() : vpx_codec_vp8_dx();
      if (vpx_codec_dec_init(&d->vpx, iface, NULL, 0) == VPX_CODEC_OK) {
        d->vpx_ready = 1;
      } else {
        d->v_track = -1;
      }
    }
  }

  // Init Opus decoder.
  if (d->a_track >= 0 && d->a_ch > 0) {
    int err = 0;
    d->opus = opus_decoder_create(48000, d->a_ch, &err);
    if (err != OPUS_OK || !d->opus) { d->opus = NULL; d->a_track = -1; }
  }

  if (d->v_track < 0 && d->a_track < 0) { webm_close(d); return NULL; }

  uint64_t dur_ns = 0;
  if (nestegg_duration(d->ne, &dur_ns) == 0) d->dur_ms = (long long)(dur_ns / 1000000ULL);

  return d;
}

int webm_has_video(WebmDec *d) { return d && d->v_track >= 0; }
void webm_video_info(WebmDec *d, int *w, int *h) {
  if (w) *w = d ? d->vw : 0;
  if (h) *h = d ? d->vh : 0;
}
int webm_has_audio(WebmDec *d) { return d && d->a_track >= 0 && d->opus; }
void webm_audio_info(WebmDec *d, int *rate, int *channels) {
  if (rate) *rate = d ? d->a_rate : 0;
  if (channels) *channels = d ? d->a_ch : 0;
}
long long webm_duration_ms(WebmDec *d) { return d ? d->dur_ms : 0; }

// Ensure a packet is buffered in d->pending. Returns 1 if one is available.
static int ensure_pending(WebmDec *d) {
  if (d->pending) return 1;
  nestegg_packet *pkt = NULL;
  int r = nestegg_read_packet(d->ne, &pkt);
  if (r != 1 || !pkt) { if (pkt) nestegg_free_packet(pkt); return 0; }
  d->pending = pkt;
  return 1;
}

int webm_next_tstamp(WebmDec *d, double *ms) {
  if (!d) return 0;
  if (!ensure_pending(d)) return 0;
  uint64_t ts = 0;
  nestegg_packet_tstamp(d->pending, &ts);
  if (ms) *ms = (double)ts / 1000000.0; // ns → ms
  return 1;
}

int webm_consume(WebmDec *d, WebmUnit *out, int16_t *apcm, int apcm_max_frames) {
  if (!d || !out) return 0;
  memset(out, 0, sizeof(*out));
  if (!ensure_pending(d)) return 0;

  nestegg_packet *pkt = d->pending;
  d->pending = NULL;

  unsigned int track = 0;
  nestegg_packet_track(pkt, &track);
  uint64_t ts = 0;
  nestegg_packet_tstamp(pkt, &ts);
  double ptsMs = (double)ts / 1000000.0;
  out->ptsMs = ptsMs;

  unsigned int nchunks = 0;
  nestegg_packet_count(pkt, &nchunks);

  if ((int)track == d->v_track && d->av1) {
    // AV1 → dav1d. Feed each chunk (a raw OBU temporal unit), emit the first
    // decoded picture.
    for (unsigned int c = 0; c < nchunks; c++) {
      unsigned char *cd = NULL; size_t cl = 0;
      if (nestegg_packet_data(pkt, c, &cd, &cl) != 0) continue;
      av1_push(d->av1, cd, (int)cl, ptsMs);
    }
    Av1Frame fr;
    if (av1_pull(d->av1, &fr)) {
      out->type = 1;
      out->planes[0] = fr.planes[0];
      out->planes[1] = fr.planes[1];
      out->planes[2] = fr.planes[2];
      out->strides[0] = fr.strides[0];
      out->strides[1] = fr.strides[1];
      out->strides[2] = fr.strides[2];
      out->vw = fr.w;
      out->vh = fr.h;
    }
  } else if ((int)track == d->v_track && d->vpx_ready) {
    // Feed each chunk to vpx; emit the first decoded frame.
    for (unsigned int c = 0; c < nchunks; c++) {
      unsigned char *cd = NULL; size_t cl = 0;
      if (nestegg_packet_data(pkt, c, &cd, &cl) != 0) continue;
      vpx_codec_decode(&d->vpx, cd, (unsigned int)cl, NULL, 0);
    }
    vpx_codec_iter_t it = NULL;
    vpx_image_t *img = vpx_codec_get_frame(&d->vpx, &it);
    if (img) {
      out->type = 1;
      out->planes[0] = img->planes[VPX_PLANE_Y];
      out->planes[1] = img->planes[VPX_PLANE_U];
      out->planes[2] = img->planes[VPX_PLANE_V];
      out->strides[0] = img->stride[VPX_PLANE_Y];
      out->strides[1] = img->stride[VPX_PLANE_U];
      out->strides[2] = img->stride[VPX_PLANE_V];
      out->vw = (int)img->d_w;
      out->vh = (int)img->d_h;
    }
  } else if ((int)track == d->a_track && d->opus && apcm) {
    int total = 0;
    for (unsigned int c = 0; c < nchunks; c++) {
      unsigned char *cd = NULL; size_t cl = 0;
      if (nestegg_packet_data(pkt, c, &cd, &cl) != 0) continue;
      int room = apcm_max_frames - total;
      if (room <= 0) break;
      int16_t *dst = apcm + (size_t)total * d->a_ch;
      int got = opus_decode(d->opus, cd, (int)cl, dst, room, 0);
      if (got <= 0) continue;
      // Drop encoder pre-skip from the very start.
      if (d->skip_left > 0) {
        int drop = d->skip_left < got ? d->skip_left : got;
        d->skip_left -= drop;
        int rem = got - drop;
        if (rem > 0)
          memmove(dst, dst + (size_t)drop * d->a_ch,
                  (size_t)rem * d->a_ch * sizeof(int16_t));
        got = rem;
      }
      total += got;
    }
    if (total > 0) { out->type = 2; out->aframes = total; }
  }

  nestegg_free_packet(pkt);
  return 1;
}

void webm_close(WebmDec *d) {
  if (!d) return;
  if (d->pending) nestegg_free_packet(d->pending);
  if (d->vpx_ready) vpx_codec_destroy(&d->vpx);
  if (d->av1) av1_close(d->av1);
  if (d->opus) opus_decoder_destroy(d->opus);
  if (d->ne) nestegg_destroy(d->ne);
  free(d);
}
