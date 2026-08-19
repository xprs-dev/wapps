#include <stdint.h>
#include "xprs_wasm_hal.h"
#include "room.h"

/* ── self-contained helpers (main.c's are static; keep this TU independent) ── */

static unsigned s_len(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static int s_eq(const char *a, const char *b) {
  unsigned i = 0; for (; a[i] && b[i]; i++) if (a[i] != b[i]) return 0;
  return a[i] == b[i];
}
static void s_cpy(char *d, const char *s, unsigned cap) {
  unsigned i = 0; if (!cap) return; for (; s && s[i] && i + 1 < cap; i++) d[i] = s[i]; d[i] = 0;
}
static void s_cat(char *d, const char *s, unsigned cap) {
  unsigned l = s_len(d), i = 0; for (; s && s[i] && l + 1 < cap; i++) d[l++] = s[i]; d[l] = 0;
}
static void u_ltoa(long v, char *out) {
  char t[24]; int n = 0, neg = v < 0; unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
  if (!u) t[n++] = '0'; while (u) { t[n++] = (char)('0' + u % 10); u /= 10; }
  int k = 0; if (neg) out[k++] = '-'; while (n) out[k++] = t[--n]; out[k] = 0;
}
static void cat_l(char *d, long v, unsigned cap) { char b[24]; u_ltoa(v, b); s_cat(d, b, cap); }
/* JSON-escape src into the end of dst. */
static void jesc(char *dst, unsigned cap, const char *src) {
  unsigned l = s_len(dst);
  for (unsigned i = 0; src && src[i] && l + 2 < cap; i++) {
    char c = src[i];
    if (c == '"' || c == '\\') { dst[l++] = '\\'; dst[l++] = c; }
    else if (c == '\n') { dst[l++] = '\\'; dst[l++] = 'n'; }
    else if (c == '\r') { dst[l++] = '\\'; dst[l++] = 'r'; }
    else if (c == '\t') { dst[l++] = '\\'; dst[l++] = 't'; }
    else if ((unsigned char)c < 0x20) { continue; }
    else dst[l++] = c;
  }
  dst[l] = 0;
}

/* Find the value position just after `"key":` at object scope (naive: first
 * match; fine for the flat NOSTR event objects and our own JSON). */
static const char *after_key(const char *j, const char *key) {
  unsigned kl = s_len(key);
  for (const char *p = j; *p; p++) {
    if (*p != '"') continue;
    unsigned i = 0; for (; i < kl && p[1 + i]; i++) if (p[1 + i] != key[i]) break;
    if (i == kl && p[1 + kl] == '"' && p[2 + kl] == ':') return p + 3 + kl;
  }
  return 0;
}
/* Copy the string value of top-level [key] into out. Returns 1 if found. */
static int j_str(const char *j, const char *key, char *out, unsigned cap) {
  const char *p = after_key(j, key);
  out[0] = 0;
  if (!p) return 0;
  while (*p == ' ') p++;
  if (*p != '"') return 0;
  p++;
  unsigned o = 0;
  for (; *p && *p != '"' && o + 1 < cap; p++) {
    if (*p == '\\' && p[1]) { p++; char c = *p;
      out[o++] = (c == 'n') ? '\n' : (c == 't') ? '\t' : (c == 'r') ? '\r' : c; }
    else out[o++] = *p;
  }
  out[o] = 0;
  return 1;
}
/* Long integer value of top-level [key], or [dflt]. */
static long j_long(const char *j, const char *key, long dflt) {
  const char *p = after_key(j, key);
  if (!p) return dflt;
  while (*p == ' ') p++;
  int neg = 0; if (*p == '-') { neg = 1; p++; }
  if (*p < '0' || *p > '9') return dflt;
  long v = 0; for (; *p >= '0' && *p <= '9'; p++) v = v * 10 + (*p - '0');
  return neg ? -v : v;
}

/* ── tag scanning over a NOSTR event's "tags":[[...],[...]] ── */

/* Start of the tags array content (just after '['), or 0. */
static const char *tags_start(const char *j) {
  const char *p = after_key(j, "tags");
  if (!p) return 0;
  while (*p == ' ') p++;
  return (*p == '[') ? p + 1 : 0;
}
/* Read the next JSON string starting at *pp (skips ws/commas); copy into out.
 * Advances *pp past the string. Returns 1 if a string was read. */
static int next_str(const char **pp, char *out, unsigned cap) {
  const char *p = *pp;
  while (*p && *p != '"' && *p != ']') p++;
  if (*p != '"') { *pp = p; out[0] = 0; return 0; }
  p++;
  unsigned o = 0;
  for (; *p && *p != '"' && o + 1 < cap; p++) {
    if (*p == '\\' && p[1]) { p++; out[o++] = *p; } else out[o++] = *p;
  }
  out[o] = 0;
  if (*p == '"') p++;
  *pp = p;
  return 1;
}
/* Get the [idx]-th element (0=name,1=first value,...) of the first tag whose
 * name == [name]. Returns 1 if found. */
static int tag_get(const char *j, const char *name, int idx, char *out, unsigned cap) {
  const char *p = tags_start(j);
  out[0] = 0;
  if (!p) return 0;
  while (*p && *p != ']') {              /* each iteration = one [ ... ] tag */
    while (*p == ' ' || *p == ',') p++;
    if (*p != '[') break;
    p++;
    char el[128]; int e = 0, hit = 0;
    while (next_str(&p, el, sizeof(el))) {
      if (e == 0) hit = s_eq(el, name);
      if (hit && e == idx) { s_cpy(out, el, cap); }
      e++;
      const char *q = p; while (*q == ' ') q++;
      if (*q == ']') { p = q + 1; break; }
    }
    if (hit && out[0]) return 1;
    while (*p && *p != ']' && *p != '[') p++; /* to next tag / array end */
    if (*p == ']') { break; }
  }
  return 0;
}

/* ── database ── */

static int g_db = -1;
static char g_self[65];        /* our x-only pubkey hex */
char ROOM_MAIN_ADMIN[65] = ""; /* project global-admin key (see room.h) */

/* Reputation tuning (documented in docs/chat-rooms.md; adjust here). */
#define REP_WINDOW_SEC (182L * 24 * 3600) /* ~6 months */
#define REP_W_MSG 2
/* Level thresholds: level = 1 + count of thresholds passed, capped at 10. */
static const long REP_THRESH[9] = {5, 15, 40, 90, 180, 350, 650, 1200, 2500};

/* ── db wrappers over hal_sqlite ── */

static int db_exec(const char *sql, const char *params) {
  return hal_sqlite_exec(g_db, sql, s_len(sql), params ? params : "",
                         params ? s_len(params) : 0);
}
static int db_query(const char *sql, const char *params, char *out, unsigned cap) {
  return hal_sqlite_query(g_db, sql, s_len(sql), params ? params : "",
                          params ? s_len(params) : 0, out, cap);
}
/* Build a JSON params array from up to 4 string args (NULL to stop). */
static void params_of(char *out, unsigned cap, const char *a, const char *b,
                      const char *c, const char *d) {
  out[0] = 0;
  s_cat(out, "[", cap);
  const char *v[4] = {a, b, c, d};
  int first = 1;
  for (int i = 0; i < 4; i++) {
    if (!v[i]) break;
    if (!first) s_cat(out, ",", cap);
    first = 0;
    s_cat(out, "\"", cap);
    jesc(out, cap, v[i]);
    s_cat(out, "\"", cap);
  }
  s_cat(out, "]", cap);
}
/* Does the query return at least one row? */
static int db_exists(const char *sql, const char *params) {
  char r[64];
  int n = db_query(sql, params, r, sizeof(r));
  return n > 2; /* "[]" is empty; anything longer has a row */
}

/* ── schema + init ── */

static void ensure_schema(void) {
  db_exec("CREATE TABLE IF NOT EXISTS rooms("
          "roomId TEXT PRIMARY KEY, adminPub TEXT, name TEXT, description TEXT,"
          "parentRoomId TEXT, access TEXT, approvedBy TEXT,"
          "closed INTEGER DEFAULT 0, createdTs INTEGER)", 0);
  db_exec("CREATE TABLE IF NOT EXISTS room_mods("
          "roomId TEXT, pub TEXT, PRIMARY KEY(roomId, pub))", 0);
  db_exec("CREATE TABLE IF NOT EXISTS ops("
          "id TEXT PRIMARY KEY, roomId TEXT, authorPub TEXT, targetPub TEXT,"
          "op TEXT, amount INTEGER, until INTEGER, reason TEXT, ts INTEGER)", 0);
  db_exec("CREATE TABLE IF NOT EXISTS msgs("
          "id TEXT PRIMARY KEY, roomId TEXT, author TEXT, ts INTEGER)", 0);
  db_exec("CREATE INDEX IF NOT EXISTS msgs_author ON msgs(author, ts)", 0);
  db_exec("CREATE INDEX IF NOT EXISTS ops_room ON ops(roomId, ts)", 0);
  db_exec("CREATE TABLE IF NOT EXISTS proposals("
          "id TEXT PRIMARY KEY, parentRoomId TEXT, proposerPub TEXT,"
          "name TEXT, description TEXT, ts INTEGER, status TEXT)", 0);
  db_exec("CREATE TABLE IF NOT EXISTS approvals("
          "id TEXT PRIMARY KEY, proposalId TEXT, approverPub TEXT, ts INTEGER)", 0);
  /* verified = the room's creation was sanctioned (root, or created by / approved
   * by a parent authority). Unverified sub-rooms are hidden until an approval
   * lands. Separate from moderation `closed`. (harmless if the column exists.) */
  db_exec("ALTER TABLE rooms ADD COLUMN verified INTEGER DEFAULT 1", 0);
}

void room_init(void) {
  if (g_db < 0) {
    g_db = hal_sqlite_open("rooms.sqlite3", 12);
    if (g_db < 0) return;
    ensure_schema();
  }
  if (!g_self[0]) {
    unsigned n = hal_nostr_self(g_self, sizeof(g_self));
    g_self[n] = 0;
  }
  /* Bring-up: with no project key configured, the running user is the global
   * admin so moderation is testable. Replace ROOM_MAIN_ADMIN before release. */
  if (!ROOM_MAIN_ADMIN[0] && g_self[0]) s_cpy(ROOM_MAIN_ADMIN, g_self, sizeof(ROOM_MAIN_ADMIN));
  if (ROOM_MAIN_ADMIN[0]) {
    char p[256];
    params_of(p, sizeof(p), MAIN_ROOM_ID, ROOM_MAIN_ADMIN, "Main room", 0);
    db_exec("INSERT OR IGNORE INTO rooms"
            "(roomId,adminPub,name,description,parentRoomId,access,closed,createdTs)"
            " VALUES(?,?,?,'','','open',0,0)", p);
  }
}

/* ── small room-table getters ── */

static void room_field(const char *roomId, const char *col, char *out, unsigned cap) {
  char sql[96] = "SELECT ";
  s_cat(sql, col, sizeof(sql));
  s_cat(sql, " AS v FROM rooms WHERE roomId=? LIMIT 1", sizeof(sql));
  char p[128]; params_of(p, sizeof(p), roomId, 0, 0, 0);
  char r[512]; out[0] = 0;
  if (db_query(sql, p, r, sizeof(r)) > 2) j_str(r, "v", out, cap);
}

int room_is_room(const char *id) {
  if (!id || !id[0]) return 0;
  char p[128]; params_of(p, sizeof(p), id, 0, 0, 0);
  return db_exists("SELECT 1 FROM rooms WHERE roomId=? LIMIT 1", p);
}

/* ── authority (subtree walk) ── */

/* Make sure we know our own pubkey (the profile key can land after init). */
static void ensure_self(void) {
  if (!g_self[0]) { unsigned n = hal_nostr_self(g_self, sizeof(g_self)); g_self[n] = 0; }
  if (!ROOM_MAIN_ADMIN[0] && g_self[0]) s_cpy(ROOM_MAIN_ADMIN, g_self, sizeof(ROOM_MAIN_ADMIN));
}

int room_has_authority(const char *pub, const char *roomId) {
  if (!pub || !pub[0]) return 0;
  if (ROOM_MAIN_ADMIN[0] && s_eq(pub, ROOM_MAIN_ADMIN)) return 1; /* global admin */
  /* global mods = mods of the main room */
  { char p[160]; params_of(p, sizeof(p), MAIN_ROOM_ID, pub, 0, 0);
    if (db_exists("SELECT 1 FROM room_mods WHERE roomId=? AND pub=? LIMIT 1", p)) return 1; }
  /* walk from roomId up to the root */
  char cur[80]; s_cpy(cur, roomId, sizeof(cur));
  for (int hop = 0; hop < 32 && cur[0]; hop++) {
    char admin[65]; room_field(cur, "adminPub", admin, sizeof(admin));
    if (admin[0] && s_eq(admin, pub)) return 1;
    char p[160]; params_of(p, sizeof(p), cur, pub, 0, 0);
    if (db_exists("SELECT 1 FROM room_mods WHERE roomId=? AND pub=? LIMIT 1", p)) return 1;
    char par[80]; room_field(cur, "parentRoomId", par, sizeof(par));
    if (!par[0] || s_eq(par, cur)) break;
    s_cpy(cur, par, sizeof(cur));
  }
  return 0;
}

int room_self_authority(const char *roomId) { ensure_self(); return room_has_authority(g_self, roomId); }

/* ── moderation reducer: a member's status in a room ── */

/* Returns: 0 active, 1 suspended (out_until set), 2 banned, 3 room closed.
 * Applies honoured ops (author has authority over the op's room) in ts order. */
static int member_status(const char *roomId, const char *pub, long *out_until) {
  if (out_until) *out_until = 0;
  int status = 0; long until = 0;
  char p[192]; params_of(p, sizeof(p), roomId, "*", pub, 0);
  char rows[4096];
  /* ops targeting pub, in this room or global (h='*'), oldest first */
  int n = db_query("SELECT authorPub,op,until,roomId FROM ops "
                   "WHERE (roomId=? OR roomId=?) AND targetPub=? ORDER BY ts",
                   p, rows, sizeof(rows));
  if (n > 2) {
    const char *q = rows;
    for (;;) {
      const char *obj = 0; for (const char *s = q; *s; s++) if (*s == '{') { obj = s; break; }
      if (!obj) break;
      const char *end = obj; int depth = 0;
      for (; *end; end++) { if (*end == '{') depth++; else if (*end == '}') { depth--; if (!depth) { end++; break; } } }
      char one[512]; unsigned L = (unsigned)(end - obj); if (L >= sizeof(one)) L = sizeof(one) - 1;
      for (unsigned i = 0; i < L; i++) one[i] = obj[i]; one[L] = 0;
      char author[65], op[16], orm[80]; long u;
      j_str(one, "authorPub", author, sizeof(author));
      j_str(one, "op", op, sizeof(op));
      j_str(one, "roomId", orm, sizeof(orm));
      u = j_long(one, "until", 0);
      /* honour only if the op author has authority over the room it names */
      if (room_has_authority(author, s_eq(orm, "*") ? MAIN_ROOM_ID : orm)) {
        if (s_eq(op, "ban")) status = 2;
        else if (s_eq(op, "kick")) { if (status != 2) status = 0; /* kick = removed, re-joinable */ }
        else if (s_eq(op, "suspend")) { status = 1; until = u; }
        else if (s_eq(op, "unsuspend")) { if (status == 1) { status = 0; until = 0; } }
      }
      q = end;
    }
  }
  if (out_until) *out_until = until;
  return status;
}

/* ── reputation ── */

static long net_points(const char *pub) {
  char p[128]; params_of(p, sizeof(p), pub, 0, 0, 0);
  char rows[4096];
  long pts = 0;
  int n = db_query("SELECT authorPub,op,amount,roomId FROM ops "
                   "WHERE targetPub=? AND (op='award' OR op='deduct') ORDER BY ts",
                   p, rows, sizeof(rows));
  if (n <= 2) return 0;
  const char *q = rows;
  for (;;) {
    const char *obj = 0; for (const char *s = q; *s; s++) if (*s == '{') { obj = s; break; }
    if (!obj) break;
    const char *end = obj; int depth = 0;
    for (; *end; end++) { if (*end == '{') depth++; else if (*end == '}') { depth--; if (!depth) { end++; break; } } }
    char one[384]; unsigned L = (unsigned)(end - obj); if (L >= sizeof(one)) L = sizeof(one) - 1;
    for (unsigned i = 0; i < L; i++) one[i] = obj[i]; one[L] = 0;
    char author[65], op[16], orm[80];
    j_str(one, "authorPub", author, sizeof(author));
    j_str(one, "op", op, sizeof(op));
    j_str(one, "roomId", orm, sizeof(orm));
    long amt = j_long(one, "amount", 0);
    if (room_has_authority(author, s_eq(orm, "*") ? MAIN_ROOM_ID : orm))
      pts += s_eq(op, "deduct") ? -amt : amt;
    q = end;
  }
  return pts;
}

int room_rep_level(const char *pub) {
  if (!pub || !pub[0]) return 1;
  long now = (long)hal_time_epoch();
  long since = now - REP_WINDOW_SEC;
  char sb[24]; u_ltoa(since, sb);
  char p[160]; params_of(p, sizeof(p), pub, sb, 0, 0);
  char r[64]; long msgs = 0;
  if (db_query("SELECT COUNT(*) AS v FROM msgs WHERE author=? AND ts>=?", p, r, sizeof(r)) > 2)
    msgs = j_long(r, "v", 0);
  long score = REP_W_MSG * msgs + net_points(pub);
  if (score < 0) score = 0;
  int level = 1;
  for (int i = 0; i < 9; i++) if (score >= REP_THRESH[i]) level = i + 2;
  if (level > 10) level = 10;
  return level;
}

/* ── ingest ── */

static void mods_replace(const char *roomId, const char *ev) {
  /* clear then re-insert every p-tag marked "moderator" */
  char p[128]; params_of(p, sizeof(p), roomId, 0, 0, 0);
  db_exec("DELETE FROM room_mods WHERE roomId=?", p);
  const char *cur = tags_start(ev);
  if (!cur) return;
  const char *t = cur;
  while (*t && *t != ']') {
    while (*t == ' ' || *t == ',') t++;
    if (*t != '[') break;
    t++;
    char name[32], v1[80], v2[80], v3[32];
    name[0] = v1[0] = v2[0] = v3[0] = 0;
    next_str(&t, name, sizeof(name));
    next_str(&t, v1, sizeof(v1));
    next_str(&t, v2, sizeof(v2));
    next_str(&t, v3, sizeof(v3));
    if (s_eq(name, "p") && v1[0] && s_eq(v3, "moderator")) {
      char pp[160]; params_of(pp, sizeof(pp), roomId, v1, 0, 0);
      db_exec("INSERT OR IGNORE INTO room_mods(roomId,pub) VALUES(?,?)", pp);
    }
    while (*t && *t != ']' && *t != '[') t++;
    if (*t == ']') t++;
  }
}

static int room_publish_def(const char *parentId, const char *name,
                            const char *approverPub, const char *approvalId);

int room_ingest(const char *ev) {
  ensure_self();
  long kind = j_long(ev, "kind", -1);
  char id[80], author[65];
  j_str(ev, "id", id, sizeof(id));
  j_str(ev, "pubkey", author, sizeof(author));
  long ts = j_long(ev, "created_at", 0);

  if (kind == KIND_ROOM_DEF) {
    char roomId[80], name[80], desc[200], parentAddr[160], access[16], approvedId[80];
    if (!tag_get(ev, "d", 1, roomId, sizeof(roomId)) || !roomId[0]) return 1;
    tag_get(ev, "name", 1, name, sizeof(name));
    tag_get(ev, "description", 1, desc, sizeof(desc));
    tag_get(ev, "access", 1, access, sizeof(access));
    tag_get(ev, "approved", 1, approvedId, sizeof(approvedId));
    char parent[80]; parent[0] = 0;
    if (tag_get(ev, "a", 1, parentAddr, sizeof(parentAddr))) {
      /* "34550:<pub>:<parentId>" -> parentId */
      int colon = 0; const char *s = parentAddr;
      for (; *s; s++) { if (*s == ':') { colon++; if (colon == 2) { s_cpy(parent, s + 1, sizeof(parent)); break; } } }
    }
    /* Approval gate: a root room is always valid; a sub-room is valid if its
     * author has authority over the parent, or it carries a 9080 approval signed
     * by a parent authority. Otherwise it stays hidden until such an approval
     * arrives (see the 9080 branch, which re-verifies). */
    int valid = 1;
    if (parent[0] && !room_has_authority(author, parent)) {
      valid = 0;
      if (approvedId[0]) {
        char ap[65] = ""; char qp[128]; params_of(qp, sizeof(qp), approvedId, 0, 0, 0);
        char r[256];
        if (db_query("SELECT approverPub AS v FROM approvals WHERE id=? LIMIT 1", qp, r, sizeof(r)) > 2)
          j_str(r, "v", ap, sizeof(ap));
        if (ap[0] && room_has_authority(ap, parent)) valid = 1;
      }
    }
    char tsb[24]; u_ltoa(ts, tsb);
    char verb[2]; verb[0] = valid ? '1' : '0'; verb[1] = 0;
    /* REPLACE fields but do NOT reset moderation `closed` on update. */
    char sql[460] =
      "INSERT INTO rooms(roomId,adminPub,name,description,parentRoomId,access,approvedBy,closed,verified,createdTs)"
      " VALUES(?,?,?,?,?,?,?,0,?,?) ON CONFLICT(roomId) DO UPDATE SET "
      "adminPub=excluded.adminPub,name=excluded.name,description=excluded.description,"
      "parentRoomId=excluded.parentRoomId,access=excluded.access,"
      "approvedBy=excluded.approvedBy,verified=excluded.verified";
    char p[900]; p[0] = 0; s_cat(p, "[", sizeof(p));
    const char *vals[8] = {roomId, author, name, desc, parent,
                           access[0] ? access : "open", approvedId, verb};
    for (int i = 0; i < 8; i++) { if (i) s_cat(p, ",", sizeof(p));
      s_cat(p, "\"", sizeof(p)); jesc(p, sizeof(p), vals[i]); s_cat(p, "\"", sizeof(p)); }
    /* createdTs is an integer, append it raw */
    s_cat(p, ",", sizeof(p)); s_cat(p, tsb, sizeof(p));
    s_cat(p, "]", sizeof(p));
    db_exec(sql, p);
    mods_replace(roomId, ev);
    return valid ? 2 : 1; /* only refresh the rail when the room is visible */
  }

  if (kind == KIND_ROOM_OP) {
    if (!id[0]) return 1;
    char roomId[80], target[65], op[16], reason[200], amt[16], until[16];
    tag_get(ev, "h", 1, roomId, sizeof(roomId));
    tag_get(ev, "p", 1, target, sizeof(target));
    tag_get(ev, "op", 1, op, sizeof(op));
    tag_get(ev, "amount", 1, amt, sizeof(amt));
    tag_get(ev, "until", 1, until, sizeof(until));
    j_str(ev, "content", reason, sizeof(reason));
    char tsb[24]; u_ltoa(ts, tsb);
    char p[900]; p[0] = 0; s_cat(p, "[", sizeof(p));
    const char *vals[9] = {id, roomId, author, target, op,
                           amt[0] ? amt : "0", until[0] ? until : "0", reason, tsb};
    for (int i = 0; i < 9; i++) { if (i) s_cat(p, ",", sizeof(p));
      s_cat(p, "\"", sizeof(p)); jesc(p, sizeof(p), vals[i]); s_cat(p, "\"", sizeof(p)); }
    s_cat(p, "]", sizeof(p));
    db_exec("INSERT OR IGNORE INTO ops"
            "(id,roomId,authorPub,targetPub,op,amount,until,reason,ts) "
            "VALUES(?,?,?,?,?,?,?,?,?)", p);
    /* A close op from an authority marks the room (and it drops out of the
     * tree, which filters closed=0). Reopen if a later 'unclose' arrives. */
    if (s_eq(op, "close") && roomId[0] && room_has_authority(author, roomId)) {
      char cp[128]; params_of(cp, sizeof(cp), roomId, 0, 0, 0);
      db_exec("UPDATE rooms SET closed=1 WHERE roomId=?", cp);
    }
    return 1;
  }

  if (kind == KIND_ROOM_PROPOSAL) {
    if (!id[0]) return 1;
    char parent[80], name[80], desc[200];
    tag_get(ev, "h", 1, parent, sizeof(parent));
    tag_get(ev, "name", 1, name, sizeof(name));
    tag_get(ev, "description", 1, desc, sizeof(desc));
    if (!parent[0] || !name[0]) return 1;
    char tsb[24]; u_ltoa(ts, tsb);
    char p[500]; p[0] = 0; s_cat(p, "[", sizeof(p));
    const char *vals[5] = {id, parent, author, name, desc};
    for (int i = 0; i < 5; i++) { if (i) s_cat(p, ",", sizeof(p));
      s_cat(p, "\"", sizeof(p)); jesc(p, sizeof(p), vals[i]); s_cat(p, "\"", sizeof(p)); }
    s_cat(p, ",", sizeof(p)); s_cat(p, tsb, sizeof(p));
    s_cat(p, "]", sizeof(p));
    db_exec("INSERT OR IGNORE INTO proposals"
            "(id,parentRoomId,proposerPub,name,description,ts,status) "
            "VALUES(?,?,?,?,?,?,'pending')", p);
    /* if this user can act on it, tell the caller to surface an approval prompt */
    if (room_has_authority(g_self, parent)) return 3;
    return 1;
  }

  if (kind == KIND_ROOM_APPROVAL) {
    if (!id[0]) return 1;
    char proposalId[80], parent[80];
    tag_get(ev, "e", 1, proposalId, sizeof(proposalId));
    tag_get(ev, "h", 1, parent, sizeof(parent));
    if (!proposalId[0]) return 1;
    char tsb[24]; u_ltoa(ts, tsb);
    { char p[400]; p[0] = 0; s_cat(p, "[", sizeof(p));
      const char *vals[3] = {id, proposalId, author};
      for (int i = 0; i < 3; i++) { if (i) s_cat(p, ",", sizeof(p));
        s_cat(p, "\"", sizeof(p)); jesc(p, sizeof(p), vals[i]); s_cat(p, "\"", sizeof(p)); }
      s_cat(p, ",", sizeof(p)); s_cat(p, tsb, sizeof(p)); s_cat(p, "]", sizeof(p));
      db_exec("INSERT OR IGNORE INTO approvals(id,proposalId,approverPub,ts) "
              "VALUES(?,?,?,?)", p); }
    /* honour only if the approver has authority over the proposal's parent */
    char proposer[65] = "", pname[80] = "", pparent[80] = "", pstat[16] = "";
    { char qp[128]; params_of(qp, sizeof(qp), proposalId, 0, 0, 0); char r[512];
      if (db_query("SELECT proposerPub,name,parentRoomId,status FROM proposals "
                   "WHERE id=? LIMIT 1", qp, r, sizeof(r)) > 2) {
        j_str(r, "proposerPub", proposer, sizeof(proposer));
        j_str(r, "name", pname, sizeof(pname));
        j_str(r, "parentRoomId", pparent, sizeof(pparent));
        j_str(r, "status", pstat, sizeof(pstat)); } }
    /* The parent may be named on the approval (h) or only known via the proposal
     * (a node might hold one but not the other). */
    const char *par = parent[0] ? parent : pparent;
    if (!par[0] || !room_has_authority(author, par)) return 1;
    /* Un-hide any sub-room published carrying this approval id. This must NOT
     * depend on holding the 9079 proposal: a node can receive the room def and
     * the approval without ever seeing the request. */
    { char up[128]; params_of(up, sizeof(up), id, 0, 0, 0);
      db_exec("UPDATE rooms SET verified=1 WHERE approvedBy=?", up); }
    /* Proposal-dependent steps: mark it approved, and if I am the proposer,
     * publish the sub-room now (self=admin, approver=mod). Guard on the pending
     * status so a re-delivered approval doesn't publish the room twice. */
    int changed = 0;
    if (s_eq(pstat, "pending")) {
      changed = 1;
      { char up[128]; params_of(up, sizeof(up), proposalId, 0, 0, 0);
        db_exec("UPDATE proposals SET status='approved' WHERE id=?", up); }
      if (proposer[0] && pname[0] && room_is_self(proposer)) {
        room_publish_def(pparent[0] ? pparent : par, pname, author, id);
        char up[128]; params_of(up, sizeof(up), proposalId, 0, 0, 0);
        db_exec("UPDATE proposals SET status='published' WHERE id=?", up);
      }
    }
    /* 2 means SOMETHING CHANGED, and the caller answers it by re-subscribing.
     * Returning it for a re-delivered approval — one already applied — closed a
     * loop that cost a phone its whole CPU: the room filter carries limit:500
     * and no `since`, so each new subscription replays hundreds of stored
     * events, one of which is this approval, which asked for another
     * subscription, ~20 times a minute forever. An event that taught us nothing
     * is consumed quietly. */
    return changed ? 2 : 1;
  }
  return 0;
}

/* A kind-1 room message: record participation, and hand the roomId back so the
 * caller can render it through the normal conversation pipeline. Returns 1 if it
 * was a room message. */
int room_note_roomid(const char *ev, char *out, unsigned cap) {
  out[0] = 0;
  char roomId[80];
  if (!tag_get(ev, "h", 1, roomId, sizeof(roomId)) || !roomId[0]) return 0;
  if (!room_is_room(roomId)) return 0;
  char id[80], author[65]; j_str(ev, "id", id, sizeof(id));
  j_str(ev, "pubkey", author, sizeof(author));
  long ts = j_long(ev, "created_at", 0);
  /* Dedup: if we already recorded this event, it is a re-delivery — do not
   * render it a second time. */
  { char dp[128]; params_of(dp, sizeof(dp), id, 0, 0, 0);
    if (db_exists("SELECT 1 FROM msgs WHERE id=? LIMIT 1", dp)) return 0; }
  char tsb[24]; u_ltoa(ts, tsb);
  char p[400]; p[0] = 0; s_cat(p, "[", sizeof(p));
  const char *vals[4] = {id, roomId, author, tsb};
  for (int i = 0; i < 4; i++) { if (i) s_cat(p, ",", sizeof(p));
    s_cat(p, "\"", sizeof(p)); jesc(p, sizeof(p), vals[i]); s_cat(p, "\"", sizeof(p)); }
  s_cat(p, "]", sizeof(p));
  db_exec("INSERT OR IGNORE INTO msgs(id,roomId,author,ts) VALUES(?,?,?,?)", p);
  s_cpy(out, roomId, cap);
  return 1;
}

/* ── publishing ── */

int room_post(const char *roomId, const char *text) {
  if (!room_is_room(roomId) || !text || !text[0]) return 0;
  char admin[65]; room_field(roomId, "adminPub", admin, sizeof(admin));
  if (!admin[0]) return 0;
  /* tags: [["a","34550:<admin>:<roomId>"],["h","<roomId>"]] */
  char tags[400] = "[[\"a\",\"34550:";
  s_cat(tags, admin, sizeof(tags));
  s_cat(tags, ":", sizeof(tags));
  s_cat(tags, roomId, sizeof(tags));
  s_cat(tags, "\"],[\"h\",\"", sizeof(tags));
  s_cat(tags, roomId, sizeof(tags));
  s_cat(tags, "\"]]", sizeof(tags));
  hal_nostr_post(KIND_ROOM_MSG, text, s_len(text), tags, s_len(tags));
  return 1;
}

/* Publish a NIP-25 reaction on room message [mid]: "+" likes, "-" retracts.
 * Tagged with the room (#h) so it rides the same subscription every member
 * already holds, and with the target (#e) so any client can tally it. */
int room_react(const char *roomId, const char *mid, int remove) {
  if (!room_is_room(roomId) || !mid || !mid[0]) return 0;
  char tags[300] = "[[\"e\",\"";
  s_cat(tags, mid, sizeof(tags));
  s_cat(tags, "\"],[\"h\",\"", sizeof(tags));
  s_cat(tags, roomId, sizeof(tags));
  s_cat(tags, "\"]]", sizeof(tags));
  const char *content = remove ? "-" : "+";
  hal_nostr_post(7, content, s_len(content), tags, s_len(tags));
  return 1;
}

int room_moderate(const char *roomId, const char *op, const char *target_pub,
                  long until, int amount, const char *reason) {
  if (!room_is_room(roomId) || !op || !op[0]) return 0;
  if (!room_self_authority(roomId)) return 0;
  char amtb[16]; u_ltoa(amount, amtb);
  char untb[24]; u_ltoa(until, untb);
  char tags[600] = "[[\"h\",\"";
  s_cat(tags, roomId, sizeof(tags));
  s_cat(tags, "\"],[\"op\",\"", sizeof(tags));
  s_cat(tags, op, sizeof(tags));
  s_cat(tags, "\"]", sizeof(tags));
  if (target_pub && target_pub[0]) {
    s_cat(tags, ",[\"p\",\"", sizeof(tags)); s_cat(tags, target_pub, sizeof(tags)); s_cat(tags, "\"]", sizeof(tags));
  }
  if (until > 0) { s_cat(tags, ",[\"until\",\"", sizeof(tags)); s_cat(tags, untb, sizeof(tags)); s_cat(tags, "\"]", sizeof(tags)); }
  if (amount != 0) { s_cat(tags, ",[\"amount\",\"", sizeof(tags)); s_cat(tags, amtb, sizeof(tags)); s_cat(tags, "\"]", sizeof(tags)); }
  s_cat(tags, ",[\"client\",\"xprs-chat\"]]", sizeof(tags));
  const char *r = (reason && reason[0]) ? reason : "";
  hal_nostr_post(KIND_ROOM_OP, r, s_len(r), tags, s_len(tags));
  return 1;
}

/* May the current user post in [roomId] right now? 0 if the room is closed or
 * self is banned / still suspended (soft gating the client enforces). */
int room_self_can_post(const char *roomId) {
  char cl[8]; room_field(roomId, "closed", cl, sizeof(cl));
  if (cl[0] == '1') return 0;
  ensure_self();
  if (!g_self[0]) return 1;
  long until = 0;
  int st = member_status(roomId, g_self, &until);
  if (st == 2) return 0;
  if (st == 1) { long now = (long)hal_time_epoch(); if (until == 0 || until > now) return 0; }
  return 1;
}

/* Is [pub] our own pubkey? (case-insensitive hex, refreshing g_self.) */
int room_is_self(const char *pub) {
  if (!pub || !pub[0]) return 0;
  ensure_self();
  if (!g_self[0]) return 0;
  unsigned i = 0;
  for (; pub[i] && g_self[i]; i++) {
    char a = pub[i], b = g_self[i];
    if (a >= 'A' && a <= 'Z') a += 32;
    if (b >= 'A' && b <= 'Z') b += 32;
    if (a != b) return 0;
  }
  return pub[i] == 0 && g_self[i] == 0;
}

/* Publish a NIP-72 34550 sub-room under [parentId] (empty = top level) with self
 * as admin. If [approverPub]/[approvalId] are set, the approver is added as a
 * moderator and the approval id is recorded (the proof this creation was
 * sanctioned). Returns 1. */
static int room_publish_def(const char *parentId, const char *name,
                            const char *approverPub, const char *approvalId) {
  ensure_self();
  if (!g_self[0] || !name || !name[0]) return 0;
  unsigned char rb[8]; hal_crypto_random((char *)rb, 8);
  static const char hx[] = "0123456789abcdef";
  char rid[20]; for (int i = 0; i < 8; i++) { rid[i * 2] = hx[rb[i] >> 4]; rid[i * 2 + 1] = hx[rb[i] & 15]; }
  rid[16] = 0;
  char tags[700] = "[[\"d\",\"";
  s_cat(tags, rid, sizeof(tags));
  s_cat(tags, "\"],[\"name\",\"", sizeof(tags));
  jesc(tags, sizeof(tags), name);
  s_cat(tags, "\"],[\"access\",\"open\"]", sizeof(tags));
  if (parentId && parentId[0]) {
    char padmin[65]; room_field(parentId, "adminPub", padmin, sizeof(padmin));
    if (padmin[0]) {
      s_cat(tags, ",[\"a\",\"34550:", sizeof(tags));
      s_cat(tags, padmin, sizeof(tags)); s_cat(tags, ":", sizeof(tags));
      s_cat(tags, parentId, sizeof(tags)); s_cat(tags, "\"]", sizeof(tags));
    }
  }
  if (approverPub && approverPub[0]) {
    s_cat(tags, ",[\"p\",\"", sizeof(tags)); s_cat(tags, approverPub, sizeof(tags));
    s_cat(tags, "\",\"\",\"moderator\"]", sizeof(tags));
  }
  if (approvalId && approvalId[0]) {
    s_cat(tags, ",[\"approved\",\"", sizeof(tags)); s_cat(tags, approvalId, sizeof(tags));
    s_cat(tags, "\"]", sizeof(tags));
  }
  s_cat(tags, "]", sizeof(tags));
  hal_nostr_post(KIND_ROOM_DEF, "", 0, tags, s_len(tags));
  return 1;
}

int room_create(const char *parentId, const char *name) {
  return room_publish_def(parentId, name, 0, 0);
}

/* Propose a sub-room. An authority over the parent creates it directly; anyone
 * else publishes a proposal (9079) for an authority to approve. */
int room_propose(const char *parentId, const char *name) {
  ensure_self();
  if (!g_self[0] || !name || !name[0]) return 0;
  const char *parent = (parentId && parentId[0]) ? parentId : MAIN_ROOM_ID;
  if (room_has_authority(g_self, parent)) return room_create(parent, name);
  char tags[400] = "[[\"h\",\"";
  s_cat(tags, parent, sizeof(tags));
  s_cat(tags, "\"],[\"name\",\"", sizeof(tags));
  jesc(tags, sizeof(tags), name);
  s_cat(tags, "\"],[\"client\",\"xprs-chat\"]]", sizeof(tags));
  hal_nostr_post(KIND_ROOM_PROPOSAL, "", 0, tags, s_len(tags));
  return 1;
}

int room_approve(const char *proposalId) {
  ensure_self();
  if (!g_self[0] || !proposalId || !proposalId[0]) return 0;
  char parent[80] = "";
  { char p[128]; params_of(p, sizeof(p), proposalId, 0, 0, 0); char r[256];
    if (db_query("SELECT parentRoomId AS v FROM proposals WHERE id=? LIMIT 1", p, r, sizeof(r)) > 2)
      j_str(r, "v", parent, sizeof(parent)); }
  if (!parent[0] || !room_has_authority(g_self, parent)) return 0;
  char tags[300] = "[[\"e\",\"";
  s_cat(tags, proposalId, sizeof(tags));
  s_cat(tags, "\"],[\"h\",\"", sizeof(tags));
  s_cat(tags, parent, sizeof(tags));
  s_cat(tags, "\"],[\"client\",\"xprs-chat\"]]", sizeof(tags));
  hal_nostr_post(KIND_ROOM_APPROVAL, "", 0, tags, s_len(tags));
  return 1;
}

int room_newest_pending(char *id, unsigned idcap, char *name, unsigned namecap,
                        char *parent, unsigned pcap) {
  id[0] = name[0] = parent[0] = 0;
  char r[1024];
  if (db_query("SELECT id,name,parentRoomId FROM proposals WHERE status='pending' "
               "ORDER BY ts DESC LIMIT 1", 0, r, sizeof(r)) <= 2) return 0;
  j_str(r, "id", id, idcap);
  j_str(r, "name", name, namecap);
  j_str(r, "parentRoomId", parent, pcap);
  if (!id[0]) return 0;
  /* only offer proposals whose parent we can act on */
  return room_has_authority(g_self, parent);
}

/* Ban [target_pub] from the whole wapp (op h="*"); only a global authority may. */
int room_ban_wapp(const char *target_pub) {
  if (!target_pub || !target_pub[0]) return 0;
  ensure_self();
  if (!room_has_authority(g_self, MAIN_ROOM_ID)) return 0;
  char tags[400] = "[[\"h\",\"*\"],[\"op\",\"ban\"],[\"p\",\"";
  s_cat(tags, target_pub, sizeof(tags));
  s_cat(tags, "\"],[\"client\",\"xprs-chat\"]]", sizeof(tags));
  hal_nostr_post(KIND_ROOM_OP, "", 0, tags, s_len(tags));
  return 1;
}

/* ── subscription filter ── */

unsigned room_sub_filter(char *out, unsigned cap) {
  /* {"kinds":[34550,9078,9079,9080],"limit":500} plus kind-1 #h across rooms */
  char ids[1024]; ids[0] = 0;
  char r[4096];
  if (db_query("SELECT roomId AS v FROM rooms LIMIT 100", 0, r, sizeof(r)) > 2) {
    const char *q = r; int first = 1;
    for (;;) {
      const char *obj = 0; for (const char *s = q; *s; s++) if (*s == '{') { obj = s; break; }
      if (!obj) break;
      char rid[80]; j_str(obj, "v", rid, sizeof(rid));
      if (rid[0]) { if (!first) s_cat(ids, ",", sizeof(ids)); first = 0;
        s_cat(ids, "\"", sizeof(ids)); jesc(ids, sizeof(ids), rid); s_cat(ids, "\"", sizeof(ids)); }
      const char *e = obj; for (; *e && *e != '}'; e++) {} q = (*e == '}') ? e + 1 : obj + 1;
    }
  }
  out[0] = 0;
  s_cat(out, "[{\"kinds\":[34550,9078,9079,9080],\"limit\":500}", cap);
  /* kind 7: NIP-25 reactions on room messages (the heart tally). */
  if (ids[0]) { s_cat(out, ",{\"kinds\":[1,7],\"#h\":[", cap); s_cat(out, ids, cap);
                s_cat(out, "],\"limit\":200}", cap); }
  s_cat(out, "]", cap);
  return s_len(out);
}

/* ── rendering: the Discord-like rail (ui.rooms.set) ── */

static char g_rail[8192];
static int g_rail_first;
static void rail_add(const char *roomId, int depth) {
  char name[80]; room_field(roomId, "name", name, sizeof(name));
  if (!name[0]) s_cpy(name, roomId, sizeof(name));
  if (!g_rail_first) s_cat(g_rail, ",", sizeof(g_rail));
  g_rail_first = 0;
  s_cat(g_rail, "{\"id\":\"", sizeof(g_rail)); jesc(g_rail, sizeof(g_rail), roomId);
  s_cat(g_rail, "\",\"name\":\"", sizeof(g_rail)); jesc(g_rail, sizeof(g_rail), name);
  s_cat(g_rail, "\",\"depth\":", sizeof(g_rail)); cat_l(g_rail, depth, sizeof(g_rail));
  if (s_eq(roomId, MAIN_ROOM_ID)) s_cat(g_rail, ",\"selected\":true", sizeof(g_rail));
  s_cat(g_rail, "}", sizeof(g_rail));
}
static void rail_children(const char *parentId, int depth) {
  if (depth > 8) return;
  char p[128]; params_of(p, sizeof(p), parentId, 0, 0, 0);
  char r[4096];
  if (db_query("SELECT roomId AS v FROM rooms WHERE parentRoomId=? AND closed=0 "
               "AND verified=1 ORDER BY name LIMIT 100", p, r, sizeof(r)) <= 2) return;
  const char *q = r;
  for (;;) {
    const char *obj = 0; for (const char *s = q; *s; s++) if (*s == '{') { obj = s; break; }
    if (!obj) break;
    char rid[80]; j_str(obj, "v", rid, sizeof(rid));
    if (rid[0]) { rail_add(rid, depth); rail_children(rid, depth + 1); }
    const char *e = obj; for (; *e && *e != '}'; e++) {} q = (*e == '}') ? e + 1 : obj + 1;
  }
}
/* How many DISTINCT people we have seen post in [roomId].
 *
 * Not a membership count — a NIP-72 room has no roster: nobody publishes
 * "joined" or "left", so the only truthful number is how many different
 * authors this device has actually seen. Callers label it "seen", never
 * "members", so the number does not promise more than it knows. */
int room_people_seen(const char *roomId) {
  char p[128]; params_of(p, sizeof(p), roomId, 0, 0, 0);
  char r[128];
  if (db_query("SELECT COUNT(DISTINCT author) AS v FROM msgs WHERE roomId=?",
               p, r, sizeof(r)) <= 2)
    return 0;
  char v[24]; j_str(r, "v", v, sizeof(v));
  if (!v[0]) {
    /* db_query renders numbers unquoted; pull the first digit run. */
    const char *s = r;
    while (*s && (*s < '0' || *s > '9')) s++;
    int n = 0;
    while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); s++; }
    return n;
  }
  int n = 0;
  for (const char *s = v; *s >= '0' && *s <= '9'; s++) n = n * 10 + (*s - '0');
  return n;
}

