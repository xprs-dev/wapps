#include "util.h"

/* ── strings ──────────────────────────────────────────────────────────── */
unsigned s_len(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }

int s_eq(const char *a, const char *b) {
  while (*a && *b && *a == *b) { a++; b++; }
  return *a == *b;
}

void s_cpy(char *d, const char *s, unsigned m) {
  unsigned i = 0;
  if (m == 0) return;
  while (i < m - 1 && s[i]) { d[i] = s[i]; i++; }
  d[i] = 0;
}

void s_cat(char *d, const char *s, unsigned m) {
  unsigned l = s_len(d), i = 0;
  if (m == 0) return;
  while (l + i < m - 1 && s[i]) { d[l + i] = s[i]; i++; }
  d[l + i] = 0;
}

/* ── JSON ─────────────────────────────────────────────────────────────── */
void jesc(char *dst, unsigned m, const char *src) {
  unsigned l = s_len(dst);
  for (const char *p = src; *p && l < m - 3; p++) {
    char c = *p;
    if (c == '"' || c == '\\') { dst[l++] = '\\'; dst[l++] = c; }
    else if (c == '\n') { dst[l++] = '\\'; dst[l++] = 'n'; }
    else dst[l++] = c;
  }
  dst[l] = 0;
}

static int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

int jstr(const char *buf, const char *key, char *out, unsigned m) {
  char pat[96]; pat[0] = '"'; pat[1] = 0;
  s_cat(pat, key, sizeof(pat)); s_cat(pat, "\":\"", sizeof(pat));
  unsigned pl = s_len(pat);
  for (const char *p = buf; *p; p++) {
    int ok = 1;
    for (unsigned i = 0; i < pl; i++) { if (p[i] != pat[i]) { ok = 0; break; } }
    if (!ok) continue;
    p += pl; unsigned i = 0;
    while (*p && *p != '"' && i < m - 1) {
      if (*p == '\\' && *(p + 1)) {
        p++;
        if (*p == 'u' && hexv(p[1]) >= 0 && hexv(p[2]) >= 0 &&
            hexv(p[3]) >= 0 && hexv(p[4]) >= 0) {
          int v = (hexv(p[1]) << 12) | (hexv(p[2]) << 8) |
                  (hexv(p[3]) << 4) | hexv(p[4]);
          p += 5;
          out[i++] = (char)(v & 0xff);
        } else if (*p == 'n') { out[i++] = '\n'; p++; }
        else { out[i++] = *p++; }
      } else out[i++] = *p++;
    }
    out[i] = 0; return 1;
  }
  out[0] = 0; return 0;
}

int jint(const char *buf, const char *key, long *out) {
  char pat[96]; pat[0] = '"'; pat[1] = 0;
  s_cat(pat, key, sizeof(pat)); s_cat(pat, "\":", sizeof(pat));
  unsigned pl = s_len(pat);
  for (const char *p = buf; *p; p++) {
    int ok = 1;
    for (unsigned i = 0; i < pl; i++) { if (p[i] != pat[i]) { ok = 0; break; } }
    if (!ok) continue;
    p += pl;
    while (*p == ' ') p++;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    if (*p < '0' || *p > '9') return 0;
    long v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    *out = neg ? -v : v;
    return 1;
  }
  return 0;
}

int jbool(const char *buf, const char *key) {
  char pat[96]; pat[0] = '"'; pat[1] = 0;
  s_cat(pat, key, sizeof(pat)); s_cat(pat, "\":", sizeof(pat));
  unsigned pl = s_len(pat);
  for (const char *p = buf; *p; p++) {
    int ok = 1;
    for (unsigned i = 0; i < pl; i++) { if (p[i] != pat[i]) { ok = 0; break; } }
    if (!ok) continue;
    p += pl; while (*p == ' ') p++;
    return (*p == 't' || *p == '1') ? 1 : 0;
  }
  return 0;
}

int next_object(const char **cur, char *out, unsigned cap) {
  const char *p = *cur;
  while (*p && *p != '{') p++;
  if (*p != '{') { *cur = p; return 0; }
  unsigned i = 0; int instr = 0, depth = 0;
  while (*p) {
    char ch = *p;
    if (i < cap - 1) out[i++] = ch;
    if (instr) {
      if (ch == '\\' && p[1]) { if (i < cap - 1) out[i++] = p[1]; p += 2; continue; }
      if (ch == '"') instr = 0;
    } else {
      if (ch == '"') instr = 1;
      else if (ch == '{') depth++;
      else if (ch == '}') { depth--; p++; if (depth == 0) break; continue; }
    }
    p++;
  }
  out[i] = 0; *cur = p; return 1;
}

/* ── hex ──────────────────────────────────────────────────────────────── */
void hex_encode(const unsigned char *in, unsigned n, char *out) {
  static const char *H = "0123456789abcdef";
  for (unsigned i = 0; i < n; i++) {
    out[i * 2] = H[(in[i] >> 4) & 0xf];
    out[i * 2 + 1] = H[in[i] & 0xf];
  }
  out[n * 2] = 0;
}

