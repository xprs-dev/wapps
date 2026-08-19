/*
 * Thread ids, reply markers and like votes. See thread.h.
 *
 * Everything here is pure: no HAL, no globals, no allocation — the wire
 * conventions only, so both the send path and the receive path can agree and
 * tests/test_thread.c can pin them down.
 */
#include <stdint.h>
#include "thread.h"

static int t_eq(const char *a, const char *b) {
  while (*a && *a == *b) { a++; b++; }
  return *a == 0 && *b == 0;
}
static int t_hex(char ch) {
  return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
}

/* ── SHA-1 (RFC 3174) — only for short, stable message ids ────────────────
 * A reply references its parent by a 4-hex-char id both sender and receiver
 * derive from the same content (from|text), so threads work across APRS-IS,
 * BLE and LXMF without any extra wire fields. Inputs are short (<~220B); a
 * fixed buffer is plenty. */
static uint32_t sha1_rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
static void sha1(const unsigned char *msg, unsigned len, unsigned char out[20]) {
  uint32_t h[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };
  unsigned char buf[384];
  if (len > 256) len = 256;
  for (unsigned i = 0; i < len; i++) buf[i] = msg[i];
  uint64_t ml = (uint64_t)len * 8u;
  unsigned n = len;
  buf[n++] = 0x80;
  while ((n % 64u) != 56u) buf[n++] = 0;
  for (int i = 7; i >= 0; i--) buf[n++] = (unsigned char)((ml >> (i * 8)) & 0xffu);
  for (unsigned off = 0; off < n; off += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
      w[i] = ((uint32_t)buf[off + i*4] << 24) | ((uint32_t)buf[off + i*4 + 1] << 16) |
             ((uint32_t)buf[off + i*4 + 2] << 8) | (uint32_t)buf[off + i*4 + 3];
    for (int i = 16; i < 80; i++) w[i] = sha1_rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
      uint32_t f, k;
      if (i < 20)      { f = (b & c) | ((~b) & d);            k = 0x5A827999u; }
      else if (i < 40) { f = b ^ c ^ d;                       k = 0x6ED9EBA1u; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d);     k = 0x8F1BBCDCu; }
      else             { f = b ^ c ^ d;                       k = 0xCA62C1D6u; }
      uint32_t t = sha1_rol(a, 5) + f + e + k + w[i];
      e = d; d = c; c = sha1_rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
  }
  for (int i = 0; i < 5; i++) {
    out[i*4]   = (unsigned char)(h[i] >> 24);
    out[i*4+1] = (unsigned char)(h[i] >> 16);
    out[i*4+2] = (unsigned char)(h[i] >> 8);
    out[i*4+3] = (unsigned char)(h[i]);
  }
}

void msg_id(const char *from, const char *text, char out[5]) {
  unsigned char in[280]; unsigned n = 0;
  for (const char *p = from; *p && n < 270; p++) in[n++] = (unsigned char)*p;
  in[n++] = '|';
  for (const char *p = text; *p && n < 279; p++) in[n++] = (unsigned char)*p;
  unsigned char d[20]; sha1(in, n, d);
  static const char hx[] = "0123456789abcdef";
  out[0] = hx[d[0] >> 4]; out[1] = hx[d[0] & 15];
  out[2] = hx[d[1] >> 4]; out[3] = hx[d[1] & 15]; out[4] = 0;
}

int thread_parse(const char *wire, char parent[5], const char **disp) {
  parent[0] = 0; *disp = wire;
  if (wire[0] != '+') return 0;
  unsigned n = 0;
  while (t_hex(wire[1 + n])) n++;
  if (wire[1 + n] != ' ') return 0;
  /* 64 hex = a marker from a build that named its parent by the LXMF envelope
   * hash or a NOSTR event id. Only the receiver ever knew those, so the id is
   * unresolvable — but the marker is still wire syntax and must not be shown.
   * Strip it and thread nothing. Any other length is a coincidence, not a
   * marker: "+1234567 back tomorrow" is a message. */
  if (n == 64) { *disp = wire + 1 + n + 1; return 1; }
  if (n != 4) return 0;
  for (int i = 0; i < 4; i++) parent[i] = wire[1 + i];
  parent[4] = 0; *disp = wire + 6;
  return 1;
}

