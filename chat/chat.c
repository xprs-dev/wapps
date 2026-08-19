/*
 * aprs.c — APRS-IS client library implementation (see aprs.h).
 * Ported from the XPRS reference; built on the Aurora HAL sockets.
 */
#include "chat.h"
#include "xprs_wasm_hal.h"

/* ── file-local libc (static so they don't clash with the host wapp) ── */
static unsigned a_len(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static void a_cpy(char *d, const char *s, unsigned m) {
  unsigned i = 0; while (i < m - 1 && s[i]) { d[i] = s[i]; i++; } d[i] = 0;
}
static void a_cat(char *d, const char *s, unsigned m) {
  unsigned l = a_len(d), i = 0;
  while (l + i < m - 1 && s[i]) { d[l + i] = s[i]; i++; }
  d[l + i] = 0;
}
static char a_up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
static int a_digit(char c) { return c >= '0' && c <= '9'; }

/* int -> decimal string */
static void a_itoa(int v, char *b) {
  char t[16]; int i = 0, neg = 0;
  if (v < 0) { neg = 1; v = -v; }
  if (v == 0) t[i++] = '0';
  while (v > 0) { t[i++] = (char)('0' + v % 10); v /= 10; }
  int j = 0;
  if (neg) b[j++] = '-';
  while (i > 0) b[j++] = t[--i];
  b[j] = 0;
}

/* double -> string with `dec` decimals (no rounding beyond truncation+0.5) */
static void a_dtoa(double v, char *b, int dec) {
  int j = 0;
  if (v < 0) { b[j++] = '-'; v = -v; }
  long whole = (long)v;
  double frac = v - (double)whole;
  char wb[24]; a_itoa((int)whole, wb);
  for (int k = 0; wb[k]; k++) b[j++] = wb[k];
  if (dec > 0) {
    b[j++] = '.';
    for (int k = 0; k < dec; k++) {
      frac *= 10.0;
      int d = (int)frac;
      if (d < 0) d = 0; if (d > 9) d = 9;
      b[j++] = (char)('0' + d);
      frac -= (double)d;
    }
  }
  b[j] = 0;
}

/* substring of [a,b) parsed as int */
static int a_sub_int(const char *s, int a, int b) {
  int v = 0;
  for (int i = a; i < b; i++) { if (!a_digit(s[i])) return -1; v = v * 10 + (s[i] - '0'); }
  return v;
}
/* substring of [a,b) parsed as double (no sign) */
static double a_sub_dbl(const char *s, int a, int b) {
  double v = 0; int seen_dot = 0; double f = 0.1;
  for (int i = a; i < b; i++) {
    char c = s[i];
    if (c == '.') { seen_dot = 1; continue; }
    if (!a_digit(c)) continue;
    if (!seen_dot) v = v * 10 + (c - '0');
    else { v += (c - '0') * f; f *= 0.1; }
  }
  return v;
}

/* ── passcode ─────────────────────────────────────────────────────────── */
int aprs_passcode(const char *callsign) {
  char base[16]; int n = 0;
  for (int i = 0; callsign[i] && callsign[i] != '-' && n < 15; i++)
    base[n++] = a_up(callsign[i]);
  base[n] = 0;
  int hash = 0x73e2;
  for (int i = 0; i < n;) {
    hash ^= (int)base[i] << 8;
    i++;
    if (i < n) { hash ^= (int)base[i]; i++; }
  }
  return hash & 0x7FFF;
}

/* ── connection ───────────────────────────────────────────────────────── */
int aprs_connect(const char *host, int port) {
  return hal_socket_open(host, a_len(host), port);
}
int aprs_is_open(int handle) { return hal_socket_status(handle) == 1; }
void aprs_disconnect(int handle) { hal_socket_close(handle); }

static void a_send(int handle, const char *line) {
  hal_socket_send(handle, line, a_len(line));
}

void aprs_build_login(char *out, unsigned max, const char *callsign,
                      int passcode, double lat, double lon, int radius_km) {
  out[0] = 0;
  a_cat(out, "user ", max);
  a_cat(out, callsign, max);
  a_cat(out, " pass ", max);
  char nb[16]; a_itoa(passcode, nb); a_cat(out, nb, max);
  a_cat(out, " vers Aurora 1.0 filter r/", max);
  char db[24];
  a_dtoa(lat, db, 4); a_cat(out, db, max); a_cat(out, "/", max);
  a_dtoa(lon, db, 4); a_cat(out, db, max); a_cat(out, "/", max);
  a_itoa(radius_km, nb); a_cat(out, nb, max);
  a_cat(out, "\r\n", max);
}

void aprs_login(int handle, const char *callsign, int passcode,
                double lat, double lon, int radius_km) {
  char line[256];
  aprs_build_login(line, sizeof(line), callsign, passcode, lat, lon, radius_km);
  a_send(handle, line);
}

void aprs_build_filter(char *out, unsigned max, double lat, double lon,
                       int radius_km, const char *extra) {
  out[0] = 0;
  a_cat(out, "r/", max);
  char db[24];
  a_dtoa(lat, db, 4); a_cat(out, db, max); a_cat(out, "/", max);
  a_dtoa(lon, db, 4); a_cat(out, db, max); a_cat(out, "/", max);
  char nb[16]; a_itoa(radius_km, nb); a_cat(out, nb, max);
  if (extra && extra[0]) { a_cat(out, " ", max); a_cat(out, extra, max); }
}

void aprs_login_ex(int handle, const char *callsign, int passcode,
                   double lat, double lon, int radius_km, const char *extra) {
  char line[760];
  line[0] = 0;
  a_cat(line, "user ", sizeof(line));
  a_cat(line, callsign, sizeof(line));
  a_cat(line, " pass ", sizeof(line));
  char nb[16]; a_itoa(passcode, nb); a_cat(line, nb, sizeof(line));
  a_cat(line, " vers Aurora 1.0 filter ", sizeof(line));
  char filt[640];
  aprs_build_filter(filt, sizeof(filt), lat, lon, radius_km, extra);
  a_cat(line, filt, sizeof(line));
  a_cat(line, "\r\n", sizeof(line));
  a_send(handle, line);
}

/* ── line framing ─────────────────────────────────────────────────────── */
static char g_rx[8192];
static int g_rxlen = 0;

int aprs_poll_line(int handle, char *line, int max) {
  /* pull whatever is available into the tail of g_rx */
  if (g_rxlen < (int)sizeof(g_rx) - 1) {
    unsigned got = hal_socket_recv(handle, g_rx + g_rxlen,
                                   (unsigned)(sizeof(g_rx) - 1 - g_rxlen));
    g_rxlen += (int)got;
  }
  for (;;) {
    int nl = -1;
    for (int i = 0; i < g_rxlen; i++) { if (g_rx[i] == '\n') { nl = i; break; } }
    if (nl < 0) {
      if (g_rxlen >= (int)sizeof(g_rx) - 1) g_rxlen = 0; /* overflow guard */
      return 0;
    }
    int len = nl;
    if (len > 0 && g_rx[len - 1] == '\r') len--;
    int out = len < max - 1 ? len : max - 1;
    for (int i = 0; i < out; i++) line[i] = g_rx[i];
    line[out] = 0;
    /* shift remaining bytes down */
    int rest = g_rxlen - (nl + 1);
    for (int i = 0; i < rest; i++) g_rx[i] = g_rx[nl + 1 + i];
    g_rxlen = rest;
    if (out == 0 || line[0] == '#') continue; /* skip blanks + server comments */
    return out;
  }
}

/* ── parsing ──────────────────────────────────────────────────────────── */
static int classify(const char *info) {
  if (!info[0]) return APRS_OTHER;
  char c = info[0];
  if (c == '!' || c == '/' || c == '=' || c == '@') return APRS_POSITION;
  if (c == '`' || c == '\'') return APRS_POSITION;
  if (c == ':') return APRS_MESSAGE;
  if (c == '>') return APRS_STATUS;
  if (c == '_') return APRS_WEATHER;
  if (c == 'T') return APRS_TELEMETRY;
  if (c == ';' || c == ')') return APRS_POSITION;
  return APRS_OTHER;
}

/* base-91 decode of `count` chars from offset */
static double b91(const char *d, int off, int count) {
  double v = 0;
  for (int i = 0; i < count; i++) {
    int ch = (int)d[off + i] - 33;
    if (ch < 0 || ch > 90) return -1e9;
    v = v * 91 + ch;
  }
  return v;
}

/* uncompressed DDMM.MMNsDDDMM.MMWc -> lat/lon; returns 1 on success */
static int parse_uncompressed(const char *d, double *lat, double *lon) {
  if ((int)a_len(d) < 19) return 0;
  int latDeg = a_sub_int(d, 0, 2);
  double latMin = a_sub_dbl(d, 2, 7);
  char latH = d[7];
  int lonDeg = a_sub_int(d, 9, 12);
  double lonMin = a_sub_dbl(d, 12, 17);
  char lonH = d[17];
  if (latDeg < 0 || lonDeg < 0) return 0;
  if (latH != 'N' && latH != 'S') return 0;
  if (lonH != 'E' && lonH != 'W') return 0;
  double la = latDeg + latMin / 60.0;
  double lo = lonDeg + lonMin / 60.0;
  if (latH == 'S') la = -la;
  if (lonH == 'W') lo = -lo;
  if (la < -90 || la > 90 || lo < -180 || lo > 180) return 0;
  *lat = la; *lon = lo; return 1;
}

/* compressed /YYYYXXXX... -> lat/lon */
static int parse_compressed(const char *d, double *lat, double *lon) {
  if ((int)a_len(d) < 13) return 0;
  double latVal = b91(d, 1, 4), lonVal = b91(d, 5, 4);
  if (latVal < -1e8 || lonVal < -1e8) return 0;
  double la = 90.0 - latVal / 380926.0;
  double lo = -180.0 + lonVal / 190463.0;
  if (la < -90 || la > 90 || lo < -180 || lo > 180) return 0;
  *lat = la; *lon = lo; return 1;
}

static int parse_position(const char *info, double *lat, double *lon) {
  char c = info[0];
  if (c == '`' || c == '\'' || c == ';' || c == ')') return 0; /* skip complex */
  const char *pos;
  if (c == '/' || c == '@') {
    if ((int)a_len(info) < 9) return 0;
    pos = info + 8;
  } else {
    pos = info + 1;
  }
  if (!pos[0]) return 0;
  if (a_digit(pos[0])) return parse_uncompressed(pos, lat, lon);
  return parse_compressed(pos, lat, lon);
}

int aprs_parse(const char *line, aprs_packet_t *out) {
  out->from[0] = 0; out->type = APRS_OTHER; out->has_pos = 0;
  out->addressee[0] = 0; out->text[0] = 0; out->msgid[0] = 0; out->comment[0] = 0;
  out->lat = 0; out->lon = 0;
  out->is_bulletin = 0; out->group[0] = 0; out->bulletin_id = 0;

  /* header up to ':' */
  int colon = -1;
  for (int i = 0; line[i]; i++) { if (line[i] == ':') { colon = i; break; } }
  if (colon < 0 || !line[colon + 1]) return 0;
  /* source call up to '>' */
  int gt = -1;
  for (int i = 0; i < colon; i++) { if (line[i] == '>') { gt = i; break; } }
  if (gt < 0) return 0;
  int fn = gt < 15 ? gt : 15;
  for (int i = 0; i < fn; i++) out->from[i] = line[i];
  out->from[fn] = 0;

  const char *info = line + colon + 1;
  out->type = classify(info);

  if (out->type == APRS_MESSAGE) {
    /* :ADDRESSEE :text{id  — addressee is 9 chars then ':' */
    int sc = -1;
    for (int i = 1; info[i]; i++) { if (info[i] == ':') { sc = i; break; } }
    if (sc > 1) {
      int an = sc - 1; if (an > 15) an = 15;
      int w = 0;
      for (int i = 1; i <= an; i++) {
        char ch = info[i];
        if (ch != ' ') out->addressee[w++] = ch;
      }
      out->addressee[w] = 0;
      /* Bulletin/announcement? addressee = "BLN" + id char + optional group.
       * (Trailing pad spaces were already stripped above.) */
      if (out->addressee[0] == 'B' && out->addressee[1] == 'L' &&
          out->addressee[2] == 'N' && out->addressee[3]) {
        out->is_bulletin = 1;
        out->bulletin_id = out->addressee[3];
        int gi = 0;
        for (int i = 4; out->addressee[i] && gi < 7; i++) {
          out->group[gi++] = out->addressee[i];
        }
        out->group[gi] = 0;
      }
      const char *body = info + sc + 1;
      int brace = -1;
      for (int i = 0; body[i]; i++) { if (body[i] == '{') { brace = i; break; } }
      int tn = brace >= 0 ? brace : (int)a_len(body);
      if (tn > 159) tn = 159;
      for (int i = 0; i < tn; i++) out->text[i] = body[i];
      out->text[tn] = 0;
      if (brace >= 0) {
        int k = 0;
        for (int i = brace + 1; body[i] && k < 15; i++) out->msgid[k++] = body[i];
        out->msgid[k] = 0;
      }
    }
  } else if (out->type == APRS_POSITION) {
    double la, lo;
    if (parse_position(info, &la, &lo)) {
      out->has_pos = 1; out->lat = la; out->lon = lo;
    }
    /* Trailing comment after the fixed position block (best-effort). */
    char c0 = info[0];
    const char *pos = 0;
    if (c0 == '!' || c0 == '=') pos = info + 1;
    else if ((c0 == '/' || c0 == '@') && (int)a_len(info) >= 9) pos = info + 8;
    if (pos && pos[0]) {
      int skip = a_digit(pos[0]) ? 19 : 13; /* uncompressed : compressed */
      if ((int)a_len(pos) > skip) {
        const char *cm = pos + skip;
        int w = 0;
        for (int i = 0; cm[i] && w < 79; i++) out->comment[w++] = cm[i];
        out->comment[w] = 0;
      }
    }
  }
  return 1;
}

/* ── transmit ─────────────────────────────────────────────────────────── */
static void fmt_lat(double dd, char *b) {
  char h = 'N'; if (dd < 0) { h = 'S'; dd = -dd; }
  int deg = (int)dd; double m = (dd - deg) * 60.0;
  int mi = (int)m; int mh = (int)((m - mi) * 100.0 + 0.5);
  int i = 0;
  b[i++] = (char)('0' + (deg / 10) % 10); b[i++] = (char)('0' + deg % 10);
  b[i++] = (char)('0' + (mi / 10) % 10);  b[i++] = (char)('0' + mi % 10);
  b[i++] = '.';
  b[i++] = (char)('0' + (mh / 10) % 10);  b[i++] = (char)('0' + mh % 10);
  b[i++] = h; b[i] = 0;
}
static void fmt_lon(double dd, char *b) {
  char h = 'E'; if (dd < 0) { h = 'W'; dd = -dd; }
  int deg = (int)dd; double m = (dd - deg) * 60.0;
  int mi = (int)m; int mh = (int)((m - mi) * 100.0 + 0.5);
  int i = 0;
  b[i++] = (char)('0' + (deg / 100) % 10); b[i++] = (char)('0' + (deg / 10) % 10);
  b[i++] = (char)('0' + deg % 10);
  b[i++] = (char)('0' + (mi / 10) % 10);   b[i++] = (char)('0' + mi % 10);
  b[i++] = '.';
  b[i++] = (char)('0' + (mh / 10) % 10);   b[i++] = (char)('0' + mh % 10);
  b[i++] = h; b[i] = 0;
}

void aprs_build_message_via(char *out, unsigned max, const char *from,
                            const char *to, const char *text, int seq,
                            const char *via) {
  out[0] = 0;
  a_cat(out, from, max);
  a_cat(out, ">APRS", max);
  if (via && via[0]) { a_cat(out, ",", max); a_cat(out, via, max); }
  a_cat(out, "::", max);
  /* addressee: callsign uppercased, padded to 9. (Groups use bulletins —
   * see aprs_build_bulletin.) */
  char dest[10];
  int i = 0; for (; to[i] && i < 9; i++) dest[i] = a_up(to[i]); dest[i] = 0;
  while (a_len(dest) < 9) a_cat(dest, " ", sizeof(dest));
  a_cat(out, dest, max);
  a_cat(out, ":", max);
  a_cat(out, text, max);
  /* seq < 0 = no message number: the recipient must not ack this line (used
   * when re-originating a third-party message whose own {id we don't carry —
   * fabricating one made recipients ack ids the sender never issued). */
  if (seq >= 0) {
    a_cat(out, "{", max);
    char nb[16]; a_itoa(seq, nb); a_cat(out, nb, max);
  }
}
void aprs_build_message(char *out, unsigned max, const char *from,
                        const char *to, const char *text, int seq) {
  aprs_build_message_via(out, max, from, to, text, seq, "TCPIP*");
}

void aprs_build_beacon(char *out, unsigned max, const char *from,
                       double lat, double lon, const char *sym,
                       const char *path, const char *comment) {
  out[0] = 0;
  char la[16], lo[16];
  fmt_lat(lat, la); fmt_lon(lon, lo);
  char st[2] = { sym && sym[0] ? sym[0] : '/', 0 };
  char sc[2] = { sym && sym[0] && sym[1] ? sym[1] : '>', 0 };
  a_cat(out, from, max);
  a_cat(out, ">APRS", max);
  if (path && path[0]) { a_cat(out, ",", max); a_cat(out, path, max); }
  a_cat(out, ":!", max);
  a_cat(out, la, max);
  a_cat(out, st, max);
  a_cat(out, lo, max);
  a_cat(out, sc, max);
  if (comment && comment[0]) a_cat(out, comment, max);
}

void aprs_send_message(int handle, const char *from, const char *to,
                       const char *text, int seq) {
  char line[256];
  aprs_build_message(line, sizeof(line), from, to, text, seq);
  a_cat(line, "\r\n", sizeof(line));
  a_send(handle, line);
}

void aprs_send_raw(int handle, const char *line) {
  char buf[300];
  a_cpy(buf, line, sizeof(buf));
  a_cat(buf, "\r\n", sizeof(buf));
  a_send(handle, buf);
}

/* --- Long-message chunking (word-boundary, APRSdroid-style) ---
 * Mirrors the reference splitAprsText: split at the last space before the
 * limit, hard-breaking only when a single word is longer than the limit. */

/* Write the next chunk of s starting at *pos into out; advance *pos past
 * it (skipping the consumed separator space). Returns 1 if a chunk was
 * produced, 0 once *pos has reached the end. */
static int a_next_chunk(const char *s, int max_len, int *pos,
                        char *out, unsigned out_sz) {
  out[0] = 0;
  int len = (int)a_len(s);
  int start = *pos;
  if (start >= len) return 0;
  if (max_len < 1) max_len = 1;

  int end, next;
  if (len - start <= max_len) {
    end = len; next = len;
  } else {
    int limit = start + max_len, sp = -1;
    for (int i = limit; i > start; i--) {
      if (s[i] == ' ') { sp = i; break; }
    }
    if (sp <= start) {                                 /* hard break */
      /* Never split a multi-byte UTF-8 codepoint across two chunks (emoji are
       * 4 bytes): back up off any continuation byte (10xxxxxx) to the codepoint
       * boundary so each chunk — and the rejoined whole — stays valid UTF-8. */
      end = limit;
      while (end > start && ((unsigned char)s[end] & 0xC0) == 0x80) end--;
      if (end <= start) end = limit;                   /* codepoint > max_len */
      next = end;
    } else { end = sp; next = sp; }                    /* word boundary */
  }

  while (end > start && s[end - 1] == ' ') end--;      /* trim chunk tail */
  while (next < len && s[next] == ' ') next++;          /* skip remainder gap */

  int w = 0;
  for (int i = start; i < end && (unsigned)(w + 1) < out_sz; i++) out[w++] = s[i];
  out[w] = 0;
  *pos = next;
  return 1;
}

int aprs_part_count(const char *text, int max_len) {
  if (!text || !text[0]) return 0;
  char tmp[80];
  int pos = 0, n = 0;
  while (a_next_chunk(text, max_len, &pos, tmp, sizeof(tmp))) n++;
  return n;
}

int aprs_split_text(const char *text, int max_len, int idx,
                    char *out, unsigned out_sz) {
  if (out_sz) out[0] = 0;
  if (!text || idx < 0) return 0;
  char tmp[80];
  int pos = 0, k = 0;
  while (a_next_chunk(text, max_len, &pos, tmp, sizeof(tmp))) {
    if (k == idx) { a_cpy(out, tmp, out_sz); return 1; }
    k++;
  }
  return 0;
}

int aprs_send_message_multi(int handle, const char *from, const char *to,
                            const char *text, int max_len, int *seq) {
  if (max_len < 1) max_len = 1;
  if (max_len > APRS_MAX_MSG_LEN) max_len = APRS_MAX_MSG_LEN;
  char chunk[80];
  int pos = 0, n = 0;
  while (a_next_chunk(text, max_len, &pos, chunk, sizeof(chunk))) {
    aprs_send_message(handle, from, to, chunk, (*seq)++);
    n++;
  }
  return n;
}

/* --- APRS bulletins / group messaging --- */

void aprs_build_bulletin_via(char *out, unsigned max, const char *from,
                             const char *group, char line_id, const char *text,
                             const char *via) {
  out[0] = 0;
  a_cat(out, from, max);
  a_cat(out, ">APRS", max);
  if (via && via[0]) { a_cat(out, ",", max); a_cat(out, via, max); }
  a_cat(out, "::", max);
  /* addressee: "BLN" + line id + up to 5 uppercase group chars, padded to 9 */
  char dest[10];
  dest[0] = 'B'; dest[1] = 'L'; dest[2] = 'N';
  dest[3] = line_id ? line_id : '0'; dest[4] = 0;
  for (int i = 0; group && group[i] && i < 5; i++) {
    char s[2] = { a_up(group[i]), 0 };
    a_cat(dest, s, sizeof(dest));
  }
  while (a_len(dest) < 9) a_cat(dest, " ", sizeof(dest));
  a_cat(out, dest, max);
  a_cat(out, ":", max);
  a_cat(out, text, max);   /* bulletins carry no {seq */
}
void aprs_build_bulletin(char *out, unsigned max, const char *from,
                         const char *group, char line_id, const char *text) {
  aprs_build_bulletin_via(out, max, from, group, line_id, text, "TCPIP*");
}

int aprs_send_bulletin_multi(int handle, const char *from, const char *group,
                             const char *text, int max_len) {
  if (max_len < 1) max_len = 1;
  if (max_len > APRS_MAX_MSG_LEN) max_len = APRS_MAX_MSG_LEN;
  char chunk[80];
  int pos = 0, n = 0;
  while (n < 10 && a_next_chunk(text, max_len, &pos, chunk, sizeof(chunk))) {
    char line[256];
    aprs_build_bulletin(line, sizeof(line), from, group, (char)('0' + n), chunk);
    a_cat(line, "\r\n", sizeof(line));
    a_send(handle, line);
    n++;
  }
  return n;
}

void aprs_send_beacon(int handle, const char *from, double lat, double lon,
                      const char *sym, const char *path, const char *comment) {
  char line[256];
  aprs_build_beacon(line, sizeof(line), from, lat, lon, sym, path, comment);
  a_cat(line, "\r\n", sizeof(line));
  a_send(handle, line);
}
