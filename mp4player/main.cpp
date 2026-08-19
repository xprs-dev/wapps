/*
 * tools.xprs.mp4player — platform-agnostic MP4 video player wapp.
 *
 * Unlike the old "movies" wapp (which handed bytes to a host-compiled
 * codec), this wapp CONTAINS the decoders: minimp4 demuxes mp4/mov and
 * openh264 (H.264), libde265 (HEVC/H.265) or dav1d (AV1) decodes the video
 * track; nestegg demuxes WebM/mkv for libvpx (VP8/VP9) and dav1d — all
 * compiled to WebAssembly. The host holds NO codec; it only receives
 * decoded RGBA frames through the generic hal_video_* sink and uploads
 * them to a texture. The decoders therefore travel inside this .wapp and
 * run the same on every platform.
 *
 * Flow:
 *   file.open / video.load  -> slurp file, demux, init decoder, tell the
 *                              host to show its <group $type="video">
 *   module_tick             -> decode frames paced to a wall-clock play
 *                              clock (~LEAD_MS ahead), push each as RGBA
 *                              via hal_video_frame(pts)
 *   end of stream           -> hal_video_end()
 *
 * Audio (AAC) is intentionally not decoded in this MVP — the host drops
 * PCM for now (raw audio out is an inherently platform-specific sink).
 *
 * Build: cd wapps && WASI_SDK_PATH=$HOME/wasi-sdk make mp4player
 */

extern "C" {
#include "xprs_wasm_hal.h"
}

#define MINIMP4_IMPLEMENTATION
#include "minimp4.h"

#include "wels/codec_api.h"
#include "wels/codec_app_def.h"
#include "wels/codec_def.h"

#include "audio_pcm.h"   // mp3/wav/flac/ogg-vorbis → s16 PCM
#include "aac_dec.h"     // AAC (in mp4/m4a) → s16 PCM (fdk-aac)
#include "opus_dec.h"    // ogg-opus → s16 PCM (libopus + libogg)
#include "webm_dec.h"    // WebM (VP8/VP9 video + Opus audio) — libnestegg + libvpx
#include "hevc_dec.h"    // HEVC/H.265 (in mp4/mov) — libde265
#include "av1_dec.h"     // AV1 (in mp4 and WebM/mkv) — dav1d
#include "draudio/dr_mp3.h"  // drmp3dec: low-level streaming MP3 frame decoder (radio)

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// ── tiny helpers (no iostream) ──────────────────────────────────────────
static unsigned cstr_len(const char* s) { unsigned n = 0; while (s[n]) n++; return n; }
static void logmsg(const char* s) { hal_log(1, s, cstr_len(s)); }

static inline uint8_t clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

// Minimal JSON string-field extractor: finds "key":"value" and copies the
// (unescaped-enough) value. Returns length, or -1 if absent.
static int json_str(const char* j, unsigned n, const char* key,
                    char* out, unsigned outcap) {
  char pat[48];
  unsigned kl = cstr_len(key);
  if (kl + 4 >= sizeof(pat)) return -1;
  pat[0] = '"'; memcpy(pat + 1, key, kl);
  pat[kl + 1] = '"'; pat[kl + 2] = ':'; pat[kl + 3] = '"'; pat[kl + 4] = 0;
  unsigned pl = kl + 4;
  for (unsigned i = 0; i + pl <= n; i++) {
    bool m = true;
    for (unsigned k = 0; k < pl; k++) if (j[i + k] != pat[k]) { m = false; break; }
    if (!m) continue;
    unsigned s = i + pl, o = 0;
    while (s < n && j[s] != '"' && o + 1 < outcap) {
      if (j[s] == '\\' && s + 1 < n) s++; // skip escape
      out[o++] = j[s++];
    }
    out[o] = 0;
    return (int)o;
  }
  return -1;
}

static bool json_has_type(const char* j, unsigned n, const char* type) {
  char want[64];
  unsigned tl = cstr_len(type);
  if (tl + 10 >= sizeof(want)) return false;
  // "type":"<type>"
  memcpy(want, "\"type\":\"", 8);
  memcpy(want + 8, type, tl);
  want[8 + tl] = '"'; want[9 + tl] = 0;
  unsigned wl = 9 + tl;
  for (unsigned i = 0; i + wl <= n; i++) {
    bool m = true;
    for (unsigned k = 0; k < wl; k++) if (j[i + k] != want[k]) { m = false; break; }
    if (m) return true;
  }
  return false;
}

// ── decoder + demux state ───────────────────────────────────────────────
static uint8_t*       g_file = nullptr;
static int64_t        g_fileSize = 0;
static MP4D_demux_t   g_mp4;
static bool           g_mp4open = false;
static int            g_vt = -1;
static int            g_lenSize = 4;        // NAL length prefix (avcC/hvcC)
static unsigned       g_sample = 0;
static unsigned       g_sampleCount = 0;
static uint32_t       g_timescale = 1;
static ISVCDecoder*   g_dec = nullptr;      // H.264 (openh264)
static HevcDec*       g_hevc = nullptr;     // H.265 (libde265)
static Av1Dec*        g_av1 = nullptr;      // AV1 (dav1d)

static uint8_t*       g_hdr = nullptr;       // SPS+PPS as annex-b
static int            g_hdrLen = 0;
static uint8_t*       g_au = nullptr;        // scratch access-unit buffer
static int            g_auCap = 0;
static uint8_t*       g_rgba = nullptr;      // scratch RGBA frame
static int            g_rgbaCap = 0;

static bool           g_playing = false;
static bool           g_ended = false;
static bool           g_configSent = false;
static int            g_outW = 0, g_outH = 0;

// Wall-clock play clock (advances only while playing), and the pts of the
// next frame we will emit. We decode while nextPts <= playClock + LEAD_MS.
static double         g_playClockMs = 0;
static uint64_t       g_lastTickMs = 0;
static double         g_nextPtsMs = 0;

static const double   LEAD_MS = 200.0;     // decode this far ahead of play
static const int      MAX_BATCH = 8;       // cap frames decoded per tick

// Thumbnail scan: decode (and emit) up to this many frames from the start as
// fast as possible, ignoring the play clock, so the host can pick the most
// attractive frame for the poster. Triggered by a "video.scan" message.
static int            g_scan = 0;          // remaining frames (sequential fallback)
static const int      SCAN_FRAMES = 48;

// Keyframe poster scan: keyframes (IDR) decode standalone, so we sample them
// spread across the WHOLE clip (not just the start) to give the host the best
// choice of poster frame.
static bool           g_scanKf = false;
static unsigned*      g_kf = nullptr;       // keyframe sample indices
static int            g_kfCount = 0;
static int            g_kfStep = 1;
static int            g_kfCursor = 0;
static const int      KF_TARGET = 24;       // ~posters sampled across the clip

// ── Audio path (mp3/wav/flac/ogg-vorbis) ─────────────────────────────────
static bool           g_audioMode = false;
static bool           g_audioDone = false;
static AudioDec       g_adec;
static int16_t*       g_apcm = nullptr;     // decode scratch (interleaved s16)
static const int      AUDIO_CHUNK = 4096;   // frames per audio_read
static long long      g_audioFrames = 0;    // frames emitted so far
static const double   AUDIO_LEAD_MS = 500.0;
// Pure-audio (music/radio) keeps a deep buffer so background playback stays
// gapless across the coarse (~2 s) native heartbeat when the screen is off.
static const double   MUSIC_LEAD_MS = 4000.0;
static const int      MUSIC_BUDGET = 80;     // frames/chunks decodable per tick

// ── AAC audio inside mp4/m4a (alongside video, or audio-only m4a) ─────────
static int            g_at = -1;            // audio track index (-1 = none)
static bool           g_aHasAudio = false;
static bool           g_aDone = false;
static bool           g_vDone = false;      // video track exhausted
static AacDec         g_aac;
static unsigned       g_aSampleCount = 0;
static unsigned       g_aSample = 0;
static int            g_aRate = 0, g_aCh = 0;
static long long      g_aFrames = 0;        // AAC frames emitted
static int16_t        g_aacOut[16384];      // up to 2048 frames * 8ch

// ── Opus (ogg-opus) ──────────────────────────────────────────────────────
static bool           g_opusMode = false;
static bool           g_opusDone = false;
static OpusDec*       g_opus = nullptr;
static int            g_opusRate = 0, g_opusCh = 0;
static long long      g_opusFrames = 0;
static const int      OPUS_READ_FRAMES = 5760; // max opus frame @48k (120ms)
static const int      SNIFF_OPUS = 100;        // sniff_audio sentinel for opus

// ── WebM (VP8/VP9 video + Opus audio) ────────────────────────────────────
static bool           g_webmMode = false;
static bool           g_webmDone = false;
static WebmDec*       g_webm = nullptr;
static int            g_webmRate = 0, g_webmCh = 0;
static long long      g_webmAFrames = 0;       // audio frames emitted (for pts)

// ── Music / Films / Radio (full-wapp mode) ───────────────────────────────
// Opened from the launcher this wapp is a media player with three tabs. Music
// and Films are folder-tree browsers (the user adds base folders, then drills
// in); a tapped track/film plays. Radio plays online streams. (When embedded
// inline in a chat it just plays the one file it is handed — g_fileLoaded marks
// that case so the launcher UI logic never runs.)
#define MAX_FOLDERS 32
#define MAX_TRACKS  4000
#define NAV_MAX     16

static char           g_musicFolders[MAX_FOLDERS][512]; // base music folders
static int            g_musicFolderCount = 0;
static char           g_filmFolders[MAX_FOLDERS][512];  // base film folders
static int            g_filmFolderCount = 0;

static char           g_mstack[NAV_MAX][512];  // music browse path stack
static int            g_mdepth = 0;
static char           g_fstack[NAV_MAX][512];  // film browse path stack
static int            g_fdepth = 0;

static int            g_pickTarget = 0;        // 0 = music folder, 1 = film folder

// Cached recursive index of all media under the base folders, for live search
// (a per-keystroke filesystem walk would be far too slow). Built lazily and
// invalidated when folders change. g_idxType: -1 none, 0 music, 1 film.
#define IDX_MAX 4000
static char           g_idxPool[768 * 1024];
static int            g_idxLen = 0;
static int            g_idxOff[IDX_MAX];
static unsigned char  g_idxDir[IDX_MAX];
static int            g_idxCount = 0;
static int            g_idxType = -1;
static char           g_stQuery[96] = "";       // active radio station filter

// Snapshot of the currently-playing folder's audio files (drives next/prev).
static char           g_trkPool[768 * 1024];   // packed null-terminated paths
static int            g_trkPoolLen = 0;
static int            g_trkOff[MAX_TRACKS];     // offset into g_trkPool
static int            g_trkCount = 0;
static int            g_musicMode = 0;         // playing from the music library?
static int            g_curTrack = -1;         // index of the current track
static bool           g_fileLoaded = false;    // inline file.open seen → no UI
static int            g_shuffle = 0, g_repeat = 1;
static long long      g_curDurMs = 0;          // duration of the current track
static uint32_t       g_rng = 0x9e3779b9u;     // shuffle PRNG state
static char           g_msg[300 * 1024];       // scratch for large UI messages
static char           g_ls[65536];             // scratch for one listdir result

