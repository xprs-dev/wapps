#include "db.h"
#include "xprs_wasm_hal.h"

/* ── strings ──────────────────────────────────────────────────────── */
unsigned s_len(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
int s_eq(const char *a, const char *b) {
  while (*a && *b && *a == *b) { a++; b++; }
  return *a == *b;
}
int s_pre(const char *s, const char *pre) {
  while (*pre) { if (*s != *pre) return 0; s++; pre++; }
  return 1;
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
char s_up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
void u_lltoa(unsigned long long v, char *out) {
  char t[24]; int j = 0;
  if (v == 0) t[j++] = '0';
  while (v > 0) { t[j++] = (char)('0' + v % 10); v /= 10; }
  int k = 0; while (j > 0) out[k++] = t[--j]; out[k] = 0;
}
void u_itoa(unsigned v, char *out) { u_lltoa(v, out); }

/* ── JSON, read ───────────────────────────────────────────────────── */
static int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
/* Find `"key":` and return a pointer just past the colon, or 0. */
static const char *jfind(const char *buf, const char *key) {
  char pat[80]; pat[0] = '"'; pat[1] = 0;
  s_cat(pat, key, sizeof(pat)); s_cat(pat, "\":", sizeof(pat));
  unsigned pl = s_len(pat);
  for (const char *p = buf; *p; p++) {
    unsigned i = 0;
    while (i < pl && p[i] == pat[i]) i++;
    if (i == pl) return p + pl;
  }
  return 0;
}
/* Decode a JSON string body starting after its opening quote into out. */
static const char *jdecode(const char *p, char *out, unsigned m) {
  unsigned i = 0;
  while (*p && *p != '"' && i < m - 1) {
    if (*p == '\\' && p[1]) {
      p++;
      /* \uXXXX -> one byte. The host JSON-encodes received bytes, so the 0x1f
       * field separator arrives as . */
      if (*p == 'u' && hexv(p[1]) >= 0 && hexv(p[2]) >= 0 &&
          hexv(p[3]) >= 0 && hexv(p[4]) >= 0) {
        int v = (hexv(p[1]) << 12) | (hexv(p[2]) << 8) |
                (hexv(p[3]) << 4) | hexv(p[4]);
        p += 5;
        out[i++] = (char)(v & 0xff);
      } else if (*p == 'n') { out[i++] = '\n'; p++; }
      else if (*p == 't') { out[i++] = '\t'; p++; }
      else if (*p == 'r') { p++; }
      else { out[i++] = *p++; }
    } else out[i++] = *p++;
  }
  out[i] = 0;
  return p;
}
int jstr(const char *buf, const char *key, char *out, unsigned m) {
  out[0] = 0;
  const char *p = jfind(buf, key);
  if (!p) return 0;
  while (*p == ' ') p++;
  if (*p != '"') return 0;
  jdecode(p + 1, out, m);
  return 1;
}
long long jint(const char *buf, const char *key) {
  const char *p = jfind(buf, key);
  if (!p) return 0;
  while (*p == ' ') p++;
  int neg = 0;
  if (*p == '-') { neg = 1; p++; }
  long long v = 0;
  while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
  return neg ? -v : v;
}
int jbool_def(const char *buf, const char *key, int def) {
  const char *p = jfind(buf, key);
  if (!p) return def;
  while (*p == ' ') p++;
  return *p == 't' || *p == '1';
}
int jbool(const char *buf, const char *key) { return jbool_def(buf, key, 0); }

/* The row carries the packet's fields verbatim and IN ORDER, as [key, value]
 * pairs -- pairs and not an object because XPRS allows a key to repeat and
 * the section 5 identifier is derived from the order. Values are quoted and
 * may contain `]`, so the walk skips strings rather than scanning bytes. */
int jfield(const char *row, const char *key, char *out, unsigned m) {
  out[0] = 0;
  const char *arr = row;
  {
    const char *pat = "\"fields\":[";
    unsigned pl = s_len(pat);
    for (;; arr++) {
      if (!*arr) return 0;
      unsigned i = 0;
      while (i < pl && arr[i] == pat[i]) i++;
      if (i == pl) { arr += pl; break; }
    }
  }
  char pat[40]; pat[0] = '['; pat[1] = '"'; pat[2] = 0;
  s_cat(pat, key, sizeof(pat)); s_cat(pat, "\",\"", sizeof(pat));
  unsigned pl = s_len(pat);
  if (*arr == ']') return 0;
  for (const char *q = arr; *q; q++) {
    if (*q == ']' && q[1] == ']') break;
    if (*q == '"') {
      for (q++; *q && *q != '"'; q++) if (*q == '\\' && q[1]) q++;
      if (!*q) break;
      continue;
    }
    if (*q != '[') continue;
    unsigned i = 0;
    while (i < pl && q[i] == pat[i]) i++;
    if (i != pl) continue;
    jdecode(q + pl, out, m);
    return 1;
  }
  return 0;
}

int next_object(const char **cur, char *out, unsigned cap) {
  const char *p = *cur;
  while (*p && *p != '{') p++;
  if (*p != '{') { *cur = p; out[0] = 0; return 0; }
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

/* ── JSON, write ──────────────────────────────────────────────────── */
void jesc(char *dst, unsigned m, const char *src) {
  unsigned l = s_len(dst);
  for (const char *p = src; *p && l + 6 < m; p++) {
    unsigned char c = (unsigned char)*p;
    if (c == '"' || c == '\\') { dst[l++] = '\\'; dst[l++] = (char)c; }
    else if (c == '\n') { dst[l++] = '\\'; dst[l++] = 'n'; }
    else if (c == '\t') { dst[l++] = '\\'; dst[l++] = 't'; }
    else if (c < 0x20) {
      /* Any other control byte as \u00XX: the host's decoder refuses a raw
       * one, and a refused message is a bubble that never appears. */
      static const char *H = "0123456789abcdef";
      dst[l++] = '\\'; dst[l++] = 'u'; dst[l++] = '0'; dst[l++] = '0';
      dst[l++] = H[c >> 4]; dst[l++] = H[c & 15];
    }
    else dst[l++] = (char)c;
  }
  dst[l] = 0;
}

void pj_init(pj_t *p) { p->b[0] = '['; p->b[1] = 0; p->first = 1; }
static void pj_sep(pj_t *p) {
  if (!p->first) s_cat(p->b, ",", sizeof(p->b));
  p->first = 0;
}
void pj_str(pj_t *p, const char *s) {
  pj_sep(p);
  s_cat(p->b, "\"", sizeof(p->b));
  jesc(p->b, sizeof(p->b), s ? s : "");
  s_cat(p->b, "\"", sizeof(p->b));
}
void pj_int(pj_t *p, long long v) {
  pj_sep(p);
  char nb[24];
  if (v < 0) { s_cat(p->b, "-", sizeof(p->b)); u_lltoa((unsigned long long)(-v), nb); }
  else u_lltoa((unsigned long long)v, nb);
  s_cat(p->b, nb, sizeof(p->b));
}
const char *pj_done(pj_t *p) { s_cat(p->b, "]", sizeof(p->b)); return p->b; }

/* ── sqlite ───────────────────────────────────────────────────────── */
int db_open(const char *relpath) {
  return hal_sqlite_open(relpath, s_len(relpath));
}
void db_close(int h) { if (h >= 0) hal_sqlite_close(h); }
int db_exec(int h, const char *sql, const char *params) {
  return hal_sqlite_exec(h, sql, s_len(sql),
                         params ? params : "", params ? s_len(params) : 0);
}
int db_query(int h, const char *sql, const char *params, char *out, unsigned cap) {
  int n = hal_sqlite_query(h, sql, s_len(sql),
                           params ? params : "", params ? s_len(params) : 0,
                           out, cap - 1);
  if (n >= 0 && (unsigned)n < cap) out[n] = 0; else out[0] = 0;
  return n;
}
long long db_int(int h, const char *sql, const char *params, long long def) {
  char out[160];
  if (db_query(h, sql, params, out, sizeof(out)) <= 0) return def;
  /* [{"n":42}] -- the first number after the first colon. Null answers def:
   * a SUM over nothing is NULL, and max(ts) of an empty room is too. */
  const char *p = out;
  while (*p && *p != ':') p++;
  if (!*p) return def;
  p++;
  while (*p == ' ') p++;
  if (*p == 'n') return def;
  int neg = 0;
  if (*p == '-') { neg = 1; p++; }
  if (*p < '0' || *p > '9') return def;
  long long v = 0;
  while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
  return neg ? -v : v;
}

void db_init_index(int h) {
  db_exec(h,
          "CREATE TABLE IF NOT EXISTS rooms("
          "id TEXT PRIMARY KEY,"           /* "#LOCAL", "#NAME", "X5ABCD", "X16JK8" */
          "file TEXT NOT NULL,"            /* rooms/<encoded>.sqlite3, written once */
          "title TEXT NOT NULL DEFAULT '',"
          "activity_ts INTEGER NOT NULL DEFAULT 0,"
          "unread INTEGER NOT NULL DEFAULT 0,"
          "last_line TEXT NOT NULL DEFAULT '',"
          "closed INTEGER NOT NULL DEFAULT 0,"
          "private INTEGER NOT NULL DEFAULT 0)",
          0);
  db_exec(h, "CREATE INDEX IF NOT EXISTS rooms_act ON rooms(activity_ts DESC)", 0);
  db_exec(h, "CREATE TABLE IF NOT EXISTS blocked(call TEXT PRIMARY KEY)", 0);
  db_exec(h, "CREATE TABLE IF NOT EXISTS hidden(mid TEXT PRIMARY KEY)", 0);
  /* Which room a 1:1 we sent lives in, keyed by the receipt id the core
   * reports on xprs.status.tx -- so a tick can find its bubble after a
   * restart. */
  db_exec(h, "CREATE TABLE IF NOT EXISTS tx(rid TEXT PRIMARY KEY, room TEXT NOT NULL)", 0);
  db_exec(h, "CREATE TABLE IF NOT EXISTS meta(k TEXT PRIMARY KEY, v TEXT)", 0);
}

void db_init_room(int h) {
  db_exec(h,
          "CREATE TABLE IF NOT EXISTS messages("
          "seq INTEGER PRIMARY KEY AUTOINCREMENT,"   /* display order */
          "mid TEXT NOT NULL UNIQUE,"                /* THE dedup */
          "dir TEXT NOT NULL,"                       /* in | out */
          "sender TEXT NOT NULL,"
          "ts INTEGER NOT NULL,"                     /* sender epoch, else arrival */
          "body TEXT NOT NULL,"
          "parent TEXT NOT NULL DEFAULT '',"
          "via TEXT NOT NULL DEFAULT '',"
          "auth TEXT NOT NULL DEFAULT '',"
          "enc INTEGER NOT NULL DEFAULT 0,"
          "rid TEXT NOT NULL DEFAULT '',"
          "status TEXT NOT NULL DEFAULT '',"
          "sys INTEGER NOT NULL DEFAULT 0)",
          0);
  db_exec(h, "CREATE TABLE IF NOT EXISTS reactions("
             "mid TEXT, who TEXT, kind TEXT, PRIMARY KEY(mid, who))", 0);
  db_exec(h, "CREATE TABLE IF NOT EXISTS meta(k TEXT PRIMARY KEY, v TEXT)", 0);
  /* Whether an inbound message has had its s:read sent (XPRS.md 13.7). A per-
   * message flag on the row itself, in the DATABASE not RAM, because the
   * message is stored by whichever engine hears it (often the headless one)
   * while the room is opened by the page engine -- the two share this file and
   * nothing else. Opening a room acks every inbound row still at 0, so OLDER
   * messages get read receipts too, not only ones that arrived live. Column is
   * added by ALTER for tables created before it existed; the duplicate-column
   * error on a table that already has it is ignored (db_exec never throws). */
  db_exec(h, "ALTER TABLE messages ADD COLUMN read_sent INTEGER NOT NULL DEFAULT 0", 0);
}