char *thread_wire(char *out, unsigned osz, const char *parent, const char *text) {
  unsigned i = 0;
  if (parent && parent[0]) {
    if (i + 1 < osz) out[i++] = '+';
    for (const char *p = parent; *p && i + 1 < osz; p++) out[i++] = *p;
    if (i + 1 < osz) out[i++] = ' ';
  }
  for (const char *p = text; *p && i + 1 < osz; p++) out[i++] = *p;
  out[i] = 0;
  return out;
}

int like_parse(const char *wire, char tgt[5], int *unlike) {
  tgt[0] = 0; *unlike = 0;
  for (int i = 0; i < 4; i++) if (!t_hex(wire[i])) return 0;
  if (wire[4] != ':') return 0;
  const char *v = wire + 5;
  if (t_eq(v, "like")) *unlike = 0;
  else if (t_eq(v, "unlike")) *unlike = 1;
  else return 0;
  for (int i = 0; i < 4; i++) tgt[i] = wire[i];
  tgt[4] = 0;
  return 1;
}

int roomlike_parse(const char *text, char mid[70], int *unlike) {
  mid[0] = 0; *unlike = 0;
  unsigned n = 0;
  while (text[n] && text[n] != ':') {
    if (!t_hex(text[n])) return 0;
    n++;
  }
  if (n < 8 || n > 64 || text[n] != ':') return 0;
  const char *v = text + n + 1;
  if (t_eq(v, "like")) *unlike = 0;
  else if (t_eq(v, "unlike")) *unlike = 1;
  else return 0;
  for (unsigned i = 0; i < n; i++) mid[i] = text[i];
  mid[n] = 0;
  return 1;
}

int anylike_parse(const char *text, char mid[70], int *unlike) {
  char tgt[5];
  if (like_parse(text, tgt, unlike)) {
    for (int i = 0; i < 5; i++) mid[i] = tgt[i];
    return 1;
  }
  return roomlike_parse(text, mid, unlike);
}

int votemark_parse(const char *wire, char mid[70], int *unlike,
                   const char **ck) {
  mid[0] = 0; *unlike = 0; *ck = "";
  const char *p;
  if (wire[0] != '+') return 0;
  if (wire[1] == 'l' && wire[2] == 'i' && wire[3] == 'k' && wire[4] == 'e' &&
      wire[5] == ':') { *unlike = 0; p = wire + 6; }
  else if (wire[1] == 'u' && wire[2] == 'n' && wire[3] == 'l' && wire[4] == 'i' &&
           wire[5] == 'k' && wire[6] == 'e' && wire[7] == ':') {
    *unlike = 1; p = wire + 8;
  } else return 0;
  unsigned n = 0;
  while (p[n] && p[n] != ' ' && n < 69) { mid[n] = p[n]; n++; }
  if (n == 0) return 0;
  mid[n] = 0;
  /* The key is optional: a message with no text has none, and then the id is
   * all there is to go on. */
  *ck = p[n] == ' ' ? p + n + 1 : "";
  return 1;
}

char *votemark_wire(char *out, unsigned osz, const char *mid, int unlike,
                    const char *ck) {
  unsigned i = 0;
  const char *tag = unlike ? "+unlike:" : "+like:";
  for (const char *p = tag; *p && i + 1 < osz; p++) out[i++] = *p;
  for (const char *p = mid; *p && i + 1 < osz; p++) out[i++] = *p;
  if (ck && ck[0]) {
    if (i + 1 < osz) out[i++] = ' ';
    for (const char *p = ck; *p && i + 1 < osz; p++) out[i++] = *p;
  }
  out[i] = 0;
  return out;
}