static void music_advance(int dir);
static void play_index(int i);
static void render_nowplaying(void);
static bool music_playback_ended(void);
static void render_music_tree(void);
static void render_film_tree(void);

// ── Radio (online streams) ───────────────────────────────────────────────
#define MAX_STATIONS 64
static char           g_stName[MAX_STATIONS][128];
static char           g_stUrl[MAX_STATIONS][256];
static int            g_stationCount = 0;
static int            g_radioMode = 0;
static int            g_radioHandle = -1;      // hal_http_stream handle
static int            g_curStation = -1;
static drmp3dec       g_mp3dec;
static unsigned char  g_rdbuf[64 * 1024];      // raw mp3 byte buffer (sliding)
static int            g_rdlen = 0;
static int16_t        g_rdpcm[DRMP3_MAX_SAMPLES_PER_FRAME];
static int            g_radioRate = 0, g_radioCh = 0;
static long long      g_radioFrames = 0;
static char           g_radioTitle[160] = "";  // ICY StreamTitle
static char           g_pendName[128] = "";     // add-station: pending name
static void render_stations(void);
static void radio_stop(void);

// minimp4 memory read callback.
static int read_cb(int64_t off, void* buf, size_t size, void* token) {
  (void)token;
  if (off < 0 || off + (int64_t)size > g_fileSize) return -1;
  memcpy(buf, g_file + off, size);
  return 0;
}

static void ensure_cap(uint8_t** buf, int* cap, int need) {
  if (*cap >= need) return;
  int n = *cap ? *cap : 4096;
  while (n < need) n *= 2;
  *buf = (uint8_t*)realloc(*buf, n);
  *cap = n;
}

static void teardown() {
  if (g_dec) { g_dec->Uninitialize(); WelsDestroyDecoder(g_dec); g_dec = nullptr; }
  if (g_hevc) { hevc_close(g_hevc); g_hevc = nullptr; }
  if (g_av1) { av1_close(g_av1); g_av1 = nullptr; }
  g_lenSize = 4;
  if (g_mp4open) { MP4D_close(&g_mp4); g_mp4open = false; }
  if (g_file) { free(g_file); g_file = nullptr; }
  g_fileSize = 0; g_vt = -1; g_sample = 0; g_sampleCount = 0; g_timescale = 1;
  if (g_hdr) { free(g_hdr); g_hdr = nullptr; } g_hdrLen = 0;
  if (g_kf) { free(g_kf); g_kf = nullptr; }
  g_kfCount = 0; g_kfStep = 1; g_kfCursor = 0; g_scanKf = false; g_scan = 0;
  if (g_adec.h) audio_close(&g_adec);
  if (g_opus) { opus_dec_close(g_opus); g_opus = nullptr; }
  g_opusMode = false; g_opusDone = false; g_opusFrames = 0;
  g_opusRate = 0; g_opusCh = 0;
  if (g_webm) { webm_close(g_webm); g_webm = nullptr; }
  g_webmMode = false; g_webmDone = false; g_webmAFrames = 0;
  g_webmRate = 0; g_webmCh = 0;
  if (g_apcm) { free(g_apcm); g_apcm = nullptr; }
  g_audioMode = false; g_audioDone = false; g_audioFrames = 0;
  if (g_aac.h) aac_close(&g_aac);
  g_at = -1; g_aHasAudio = false; g_aDone = false; g_vDone = false;
  g_aSampleCount = 0; g_aSample = 0; g_aRate = 0; g_aCh = 0; g_aFrames = 0;
  g_playing = false; g_ended = false; g_configSent = false;
  g_outW = g_outH = 0; g_playClockMs = 0; g_nextPtsMs = 0;
  radio_stop();
}

// Slurp the whole file into g_file via the host file HAL.
static bool slurp(const char* path, unsigned plen) {
  int h = hal_file_open(path, plen, 0 /*read*/);
  if (h < 0) return false;
  int cap = 1 << 16, len = 0;
  uint8_t* b = (uint8_t*)malloc(cap);
  for (;;) {
    if (len == cap) { cap *= 2; b = (uint8_t*)realloc(b, cap); }
    int r = hal_file_read(h, (char*)b + len, cap - len);
    if (r < 0) { free(b); hal_file_close(h); return false; }
    if (r == 0) break;
    len += r;
  }
  hal_file_close(h);
  g_file = b; g_fileSize = len;
  return true;
}

static void append_annexb(uint8_t** buf, int* cap, int* len,
                          const uint8_t* nal, int n) {
  static const uint8_t sc[4] = {0, 0, 0, 1};
  ensure_cap(buf, cap, *len + 4 + n);
  memcpy(*buf + *len, sc, 4); *len += 4;
  memcpy(*buf + *len, nal, n); *len += n;
}

static void build_header() {
  g_hdrLen = 0;
  int cap = 0; uint8_t* h = nullptr; int len = 0;
  for (int k = 0;; k++) {
    int n = 0; const void* p = MP4D_read_sps(&g_mp4, (unsigned)g_vt, k, &n);
    if (!p || n <= 0) break;
    append_annexb(&h, &cap, &len, (const uint8_t*)p, n);
  }
  for (int k = 0;; k++) {
    int n = 0; const void* p = MP4D_read_pps(&g_mp4, (unsigned)g_vt, k, &n);
    if (!p || n <= 0) break;
    append_annexb(&h, &cap, &len, (const uint8_t*)p, n);
  }
  g_hdr = h; g_hdrLen = len;
}

static void send_media_meta(long long durMs, bool hasV, bool hasA,
                            int rate, int ch); // defined below

// Open the mp4/H.264 video already slurped into g_file (the dispatcher slurps
// + sniffs first). build_keyframes/decode use g_file.
static bool open_video_loaded(void) {
  memset(&g_mp4, 0, sizeof(g_mp4));
  if (MP4D_open(&g_mp4, read_cb, nullptr, g_fileSize) != 1) {
    logmsg("[mp4player] not a valid mp4"); return false;
  }
  g_mp4open = true;
  g_vt = -1; g_at = -1;
  for (unsigned i = 0; i < g_mp4.track_count; i++) {
    unsigned ht = g_mp4.track[i].handler_type;
    if (ht == MP4D_HANDLER_TYPE_VIDE && g_vt < 0) g_vt = (int)i;
    else if (ht == MP4D_HANDLER_TYPE_SOUN && g_at < 0) g_at = (int)i;
  }

  // Video track → H.264 (openh264) or H.265 (libde265) decoder.
  if (g_vt >= 0) {
    MP4D_track_t* tr = &g_mp4.track[g_vt];
    g_sampleCount = tr->sample_count;
    g_timescale = tr->timescale ? tr->timescale : 1;
    g_outW = (int)tr->SampleDescription.video.width;
    g_outH = (int)tr->SampleDescription.video.height;
    if (tr->object_type_indication == MP4_OBJECT_TYPE_HEVC) {
      if (!tr->dsi || tr->dsi_bytes == 0) { logmsg("[mp4player] hevc: no hvcC record"); return false; }
      int ls = 4;
      g_hevc = hevc_open(tr->dsi, (int)tr->dsi_bytes, &ls);
      if (!g_hevc) { logmsg("[mp4player] hevc decoder open failed"); return false; }
      g_lenSize = ls;
      logmsg("[mp4player] hevc track (libde265)");
    } else if (tr->object_type_indication == MP4_OBJECT_TYPE_AV1) {
      g_av1 = av1_open(tr->dsi, (int)tr->dsi_bytes);
      if (!g_av1) { logmsg("[mp4player] av1 decoder open failed"); return false; }
      logmsg("[mp4player] av1 track (dav1d)");
    } else {
      g_lenSize = 4;
      build_header();
      if (WelsCreateDecoder(&g_dec) != 0 || !g_dec) { logmsg("[mp4player] decoder create failed"); return false; }
      SDecodingParam dp; memset(&dp, 0, sizeof(dp));
      dp.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
      dp.eEcActiveIdc = ERROR_CON_DISABLE;
      if (g_dec->Initialize(&dp) != 0) { logmsg("[mp4player] decoder init failed"); return false; }
    }
  }

  // Audio track → AAC decoder (so videos get sound; m4a plays).
  if (g_at >= 0) {
    MP4D_track_t* atr = &g_mp4.track[g_at];
    if (atr->object_type_indication == 0x40 /*AAC*/ && atr->dsi &&
        atr->dsi_bytes > 0 &&
        aac_open(&g_aac, atr->dsi, (int)atr->dsi_bytes)) {
      g_aHasAudio = true;
      g_aSampleCount = atr->sample_count;
      g_aRate = (int)atr->SampleDescription.audio.samplerate_hz;
      g_aCh = (int)atr->SampleDescription.audio.channelcount;
    }
  }

  if (g_vt < 0 && !g_aHasAudio) { logmsg("[mp4player] no playable track"); return false; }

  g_sample = 0; g_aSample = 0; g_aFrames = 0;
  g_playClockMs = 0; g_nextPtsMs = 0;
  g_ended = false; g_vDone = (g_vt < 0); g_aDone = !g_aHasAudio;
  g_configSent = false;
  g_playing = true;
  g_lastTickMs = hal_time_ms();
  long long durMs = (g_mp4.timescale > 0)
      ? (long long)g_mp4.duration_lo * 1000 / g_mp4.timescale : 0;
  send_media_meta(durMs, g_vt >= 0, g_aHasAudio, g_aRate, g_aCh);
  logmsg("[mp4player] playing");
  return true;
}

// Convert an I420 frame (separate Y/U/V planes with their strides) to RGBA and
// push it to the host sink. BT.601 limited-range — shared by the H.264 (openh264)
// and VP8/VP9 (libvpx) paths. [sy] = luma stride, [sc] = chroma stride.
static void emit_yuv420(const uint8_t* Y, const uint8_t* U, const uint8_t* V,
                        int sy, int sc, int w, int h, double ptsMs) {
  if (w <= 0 || h <= 0) return;
  if (!g_configSent) {
#ifndef MP4P_NO_SINK
    hal_video_config(w, h, 0 /*RGBA8888*/);
#endif
    g_configSent = true;
  }
  ensure_cap(&g_rgba, &g_rgbaCap, w * h * 4);
  for (int y = 0; y < h; y++) {
    const uint8_t* yr = Y + y * sy;
    const uint8_t* ur = U + (y >> 1) * sc;
    const uint8_t* vr = V + (y >> 1) * sc;
    uint8_t* o = g_rgba + (size_t)y * w * 4;
    for (int x = 0; x < w; x++) {
      int c = yr[x] - 16;
      int d = ur[x >> 1] - 128;
      int e = vr[x >> 1] - 128;
      o[0] = clamp8((298 * c + 409 * e + 128) >> 8);
      o[1] = clamp8((298 * c - 100 * d - 208 * e + 128) >> 8);
      o[2] = clamp8((298 * c + 516 * d + 128) >> 8);
      o[3] = 255;
      o += 4;
    }
  }
#ifndef MP4P_NO_SINK
  hal_video_frame(g_rgba, (uint32_t)(w * h * 4), w, h, 0, (int32_t)ptsMs);
#else
  (void)ptsMs;
#endif
}

