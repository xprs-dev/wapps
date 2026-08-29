/*
 * xprs.c — chat's envelope on the licence-free transports (see xprs.h).
 *
 * The format is docs/XPRS.md in the aurora repo. Only the keys that document
 * already defines are used here; nothing is invented.
 */
#include "xprs.h"

/* ── file-local libc (static, so nothing clashes with main.c's) ────────── */
static unsigned x_len(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static void x_cpy(char *d, const char *s, unsigned m) {
  unsigned i = 0; while (i < m - 1 && s[i]) { d[i] = s[i]; i++; } d[i] = 0;
}
static void x_cat(char *d, const char *s, unsigned m) {
  unsigned l = x_len(d), i = 0;
  while (l + i < m - 1 && s[i]) { d[l + i] = s[i]; i++; }
  d[l + i] = 0;
}
static int x_digit(char c) { return c >= '0' && c <= '9'; }
static void x_two(char *d, int v) { d[0] = (char)('0' + (v / 10) % 10); d[1] = (char)('0' + v % 10); }

/* ── Time, XPRS section 4.8: YYYY-MM-DD_HH:MM:SS, always UTC ───────────── */

/* Days since 1970-01-01 -> civil date. Howard Hinnant's days_from_civil in
 * reverse; exact for every date this format will ever carry, and no libc. */
static void civil_from_days(long z, int *y, int *m, int *d) {
  z += 719468;
  long era = (z >= 0 ? z : z - 146096) / 146097;
  unsigned long doe = (unsigned long)(z - era * 146097);
  unsigned long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  long yy = (long)yoe + era * 400;
  unsigned long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned long mp = (5 * doy + 2) / 153;
  unsigned long dd = doy - (153 * mp + 2) / 5 + 1;
  unsigned long mm = mp < 10 ? mp + 3 : mp - 9;
  *y = (int)(yy + (mm <= 2 ? 1 : 0));
  *m = (int)mm;
  *d = (int)dd;
}

static long days_from_civil(int y, int m, int d) {
  y -= m <= 2 ? 1 : 0;
  long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long)doe - 719468;
}

void xprs_stamp(char *out, unsigned max, unsigned long long epoch) {
  if (max < 20) { out[0] = 0; return; }
  long days = (long)(epoch / 86400ULL);
  int secs = (int)(epoch % 86400ULL);
  int y = 1970, m = 1, d = 1;
  civil_from_days(days, &y, &m, &d);
  int i = 0;
  out[i++] = (char)('0' + (y / 1000) % 10);
  out[i++] = (char)('0' + (y / 100) % 10);
  out[i++] = (char)('0' + (y / 10) % 10);
  out[i++] = (char)('0' + y % 10);
  out[i++] = '-'; x_two(out + i, m); i += 2;
  out[i++] = '-'; x_two(out + i, d); i += 2;
  out[i++] = '_'; x_two(out + i, secs / 3600); i += 2;
  out[i++] = ':'; x_two(out + i, (secs / 60) % 60); i += 2;
  out[i++] = ':'; x_two(out + i, secs % 60); i += 2;
  out[i] = 0;
}

unsigned long long xprs_parse_stamp(const char *s) {
  /* 0123456789...  YYYY-MM-DD_HH:MM:SS */
  if (!s) return 0;
  for (int i = 0; i < 19; i++) if (!s[i]) return 0;
  for (int i = 0; i < 19; i++) {
    int sep = (i == 4 || i == 7 || i == 10 || i == 13 || i == 16);
    if (sep ? x_digit(s[i]) : !x_digit(s[i])) return 0;
  }
  int y = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
  int mo = (s[5]-'0')*10 + (s[6]-'0');
  int d  = (s[8]-'0')*10 + (s[9]-'0');
  int hh = (s[11]-'0')*10 + (s[12]-'0');
  int mi = (s[14]-'0')*10 + (s[15]-'0');
  int ss = (s[17]-'0')*10 + (s[18]-'0');
  if (mo < 1 || mo > 12 || d < 1 || d > 31 || hh > 23 || mi > 59 || ss > 60) return 0;
  long days = days_from_civil(y, mo, d);
  if (days < 0) return 0;
  return (unsigned long long)days * 86400ULL + (unsigned long long)(hh * 3600 + mi * 60 + ss);
}

/* ── Addresses ─────────────────────────────────────────────────────────── */

