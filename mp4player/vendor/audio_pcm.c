// Implementations of mp3/wav/flac decoders (dr_libs) + the stb_vorbis header
// declarations. The stb_vorbis IMPLEMENTATION is compiled separately from
// stb_vorbis.c. All configured for no-stdio (wasm has no filesystem here; the
// wapp slurps bytes via the host HAL and we decode from memory).
#include "audio_pcm.h"

#include <stdlib.h>
#include <string.h>

#define DR_MP3_IMPLEMENTATION
#define DRMP3_NO_STDIO
#include "draudio/dr_mp3.h"

#define DR_WAV_IMPLEMENTATION
#define DRWAV_NO_STDIO
#include "draudio/dr_wav.h"

#define DR_FLAC_IMPLEMENTATION
#define DRFLAC_NO_STDIO
#include "draudio/dr_flac.h"

// stb_vorbis: pull in the header-only declarations; the implementation is in
// stb_vorbis.c (compiled with the same NO_STDIO config).
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
#undef STB_VORBIS_HEADER_ONLY

int audio_open(AudioDec* d, const uint8_t* data, int len, int fmt) {
  memset(d, 0, sizeof(*d));
  d->fmt = fmt;
  switch (fmt) {
    case AUDIO_FMT_MP3: {
      drmp3* m = (drmp3*)malloc(sizeof(drmp3));
      if (!m) return 0;
      if (!drmp3_init_memory(m, data, (size_t)len, NULL)) { free(m); return 0; }
      d->h = m;
      d->rate = (int)m->sampleRate;
      d->channels = (int)m->channels;
      // Count frames for the progress bar/duration. This parses the file once
      // (then seeks back to the start), so playback still begins at frame 0.
      d->total_frames = (long long)drmp3_get_pcm_frame_count(m);
      return 1;
    }
    case AUDIO_FMT_WAV: {
      drwav* w = (drwav*)malloc(sizeof(drwav));
      if (!w) return 0;
      if (!drwav_init_memory(w, data, (size_t)len, NULL)) { free(w); return 0; }
      d->h = w;
      d->rate = (int)w->sampleRate;
      d->channels = (int)w->channels;
      d->total_frames = (long long)w->totalPCMFrameCount;
      return 1;
    }
    case AUDIO_FMT_FLAC: {
      drflac* f = drflac_open_memory(data, (size_t)len, NULL);
      if (!f) return 0;
      d->h = f;
      d->rate = (int)f->sampleRate;
      d->channels = (int)f->channels;
      d->total_frames = (long long)f->totalPCMFrameCount;
      return 1;
    }
    case AUDIO_FMT_VORBIS: {
      int err = 0;
      stb_vorbis* v = stb_vorbis_open_memory(data, len, &err, NULL);
      if (!v) return 0;
      stb_vorbis_info inf = stb_vorbis_get_info(v);
      d->h = v;
      d->rate = (int)inf.sample_rate;
      d->channels = inf.channels;
      d->total_frames = (long long)stb_vorbis_stream_length_in_samples(v);
      return 1;
    }
    default:
      return 0;
  }
}

int audio_read(AudioDec* d, int16_t* out, int max_frames) {
  if (!d || !d->h || max_frames <= 0) return 0;
  switch (d->fmt) {
    case AUDIO_FMT_MP3:
      return (int)drmp3_read_pcm_frames_s16((drmp3*)d->h, max_frames, out);
    case AUDIO_FMT_WAV:
      return (int)drwav_read_pcm_frames_s16((drwav*)d->h, max_frames, out);
    case AUDIO_FMT_FLAC:
      return (int)drflac_read_pcm_frames_s16((drflac*)d->h, max_frames, out);
    case AUDIO_FMT_VORBIS:
      return stb_vorbis_get_samples_short_interleaved(
          (stb_vorbis*)d->h, d->channels, out, max_frames * d->channels);
    default:
      return 0;
  }
}

void audio_close(AudioDec* d) {
  if (!d || !d->h) return;
  switch (d->fmt) {
    case AUDIO_FMT_MP3:
      drmp3_uninit((drmp3*)d->h);
      free(d->h);
      break;
    case AUDIO_FMT_WAV:
      drwav_uninit((drwav*)d->h);
      free(d->h);
      break;
    case AUDIO_FMT_FLAC:
      drflac_close((drflac*)d->h);
      break;
    case AUDIO_FMT_VORBIS:
      stb_vorbis_close((stb_vorbis*)d->h);
      break;
  }
  d->h = NULL;
}