static void emit_frame(uint8_t* pd[3], SBufferInfo& bi, double ptsMs) {
  if (bi.iBufferStatus != 1) return;
  int w = bi.UsrData.sSystemBuffer.iWidth;
  int h = bi.UsrData.sSystemBuffer.iHeight;
  int sy = bi.UsrData.sSystemBuffer.iStride[0];
  int sc = bi.UsrData.sSystemBuffer.iStride[1];
  emit_yuv420(pd[0], pd[1], pd[2], sy, sc, w, h, ptsMs);
}

// Pull every picture the HEVC decoder has ready and push them to the sink.
static void hevc_drain() {
  HevcFrame fr;
  while (hevc_pull(g_hevc, &fr)) {
    emit_yuv420(fr.planes[0], fr.planes[1], fr.planes[2],
                fr.strides[0], fr.strides[1], fr.w, fr.h, fr.ptsMs);
  }
}

// Decode one sample (the next one) and emit its frame if ready.
static bool decode_one() {
  if (g_sample >= g_sampleCount) return false;
  unsigned bytes = 0, ts = 0, dur = 0;
  MP4D_file_offset_t ofs = MP4D_frame_offset(&g_mp4, (unsigned)g_vt, g_sample, &bytes, &ts, &dur);
  if (g_hevc) {
    double thisPts = g_nextPtsMs;
    g_nextPtsMs += (double)dur * 1000.0 / (double)g_timescale;
    g_sample++;
    hevc_push_sample(g_hevc, g_file + ofs, (int)bytes, thisPts);
    hevc_drain();
    return true;
  }
  if (g_av1) {
    double thisPts = g_nextPtsMs;
    g_nextPtsMs += (double)dur * 1000.0 / (double)g_timescale;
    g_sample++;
    // mp4 av01 samples are raw OBU temporal units — feed as-is.
    av1_push(g_av1, g_file + ofs, (int)bytes, thisPts);
    Av1Frame fr;
    while (av1_pull(g_av1, &fr)) {
      emit_yuv420(fr.planes[0], fr.planes[1], fr.planes[2],
                  fr.strides[0], fr.strides[1], fr.w, fr.h, fr.ptsMs);
    }
    return true;
  }
  int len = 0;
  if (g_sample == 0 && g_hdrLen > 0) {
    ensure_cap(&g_au, &g_auCap, g_hdrLen);
    memcpy(g_au, g_hdr, g_hdrLen); len = g_hdrLen;
  }
  const uint8_t* sp = g_file + ofs;
  unsigned i = 0;
  while (i + g_lenSize <= bytes) {
    uint32_t nl = 0;
    for (int b = 0; b < g_lenSize; b++) nl = (nl << 8) | sp[i + b];
    i += g_lenSize;
    if (i + nl > bytes) break;
    append_annexb(&g_au, &g_auCap, &len, sp + i, (int)nl);
    i += nl;
  }
  double thisPts = g_nextPtsMs;
  g_nextPtsMs += (double)dur * 1000.0 / (double)g_timescale;
  g_sample++;

  uint8_t* pd[3] = {nullptr, nullptr, nullptr};
  SBufferInfo bi; memset(&bi, 0, sizeof(bi));
  g_dec->DecodeFrameNoDelay(g_au, len, pd, &bi);
  emit_frame(pd, bi, thisPts);
  return true;
}

static void flush_tail() {
  if (g_hevc) {
    hevc_flush(g_hevc); // end-of-stream → reordered pictures drain
    hevc_drain();
    return;
  }
  if (g_av1) {
    Av1Frame fr; // drain whatever dav1d still holds (max_frame_delay = 1)
    while (av1_pull(g_av1, &fr)) {
      emit_yuv420(fr.planes[0], fr.planes[1], fr.planes[2],
                  fr.strides[0], fr.strides[1], fr.w, fr.h, fr.ptsMs);
    }
    return;
  }
  for (int k = 0; k < 8; k++) {
    uint8_t* pd[3] = {nullptr, nullptr, nullptr};
    SBufferInfo bi; memset(&bi, 0, sizeof(bi));
    g_dec->DecodeFrameNoDelay(nullptr, 0, pd, &bi);
    if (bi.iBufferStatus == 1) emit_frame(pd, bi, g_nextPtsMs);
  }
}

// True if sample s is a random-access point (decodable standalone).
// H.264: first slice NAL is type 5 (IDR). HEVC: NAL type 16..21 (IRAP:
// BLA/IDR/CRA). Reads only NAL headers, no decode.
static bool sample_is_keyframe(unsigned s) {
  // AV1: keyframe detection needs a frame-header bitstream parse — skip it so
  // build_keyframes() finds none and video.scan uses the sequential fallback
  // (every AV1 sample decodes in order, so the first frames scan works).
  if (g_av1) return false;
  unsigned bytes = 0, ts = 0, dur = 0;
  MP4D_file_offset_t ofs =
      MP4D_frame_offset(&g_mp4, (unsigned)g_vt, s, &bytes, &ts, &dur);
  const uint8_t* sp = g_file + ofs;
  unsigned i = 0;
  while (i + g_lenSize <= bytes) {
    uint32_t nl = 0;
    for (int b = 0; b < g_lenSize; b++) nl = (nl << 8) | sp[i + b];
    i += g_lenSize;
    if (nl == 0 || i + nl > bytes) break;
    if (g_hevc) {
      uint8_t t = (sp[i] >> 1) & 0x3F;
      if (t >= 16 && t <= 21) return true; // IRAP (BLA/IDR/CRA)
      if (t <= 9) return false;            // non-IRAP slice
    } else {
      uint8_t t = sp[i] & 0x1F;
      if (t == 5) return true;  // IDR slice
      if (t == 1) return false; // non-IDR slice — not a keyframe
    }
    i += nl;                  // VPS/SPS/PPS/SEI/AUD — keep looking
  }
  return false;
}

// Index every keyframe, then pick a stride so we sample ~KF_TARGET of them
// spread evenly across the whole clip.
static void build_keyframes() {
  if (g_kf) { free(g_kf); g_kf = nullptr; }
  g_kfCount = 0; g_kfCursor = 0; g_kfStep = 1;
  if (g_sampleCount == 0) return;
  g_kf = (unsigned*)malloc(sizeof(unsigned) * g_sampleCount);
  if (!g_kf) return;
  for (unsigned s = 0; s < g_sampleCount; s++) {
    if (sample_is_keyframe(s)) g_kf[g_kfCount++] = s;
  }
  if (g_kfCount > KF_TARGET) g_kfStep = g_kfCount / KF_TARGET;
}

// Decode one keyframe standalone (SPS/PPS + the IDR sample) and emit it.
static void decode_keyframe(unsigned s) {
  unsigned bytes = 0, ts = 0, dur = 0;
  MP4D_file_offset_t ofs =
      MP4D_frame_offset(&g_mp4, (unsigned)g_vt, s, &bytes, &ts, &dur);
  if (g_hevc) {
    // IRAPs decode standalone; output may lag one push — the tick's next
    // keyframe (or the end-of-scan flush) drains it.
    hevc_push_sample(g_hevc, g_file + ofs,
                     (int)bytes, (double)ts * 1000.0 / (double)g_timescale);
    hevc_drain();
    return;
  }
  int len = 0;
  ensure_cap(&g_au, &g_auCap, g_hdrLen);
  if (g_hdrLen > 0) { memcpy(g_au, g_hdr, g_hdrLen); len = g_hdrLen; }
  const uint8_t* sp = g_file + ofs;
  unsigned i = 0;
  while (i + g_lenSize <= bytes) {
    uint32_t nl = 0;
    for (int b = 0; b < g_lenSize; b++) nl = (nl << 8) | sp[i + b];
    i += g_lenSize;
    if (i + nl > bytes) break;
    append_annexb(&g_au, &g_auCap, &len, sp + i, (int)nl);
    i += nl;
  }
  uint8_t* pd[3] = {nullptr, nullptr, nullptr};
  SBufferInfo bi; memset(&bi, 0, sizeof(bi));
  g_dec->DecodeFrameNoDelay(g_au, len, pd, &bi);
  emit_frame(pd, bi, (double)ts * 1000.0 / (double)g_timescale);
}

// ── media.meta (tells the host duration + track presence) ────────────────
static int append_ll(char* b, int o, long long v) {
  if (v < 0) { b[o++] = '-'; v = -v; }
  char tmp[24]; int t = 0;
  if (v == 0) tmp[t++] = '0';
  while (v > 0) { tmp[t++] = (char)('0' + (int)(v % 10)); v /= 10; }
  while (t > 0) b[o++] = tmp[--t];
  return o;
}
static int append_str(char* b, int o, const char* s) {
  while (*s) b[o++] = *s++;
  return o;
}
static void send_media_meta(long long durMs, bool hasV, bool hasA,
                            int rate, int ch) {
  char m[192]; int o = 0;
  o = append_str(m, o, "{\"type\":\"media.meta\",\"durationMs\":");
  o = append_ll(m, o, durMs);
  o = append_str(m, o, ",\"hasVideo\":");
  o = append_str(m, o, hasV ? "true" : "false");
  o = append_str(m, o, ",\"hasAudio\":");
  o = append_str(m, o, hasA ? "true" : "false");
  o = append_str(m, o, ",\"sampleRate\":");
  o = append_ll(m, o, rate);
  o = append_str(m, o, ",\"channels\":");
  o = append_ll(m, o, ch);
  o = append_str(m, o, "}");
  hal_msg_send(m, (uint32_t)o);
}

// ── Audio path ───────────────────────────────────────────────────────────
// Recognize an audio container from its magic bytes. Returns AUDIO_FMT_* or 0
// (= not audio → the mp4/video path handles it). Extension is only a hint.
static int sniff_audio(const uint8_t* d, int n) {
  if (n < 12) return 0;
  if (d[0] == 'O' && d[1] == 'g' && d[2] == 'g' && d[3] == 'S') {
    // ogg: opus (OpusHead) vs vorbis ("vorbis" codec id).
    int lim = n < 256 ? n : 256;
    for (int i = 0; i + 8 <= lim; i++) {
      if (memcmp(d + i, "OpusHead", 8) == 0) return SNIFF_OPUS;
    }
    for (int i = 0; i + 6 <= lim; i++) {
      if (d[i] == 'v' && d[i + 1] == 'o' && d[i + 2] == 'r' &&
          d[i + 3] == 'b' && d[i + 4] == 'i' && d[i + 5] == 's')
        return AUDIO_FMT_VORBIS;
    }
    return AUDIO_FMT_VORBIS;
  }
  if (d[0] == 'R' && d[1] == 'I' && d[2] == 'F' && d[3] == 'F' &&
      d[8] == 'W' && d[9] == 'A' && d[10] == 'V' && d[11] == 'E')
    return AUDIO_FMT_WAV;
  if (d[0] == 'f' && d[1] == 'L' && d[2] == 'a' && d[3] == 'C')
    return AUDIO_FMT_FLAC;
  if (d[0] == 'I' && d[1] == 'D' && d[2] == '3') return AUDIO_FMT_MP3;
  if (d[0] == 0xFF && (d[1] & 0xE0) == 0xE0) return AUDIO_FMT_MP3; // frame sync
  return 0; // ftyp/mp4/ebml/other → video path
}