void room_name_of(const char *roomId, char *out, unsigned cap) {
  room_field(roomId, "name", out, cap);
  if (!out[0]) s_cpy(out, roomId, cap);
}

/* Is [roomId] reachable by walking parents up to the main room? The rail
 * renders the main tree, so a room joined from search that hangs elsewhere
 * (or has no parent at all) would otherwise be openable and invisible. */
int room_on_main_tree(const char *roomId) {
  char cur[80]; s_cpy(cur, roomId, sizeof(cur));
  for (int depth = 0; depth < 12; depth++) {
    if (s_eq(cur, MAIN_ROOM_ID)) return 1;
    char parent[80];
    room_field(cur, "parentRoomId", parent, sizeof(parent));
    if (!parent[0]) return 0;
    s_cpy(cur, parent, sizeof(cur));
  }
  return 0;
}

/* Rooms whose name (or id) contains [q], case-insensitively — every room this
 * device knows, not only the ones already on the rail. Writes JSON items
 * {"id","name"} (no enclosing brackets) so the caller can drop them straight
 * into a people-list section. Returns the count. */
int room_search(const char *q, char *out, unsigned cap) {
  out[0] = 0;
  char r[8192];
  if (db_query("SELECT roomId AS v, name AS n FROM rooms WHERE closed=0 "
               "AND verified=1 ORDER BY name LIMIT 200", "[]", r, sizeof(r)) <= 2)
    return 0;
  /* Lowercase the needle once. */
  char needle[64]; unsigned qi = 0;
  for (; q[qi] && qi < sizeof(needle) - 1; qi++) {
    char c = q[qi];
    needle[qi] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
  }
  needle[qi] = 0;

  int n = 0, first = 1;
  const char *p = r;
  for (;;) {
    const char *obj = 0; for (const char *s = p; *s; s++) if (*s == '{') { obj = s; break; }
    if (!obj) break;
    char rid[80], name[80];
    j_str(obj, "v", rid, sizeof(rid));
    j_str(obj, "n", name, sizeof(name));
    const char *e = obj; for (; *e && *e != '}'; e++) {} p = (*e == '}') ? e + 1 : obj + 1;
    if (!rid[0]) continue;
    if (!name[0]) s_cpy(name, rid, sizeof(name));
    if (needle[0]) {
      /* case-insensitive substring over "name id" */
      char hay[170]; hay[0] = 0;
      s_cat(hay, name, sizeof(hay)); s_cat(hay, " ", sizeof(hay));
      s_cat(hay, rid, sizeof(hay));
      for (unsigned i = 0; hay[i]; i++)
        if (hay[i] >= 'A' && hay[i] <= 'Z') hay[i] = (char)(hay[i] + 32);
      unsigned nl = s_len(needle), hl = s_len(hay);
      int hit = 0;
      for (unsigned i = 0; nl && i + nl <= hl; i++) {
        unsigned k = 0;
        for (; k < nl; k++) if (hay[i + k] != needle[k]) break;
        if (k == nl) { hit = 1; break; }
      }
      if (!hit) continue;
    }
    if (!first) s_cat(out, ",", cap);
    first = 0;
    s_cat(out, "{\"id\":\"", cap); jesc(out, cap, rid);
    s_cat(out, "\",\"name\":\"", cap); jesc(out, cap, name);
    s_cat(out, "\"}", cap);
    n++;
  }
  return n;
}

