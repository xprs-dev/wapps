// ogg-opus → s16 PCM. libogg demuxes pages; libopus decodes packets. Streaming
// (decodes on demand) so long files don't need a giant PCM buffer.
#include "opus_dec.h"

#include <stdlib.h>
#include <string.h>

#include "ogg/ogg.h"
#include "opus.h"

#define OPUS_MAX_FRAME 5760 // 120ms @ 48k, the largest opus frame

struct OpusDec {
  ogg_sync_state oy;
  ogg_stream_state os;
  int have_stream;
  OpusDecoder* dec;
  int channels;
  int pre_skip;
  int skip_left;
};

static int rd_le16(const unsigned char* p) { return p[0] | (p[1] << 8); }

OpusDec* opus_dec_open(const uint8_t* data, int len, int* rate, int* channels) {
  OpusDec* d = (OpusDec*)calloc(1, sizeof(OpusDec));
  if (!d) return NULL;
  ogg_sync_init(&d->oy);
  // Feed the whole file into the sync buffer.
  char* buf = ogg_sync_buffer(&d->oy, len);
  if (!buf) { free(d); return NULL; }
  memcpy(buf, data, len);
  ogg_sync_wrote(&d->oy, len);

  // Read pages until we've parsed the OpusHead packet.
  ogg_page og;
  ogg_packet op;
  int got_head = 0, got_tags = 0;
  while (!got_tags) {
    int r = ogg_sync_pageout(&d->oy, &og);
    if (r != 1) break; // need more data (we fed all) → done/failed
    if (!d->have_stream) {
      ogg_stream_init(&d->os, ogg_page_serialno(&og));
      d->have_stream = 1;
    }
    if (ogg_stream_pagein(&d->os, &og) < 0) continue;
    while (ogg_stream_packetout(&d->os, &op) == 1) {
      if (!got_head) {
        if (op.bytes >= 19 && memcmp(op.packet, "OpusHead", 8) == 0) {
          d->channels = op.packet[9];
          d->pre_skip = rd_le16(op.packet + 10);
          int err = 0;
          d->dec = opus_decoder_create(48000, d->channels, &err);
          if (err != OPUS_OK || !d->dec) { opus_dec_close(d); return NULL; }
          d->skip_left = d->pre_skip;
          got_head = 1;
        }
      } else {
        got_tags = 1; // OpusTags — header done; stop priming
        break;
      }
    }
  }
  if (!got_head || !d->dec) { opus_dec_close(d); return NULL; }
  if (rate) *rate = 48000;
  if (channels) *channels = d->channels;
  return d;
}

// Pull the next opus packet (refilling pages as needed) and decode it.
int opus_dec_read(OpusDec* d, int16_t* out, int max_frames) {
  if (!d || !d->dec) return 0;
  ogg_packet op;
  ogg_page og;
  for (;;) {
    int pr = ogg_stream_packetout(&d->os, &op);
    if (pr == 1) {
      int got =
          opus_decode(d->dec, op.packet, op.bytes, out, max_frames, 0);
      if (got <= 0) continue; // skip bad packet
      // Drop encoder pre-skip samples from the very start.
      if (d->skip_left > 0) {
        int drop = d->skip_left < got ? d->skip_left : got;
        d->skip_left -= drop;
        int rem = got - drop;
        if (rem > 0) {
          memmove(out, out + drop * d->channels,
                  (size_t)rem * d->channels * sizeof(int16_t));
        }
        got = rem;
        if (got == 0) continue;
      }
      return got;
    }
    // Need another page.
    int r = ogg_sync_pageout(&d->oy, &og);
    if (r != 1) return 0; // no more data → EOF
    ogg_stream_pagein(&d->os, &og);
  }
}

void opus_dec_close(OpusDec* d) {
  if (!d) return;
  if (d->dec) opus_decoder_destroy(d->dec);
  if (d->have_stream) ogg_stream_clear(&d->os);
  ogg_sync_clear(&d->oy);
  free(d);
}