static bool open_audio(int fmt) {
  if (!audio_open(&g_adec, g_file, (int)g_fileSize, fmt)) {
    logmsg("[player] audio open failed");
    return false;
  }
  if (g_apcm) { free(g_apcm); g_apcm = nullptr; }
  g_apcm = (int16_t*)malloc((size_t)AUDIO_CHUNK * g_adec.channels * 2);
  if (!g_apcm) return false;
  g_audioFrames = 0; g_audioDone = false; g_audioMode = true;
  g_playing = true; g_playClockMs = 0; g_lastTickMs = hal_time_ms();
  long long durMs = (g_adec.total_frames > 0 && g_adec.rate > 0)
      ? g_adec.total_frames * 1000 / g_adec.rate : 0;
  send_media_meta(durMs, false, true, g_adec.rate, g_adec.channels);
  logmsg("[player] playing audio");
  return true;
}

static void audio_tick(void) {
  if (!g_playing || g_audioDone || !g_apcm) return;
  uint64_t now = hal_time_ms();
  double delta = (double)(now - g_lastTickMs);
  if (delta < 0) delta = 0;
  if (delta > 8000) delta = 8000;     // tolerate the coarse background heartbeat
  g_playClockMs += delta;
  g_lastTickMs = now;
  double ptsMs = (double)g_audioFrames * 1000.0 / (double)g_adec.rate;
  int budget = MUSIC_BUDGET;
  while (budget-- > 0 && ptsMs <= g_playClockMs + MUSIC_LEAD_MS) {
    int frames = audio_read(&g_adec, g_apcm, AUDIO_CHUNK);
    if (frames <= 0) {
      g_audioDone = true;
      g_playing = false;
      if (g_musicMode) { music_advance(1); return; }
      hal_video_end();
      logmsg("[player] audio end");
      break;
    }
    hal_audio_pcm((const uint8_t*)g_apcm,
                  (uint32_t)(frames * g_adec.channels * 2),
                  g_adec.rate, g_adec.channels, 0 /*s16*/, (int32_t)ptsMs);
    g_audioFrames += frames;
    ptsMs = (double)g_audioFrames * 1000.0 / (double)g_adec.rate;
  }
}

static bool open_opus(void) {
  int rate = 0, ch = 0;
  g_opus = opus_dec_open(g_file, (int)g_fileSize, &rate, &ch);
  if (!g_opus) { logmsg("[player] opus open failed"); return false; }
  g_opusRate = rate; g_opusCh = ch;
  if (g_apcm) { free(g_apcm); g_apcm = nullptr; }
  g_apcm = (int16_t*)malloc((size_t)OPUS_READ_FRAMES * ch * 2);
  if (!g_apcm) return false;
  g_opusFrames = 0; g_opusDone = false; g_opusMode = true;
  g_playing = true; g_playClockMs = 0; g_lastTickMs = hal_time_ms();
  send_media_meta(0, false, true, rate, ch); // duration unknown (streaming)
  logmsg("[player] playing opus");
  return true;
}

static void opus_tick(void) {
  if (!g_playing || g_opusDone || !g_apcm) return;
  uint64_t now = hal_time_ms();
  double delta = (double)(now - g_lastTickMs);
  if (delta < 0) delta = 0;
  if (delta > 8000) delta = 8000;     // tolerate the coarse background heartbeat
  g_playClockMs += delta;
  g_lastTickMs = now;
  double ptsMs = (double)g_opusFrames * 1000.0 / (double)g_opusRate;
  int budget = MUSIC_BUDGET;
  while (budget-- > 0 && ptsMs <= g_playClockMs + MUSIC_LEAD_MS) {
    int frames = opus_dec_read(g_opus, g_apcm, OPUS_READ_FRAMES);
    if (frames <= 0) {
      g_opusDone = true;
      g_playing = false;
      if (g_musicMode) { music_advance(1); return; }
      hal_video_end();
      logmsg("[player] opus end");
      break;
    }
    hal_audio_pcm((const uint8_t*)g_apcm,
                  (uint32_t)(frames * g_opusCh * 2),
                  g_opusRate, g_opusCh, 0 /*s16*/, (int32_t)ptsMs);
    g_opusFrames += frames;
    ptsMs = (double)g_opusFrames * 1000.0 / (double)g_opusRate;
  }
}

// Decode mp4/m4a AAC audio access units up to the play-clock lead; emit PCM.
static void decode_audio_mp4(void) {
  if (!g_aHasAudio || g_aDone) return;
  double aPts = (g_aRate > 0) ? (double)g_aFrames * 1000.0 / (double)g_aRate : 0;
  int budget = 16;
  while (budget-- > 0 && aPts <= g_playClockMs + AUDIO_LEAD_MS) {
    if (g_aSample >= g_aSampleCount) { g_aDone = true; break; }
    unsigned ab = 0, ats = 0, ad = 0;
    MP4D_file_offset_t ofs =
        MP4D_frame_offset(&g_mp4, (unsigned)g_at, g_aSample, &ab, &ats, &ad);
    g_aSample++;
    int ch = 0, rate = 0;
    int frames = aac_decode(&g_aac, g_file + ofs, (int)ab, g_aacOut,
                            (int)(sizeof(g_aacOut) / 2), &ch, &rate);
    if (frames > 0 && ch > 0 && rate > 0) {
      hal_audio_pcm((const uint8_t*)g_aacOut, (uint32_t)(frames * ch * 2),
                    rate, ch, 0 /*s16*/, (int32_t)aPts);
      g_aRate = rate;
      g_aCh = ch;
      g_aFrames += frames;
      aPts = (double)g_aFrames * 1000.0 / (double)g_aRate;
    }
  }
}

// ── WebM (VP8/VP9 + Opus) ────────────────────────────────────────────────
static bool is_webm(const uint8_t* d, int n) {
  return n >= 4 && d[0] == 0x1A && d[1] == 0x45 && d[2] == 0xDF && d[3] == 0xA3;
}

static bool open_webm(void) {
  g_webm = webm_open(g_file, (int)g_fileSize);
  if (!g_webm) { logmsg("[player] webm open failed"); return false; }
  bool hasV = webm_has_video(g_webm);
  bool hasA = webm_has_audio(g_webm);
  if (hasA) {
    webm_audio_info(g_webm, &g_webmRate, &g_webmCh);
    if (g_apcm) { free(g_apcm); g_apcm = nullptr; }
    int ch = g_webmCh > 0 ? g_webmCh : 2;
    g_apcm = (int16_t*)malloc((size_t)OPUS_READ_FRAMES * ch * 2);
    if (!g_apcm) { webm_close(g_webm); g_webm = nullptr; return false; }
  }
  g_webmAFrames = 0; g_webmDone = false; g_webmMode = true;
  g_playing = true; g_playClockMs = 0; g_lastTickMs = hal_time_ms();
  send_media_meta(webm_duration_ms(g_webm), hasV, hasA, g_webmRate, g_webmCh);
  logmsg("[player] playing webm");
  return true;
}

// Pump WebM packets up to the play-clock lead, emitting video frames and Opus
// PCM with their own timestamps (the host re-syncs to the audio master clock).
static void webm_tick(void) {
  if (!g_playing || g_webmDone || !g_webm) return;
  uint64_t now = hal_time_ms();
  double delta = (double)(now - g_lastTickMs);
  if (delta < 0) delta = 0;
  if (delta > 1000) delta = 1000;
  g_playClockMs += delta;
  g_lastTickMs = now;
  int budget = 32;
  while (budget-- > 0) {
    double nextMs = 0;
    if (!webm_next_tstamp(g_webm, &nextMs)) {
      g_webmDone = true;
      g_playing = false;
      if (g_musicMode) { music_advance(1); return; }
      hal_video_end();
      logmsg("[player] webm end");
      break;
    }
    if (nextMs > g_playClockMs + AUDIO_LEAD_MS) break; // decoded far enough ahead
    WebmUnit u;
    if (!webm_consume(g_webm, &u, g_apcm, OPUS_READ_FRAMES)) {
      g_webmDone = true;
      g_playing = false;
      if (g_musicMode) { music_advance(1); return; }
      hal_video_end();
      break;
    }
    if (u.type == 1) {
      emit_yuv420(u.planes[0], u.planes[1], u.planes[2], u.strides[0],
                  u.strides[1], u.vw, u.vh, u.ptsMs);
    } else if (u.type == 2) {
      hal_audio_pcm((const uint8_t*)g_apcm, (uint32_t)(u.aframes * g_webmCh * 2),
                    g_webmRate, g_webmCh, 0 /*s16*/, (int32_t)u.ptsMs);
      g_webmAFrames += u.aframes;
    }
  }
}

// ── Music library helpers ────────────────────────────────────────────────
static bool str_eq(const char* a, const char* b) {
  while (*a && *b) { if (*a != *b) return false; a++; b++; }
  return *a == *b;
}

// JSON-escape and append a string (handles " \ and control chars).
static int append_jesc(char* b, int o, const char* s) {
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') { b[o++] = '\\'; b[o++] = (char)c; }
    else if (c == '\n') { b[o++] = '\\'; b[o++] = 'n'; }
    else if (c == '\t') { b[o++] = '\\'; b[o++] = 't'; }
    else if (c < 0x20) { b[o++] = ' '; }
    else b[o++] = (char)c;
  }
  return o;
}

static bool has_substr(const char* hay, const char* needle) {
  for (const char* p = hay; *p; p++) {
    const char* a = p; const char* b = needle;
    while (*a && *b && *a == *b) { a++; b++; }
    if (!*b) return true;
  }
  return false;
}

// Parse a JSON bool field ("key":true). Returns 1/0; def if absent.
static int json_bool(const char* j, unsigned n, const char* key, int def) {
  char pat[40];
  unsigned kl = cstr_len(key);
  if (kl + 3 >= sizeof(pat)) return def;
  pat[0] = '"'; memcpy(pat + 1, key, kl);
  pat[kl + 1] = '"'; pat[kl + 2] = ':'; pat[kl + 3] = 0;
  unsigned pl = kl + 3;
  for (unsigned i = 0; i + pl <= n; i++) {
    bool m = true;
    for (unsigned k = 0; k < pl; k++) if (j[i + k] != pat[k]) { m = false; break; }
    if (!m) continue;
    unsigned s = i + pl;
    while (s < n && (j[s] == ' ' || j[s] == '\t')) s++;
    return (s < n && j[s] == 't') ? 1 : 0;
  }
  return def;
}

