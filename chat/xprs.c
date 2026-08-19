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