int hex_decode(const char *in, unsigned char *out, unsigned maxout) {
  unsigned n = 0;
  while (in[0] && in[1]) {
    int hi = hexv(in[0]), lo = hexv(in[1]);
    if (hi < 0 || lo < 0) return -1;
    if (n >= maxout) return -1;
    out[n++] = (unsigned char)((hi << 4) | lo);
    in += 2;
  }
  if (in[0]) return -1; /* odd length */
  return (int)n;
}

/* ── base64url ────────────────────────────────────────────────────────── */
static const char *B64U = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int b64url_encode(const unsigned char *in, unsigned n, char *out, unsigned maxout) {
  unsigned o = 0, i = 0;
  while (i + 3 <= n) {
    unsigned v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
    if (o + 4 >= maxout) return -1;
    out[o++] = B64U[(v >> 18) & 63];
    out[o++] = B64U[(v >> 12) & 63];
    out[o++] = B64U[(v >> 6) & 63];
    out[o++] = B64U[v & 63];
    i += 3;
  }
  unsigned rem = n - i;
  if (rem == 1) {
    unsigned v = in[i] << 16;
    if (o + 2 >= maxout) return -1;
    out[o++] = B64U[(v >> 18) & 63];
    out[o++] = B64U[(v >> 12) & 63];
  } else if (rem == 2) {
    unsigned v = (in[i] << 16) | (in[i + 1] << 8);
    if (o + 3 >= maxout) return -1;
    out[o++] = B64U[(v >> 18) & 63];
    out[o++] = B64U[(v >> 12) & 63];
    out[o++] = B64U[(v >> 6) & 63];
  }
  if (o >= maxout) return -1;
  out[o] = 0;
  return (int)o;
}

static int b64v(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '-') return 62;
  if (c == '_') return 63;
  if (c == '+') return 62; /* tolerate standard alphabet too */
  if (c == '/') return 63;
  return -1;
}

int b64url_decode(const char *in, unsigned char *out, unsigned maxout) {
  unsigned acc = 0, bits = 0, o = 0;
  for (const char *p = in; *p; p++) {
    if (*p == '=' || *p == '\n' || *p == '\r') continue;
    int v = b64v(*p);
    if (v < 0) return -1;
    acc = (acc << 6) | (unsigned)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (o >= maxout) return -1;
      out[o++] = (unsigned char)((acc >> bits) & 0xff);
    }
  }
  return (int)o;
}

/* ── bech32 (NIP-19 npub) ─────────────────────────────────────────────── */
static const char *BECH = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";

static int bech_val(char c) {
  for (int i = 0; i < 32; i++) if (BECH[i] == c) return i;
  return -1;
}

static unsigned bech_polymod(const unsigned char *values, unsigned len) {
  static const unsigned gen[5] = {
      0x3b6a57b2u, 0x26508e6du, 0x1ea119fau, 0x3d4233ddu, 0x2a1462b3u};
  unsigned chk = 1;
  for (unsigned i = 0; i < len; i++) {
    unsigned top = chk >> 25;
    chk = ((chk & 0x1ffffffu) << 5) ^ values[i];
    for (int j = 0; j < 5; j++) if ((top >> j) & 1) chk ^= gen[j];
  }
  return chk;
}

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

int npub_decode(const char *npub, unsigned char out32[32]) {
  /* Locate the separator '1' (last one). */
  char buf[120];
  unsigned n = s_len(npub);
  if (n == 0 || n >= sizeof(buf)) return -1;
  for (unsigned i = 0; i < n; i++) buf[i] = lc(npub[i]);
  buf[n] = 0;
  int sep = -1;
  for (int i = (int)n - 1; i >= 0; i--) if (buf[i] == '1') { sep = i; break; }
  if (sep < 0) return -1;
  /* hrp must be "npub". */
  if (!(sep == 4 && buf[0] == 'n' && buf[1] == 'p' && buf[2] == 'u' && buf[3] == 'b'))
    return -1;
  unsigned dlen = n - (unsigned)sep - 1;
  if (dlen < 6) return -1;
  /* Build the polymod input: hrp expanded + data values. */
  unsigned char vals[120];
  unsigned vi = 0;
  /* hrp high bits, separator 0, hrp low bits */
  const char *hrp = "npub";
  for (int i = 0; i < 4; i++) vals[vi++] = (unsigned char)(hrp[i] >> 5);
  vals[vi++] = 0;
  for (int i = 0; i < 4; i++) vals[vi++] = (unsigned char)(hrp[i] & 31);
  unsigned data_start = vi;
  for (unsigned i = 0; i < dlen; i++) {
    int v = bech_val(buf[sep + 1 + i]);
    if (v < 0) return -1;
    vals[vi++] = (unsigned char)v;
  }
  if (bech_polymod(vals, vi) != 1) return -1;
  /* Drop the 6-symbol checksum, convert 5-bit groups to bytes. */
  unsigned ndata = dlen - 6;
  unsigned acc = 0, bits = 0, o = 0;
  for (unsigned i = 0; i < ndata; i++) {
    acc = (acc << 5) | vals[data_start + i];
    bits += 5;
    if (bits >= 8) {
      bits -= 8;
      if (o >= 32) return -1;
      out32[o++] = (unsigned char)((acc >> bits) & 0xff);
    }
  }
  if (o != 32) return -1;
  /* Any leftover bits must be zero padding. */
  if (bits >= 5 || (acc & ((1u << bits) - 1)) != 0) return -1;
  return 0;
}
