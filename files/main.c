/*
 * Files — decentralized media archive (DESIGN.md)
 *
 * The control surface over the host's content-addressed media archive
 * (XPRS.md section 16): browse/search the files behind `file:<sha256>.<ext>`
 * tokens, add new files (picker → archive → shareable token), fetch hashes
 * announced by others, and switch on the two provider transports —
 * the Blossom-compatible HTTP endpoint and the BitTorrent seeder.
 *
 * All storage/networking machinery is host-side behind hal_media_* /
 * hal_share_*; this module renders UI and applies policy.
 */

#include <stdint.h>
#include "xprs_wasm_hal.h"

/* ── tiny libc (same helpers as the other wapps) ──────────────────────── */
static unsigned s_len(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static int s_eq(const char *a, const char *b) {
  while (*a && *b && *a == *b) { a++; b++; } return *a == *b;
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
  int neg = 0, v = 0;
  if (*s == '-') { neg = 1; s++; }
  while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
  return neg ? -v : v;
}
static void jesc(char *dst, unsigned m, const char *src) {
  unsigned l = s_len(dst);
  for (const char *p = src; *p && l < m - 3; p++) {
    char c = *p;
    if (c == '"' || c == '\\') { dst[l++] = '\\'; dst[l++] = c; }
    else if (c == '\n') { dst[l++] = '\\'; dst[l++] = 'n'; }
    else dst[l++] = c;
  }
  dst[l] = 0;
}
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
        /* Decode JSON escapes — notably \t and \n, which we use as field
         * separators inside people-row ids (sha\tname, parent\tfolder). */
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
/* "key":<number> → int (0 when absent). */
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

static void notify(const char *level, const char *body) {
  char m[512] = "{\"type\":\"notify\",\"level\":\"";
  s_cat(m, level, sizeof(m));
  s_cat(m, "\",\"title\":\"Share\",\"body\":\"", sizeof(m));
  jesc(m, sizeof(m), body);
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
static void log_line(const char *field, const char *text) {
  char m[400] = "{\"type\":\"ui.log.append\",\"field\":\"";
  s_cat(m, field, sizeof(m));
  s_cat(m, "\",\"line\":\"", sizeof(m));
  jesc(m, sizeof(m), text);
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
static void log_clear(const char *field) {
  char m[80] = "{\"type\":\"ui.log.clear\",\"field\":\"";
  s_cat(m, field, sizeof(m)); s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* "1.2 MB" style size for subtitles. */
static void fmt_size(unsigned b, char *out, unsigned m) {
  char nb[16];
  out[0] = 0;
  if (b < 1024) { u_itoa(b, nb); s_cat(out, nb, m); s_cat(out, " B", m); return; }
  if (b < 1024u * 1024u) {
    u_itoa(b / 1024u, nb); s_cat(out, nb, m); s_cat(out, " KB", m); return;
  }
  u_itoa(b / (1024u * 1024u), nb); s_cat(out, nb, m);
  s_cat(out, " MB", m);
}

/* Format a unix-seconds timestamp as YYYY-MM-DD (civil_from_days). */
static void fmt_date(long secs, char *out, unsigned m) {
  out[0] = 0;
  if (secs <= 0) return;
  long z = secs / 86400 + 719468;
  long era = (z >= 0 ? z : z - 146096) / 146097;
  unsigned doe = (unsigned)(z - era * 146097);
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  long y = (long)yoe + era * 400;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned mp = (5 * doy + 2) / 153;
  unsigned d = doy - (153 * mp + 2) / 5 + 1;
  unsigned mo = mp < 10 ? mp + 3 : mp - 9;
  y += (mo <= 2);
  char nb[16];
  u_itoa((unsigned)y, nb); s_cat(out, nb, m); s_cat(out, "-", m);
  if (mo < 10) s_cat(out, "0", m); u_itoa(mo, nb); s_cat(out, nb, m); s_cat(out, "-", m);
  if (d < 10) s_cat(out, "0", m); u_itoa(d, nb); s_cat(out, nb, m);
}

/* djb2 string hash, for change-detection on rendered lists. */
static uint32_t djb2(const char *s) {
  uint32_t h = 5381;
  for (; *s; s++) h = ((h << 5) + h) ^ (unsigned char)*s;
  return h;
}

/* Send [m] on the wapp bus only if it differs from the last value sent for
 * this *last hash. Prevents the periodic tick from replacing an unchanged list
 * every few seconds — which reset/janked the scroll position. */
static int changed_send(const char *m, uint32_t *last) {
  uint32_t h = djb2(m);
  if (h == *last) return 0;
  *last = h;
  hal_msg_send(m, s_len(m));
  return 1;
}
static uint32_t g_folders_hash = 0; /* shared by render_owned + render_open */
static uint32_t g_lib_hash = 0;     /* render_library */

/* ── sharing settings (persisted in KV) ─────────────────────────────────── */
static int g_blossom_on = 1;
static int g_uploads_on = 0;
static int g_seed_on = 1;
static int g_port = 3457;

static void share_save(void) {
  char b[40]; b[0] = 0;
  s_cat(b, g_blossom_on ? "1" : "0", sizeof(b));
  s_cat(b, g_uploads_on ? "1" : "0", sizeof(b));
  s_cat(b, g_seed_on ? "1" : "0", sizeof(b));
  char pb[12]; u_itoa((unsigned)g_port, pb);
  s_cat(b, pb, sizeof(b));
  hal_kv_set("share", 5, b, s_len(b));
}
static void share_load(void) {
  char b[40];
  uint32_t n = hal_kv_get("share", 5, b, sizeof(b) - 1);
  if (n < 4) return;
  b[n] = 0;
  g_blossom_on = b[0] == '1';
  g_uploads_on = b[1] == '1';
  g_seed_on = b[2] == '1';
  int p = to_int(b + 3);
  if (p > 0 && p < 65536) g_port = p;
}
/* Push the current settings to the host services. */
static void share_apply(void) {
  char m[160] = "{\"server\":";
  s_cat(m, g_blossom_on ? "true" : "false", sizeof(m));
  s_cat(m, ",\"port\":", sizeof(m));
  { char pb[12]; u_itoa((unsigned)g_port, pb); s_cat(m, pb, sizeof(m)); }
  s_cat(m, ",\"uploads\":", sizeof(m));
  s_cat(m, g_uploads_on ? "true" : "false", sizeof(m));
  s_cat(m, ",\"seed\":", sizeof(m));
  s_cat(m, g_seed_on ? "true" : "false", sizeof(m));
  s_cat(m, "}", sizeof(m));
  hal_share_ctl(m, s_len(m));
}

/* ── Library rendering (people list) ────────────────────────────────────── */
static char g_list[65536];   /* hal_media_* / folder-browse JSON */
static char g_out[65536];    /* ui.people.set message */

/* Current library view, so async refreshes (module_tick) keep what the user is
 * looking at: 0 = all files, 1 = search results, 2 = one folder, 3 = folder list. */
static int  g_view = 0;
static char g_query[160] = "";
static char g_dir_parent[96] = "";
static char g_dir_folder[96] = "";

static int s_pre(const char *s, const char *pre) {
  while (*pre) { if (*s != *pre) return 0; s++; pre++; } return 1;
}

/* Copy the raw `"tags":[...]` array out of one entry's JSON slice. */
static void copy_tags(const char *obj, const char *end, char *out, unsigned m) {
  s_cpy(out, "[]", m);
  for (const char *p = obj; p < end - 8; p++) {
    if (p[0]=='"'&&p[1]=='t'&&p[2]=='a'&&p[3]=='g'&&p[4]=='s'&&p[5]=='"'&&p[6]==':'&&p[7]=='[') {
      const char *q = p + 7;
      unsigned i = 0;
      while (q < end && *q != ']' && i < m - 2) out[i++] = *q++;
      if (i < m - 2) out[i++] = ']';
      out[i] = 0;
      return;
    }
  }
}

/* Render a hal_media_* JSON array (list / search / folder) into the people list. */
static void render_items(const char *json, const char *title) {
  char *m = g_out; const unsigned sz = sizeof(g_out);
  m[0] = 0;
  s_cat(m, "{\"type\":\"ui.people.set\",\"field\":\"library\",\"sections\":["
           "{\"title\":\"", sz);
  jesc(m, sz, title);
  s_cat(m, "\",\"items\":[", sz);
  int first = 1;
  /* Walk the JSON array entry by entry: each object starts at '{"sha256"'. */
  const char *p = json;
  while (p && *p) {
    if (!(p[0]=='{'&&p[1]=='"'&&p[2]=='s'&&p[3]=='h'&&p[4]=='a'&&p[5]=='2')) { p++; continue; }
    const char *end = p + 1;
    while (*end && !(end[0]=='}'&&end[1]==','&&end[2]=='{'&&end[3]=='"'&&end[4]=='s')) end++;
    if (*end) end++;            /* include the closing '}' */
    char slice[1600]; unsigned si = 0;
    for (const char *q = p; q < end && si < sizeof(slice) - 1; q++) slice[si++] = *q;
    slice[si] = 0;

    char token[80], name[96], ext[20], tags[200], folder[96];
    jstr(slice, "token", token, sizeof(token));
    jstr(slice, "name", name, sizeof(name));
    jstr(slice, "ext", ext, sizeof(ext));
    jstr(slice, "folder", folder, sizeof(folder));
    copy_tags(slice, slice + si, tags, sizeof(tags));
    unsigned size = (unsigned)jnum(slice, "size");
    unsigned dls = (unsigned)jnum(slice, "downloads");

    char sub[140]; sub[0] = 0;
    s_cat(sub, ".", sizeof(sub)); s_cat(sub, ext, sizeof(sub));
    s_cat(sub, " - ", sizeof(sub));
    { char fs[24]; fmt_size(size, fs, sizeof(fs)); s_cat(sub, fs, sizeof(sub)); }
    { char nb[12]; u_itoa(dls, nb);
      s_cat(sub, " - ", sizeof(sub)); s_cat(sub, nb, sizeof(sub));
      s_cat(sub, " dl", sizeof(sub)); }
    if (folder[0]) { s_cat(sub, " - ", sizeof(sub)); s_cat(sub, folder, sizeof(sub)); }

    if (!first) s_cat(m, ",", sz);
    first = 0;
    s_cat(m, "{\"id\":\"", sz); jesc(m, sz, token);
    s_cat(m, "\",\"title\":\"", sz); jesc(m, sz, name[0] ? name : token);
    s_cat(m, "\",\"subtitle\":\"", sz); jesc(m, sz, sub);
    s_cat(m, "\",\"tags\":", sz); s_cat(m, tags, sz);
    s_cat(m, "}", sz);

    p = end;
  }
  s_cat(m, "]}]}", sz);
  hal_msg_send(m, s_len(m));
}

static void render_library(void) {
  uint32_t n = hal_media_list(0, 100, g_list, sizeof(g_list) - 1);
  g_list[n] = 0;
  render_items(g_list, "Files");
}

static void render_search(void) {
  uint32_t n = hal_media_search(g_query, s_len(g_query), g_list, sizeof(g_list) - 1);
  g_list[n] = 0;
  char title[200]; title[0] = 0;
  s_cat(title, "Search: ", sizeof(title)); s_cat(title, g_query, sizeof(title));
  render_items(g_list, title);
}

static void render_folder(void) {
  char j[240] = "{\"parent\":\"";
  jesc(j, sizeof(j), g_dir_parent);
  s_cat(j, "\",\"folder\":\"", sizeof(j));
  jesc(j, sizeof(j), g_dir_folder);
  s_cat(j, "\"}", sizeof(j));
  uint32_t n = hal_media_list_folder(j, s_len(j), g_list, sizeof(g_list) - 1);
  g_list[n] = 0;
  char title[200]; title[0] = 0;
  if (g_dir_parent[0]) { s_cat(title, g_dir_parent, sizeof(title)); s_cat(title, " / ", sizeof(title)); }
  s_cat(title, g_dir_folder[0] ? g_dir_folder : "(uncategorized)", sizeof(title));
  render_items(g_list, title);
}

/* The virtual-folder tree as tappable rows (id "dir:<parent>\t<folder>"). */
static void render_folders(void) {
  uint32_t n = hal_media_folders(g_list, sizeof(g_list) - 1);
  g_list[n] = 0;
  char *m = g_out; const unsigned sz = sizeof(g_out);
  m[0] = 0;
  s_cat(m, "{\"type\":\"ui.people.set\",\"field\":\"library\",\"sections\":["
           "{\"title\":\"Folders\",\"items\":[", sz);
  int first = 1;
  const char *p = g_list;
  while (p && *p) {
    if (*p != '{') { p++; continue; }
    const char *end = p + 1;
    while (*end && *end != '}') end++;
    char slice[280]; unsigned si = 0;
    for (const char *q = p; q <= end && si < sizeof(slice) - 1; q++) slice[si++] = *q;
    slice[si] = 0;
    char parent[96], folder[96];
    jstr(slice, "parent", parent, sizeof(parent));
    jstr(slice, "folder", folder, sizeof(folder));
    unsigned cnt = (unsigned)jnum(slice, "count");
    char label[210]; label[0] = 0;
    if (parent[0]) { s_cat(label, parent, sizeof(label)); s_cat(label, " / ", sizeof(label)); }
    s_cat(label, folder[0] ? folder : "(uncategorized)", sizeof(label));
    char id[210]; id[0] = 0;
    s_cat(id, "dir:", sizeof(id)); s_cat(id, parent, sizeof(id));
    s_cat(id, "\t", sizeof(id)); s_cat(id, folder, sizeof(id));
    char cb[12]; u_itoa(cnt, cb);
    char subc[24]; subc[0] = 0; s_cat(subc, cb, sizeof(subc)); s_cat(subc, " files", sizeof(subc));
    if (!first) s_cat(m, ",", sz);
    first = 0;
    s_cat(m, "{\"id\":\"", sz); jesc(m, sz, id);
    s_cat(m, "\",\"title\":\"", sz); jesc(m, sz, label);
    s_cat(m, "\",\"subtitle\":\"", sz); jesc(m, sz, subc);
    s_cat(m, "\"}", sz);
    p = end + 1;
  }
  s_cat(m, "]}]}", sz);
  changed_send(m, &g_lib_hash);
}

static void render_current(void) {
  if (g_view == 1) render_search();
  else if (g_view == 2) render_folder();
  else if (g_view == 3) render_folders();
  else render_library();
}

/* ── status (Sharing panel log) ─────────────────────────────────────────── */
static uint32_t g_status_hash = 0;
static void render_status(void) {
  char st[2048];
  uint32_t n = hal_share_status(st, sizeof(st) - 1);
  if (n == 0) return;
  st[n] = 0;
  /* Skip the whole log rebuild (and the host rebuild it triggers) when the
   * sharing status is unchanged — otherwise this fired a UI rebuild every few
   * seconds, janking any scrolling in progress. */
  uint32_t h = djb2(st);
  if (h == g_status_hash) return;
  g_status_hash = h;
  log_clear("share_log");
  char line[120]; line[0] = 0;
  s_cat(line, "Blossom HTTP: ", sizeof(line));
  s_cat(line, jbool_def(st, "running", 0) ? "serving on port " : "stopped (port ",
        sizeof(line));
  { char pb[12]; u_itoa((unsigned)jnum(st, "port"), pb); s_cat(line, pb, sizeof(line)); }
  if (!jbool_def(st, "running", 0)) s_cat(line, ")", sizeof(line));
  log_line("share_log", line);

  line[0] = 0;
  s_cat(line, "Requests served: ", sizeof(line));
  { char nb[12]; u_itoa((unsigned)jnum(st, "requests"), nb); s_cat(line, nb, sizeof(line)); }
  s_cat(line, "  -  bytes: ", sizeof(line));
  { char fs[24]; fmt_size((unsigned)jnum(st, "bytes"), fs, sizeof(fs)); s_cat(line, fs, sizeof(line)); }
  log_line("share_log", line);

  /* one line per active torrent: "seed <token> peers:N" */
  const char *p = st;
  while (*p) {
    if (p[0]=='"'&&p[1]=='i'&&p[2]=='n'&&p[3]=='f'&&p[4]=='o'&&p[5]=='h') {
      char slice[400]; unsigned si = 0;
      const char *q = p;
      while (*q && *q != '}' && si < sizeof(slice) - 2) slice[si++] = *q++;
      slice[si] = 0;
      char tok[80], ih[48], prog[8];
      jstr(slice, "token", tok, sizeof(tok));
      jstr(slice, "infohash", ih, sizeof(ih));
      jstr(slice, "progress", prog, sizeof(prog));
      line[0] = 0;
      s_cat(line, jbool_def(slice, "seeding", 0) ? "seed " : "fetch ", sizeof(line));
      if (tok[0]) s_cat(line, tok, sizeof(line));
      else { s_cat(line, "ih:", sizeof(line)); s_cat(line, ih, sizeof(line)); }
      s_cat(line, " ", sizeof(line)); s_cat(line, prog, sizeof(line));
      s_cat(line, "% peers:", sizeof(line));
      { char nb[12]; u_itoa((unsigned)jnum(slice, "peers"), nb); s_cat(line, nb, sizeof(line)); }
      log_line("share_log", line);
      p = q;
    }
    p++;
  }
}

/* ── Folders (mutable, IPNS-like) ───────────────────────────────────────── */
static char g_cur_folder[80] = "";       /* hex/npub folderId open ("" = list) */
static char g_cur_folder_name[96] = "";
/* Current sub-path within the open folder: "" (root) or ends with '/', e.g.
 * "Albums/2024/". File entries carry their full relative path as `name`, so the
 * tree is navigated by prefix — only one directory level is shown at a time. */
static char g_cur_path[256] = "";
static char g_cur_npub[80] = "";        /* shareable npub of the open folder */

/* Folder targeted by the Info/Edit panels (the open folder, or one picked via a
 * row's "..." menu). Kept separate from g_cur_folder so picking "Stats"/"Edit"
 * from the list doesn't change the underlying browse view. */
static char g_sel_folder[80] = "";
static char g_sel_npub[80] = "";
static char g_sel_name[120] = "";
static uint32_t g_info_hash = 0;   /* change-detect for the Info panel rebuild */
static int g_owned = 0;            /* is the currently-open folder ours to edit? */

/* Parent stack for browsing INTO linked folders: each entry is a folderId we
 * came from, so back returns to the parent folder (not straight to the list). */
static char g_nav_stack[8][80];
static int g_nav_depth = 0;

static void nav_update(void);
static void render_info(void);

/* find `"key":[` → pointer just after '[', or 0. */
static const char *find_arr(const char *buf, const char *key) {
  char pat[40]; pat[0] = '"'; pat[1] = 0;
  s_cat(pat, key, sizeof(pat)); s_cat(pat, "\":[", sizeof(pat));
  unsigned pl = s_len(pat);
  for (const char *p = buf; *p; p++) {
    int ok = 1;
    for (unsigned i = 0; i < pl; i++) { if (p[i] != pat[i]) { ok = 0; break; } }
    if (ok) return p + pl;
  }
  return 0;
}

static void render_owned(void) {
  uint32_t n = hal_folder_list(g_list, sizeof(g_list) - 1);
  g_list[n] = 0;
  char *m = g_out; const unsigned sz = sizeof(g_out);
  m[0] = 0;
  s_cat(m, "{\"type\":\"ui.people.set\",\"field\":\"folders\",\"sections\":["
           "{\"title\":\"My folders\",\"items\":[", sz);
  int first = 1;
  const char *p = g_list;
  while (p && *p) {
    if (*p != '{') { p++; continue; }
    const char *end = p + 1;
    while (*end && *end != '}') end++;
    char slice[300]; unsigned si = 0;
    for (const char *q = p; q <= end && si < sizeof(slice) - 1; q++) slice[si++] = *q;
    slice[si] = 0;
    char fid[80], nm[96], npub[80];
    jstr(slice, "folderId", fid, sizeof(fid));
    jstr(slice, "name", nm, sizeof(nm));
    jstr(slice, "npub", npub, sizeof(npub));
    int on_disk = jbool_def(slice, "onDisk", 0);
    if (fid[0]) {
      if (!first) s_cat(m, ",", sz);
      first = 0;
      s_cat(m, "{\"id\":\"own:", sz); jesc(m, sz, fid);
      s_cat(m, "\",\"title\":\"", sz); jesc(m, sz, nm[0] ? nm : "(folder)");
      s_cat(m, "\",\"subtitle\":\"", sz); jesc(m, sz, npub[0] ? npub : fid);
      /* per-row "..." overflow menu (these rows are all folders we own) */
      s_cat(m, "\",\"menu\":["
               "{\"label\":\"Stats\",\"value\":\"fmenu_stats\"},"
               "{\"label\":\"Copy link\",\"value\":\"fmenu_copy\"},", sz);
      if (on_disk)
        s_cat(m, "{\"label\":\"Open on disk\",\"value\":\"fmenu_open\"},", sz);
      s_cat(m, "{\"label\":\"Edit\",\"value\":\"fmenu_edit\"},"
               "{\"label\":\"Add linked folder\",\"value\":\"fmenu_link\"},"
               "{\"label\":\"Remove\",\"value\":\"fmenu_remove\"}]}", sz);
    }
    p = end + 1;
  }
  s_cat(m, "]}]}", sz);
  changed_send(m, &g_folders_hash);
  nav_update();   /* at the folder list, back leaves the wapp */
  render_info();
}


/* ── In-wapp navigation chrome ───────────────────────────────────────────
 * Tell the host the AppBar title (the deepest folder/segment name) and whether
 * system-back should drill up (inside a folder) or leave the wapp (at the
 * folder list). Sent only on change so the periodic re-render doesn't churn. */
static char g_nav_last_title[96] = "";
static int  g_nav_last_back = -1;

static void nav_send(const char *title, int back) {
  if (back == g_nav_last_back && s_eq(title, g_nav_last_title)) return;
  s_cpy(g_nav_last_title, title, sizeof(g_nav_last_title));
  g_nav_last_back = back;
  char m[200] = "{\"type\":\"ui.nav\",\"title\":\"";
  jesc(m, sizeof(m), title);
  s_cat(m, "\",\"back\":", sizeof(m));
  s_cat(m, back ? "true}" : "false}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

static void nav_update(void) {
  if (!g_cur_folder[0]) { nav_send("", 0); return; }   /* folder list → exits */
  const unsigned pl = s_len(g_cur_path);
  if (!pl) { nav_send(g_cur_folder_name[0] ? g_cur_folder_name : "Folder", 1); return; }
  /* last segment of g_cur_path (which ends with '/') */
  int k = (int)pl - 2;                 /* skip the trailing '/' */
  while (k >= 0 && g_cur_path[k] != '/') k--;
  char seg[96]; s_cpy(seg, g_cur_path + k + 1, sizeof(seg));
  unsigned sl = s_len(seg);
  if (sl && seg[sl - 1] == '/') seg[sl - 1] = 0;
  nav_send(seg[0] ? seg : g_cur_folder_name, 1);
}

/* Render the open folder at the current sub-path using the host's path-scoped
 * browse ("folderId\tpath"): the host returns ONLY the immediate subfolders and
 * files at this level (de-duped + sorted), so the payload and the work stay flat
 * however large the folder is. Subfolders are navigable; each file id keeps the
 * FULL relative path so a download recreates the same on-disk structure. */
static void render_open(void) {
  char arg[360];
  s_cpy(arg, g_cur_folder, sizeof(arg));
  s_cat(arg, "\t", sizeof(arg));
  s_cat(arg, g_cur_path, sizeof(arg));
  uint32_t n = hal_folder_browse(arg, s_len(arg), g_list, sizeof(g_list) - 1);
  g_list[n] = 0;
  jstr(g_list, "name", g_cur_folder_name, sizeof(g_cur_folder_name));
  jstr(g_list, "npub", g_cur_npub, sizeof(g_cur_npub));
  g_owned = jbool_def(g_list, "owned", 0);

  char *m = g_out; const unsigned sz = sizeof(g_out);
  m[0] = 0;
  const unsigned pl = s_len(g_cur_path);

  /* One single list — no tabs. Linked folders (root only), then subfolders,
   * then files; all in the same browsing structure. */
  s_cat(m, "{\"type\":\"ui.people.set\",\"field\":\"folders\",\"sections\":[", sz);
  s_cat(m, "{\"title\":\"\",\"items\":[", sz);
  int first = 1;

  /* Linked folders (other mutable folders linked into this one) — root only.
   * Tap to browse (read-only); "..." copies the link / removes it (owner). */
  if (!pl) {
    for (const char *p = find_arr(g_list, "links"); p && *p && *p != ']';) {
      if (*p != '{') { p++; continue; }
      const char *end = p + 1; while (*end && *end != '}') end++;
      char slice[320]; unsigned si = 0;
      for (const char *q = p; q <= end && si < sizeof(slice) - 1; q++) slice[si++] = *q;
      slice[si] = 0;
      char fidv[80], nm[200];
      jstr(slice, "f", fidv, sizeof(fidv));
      jstr(slice, "name", nm, sizeof(nm));
      if (fidv[0]) {
        if (!first) s_cat(m, ",", sz);
        first = 0;
        s_cat(m, "{\"id\":\"dir:", sz); jesc(m, sz, fidv);
        s_cat(m, "\",\"title\":\"", sz); jesc(m, sz, nm[0] ? nm : "linked folder");
        s_cat(m, "\",\"subtitle\":\"linked folder\",\"menu\":["
                 "{\"label\":\"Copy link\",\"value\":\"linkmenu_copy\"}", sz);
        if (g_owned)
          s_cat(m, ",{\"label\":\"Remove link\",\"value\":\"linkmenu_remove\"}", sz);
        s_cat(m, "]}", sz);
      }
      p = (*end) ? end + 1 : end;
    }
  }

  /* Subfolders (path prefixes) — navigation only. */
  for (const char *p = find_arr(g_list, "dirs"); p && *p && *p != ']';) {
    if (*p != '{') { p++; continue; }
    const char *end = p + 1; while (*end && *end != '}') end++;
    char slice[320]; unsigned si = 0;
    for (const char *q = p; q <= end && si < sizeof(slice) - 1; q++) slice[si++] = *q;
    slice[si] = 0;
    char nm[220]; jstr(slice, "name", nm, sizeof(nm));
    if (nm[0]) {
      if (!first) s_cat(m, ",", sz);
      first = 0;
      s_cat(m, "{\"id\":\"cd:", sz); jesc(m, sz, nm);
      s_cat(m, "\",\"title\":\"", sz); jesc(m, sz, nm);
      s_cat(m, "\",\"subtitle\":\"folder\"}", sz);
    }
    p = (*end) ? end + 1 : end;
  }

  /* Files at this level: basename title, "<size>   <date>   N shared". The
   * "..." menu copies the file's share token or downloads it. */
  for (const char *p = find_arr(g_list, "files"); p && *p && *p != ']';) {
    if (*p != '{') { p++; continue; }
    const char *end = p + 1; while (*end && *end != '}') end++;
    char slice[480]; unsigned si = 0;
    for (const char *q = p; q <= end && si < sizeof(slice) - 1; q++) slice[si++] = *q;
    slice[si] = 0;
    char idv[80], nm[256], base[220];
    jstr(slice, "x", idv, sizeof(idv));
    jstr(slice, "name", nm, sizeof(nm));
    jstr(slice, "base", base, sizeof(base));
    if (idv[0]) {
      if (!first) s_cat(m, ",", sz);
      first = 0;
      s_cat(m, "{\"id\":\"ffile:", sz); jesc(m, sz, idv);
      s_cat(m, "\\t", sz); jesc(m, sz, nm[0] ? nm : base);
      s_cat(m, "\",\"title\":\"", sz); jesc(m, sz, base[0] ? base : (nm[0] ? nm : idv));
      s_cat(m, "\",\"subtitle\":\"", sz);
      char ms[96]; ms[0] = 0;
      unsigned size = (unsigned)jnum(slice, "size");
      if (size) { char fs[24]; fmt_size(size, fs, sizeof(fs)); s_cat(ms, fs, sizeof(ms)); }
      long ts = (long)jnum(slice, "ts");
      if (ts > 0) { char db[16]; fmt_date(ts, db, sizeof(db));
        if (ms[0]) s_cat(ms, "   ", sizeof(ms)); s_cat(ms, db, sizeof(ms)); }
      long dl = (long)jnum(slice, "dl");
      if (dl > 0) { char nb[12]; u_itoa((unsigned)dl, nb);
        if (ms[0]) s_cat(ms, "   ", sizeof(ms));
        s_cat(ms, nb, sizeof(ms)); s_cat(ms, " shared", sizeof(ms)); }
      if (ms[0]) jesc(m, sz, ms);
      s_cat(m, "\",\"menu\":["
               "{\"label\":\"Copy link\",\"value\":\"filemenu_copy\"},"
               "{\"label\":\"Download\",\"value\":\"filemenu_dl\"}]}", sz);
    }
    p = (*end) ? end + 1 : end;
  }
  s_cat(m, "]}]}", sz);
  changed_send(m, &g_folders_hash);

  nav_update();   /* title = current folder/segment; back drills up */
  if (!s_eq(g_sel_folder, g_cur_folder)) {
    s_cpy(g_sel_folder, g_cur_folder, sizeof(g_sel_folder));
    g_info_hash = 0;            /* selection changed → rebuild the Info panel */
  }
  render_info();  /* keep the Info panel current for the open folder */
}

/* Info panel (full-size menu screen): the folder's share link plus serve
 * statistics (how many times its files were shared, and how often over time).
 * Rebuilt only when the underlying stats change, so it never janks scrolling. */
static void render_info(void) {
  if (!g_sel_folder[0]) {
    if (g_info_hash == 1) return;
    g_info_hash = 1;
    log_clear("info_log");
    log_line("info_log", "Open a folder, or use the ... menu on a folder, to see "
                         "its share link and statistics.");
    return;
  }
  char st[2048];
  uint32_t n = hal_folder_stats(g_sel_folder, s_len(g_sel_folder), st, sizeof(st) - 1);
  st[n] = 0;
  /* Refresh the selected folder's link/name every call (cheap) so the Copy/Edit
   * actions always act on the right folder, even when the log rebuild is skipped. */
  jstr(st, "npub", g_sel_npub, sizeof(g_sel_npub));
  jstr(st, "name", g_sel_name, sizeof(g_sel_name));
  uint32_t h = djb2(st);
  if (h == 0 || h == 1) h = 2;
  if (h == g_info_hash) return;
  g_info_hash = h;

  log_clear("info_log");
  char line[220];
  line[0] = 0; s_cat(line, "Folder: ", sizeof(line));
  s_cat(line, g_sel_name[0] ? g_sel_name : "(folder)", sizeof(line));
  log_line("info_log", line);

  log_line("info_log", "Share link (others Open by id with this):");
  log_line("info_log", g_sel_npub[0] ? g_sel_npub : g_sel_folder);
  log_line("info_log", "Use \"Copy link\" above to share it.");

  /* Owner's personal npub (for messaging the admin directly, in the future). */
  { char owner[80]; jstr(st, "owner", owner, sizeof(owner));
    if (owner[0]) {
      log_line("info_log", "");
      log_line("info_log", "Owner (admin) npub:");
      log_line("info_log", owner);
    } }
  log_line("info_log", "");

  line[0] = 0; s_cat(line, "Files: ", sizeof(line));
  { char nb[12]; u_itoa((unsigned)jnum(st, "fileCount"), nb); s_cat(line, nb, sizeof(line)); }
  s_cat(line, "    Total size: ", sizeof(line));
  { char fs[24]; fmt_size((unsigned)jnum(st, "totalBytes"), fs, sizeof(fs)); s_cat(line, fs, sizeof(line)); }
  log_line("info_log", line);

  long serves = (long)jnum(st, "serves");
  line[0] = 0; s_cat(line, "Shared ", sizeof(line));
  { char nb[12]; u_itoa((unsigned)serves, nb); s_cat(line, nb, sizeof(line)); }
  s_cat(line, serves == 1 ? " time in total" : " times in total", sizeof(line));
  log_line("info_log", line);

  line[0] = 0; s_cat(line, "   last 24 hours: ", sizeof(line));
  { char nb[12]; u_itoa((unsigned)jnum(st, "last24h"), nb); s_cat(line, nb, sizeof(line)); }
  log_line("info_log", line);
  line[0] = 0; s_cat(line, "   last 7 days:   ", sizeof(line));
  { char nb[12]; u_itoa((unsigned)jnum(st, "last7d"), nb); s_cat(line, nb, sizeof(line)); }
  log_line("info_log", line);
  line[0] = 0; s_cat(line, "   last 30 days:  ", sizeof(line));
  { char nb[12]; u_itoa((unsigned)jnum(st, "last30d"), nb); s_cat(line, nb, sizeof(line)); }
  log_line("info_log", line);

  const char *top = find_arr(st, "top");
  if (top && *top && *top != ']') {
    log_line("info_log", "");
    log_line("info_log", "Most shared files:");
    for (const char *p = top; p && *p && *p != ']';) {
      if (*p != '{') { p++; continue; }
      const char *end = p + 1; while (*end && *end != '}') end++;
      char slice[300]; unsigned si = 0;
      for (const char *q = p; q <= end && si < sizeof(slice) - 1; q++) slice[si++] = *q;
      slice[si] = 0;
      char tn[200]; jstr(slice, "name", tn, sizeof(tn));
      line[0] = 0; s_cat(line, "   ", sizeof(line)); s_cat(line, tn[0] ? tn : "?", sizeof(line));
      s_cat(line, "  -  ", sizeof(line));
      { char nb[12]; u_itoa((unsigned)jnum(slice, "serves"), nb); s_cat(line, nb, sizeof(line)); }
      log_line("info_log", line);
      p = (*end) ? end + 1 : end;
    }
  }
}

/* Is auto-sync currently on for [fid]? (reads hal_folder_subs) */
static int folder_autosync_on(const char *fid) {
  char buf[2048];
  uint32_t n = hal_folder_subs(buf, sizeof(buf) - 1);
  buf[n] = 0;
  const char *p = buf;
  while (*p) {
    if (*p == '{') {
      const char *end = p + 1;
      while (*end && *end != '}') end++;
      char slice[400]; unsigned si = 0;
      for (const char *q = p; q <= end && si < sizeof(slice) - 1; q++) slice[si++] = *q;
      slice[si] = 0;
      char id[80]; jstr(slice, "folderId", id, sizeof(id));
      if (s_eq(id, fid)) return jbool_def(slice, "autoSync", 0);
      p = end + 1;
      continue;
    }
    p++;
  }
  return 0;
}

static void render_mfolders(void) {
  if (g_cur_folder[0]) render_open();
  else render_owned();
}

/* A single-input folder prompt; [id] is the result-id prefix, [folderId] is
 * appended so the result carries it. */
static void prompt_input1(const char *id, const char *folderId,
                          const char *title, const char *hint, unsigned mx) {
  char m[500] = "{\"type\":\"ui.prompt\",\"id\":\"";
  s_cat(m, id, sizeof(m)); jesc(m, sizeof(m), folderId);
  s_cat(m, "\",\"title\":\"", sizeof(m)); jesc(m, sizeof(m), title);
  s_cat(m, "\",\"input\":{\"hint\":\"", sizeof(m)); jesc(m, sizeof(m), hint);
  s_cat(m, "\",\"max\":", sizeof(m));
  { char nb[12]; u_itoa(mx, nb); s_cat(m, nb, sizeof(m)); }
  s_cat(m, "},\"confirm\":\"OK\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* A read-only popup that shows [body] (selectable) and a Copy button bound to
 * [copyval] — the shareable folder link. id "noop" so the result is ignored. */
static void prompt_copy(const char *title, const char *body, const char *copyval) {
  char m[700] = "{\"type\":\"ui.prompt\",\"id\":\"noop\",\"title\":\"";
  jesc(m, sizeof(m), title);
  s_cat(m, "\",\"body\":\"", sizeof(m)); jesc(m, sizeof(m), body);
  s_cat(m, "\",\"copy\":\"", sizeof(m)); jesc(m, sizeof(m), copyval);
  s_cat(m, "\",\"confirm\":\"Close\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* Prefill a host scalar field (e.g. the Edit panel inputs). */
static void field_set(const char *field, const char *value) {
  char m[420] = "{\"type\":\"ui.field.set\",\"field\":\"";
  s_cat(m, field, sizeof(m));
  s_cat(m, "\",\"value\":\"", sizeof(m)); jesc(m, sizeof(m), value);
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* One setMeta op carrying name+desc+tags together. */
static void folder_setmeta3(const char *fid, const char *name,
                            const char *desc, const char *tags) {
  char j[800] = "{\"op\":\"setMeta\",\"name\":\"";
  jesc(j, sizeof(j), name);
  s_cat(j, "\",\"desc\":\"", sizeof(j)); jesc(j, sizeof(j), desc);
  s_cat(j, "\",\"tags\":\"", sizeof(j)); jesc(j, sizeof(j), tags);
  s_cat(j, "\"}", sizeof(j));
  hal_folder_edit(fid, s_len(fid), j, s_len(j));
}

/* Load a folder's current metadata into the Edit panel and open it. */
static void open_folder_edit(const char *fid) {
  char st[2048];
  uint32_t n = hal_folder_stats(fid, s_len(fid), st, sizeof(st) - 1);
  st[n] = 0;
  char name[160], desc[300], tags[120];
  jstr(st, "name", name, sizeof(name));
  jstr(st, "desc", desc, sizeof(desc));
  jstr(st, "tags", tags, sizeof(tags));
  jstr(st, "npub", g_sel_npub, sizeof(g_sel_npub));
  jstr(st, "name", g_sel_name, sizeof(g_sel_name));
  field_set("edit_name", name);
  field_set("edit_desc", desc);
  field_set("edit_tags", tags);
  const char *m = "{\"type\":\"ui.screen.open\",\"name\":\"Edit\"}";
  hal_msg_send(m, s_len(m));
}

static void prompt_folder_manage(void) {
  char m[900] = "{\"type\":\"ui.prompt\",\"id\":\"fmg:";
  jesc(m, sizeof(m), g_cur_folder);
  s_cat(m, "\",\"title\":\"", sizeof(m));
  jesc(m, sizeof(m), g_cur_folder_name[0] ? g_cur_folder_name : "Folder");
  s_cat(m, "\",\"body\":\"Folder id (share so others can browse):\\n", sizeof(m));
  jesc(m, sizeof(m), g_cur_folder);
  s_cat(m, "\",\"chips\":["
          "{\"label\":\"Download all\",\"value\":\"dlall\"},"
          "{\"label\":\"", sizeof(m));
  s_cat(m, folder_autosync_on(g_cur_folder) ? "Auto-sync: ON" : "Auto-sync: OFF",
        sizeof(m));
  s_cat(m, "\",\"value\":\"sync\"},"
          "{\"label\":\"Add file (owner)\",\"value\":\"addf\"},"
          "{\"label\":\"Rename (owner)\",\"value\":\"name\"},"
          "{\"label\":\"Description (owner)\",\"value\":\"desc\"},"
          "{\"label\":\"Link folder (owner)\",\"value\":\"link\"},"
          "{\"label\":\"Grant admin (owner)\",\"value\":\"grant\"},"
          "{\"label\":\"Revoke admin (owner)\",\"value\":\"revoke\"},"
          "{\"label\":\"Rescan disk (owner)\",\"value\":\"rescan\"}]}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

static void folder_edit_kv(const char *fid, const char *op, const char *key,
                           const char *val) {
  char j[600] = "{\"op\":\"";
  s_cat(j, op, sizeof(j)); s_cat(j, "\",\"", sizeof(j)); s_cat(j, key, sizeof(j));
  s_cat(j, "\":\"", sizeof(j)); jesc(j, sizeof(j), val); s_cat(j, "\"}", sizeof(j));
  hal_folder_edit(fid, s_len(fid), j, s_len(j));
}

/* ── prompts ────────────────────────────────────────────────────────────── */
static void prompt_search(void) {
  const char *m = "{\"type\":\"ui.prompt\",\"id\":\"fsearch\","
    "\"title\":\"Find a file\","
    "\"body\":\"Paste a file: token (found on the local network over Blossom) "
    "or a magnet: link (fetched from the BitTorrent swarm).\","
    "\"input\":{\"hint\":\"file:<sha256>.<ext>  or  magnet:?xt=...\",\"max\":600},"
    "\"confirm\":\"Fetch\"}";
  hal_msg_send(m, s_len(m));
}

/* Flatten the "tags":[...] array of one meta JSON into "a b c" for editing. */
static void tags_flat(const char *meta, char *out, unsigned m) {
  char arr[200]; copy_tags(meta, meta + s_len(meta), arr, sizeof(arr));
  unsigned o = 0;
  for (const char *p = arr; *p && o < m - 1; p++) {
    if (*p == '"' || *p == '[' || *p == ']') continue;
    out[o++] = (*p == ',') ? ' ' : *p;
  }
  out[o] = 0;
}

/* Set a single string metadata field via hal_media_set_meta. */
static void set_meta_kv(const char *token, const char *key, const char *val) {
  char j[400] = "{\"";
  s_cat(j, key, sizeof(j)); s_cat(j, "\":\"", sizeof(j));
  jesc(j, sizeof(j), val); s_cat(j, "\"}", sizeof(j));
  hal_media_set_meta(token, s_len(token), j, s_len(j));
}

/* A focused single-field editor, prefilled with the current value. [kind] is a
 * 2-char tag: nm name, ds description, tg tags, fd folder, pa parent. */
static void prompt_edit(const char *kind, const char *token,
                        const char *title, const char *cur, unsigned maxlen) {
  char m[1100] = "{\"type\":\"ui.prompt\",\"id\":\"f";
  s_cat(m, kind, sizeof(m)); s_cat(m, ":", sizeof(m));
  jesc(m, sizeof(m), token);
  s_cat(m, "\",\"title\":\"", sizeof(m)); jesc(m, sizeof(m), title);
  s_cat(m, "\",\"input\":{\"hint\":\"", sizeof(m)); jesc(m, sizeof(m), title);
  s_cat(m, "\",\"value\":\"", sizeof(m)); jesc(m, sizeof(m), cur);
  s_cat(m, "\",\"max\":", sizeof(m));
  { char nb[12]; u_itoa(maxlen, nb); s_cat(m, nb, sizeof(m)); }
  s_cat(m, "},\"confirm\":\"Save\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

static void prompt_details(const char *token) {
  char meta[1600];
  uint32_t n = hal_media_meta(token, s_len(token), meta, sizeof(meta) - 1);
  if (n == 0) return;
  meta[n] = 0;
  char name[96], desc[300], folder[96], parent[96];
  jstr(meta, "name", name, sizeof(name));
  jstr(meta, "description", desc, sizeof(desc));
  jstr(meta, "folder", folder, sizeof(folder));
  jstr(meta, "parent", parent, sizeof(parent));
  unsigned dls = (unsigned)jnum(meta, "downloads");

  char body[1100]; body[0] = 0;
  { char fs[24]; fmt_size((unsigned)jnum(meta, "size"), fs, sizeof(fs));
    s_cat(body, "Size: ", sizeof(body)); s_cat(body, fs, sizeof(body)); }
  { char nb[12]; u_itoa(dls, nb);
    s_cat(body, "   Downloads: ", sizeof(body)); s_cat(body, nb, sizeof(body)); }
  if (parent[0] || folder[0]) {
    s_cat(body, "\nFolder: ", sizeof(body));
    if (parent[0]) { s_cat(body, parent, sizeof(body)); s_cat(body, " / ", sizeof(body)); }
    s_cat(body, folder[0] ? folder : "(none)", sizeof(body));
  }
  if (desc[0]) { s_cat(body, "\n\n", sizeof(body)); s_cat(body, desc, sizeof(body)); }
  s_cat(body, "\n\nToken (paste into any chat):\n", sizeof(body));
  s_cat(body, token, sizeof(body));
  { char magnet[400];
    uint32_t mg = hal_media_magnet(token, s_len(token), magnet, sizeof(magnet) - 1);
    if (mg > 0) { magnet[mg] = 0;
      s_cat(body, "\n\nMagnet (BitTorrent):\n", sizeof(body));
      s_cat(body, magnet, sizeof(body)); } }

  char m[2000] = "{\"type\":\"ui.prompt\",\"id\":\"fdet:";
  jesc(m, sizeof(m), token);
  s_cat(m, "\",\"title\":\"", sizeof(m));
  jesc(m, sizeof(m), name[0] ? name : "File");
  s_cat(m, "\",\"body\":\"", sizeof(m));
  jesc(m, sizeof(m), body);
  s_cat(m, "\",\"copy\":\"", sizeof(m));
  jesc(m, sizeof(m), token);
  s_cat(m, "\",\"chips\":["
          "{\"label\":\"Rename\",\"value\":\"nm\"},"
          "{\"label\":\"Description\",\"value\":\"ds\"},"
          "{\"label\":\"Tags\",\"value\":\"tg\"},"
          "{\"label\":\"Folder\",\"value\":\"fd\"},"
          "{\"label\":\"Parent\",\"value\":\"pa\"},"
          "{\"label\":\"Delete\",\"value\":\"del\"}]}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* Parse a space-separated tag string into {"tags":[...]} and apply. */
static void apply_tags(const char *token, const char *tags) {
  char j[260] = "{\"tags\":[";
  const char *t = tags; int first = 1; char one[40]; unsigned oi = 0;
  for (;; t++) {
    char c = *t;
    if (c == ' ' || c == 0) {
      if (oi) {
        one[oi] = 0;
        if (!first) s_cat(j, ",", sizeof(j));
        s_cat(j, "\"", sizeof(j)); jesc(j, sizeof(j), one); s_cat(j, "\"", sizeof(j));
        first = 0; oi = 0;
      }
      if (!c) break;
    } else if (oi < sizeof(one) - 1) one[oi++] = c;
  }
  s_cat(j, "]}", sizeof(j));
  hal_media_set_meta(token, s_len(token), j, s_len(j));
  render_current();
}

/* Open the right single-field editor for a detail chip, prefilled. */
static void open_edit(const char *token, const char *which) {
  char meta[1600];
  uint32_t n = hal_media_meta(token, s_len(token), meta, sizeof(meta) - 1);
  meta[n] = 0;
  char cur[300] = "";
  if (s_eq(which, "nm")) { jstr(meta, "name", cur, sizeof(cur)); prompt_edit("nm", token, "Name", cur, 96); }
  else if (s_eq(which, "ds")) { jstr(meta, "description", cur, sizeof(cur)); prompt_edit("ds", token, "Description (max 250)", cur, 250); }
  else if (s_eq(which, "tg")) { tags_flat(meta, cur, sizeof(cur)); prompt_edit("tg", token, "Tags (space-separated)", cur, 120); }
  else if (s_eq(which, "fd")) { jstr(meta, "folder", cur, sizeof(cur)); prompt_edit("fd", token, "Folder name", cur, 96); }
  else if (s_eq(which, "pa")) { jstr(meta, "parent", cur, sizeof(cur)); prompt_edit("pa", token, "Parent folder", cur, 96); }
}

/* ── prompt results / commands ──────────────────────────────────────────── */
static void do_prompt_result(const char *buf) {
  char pid[280] = "", val[24] = "";
  jstr(buf, "prompt_id", pid, sizeof(pid));
  jstr(buf, "prompt_value", val, sizeof(val));
  char inp[320] = "";
  jstr(buf, "prompt_input", inp, sizeof(inp));

  if (s_eq(pid, "fsearch")) {
    /* A magnet link can be long, so re-read prompt_input into a big buffer. */
    char big[600] = "";
    jstr(buf, "prompt_input", big, sizeof(big));
    if (!big[0]) return;
    if (big[0]=='m'&&big[1]=='a'&&big[2]=='g'&&big[3]=='n'&&big[4]=='e'&&big[5]=='t') {
      hal_media_fetch_magnet(big, s_len(big), "", 0);
      notify("info", "Joining the BitTorrent swarm…");
    } else if (hal_media_fetch(big, s_len(big))) {
      notify("info", "Looking on the local network for the file…");
    } else {
      notify("warning", "Paste a file: token or a magnet: link");
    }
    return;
  }
  if (s_eq(pid, "flq")) {
    /* Local archive text search (name, tag, sha256). */
    s_cpy(g_query, inp, sizeof(g_query));
    if (g_query[0]) { g_view = 1; render_search(); }
    else { g_view = 0; render_library(); }
    return;
  }
  if (s_pre(pid, "fdet:")) {
    const char *token = pid + 5;
    if (s_eq(val, "del")) {
      hal_media_delete(token, s_len(token));
      render_current();
      notify("info", "Deleted from the archive");
    } else {
      open_edit(token, val);
    }
    return;
  }
  if (s_pre(pid, "fnm:")) { set_meta_kv(pid + 4, "name", inp); render_current(); notify("info", "Saved"); return; }
  if (s_pre(pid, "fds:")) { if (s_len(inp) > 250) inp[250] = 0; set_meta_kv(pid + 4, "description", inp); render_current(); notify("info", "Saved"); return; }
  if (s_pre(pid, "ftg:")) { apply_tags(pid + 4, inp); notify("info", "Saved"); return; }
  if (s_pre(pid, "ffd:")) { set_meta_kv(pid + 4, "folder", inp); render_current(); notify("info", "Saved"); return; }
  if (s_pre(pid, "fpa:")) { set_meta_kv(pid + 4, "parent", inp); render_current(); notify("info", "Saved"); return; }

  // ── Mutable folders ──
  if (s_eq(pid, "fnew")) {
    if (!inp[0]) return;
    char j[200] = "{\"name\":\""; jesc(j, sizeof(j), inp); s_cat(j, "\"}", sizeof(j));
    char id[80]; uint32_t n = hal_folder_create(j, s_len(j), id, sizeof(id) - 1);
    id[n] = 0;
    if (id[0]) {
      s_cpy(g_cur_folder, id, sizeof(g_cur_folder));
      s_cpy(g_cur_folder_name, inp, sizeof(g_cur_folder_name));
      render_mfolders();
      notify("info", "Folder created");
    } else {
      notify("warning", "Could not create folder (is the network on?)");
    }
    return;
  }
  if (s_eq(pid, "fopen")) {
    if (!inp[0]) return;
    s_cpy(g_cur_folder, inp, sizeof(g_cur_folder));
    g_cur_folder_name[0] = 0;
    render_mfolders();
    return;
  }
  if (s_pre(pid, "fmg:")) {
    const char *fid = pid + 4;
    if (s_eq(val, "addf")) prompt_input1("fadf:", fid, "Add file (token or sha256)", "file:... or sha256", 120);
    else if (s_eq(val, "name")) prompt_input1("fnm2:", fid, "Folder name", "name", 96);
    else if (s_eq(val, "desc")) prompt_input1("fds2:", fid, "Description", "description", 250);
    else if (s_eq(val, "link")) prompt_input1("flnk:", fid, "Link a folder (id or npub)", "folder id / npub", 120);
    else if (s_eq(val, "grant")) prompt_input1("fgr:", fid, "Grant admin (npub or hex)", "npub / hex pubkey", 120);
    else if (s_eq(val, "revoke")) prompt_input1("frv:", fid, "Revoke admin (npub or hex)", "npub / hex pubkey", 120);
    else if (s_eq(val, "rescan")) { hal_folder_rescan(fid, s_len(fid)); notify("info", "Rescanning disk..."); }
    else if (s_eq(val, "dlall")) {
      char j[] = "{\"all\":true}";
      hal_folder_download(fid, s_len(fid), j, s_len(j));
      notify("info", "Downloading all files...");
    } else if (s_eq(val, "sync")) {
      int on = folder_autosync_on(fid) ? 0 : 1;
      hal_folder_autosync(fid, s_len(fid), on);
      notify("info", on ? "Auto-sync on" : "Auto-sync off");
    }
    return;
  }
  if (s_pre(pid, "fadf:")) { folder_edit_kv(pid + 5, "addFile", "x", inp); render_mfolders(); notify("info", "Adding file..."); return; }
  if (s_pre(pid, "fnm2:")) { folder_edit_kv(pid + 5, "setMeta", "name", inp); render_mfolders(); notify("info", "Saved"); return; }
  if (s_pre(pid, "fds2:")) { if (s_len(inp) > 250) inp[250] = 0; folder_edit_kv(pid + 5, "setMeta", "desc", inp); render_mfolders(); notify("info", "Saved"); return; }
  if (s_pre(pid, "flnk:")) { folder_edit_kv(pid + 5, "link", "f", inp); render_mfolders(); notify("info", "Linked"); return; }
  if (s_pre(pid, "fgr:")) { folder_edit_kv(pid + 4, "grant", "p", inp); notify("info", "Admin granted"); return; }
  if (s_pre(pid, "frv:")) { folder_edit_kv(pid + 4, "revoke", "p", inp); notify("info", "Admin revoked"); return; }
  if (s_pre(pid, "frm:")) {
    /* Remove-folder confirmation; the "Remove" chip carries value "yes". */
    if (s_eq(val, "yes")) {
      const char *fid = pid + 4;
      hal_folder_remove(fid, s_len(fid));
      if (s_eq(g_cur_folder, fid)) { g_cur_folder[0] = 0; g_cur_path[0] = 0; }
      if (s_eq(g_sel_folder, fid)) { g_sel_folder[0] = 0; g_info_hash = 0; }
      render_mfolders();
      notify("info", "Folder removed (files on disk kept)");
    }
    return;
  }
  /* file inside a folder: id "ffl:<sha>\t<name>" */
  if (s_pre(pid, "ffl:")) {
    char sha[80] = "", name[160] = "";
    const char *r = pid + 4; unsigned i = 0;
    while (*r && *r != '\t' && i < sizeof(sha) - 1) sha[i++] = *r++;
    sha[i] = 0;
    if (*r == '\t') r++;
    i = 0; while (*r && i < sizeof(name) - 1) name[i++] = *r++;
    name[i] = 0;
    if (s_eq(val, "dl")) {
      char j[300] = "{\"sha\":\"";
      jesc(j, sizeof(j), sha); s_cat(j, "\",\"name\":\"", sizeof(j));
      jesc(j, sizeof(j), name[0] ? name : sha); s_cat(j, "\"}", sizeof(j));
      hal_folder_download(g_cur_folder, s_len(g_cur_folder), j, s_len(j));
      notify("info", "Downloading...");
    } else if (s_eq(val, "fetch")) {
      hal_media_fetch(sha, s_len(sha));
      notify("info", "Fetching...");
    }
    return;
  }
}

/* ── module entry points ────────────────────────────────────────────────── */
static uint64_t g_tick = 0;

__attribute__((export_name("module_init")))
void module_init(void) {
  share_load();
  share_apply();      /* resume the providers with the saved settings */
  render_mfolders();
  render_status();
  hal_log(1, "files: ready", 12);
}

/* Routine LAN scan: refresh the host's directory of reachable Blossom servers
 * so media resolution can query nearby devices by hash without scanning per
 * message. Run shortly after start and then every ~60s. */
static void lan_scan(void) {
  char servers[512];
  hal_lan_scan(servers, sizeof(servers) - 1);
}

__attribute__((export_name("module_tick")))
void module_tick(void) {
  g_tick++;
  if (g_tick == 3 || g_tick % 60 == 0) lan_scan();
  if (g_tick % 5 == 0) render_status();
  /* Refresh the Folders view (folder browse is async/cached on the host). */
  if (g_tick % 6 == 0) render_mfolders();
}

__attribute__((export_name("module_handle_event")))
void module_handle_event(void) {
  if (hal_msg_available() == 0) return;
  char buf[2048];
  uint32_t n = hal_msg_recv(buf, sizeof(buf) - 1);
  if (n == 0) return;
  buf[n] = 0;

  char cmd[40] = "", typ[24] = "";
  jstr(buf, "command", cmd, sizeof(cmd));
  jstr(buf, "type", typ, sizeof(typ));

  if (s_eq(typ, "file.open")) {
    /* Picker result: archive the file and surface its token. */
    char path[400] = "";
    jstr(buf, "path", path, sizeof(path));
    if (!path[0]) return;
    char token[80];
    uint32_t n = hal_media_put_file(path, s_len(path), token, sizeof(token) - 1);
    if (n == 0) { notify("warning", "Could not read that file"); return; }
    token[n] = 0;
    render_current();
    prompt_details(token);    /* shows the shareable token right away */
    return;
  }

  if (s_eq(typ, "fs.picked")) {
    /* file/folder navigator result: a folder is shared from disk; a file is
     * archived + its shareable token shown. */
    char path[400] = "";
    jstr(buf, "path", path, sizeof(path));
    if (!path[0]) return;
    if (jbool_def(buf, "dir", 0)) {
      if (hal_folder_add_disk(path, s_len(path))) {
        g_cur_folder[0] = 0;
        render_owned();
        notify("info", "Sharing folder from disk...");
      } else {
        notify("warning", "Reticulum node still starting - try again in a moment");
      }
    } else {
      char token[80];
      uint32_t n = hal_media_put_file(path, s_len(path), token, sizeof(token) - 1);
      if (n == 0) { notify("warning", "Could not read that file"); return; }
      token[n] = 0;
      render_current();
      prompt_details(token);
    }
    return;
  }

  if (s_eq(cmd, "add_file")) {
    const char *m = "{\"type\":\"fs.pick\",\"mode\":\"both\","
                    "\"title\":\"Add a file, or share a folder\"}";
    hal_msg_send(m, s_len(m));
  } else if (s_eq(cmd, "find_hash")) {
    prompt_search();
  } else if (s_eq(cmd, "search")) {
    const char *m = "{\"type\":\"ui.prompt\",\"id\":\"flq\","
      "\"title\":\"Search your files\","
      "\"input\":{\"hint\":\"name, tag, or sha256\",\"max\":150},"
      "\"confirm\":\"Search\"}";
    hal_msg_send(m, s_len(m));
  } else if (s_eq(cmd, "show_all")) {
    g_view = 0; g_query[0] = 0; render_library();
  } else if (s_eq(cmd, "folders")) {
    g_view = 3; render_folders();
  } else if (s_eq(cmd, "folder_new")) {
    prompt_input1("fnew", "", "New folder", "folder name", 96);
  } else if (s_eq(cmd, "folder_open_id")) {
    prompt_input1("fopen", "", "Open folder", "folder id or npub", 120);
  } else if (s_eq(cmd, "folder_add_disk")) {
    const char *m = "{\"type\":\"fs.pick\",\"mode\":\"both\","
                    "\"title\":\"Pick a file to add, or a folder to share\"}";
    hal_msg_send(m, s_len(m));
  } else if (s_eq(cmd, "folder_back")) {
    g_cur_folder[0] = 0; g_cur_path[0] = 0; g_nav_depth = 0; render_mfolders();
  } else if (s_eq(cmd, "nav_back")) {
    /* System-back / AppBar up: up one subpath, then to the parent linked folder
     * (if we browsed into one), then out to the folder list. */
    if (g_cur_path[0]) {
      unsigned L = s_len(g_cur_path);
      g_cur_path[L - 1] = 0;
      int k = (int)s_len(g_cur_path) - 1;
      while (k >= 0 && g_cur_path[k] != '/') k--;
      g_cur_path[k + 1] = 0;
      render_open();
    } else if (g_nav_depth > 0) {
      s_cpy(g_cur_folder, g_nav_stack[--g_nav_depth], sizeof(g_cur_folder));
      g_cur_folder_name[0] = 0;
      render_open();
    } else if (g_cur_folder[0]) {
      g_cur_folder[0] = 0; render_mfolders();
    }
  } else if (s_eq(cmd, "copy_key")) {
    if (!g_sel_folder[0]) { notify("info", "Open a folder first"); }
    else {
      char body[420] = "Share this so others can open the folder (Open by id):\n";
      s_cat(body, g_sel_npub[0] ? g_sel_npub : g_sel_folder, sizeof(body));
      prompt_copy("Folder share link", body,
                  g_sel_npub[0] ? g_sel_npub : g_sel_folder);
    }
  } else if (s_eq(cmd, "copy_id")) {
    if (!g_sel_folder[0]) { notify("info", "Open a folder first"); }
    else {
      char body[200] = "Folder id (hex):\n";
      s_cat(body, g_sel_folder, sizeof(body));
      prompt_copy("Folder id", body, g_sel_folder);
    }
  } else if (s_pre(cmd, "fmenu_")) {
    /* Row "..." menu on a shared folder: id "own:<fid>". */
    char id[140] = ""; jstr(buf, "folders_id", id, sizeof(id));
    const char *fid = (s_pre(id, "own:") || s_pre(id, "dir:")) ? id + 4 : id;
    if (!fid[0]) return;
    s_cpy(g_sel_folder, fid, sizeof(g_sel_folder));
    g_info_hash = 0;                 /* selection changed → refresh Info */
    if (s_eq(cmd, "fmenu_stats")) {
      render_info();
      const char *m = "{\"type\":\"ui.screen.open\",\"name\":\"Info\"}";
      hal_msg_send(m, s_len(m));
    } else if (s_eq(cmd, "fmenu_copy")) {
      render_info();                 /* populates g_sel_npub */
      char body[420] = "Share this so others can open the folder (Open by id):\n";
      s_cat(body, g_sel_npub[0] ? g_sel_npub : g_sel_folder, sizeof(body));
      prompt_copy("Folder share link", body,
                  g_sel_npub[0] ? g_sel_npub : g_sel_folder);
    } else if (s_eq(cmd, "fmenu_open")) {
      if (hal_folder_opendir(g_sel_folder, s_len(g_sel_folder)))
        notify("info", "Opening folder on disk - edits sync automatically");
      else
        notify("warning", "No file manager, or not a disk folder");
    } else if (s_eq(cmd, "fmenu_edit")) {
      open_folder_edit(g_sel_folder);
    } else if (s_eq(cmd, "fmenu_link")) {
      /* Add a linked folder INTO this folder — just its npub/id. */
      prompt_input1("flnk:", g_sel_folder, "Add linked folder",
                    "folder npub or id", 120);
    } else if (s_eq(cmd, "fmenu_remove")) {
      char m[420] = "{\"type\":\"ui.prompt\",\"id\":\"frm:";
      jesc(m, sizeof(m), g_sel_folder);
      s_cat(m, "\",\"title\":\"Remove shared folder?\",\"body\":\"This stops "
               "sharing the folder. The files on disk are NOT deleted.\","
               "\"chips\":[{\"label\":\"Remove\",\"value\":\"yes\"}],"
               "\"confirm\":\"Cancel\"}", sizeof(m));
      hal_msg_send(m, s_len(m));
    }
  } else if (s_pre(cmd, "filemenu_")) {
    /* File "..." menu: folders_id = "ffile:<sha>\t<name>". */
    char id[300] = ""; jstr(buf, "folders_id", id, sizeof(id));
    if (!s_pre(id, "ffile:")) return;
    char sha[80] = "", name[200] = "";
    const char *r = id + 6; unsigned i = 0;
    while (*r && *r != '\t' && i < sizeof(sha) - 1) sha[i++] = *r++;
    sha[i] = 0;
    if (*r == '\t') r++;
    i = 0; while (*r && i < sizeof(name) - 1) name[i++] = *r++;
    name[i] = 0;
    if (!sha[0]) return;
    if (s_eq(cmd, "filemenu_copy")) {
      char body[320] = "Share this file (fetch by its token):\nfile:";
      s_cat(body, sha, sizeof(body));
      char tok[90] = "file:"; s_cat(tok, sha, sizeof(tok));
      prompt_copy(name[0] ? name : "File", body, tok);
    } else if (s_eq(cmd, "filemenu_dl")) {
      char j[320] = "{\"sha\":\"";
      jesc(j, sizeof(j), sha); s_cat(j, "\",\"name\":\"", sizeof(j));
      jesc(j, sizeof(j), name[0] ? name : sha); s_cat(j, "\"}", sizeof(j));
      hal_folder_download(g_cur_folder, s_len(g_cur_folder), j, s_len(j));
      notify("info", "Downloading...");
    }
  } else if (s_pre(cmd, "linkmenu_")) {
    /* Linked-folder "..." menu: folders_id = "dir:<childId>". */
    char id[140] = ""; jstr(buf, "folders_id", id, sizeof(id));
    const char *child = s_pre(id, "dir:") ? id + 4 : id;
    if (!child[0]) return;
    if (s_eq(cmd, "linkmenu_copy")) {
      char st[1024]; uint32_t n = hal_folder_stats(child, s_len(child), st, sizeof(st) - 1);
      st[n] = 0;
      char npub[80]; jstr(st, "npub", npub, sizeof(npub));
      char body[320] = "Linked folder share link (Open by id):\n";
      s_cat(body, npub[0] ? npub : child, sizeof(body));
      prompt_copy("Linked folder link", body, npub[0] ? npub : child);
    } else if (s_eq(cmd, "linkmenu_remove")) {
      /* Unlink it from the folder we're viewing (g_cur_folder). */
      folder_edit_kv(g_cur_folder, "unlink", "f", child);
      notify("info", "Linked folder removed");
      render_mfolders();
    }
  } else if (s_eq(cmd, "folder_save")) {
    if (!g_sel_folder[0]) { notify("info", "No folder selected"); }
    else {
      char name[200], desc[320], tags[160];
      jstr(buf, "edit_name", name, sizeof(name));
      jstr(buf, "edit_desc", desc, sizeof(desc));
      jstr(buf, "edit_tags", tags, sizeof(tags));
      if (s_len(name) > 100) name[100] = 0;     /* enforce caps defensively */
      if (s_len(desc) > 250) desc[250] = 0;
      if (s_len(tags) > 50) tags[50] = 0;
      folder_setmeta3(g_sel_folder, name, desc, tags);
      const char *m = "{\"type\":\"ui.screen.close\"}";
      hal_msg_send(m, s_len(m));
      g_info_hash = 0;
      notify("info", "Saved");
      render_mfolders();
    }
  } else if (s_eq(cmd, "folder_manage")) {
    if (g_cur_folder[0]) prompt_folder_manage();
    else notify("info", "Open a folder first");
  } else if (s_eq(cmd, "folders_tap")) {
    char id[280] = "";
    jstr(buf, "folders_id", id, sizeof(id));
    if (!id[0]) return;
    if (s_pre(id, "own:")) {
      /* open a top-level folder from the list — fresh navigation. */
      g_nav_depth = 0;
      s_cpy(g_cur_folder, id + 4, sizeof(g_cur_folder));
      g_cur_folder_name[0] = 0;
      g_cur_path[0] = 0;          /* enter at the folder root */
      render_open();
    } else if (s_pre(id, "dir:")) {
      /* browse INTO a linked folder — remember the parent so back returns here. */
      if (g_cur_folder[0] && g_nav_depth < 8)
        s_cpy(g_nav_stack[g_nav_depth++], g_cur_folder, 80);
      s_cpy(g_cur_folder, id + 4, sizeof(g_cur_folder));
      g_cur_folder_name[0] = 0;
      g_cur_path[0] = 0;
      render_open();
    } else if (s_pre(id, "cd:")) {
      /* descend into a subfolder: append "<segment>/" to the current path */
      s_cat(g_cur_path, id + 3, sizeof(g_cur_path));
      s_cat(g_cur_path, "/", sizeof(g_cur_path));
      render_open();
    } else if (s_eq(id, "up:")) {
      /* go up one level: drop the trailing '/', then the last segment */
      unsigned L = s_len(g_cur_path);
      if (L) {
        g_cur_path[L - 1] = 0;
        int k = (int)s_len(g_cur_path) - 1;
        while (k >= 0 && g_cur_path[k] != '/') k--;
        g_cur_path[k + 1] = 0;
      }
      render_open();
    } else if (s_pre(id, "ffile:")) {
      /* id = "ffile:<sha>\t<name>" */
      char sha[80] = "", name[160] = "";
      const char *r = id + 6; unsigned i = 0;
      while (*r && *r != '\t' && i < sizeof(sha) - 1) sha[i++] = *r++;
      sha[i] = 0;
      if (*r == '\t') r++;
      i = 0; while (*r && i < sizeof(name) - 1) name[i++] = *r++;
      name[i] = 0;
      char m[500] = "{\"type\":\"ui.prompt\",\"id\":\"ffl:";
      jesc(m, sizeof(m), sha); s_cat(m, "\\t", sizeof(m)); jesc(m, sizeof(m), name);
      s_cat(m, "\",\"title\":\"", sizeof(m)); jesc(m, sizeof(m), name[0] ? name : "File");
      s_cat(m, "\",\"body\":\"sha256:\\n", sizeof(m)); jesc(m, sizeof(m), sha);
      s_cat(m, "\",\"copy\":\"", sizeof(m)); jesc(m, sizeof(m), sha);
      s_cat(m, "\",\"chips\":[{\"label\":\"Download\",\"value\":\"dl\"},"
              "{\"label\":\"Fetch\",\"value\":\"fetch\"}]}", sizeof(m));
      hal_msg_send(m, s_len(m));
    }
  } else if (s_eq(cmd, "library_tap")) {
    char id[200] = "";
    jstr(buf, "library_id", id, sizeof(id));
    if (!id[0]) return;
    if (s_pre(id, "dir:")) {
      /* "dir:<parent>\t<folder>" — open that virtual folder. */
      const char *r = id + 4;
      unsigned i = 0;
      while (*r && *r != '\t' && i < sizeof(g_dir_parent) - 1) g_dir_parent[i++] = *r++;
      g_dir_parent[i] = 0;
      if (*r == '\t') r++;
      i = 0;
      while (*r && i < sizeof(g_dir_folder) - 1) g_dir_folder[i++] = *r++;
      g_dir_folder[i] = 0;
      g_view = 2; render_folder();
    } else {
      prompt_details(id);
    }
  } else if (s_eq(cmd, "share_apply")) {
    g_blossom_on = jbool_def(buf, "blossom_on", 1);
    g_uploads_on = jbool_def(buf, "uploads_on", 0);
    g_seed_on = jbool_def(buf, "seed_on", 1);
    char pb[12] = "";
    jstr(buf, "blossom_port", pb, sizeof(pb));
    if (pb[0]) { int p = to_int(pb); if (p > 0 && p < 65536) g_port = p; }
    share_save();
    share_apply();
    render_status();
    notify("info", "Sharing settings applied");
  } else if (s_eq(cmd, "prompt")) {
    do_prompt_result(buf);
  }
}

__attribute__((export_name("module_destroy")))
void module_destroy(void) {}

__attribute__((export_name("module_tick_interval_ms")))
uint32_t module_tick_interval_ms(void) { return 1000; }
