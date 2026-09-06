#include "room.h"
#include "db.h"
#include "xprs.h"
#include "xprs_wasm_hal.h"

#define LOCAL "#LOCAL"
#define BODY_MAX 900
#define TAIL 50
#define ROOM_H_MAX 8

static char g_self[16] = "";
static char g_open[48] = "";     /* the conversation on screen, or "" */
static char g_top[48] = "";      /* first row of the rail as last drawn */
static int  g_local_on = 1;
static int  g_idx = -1;
static int  g_said_nodb = 0;
static struct { char id[48]; int h; unsigned long long used; } g_rh[ROOM_H_MAX];
static unsigned long long g_tick;

/* One reply buffer for every query here: 50 rows of a 900-byte body with
 * JSON escaping is ~57 KB, so 64. Sized for the worst legitimate input, not
 * the median (docs/performance.md 8.9). */
static char g_q[65536];

void log1(const char *line) { hal_log(1, line, s_len(line)); }

/* ── ids ──────────────────────────────────────────────────────────── */
static void room_flush_reads(int h);

static int is_xgroup(const char *id) {
  return id[0] == '#' && id[1] == 'X' && id[2] == '5' && s_len(id) == 7;
}
int room_renderable(const char *id) {
  if (!id || !id[0]) return 0;
  if (id[0] == '#') return id[1] != 0;
  return xprs_is_station(id);
}
static const char *room_icon(const char *id) {
  if (s_eq(id, LOCAL)) return "campaign";
  if (is_xgroup(id)) return "group";
  if (id[0] == '#') return "tag";
  return "person";
}
/* Injective, never reversed, never renamed: an encrypted profile derives the
 * database key from the relative path, so a renamed file is unreadable. */
static int room_file(const char *id, char *out, unsigned cap) {
  s_cpy(out, "rooms/", cap);
  unsigned l = s_len(out);
  for (const char *p = id; *p; p++) {
    char c = *p;
    const char *rep = 0;
    if (c == '#') rep = "_h";
    else if (c == '*') rep = "_s";
    else if (c == '-') rep = "_d";
    else if (c == '_') rep = "__";
    else if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9'))) return 0;
    if (l + 3 >= cap) return 0;
    if (rep) { out[l++] = rep[0]; out[l++] = rep[1]; }
    else out[l++] = c;
    out[l] = 0;
  }
  s_cat(out, ".sqlite3", cap);
  return 1;
}

/* ── handles ──────────────────────────────────────────────────────── */
static int room_handle(const char *id) {
  int free_i = -1, lru = 0;
  for (int i = 0; i < ROOM_H_MAX; i++) {
    if (g_rh[i].h >= 0 && s_eq(g_rh[i].id, id)) { g_rh[i].used = ++g_tick; return g_rh[i].h; }
    if (g_rh[i].h < 0) { if (free_i < 0) free_i = i; }
    else if (g_rh[i].used < g_rh[lru].used) lru = i;
  }
  char file[80];
  if (!room_file(id, file, sizeof(file))) return -1;
  int h = db_open(file);
  if (h < 0) return -1;
  db_init_room(h);
  int slot = free_i >= 0 ? free_i : lru;
  if (free_i < 0) db_close(g_rh[slot].h);
  s_cpy(g_rh[slot].id, id, sizeof(g_rh[slot].id));
  g_rh[slot].h = h;
  g_rh[slot].used = ++g_tick;
  return h;
}

/* ── time ─────────────────────────────────────────────────────────── */
static int g_tz_min, g_tz_known;
void fmt_time_at(char *b, unsigned long long e) {
  if (!g_tz_known) { g_tz_min = hal_time_utc_offset(); g_tz_known = 1; }
  long long secs = (long long)e + (long long)g_tz_min * 60;
  long long day = secs % 86400;
  if (day < 0) day += 86400;
  int hh = (int)(day / 3600), mm = (int)((day / 60) % 60);
  b[0] = (char)('0' + hh / 10); b[1] = (char)('0' + hh % 10); b[2] = ':';
  b[3] = (char)('0' + mm / 10); b[4] = (char)('0' + mm % 10); b[5] = 0;
}