/* Section 6.3: an open group is uppercase 1..16 and may not be named like a
 * station, and a station is told from a group by its prefix. XPRS callsigns
 * are X1/X3/X5 plus four; an amateur callsign carries a digit inside the first
 * three characters (CT1ABC) and often an SSID (CT1ABC-9). Everything else —
 * LISBOA, FEED, NOSTR — is a group. */
int xprs_is_station(const char *addr) {
  if (!addr || !addr[0]) return 0;
  unsigned n = x_len(addr);
  if (n >= 6 && addr[0] == 'X' &&
      (addr[1] == '1' || addr[1] == '3' || addr[1] == '5')) return 1;
  for (unsigned i = 0; i < n; i++) if (addr[i] == '-') return 1;
  for (unsigned i = 1; i < n && i < 3; i++) if (x_digit(addr[i])) return 1;
  return 0;
}

/* ── Building ──────────────────────────────────────────────────────────── */

/* A value that would break the field grammar (a space, a control byte) cannot
 * ride in anything but `m:`, which is greedy and last. Chat only ever puts a
 * callsign or a group name in `d:`, so this is a guard, not a transformation:
 * a malformed address means no XPRS form and the caller airs the old frame. */
static int x_field_safe(const char *s) {
  if (!s || !s[0]) return 0;
  for (const char *p = s; *p; p++) if (*p <= ' ' || *p == 0x7f) return 0;
  return 1;
}

unsigned xprs_pack(char *out, unsigned max, const char *from, const char *to,
                   const char *text, unsigned long long now) {
  if (!out || max < 32 || !x_field_safe(from)) return 0;
  if (!to) to = "";

  /* Control frames (?MAIL, ?IGATE, ?PING…) are wapp-to-wapp and have no XPRS
   * meaning yet. Refusing them here is what keeps the old compact frame as
   * their wire, which both ends still read. */
  if (to[0] == '?') return 0;

  char stamp[24]; xprs_stamp(stamp, sizeof(stamp), now);
  int position = (to[0] == '!' && to[1] == 0);

  out[0] = 0;
  x_cat(out, position ? "t:observation f:" : "t:message f:", max);
  x_cat(out, from, max);
  if (!position && to[0]) {
    const char *addr = to[0] == '#' ? to + 1 : to;   /* chat marks groups '#' */
    if (!x_field_safe(addr)) return 0;
    x_cat(out, " d:", max);
    x_cat(out, addr, max);
  }
  x_cat(out, " ts:", max);
  x_cat(out, stamp, max);

  if (position) {
    /* text is "lat,lon[,comment]" — the first two fields become pos:, and
     * anything after them is the human part and belongs in m:. */
    const char *p = text ? text : "";
    int commas = 0; unsigned cut = 0;
    for (unsigned i = 0; p[i]; i++) {
      if (p[i] != ',') continue;
      if (++commas == 2) { cut = i; break; }
    }
    char coord[48] = "";
    unsigned n = cut ? cut : x_len(p);
    if (n >= sizeof(coord)) return 0;
    for (unsigned i = 0; i < n; i++) coord[i] = p[i];
    coord[n] = 0;
    if (!x_field_safe(coord)) return 0;
    x_cat(out, " pos:", max);
    x_cat(out, coord, max);
    if (cut && p[cut + 1]) { x_cat(out, " m:", max); x_cat(out, p + cut + 1, max); }
  } else if (text && text[0]) {
    x_cat(out, " m:", max);
    x_cat(out, text, max);
  }

  unsigned n = x_len(out);
  /* Truncating a packet would silently corrupt the message body, so a frame
   * that did not fit is reported as no frame at all. */
  return n >= max - 1 ? 0 : n;
}

/* ── Reading ───────────────────────────────────────────────────────────── */

int xprs_looks_like(const char *wire) {
  return wire && wire[0] == 't' && wire[1] == ':';
}

/* Every packet type the specification defines (section 4.2). The list is
 * CLOSED: "An unknown type is ignored. It is never an error and is never
 * displayed as a message." */
static const char *const XPRS_TYPES[] = {
  "message", "observation", "receipt", "reaction", "request", "identity",
  "track", "sos", "info", "blog", "poll", "file", "report", "place", "status",
  "passage", "event", "offer", "need", "channel", "mailbox", "service",
  "command", "result", "moderate", "challenge", "response", "warning",
  "ping", "pong", 0
};

