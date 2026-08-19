/*
 * Torrents — folder torrents over Reticulum (aurora/docs/torrents.md)
 *
 * The unit of sharing is a FOLDER, not a file, and its address is a key
 * (`ntorrent1…`) rather than a hash of its contents — so a publisher can add or
 * remove files and every seeder converges on the new state under the SAME link.
 * Files inside stay content-addressed (sha256), exactly like BitTorrent: the
 * directory is mutable, the bytes are not.
 *
 * The tracker is the Indexer mesh: it answers "who has this folder" with a list
 * of devices and their physical profile (mains or battery, WiFi or cellular,
 * how recently we heard them), never with bytes. Pinning is how a device joins
 * the swarm: keep a full copy, follow the op-log, and advertise yourself as a
 * holder — which is what stops the publisher's phone from being the only source.
 *
 * All storage and networking is host-side behind hal_folder_* (create, browse,
 * download, pin, swarm, link). This module renders and applies policy.
 *
 * Performance (aurora/docs/performance.md): this wapp also runs in the
 * BACKGROUND with no page attached, so the tick must stay near-free. Every
 * render is diffed before it is sent (changed_send), the host HALs are polled on
 * long periods rather than every tick, and the swarm — whose refresh is a DHT
 * walk — is only asked about the torrent the user actually has open.
 */

#include <stdint.h>
#include "xprs_wasm_hal.h"

/* ── tiny libc (same helpers as the other wapps) ──────────────────────────── */
static unsigned s_len(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static int s_eq(const char *a, const char *b) {
  while (*a && *b && *a == *b) { a++; b++; } return *a == *b;
}
static int s_pre(const char *s, const char *pre) {
  while (*pre) { if (*s != *pre) return 0; s++; pre++; } return 1;
}
static void s_cpy(char *d, const char *s, unsigned m) {
  unsigned i = 0; while (i < m - 1 && s[i]) { d[i] = s[i]; i++; } d[i] = 0;
}
static void s_cat(char *d, const char *s, unsigned m) {
  unsigned l = s_len(d), i = 0;
  while (l + i < m - 1 && s[i]) { d[l + i] = s[i]; i++; } d[l + i] = 0;
}
static void u_itoa(unsigned v, char *out) {
  char t[12]; int j = 0;
  if (v == 0) t[j++] = '0';
  while (v > 0) { t[j++] = (char)('0' + v % 10); v /= 10; }
  int k = 0; while (j > 0) out[k++] = t[--j];
  out[k] = 0;
}
static int to_int(const char *s) {
  int v = 0;
  while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
  return v;
}
/* JSON string escaping. The TAB matters: row ids carry "sha\tname", and a raw
 * control character inside a JSON string makes the WHOLE message unparseable —
 * the host drops it, the list never updates, and the wapp looks dead while
 * being perfectly alive. (That is exactly what happened: tapping a torrent did
 * nothing, and the log said "dropped unparseable message: Control character in
 * string".) jstr() already decodes \t; this is the missing other half. */
static void jesc(char *dst, unsigned m, const char *src) {
  unsigned l = s_len(dst);
  for (const char *p = src; *p && l < m - 3; p++) {
    char c = *p;
    if (c == '"' || c == '\\') { dst[l++] = '\\'; dst[l++] = c; }
    else if (c == '\n') { dst[l++] = '\\'; dst[l++] = 'n'; }
    else if (c == '\t') { dst[l++] = '\\'; dst[l++] = 't'; }
    else if (c == '\r') { dst[l++] = '\\'; dst[l++] = 'r'; }
    else if ((unsigned char)c < 0x20) { dst[l++] = ' '; }  /* never raw */
    else dst[l++] = c;
  }
  dst[l] = 0;
}
/* "key":"value" → out. Decodes \t and \n, which we use as field separators. */
static int jstr(const char *buf, const char *key, char *out, unsigned m) {
  char pat[80]; pat[0] = '"'; pat[1] = 0;
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
        char c = *p++;
        if (c == 'n') out[i++] = '\n';
        else if (c == 't') out[i++] = '\t';
        else if (c == 'r') out[i++] = '\r';
        else out[i++] = c;
      } else out[i++] = *p++;
    }
    out[i] = 0; return 1;
  }
  out[0] = 0; return 0;
}
static int jnum(const char *buf, const char *key) {
  char pat[80]; pat[0] = '"'; pat[1] = 0;
  s_cat(pat, key, sizeof(pat)); s_cat(pat, "\":", sizeof(pat));
  unsigned pl = s_len(pat);
  for (const char *p = buf; *p; p++) {
    int ok = 1;
    for (unsigned i = 0; i < pl; i++) { if (p[i] != pat[i]) { ok = 0; break; } }
    if (!ok) continue;
    return to_int(p + pl);
  }
  return 0;
}
static int jbool_def(const char *buf, const char *key, int def) {
  char pat[80]; pat[0] = '"'; pat[1] = 0;
  s_cat(pat, key, sizeof(pat)); s_cat(pat, "\":", sizeof(pat));
  unsigned pl = s_len(pat);
  for (const char *p = buf; *p; p++) {
    int ok = 1;
    for (unsigned i = 0; i < pl; i++) { if (p[i] != pat[i]) { ok = 0; break; } }
    if (!ok) continue;
    p += pl; while (*p == ' ') p++;
    return *p == 't' || *p == '1';
  }
  return def;
}
/* djb2 — change-detection so an unchanged list is never re-sent (a re-sent list
 * resets the user's scroll position, and costs a rebuild for nothing). */
static uint32_t djb2(const char *s) {
  uint32_t h = 5381;
  for (; *s; s++) h = ((h << 5) + h) ^ (unsigned char)*s;
  return h;
}
static int changed_send(const char *m, uint32_t *last) {
  uint32_t h = djb2(m);
  if (h == *last) return 0;
  *last = h;
  hal_msg_send(m, s_len(m));
  return 1;
}

