#include "aac_dec.h"

#include "aacdecoder_lib.h"

#include <stdlib.h>
#include <string.h>

extern "C" int aac_open(AacDec* d, const uint8_t* asc, int asc_len) {
  memset(d, 0, sizeof(*d));
  if (!asc || asc_len <= 0) return 0;
  HANDLE_AACDECODER h = aacDecoder_Open(TT_MP4_RAW, 1);
  if (!h) return 0;
  UCHAR* conf[1] = {(UCHAR*)asc};
  UINT confLen[1] = {(UINT)asc_len};
  if (aacDecoder_ConfigRaw(h, conf, confLen) != AAC_DEC_OK) {
    aacDecoder_Close(h);
    return 0;
  }
  d->h = h;
  return 1;
}

extern "C" int aac_decode(AacDec* d, const uint8_t* au, int au_len,
                          int16_t* out, int out_max, int* out_ch,
                          int* out_rate) {
  if (!d->h) return -1;
  HANDLE_AACDECODER h = (HANDLE_AACDECODER)d->h;
  UCHAR* in[1] = {(UCHAR*)au};
  UINT inLen[1] = {(UINT)au_len};
  UINT valid = (UINT)au_len;
  if (aacDecoder_Fill(h, in, inLen, &valid) != AAC_DEC_OK) return -1;
  AAC_DECODER_ERROR e =
      aacDecoder_DecodeFrame(h, (INT_PCM*)out, out_max, 0);
  if (e == AAC_DEC_NOT_ENOUGH_BITS) return 0;
  if (e != AAC_DEC_OK) return -1;
  CStreamInfo* si = aacDecoder_GetStreamInfo(h);
  if (!si || si->frameSize <= 0) return 0;
  if (out_ch) *out_ch = si->numChannels;
  if (out_rate) *out_rate = si->sampleRate;
  return si->frameSize;
}

extern "C" void aac_close(AacDec* d) {
  if (d && d->h) {
    aacDecoder_Close((HANDLE_AACDECODER)d->h);
    d->h = NULL;
  }
}