/* Is [s] (a token, up to the next space) one of them? */
static int x_known_type(const char *s) {
  for (int i = 0; XPRS_TYPES[i]; i++) {
    const char *a = s, *b = XPRS_TYPES[i];
    while (*b && *a == *b) { a++; b++; }
    if (!*b && (*a == 0 || *a == ' ')) return 1;
  }
  return 0;
}

/* Is [s] an XPRS protocol wire rather than something a person wrote?
 *
 * xprs_looks_like() only asks whether the wire STARTS with `t:`, and the whole
 * garbage-in-the-chat problem was that the sender does not always put `t:`
 * first: a sealed packet arrived as
 *   x:<blob> t:message f:X3ARK d:X1VCVM ts:... n:2/3 sig:...
 * which failed the prefix test in lxmf_drain and was rendered verbatim as a
 * chat bubble, with a notification, hundreds of times.
 *
 * So ask the honest question: is the text SHAPED like a packet, wherever its
 * fields sit. A wire always carries a type from the closed vocabulary and the
 * callsign that sent it. `m:` is deliberately not required -- a sealed packet
 * replaces it with `x:` (section 9.2).
 *
 * Prose does not collide with this: a colon in "meet me at 5: the pub" is not
 * a `t:` token, and a person would have to type both a real type word and an
 * `f:` callsign to be taken for a packet. */
int xprs_is_wire(const char *s) {
  if (!s || !s[0]) return 0;
  int saw_type = 0, saw_from = 0;
  for (const char *p = s; *p; ) {
    if (p[0] && p[1] == ':') {
      if (!saw_type && p[0] == 't') saw_type = x_known_type(p + 2);
      else if (!saw_from && p[0] == 'f' && p[2] && p[2] != ' ') saw_from = 1;
      if (saw_type && saw_from) return 1;
    }
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
  }
  return 0;
}

/* Copy the value of `key` into out. `m:` is greedy: it runs to the end of the
 * packet, spaces and colons included (section 4). Returns 1 when found. */
static int x_field(const char *wire, const char *key, char *out, unsigned max) {
  unsigned kl = x_len(key);
  out[0] = 0;
  for (const char *p = wire; *p; ) {
    /* p is at the start of a field */
    unsigned i = 0;
    while (i < kl && p[i] == key[i]) i++;
    int hit = (i == kl && p[kl] == ':');
    const char *v = p;
    while (*v && *v != ':') v++;
    if (!*v) return 0;
    v++;
    if (hit) {
      unsigned greedy = (kl == 1 && key[0] == 'm');
      unsigned o = 0;
      while (v[o] && (greedy || v[o] != ' ') && o < max - 1) { out[o] = v[o]; o++; }
      out[o] = 0;
      return 1;
    }
    /* skip to the next field */
    while (*v && *v != ' ') v++;
    while (*v == ' ') v++;
    if (!*v) return 0;
    p = v;
  }
  return 0;
}

int xprs_unpack(const char *wire, char *from, unsigned fmax, char *to,
                unsigned tmax, char *text, unsigned xmax,
                unsigned long long *ts_out) {
  if (!xprs_looks_like(wire)) return 0;
  from[0] = 0; to[0] = 0; text[0] = 0;
  if (ts_out) *ts_out = 0;

  char type[24] = "";
  if (!x_field(wire, "t", type, sizeof(type))) return 0;
  if (!x_field(wire, "f", from, fmax) || !from[0]) return 0;

  char stamp[24] = "";
  if (ts_out && x_field(wire, "ts", stamp, sizeof(stamp))) {
    *ts_out = xprs_parse_stamp(stamp);
  }

  char m[512] = "";
  x_field(wire, "m", m, sizeof(m));

  /* Section 6.6: "A partial message is never displayed."
   *
   * A long message is split into up to nine parts, each carrying `n:i/total`
   * and the WHOLE envelope, so every part looks like a complete t:message from
   * here. Without this test each one became its own chat entry: a single
   * message from a neighbour arrived as four lines of `n:3/4 x:TH`, and a
   * store-and-forward backlog draining turned that into hundreds of them.
   *
   * Rejoining them is the host's job, not ours -- it holds the set for ten
   * minutes, joins the parts in order and hands us the message. A part is not
   * a message and has nothing to show. */
  {
    char part[8] = "";
    if (x_field(wire, "n", part, sizeof(part))) return 0;
  }

  if (type[0] == 'o') {                       /* observation */
    char pos[64] = "";
    if (!x_field(wire, "pos", pos, sizeof(pos))) return 0;   /* nothing to show */
    x_cpy(to, "!", tmax);
    x_cpy(text, pos, xmax);
    if (m[0]) { x_cat(text, ",", xmax); x_cat(text, m, xmax); }
    return 1;
  }
  if (type[0] != 'm') return 0;               /* message; anything else is not ours */

  char d[32] = "";
  if (x_field(wire, "d", d, sizeof(d)) && d[0]) {
    if (xprs_is_station(d)) {
      x_cpy(to, d, tmax);
    } else {
      x_cpy(to, "#", tmax);                   /* chat marks a group with '#' */
      x_cat(to, d, tmax);
    }
  }
  x_cpy(text, m, xmax);
  return 1;
}