static void notify(const char *level, const char *body) {
  char m[512] = "{\"type\":\"notify\",\"level\":\"";
  s_cat(m, level, sizeof(m));
  s_cat(m, "\",\"title\":\"Torrents\",\"body\":\"", sizeof(m));
  jesc(m, sizeof(m), body);
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
static void log_clear(const char *field) {
  char m[80] = "{\"type\":\"ui.log.clear\",\"field\":\"";
  s_cat(m, field, sizeof(m)); s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
static void log_line(const char *field, const char *text) {
  char m[600] = "{\"type\":\"ui.log.append\",\"field\":\"";
  s_cat(m, field, sizeof(m));
  s_cat(m, "\",\"line\":\"", sizeof(m));
  jesc(m, sizeof(m), text);
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
/* Prefill a host scalar field (the Edit-listing inputs). */
static void field_set(const char *field, const char *value) {
  char m[500] = "{\"type\":\"ui.field.set\",\"field\":\"";
  s_cat(m, field, sizeof(m));
  s_cat(m, "\",\"value\":\"", sizeof(m));
  jesc(m, sizeof(m), value);
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* Copy the raw `"key":[ ... ]` array out of a JSON object, brackets included
 * ("[]" when absent). Used to carry the gallery through a save untouched: the
 * Edit screen changes WORDS, and it must not silently drop the artwork. */
static void copy_array(const char *json, const char *key, char *out, unsigned m) {
  s_cpy(out, "[]", m);
  char pat[32]; pat[0] = '"'; pat[1] = 0;
  s_cat(pat, key, sizeof(pat));
  s_cat(pat, "\":[", sizeof(pat));
  const char *p = json;
  while (*p && !s_pre(p, pat)) p++;
  if (!*p) return;
  p += s_len(pat) - 1;             /* land on '[' */
  unsigned i = 0;
  while (*p && i < m - 1) {
    out[i++] = *p;
    if (*p == ']') break;
    p++;
  }
  out[i] = 0;
}

static void prompt_copy(const char *title, const char *body, const char *copyval) {
  /* Copy AND a QR of the same value — show a code someone can scan instead of
   * copy-pasting a long link. The QR renders on every platform (desktop too). */
  char m[1600] = "{\"type\":\"ui.prompt\",\"id\":\"noop\",\"title\":\"";
  jesc(m, sizeof(m), title);
  s_cat(m, "\",\"body\":\"", sizeof(m)); jesc(m, sizeof(m), body);
  s_cat(m, "\",\"copy\":\"", sizeof(m)); jesc(m, sizeof(m), copyval);
  s_cat(m, "\",\"qr\":\"", sizeof(m)); jesc(m, sizeof(m), copyval);
  s_cat(m, "\",\"confirm\":\"Close\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
static void prompt_input(const char *id, const char *title, const char *hint,
                         unsigned mx) {
  char m[400] = "{\"type\":\"ui.prompt\",\"id\":\"";
  s_cat(m, id, sizeof(m));
  s_cat(m, "\",\"title\":\"", sizeof(m)); jesc(m, sizeof(m), title);
  s_cat(m, "\",\"input\":{\"hint\":\"", sizeof(m)); jesc(m, sizeof(m), hint);
  s_cat(m, "\",\"max\":", sizeof(m));
  { char nb[12]; u_itoa(mx, nb); s_cat(m, nb, sizeof(m)); }
  s_cat(m, "},\"confirm\":\"OK\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* Sizes the way a torrent client shows them. */
static void fmt_size(unsigned b, char *out, unsigned m) {
  char nb[16];
  out[0] = 0;
  if (b < 1024u) { u_itoa(b, nb); s_cat(out, nb, m); s_cat(out, " B", m); return; }
  if (b < 1024u * 1024u) {
    u_itoa(b / 1024u, nb); s_cat(out, nb, m); s_cat(out, " KB", m); return;
  }
  if (b < 1024u * 1024u * 1024u) {
    u_itoa(b / (1024u * 1024u), nb); s_cat(out, nb, m);
    s_cat(out, " MB", m); return;
  }
  u_itoa(b / (1024u * 1024u * 1024u), nb); s_cat(out, nb, m);
  s_cat(out, " GB", m);
}
/* "40s" / "12m" / "3h" — how long ago we heard a holder. */
static void fmt_age(unsigned ms, char *out, unsigned m) {
  char nb[16];
  unsigned secs = ms / 1000u;
  out[0] = 0;
  if (secs < 90u) { u_itoa(secs, nb); s_cat(out, nb, m); s_cat(out, "s", m); return; }
  if (secs < 5400u) { u_itoa(secs / 60u, nb); s_cat(out, nb, m); s_cat(out, "m", m); return; }
  u_itoa(secs / 3600u, nb); s_cat(out, nb, m); s_cat(out, "h", m);
}

/* ── state ───────────────────────────────────────────────────────────────── */
static char g_json[65536];   /* HAL replies */
static char g_out[65536];    /* ui.people.set payload */

/* 0 = the torrent list, 1 = inside one torrent. */
static int  g_view = 0;
static char g_cur[80] = "";        /* open folderId (hex) */
static char g_cur_name[120] = "";
static char g_cur_path[512] = "";  /* "" = root, else ends with '/' */
static char g_sel[80] = "";        /* the torrent a "..." menu was opened on */

/* Search panel state. */
static char g_srch_q[80] = "";     /* free-text query */
static char g_srch_cat[24] = "";   /* restrict to one category ("" = all) */
static char g_srch_sort[12] = "seeders"; /* seeders | updated | size */
static uint32_t g_srch_hash = 0;
static int g_srch_busy = 0;        /* the mesh fan-out is still in flight */

/* Dashboard (the hero stats over the list) state. */
static uint32_t g_dash_hash = 0;
static unsigned g_dash_at = 0;     /* tick of the last aggregate walk */
static int g_dash_dirty = 1;       /* something changed: recount promptly */

/* Library (main list) state: the download-folder tree we are navigating. */
static char g_lib_path[512] = "";  /* current subfolder ("" = root) */
static int  g_filter = 0;          /* 0 = All, 1 = Mine (owner-created only) */
static int  g_pick_root = 0;       /* a folder picker is choosing the download root */

static uint32_t g_list_hash = 0;
static unsigned g_tick = 0;

/* Settings (KV) */
static int g_pin_on_open = 1;
static int g_rescan_min = 15;
static int g_share_author = 0;   /* include my npub in shared links — OFF by default */

static void settings_save(void) {
  char b[24]; b[0] = 0;
  s_cat(b, g_pin_on_open ? "1" : "0", sizeof(b));
  { char nb[12]; u_itoa((unsigned)g_rescan_min, nb); s_cat(b, nb, sizeof(b)); }
  hal_kv_set("cfg", 3, b, s_len(b));
  hal_kv_set("aut", 3, g_share_author ? "1" : "0", 1);
}
static void settings_load(void) {
  char b[24];
  uint32_t n = hal_kv_get("cfg", 3, b, sizeof(b) - 1);
  if (n >= 2) {
    b[n] = 0;
    g_pin_on_open = b[0] == '1';
    int r = to_int(b + 1);
    if (r >= 0 && r < 10000) g_rescan_min = r;
  }
  char a[4];
  if (hal_kv_get("aut", 3, a, 3) >= 1) g_share_author = a[0] == '1';
}

/* ── the torrent list ────────────────────────────────────────────────────── */

/* Copy the next JSON object at or after [p] into [slice] and return the cursor
 * just past it (NULL when there is none left). The host's folder HALs answer
 * with arrays of objects, so a brace counter is all the parsing we need — and
 * one iterator keeps every list walk in this file identical. */
static const char *next_obj(const char *p, char *slice, unsigned m) {
  if (!p) return 0;
  while (*p && *p != '{') p++;
  if (!*p) return 0;
  int depth = 0;
  unsigned i = 0;
  while (*p) {
    if (*p == '{') depth++;
    else if (*p == '}') depth--;
    if (i < m - 1) slice[i++] = *p;
    p++;
    if (depth == 0) break;
  }
  slice[i] = 0;
  return p;
}

/* One row of the torrent list, appended to g_out. */
static int g_first_row = 1;
static void row_open_field(const char *field) {
  g_first_row = 1;
  g_out[0] = 0;
  s_cat(g_out, "{\"type\":\"ui.people.set\",\"field\":\"", sizeof(g_out));
  s_cat(g_out, field, sizeof(g_out));
  s_cat(g_out, "\",\"sections\":[", sizeof(g_out));
}
static void row_open(void) { row_open_field("torrents"); }
static void section_open(const char *title) {
  if (!g_first_row) s_cat(g_out, "]},", sizeof(g_out));
  s_cat(g_out, "{\"title\":\"", sizeof(g_out));
  jesc(g_out, sizeof(g_out), title);
  s_cat(g_out, "\",\"items\":[", sizeof(g_out));
  g_first_row = 1;
}
/* A row, optionally with an icon. Pass icon=0 for a torrent (it keeps the
 * generated avatar, which makes one key distinguishable from another at a
 * glance); pass a name for a folder or a file, where a random coloured sigil
 * says nothing and the TYPE is the whole point. */
static void row_icon(const char *id, const char *title, const char *subtitle,
                     const char *icon) {
  if (!g_first_row) s_cat(g_out, ",", sizeof(g_out));
  g_first_row = 0;
  s_cat(g_out, "{\"id\":\"", sizeof(g_out)); jesc(g_out, sizeof(g_out), id);
  s_cat(g_out, "\",\"title\":\"", sizeof(g_out)); jesc(g_out, sizeof(g_out), title);
  s_cat(g_out, "\",\"subtitle\":\"", sizeof(g_out)); jesc(g_out, sizeof(g_out), subtitle);
  if (icon && icon[0]) {
    s_cat(g_out, "\",\"icon\":\"", sizeof(g_out));
    jesc(g_out, sizeof(g_out), icon);
  }
  s_cat(g_out, "\"}", sizeof(g_out));
}
static void row(const char *id, const char *title, const char *subtitle) {
  row_icon(id, title, subtitle, 0);
}

/* A torrent row whose avatar is a media token (its favicon-style icon). When the
 * token is empty the host draws the generated key-sigil, as before. */
static void row_avatar(const char *id, const char *title, const char *subtitle,
                       const char *avatar) {
  if (!avatar || !avatar[0]) { row(id, title, subtitle); return; }
  if (!g_first_row) s_cat(g_out, ",", sizeof(g_out));
  g_first_row = 0;
  s_cat(g_out, "{\"id\":\"", sizeof(g_out)); jesc(g_out, sizeof(g_out), id);
  s_cat(g_out, "\",\"title\":\"", sizeof(g_out)); jesc(g_out, sizeof(g_out), title);
  s_cat(g_out, "\",\"subtitle\":\"", sizeof(g_out)); jesc(g_out, sizeof(g_out), subtitle);
  s_cat(g_out, "\",\"avatar\":\"", sizeof(g_out)); jesc(g_out, sizeof(g_out), avatar);
  s_cat(g_out, "\"}", sizeof(g_out));
}

/* The icon a file's NAME earns it. The extension is all we have (the bytes may
 * not even be here yet), and it is what the OS routes on anyway. */
static const char *icon_for(const char *name) {
  const char *dot = 0;
  for (const char *p = name; *p; p++) {
    if (*p == '.') dot = p;
  }
  if (!dot || !dot[1]) return "insert_drive_file";
  char e[8];
  unsigned i = 0;
  for (const char *p = dot + 1; *p && i < sizeof(e) - 1; p++) {
    char c = *p;
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    e[i++] = c;
  }
  e[i] = 0;
  if (s_eq(e, "jpg") || s_eq(e, "jpeg") || s_eq(e, "png") || s_eq(e, "gif") ||
      s_eq(e, "webp") || s_eq(e, "bmp") || s_eq(e, "svg"))
    return "image";
  if (s_eq(e, "mp4") || s_eq(e, "mkv") || s_eq(e, "webm") || s_eq(e, "avi") ||
      s_eq(e, "mov"))
    return "movie";
  if (s_eq(e, "mp3") || s_eq(e, "ogg") || s_eq(e, "wav") || s_eq(e, "flac") ||
      s_eq(e, "opus") || s_eq(e, "m4a"))
    return "audiotrack";
  if (s_eq(e, "pdf")) return "picture_as_pdf";
  if (s_eq(e, "apk")) return "android";
  if (s_eq(e, "zip") || s_eq(e, "gz") || s_eq(e, "xz") || s_eq(e, "tar") ||
      s_eq(e, "7z") || s_eq(e, "rar"))
    return "archive";
  if (s_eq(e, "txt") || s_eq(e, "md") || s_eq(e, "log") || s_eq(e, "json"))
    return "description";
  return "insert_drive_file";
}

/* Tell the host we are INSIDE something, so the back arrow and the system-back
 * gesture come to us as `nav_back` (up one level) instead of leaving the wapp.
 * Clearing it at the root is what makes the next back exit, as it should. */
static void nav_set(int inside, const char *title) {
  char m[300] = "{\"type\":\"ui.nav\",\"back\":";
  s_cat(m, inside ? "true" : "false", sizeof(m));
  s_cat(m, ",\"title\":\"", sizeof(m));
  jesc(m, sizeof(m), title ? title : "");
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
static void row_close(void) {
  s_cat(g_out, "]}]}", sizeof(g_out));
}

/* One row's title + subtitle, from ONE hal_folder_stats call.
 *
 * hal_folder_stats is not free on the host (it reduces the signed op-log and
 * totals the serve counters), so this is a place where a lazy render turns into
 * a hot loop: two calls per row, every 6s, per torrent. It is called once per
 * row and the result is cached — a torrent's name and size do not change between
 * two adjacent frames (docs/performance.md §4.2, "a cosmetic value never
 * deserves a hot loop").
 */
#define STATS_CACHE 24
#define STATS_TTL_TICKS 30      /* ~30s; a rescan/download refreshes it anyway */
static char g_sc_fid[STATS_CACHE][80];
static char g_sc_title[STATS_CACHE][160];
static char g_sc_sub[STATS_CACHE][200];
static char g_sc_avatar[STATS_CACHE][120];  /* the row's icon media token, if any */
static unsigned g_sc_at[STATS_CACHE];
static unsigned g_sc_used = 0;

/* Drop every cached row (after an edit/rescan/pin, where the numbers really did
 * change and the user is owed the truth immediately). */
static void stats_cache_clear(void) { g_sc_used = 0; g_dash_dirty = 1; }

static void torrent_row(const char *fid, int owned, int pinned,
                        char *title, unsigned tm, char *sub, unsigned sm,
                        char *avatar, unsigned am) {
  if (avatar) avatar[0] = 0;
  for (unsigned i = 0; i < g_sc_used; i++) {
    if (!s_eq(g_sc_fid[i], fid)) continue;
    if (g_tick - g_sc_at[i] > STATS_TTL_TICKS) break;   /* stale: re-stat below */
    s_cpy(title, g_sc_title[i], tm);
    s_cpy(sub, g_sc_sub[i], sm);
    if (avatar) s_cpy(avatar, g_sc_avatar[i], am);
    return;
  }

  char st[4096];
  uint32_t n = hal_folder_stats(fid, s_len(fid), st, sizeof(st) - 1);
  st[n] = 0;

  /* The listing's favicon-style icon (a media token), for the row avatar. */
  if (avatar) jstr(st, "icon", avatar, am);

  /* Title: what the publisher CALLED it (the listing's title, from
   * data/meta.json), else the folder's directory name, else the head of its key.
   * All three come from the SIGNED op-log — never from a link, which is unsigned
   * and therefore a lie waiting to happen (docs/torrents.md §11).
   *
   * A torrent we do not own has no directory name of ours to fall back on, so
   * without the listing every downloaded torrent read as "npub1r59ucj…" — a key,
   * where a person expects a name. */
  char name[160];
  jstr(st, "title", name, sizeof(name));
  if (!name[0]) jstr(st, "name", name, sizeof(name));
  if (name[0]) {
    s_cpy(title, name, tm);
  } else {
    char npub[90];
    jstr(st, "npub", npub, sizeof(npub));
    s_cpy(title, npub[0] ? npub : fid, tm);
    if (s_len(title) > 20) title[20] = 0;
  }

  unsigned files = (unsigned)jnum(st, "fileCount");
  unsigned bytes = (unsigned)jnum(st, "totalBytes");
  unsigned serves = (unsigned)jnum(st, "serves");
  sub[0] = 0;
  /* The category leads: it is what a person scans a torrent list FOR. */
  { char cat[32];
    jstr(st, "cat", cat, sizeof(cat));
    if (cat[0]) {
      s_cat(sub, cat, sm);
      if (jbool_def(st, "adult", 0)) s_cat(sub, " +18", sm);
      s_cat(sub, " · ", sm);
    } }
  s_cat(sub, owned ? "mine" : (pinned ? "pinned" : "following"), sm);
  { char nb[12]; u_itoa(files, nb);
    s_cat(sub, " · ", sm); s_cat(sub, nb, sm);
    s_cat(sub, files == 1 ? " file" : " files", sm); }
  { char fs[24]; fmt_size(bytes, fs, sizeof(fs));
    s_cat(sub, " · ", sm); s_cat(sub, fs, sm); }
  if (serves) {
    char nb[12]; u_itoa(serves, nb);
    s_cat(sub, " · served ", sm); s_cat(sub, nb, sm); s_cat(sub, "x", sm);
  }

  /* Cache it. Reuse this folder's slot if it has one, else append; when the
   * cache is full the oldest row goes — with a couple of dozen torrents open
   * that never happens, and if it does the cost is one extra stats call. */
  unsigned slot = g_sc_used;
  for (unsigned i = 0; i < g_sc_used; i++) {
    if (s_eq(g_sc_fid[i], fid)) { slot = i; break; }
  }
  if (slot == g_sc_used) {
    if (g_sc_used < STATS_CACHE) g_sc_used++;
    else {
      slot = 0;
      for (unsigned i = 1; i < g_sc_used; i++) {
        if (g_sc_at[i] < g_sc_at[slot]) slot = i;
      }
    }
  }
  s_cpy(g_sc_fid[slot], fid, sizeof(g_sc_fid[slot]));
  s_cpy(g_sc_title[slot], title, sizeof(g_sc_title[slot]));
  s_cpy(g_sc_sub[slot], sub, sizeof(g_sc_sub[slot]));
  s_cpy(g_sc_avatar[slot], avatar ? avatar : "", sizeof(g_sc_avatar[slot]));
  g_sc_at[slot] = g_tick;
}

/* Tell the host whether the back arrow should appear (we are inside a
 * subfolder) or not (at the library root, where back leaves the wapp). */
static void lib_nav(void) {
  if (g_lib_path[0]) {
    char t[520];
    s_cpy(t, g_lib_path, sizeof(t));
    unsigned L = s_len(t);
    if (L && t[L - 1] == '/') t[L - 1] = 0;   /* drop the trailing slash for display */
    nav_set(1, t);
  } else {
    nav_set(0, "");
  }
}

/* The list is ONE navigable panel over the download-folder tree: the organizing
 * subfolders at the current level, then the torrents filed there. One section
 * (so the host draws no tabs); the "All / Mine" toolbar toggles whether we show
 * everything or only the folders this device created. */
static void render_list(void) {
  char arg[520];
  s_cpy(arg, g_lib_path, sizeof(arg));
  uint32_t n = hal_folder_library(arg, s_len(arg), g_json, sizeof(g_json) - 1);
  g_json[n] = 0;

  char slice[1200];
  row_open();

  char head[560];
  if (g_lib_path[0]) {
    s_cpy(head, g_lib_path, sizeof(head));
    unsigned L = s_len(head);
    if (L && head[L - 1] == '/') head[L - 1] = 0;
  } else {
    s_cpy(head, g_filter ? "My torrents" : "All torrents", sizeof(head));
  }
  section_open(head);

  /* No ".." row: the app-bar back arrow (nav_back) already goes up a folder, and
   * a second up-control on the same panel is clutter. */

  /* Subfolders: {"dirs":[{"name"}]} — stop at the array's ']' so the torrents
   * that follow are not read as directories. */
  const char *d = g_json;
  while (*d && !s_pre(d, "\"dirs\":[")) d++;
  if (*d) {
    const char *end = d;
    while (*end && *end != ']') end++;
    const char *p = next_obj(d, slice, sizeof(slice));
    while (p && p <= end) {
      char dn[200];
      jstr(slice, "name", dn, sizeof(dn));
      if (dn[0]) {
        char rid[240] = "dir:";
        s_cat(rid, dn, sizeof(rid));
        row_icon(rid, dn, "folder", "folder");
      }
      p = next_obj(p, slice, sizeof(slice));
    }
  }

  /* Torrents: {"torrents":[{"folderId","name","owned"}]} */
  const char *t = g_json;
  while (*t && !s_pre(t, "\"torrents\":[")) t++;
  if (*t) {
    const char *p = next_obj(t, slice, sizeof(slice));
    while (p) {
      char fid[80];
      jstr(slice, "folderId", fid, sizeof(fid));
      int owned = jbool_def(slice, "owned", 0);
      if (fid[0] && (!g_filter || owned)) {
        char title[160], sub[200], avatar[120], rid[90] = "t:";
        torrent_row(fid, owned, 0, title, sizeof(title), sub, sizeof(sub),
                    avatar, sizeof(avatar));
        s_cat(rid, fid, sizeof(rid));
        row_avatar(rid, title, sub, avatar);
      }
      p = next_obj(p, slice, sizeof(slice));
    }
  }

  row_close();
  changed_send(g_out, &g_list_hash);
}

/* ── the dashboard: what this device holds and gives ─────────────────────────
 *
 * Four numbers over the list, so opening the wapp answers the questions a
 * file-sharer actually has — what am I sharing, what am I seeding, how big is
 * the library, is anyone taking from it — before a single row is read.
 *
 * The walk (owned + subs, one hal_folder_stats each) costs a HAL round trip per
 * torrent, so it runs every ~30s and immediately after anything that changes
 * the numbers (pin, rescan, edit — stats_cache_clear sets g_dash_dirty). */
static void render_dash(void) {
  g_dash_at = g_tick;
  g_dash_dirty = 0;

  unsigned owned = 0, pinned = 0, following = 0;
  unsigned files = 0, serves = 0, serves7d = 0;
  unsigned bytes_mb = 0; /* sum in MB so >4GB libraries don't wrap 32 bits */

  char ids[48][80];
  unsigned nids = 0;
  char slice[1200];

  /* Owned folders. */
  uint32_t n = hal_folder_owned(g_json, sizeof(g_json) - 1);
  g_json[n] = 0;
  const char *p = next_obj(g_json, slice, sizeof(slice));
  while (p) {
    char fid[80];
    jstr(slice, "folderId", fid, sizeof(fid));
    if (fid[0] && nids < 48) {
      owned++;
      s_cpy(ids[nids++], fid, sizeof(ids[0]));
    }
    p = next_obj(p, slice, sizeof(slice));
  }

  /* Followed folders (a pin is a follow with autoSync). */
  n = hal_folder_subs(g_json, sizeof(g_json) - 1);
  g_json[n] = 0;
  p = next_obj(g_json, slice, sizeof(slice));
  while (p) {
    char fid[80];
    jstr(slice, "folderId", fid, sizeof(fid));
    if (fid[0]) {
      int dup = 0;
      for (unsigned i = 0; i < nids; i++) {
        if (s_eq(ids[i], fid)) { dup = 1; break; }
      }
      if (!dup) {
        if (jbool_def(slice, "autoSync", 0)) pinned++;
        else following++;
        if (nids < 48) s_cpy(ids[nids++], fid, sizeof(ids[0]));
      }
    }
    p = next_obj(p, slice, sizeof(slice));
  }

  /* One stats pass over everything this device touches. */
  for (unsigned i = 0; i < nids; i++) {
    char st[4096];
    uint32_t sn = hal_folder_stats(ids[i], s_len(ids[i]), st, sizeof(st) - 1);
    st[sn] = 0;
    files += (unsigned)jnum(st, "fileCount");
    bytes_mb += (unsigned)jnum(st, "totalBytes") / (1024u * 1024u);
    serves += (unsigned)jnum(st, "serves");
    serves7d += (unsigned)jnum(st, "last7d");
  }

  char m[2048] = "{\"type\":\"ui.stats.set\",\"field\":\"dash\",\"tiles\":[";
  char nb[16];

  s_cat(m, "{\"id\":\"mine\",\"label\":\"Sharing\",\"value\":\"", sizeof(m));
  u_itoa(owned, nb); s_cat(m, nb, sizeof(m));
  s_cat(m, "\",\"hint\":\"folders you publish\",\"tap\":true},", sizeof(m));

  s_cat(m, "{\"id\":\"seed\",\"label\":\"Seeding\",\"value\":\"", sizeof(m));
  u_itoa(pinned, nb); s_cat(m, nb, sizeof(m));
  s_cat(m, "\",\"hint\":\"", sizeof(m));
  if (following) {
    s_cat(m, "pinned copies · following ", sizeof(m));
    u_itoa(following, nb); s_cat(m, nb, sizeof(m));
    s_cat(m, " more", sizeof(m));
  } else {
    s_cat(m, "pinned full copies", sizeof(m));
  }
  s_cat(m, "\"},", sizeof(m));

  s_cat(m, "{\"id\":\"lib\",\"label\":\"Library\",\"value\":\"", sizeof(m));
  if (bytes_mb >= 1024u) {
    u_itoa(bytes_mb / 1024u, nb); s_cat(m, nb, sizeof(m));
    s_cat(m, "\",\"unit\":\"GB\"", sizeof(m));
  } else {
    u_itoa(bytes_mb, nb); s_cat(m, nb, sizeof(m));
    s_cat(m, "\",\"unit\":\"MB\"", sizeof(m));
  }
  s_cat(m, ",\"hint\":\"", sizeof(m));
  u_itoa(files, nb); s_cat(m, nb, sizeof(m));
  s_cat(m, files == 1 ? " file across " : " files across ", sizeof(m));
  u_itoa(nids, nb); s_cat(m, nb, sizeof(m));
  s_cat(m, nids == 1 ? " torrent" : " torrents", sizeof(m));
  s_cat(m, "\"},", sizeof(m));

  s_cat(m, "{\"id\":\"served\",\"label\":\"Served\",\"value\":\"", sizeof(m));
  u_itoa(serves, nb); s_cat(m, nb, sizeof(m));
  s_cat(m, "\",\"unit\":\"files\",\"hint\":\"", sizeof(m));
  u_itoa(serves7d, nb); s_cat(m, nb, sizeof(m));
  s_cat(m, " in the last 7 days — downloads others took from this device",
        sizeof(m));
  s_cat(m, "\"}]}", sizeof(m));

  changed_send(m, &g_dash_hash);
}

/* A human label for a fixed category id. */
static const char *cat_label(const char *c) {
  if (s_eq(c, "film")) return "Film";
  if (s_eq(c, "series")) return "Series / TV";
  if (s_eq(c, "anime")) return "Anime";
  if (s_eq(c, "documentary")) return "Documentary";
  if (s_eq(c, "music")) return "Music";
  if (s_eq(c, "audiobook")) return "Audiobook";
  if (s_eq(c, "book")) return "Book";
  if (s_eq(c, "comic")) return "Comic";
  if (s_eq(c, "manga")) return "Manga";
  if (s_eq(c, "magazine")) return "Magazine";
  if (s_eq(c, "game")) return "Game";
  if (s_eq(c, "software")) return "Software";
  if (s_eq(c, "course")) return "Course";
  if (s_eq(c, "podcast")) return "Podcast";
  if (s_eq(c, "photo")) return "Photos";
  if (s_eq(c, "dataset")) return "Dataset";
  if (s_eq(c, "other")) return "Other";
  return c;
}

/* The Search panel's results feed — the WHOLE network's, not this device's.
 *
 * hal_folder_search_global merges the local index with a mesh fan-out: every
 * published listing is already replicated to the indexer mesh as a signed
 * event, and a serve-node answers a full-text query against everything it
 * holds. The fan-out takes seconds, so the HAL answers from its snapshot and
 * says `busy` — the tick re-renders until the mesh has finished answering,
 * and rows fill in as they arrive.
 *
 * With no query and no category, it lists the non-empty categories (tap one
 * to browse); otherwise the matching torrents. Everything is host-side and
 * generic; the wapp only lays it out. */
static void render_search(void) {
  char q[240] = "{\"q\":\"";
  jesc(q, sizeof(q), g_srch_q);
  s_cat(q, "\",\"cat\":\"", sizeof(q));
  jesc(q, sizeof(q), g_srch_cat);
  s_cat(q, "\",\"sort\":\"", sizeof(q));
  jesc(q, sizeof(q), g_srch_sort);
  s_cat(q, "\"}", sizeof(q));

  uint32_t n = hal_folder_search_global(q, s_len(q), g_json, sizeof(g_json) - 1);
  g_json[n] = 0;
  g_srch_busy = jbool_def(g_json, "busy", 0);

  const int browsing = (!g_srch_q[0] && !g_srch_cat[0]);
  char slice[1200];

  row_open_field("results");

  if (browsing) {
    /* {"cats":[{"cat":"film","count":12}]} */
    section_open(g_srch_busy ? "Browse categories  ·  asking the network…"
                             : "Browse categories");
    const char *c = g_json;
    while (*c && !s_pre(c, "\"cats\":[")) c++;
    if (*c) {
      const char *end = c;
      while (*end && *end != ']') end++;
      const char *p = next_obj(c, slice, sizeof(slice));
      while (p && p <= end) {
        char cat[24];
        jstr(slice, "cat", cat, sizeof(cat));
        unsigned count = (unsigned)jnum(slice, "count");
        if (cat[0]) {
          char rid[40] = "cat:";
          s_cat(rid, cat, sizeof(rid));
          char sub[40];
          u_itoa(count, sub);
          s_cat(sub, count == 1 ? " torrent" : " torrents", sizeof(sub));
          row_icon(rid, cat_label(cat), sub, "category");
        }
        p = next_obj(p, slice, sizeof(slice));
      }
    }
  } else {
    char head[80];
    if (g_srch_cat[0]) {
      s_cpy(head, cat_label(g_srch_cat), sizeof(head));
    } else {
      s_cpy(head, "Results", sizeof(head));
    }
    if (g_srch_busy) {
      s_cat(head, "  ·  asking the network…", sizeof(head));
    } else {
      s_cat(head, "  ·  by ", sizeof(head));
      s_cat(head, g_srch_sort, sizeof(head));
    }
    section_open(head);

    /* Skip the "cats" array so its objects are not read as results, then walk
     * the "results" array. */
    const char *r = g_json;
    while (*r && !s_pre(r, "\"results\":[")) r++;
    if (*r) {
      const char *p = next_obj(r, slice, sizeof(slice));
      int any = 0;
      while (p) {
        char fid[80], title[160], cat[24], avatar[120], where[12];
        jstr(slice, "folderId", fid, sizeof(fid));
        jstr(slice, "title", title, sizeof(title));
        jstr(slice, "cat", cat, sizeof(cat));
        jstr(slice, "icon", avatar, sizeof(avatar));
        jstr(slice, "where", where, sizeof(where));
        unsigned seeders = (unsigned)jnum(slice, "seeders");
        unsigned size = (unsigned)jnum(slice, "size");
        if (fid[0]) {
          any = 1;
          char sub[140] = "";
          if (s_eq(where, "mesh")) {
            /* A listing only the network knows: seeders/size are unknown
             * until it is opened, so say where it came from instead of
             * quoting zeros that read as "dead torrent". */
            s_cat(sub, "on the network", sizeof(sub));
          } else {
            { char nb[12]; u_itoa(seeders, nb); s_cat(sub, nb, sizeof(sub)); }
            s_cat(sub, seeders == 1 ? " seeder" : " seeders", sizeof(sub));
            if (size) { char fs[24]; fmt_size(size, fs, sizeof(fs));
              s_cat(sub, "  ·  ", sizeof(sub)); s_cat(sub, fs, sizeof(sub)); }
          }
          if (cat[0]) { s_cat(sub, "  ·  ", sizeof(sub));
            s_cat(sub, cat_label(cat), sizeof(sub)); }
          char rid[90] = "t:";
          s_cat(rid, fid, sizeof(rid));
          row_avatar(rid, title[0] ? title : fid, sub, avatar);
        }
        p = next_obj(p, slice, sizeof(slice));
      }
      if (!any && !g_srch_busy) section_open("No torrents match");
    }
  }

  row_close();
  changed_send(g_out, &g_srch_hash);
}

/* Inside one torrent: this directory level only (the host keeps the payload —
 * and our work — proportional to one level, not to the whole folder). */
static void render_open(void) {
  char arg[600];
  s_cpy(arg, g_cur, sizeof(arg));
  s_cat(arg, "\t", sizeof(arg));
  s_cat(arg, g_cur_path, sizeof(arg));
  uint32_t n = hal_folder_browse(arg, s_len(arg), g_json, sizeof(g_json) - 1);
  g_json[n] = 0;

  char name[160];
  jstr(g_json, "name", name, sizeof(name));
  if (name[0]) s_cpy(g_cur_name, name, sizeof(g_cur_name));

  char title[300];
  s_cpy(title, g_cur_name[0] ? g_cur_name : "Torrent", sizeof(title));
  if (g_cur_path[0]) {
    s_cat(title, " / ", sizeof(title));
    s_cat(title, g_cur_path, sizeof(title));
  }

  /* Inside a torrent: the ONE back control (the AppBar arrow / the system-back
   * gesture) comes to us as `nav_back` and goes up a level. No ".." row — a
   * second back affordance on the same panel is clutter, and the user already
   * has one that works everywhere else in the app. */
  nav_set(1, g_cur_name[0] ? g_cur_name : "Torrent");

  row_open();
  section_open(title);

  char slice[1200];

  /* Subfolders: {"dirs":[{"name":..}]} — stop at the array's ']' so the files
   * that follow are not read as directories. */
  const char *d = g_json;
  while (*d && !s_pre(d, "\"dirs\":[")) d++;
  if (*d) {
    const char *end = d;
    while (*end && *end != ']') end++;
    const char *p = next_obj(d, slice, sizeof(slice));
    while (p && p <= end) {
      char dn[200];
      jstr(slice, "name", dn, sizeof(dn));
      if (dn[0]) {
        char rid[240] = "cd:";
        s_cat(rid, dn, sizeof(rid));
        row_icon(rid, dn, "folder", "folder");
      }
      p = next_obj(p, slice, sizeof(slice));
    }
  }

  /* Files: {"files":[{"x":sha,"base":leaf,"name":path,"size":n}]} */
  const char *f = g_json;
  while (*f && !s_pre(f, "\"files\":[")) f++;
  if (*f) {
    const char *p = next_obj(f, slice, sizeof(slice));
    while (p) {
      char sha[80], base[200], full[400];
      jstr(slice, "x", sha, sizeof(sha));
      jstr(slice, "base", base, sizeof(base));
      jstr(slice, "name", full, sizeof(full));
      unsigned size = (unsigned)jnum(slice, "size");
      if (sha[0]) {
        char sub[80];
        fmt_size(size, sub, sizeof(sub));
        char rid[520] = "f:";
        s_cat(rid, sha, sizeof(rid));
        s_cat(rid, "\t", sizeof(rid));
        s_cat(rid, full[0] ? full : base, sizeof(rid));
        row_icon(rid, base[0] ? base : sha, sub, icon_for(base));
      }
      p = next_obj(p, slice, sizeof(slice));
    }
  }

  row_close();
  changed_send(g_out, &g_list_hash);
}

static void render_current(void) {
  if (g_view == 1 && g_cur[0]) render_open();
  else render_list();
}

/* ── the swarm: who has this, and what are they made of ──────────────────── */
static void render_swarm(void) {
  log_clear("swarm_log");
  if (!g_cur[0]) {
    log_line("swarm_log", "Open a torrent first.");
    return;
  }
  uint32_t n = hal_folder_swarm(g_cur, s_len(g_cur), g_json, sizeof(g_json) - 1);
  g_json[n] = 0;

  int count = 0;
  char slice[1400];
  const char *p = next_obj(g_json, slice, sizeof(slice));
  while (p) {
    char dest[60], power[24], uplink[24], prov[16], region[24];
    jstr(slice, "dest", dest, sizeof(dest));
    jstr(slice, "power", power, sizeof(power));
    jstr(slice, "uplink", uplink, sizeof(uplink));
    jstr(slice, "provenance", prov, sizeof(prov));
    jstr(slice, "region", region, sizeof(region));
    unsigned heard = (unsigned)jnum(slice, "lastHeardMs");
    unsigned hops = (unsigned)jnum(slice, "hops");

    if (dest[0]) {
      char line[400] = "";
      char shortd[20];
      s_cpy(shortd, dest, sizeof(shortd));
      if (s_len(shortd) > 8) shortd[8] = 0;
      s_cat(line, shortd, sizeof(line));
      s_cat(line, "  ", sizeof(line));
      s_cat(line, power[0] ? power : "power?", sizeof(line));
      s_cat(line, "/", sizeof(line));
      s_cat(line, uplink[0] ? uplink : "uplink?", sizeof(line));
      if (hops) {
        char nb[12]; u_itoa(hops, nb);
        s_cat(line, "  ", sizeof(line)); s_cat(line, nb, sizeof(line));
        s_cat(line, hops == 1 ? " hop" : " hops", sizeof(line));
      }
      if (heard) {
        char ab[16]; fmt_age(heard, ab, sizeof(ab));
        s_cat(line, "  heard ", sizeof(line)); s_cat(line, ab, sizeof(line));
        s_cat(line, " ago", sizeof(line));
      }
      /* Provenance is not decoration: after Indexer-to-Indexer sync the
       * freshness we are quoting is second-hand, and the age of the information
       * is not the age of the device. */
      s_cat(line, s_eq(prov, "direct") ? " (heard directly)"
                                       : " (an Indexer told us)", sizeof(line));
      if (region[0]) {
        s_cat(line, "  region ", sizeof(line)); s_cat(line, region, sizeof(line));
      }
      log_line("swarm_log", line);
      count++;
    }
    p = next_obj(p, slice, sizeof(slice));
  }

  if (count == 0) {
    log_line("swarm_log", "No holders known yet.");
    log_line("swarm_log",
             "The DHT resolve runs in the background - reopen this in a moment.");
    log_line("swarm_log",
             "If it stays empty, nobody reachable is holding this folder: pin it "
             "and you become its first other copy.");
  } else {
    char l[120] = "";
    char nb[12]; u_itoa((unsigned)count, nb);
    s_cat(l, nb, sizeof(l));
    s_cat(l, count == 1 ? " holder. " : " holders. ", sizeof(l));
    s_cat(l, "Best first: mains + a fat uplink beats a phone on cellular.",
          sizeof(l));
    log_line("swarm_log", l);
  }
}

/* ── the listing: data/meta.json ─────────────────────────────────────────────
 *
 * What a torrent SAYS IT IS — a title, one category, a description, tags and its
 * artwork. It lives as ordinary files inside the shared folder (data/meta.json +
 * data/cover.jpg …), so it travels with the content and can be written by hand.
 * The signed op-log mirrors the fields, which is why the panel below can be
 * filled in for a torrent nobody on this device has downloaded.
 */

/* Which media slot a picker result is for (0 = none in flight). */
static char g_pick_slot[16] = "";

static void render_listing(void) {
  if (!g_cur[0]) return;

  /* One hero card. hal_folder_media returns the whole listing — banner, poster,
   * title, category, tags, description, screenshots — from the SIGNED op-log, so
   * it is there even for a torrent we have not downloaded. The wapp never touches
   * the bytes; it hands the JSON to the host's gallery field and the host draws
   * it. (That HAL boundary is why the wapp can be updated on its own, without a
   * new engine.) */
  /* "folderId\tpath" — the host returns the hero AND the file list at this
   * directory level, so the compact browser under the hero shares the folder
   * path with the full-screen browser (going full-screen keeps your place). */
  char arg[600];
  s_cpy(arg, g_cur, sizeof(arg));
  s_cat(arg, "\t", sizeof(arg));
  s_cat(arg, g_cur_path, sizeof(arg));
  uint32_t n = hal_folder_media(arg, s_len(arg), g_json, sizeof(g_json) - 1);
  g_json[n] = 0;
  char m[16384] =
      "{\"type\":\"ui.field.set\",\"field\":\"listing_media\",\"value\":\"";
  jesc(m, sizeof(m), g_json);
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* Load data/meta.json into the Edit screen and open it. */
static void open_listing_edit(void) {
  if (!g_cur[0]) { notify("info", "Open a torrent first"); return; }

  char st[4096];
  uint32_t n = hal_folder_stats(g_cur, s_len(g_cur), st, sizeof(st) - 1);
  st[n] = 0;
  if (!jbool_def(st, "owned", 0)) {
    notify("info", "Only the publisher can edit this listing");
    return;
  }

  n = hal_folder_meta_get(g_cur, s_len(g_cur), g_json, sizeof(g_json) - 1);
  g_json[n] = 0;

  char title[80], desc[260], cat[32];
  jstr(g_json, "title", title, sizeof(title));
  jstr(g_json, "desc", desc, sizeof(desc));
  jstr(g_json, "cat", cat, sizeof(cat));
  field_set("m_title", title);
  field_set("m_desc", desc);
  field_set("m_cat", cat[0] ? cat : "other");

  /* tags arrive as a JSON array; the field is a comma-separated line. */
  {
    char flat[200] = "";
    const char *p = g_json;
    while (*p && !s_pre(p, "\"tags\":[")) p++;
    if (*p) {
      p += 8;
      int first = 1;
      while (*p && *p != ']') {
        if (*p == '"') {
          char t[32]; unsigned i = 0;
          p++;
          while (*p && *p != '"' && i < sizeof(t) - 1) t[i++] = *p++;
          t[i] = 0;
          if (t[0]) {
            if (!first) s_cat(flat, ", ", sizeof(flat));
            s_cat(flat, t, sizeof(flat));
            first = 0;
          }
        }
        if (*p) p++;
      }
    }
    field_set("m_tags", flat);
  }
  { const char *m = jbool_def(g_json, "adult", 0)
        ? "{\"type\":\"ui.field.set\",\"field\":\"m_adult\",\"value\":true}"
        : "{\"type\":\"ui.field.set\",\"field\":\"m_adult\",\"value\":false}";
    hal_msg_send(m, s_len(m)); }

  const char *m = "{\"type\":\"ui.screen.open\",\"name\":\"Edit listing\"}";
  hal_msg_send(m, s_len(m));
}

/* Save the Edit screen back into data/meta.json (the host clamps every limit and
 * republishes the signed listing). */
static void save_listing(const char *buf) {
  if (!g_cur[0]) return;
  char title[120], desc[400], cat[32], tags[240];
  jstr(buf, "m_title", title, sizeof(title));
  jstr(buf, "m_desc", desc, sizeof(desc));
  jstr(buf, "m_cat", cat, sizeof(cat));
  jstr(buf, "m_tags", tags, sizeof(tags));
  int adult = jbool_def(buf, "m_adult", 0);

  /* Keep the artwork the listing already names — this screen edits words only. */
  uint32_t n = hal_folder_meta_get(g_cur, s_len(g_cur), g_json, sizeof(g_json) - 1);
  g_json[n] = 0;
  char cover[64], banner[64], trailer[64];
  jstr(g_json, "cover", cover, sizeof(cover));
  jstr(g_json, "banner", banner, sizeof(banner));
  jstr(g_json, "trailer", trailer, sizeof(trailer));

  char j[2048] = "{\"title\":\"";
  jesc(j, sizeof(j), title);
  s_cat(j, "\",\"desc\":\"", sizeof(j));
  jesc(j, sizeof(j), desc);
  s_cat(j, "\",\"cat\":\"", sizeof(j));
  jesc(j, sizeof(j), cat[0] ? cat : "other");
  s_cat(j, "\",\"adult\":", sizeof(j));
  s_cat(j, adult ? "true" : "false", sizeof(j));
  s_cat(j, ",\"tags\":[", sizeof(j));
  {
    /* split the comma/space separated line into a JSON array */
    int first = 1;
    const char *p = tags;
    while (*p) {
      while (*p == ' ' || *p == ',') p++;
      if (!*p) break;
      char t[32]; unsigned i = 0;
      while (*p && *p != ',' && i < sizeof(t) - 1) t[i++] = *p++;
      while (i > 0 && t[i - 1] == ' ') i--;   /* trim */
      t[i] = 0;
      if (t[0]) {
        if (!first) s_cat(j, ",", sizeof(j));
        s_cat(j, "\"", sizeof(j));
        jesc(j, sizeof(j), t);
        s_cat(j, "\"", sizeof(j));
        first = 0;
      }
    }
  }
  s_cat(j, "]", sizeof(j));
  if (cover[0])   { s_cat(j, ",\"cover\":\"", sizeof(j));   jesc(j, sizeof(j), cover);   s_cat(j, "\"", sizeof(j)); }
  if (banner[0])  { s_cat(j, ",\"banner\":\"", sizeof(j));  jesc(j, sizeof(j), banner);  s_cat(j, "\"", sizeof(j)); }
  if (trailer[0]) { s_cat(j, ",\"trailer\":\"", sizeof(j)); jesc(j, sizeof(j), trailer); s_cat(j, "\"", sizeof(j)); }
  /* Carry the gallery through UNTOUCHED. The host writes exactly the JSON it is
   * given, so omitting it here would quietly delete every screenshot the moment
   * somebody fixed a typo in the title. */
  {
    char gal[600];
    copy_array(g_json, "gallery", gal, sizeof(gal));
    if (!s_eq(gal, "[]")) {
      s_cat(j, ",\"gallery\":", sizeof(j));
      s_cat(j, gal, sizeof(j));
    }
  }
  s_cat(j, "}", sizeof(j));

  hal_folder_meta_set(g_cur, s_len(g_cur), j, s_len(j));
  stats_cache_clear();
  notify("info", "Listing published");
  const char *m = "{\"type\":\"ui.screen.close\"}";
  hal_msg_send(m, s_len(m));
}

/* ── info + settings panels ──────────────────────────────────────────────── */
static void render_info(void) {
  log_clear("info_log");
  if (!g_cur[0]) {
    log_line("info_log", "Open a torrent first.");
    return;
  }
  char link[400];
  uint32_t n = hal_folder_link(g_cur, s_len(g_cur), link, sizeof(link) - 1);
  link[n] = 0;

  char st[4096];
  n = hal_folder_stats(g_cur, s_len(g_cur), st, sizeof(st) - 1);
  st[n] = 0;

  char name[160];
  jstr(st, "name", name, sizeof(name));
  if (name[0]) log_line("info_log", name);
  if (link[0]) log_line("info_log", link);

  char l[200] = "";
  { char nb[12]; u_itoa((unsigned)jnum(st, "fileCount"), nb);
    s_cat(l, nb, sizeof(l)); s_cat(l, " files, ", sizeof(l)); }
  { char fs[24]; fmt_size((unsigned)jnum(st, "totalBytes"), fs, sizeof(fs));
    s_cat(l, fs, sizeof(l)); }
  log_line("info_log", l);

  l[0] = 0;
  { char nb[12]; u_itoa((unsigned)jnum(st, "serves"), nb);
    s_cat(l, "served ", sizeof(l)); s_cat(l, nb, sizeof(l));
    s_cat(l, " times from this device", sizeof(l)); }
  log_line("info_log", l);
  log_line("info_log",
           jbool_def(st, "owned", 0)
               ? "You hold this folder's key: only you can change what's in it."
               : "Someone else holds this folder's key. You can read, host and "
                 "re-share it - you cannot change it.");
}

static void render_settings(void) {
  /* Current download folder (real files on disk; downloads land here and the
   * subfolders under it organize the list). */
  char root[520];
  uint32_t rn = hal_folder_download_root(root, sizeof(root) - 1);
  root[rn] = 0;
  field_set("dl_root", root[0] ? root : "(not set)");

  { const char *m = g_share_author
        ? "{\"type\":\"ui.field.set\",\"field\":\"share_author\",\"value\":true}"
        : "{\"type\":\"ui.field.set\",\"field\":\"share_author\",\"value\":false}";
    hal_msg_send(m, s_len(m)); }

  log_clear("settings_log");
  uint32_t n = hal_folder_subs(g_json, sizeof(g_json) - 1);
  g_json[n] = 0;
  int pinned = 0, subs = 0;
  char slice[1024];
  const char *p = next_obj(g_json, slice, sizeof(slice));
  while (p) {
    subs++;
    if (jbool_def(slice, "autoSync", 0)) pinned++;
    p = next_obj(p, slice, sizeof(slice));
  }
  char l[200] = "";
  char nb[12];
  u_itoa((unsigned)pinned, nb);
  s_cat(l, "Pinned: ", sizeof(l)); s_cat(l, nb, sizeof(l));
  u_itoa((unsigned)subs, nb);
  s_cat(l, " of ", sizeof(l)); s_cat(l, nb, sizeof(l));
  s_cat(l, " followed torrents", sizeof(l));
  log_line("settings_log", l);
  log_line("settings_log",
           "A pinned torrent is held in full and announced to the Indexers, so "
           "the swarm stops waking the publisher's phone.");
  log_line("settings_log",
           "Seeding continues while the app is in the background.");
}

/* ── manage menu ─────────────────────────────────────────────────────────── */
static void prompt_manage(void) {
  int pinned = 0;
  {
    /* Ask the host, not our own memory: the pin may have been set elsewhere. */
    uint32_t n = hal_folder_subs(g_json, sizeof(g_json) - 1);
    g_json[n] = 0;
    char slice[1024];
    const char *p = next_obj(g_json, slice, sizeof(slice));
    while (p) {
      char fid[80];
      jstr(slice, "folderId", fid, sizeof(fid));
      if (s_eq(fid, g_cur) && jbool_def(slice, "autoSync", 0)) pinned = 1;
      p = next_obj(p, slice, sizeof(slice));
    }
  }
  char m[900] = "{\"type\":\"ui.prompt\",\"id\":\"mng:";
  jesc(m, sizeof(m), g_cur);
  s_cat(m, "\",\"title\":\"Manage torrent\",\"body\":\"", sizeof(m));
  s_cat(m, pinned ? "Pinned: this device keeps a full copy and seeds it."
                  : "Not pinned: this device is not holding a copy for others.",
        sizeof(m));
  s_cat(m, "\",\"chips\":["
           "{\"label\":\"Download all\",\"value\":\"dlall\"},", sizeof(m));
  s_cat(m, pinned ? "{\"label\":\"Unpin\",\"value\":\"unpin\"},"
                  : "{\"label\":\"Pin (keep + seed)\",\"value\":\"pin\"},",
        sizeof(m));
  s_cat(m, "{\"label\":\"Open folder\",\"value\":\"opendir\"},"
           "{\"label\":\"Copy link\",\"value\":\"link\"},"
           "{\"label\":\"Rescan\",\"value\":\"rescan\"},"
           "{\"label\":\"Remove\",\"value\":\"remove\"}],"
           "\"confirm\":\"Cancel\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

static void do_copy_link(void) {
  if (!g_cur[0]) { notify("info", "Open a torrent first"); return; }
  /* Include the author npub only when the user opted in (off by default — a
   * shared link should not name a person unless they choose to). */
  char arg[90];
  s_cpy(arg, g_cur, sizeof(arg));
  if (g_share_author) s_cat(arg, "\t1", sizeof(arg));
  char link[400];
  uint32_t n = hal_folder_link(arg, s_len(arg), link, sizeof(link) - 1);
  link[n] = 0;
  if (!link[0]) { notify("warning", "No link yet"); return; }
  char body[600] = "Anyone with this link can read, download and re-host this "
                   "torrent - not change it:\n";
  s_cat(body, link, sizeof(body));
  prompt_copy(g_cur_name[0] ? g_cur_name : "Torrent link", body, link);
}

/* Popularity panel: pull the device-local monthly seeders/leechers for this
 * torrent and open the chart screen. The count lives on this device only (never
 * in the folder) — it grows as the torrent is shared. */
static void show_popularity(void) {
  if (!g_cur[0]) { notify("info", "Open a torrent first"); return; }
  uint32_t n = hal_folder_popularity(g_cur, s_len(g_cur), g_json, sizeof(g_json) - 1);
  g_json[n] = 0;
  char m[4096] = "{\"type\":\"ui.field.set\",\"field\":\"pop_chart\",\"value\":\"";
  jesc(m, sizeof(m), g_json);
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
  const char *o = "{\"type\":\"ui.screen.open\",\"name\":\"Popularity\"}";
  hal_msg_send(o, s_len(o));
}

/* A torrent folder is dynamic: the publisher can push changes. Let the user
 * follow those updates or freeze a static copy (e.g. to save bandwidth). The
 * publisher has nothing to freeze — they are the source of the updates. */
static void prompt_updates(void) {
  if (!g_cur[0]) { notify("info", "Open a torrent first"); return; }
  char st[4096];
  uint32_t sn = hal_folder_stats(g_cur, s_len(g_cur), st, sizeof(st) - 1);
  st[sn] = 0;
  if (jbool_def(st, "owned", 0)) {
    notify("info", "You publish this torrent - updates are yours to make");
    return;
  }
  int on = hal_folder_updates(g_cur, s_len(g_cur));
  char m[700] = "{\"type\":\"ui.prompt\",\"id\":\"upd:";
  jesc(m, sizeof(m), g_cur);
  s_cat(m, "\",\"title\":\"Folder updates\",\"body\":\"", sizeof(m));
  s_cat(m, on ? "Following updates: new versions the publisher pushes are "
                "downloaded automatically."
              : "Frozen: you keep the version you hold and do not download the "
                "publisher's changes.",
        sizeof(m));
  s_cat(m, "\",\"chips\":["
           "{\"label\":\"Follow updates\",\"value\":\"on\"},"
           "{\"label\":\"Freeze (static copy)\",\"value\":\"off\"}],"
           "\"confirm\":\"Cancel\"}",
        sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* Open a torrent by any address a user might paste. */
static void open_by_id(const char *idOrLink) {
  char st[4096];
  uint32_t n = hal_folder_stats(idOrLink, s_len(idOrLink), st, sizeof(st) - 1);
  st[n] = 0;
  char fid[80];
  jstr(st, "folderId", fid, sizeof(fid));
  if (!fid[0]) {
    notify("warning", "That is not a folder link");
    return;
  }
  s_cpy(g_cur, fid, sizeof(g_cur));
  g_cur_name[0] = 0;
  g_cur_path[0] = 0;
  g_view = 1;
  /* Pull the signed op-log so the listing appears; the browse triggers it. */
  render_open();
  if (g_pin_on_open) {
    hal_folder_pin(fid, s_len(fid), 1);
    notify("info", "Pinned: downloading and seeding this torrent");
  } else {
    notify("info", "Opening torrent...");
  }
}

/* Open a known torrent to its Listing (Info) screen — the hero card plus a
 * compact file browser. The torrent LIST stays underneath (g_view=0), so backing
 * out of the Info screen returns to the list. Shared by the list tap and the
 * search-result tap. */
static void open_torrent(const char *fid) {
  s_cpy(g_cur, fid, sizeof(g_cur));
  s_cpy(g_sel, g_cur, sizeof(g_sel));
  g_cur_path[0] = 0;
  g_view = 0;
  /* The name shown in the Info screen's app bar: the listing's TITLE first, else
   * the folder's own name. */
  { char st[4096]; uint32_t sn = hal_folder_stats(g_cur, s_len(g_cur), st, sizeof(st) - 1);
    st[sn] = 0; char nm[160]; jstr(st, "title", nm, sizeof(nm));
    if (!nm[0]) jstr(st, "name", nm, sizeof(nm));
    s_cpy(g_cur_name, nm, sizeof(g_cur_name)); }
  render_list();
  nav_set(0, "");
  render_swarm();
  render_info();
  render_listing();
  /* App-bar title = the torrent's name (not the static "Listing"); the hero card
   * no longer repeats it as text, so the name is said once. */
  char m[240] = "{\"type\":\"ui.screen.open\",\"name\":\"Listing\",\"title\":\"";
  jesc(m, sizeof(m), g_cur_name[0] ? g_cur_name : "Torrent");
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* Offer to open or download one file: a prompt showing the hash every byte is
 * checked against. Shared by the full browser and the compact listing browser. */
static void offer_file(const char *sha, const char *name) {
  if (!sha[0]) return;
  char m[900] = "{\"type\":\"ui.prompt\",\"id\":\"file:";
  jesc(m, sizeof(m), sha);
  s_cat(m, "\\t", sizeof(m));
  jesc(m, sizeof(m), name);
  s_cat(m, "\",\"title\":\"", sizeof(m));
  jesc(m, sizeof(m), name[0] ? name : "File");
  s_cat(m, "\",\"body\":\"Opens with whatever this device uses for that "
           "type. Every byte is checked against this hash before it is "
           "kept:\\n", sizeof(m));
  jesc(m, sizeof(m), sha);
  s_cat(m, "\",\"copy\":\"", sizeof(m));
  jesc(m, sizeof(m), sha);
  s_cat(m, "\",\"chips\":[{\"label\":\"Open\",\"value\":\"open\"},"
           "{\"label\":\"Download\",\"value\":\"dl\"}],"
           "\"confirm\":\"Close\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* "sha\tname" → offer_file. Used by both browsers' file taps. */
static void offer_file_id(const char *id) {
  char sha[80] = "", name[400] = "";
  unsigned i = 0;
  while (*id && *id != '\t' && i < sizeof(sha) - 1) sha[i++] = *id++;
  sha[i] = 0;
  if (*id == '\t') id++;
  i = 0;
  while (*id && i < sizeof(name) - 1) name[i++] = *id++;
  name[i] = 0;
  offer_file(sha, name);
}

/* ── lifecycle ───────────────────────────────────────────────────────────── */
__attribute__((export_name("module_init")))
void module_init(void) {
  settings_load();
  /* Only render when somebody is looking. As a background service this engine's
   * ui.* messages are read by nobody, and building them still costs main-isolate
   * time — which is exactly the 300ms stall this check removed. Seeding, which
   * is the reason this wapp runs in the background at all, is host-side and
   * needs no render. */
  if (hal_ui_attached()) {
    render_dash();
    render_list();
    render_search();
    render_settings();
  }
  hal_log(1, "torrents: ready", 15);
}

__attribute__((export_name("module_tick")))
void module_tick(void) {
  g_tick++;

  /* Diffed and cached, but a render still costs a HAL round trip per torrent
   * (stats → the host reduces the signed op-log). Every 6s is plenty for a page
   * somebody has open, and it is skipped entirely when nobody does.
   * (docs/performance.md: "what does this cost per hour with the screen off,
   * and who is awake to see the result?") */
  if (g_tick % 6 == 0 && hal_ui_attached()) render_current();

  /* The dashboard walks every torrent, so it refreshes on its own slower
   * cadence — and promptly after anything that moved the numbers. */
  if (hal_ui_attached() &&
      (g_dash_dirty || g_tick - g_dash_at >= 30)) render_dash();

  /* A mesh search is filling in: poll the snapshot until the fan-out lands.
   * Diffed, so the quiet polls cost one HAL read and no UI rebuild. */
  if (g_srch_busy && g_tick % 2 == 0 && hal_ui_attached()) render_search();

  /* Republish what changed on disk, so a torrent shared from a directory tracks
   * the directory. The host diffs it against the signed op-log and only writes
   * an op when something actually moved. */
  if (g_rescan_min > 0 &&
      g_tick % (unsigned)(g_rescan_min * 60) == 0) {
    hal_folder_rescan("", 0);   /* all owned disk folders */
    stats_cache_clear();        /* the sizes may genuinely have moved */
  }
}

__attribute__((export_name("module_handle_event")))
void module_handle_event(void) {
  if (hal_msg_available() == 0) return;
  char buf[4096];
  uint32_t n = hal_msg_recv(buf, sizeof(buf) - 1);
  if (n == 0) return;
  buf[n] = 0;

  char cmd[40] = "", typ[24] = "";
  jstr(buf, "command", cmd, sizeof(cmd));
  jstr(buf, "type", typ, sizeof(typ));
  /* A popup-menu item (the Add "+" menu) arrives as {type:"action",action:name}
   * rather than a {command:name}; fold its name into cmd so the action handlers
   * below fire for it too. */
  if (!cmd[0] && s_eq(typ, "action")) jstr(buf, "action", cmd, sizeof(cmd));

  /* The QR scanner came back with a decoded string — open it like a pasted link.
   * On desktop the host has no camera and returns an error instead. */
  if (s_eq(typ, "qr.scanned")) {
    char text[500] = "";
    jstr(buf, "text", text, sizeof(text));
    if (text[0]) {
      open_by_id(text);
    } else {
      char err[24] = "";
      jstr(buf, "error", err, sizeof(err));
      if (s_eq(err, "nocamera"))
        notify("info", "Scanning needs a phone camera");
    }
    return;
  }

  /* The picker came back: a directory becomes a torrent — unless we asked for a
   * piece of artwork, in which case it is a FILE for a listing slot. */
  if (s_eq(typ, "fs.picked")) {
    char path[400] = "";
    jstr(buf, "path", path, sizeof(path));
    if (!path[0]) return;

    if (g_pick_root) {
      g_pick_root = 0;
      if (!jbool_def(buf, "dir", 0)) {
        notify("info", "Pick a folder, not a file");
        return;
      }
      hal_folder_set_download_root(path, s_len(path));
      stats_cache_clear();
      notify("info", "Download folder set - adopting any torrents inside it");
      render_settings();
      render_list();
      return;
    }

    if (g_pick_slot[0]) {
      char slot[16];
      s_cpy(slot, g_pick_slot, sizeof(slot));
      g_pick_slot[0] = 0;
      if (jbool_def(buf, "dir", 0)) {
        notify("info", "Pick a file, not a folder");
        return;
      }
      char a[440] = "";
      s_cat(a, slot, sizeof(a));
      s_cat(a, "\t", sizeof(a));
      s_cat(a, path, sizeof(a));
      hal_folder_set_media(g_cur, s_len(g_cur), a, s_len(a));
      stats_cache_clear();
      notify("info", "Added to the listing");
      return;
    }

    if (!jbool_def(buf, "dir", 0)) {
      notify("info", "Pick a folder: a torrent is a folder, not a single file");
      return;
    }
    if (hal_folder_add_disk(path, s_len(path))) {
      g_view = 0;
      g_cur[0] = 0;
      stats_cache_clear();
      notify("info", "Creating the torrent - hashing the folder...");
    } else {
      notify("warning", "Reticulum is still starting - try again in a moment");
    }
    return;
  }

  /* A ui.prompt result comes back as the command "prompt" with the fields
   * prompt_id / prompt_value / prompt_input (the host flattens them into the
   * message; jstr finds them wherever they sit). */
  if (s_eq(cmd, "prompt")) {
    char id[120] = "", val[400] = "", input[400] = "";
    jstr(buf, "prompt_id", id, sizeof(id));
    jstr(buf, "prompt_value", val, sizeof(val));
    jstr(buf, "prompt_input", input, sizeof(input));

    if (s_eq(id, "open")) {
      if (input[0]) open_by_id(input);
      return;
    }
    if (s_eq(id, "newfolder")) {
      if (input[0]) {
        char rel[560] = "";
        s_cpy(rel, g_lib_path, sizeof(rel));   /* create under the current level */
        s_cat(rel, input, sizeof(rel));
        hal_folder_mkdir(rel, s_len(rel));
        notify("info", "Folder created");
        render_list();
      }
      return;
    }
    if (s_eq(id, "movefolder")) {
      if (!g_cur[0]) return;
      char mv[620] = "";
      s_cpy(mv, g_cur, sizeof(mv));
      s_cat(mv, "\t", sizeof(mv));
      s_cat(mv, input, sizeof(mv));       /* input="" = top level */
      hal_folder_move(mv, s_len(mv));
      stats_cache_clear();
      notify("info", "Moved");
      /* leave the listing and show the library where it now lives */
      g_view = 0;
      s_cpy(g_lib_path, input, sizeof(g_lib_path));
      if (g_lib_path[0]) s_cat(g_lib_path, "/", sizeof(g_lib_path));
      lib_nav();
      render_list();
      const char *m = "{\"type\":\"ui.screen.close\"}";
      hal_msg_send(m, s_len(m));
      return;
    }
    if (s_pre(id, "mng:")) {
      const char *fid = id + 4;
      if (s_eq(val, "dlall")) {
        const char *j = "{\"all\":true}";
        hal_folder_download(fid, s_len(fid), j, s_len(j));
        notify("info", "Downloading every file in this torrent");
      } else if (s_eq(val, "pin")) {
        hal_folder_pin(fid, s_len(fid), 1);
        stats_cache_clear();
        notify("info", "Pinned: keeping a full copy and telling the Indexers");
      } else if (s_eq(val, "unpin")) {
        hal_folder_pin(fid, s_len(fid), 0);
        stats_cache_clear();
        notify("info", "Unpinned: no longer keeping this in sync");
      } else if (s_eq(val, "opendir")) {
        if (!hal_folder_opendir(fid, s_len(fid)))
          notify("info", "No local copy on disk yet - pin or download it first");
      } else if (s_eq(val, "link")) {
        do_copy_link();
      } else if (s_eq(val, "rescan")) {
        hal_folder_rescan(fid, s_len(fid));
        stats_cache_clear();
        notify("info", "Rescanning the folder on disk");
      } else if (s_eq(val, "remove")) {
        hal_folder_remove(fid, s_len(fid));
        stats_cache_clear();
        g_view = 0; g_cur[0] = 0; g_cur_path[0] = 0;
        notify("info", "Removed. The files on disk were not touched.");
        render_list();
      }
      return;
    }
    if (s_pre(id, "upd:")) {
      const char *fid = id + 4;
      if (s_eq(val, "on")) {
        hal_folder_set_updates(fid, s_len(fid), 1);
        notify("info", "Following updates for this torrent");
      } else if (s_eq(val, "off")) {
        hal_folder_set_updates(fid, s_len(fid), 0);
        notify("info", "Frozen: keeping a static copy, no updates");
      }
      return;
    }
    if (s_pre(id, "file:")) {
      /* "file:<sha>\t<name>" — the one file the user tapped. */
      const char *r = id + 5;
      char sha[80] = "", name[300] = "";
      unsigned i = 0;
      while (*r && *r != '\t' && i < sizeof(sha) - 1) sha[i++] = *r++;
      sha[i] = 0;
      if (*r == '\t') r++;
      i = 0;
      while (*r && i < sizeof(name) - 1) name[i++] = *r++;
      name[i] = 0;
      if (!sha[0] || !g_cur[0]) return;
      if (s_eq(val, "open")) {
        /* "sha\tname": the name carries the extension, which is what the OS
         * routes on. The host opens a disk-backed file in place and exports a
         * downloaded one off the UI isolate first. */
        char a[500] = "";
        s_cat(a, sha, sizeof(a));
        s_cat(a, "\t", sizeof(a));
        s_cat(a, name[0] ? name : sha, sizeof(a));
        hal_folder_open_file(g_cur, s_len(g_cur), a, s_len(a));
        notify("info", "Opening...");
      } else if (s_eq(val, "dl")) {
        char j[420] = "{\"sha\":\"";
        jesc(j, sizeof(j), sha);
        s_cat(j, "\",\"name\":\"", sizeof(j));
        jesc(j, sizeof(j), name[0] ? name : sha);
        s_cat(j, "\"}", sizeof(j));
        hal_folder_download(g_cur, s_len(g_cur), j, s_len(j));
        notify("info", "Downloading...");
      }
      return;
    }
    return;
  }

  /* ── actions ── */
  if (s_eq(cmd, "t_add_disk")) {
    const char *m = "{\"type\":\"fs.pick\",\"mode\":\"dir\","
                    "\"title\":\"Pick a folder to share as a torrent\"}";
    hal_msg_send(m, s_len(m));
  } else if (s_eq(cmd, "t_open_link")) {
    prompt_input("open", "Open a torrent", "ntorrent1... / npub / hex id", 400);
  } else if (s_eq(cmd, "t_scan_qr")) {
    /* Ask the host to open the camera scanner; the decoded text comes back as a
     * qr.scanned message (Android only; desktop replies with no camera). */
    const char *m = "{\"type\":\"qr.scan\"}";
    hal_msg_send(m, s_len(m));
  } else if (s_eq(cmd, "t_back") || s_eq(cmd, "nav_back")) {
    /* One back control, one sensible chain. Inside a torrent's file browser:
     * subfolder -> parent -> back to the library list. In the library list:
     * subfolder -> parent -> (nav cleared) out of the wapp. */
    if (g_view == 1) {
      if (g_cur_path[0]) {
        unsigned L = s_len(g_cur_path);
        g_cur_path[L - 1] = 0;
        int k = (int)s_len(g_cur_path) - 1;
        while (k >= 0 && g_cur_path[k] != '/') k--;
        g_cur_path[k + 1] = 0;
        render_open();
      } else {
        g_view = 0; g_cur[0] = 0; g_cur_name[0] = 0;
        lib_nav();
        render_list();
      }
    } else if (g_lib_path[0]) {
      unsigned L = s_len(g_lib_path);
      g_lib_path[L - 1] = 0;
      int k = (int)s_len(g_lib_path) - 1;
      while (k >= 0 && g_lib_path[k] != '/') k--;
      g_lib_path[k + 1] = 0;
      lib_nav();
      render_list();
    } else {
      nav_set(0, "");     /* at the root: the next back leaves the wapp */
      render_list();
    }
  } else if (s_eq(cmd, "flt_all")) {
    g_filter = 0;
    render_list();
  } else if (s_eq(cmd, "flt_mine")) {
    g_filter = 1;
    render_list();
  } else if (s_eq(cmd, "dash_tap")) {
    /* The Sharing tile filters the list to the folders this device publishes —
     * the number and the list it counts should be one tap apart. */
    char id[24] = "";
    jstr(buf, "dash_id", id, sizeof(id));
    if (s_eq(id, "mine")) {
      g_filter = 1;
      render_list();
    }
  } else if (s_eq(cmd, "lib_newfolder")) {
    prompt_input("newfolder", "New folder", "folder name", 60);
  } else if (s_eq(cmd, "listing_move")) {
    if (!g_cur[0]) { notify("info", "Open a torrent first"); return; }
    prompt_input("movefolder", "Move to folder",
                 "folder name (blank = top level)", 200);
  } else if (s_eq(cmd, "dl_folder")) {
    g_pick_root = 1;
    const char *m = "{\"type\":\"fs.pick\",\"mode\":\"dir\","
                    "\"title\":\"Choose the download folder\"}";
    hal_msg_send(m, s_len(m));
  } else if (s_eq(cmd, "t_manage")) {
    if (g_cur[0]) prompt_manage();
    else notify("info", "Open a torrent first");
  } else if (s_eq(cmd, "swarm_refresh")) {
    render_swarm();
  } else if (s_eq(cmd, "listing_edit")) {
    open_listing_edit();
  } else if (s_eq(cmd, "listing_media_cd")) {
    /* enter a subfolder in the compact browser (shares g_cur_path with the
     * full-screen browser, so going full-screen keeps your place) */
    char sel[300] = "";
    jstr(buf, "listing_media_sel", sel, sizeof(sel));
    if (sel[0]) {
      s_cat(g_cur_path, sel, sizeof(g_cur_path));
      s_cat(g_cur_path, "/", sizeof(g_cur_path));
      render_listing();
    }
  } else if (s_eq(cmd, "listing_media_up")) {
    unsigned L = s_len(g_cur_path);
    if (L) {
      g_cur_path[L - 1] = 0;
      int k = (int)s_len(g_cur_path) - 1;
      while (k >= 0 && g_cur_path[k] != '/') k--;
      g_cur_path[k + 1] = 0;
    }
    render_listing();
  } else if (s_eq(cmd, "listing_media_open")) {
    char sel[440] = "";
    jstr(buf, "listing_media_sel", sel, sizeof(sel));
    if (sel[0]) offer_file_id(sel);   /* sel = "sha\tname" */
  } else if (s_eq(cmd, "listing_media_full") || s_eq(cmd, "listing_browse")) {
    /* go full-screen: the Torrents screen becomes the browser at the same path,
     * then close the Info screen to reveal it. Reached from the gallery or the
     * Info screen's ☰ menu ("Browse files"). */
    if (g_cur[0]) {
      g_view = 1;
      render_open();
      const char *m = "{\"type\":\"ui.screen.close\"}";
      hal_msg_send(m, s_len(m));
    }
  } else if (s_eq(cmd, "listing_opendir")) {
    /* The shared folder as the OS sees it — for an owned torrent that is the
     * directory being published; for a pinned one, its materialized copy. */
    if (!g_cur[0]) { notify("info", "Open a torrent first"); return; }
    if (!hal_folder_opendir(g_cur, s_len(g_cur)))
      notify("info", "No local copy on disk yet - pin or download it first");
  } else if (s_eq(cmd, "listing_popularity")) {
    show_popularity();
  } else if (s_eq(cmd, "listing_updates")) {
    prompt_updates();
  } else if (s_eq(cmd, "results_search")) {
    /* the built-in search bar fired: read its query, search across all cats */
    jstr(buf, "results_query", g_srch_q, sizeof(g_srch_q));
    g_srch_cat[0] = 0;
    render_search();
  } else if (s_eq(cmd, "srch_cats")) {
    /* back to the category browser — clear the host's search box too, or the
     * old query sits there contradicting the category list below it */
    g_srch_q[0] = 0;
    g_srch_cat[0] = 0;
    field_set("results_query", "");
    render_search();
  } else if (s_eq(cmd, "srch_sort")) {
    /* cycle seeders -> updated -> size */
    if (s_eq(g_srch_sort, "seeders")) s_cpy(g_srch_sort, "updated", sizeof(g_srch_sort));
    else if (s_eq(g_srch_sort, "updated")) s_cpy(g_srch_sort, "size", sizeof(g_srch_sort));
    else s_cpy(g_srch_sort, "seeders", sizeof(g_srch_sort));
    { char t[40] = "Sorting by "; s_cat(t, g_srch_sort, sizeof(t)); notify("info", t); }
    render_search();
  } else if (s_eq(cmd, "results_tap")) {
    char id[600] = "";
    jstr(buf, "results_id", id, sizeof(id));
    if (!id[0] || s_eq(id, "none")) return;
    if (s_pre(id, "cat:")) {
      s_cpy(g_srch_cat, id + 4, sizeof(g_srch_cat));  /* "" = back to categories */
      g_srch_q[0] = 0;
      render_search();
    } else if (s_pre(id, "t:")) {
      open_torrent(id + 2);
    }
  } else if (s_eq(cmd, "m_save")) {
    save_listing(buf);
    render_listing();
  } else if (s_eq(cmd, "m_cover") || s_eq(cmd, "m_banner") ||
             s_eq(cmd, "m_trailer") || s_eq(cmd, "m_gallery") ||
             s_eq(cmd, "m_icon")) {
    if (!g_cur[0]) { notify("info", "Open a torrent first"); return; }
    /* the action name minus the "m_" prefix IS the slot, except the gallery */
    s_cpy(g_pick_slot, s_eq(cmd, "m_gallery") ? "gallery" : cmd + 2,
          sizeof(g_pick_slot));
    const char *m = "{\"type\":\"fs.pick\",\"mode\":\"file\","
                    "\"title\":\"Pick an image or a short clip (max 30MB)\"}";
    hal_msg_send(m, s_len(m));
  } else if (s_eq(cmd, "copy_link")) {
    do_copy_link();
  } else if (s_eq(cmd, "copy_id")) {
    if (!g_cur[0]) { notify("info", "Open a torrent first"); return; }
    prompt_copy("Folder id (hex)", g_cur, g_cur);
  } else if (s_eq(cmd, "settings_apply")) {
    g_pin_on_open = jbool_def(buf, "pin_on_open", 1);
    g_share_author = jbool_def(buf, "share_author", 0);
    char rb[16] = "";
    jstr(buf, "rescan_min", rb, sizeof(rb));
    if (rb[0]) {
      int v = to_int(rb);
      if (v >= 0 && v < 10000) g_rescan_min = v;
    }
    settings_save();
    render_settings();
    notify("info", "Saved");
  } else if (s_eq(cmd, "torrents_tap")) {
    char id[600] = "";
    jstr(buf, "torrents_id", id, sizeof(id));
    if (!id[0] || s_eq(id, "none")) return;

    if (g_view == 1) {
      /* the full-screen file browser inside one torrent */
      if (s_pre(id, "cd:")) {
        s_cat(g_cur_path, id + 3, sizeof(g_cur_path));
        s_cat(g_cur_path, "/", sizeof(g_cur_path));
        render_open();
      } else if (s_eq(id, "up:")) {
        unsigned L = s_len(g_cur_path);
        if (L) {
          g_cur_path[L - 1] = 0;
          int k = (int)s_len(g_cur_path) - 1;
          while (k >= 0 && g_cur_path[k] != '/') k--;
          g_cur_path[k + 1] = 0;
        }
        render_open();
      } else if (s_pre(id, "f:")) {
        offer_file_id(id + 2);
      }
    } else {
      /* the library navigator (main list) */
      if (s_pre(id, "t:")) {
        open_torrent(id + 2);
      } else if (s_pre(id, "dir:")) {
        s_cat(g_lib_path, id + 4, sizeof(g_lib_path));
        s_cat(g_lib_path, "/", sizeof(g_lib_path));
        lib_nav();
        render_list();
      } else if (s_eq(id, "up:")) {
        unsigned L = s_len(g_lib_path);
        if (L) {
          g_lib_path[L - 1] = 0;
          int k = (int)s_len(g_lib_path) - 1;
          while (k >= 0 && g_lib_path[k] != '/') k--;
          g_lib_path[k + 1] = 0;
        }
        lib_nav();
        render_list();
      }
    }
  }
}

__attribute__((export_name("module_destroy")))
void module_destroy(void) {}

__attribute__((export_name("module_tick_interval_ms")))
uint32_t module_tick_interval_ms(void) { return 1000; }