void room_render_tree_with(const char *extra_items) {
  g_rail[0] = 0;
  s_cat(g_rail, "{\"type\":\"ui.rooms.set\",\"field\":\"rooms\",\"rooms\":[", sizeof(g_rail));
  g_rail_first = 1;
  rail_add(MAIN_ROOM_ID, 0);
  rail_children(MAIN_ROOM_ID, 1);
  /* Channels (broadcast groups, NomadNet, …) the caller wants on the same
   * rail, pre-built as JSON items — the rail is the whole navigation surface,
   * so anything not on it is a conversation the user can never reach. */
  if (extra_items && extra_items[0]) {
    if (!g_rail_first) s_cat(g_rail, ",", sizeof(g_rail));
    s_cat(g_rail, extra_items, sizeof(g_rail));
  }
  s_cat(g_rail, "]}", sizeof(g_rail));
  hal_msg_send(g_rail, s_len(g_rail));
}
void room_render_tree(void) { room_render_tree_with(0); }

/* Append one people-item {id,title,subtitle} for [pub] with role/status label. */
static void member_item(char *m, unsigned cap, const char *roomId, const char *pub,
                        const char *forced_role) {
  long until = 0;
  const char *label = forced_role;
  char stbuf[16];
  if (!label) {
    int st = member_status(roomId, pub, &until);
    label = (st == 2) ? "banned" : (st == 1) ? "suspended" : "member";
    (void)stbuf;
  }
  s_cat(m, "{\"id\":\"", cap); jesc(m, cap, pub);
  s_cat(m, "\",\"title\":\"", cap); s_cat(m, label, cap);
  s_cat(m, "\",\"subtitle\":\"level ", cap); cat_l(m, room_rep_level(pub), cap);
  s_cat(m, "\"}", cap);
}