/* ── the view (ui.* out) ──────────────────────────────────────────── */
static const char *via_label(const char *via) {
  if (!via || !via[0]) return "";
  if (s_eq(via, "ble"))     return "BLE";
  if (s_eq(via, "lan"))     return "LAN";
  if (s_eq(via, "espnow"))  return "ESP-NOW";
  if (s_eq(via, "lora"))    return "LoRa";
  if (s_eq(via, "wifi"))    return "WiFi";
  if (s_eq(via, "vhf"))     return "VHF";
  if (s_eq(via, "uhf"))     return "UHF";
  if (s_eq(via, "hf"))      return "HF";
  if (s_eq(via, "rns"))     return "Reticulum";
  if (s_eq(via, "custody")) return "Carried";
  return via;
}

/* One row of the index as the host's list row. */
static void emit_upsert(const char *id, int bump, int select) {
  pj_t p; pj_init(&p); pj_str(&p, id);
  char row[400];
  if (db_query(g_idx, "SELECT title,unread,private,last_line FROM rooms WHERE id=? LIMIT 1",
               pj_done(&p), row, sizeof(row)) <= 0) return;
  char title[80], last[140];
  jstr(row, "title", title, sizeof(title));
  jstr(row, "last_line", last, sizeof(last));
  char m[700] = "{\"type\":\"ui.convo.upsert\",\"id\":\"";
  jesc(m, sizeof(m), id);
  s_cat(m, "\",\"title\":\"", sizeof(m)); jesc(m, sizeof(m), title);
  s_cat(m, "\",\"subtitle\":\"", sizeof(m)); jesc(m, sizeof(m), last);
  s_cat(m, "\",\"icon\":\"", sizeof(m)); s_cat(m, room_icon(id), sizeof(m));
  s_cat(m, "\",\"unread\":", sizeof(m));
  { char nb[16]; u_lltoa((unsigned long long)jint(row, "unread"), nb); s_cat(m, nb, sizeof(m)); }
  s_cat(m, ",\"closed\":false,\"private\":", sizeof(m));
  s_cat(m, jint(row, "private") ? "true" : "false", sizeof(m));
  if (bump) s_cat(m, ",\"bump\":true", sizeof(m));
  if (select) s_cat(m, ",\"select\":true", sizeof(m));
  s_cat(m, "}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* One message as a bubble. Always backfill:true -- the host stores and draws,
 * and nothing else: unread, order and activity are this unit's, stated on the
 * row's upsert. */
static void emit_msg(const char *id, const char *mid, const char *dir,
                     const char *sender, unsigned long long ts, const char *body,
                     const char *parent, const char *via, const char *auth,
                     int enc, const char *rid, const char *status, int sys,
                     int priv) {
  char t[8]; fmt_time_at(t, ts);
  /* 1400: a 900-byte body doubles under escaping in the worst case, and the
   * old 640 silently truncated the JSON, which the host then refused. */
  static char m[1400];
  s_cpy(m, "{\"type\":\"ui.convo.msg\",\"id\":\"", sizeof(m));
  jesc(m, sizeof(m), id);
  s_cat(m, "\",\"dir\":\"", sizeof(m)); s_cat(m, dir, sizeof(m));
  s_cat(m, "\",\"from\":\"", sizeof(m)); jesc(m, sizeof(m), sender);
  s_cat(m, "\",\"text\":\"", sizeof(m)); jesc(m, sizeof(m), body);
  s_cat(m, "\",\"time\":\"", sizeof(m)); s_cat(m, t, sizeof(m));
  s_cat(m, "\",\"key\":\"", sizeof(m)); jesc(m, sizeof(m), mid);
  s_cat(m, "\",\"mid\":\"", sizeof(m)); jesc(m, sizeof(m), mid);
  s_cat(m, "\"", sizeof(m));
  if (parent && parent[0]) { s_cat(m, ",\"parent\":\"", sizeof(m)); jesc(m, sizeof(m), parent); s_cat(m, "\"", sizeof(m)); }
  if (via && via[0]) { s_cat(m, ",\"via\":\"", sizeof(m)); jesc(m, sizeof(m), via_label(via)); s_cat(m, "\"", sizeof(m)); }
  if (auth && auth[0]) { s_cat(m, ",\"auth\":\"", sizeof(m)); jesc(m, sizeof(m), auth); s_cat(m, "\"", sizeof(m)); }
  if (rid && rid[0]) { s_cat(m, ",\"rid\":\"", sizeof(m)); jesc(m, sizeof(m), rid); s_cat(m, "\"", sizeof(m)); }
  if (status && status[0]) { s_cat(m, ",\"status\":\"", sizeof(m)); jesc(m, sizeof(m), status); s_cat(m, "\"", sizeof(m)); }
  if (enc) s_cat(m, ",\"enc\":true", sizeof(m));
  else if (id[0] != '#' && !sys) s_cat(m, ",\"plain\":true", sizeof(m));
  if (priv) s_cat(m, ",\"private\":true", sizeof(m));
  if (sys) s_cat(m, ",\"sys\":true", sizeof(m));
  s_cat(m, ",\"backfill\":true}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

static void emit_react(const char *id, const char *mid, const char *who, int remove, int mine) {
  char m[260] = "{\"type\":\"ui.convo.react\",\"id\":\"";
  jesc(m, sizeof(m), id);
  s_cat(m, "\",\"mid\":\"", sizeof(m)); jesc(m, sizeof(m), mid);
  s_cat(m, "\",\"from\":\"", sizeof(m)); jesc(m, sizeof(m), who);
  s_cat(m, "\"", sizeof(m));
  if (remove) s_cat(m, ",\"remove\":true", sizeof(m));
  if (mine) s_cat(m, ",\"mine\":true", sizeof(m));
  s_cat(m, "}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

static void emit_remove(const char *id, const char *key) {
  char m[200] = "{\"type\":\"ui.convo.remove\",\"id\":\"";
  jesc(m, sizeof(m), id);
  if (key && key[0]) { s_cat(m, "\",\"key\":\"", sizeof(m)); jesc(m, sizeof(m), key); }
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* The launcher badge: one number, asked of the database as a number. */
static void unread_publish(void) {
  long long n = db_int(g_idx,
      g_local_on ? "SELECT COALESCE(SUM(unread),0) AS n FROM rooms WHERE closed=0"
                 : "SELECT COALESCE(SUM(unread),0) AS n FROM rooms WHERE closed=0 AND id<>'#LOCAL'",
      0, 0);
  char m[96] = "{\"type\":\"unread\",\"intent\":\"chat\",\"count\":";
  char nb[24]; u_lltoa((unsigned long long)n, nb);
  s_cat(m, nb, sizeof(m)); s_cat(m, "}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* A person cannot see this message: the app is in the background or they
 * are in another room. The host dedups on the tag across restarts, so the
 * room and the id are the tag. */
static void notify_msg(const char *room, const char *from, const char *body, const char *mid) {
  char m[600] = "{\"type\":\"notify\",\"level\":\"info\",\"title\":\"";
  jesc(m, sizeof(m), room[0] == '#' ? room : from);
  s_cat(m, "\",\"body\":\"", sizeof(m));
  if (room[0] == '#') { jesc(m, sizeof(m), from); s_cat(m, ": ", sizeof(m)); }
  { char short_body[160]; s_cpy(short_body, body, sizeof(short_body)); jesc(m, sizeof(m), short_body); }
  s_cat(m, "\",\"convo\":\"", sizeof(m)); jesc(m, sizeof(m), room);
  s_cat(m, "\",\"tag\":\"chat:", sizeof(m)); jesc(m, sizeof(m), room);
  s_cat(m, ":", sizeof(m)); jesc(m, sizeof(m), mid);
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

void room_rail(void) {
  static char rail[8192];
  s_cpy(rail, "{\"type\":\"ui.rooms.set\",\"field\":\"rooms\",\"rooms\":[", sizeof(rail));
  int n = db_query(g_idx, "SELECT id,title,activity_ts FROM rooms WHERE closed=0 "
                          "ORDER BY activity_ts DESC, id LIMIT 64", 0, g_q, sizeof(g_q));
  char lg[900] = "[chat] rail:";
  int first = 1;
  g_top[0] = 0;
  if (n > 0) {
    const char *cur = g_q; char row[400];
    while (next_object(&cur, row, sizeof(row))) {
      char id[48], title[80];
      jstr(row, "id", id, sizeof(id));
      jstr(row, "title", title, sizeof(title));
      if (!g_local_on && s_eq(id, LOCAL)) continue;
      if (!first) s_cat(rail, ",", sizeof(rail));
      if (first) s_cpy(g_top, id, sizeof(g_top));
      first = 0;
      s_cat(rail, "{\"id\":\"", sizeof(rail)); jesc(rail, sizeof(rail), id);
      s_cat(rail, "\",\"name\":\"", sizeof(rail)); jesc(rail, sizeof(rail), title);
      s_cat(rail, "\",\"depth\":0,\"seen\":", sizeof(rail));
      { char nb[24]; u_lltoa((unsigned long long)jint(row, "activity_ts"), nb); s_cat(rail, nb, sizeof(rail)); }
      s_cat(rail, "}", sizeof(rail));
      s_cat(lg, " ", sizeof(lg)); s_cat(lg, id, sizeof(lg));
    }
  }
  s_cat(rail, "]}", sizeof(rail));
  hal_msg_send(rail, s_len(rail));
  /* Once per redraw, and a redraw is a set change, not a message. */
  log1(lg);
}

void blocked_publish(void) {
  char m[1200] = "{\"type\":\"ui.convo.blocked\",\"from\":[";
  int n = db_query(g_idx, "SELECT call FROM blocked LIMIT 64", 0, g_q, sizeof(g_q));
  int first = 1;
  if (n > 0) {
    const char *cur = g_q; char row[64];
    while (next_object(&cur, row, sizeof(row))) {
      char c[16]; jstr(row, "call", c, sizeof(c));
      if (!first) s_cat(m, ",", sizeof(m));
      first = 0;
      s_cat(m, "\"", sizeof(m)); jesc(m, sizeof(m), c); s_cat(m, "\"", sizeof(m));
    }
  }
  s_cat(m, "]}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

void room_hydrate(void) {
  room_rail();
  int n = db_query(g_idx, "SELECT id FROM rooms WHERE closed=0 ORDER BY activity_ts DESC LIMIT 64",
                   0, g_q, sizeof(g_q));
  if (n > 0) {
    /* emit_upsert queries the index too, so walk a copy of the id list. */
    static char ids[64][48]; int k = 0;
    const char *cur = g_q; char row[120];
    while (k < 64 && next_object(&cur, row, sizeof(row))) jstr(row, "id", ids[k++], 48);
    for (int i = 0; i < k; i++) emit_upsert(ids[i], 0, 0);
  }
  blocked_publish();
  unread_publish();
}

void room_set_local_enabled(int on) {
  g_local_on = on ? 1 : 0;
  if (g_idx < 0) return;
  room_rail();
  unread_publish();
}

/* ── the index ────────────────────────────────────────────────────── */
int room_known(const char *id) {
  pj_t p; pj_init(&p); pj_str(&p, id);
  return db_int(g_idx, "SELECT 1 AS n FROM rooms WHERE id=? LIMIT 1", pj_done(&p), 0) == 1;
}

int room_ensure(const char *id, const char *title) {
  if (g_idx < 0 || !room_renderable(id)) return -1;
  char file[80];
  if (!room_file(id, file, sizeof(file))) return -1;
  pj_t p; pj_init(&p); pj_str(&p, id); pj_str(&p, file);
  pj_str(&p, (title && title[0]) ? title : id);
  if (db_exec(g_idx, "INSERT OR IGNORE INTO rooms(id,file,title) VALUES(?,?,?)", pj_done(&p)) != 0)
    return -1;
  return db_int(g_idx, "SELECT changes() AS n", 0, 0) > 0 ? 1 : 0;
}

void room_set_title(const char *id, const char *title) {
  if (!title || !title[0] || !room_known(id)) return;
  pj_t p; pj_init(&p); pj_str(&p, title); pj_str(&p, id);
  db_exec(g_idx, "UPDATE rooms SET title=? WHERE id=?", pj_done(&p));
  emit_upsert(id, 0, 0);
  room_rail();
}

/* ── THE DOOR ─────────────────────────────────────────────────────── */
int room_admit(const room_msg_t *m) {
  if (!m->room || !m->mid || !m->mid[0] || !m->body) return 0;
  if (!room_renderable(m->room)) return 0;
  int in = s_eq(m->dir, "in");
  if (in && !m->sys && is_blocked(m->sender)) return 0;
  if (g_idx < 0) {
    if (!g_said_nodb) { g_said_nodb = 1; log1("[chat] no index database -- nothing can be shown"); }
    return -1;
  }
  { pj_t p; pj_init(&p); pj_str(&p, m->mid);
    if (db_int(g_idx, "SELECT 1 AS n FROM hidden WHERE mid=? LIMIT 1", pj_done(&p), 0) == 1) return 0; }
  int h = room_handle(m->room);
  if (h < 0) {
    if (!g_said_nodb) { g_said_nodb = 1; log1("[chat] a room database would not open -- message dropped"); }
    return -1;
  }
  /* "Have I seen this" is the primary key. A second copy off another bearer,
   * our own post echoed off the air, a refill re-reading last week: one row. */
  { pj_t p; pj_init(&p); pj_str(&p, m->mid);
    if (db_int(h, "SELECT 1 AS n FROM messages WHERE mid=? LIMIT 1", pj_done(&p), 0) == 1) {
      /* Already stored -- typically by the OTHER engine. Two engines each drain
       * their own copy of the inbound stream: the headless one commonly WINS the
       * insert (and its ui.convo.* is read by nobody), while the page engine,
       * which draws the open thread and holds g_open, gets here as a dup and USED
       * TO return without drawing anything -- so a message that arrived while you
       * were looking at the conversation did not appear until the view was next
       * refreshed. If this dup is a live inbound for the room ON SCREEN, the page
       * engine paints the bubble now and flushes the read receipt it is owed. */
      if (s_eq(m->dir, "in") && !m->replay && m->room[0] != '#' &&
          s_eq(m->room, g_open)) {
        int priv = room_is_private(m->room);
        emit_msg(m->room, m->mid, m->dir, m->sender ? m->sender : "",
                 m->ts ? m->ts : hal_time_epoch(), m->body,
                 m->parent ? m->parent : "", m->via ? m->via : "",
                 m->auth ? m->auth : "", m->enc, m->rid ? m->rid : "",
                 m->status ? m->status : "", m->sys, priv);
        /* The row is already stored (read_sent=0), so flushing acks it now --
         * the page engine is the one that knows it is being read on screen. */
        room_flush_reads(h);
      }
      return 0;
    } }

  char body[BODY_MAX + 1]; s_cpy(body, m->body, sizeof(body));
  unsigned long long ts = m->ts ? m->ts : hal_time_epoch();
  const char *sender = m->sender ? m->sender : "";
  const char *parent = m->parent ? m->parent : "";
  const char *via = m->via ? m->via : "";
  const char *auth = m->auth ? m->auth : "";
  const char *rid = m->rid ? m->rid : "";
  const char *status = m->status ? m->status : "";
  { pj_t p; pj_init(&p);
    pj_str(&p, m->mid); pj_str(&p, m->dir); pj_str(&p, sender); pj_int(&p, (long long)ts);
    pj_str(&p, body); pj_str(&p, parent); pj_str(&p, via); pj_str(&p, auth);
    pj_int(&p, m->enc ? 1 : 0); pj_str(&p, rid); pj_str(&p, status); pj_int(&p, m->sys ? 1 : 0);
    if (db_exec(h, "INSERT INTO messages(mid,dir,sender,ts,body,parent,via,auth,enc,rid,status,sys)"
                   " VALUES(?,?,?,?,?,?,?,?,?,?,?,?)", pj_done(&p)) != 0) return -1; }

  int created = room_ensure(m->room, m->title);
  int count = (!m->replay && in && !m->sys && !s_eq(m->room, g_open)) ? 1 : 0;
  if (m->replay) {
    pj_t p; pj_init(&p); pj_str(&p, m->room);
    db_exec(g_idx, "UPDATE rooms SET closed=0 WHERE id=?", pj_done(&p));
  } else {
    char last[128] = "";
    if (!m->sys) {
      s_cpy(last, sender, sizeof(last));
      if (last[0]) s_cat(last, ": ", sizeof(last));
      s_cat(last, body, sizeof(last));
    }
    pj_t p; pj_init(&p);
    pj_int(&p, (long long)hal_time_epoch());
    if (m->sys) pj_str(&p, "");
    pj_str(&p, last); pj_int(&p, count); pj_str(&p, m->room);
    db_exec(g_idx, m->sys
        ? "UPDATE rooms SET activity_ts=?, closed=0, unread=unread+? WHERE id=?"
        : "UPDATE rooms SET activity_ts=?, last_line=?, closed=0, unread=unread+? WHERE id=?",
        pj_done(&p));
  }

  /* The row first, then the bubble: the host drops a bubble for a row it
   * holds as closed, and the row says closed:false. */
  emit_upsert(m->room, !m->replay, 0);
  int priv = (m->room[0] != '#') && room_is_private(m->room);
  emit_msg(m->room, m->mid, m->dir, sender, ts, body, parent, via, auth,
           m->enc, rid, status, m->sys, priv);
  if (!m->replay) {
    if (created > 0 || !s_eq(g_top, m->room)) room_rail();
    if (in && !m->sys) notify_msg(m->room, sender, body, m->mid);
    unread_publish();
    /* A live 1:1 we just heard owes its sender an s:read once a person opens
     * this room (XPRS.md 13.7). Recorded here, in the room's own database, so
     * the receipt survives the engine that opens the room being a different
     * one from the engine that stored this. The core applies 13.7.1's
     * exclusions when we ask, so queuing one that turns out not to qualify
     * costs nothing. Replays (archive refill) are not live arrivals and are
     * skipped, so reopening an old thread does not re-ack it. */
    /* Read RIGHT NOW if this is the conversation on screen: a reply that lands
     * while you are looking at the thread is read the instant it arrives, so its
     * s:read must fire now, not only when the thread is next opened. The row was
     * stored with read_sent=0 (the column default), and room_flush_reads acks
     * every inbound row still at 0 -- so nothing needs queuing here. */
    if (in && !m->sys && m->room[0] != '#' && s_eq(m->room, g_open)) {
      room_flush_reads(h);
    }
  } else if (created > 0) {
    room_rail();
  }
  return 1;
}

/* ── opening ──────────────────────────────────────────────────────── */
static void emit_tail(const char *id, int h) {
  int limit = TAIL, n = -2;
  while (limit >= 12) {
    pj_t p; pj_init(&p); pj_int(&p, limit);
    n = db_query(h, "SELECT mid,dir,sender,ts,body,parent,via,auth,enc,rid,status,sys FROM "
                    "(SELECT * FROM messages ORDER BY seq DESC LIMIT ?) ORDER BY seq ASC",
                 pj_done(&p), g_q, sizeof(g_q));
    if (n != -2) break;
    limit /= 2;
  }
  if (n == -2) { log1("[chat] open: the newest twelve messages do not fit the reply buffer"); return; }
  if (n <= 0) return;
  int priv = (id[0] != '#') && room_is_private(id);
  const char *cur = g_q;
  static char row[1400];
  while (next_object(&cur, row, sizeof(row))) {
    char mid[40], dir[8], sender[24], parent[40], via[16], auth[16], rid[40], status[16];
    static char body[BODY_MAX + 1];
    jstr(row, "mid", mid, sizeof(mid)); jstr(row, "dir", dir, sizeof(dir));
    jstr(row, "sender", sender, sizeof(sender)); jstr(row, "body", body, sizeof(body));
    jstr(row, "parent", parent, sizeof(parent)); jstr(row, "via", via, sizeof(via));
    jstr(row, "auth", auth, sizeof(auth)); jstr(row, "rid", rid, sizeof(rid));
    jstr(row, "status", status, sizeof(status));
    emit_msg(id, mid, dir, sender, (unsigned long long)jint(row, "ts"), body, parent, via,
             auth, (int)jint(row, "enc"), rid, status, (int)jint(row, "sys"), priv);
  }
  /* Hearts on those bubbles. */
  pj_t p; pj_init(&p); pj_int(&p, limit);
  n = db_query(h, "SELECT mid,who FROM reactions WHERE mid IN "
                  "(SELECT mid FROM messages ORDER BY seq DESC LIMIT ?) LIMIT 200",
               pj_done(&p), g_q, sizeof(g_q));
  if (n <= 0) return;
  cur = g_q;
  char r[160];
  while (next_object(&cur, r, sizeof(r))) {
    char mid[40], who[24];
    jstr(r, "mid", mid, sizeof(mid)); jstr(r, "who", who, sizeof(who));
    emit_react(id, mid, who, 0, s_eq(who, g_self));
  }
}

/* Every inbound 1:1 in [id] that still owes an s:read: hand each to the core
 * (which composes, signs and airs it down the arrival lane), then forget them.
 * Called from room_open only -- opening a thread is the one moment a person is
 * looking at it. Runs in whatever engine the user is driving; the pending set
 * was written by whatever engine heard the messages, because it lives in this
 * room's database rather than either engine's memory. */
static void room_flush_reads(int h) {
  /* Ask the core to ack read for every inbound message in THIS conversation not
   * yet acked -- older ones the first time it is opened, not only live arrivals.
   * Newest first and capped, so opening a long thread does not air a flood; the
   * rest are caught on the next open. The core resolves each id (its live pocket
   * or its persistent spool) and applies 13.7.1, so a group post or a stranger's
   * is a no-op there -- we still mark it done so it is not re-asked. */
  int n = db_query(h,
      "SELECT mid FROM messages WHERE dir='in' AND sys=0 AND read_sent=0 "
      "ORDER BY seq DESC LIMIT 500", 0, g_q, sizeof(g_q));
  if (n <= 0) return;
  const char *cur = g_q;
  static char row[64];
  int any = 0;
  while (next_object(&cur, row, sizeof(row))) {
    char mid[40];
    if (jstr(row, "mid", mid, sizeof(mid)) && mid[0]) {
      hal_xprs_read(mid, s_len(mid));
      any = 1;
    }
  }
  if (any)
    db_exec(h,
        "UPDATE messages SET read_sent=1 WHERE mid IN "
        "(SELECT mid FROM messages WHERE dir='in' AND sys=0 AND read_sent=0 "
        "ORDER BY seq DESC LIMIT 500)", 0);
}

void room_open(const char *id) {
  if (g_idx < 0 || !room_renderable(id)) return;
  s_cpy(g_open, id, sizeof(g_open));
  int created = room_ensure(id, 0);
  if (created < 0) return;
  { pj_t p; pj_init(&p); pj_str(&p, id);
    db_exec(g_idx, "UPDATE rooms SET closed=0, unread=0 WHERE id=?", pj_done(&p)); }
  /* Opened from outside -- a notification, the mesh graph -- before it had a
   * row: it is a conversation now, so it is on the rail. */
  if (created > 0) room_rail();
  /* Clear removes the row on the host; the upsert right after brings it
   * back, and the tail fills it. Emission order is delivery order. */
  { char m[120] = "{\"type\":\"ui.convo.clear\",\"id\":\"";
    jesc(m, sizeof(m), id); s_cat(m, "\"}", sizeof(m)); hal_msg_send(m, s_len(m)); }
  emit_upsert(id, 0, 0);
  int h = room_handle(id);
  if (h >= 0) {
    emit_tail(id, h);
    /* Groups never earn read receipts (13.7.1); only a 1:1 flushes. */
    if (id[0] != '#') room_flush_reads(h);
  }
  unread_publish();
}

void room_left(void) { g_open[0] = 0; }
const char *room_open_id(void) { return g_open; }

void room_start(const char *id, const char *title) {
  if (room_ensure(id, title) < 0) return;
  { pj_t p; pj_init(&p); pj_str(&p, id);
    db_exec(g_idx, "UPDATE rooms SET closed=0 WHERE id=?", pj_done(&p)); }
  emit_upsert(id, 1, 1);
  room_rail();
}

/* ── the rest of the index ────────────────────────────────────────── */
void room_react(const char *id, const char *mid, const char *who, int remove, int mine) {
  if (!room_renderable(id) || !mid[0] || !who[0]) return;
  int h = room_handle(id);
  if (h < 0) return;
  pj_t p; pj_init(&p); pj_str(&p, mid); pj_str(&p, who);
  db_exec(h, remove ? "DELETE FROM reactions WHERE mid=? AND who=?"
                    : "INSERT OR REPLACE INTO reactions(mid,who,kind) VALUES(?,?,'like')",
          pj_done(&p));
  emit_react(id, mid, who, remove, mine);
}

void room_tx_note(const char *rid, const char *room) {
  if (!rid[0]) return;
  pj_t p; pj_init(&p); pj_str(&p, rid); pj_str(&p, room);
  db_exec(g_idx, "INSERT OR REPLACE INTO tx(rid,room) VALUES(?,?)", pj_done(&p));
}

void room_status(const char *rid, const char *state) {
  if (!rid[0] || !state[0]) return;
  char row[160], room[48] = "";
  { pj_t p; pj_init(&p); pj_str(&p, rid);
    if (db_query(g_idx, "SELECT room FROM tx WHERE rid=? LIMIT 1", pj_done(&p), row, sizeof(row)) > 0)
      jstr(row, "room", room, sizeof(room)); }
  if (room[0]) {
    int h = room_handle(room);
    if (h >= 0) {
      /* Never walk a tick backwards: `read` is terminal (a read implies the
       * ack), so a late `ack` arriving after it must not downgrade the bubble.
       * This matters now that a receipt with no outbox row still reaches here. */
      pj_t p; pj_init(&p); pj_str(&p, state); pj_str(&p, rid);
      db_exec(h, "UPDATE messages SET status=? WHERE rid=? AND status<>'read'",
              pj_done(&p));
    }
  }
  char m[120] = "{\"type\":\"ui.convo.status\",\"rid\":\"";
  jesc(m, sizeof(m), rid);
  s_cat(m, "\",\"status\":\"", sizeof(m)); jesc(m, sizeof(m), state);
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

void room_close(const char *id) {
  if (!room_known(id)) return;
  pj_t p; pj_init(&p); pj_str(&p, id);
  db_exec(g_idx, "UPDATE rooms SET closed=1, unread=0 WHERE id=?", pj_done(&p));
  if (s_eq(g_open, id)) room_left();
  emit_remove(id, 0);
  room_rail();
  unread_publish();
}

int room_is_private(const char *id) {
  pj_t p; pj_init(&p); pj_str(&p, id);
  return db_int(g_idx, "SELECT private AS n FROM rooms WHERE id=? LIMIT 1", pj_done(&p), 0) == 1;
}
int room_set_private(const char *id, int on) {
  if (id[0] == '#' || room_ensure(id, 0) < 0) return -1;
  pj_t p; pj_init(&p); pj_int(&p, on ? 1 : 0); pj_str(&p, id);
  db_exec(g_idx, "UPDATE rooms SET private=? WHERE id=?", pj_done(&p));
  emit_upsert(id, 0, 0);
  return on ? 1 : 0;
}

void room_hide(const char *id, const char *mid) {
  if (!mid[0]) return;
  { pj_t p; pj_init(&p); pj_str(&p, mid);
    db_exec(g_idx, "INSERT OR IGNORE INTO hidden(mid) VALUES(?)", pj_done(&p)); }
  int h = room_handle(id);
  if (h >= 0) {
    pj_t p; pj_init(&p); pj_str(&p, mid);
    db_exec(h, "DELETE FROM messages WHERE mid=?", pj_done(&p));
  }
  emit_remove(id, mid);
}

unsigned long long room_max_ts(const char *id) {
  int h = room_handle(id);
  if (h < 0) return 0;
  return (unsigned long long)db_int(h, "SELECT max(ts) AS n FROM messages", 0, 0);
}

/* ── blocking ─────────────────────────────────────────────────────── */
static void upcall(const char *call, char out[16]) {
  int j = 0;
  for (int i = 0; call[i] && j < 15; i++) out[j++] = s_up(call[i]);
  out[j] = 0;
}
int is_blocked(const char *call) {
  if (!call || !call[0] || g_idx < 0) return 0;
  char c[16]; upcall(call, c);
  pj_t p; pj_init(&p); pj_str(&p, c);
  return db_int(g_idx, "SELECT 1 AS n FROM blocked WHERE call=? LIMIT 1", pj_done(&p), 0) == 1;
}
int block_add(const char *call) {
  char c[16]; upcall(call, c);
  if (!c[0] || s_eq(c, g_self) || is_blocked(c)) return 0;
  pj_t p; pj_init(&p); pj_str(&p, c);
  db_exec(g_idx, "INSERT OR IGNORE INTO blocked(call) VALUES(?)", pj_done(&p));
  blocked_publish();
  return 1;
}
int block_remove(const char *call) {
  char c[16]; upcall(call, c);
  if (!is_blocked(c)) return 0;
  pj_t p; pj_init(&p); pj_str(&p, c);
  db_exec(g_idx, "DELETE FROM blocked WHERE call=?", pj_done(&p));
  blocked_publish();
  return 1;
}

/* ── lifecycle ────────────────────────────────────────────────────── */
void room_init(const char *self) {
  s_cpy(g_self, self ? self : "", sizeof(g_self));
  /* A fresh module has no conversation on screen. Reset it explicitly: g_open
   * is a static that outlives a module_destroy/init, and a stale value makes an
   * arriving message look "read on screen" when nothing is open. */
  g_open[0] = 0;
  for (int i = 0; i < ROOM_H_MAX; i++) { g_rh[i].h = -1; g_rh[i].id[0] = 0; }
  g_idx = db_open("index.sqlite3");
  if (g_idx < 0) { log1("[chat] index.sqlite3 would not open"); return; }
  db_init_index(g_idx);
  /* Clean slate. Every key an older build kept is gone, once: the rooms and
   * their history live here now, and nothing is imported. */
  if (db_int(g_idx, "SELECT 1 AS n FROM meta WHERE k='kv_cleaned' LIMIT 1", 0, 0) != 1) {
    static const char *old[] = { "groups", "recent", "gseen", "blocked", "muted",
      "hidden", "lxnames", "pubkeys", "rnsdest", "xgrp", "cpriv", "grponly",
      "grpclean", "xglobal", "xlxmf", "follows", "igate", "aprsis", "nearby" };
    for (unsigned i = 0; i < sizeof(old) / sizeof(old[0]); i++)
      hal_kv_delete(old[i], s_len(old[i]));
    db_exec(g_idx, "INSERT OR REPLACE INTO meta(k,v) VALUES('kv_cleaned','1')", 0);
  }
  room_ensure(LOCAL, "Local chat");
}

void room_destroy(void) {
  for (int i = 0; i < ROOM_H_MAX; i++) { db_close(g_rh[i].h); g_rh[i].h = -1; }
  db_close(g_idx); g_idx = -1;
}