// Last path segment (the file/folder name).
static const char* base_name(const char* path) {
  const char* b = path;
  for (const char* p = path; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
  return b;
}

static bool is_audio_ext(const char* p) {
  int len = (int)cstr_len(p), dot = -1;
  for (int i = len - 1; i >= 0 && p[i] != '/'; i--) { if (p[i] == '.') { dot = i; break; } }
  if (dot < 0) return false;
  char e[8]; int j = 0;
  for (int i = dot + 1; i < len && j < 7; i++) {
    char c = p[i]; if (c >= 'A' && c <= 'Z') c += 32; e[j++] = c;
  }
  e[j] = 0;
  static const char* exts[] = {"mp3", "m4a", "aac", "flac", "wav",
                               "ogg", "oga", "opus", nullptr};
  for (int k = 0; exts[k]; k++) if (str_eq(e, exts[k])) return true;
  return false;
}

static bool is_video_ext(const char* p) {
  int len = (int)cstr_len(p), dot = -1;
  for (int i = len - 1; i >= 0 && p[i] != '/'; i--) { if (p[i] == '.') { dot = i; break; } }
  if (dot < 0) return false;
  char e[8]; int j = 0;
  for (int i = dot + 1; i < len && j < 7; i++) {
    char c = p[i]; if (c >= 'A' && c <= 'Z') c += 32; e[j++] = c;
  }
  e[j] = 0;
  static const char* exts[] = {"mp4", "m4v", "mov", "webm", "mkv", nullptr};
  for (int k = 0; exts[k]; k++) if (str_eq(e, exts[k])) return true;
  return false;
}

// ── Folder list persistence (one KV key per tab, '\n'-separated) ──────────
static void load_folders(const char* key, char folders[][512], int* count) {
  *count = 0;
  static char b[MAX_FOLDERS * 512];
  uint32_t n = hal_kv_get(key, cstr_len(key), b, sizeof(b) - 1);
  b[n] = 0;
  unsigned i = 0;
  while (b[i] && *count < MAX_FOLDERS) {
    char* dst = folders[*count]; int j = 0;
    while (b[i] && b[i] != '\n' && j < 511) dst[j++] = b[i++];
    dst[j] = 0;
    if (b[i] == '\n') i++;
    if (j > 0) (*count)++;
  }
}

static void save_folders(const char* key, char folders[][512], int count) {
  static char b[MAX_FOLDERS * 512];
  int o = 0;
  for (int k = 0; k < count; k++) { o = append_str(b, o, folders[k]); b[o++] = '\n'; }
  b[o] = 0;
  hal_kv_set(key, cstr_len(key), b, (uint32_t)o);
}

// ── Folder-tree rendering (Music + Films) ─────────────────────────────────
// Row ids: "d:<path>" = a folder to drill into, "f:<path>" = a playable file,
// "up" = go up one level. At the root (depth 0) we list the base folders, each
// with a Remove menu. rmCmd is the command its Remove menu sends.
static void render_tree(const char* field, char stack[][512], int depth,
                        char folders[][512], int folderCount,
                        int wantAudio, const char* rmCmd) {
  char* m = g_msg; int o = 0; const int cap = (int)sizeof(g_msg);
  o = append_str(m, o, "{\"type\":\"ui.people.set\",\"field\":\"");
  o = append_str(m, o, field);
  o = append_str(m, o, "\",\"sections\":[{\"title\":\"");
  if (depth > 0) o = append_jesc(m, o, base_name(stack[depth - 1]));
  o = append_str(m, o, "\",\"items\":[");
  int first = 1;
  if (depth == 0) {
    if (folderCount == 0) {
      o = append_str(m, o, "{\"id\":\"none\",\"title\":\"No folders yet\","
                           "\"subtitle\":\"Tap Add folder\"}");
    } else {
      for (int k = 0; k < folderCount; k++) {
        if (!first) o = append_str(m, o, ","); first = 0;
        o = append_str(m, o, "{\"id\":\"d:"); o = append_jesc(m, o, folders[k]);
        o = append_str(m, o, "\",\"title\":\""); o = append_jesc(m, o, base_name(folders[k]));
        o = append_str(m, o, "\",\"subtitle\":\""); o = append_jesc(m, o, folders[k]);
        o = append_str(m, o, "\",\"menu\":[{\"label\":\"Remove\",\"value\":\"");
        o = append_str(m, o, rmCmd);
        o = append_str(m, o, "\"}]}");
      }
    }
  } else {
    o = append_str(m, o, "{\"id\":\"up\",\"title\":\"..\"}"); first = 0;
    const char* dir = stack[depth - 1];
    uint32_t n = hal_fs_listdir(dir, cstr_len(dir), g_ls, sizeof(g_ls) - 1);
    g_ls[n] = 0;
    for (int pass = 0; pass < 2; pass++) {       // folders first, then files
      const char* p = g_ls;
      while (*p && o < cap - 1400) {
        if (*p != '{') { p++; continue; }
        const char* e = p + 1; while (*e && *e != '}') e++;
        char slice[700]; int si = 0;
        for (const char* q = p; q <= e && si < (int)sizeof(slice) - 1; q++) slice[si++] = *q;
        slice[si] = 0;
        char path[512];
        int got = json_str(slice, (unsigned)si, "path", path, sizeof(path));
        bool isdir = has_substr(slice, "\"dir\":true");
        if (got > 0) {
          if (pass == 0 && isdir) {
            o = append_str(m, o, ",{\"id\":\"d:"); o = append_jesc(m, o, path);
            o = append_str(m, o, "\",\"title\":\""); o = append_jesc(m, o, base_name(path));
            o = append_str(m, o, "\"}");
          } else if (pass == 1 && !isdir &&
                     (wantAudio ? is_audio_ext(path) : is_video_ext(path))) {
            o = append_str(m, o, ",{\"id\":\"f:"); o = append_jesc(m, o, path);
            o = append_str(m, o, "\",\"title\":\""); o = append_jesc(m, o, base_name(path));
            o = append_str(m, o, "\"}");
          }
        }
        p = (*e) ? e + 1 : e;
      }
    }
  }
  o = append_str(m, o, "]}]}");
  m[o] = 0;
  hal_msg_send(m, (uint32_t)o);
}

static void render_music_tree(void) {
  render_tree("mtree", g_mstack, g_mdepth, g_musicFolders, g_musicFolderCount,
              1, "music_rmfolder");
}
static void render_film_tree(void) {
  render_tree("ftree", g_fstack, g_fdepth, g_filmFolders, g_filmFolderCount,
              0, "film_rmfolder");
}

// Snapshot the audio files of [dir] into the playing-track pool (in listdir
// order, matching the browse view), returning the index of [selPath] or -1.
static int snapshot_tracks(const char* dir, const char* selPath) {
  g_trkCount = 0; g_trkPoolLen = 0;
  if (!dir || !dir[0]) return -1;
  uint32_t n = hal_fs_listdir(dir, cstr_len(dir), g_ls, sizeof(g_ls) - 1);
  g_ls[n] = 0;
  int sel = -1;
  const char* p = g_ls;
  while (*p) {
    if (*p != '{') { p++; continue; }
    const char* e = p + 1; while (*e && *e != '}') e++;
    char slice[700]; int si = 0;
    for (const char* q = p; q <= e && si < (int)sizeof(slice) - 1; q++) slice[si++] = *q;
    slice[si] = 0;
    char path[512];
    int got = json_str(slice, (unsigned)si, "path", path, sizeof(path));
    bool isdir = has_substr(slice, "\"dir\":true");
    if (got > 0 && !isdir && is_audio_ext(path)) {
      int len = (int)cstr_len(path);
      if (g_trkCount < MAX_TRACKS && g_trkPoolLen + len + 1 <= (int)sizeof(g_trkPool)) {
        g_trkOff[g_trkCount] = g_trkPoolLen;
        for (int i = 0; i <= len; i++) g_trkPool[g_trkPoolLen++] = path[i];
        if (str_eq(path, selPath)) sel = g_trkCount;
        g_trkCount++;
      }
    }
    p = (*e) ? e + 1 : e;
  }
  return sel;
}

// Parent directory of a path (everything up to the last '/').
static void parent_dir(const char* path, char* out, int cap) {
  int last = -1;
  for (int i = 0; path[i]; i++) if (path[i] == '/') last = i;
  int n = last < 0 ? 0 : last;
  if (n > cap - 1) n = cap - 1;
  for (int i = 0; i < n; i++) out[i] = path[i];
  out[n] = 0;
}

// True if the path's basename contains [qlower] (already lower-cased).
static bool base_contains(const char* path, const char* qlower) {
  if (!qlower[0]) return true;
  const char* b = base_name(path);
  for (const char* p = b; *p; p++) {
    const char* a = p; const char* q = qlower;
    while (*a && *q) {
      char ca = *a; if (ca >= 'A' && ca <= 'Z') ca += 32;
      if (ca != *q) break;
      a++; q++;
    }
    if (!*q) return true;
  }
  return false;
}

// Case-insensitive substring match over the whole string.
static bool str_contains_ci(const char* hay, const char* qlower) {
  if (!qlower[0]) return true;
  for (const char* p = hay; *p; p++) {
    const char* a = p; const char* q = qlower;
    while (*a && *q) {
      char ca = *a; if (ca >= 'A' && ca <= 'Z') ca += 32;
      if (ca != *q) break;
      a++; q++;
    }
    if (!*q) return true;
  }
  return false;
}

// ── Search index (recursive, cached per media type) ───────────────────────
static void idx_add(const char* path, int isdir) {
  if (g_idxCount >= IDX_MAX) return;
  int len = (int)cstr_len(path);
  if (g_idxLen + len + 1 > (int)sizeof(g_idxPool)) return;
  g_idxOff[g_idxCount] = g_idxLen;
  for (int i = 0; i <= len; i++) g_idxPool[g_idxLen++] = path[i];
  g_idxDir[g_idxCount] = (unsigned char)isdir;
  g_idxCount++;
}

static void build_index(int type) {
  g_idxCount = 0; g_idxLen = 0; g_idxType = type;
  char (*folders)[512] = type ? g_filmFolders : g_musicFolders;
  int fc = type ? g_filmFolderCount : g_musicFolderCount;
  static char stack[256][512];
  for (int fi = 0; fi < fc && g_idxCount < IDX_MAX; fi++) {
    int top = 0;
    { int l = 0; const char* s = folders[fi];
      while (s[l] && l < 511) { stack[0][l] = s[l]; l++; } stack[0][l] = 0; top = 1; }
    int visited = 0;
    while (top > 0 && g_idxCount < IDX_MAX && visited < 3000) {
      top--; visited++;
      char dir[512]; { int l = 0;
        while (stack[top][l] && l < 511) { dir[l] = stack[top][l]; l++; } dir[l] = 0; }
      uint32_t n = hal_fs_listdir(dir, cstr_len(dir), g_ls, sizeof(g_ls) - 1);
      if (n == 0) continue;
      g_ls[n] = 0;
      const char* p = g_ls;
      while (*p) {
        if (*p != '{') { p++; continue; }
        const char* e = p + 1; while (*e && *e != '}') e++;
        char slice[700]; int si = 0;
        for (const char* q = p; q <= e && si < (int)sizeof(slice) - 1; q++) slice[si++] = *q;
        slice[si] = 0;
        char path[512];
        int got = json_str(slice, (unsigned)si, "path", path, sizeof(path));
        bool isdir = has_substr(slice, "\"dir\":true");
        if (got > 0) {
          if (isdir) {
            idx_add(path, 1);
            if (top < 256) { int l = 0;
              while (path[l] && l < 511) { stack[top][l] = path[l]; l++; } stack[top][l] = 0; top++; }
          } else if (type ? is_video_ext(path) : is_audio_ext(path)) {
            idx_add(path, 0);
          }
        }
        p = (*e) ? e + 1 : e;
      }
    }
  }
}

// Render search results into the [field] people list (folders drill in, files
// play). Builds/uses the cached index for [type].
static void render_search(const char* field, int type, const char* query) {
  if (g_idxType != type) build_index(type);
  char ql[96]; int j = 0;
  for (const char* q = query; *q && j < 95; q++) {
    char c = *q; if (c >= 'A' && c <= 'Z') c += 32; ql[j++] = c;
  }
  ql[j] = 0;
  char* m = g_msg; int o = 0; const int cap = (int)sizeof(g_msg);
  o = append_str(m, o, "{\"type\":\"ui.people.set\",\"field\":\"");
  o = append_str(m, o, field);
  o = append_str(m, o, "\",\"sections\":[{\"title\":\"Search results\",\"items\":[");
  int first = 1, matches = 0;
  for (int i = 0; i < g_idxCount && o < cap - 1400; i++) {
    const char* path = g_idxPool + g_idxOff[i];
    if (!base_contains(path, ql)) continue;
    matches++;
    if (!first) o = append_str(m, o, ","); first = 0;
    o = append_str(m, o, g_idxDir[i] ? "{\"id\":\"d:" : "{\"id\":\"f:");
    o = append_jesc(m, o, path);
    o = append_str(m, o, "\",\"title\":\""); o = append_jesc(m, o, base_name(path));
    if (!g_idxDir[i]) {
      char par[512]; parent_dir(path, par, sizeof(par));
      o = append_str(m, o, "\",\"subtitle\":\""); o = append_jesc(m, o, base_name(par));
    }
    o = append_str(m, o, "\"}");
  }
  if (matches == 0) {
    o = append_str(m, o, "{\"id\":\"none\",\"title\":\"No matches\"}");
  }
  o = append_str(m, o, "]}]}");
  m[o] = 0;
  hal_msg_send(m, (uint32_t)o);
}

static void set_field(const char* field, const char* value) {
  char* m = g_msg; int o = 0;
  o = append_str(m, o, "{\"type\":\"ui.field.set\",\"field\":\"");
  o = append_str(m, o, field);
  o = append_str(m, o, "\",\"value\":\"");
  o = append_jesc(m, o, value);
  o = append_str(m, o, "\"}");
  m[o] = 0;
  hal_msg_send(m, (uint32_t)o);
}

static int fmt_mmss(char* out, long long ms) {
  if (ms < 0) ms = 0;
  long long s = ms / 1000;
  long long mm = s / 60, ss = s % 60;
  int o = 0; o = append_ll(out, o, mm);
  out[o++] = ':';
  out[o++] = (char)('0' + (int)(ss / 10));
  out[o++] = (char)('0' + (int)(ss % 10));
  out[o] = 0;
  return o;
}

static long long music_elapsed_ms(void) {
  if (g_audioMode && g_adec.rate > 0)
    return g_audioFrames * 1000 / g_adec.rate;
  if (g_opusMode && g_opusRate > 0)
    return g_opusFrames * 1000 / g_opusRate;
  if (g_webmMode && g_webmRate > 0)
    return g_webmAFrames * 1000 / g_webmRate;
  if (g_aHasAudio && g_aRate > 0)
    return g_aFrames * 1000 / g_aRate;
  return 0;
}

// Tell the host the current media-session state so it can keep audio alive in
// the background and drive the Android lock-screen / notification controls.
static void send_media_session(void) {
  const char* state; const char* title = ""; char artist[160] = "";
  long long durMs = 0, posMs = 0; bool canNav = false;
  char tbuf[256];
  if (g_radioMode) {
    state = g_playing ? "playing" : "paused";
    title = (g_curStation >= 0 && g_curStation < g_stationCount) ? g_stName[g_curStation] : "Radio";
    int j = 0; while (g_radioTitle[j] && j < 159) { artist[j] = g_radioTitle[j]; j++; } artist[j] = 0;
    canNav = g_stationCount > 1;
  } else if (g_curTrack >= 0 && g_curTrack < g_trkCount) {
    state = music_playback_ended() ? "stopped" : (g_playing ? "playing" : "paused");
    const char* p = base_name(g_trkPool + g_trkOff[g_curTrack]);
    int j = 0; while (p[j] && j < 255) { tbuf[j] = p[j]; j++; } tbuf[j] = 0; title = tbuf;
    durMs = g_curDurMs; posMs = music_elapsed_ms();
    canNav = g_trkCount > 1;
  } else {
    state = "stopped";
  }
  char* m = g_msg; int o = 0;
  o = append_str(m, o, "{\"type\":\"media.session\",\"state\":\"");
  o = append_str(m, o, state);
  o = append_str(m, o, "\",\"title\":\""); o = append_jesc(m, o, title);
  o = append_str(m, o, "\",\"artist\":\""); o = append_jesc(m, o, artist);
  o = append_str(m, o, "\",\"durationMs\":"); o = append_ll(m, o, durMs);
  o = append_str(m, o, ",\"positionMs\":"); o = append_ll(m, o, posMs);
  o = append_str(m, o, ",\"canNext\":"); o = append_str(m, o, canNav ? "true" : "false");
  o = append_str(m, o, ",\"canPrev\":"); o = append_str(m, o, canNav ? "true" : "false");
  o = append_str(m, o, "}");
  m[o] = 0;
  hal_msg_send(m, (uint32_t)o);
}

static void render_nowplaying(void) {
  send_media_session();
  if (g_radioMode) {
    set_field("np_title",
              g_curStation >= 0 && g_curStation < g_stationCount ? g_stName[g_curStation] : "Radio");
    set_field("np_time", g_radioTitle[0] ? g_radioTitle : "Live");
    set_field("np_progress", "0");
    set_field("np_playing", g_playing ? "true" : "false");
    set_field("np_shuffle", "false");
    set_field("np_repeat", "false");
    return;
  }
  if (g_curTrack < 0 || g_curTrack >= g_trkCount) {
    set_field("np_title", "");
    set_field("np_time", "");
    set_field("np_progress", "0");
    set_field("np_playing", "false");
  } else {
    set_field("np_title", base_name(g_trkPool + g_trkOff[g_curTrack]));
    long long el = music_elapsed_ms();
    char t[48]; int o = fmt_mmss(t, el);
    if (g_curDurMs > 0) { t[o++] = ' '; t[o++] = '/'; t[o++] = ' '; fmt_mmss(t + o, g_curDurMs); }
    set_field("np_time", t);
    long long frac = (g_curDurMs > 0) ? (el * 1000 / g_curDurMs) : 0;
    if (frac < 0) frac = 0; if (frac > 1000) frac = 1000;
    char pg[12]; int po = append_ll(pg, 0, frac); pg[po] = 0;
    set_field("np_progress", pg);
    set_field("np_playing", (g_playing && !music_playback_ended()) ? "true" : "false");
  }
  set_field("np_shuffle", g_shuffle ? "true" : "false");
  set_field("np_repeat", g_repeat ? "true" : "false");
}

// ── Music playback ────────────────────────────────────────────────────────
static void play_index(int i) {
  if (i < 0 || i >= g_trkCount) return;
  char path[512];
  { const char* s = g_trkPool + g_trkOff[i]; int l = 0;
    while (s[l] && l < 511) { path[l] = s[l]; l++; } path[l] = 0; }
  int pl = (int)cstr_len(path);
  teardown();
  g_musicMode = 1; g_curTrack = i; g_curDurMs = 0;
  if (slurp(path, (unsigned)pl)) {
    int afmt = sniff_audio(g_file, (int)g_fileSize);
    if (afmt == SNIFF_OPUS) open_opus();
    else if (afmt) open_audio(afmt);
    else if (is_webm(g_file, (int)g_fileSize)) open_webm();
    else open_video_loaded();   // m4a / mp4-audio (AAC)
    // Duration for the now-playing readout.
    if (g_audioMode && g_adec.rate > 0)
      g_curDurMs = g_adec.total_frames * 1000 / g_adec.rate;
    else if (g_webm) g_curDurMs = webm_duration_ms(g_webm);
    else if (g_mp4open && g_mp4.timescale > 0)
      g_curDurMs = (long long)g_mp4.duration_lo * 1000 / g_mp4.timescale;
  }
  render_nowplaying();
}

static void music_advance(int dir) {
  if (g_trkCount == 0) { g_musicMode = 0; return; }
  int nxt;
  if (g_shuffle && g_trkCount > 1) {
    g_rng = g_rng * 1664525u + 1013904223u;
    nxt = (int)(g_rng % (uint32_t)g_trkCount);
    if (nxt == g_curTrack) nxt = (nxt + 1) % g_trkCount;
  } else {
    nxt = g_curTrack + dir;
  }
  if (nxt >= g_trkCount) {
    if (!g_repeat) { render_nowplaying(); return; } // reached the end, stop
    nxt = 0;
  }
  if (nxt < 0) nxt = g_trkCount - 1;
  play_index(nxt);
}

static bool music_playback_ended(void) {
  return g_audioDone || g_opusDone || g_webmDone || g_ended;
}

// Open one file by content sniff (audio or video). For video, open the Video
// surface screen and mount it. [inlineMode] = handed a single file inline in a
// chat / "open with" (no launcher UI); a film tapped from the Films tab is not.
static void open_media_file(const char* path, int pl, bool inlineMode) {
  if (inlineMode) g_fileLoaded = true;
  g_musicMode = 0;
  teardown();
  bool mountVideo = false;
  if (slurp(path, (unsigned)pl)) {
    int afmt = sniff_audio(g_file, (int)g_fileSize);
    if (afmt == SNIFF_OPUS) open_opus();
    else if (afmt) open_audio(afmt);
    else if (is_webm(g_file, (int)g_fileSize)) { if (open_webm()) mountVideo = webm_has_video(g_webm); }
    else if (open_video_loaded()) mountVideo = (g_vt >= 0);
  }
  if (mountVideo) {
    const char* so = "{\"type\":\"ui.screen.open\",\"name\":\"Video\"}";
    hal_msg_send(so, cstr_len(so));
    char m[640]; int o = 0;
    o = append_str(m, o, "{\"type\":\"video.load\",\"path\":\"");
    o = append_jesc(m, o, path);
    o = append_str(m, o, "\",\"autoplay\":true}");
    m[o] = 0;
    hal_msg_send(m, (uint32_t)o);
  }
}

// ── Radio (online streams via the streaming-HTTP HAL + dr_mp3) ────────────
static void load_stations(void) {
  g_stationCount = 0;
  static char b[MAX_STATIONS * 400];
  uint32_t n = hal_kv_get("radio_stations", 14, b, sizeof(b) - 1);
  b[n] = 0;
  unsigned i = 0;
  while (b[i] && g_stationCount < MAX_STATIONS) {
    char* nm = g_stName[g_stationCount]; int j = 0;
    while (b[i] && b[i] != '\t' && b[i] != '\n' && j < 127) nm[j++] = b[i++];
    nm[j] = 0;
    char* ur = g_stUrl[g_stationCount]; int k = 0;
    if (b[i] == '\t') {
      i++;
      while (b[i] && b[i] != '\n' && k < 255) ur[k++] = b[i++];
    }
    ur[k] = 0;
    if (b[i] == '\n') i++;
    if (nm[0] && ur[0]) g_stationCount++;
  }
}

static void save_stations(void) {
  static char b[MAX_STATIONS * 400];
  int o = 0;
  for (int s = 0; s < g_stationCount; s++) {
    o = append_str(b, o, g_stName[s]); b[o++] = '\t';
    o = append_str(b, o, g_stUrl[s]);  b[o++] = '\n';
  }
  b[o] = 0;
  hal_kv_set("radio_stations", 14, b, (uint32_t)o);
}

static void add_station(const char* name, const char* url) {
  if (g_stationCount >= MAX_STATIONS || !name[0] || !url[0]) return;
  int j = 0; while (name[j] && j < 127) { g_stName[g_stationCount][j] = name[j]; j++; }
  g_stName[g_stationCount][j] = 0;
  int k = 0; while (url[k] && k < 255) { g_stUrl[g_stationCount][k] = url[k]; k++; }
  g_stUrl[g_stationCount][k] = 0;
  g_stationCount++;
  save_stations();
}

// Seed a few well-known public MP3 streams the first time the wapp runs.
static void seed_stations(void) {
  add_station("SomaFM Groove Salad", "http://ice1.somafm.com/groovesalad-128-mp3");
  add_station("SomaFM Drone Zone", "http://ice1.somafm.com/dronezone-128-mp3");
  add_station("SomaFM Indie Pop Rocks", "http://ice1.somafm.com/indiepop-128-mp3");
}

static void render_stations(void) {
  char* m = g_msg; int o = 0;
  o = append_str(m, o, "{\"type\":\"ui.people.set\",\"field\":\"stations\",\"sections\":[{\"title\":\"\",\"items\":[");
  // Lower-case the active filter (matches station name or url).
  char ql[96]; int j = 0;
  for (const char* q = g_stQuery; *q && j < 95; q++) {
    char c = *q; if (c >= 'A' && c <= 'Z') c += 32; ql[j++] = c;
  }
  ql[j] = 0;
  if (g_stationCount == 0) {
    o = append_str(m, o, "{\"id\":\"none\",\"title\":\"No stations yet\","
                         "\"subtitle\":\"Tap Add station\"}");
  } else {
    int first = 1, matches = 0;
    for (int s = 0; s < g_stationCount; s++) {
      if (ql[0] && !str_contains_ci(g_stName[s], ql) && !str_contains_ci(g_stUrl[s], ql)) continue;
      matches++;
      if (!first) o = append_str(m, o, ","); first = 0;
      o = append_str(m, o, "{\"id\":\""); o = append_ll(m, o, s);
      o = append_str(m, o, "\",\"title\":\""); o = append_jesc(m, o, g_stName[s]);
      o = append_str(m, o, "\",\"subtitle\":\""); o = append_jesc(m, o, g_stUrl[s]);
      o = append_str(m, o, "\",\"menu\":[{\"label\":\"Remove\",\"value\":\"station_remove\"}]}");
    }
    if (matches == 0) o = append_str(m, o, "{\"id\":\"none\",\"title\":\"No matches\"}");
  }
  o = append_str(m, o, "]}]}");
  m[o] = 0;
  hal_msg_send(m, (uint32_t)o);
}

static void radio_stop(void) {
  if (g_radioHandle >= 0) { hal_http_stream_close(g_radioHandle); g_radioHandle = -1; }
  g_radioMode = 0; g_rdlen = 0; g_radioFrames = 0;
  g_radioRate = 0; g_radioCh = 0; g_radioTitle[0] = 0;
}

static void play_station(int i) {
  if (i < 0 || i >= g_stationCount) return;
  teardown();                       // stops any music/video AND the prior stream
  g_radioHandle = hal_http_stream_open(g_stUrl[i], cstr_len(g_stUrl[i]));
  if (g_radioHandle < 0) { logmsg("[player] radio open failed"); return; }
  drmp3dec_init(&g_mp3dec);
  g_radioMode = 1; g_curStation = i; g_musicMode = 0;
  g_rdlen = 0; g_radioFrames = 0; g_radioRate = 0; g_radioCh = 0; g_radioTitle[0] = 0;
  g_playing = true; g_ended = false; g_lastTickMs = hal_time_ms(); g_playClockMs = 0;
  render_nowplaying();
  logmsg("[player] playing radio");
}

static void radio_tick(void) {
  if (!g_playing || !g_radioMode || g_radioHandle < 0) return;
  int space = (int)sizeof(g_rdbuf) - g_rdlen;
  if (space > 0) {
    int got = hal_http_stream_read(g_radioHandle, (char*)g_rdbuf + g_rdlen, (uint32_t)space);
    if (got > 0) g_rdlen += got;
    else if (got < 0 && g_rdlen == 0) { radio_stop(); render_nowplaying(); return; }
  }
  int guard = 300;
  while (guard-- > 0 && g_rdlen > 0) {
    drmp3dec_frame_info info;
    int samples = drmp3dec_decode_frame(&g_mp3dec, g_rdbuf, g_rdlen, g_rdpcm, &info);
    if (info.frame_bytes == 0) break;               // need more bytes
    if (info.frame_bytes <= g_rdlen) {
      memmove(g_rdbuf, g_rdbuf + info.frame_bytes, g_rdlen - info.frame_bytes);
      g_rdlen -= info.frame_bytes;
    } else { g_rdlen = 0; }
    if (samples > 0 && info.channels > 0 && info.sample_rate > 0) {
      g_radioRate = info.sample_rate; g_radioCh = info.channels;
      hal_audio_pcm((const uint8_t*)g_rdpcm, (uint32_t)(samples * info.channels * 2),
                    info.sample_rate, info.channels, 0,
                    (int32_t)(g_radioFrames * 1000 / info.sample_rate));
      g_radioFrames += samples;
    }
  }
  // ICY now-playing title.
  char t[160];
  uint32_t tn = hal_http_stream_meta(g_radioHandle, t, sizeof(t) - 1);
  if (tn > 0) {
    t[tn] = 0;
    if (!str_eq(t, g_radioTitle)) {
      int j = 0; while (t[j] && j < (int)sizeof(g_radioTitle) - 1) { g_radioTitle[j] = t[j]; j++; }
      g_radioTitle[j] = 0;
      render_nowplaying();
    }
  }
}

// ── wapp module ABI ─────────────────────────────────────────────────────
static int g_initTicks = 0;
static bool g_inited = false;
static int g_npTick = 0;

extern "C" void module_init(void) {
  load_folders("music_folders", g_musicFolders, &g_musicFolderCount);
  load_folders("film_folders", g_filmFolders, &g_filmFolderCount);
  load_stations();
  if (g_stationCount == 0) seed_stations();
  g_rng ^= (uint32_t)hal_time_ms();
  logmsg("[player] ready (wasm media decoder)");
}

extern "C" void module_handle_event(void) {
  char buf[2048];
  while (hal_msg_available() != 0) {
    uint32_t n = hal_msg_recv(buf, sizeof(buf) - 1);
    if (n == 0) break;
    buf[n] = 0;
    char cmd[40] = "";
    json_str(buf, n, "command", cmd, sizeof(cmd));

    if (json_has_type(buf, n, "file.open") ||
        json_has_type(buf, n, "video.load")) {
      char path[512];
      int pl = json_str(buf, n, "path", path, sizeof(path));
      if (pl > 0) open_media_file(path, pl, true); // handed a single file inline
    } else if (json_has_type(buf, n, "fs.picked")) {
      // Result of "Add folder": store the base folder for the pending target.
      if (json_bool(buf, n, "dir", 0)) {
        char path[512];
        int pl = json_str(buf, n, "path", path, sizeof(path));
        if (pl > 0) {
          char (*folders)[512] = g_pickTarget ? g_filmFolders : g_musicFolders;
          int* count = g_pickTarget ? &g_filmFolderCount : &g_musicFolderCount;
          bool dup = false;
          for (int k = 0; k < *count; k++) if (str_eq(folders[k], path)) dup = true;
          if (!dup && *count < MAX_FOLDERS) {
            int j = 0; while (path[j] && j < 511) { folders[*count][j] = path[j]; j++; }
            folders[*count][j] = 0;
            (*count)++;
            g_idxType = -1; // folders changed → rebuild the search index
            if (g_pickTarget) {
              save_folders("film_folders", g_filmFolders, g_filmFolderCount);
              g_fdepth = 0; render_film_tree();
            } else {
              save_folders("music_folders", g_musicFolders, g_musicFolderCount);
              g_mdepth = 0; render_music_tree();
            }
          }
        }
      }
    } else if (cmd[0]) {
      // ── UI commands ──
      if (str_eq(cmd, "shuffle")) {
        g_shuffle = !g_shuffle; render_nowplaying();
      } else if (str_eq(cmd, "repeat")) {
        g_repeat = !g_repeat; render_nowplaying();
      } else if (str_eq(cmd, "add_folder") || str_eq(cmd, "add_film_folder")) {
        g_pickTarget = str_eq(cmd, "add_film_folder") ? 1 : 0;
        char m[200] = "{\"type\":\"fs.pick\",\"mode\":\"folder\","
                      "\"title\":\"Pick a folder\"";
        int o = (int)cstr_len(m);
        char home[256];
        uint32_t hn = hal_fs_home(home, sizeof(home) - 1);
        if (hn > 0) {
          home[hn] = 0;
          o = append_str(m, o, ",\"initial\":\"");
          o = append_jesc(m, o, home);
          m[o++] = '"';
        }
        m[o++] = '}'; m[o] = 0;
        hal_msg_send(m, (uint32_t)o);
      } else if (str_eq(cmd, "music_home")) {
        g_mdepth = 0; render_music_tree();
      } else if (str_eq(cmd, "film_home")) {
        g_fdepth = 0; render_film_tree();
      } else if (str_eq(cmd, "music_rmfolder") || str_eq(cmd, "film_rmfolder")) {
        bool film = str_eq(cmd, "film_rmfolder");
        char id[520] = ""; json_str(buf, n, film ? "ftree_id" : "mtree_id", id, sizeof(id));
        const char* path = (id[0] == 'd' && id[1] == ':') ? id + 2 : id;
        char (*folders)[512] = film ? g_filmFolders : g_musicFolders;
        int* count = film ? &g_filmFolderCount : &g_musicFolderCount;
        for (int k = 0; k < *count; k++) {
          if (str_eq(folders[k], path)) {
            for (int j = k; j < *count - 1; j++) {
              int c = 0; while (folders[j + 1][c]) { folders[j][c] = folders[j + 1][c]; c++; }
              folders[j][c] = 0;
            }
            (*count)--; break;
          }
        }
        g_idxType = -1; // folders changed → rebuild the search index
        if (film) { save_folders("film_folders", g_filmFolders, g_filmFolderCount); g_fdepth = 0; render_film_tree(); }
        else { save_folders("music_folders", g_musicFolders, g_musicFolderCount); g_mdepth = 0; render_music_tree(); }
      } else if (str_eq(cmd, "mtree_tap")) {
        char id[520] = ""; json_str(buf, n, "mtree_id", id, sizeof(id));
        if (str_eq(id, "up")) { if (g_mdepth > 0) g_mdepth--; render_music_tree(); }
        else if (id[0] == 'd' && id[1] == ':') {
          if (g_mdepth < NAV_MAX) {
            int j = 0; const char* s = id + 2;
            while (s[j] && j < 511) { g_mstack[g_mdepth][j] = s[j]; j++; }
            g_mstack[g_mdepth][j] = 0; g_mdepth++;
          }
          render_music_tree();
        } else if (id[0] == 'f' && id[1] == ':') {
          // Derive the folder from the file path itself, so this works for both
          // the browse view and search results (which span many folders).
          char dir[512]; parent_dir(id + 2, dir, sizeof(dir));
          int idx = snapshot_tracks(dir, id + 2);
          if (idx >= 0) play_index(idx);
        }
      } else if (str_eq(cmd, "ftree_tap")) {
        char id[520] = ""; json_str(buf, n, "ftree_id", id, sizeof(id));
        if (str_eq(id, "up")) { if (g_fdepth > 0) g_fdepth--; render_film_tree(); }
        else if (id[0] == 'd' && id[1] == ':') {
          if (g_fdepth < NAV_MAX) {
            int j = 0; const char* s = id + 2;
            while (s[j] && j < 511) { g_fstack[g_fdepth][j] = s[j]; j++; }
            g_fstack[g_fdepth][j] = 0; g_fdepth++;
          }
          render_film_tree();
        } else if (id[0] == 'f' && id[1] == ':') {
          open_media_file(id + 2, (int)cstr_len(id + 2), false); // play the film
        }
      } else if (str_eq(cmd, "mtree_search")) {
        char q[96] = ""; json_str(buf, n, "mtree_query", q, sizeof(q));
        if (!q[0]) render_music_tree(); else render_search("mtree", 0, q);
      } else if (str_eq(cmd, "ftree_search")) {
        char q[96] = ""; json_str(buf, n, "ftree_query", q, sizeof(q));
        if (!q[0]) render_film_tree(); else render_search("ftree", 1, q);
      } else if (str_eq(cmd, "stations_search")) {
        char q[96] = ""; json_str(buf, n, "stations_query", q, sizeof(q));
        int j = 0; while (q[j] && j < 95) { g_stQuery[j] = q[j]; j++; } g_stQuery[j] = 0;
        render_stations();
      } else if (str_eq(cmd, "stations_tap")) {
        char id[16] = ""; json_str(buf, n, "stations_id", id, sizeof(id));
        if (id[0] >= '0' && id[0] <= '9') play_station((int)strtol(id, nullptr, 10));
      } else if (str_eq(cmd, "add_station")) {
        const char* m = "{\"type\":\"ui.prompt\",\"id\":\"st_name\","
                        "\"title\":\"Station name\","
                        "\"input\":{\"hint\":\"e.g. My Radio\",\"max\":120},"
                        "\"confirm\":\"Next\"}";
        hal_msg_send(m, cstr_len(m));
      } else if (str_eq(cmd, "station_remove")) {
        char id[16] = ""; json_str(buf, n, "stations_id", id, sizeof(id));
        int idx = (id[0] >= '0' && id[0] <= '9') ? (int)strtol(id, nullptr, 10) : -1;
        if (idx >= 0 && idx < g_stationCount) {
          for (int k = idx; k < g_stationCount - 1; k++) {
            int j = 0; while (g_stName[k + 1][j]) { g_stName[k][j] = g_stName[k + 1][j]; j++; } g_stName[k][j] = 0;
            j = 0; while (g_stUrl[k + 1][j]) { g_stUrl[k][j] = g_stUrl[k + 1][j]; j++; } g_stUrl[k][j] = 0;
          }
          g_stationCount--;
          save_stations();
          render_stations();
        }
      } else if (str_eq(cmd, "prompt")) {
        char pid[24] = "", inp[300] = "";
        json_str(buf, n, "prompt_id", pid, sizeof(pid));
        json_str(buf, n, "prompt_input", inp, sizeof(inp));
        if (str_eq(pid, "st_name")) {
          int j = 0; while (inp[j] && j < 127) { g_pendName[j] = inp[j]; j++; } g_pendName[j] = 0;
          const char* m = "{\"type\":\"ui.prompt\",\"id\":\"st_url\","
                          "\"title\":\"Stream URL\","
                          "\"input\":{\"hint\":\"http://...\",\"max\":255},"
                          "\"confirm\":\"Add\"}";
          hal_msg_send(m, cstr_len(m));
        } else if (str_eq(pid, "st_url")) {
          if (inp[0]) { add_station(g_pendName[0] ? g_pendName : inp, inp); render_stations(); }
        }
      } else if (str_eq(cmd, "playpause")) {
        if (g_radioMode) {
          g_playing = !g_playing; if (g_playing) g_lastTickMs = hal_time_ms();
          render_nowplaying();
        } else if (g_curTrack < 0) { if (g_trkCount > 0) play_index(0); }
        else if (music_playback_ended()) { play_index(g_curTrack); }
        else { g_playing = !g_playing; if (g_playing) g_lastTickMs = hal_time_ms(); render_nowplaying(); }
      } else if (str_eq(cmd, "next")) {
        if (g_radioMode) { if (g_stationCount > 0) play_station((g_curStation + 1) % g_stationCount); }
        else if (g_curTrack < 0) { if (g_trkCount > 0) play_index(0); } else music_advance(1);
      } else if (str_eq(cmd, "prev")) {
        if (g_radioMode) { if (g_stationCount > 0) play_station((g_curStation - 1 + g_stationCount) % g_stationCount); }
        else if (g_curTrack < 0) { if (g_trkCount > 0) play_index(0); } else music_advance(-1);
      }
    } else if (json_has_type(buf, n, "video.scan")) {
      // Headless poster scan: sample keyframes across the WHOLE clip (they
      // decode standalone) and emit each so the host can score and keep the
      // best. Fall back to the first frames if a clip has no keyframe table.
      g_playing = false;
      build_keyframes();
      if (g_kfCount > 0) {
        g_scanKf = true;
      } else {
        g_scan = SCAN_FRAMES;
      }
    } else if (json_has_type(buf, n, "video.play")) {
      if (!g_ended) { g_playing = true; g_lastTickMs = hal_time_ms(); }
    } else if (json_has_type(buf, n, "video.pause")) {
      g_playing = false;
    } else if (json_has_type(buf, n, "video.stop")) {
      g_playing = false;
    }
    // video.seek / video.skip: precise seek needs keyframe handling; not in
    // the MVP. The frame pipeline plays straight through.
  }
}

extern "C" void module_tick(void) {
  // Launcher (full-wapp) mode: populate the tab views a few ticks in, once, and
  // only if we weren't handed a single file to play (inline/"open with").
  if (!g_inited && !g_fileLoaded) {
    if (++g_initTicks >= 3) {
      g_inited = true;
      render_music_tree();
      render_film_tree();
      render_stations();
      render_nowplaying();
    }
  }
  // Keep the now-playing position fresh while music plays.
  if (g_musicMode && g_playing && (++g_npTick % 15 == 0)) render_nowplaying();

  // Radio (online stream).
  if (g_radioMode) { radio_tick(); return; }
  // Audio path (mp3/wav/flac/ogg-vorbis).
  if (g_audioMode) { audio_tick(); return; }
  // Opus path (ogg-opus).
  if (g_opusMode) { opus_tick(); return; }
  // WebM path (VP8/VP9 video + Opus audio).
  if (g_webmMode) { webm_tick(); return; }
  // Poster scan (keyframes across the whole clip).
  if (g_scanKf && (g_dec || g_hevc || g_av1)) {
    int batch = 4;
    while (batch-- > 0 && g_kfCursor < g_kfCount) {
      decode_keyframe(g_kf[(unsigned)g_kfCursor]);
      g_kfCursor += g_kfStep;
    }
    if (g_kfCursor >= g_kfCount) {
      if (g_hevc) { hevc_flush(g_hevc); hevc_drain(); } // emit lagged pictures
      g_scanKf = false;
      hal_video_end();
    }
    return;
  }
  // Thumbnail scan path (sequential fallback): decode-ahead ignoring clock.
  if (g_scan > 0 && (g_dec || g_hevc || g_av1)) {
    int batch = 8;
    while (batch-- > 0 && g_scan > 0) {
      if (!decode_one()) { g_scan = 0; break; } // end of stream
      g_scan--;
    }
    if (g_scan == 0) {
      if (g_hevc) { hevc_flush(g_hevc); hevc_drain(); }
      hal_video_end();
    }
    return;
  }
  // mp4/m4a playback (video and/or AAC audio).
  if (!g_playing || g_ended) return;
  uint64_t now = hal_time_ms();
  double delta = (double)(now - g_lastTickMs);
  if (delta < 0) delta = 0;
  if (delta > 1000) delta = 1000; // clamp after a long stall
  g_playClockMs += delta;
  g_lastTickMs = now;

  // Video frames (paced to the play clock).
  if (g_vt >= 0 && (g_dec || g_hevc || g_av1) && !g_vDone) {
    int budget = MAX_BATCH;
    while (budget-- > 0 && g_nextPtsMs <= g_playClockMs + LEAD_MS) {
      if (!decode_one()) {
        flush_tail();
        g_vDone = true;
        break;
      }
    }
  }

  // AAC audio (paced to the play clock).
  decode_audio_mp4();

  // End only when every present track is exhausted.
  if (g_vDone && g_aDone && !g_ended) {
    g_ended = true;
    g_playing = false;
    if (g_musicMode) { music_advance(1); return; }
    hal_video_end();
    logmsg("[mp4player] end of stream");
  }
}

extern "C" void module_destroy(void) {
  teardown();
}

extern "C" uint32_t module_tick_interval_ms(void) {
  return 16; // ~60Hz decode pump; actual fps gated by the play clock
}