/* Render the member roster of [roomId] (admin, mods, recent participants) with a
 * reputation-level badge, into the people-list field "room_members". */
static char g_members_buf[8192];
void room_render_members(const char *roomId) {
  char *m = g_members_buf; const unsigned sz = sizeof(g_members_buf);
  m[0] = 0;
  s_cat(m, "{\"type\":\"ui.people.set\",\"field\":\"room_members\","
           "\"sections\":[{\"title\":\"Members\",\"items\":[", sz);
  char seen[4096]; seen[0] = 0;
  int first = 1;
  char admin[65]; room_field(roomId, "adminPub", admin, sizeof(admin));
  if (admin[0]) { member_item(m, sz, roomId, admin, "admin"); first = 0;
                  s_cat(seen, ",", sizeof(seen)); s_cat(seen, admin, sizeof(seen)); s_cat(seen, ",", sizeof(seen)); }
  char rows[4096];
  char p[128]; params_of(p, sizeof(p), roomId, 0, 0, 0);
  if (db_query("SELECT DISTINCT author AS v FROM msgs WHERE roomId=? ORDER BY ts DESC LIMIT 80",
               p, rows, sizeof(rows)) > 2) {
    const char *q = rows;
    for (;;) {
      const char *obj = 0; for (const char *s = q; *s; s++) if (*s == '{') { obj = s; break; }
      if (!obj) break;
      char pub[65]; j_str(obj, "v", pub, sizeof(pub));
      const char *e = obj; for (; *e && *e != '}'; e++) {} q = (*e == '}') ? e + 1 : obj + 1;
      if (!pub[0]) continue;
      /* dedup: look for ",<pub>," in seen */
      char needle[68] = ","; s_cat(needle, pub, sizeof(needle)); s_cat(needle, ",", sizeof(needle));
      int dup = 0; unsigned nl = s_len(needle), sl = s_len(seen);
      for (unsigned i = 0; nl && i + nl <= sl; i++) { unsigned k = 0; for (; k < nl; k++) if (seen[i + k] != needle[k]) break; if (k == nl) { dup = 1; break; } }
      if (dup) continue;
      s_cat(seen, pub, sizeof(seen)); s_cat(seen, ",", sizeof(seen));
      if (!first) s_cat(m, ",", sz); first = 0;
      member_item(m, sz, roomId, pub, 0);
    }
  }
  s_cat(m, "]}]}", sz);
  hal_msg_send(m, s_len(m));
}