/* ── Message identifier, XPRS section 5 ─────────────────────────────────
 * id = first 6 lowercase hex of sha256(wire minus sig:/via:). The wires this
 * wapp BUILDS carry neither key (the host signs after us), so the caller
 * hashes exactly the bytes it is about to send. */
static const unsigned int xk[64] = {
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,
  0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
  0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,
  0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
  0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
  0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,
  0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,
  0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
  0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
#define XROR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))
static void x_sha_block(unsigned int h[8], const unsigned char blk[64]) {
  unsigned int w[64], a, b, c, d, e, f, g, hh;
  for (int t = 0; t < 16; t++)
    w[t] = ((unsigned int)blk[t*4] << 24) | ((unsigned int)blk[t*4+1] << 16)
         | ((unsigned int)blk[t*4+2] << 8) | blk[t*4+3];
  for (int t = 16; t < 64; t++) {
    unsigned int s0 = XROR(w[t-15],7) ^ XROR(w[t-15],18) ^ (w[t-15] >> 3);
    unsigned int s1 = XROR(w[t-2],17) ^ XROR(w[t-2],19) ^ (w[t-2] >> 10);
    w[t] = w[t-16] + s0 + w[t-7] + s1;
  }
  a=h[0]; b=h[1]; c=h[2]; d=h[3]; e=h[4]; f=h[5]; g=h[6]; hh=h[7];
  for (int t = 0; t < 64; t++) {
    unsigned int S1 = XROR(e,6) ^ XROR(e,11) ^ XROR(e,25);
    unsigned int ch = (e & f) ^ (~e & g);
    unsigned int t1 = hh + S1 + ch + xk[t] + w[t];
    unsigned int S0 = XROR(a,2) ^ XROR(a,13) ^ XROR(a,22);
    unsigned int mj = (a & b) ^ (a & c) ^ (b & c);
    unsigned int t2 = S0 + mj;
    hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
  }
  h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
}
static void x_sha256(const unsigned char *msg, unsigned len,
                     unsigned char out[32]) {
  unsigned int h[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
  unsigned i = 0;
  while (len - i >= 64) { x_sha_block(h, msg + i); i += 64; }
  unsigned char blk[64];
  unsigned n = 0;
  while (i < len) blk[n++] = msg[i++];
  blk[n++] = 0x80;
  if (n > 56) {                       /* padding spills into a second block */
    while (n < 64) blk[n++] = 0;
    x_sha_block(h, blk);
    n = 0;
  }
  while (n < 56) blk[n++] = 0;
  unsigned long long bits = (unsigned long long)len * 8;
  for (int t = 7; t >= 0; t--) blk[n++] = (unsigned char)(bits >> (t*8));
  x_sha_block(h, blk);
  for (int t = 0; t < 8; t++) {
    out[t*4]   = (unsigned char)(h[t] >> 24);
    out[t*4+1] = (unsigned char)(h[t] >> 16);
    out[t*4+2] = (unsigned char)(h[t] >> 8);
    out[t*4+3] = (unsigned char)(h[t]);
  }
}

void xprs_id(const char *wire, unsigned len, char out[7]) {
  unsigned char d[32];
  x_sha256((const unsigned char *)wire, len, d);
  static const char hx[] = "0123456789abcdef";
  for (int i = 0; i < 3; i++) {
    out[i*2]   = hx[d[i] >> 4];
    out[i*2+1] = hx[d[i] & 15];
  }
  out[6] = 0;
}
