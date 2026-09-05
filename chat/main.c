/*
 * chat — conversations over XPRS, and nothing else.
 *
 * The Local room (XPRS.md 13.11, "#LOCAL"), an open group (6.3, "#NAME"), a
 * closed group (26, "#X5ABCD") and a 1:1 with a station ("X16JK8"). Replies,
 * hearts, blocking, hiding, read receipts, the tick on a bubble.
 *
 * ── ONE DOOR IN, ONE STORE PER ROOM ─────────────────────────────────────
 *
 * Every message -- heard live, sent by us, replayed from the core's archive,
 * or a system note -- enters through room_admit() in room.c, which decides
 * once whether it is blocked, hidden or already held, writes it to that
 * room's own sqlite database, and paints the host's view from what it wrote.
 * The host draws; it remembers nothing. "Already held" is the message id
 * being a primary key. There is no seen-ring, no membership list in RAM and
 * no key in KV: the database is the only memory this wapp has, and it is
 * the same one whichever engine (the page's or the headless one) is running.
 *
 * Only "#LOCAL" exists by default. Any other room appears when a message
 * arrives for it or the user starts one.
 *
 * ── THIS WAPP OWNS NO TRANSPORT ─────────────────────────────────────────
 *
 * It says what it wants said and to whom:
 *
 *     hal_xprs_broadcast(text, "local", reply_to, &id)   the Local room
 *     hal_xprs_message(to, text, private, &id)           a 1:1
 *     hal_xprs_send(wire)                                a composed packet
 *     hal_xprs_read(id)                                  a person read it
 *
 * and the core composes, seals (9.2), signs (9.1), splits (6.6), ranks the
 * bearers (36.0), parks a custody copy and reports back. What arrives comes
 * on the event bus, reassembled, unsealed, verified, with its section 5
 * identifier, and only for the packet types this wapp subscribed to.
 *
 * There is no clock. module_tick is empty and the interval is 0: everything
 * here starts with something happening -- a packet from the core, or a
 * person typing.
 */
#include <stdint.h>
#include "xprs_wasm_hal.h"
#include "db.h"
#include "room.h"
#include "thread.h"
#include "xprs.h"

#define XROOM_LOCAL "#LOCAL"

/* ── state ──────────────────────────────────────────────────────────── */
static char g_call[16] = "N0CALL";   /* replaced at init by hal_identity() */
static int  g_chan_local = 1;        /* Settings: the Local room switch (KV "chan") */
static uint64_t g_xroom_fill_at;     /* epoch of the last archive refill */

static int is_self_call(const char *c) {
  if (!c || !c[0]) return 0;
  for (int i = 0; g_call[i] || c[i]; i++)
    if (s_up(g_call[i]) != s_up(c[i])) return 0;
  return 1;
}
static int xroom_is(const char *id) { return s_eq(id, XROOM_LOCAL); }

static void notify(const char *level, const char *body) {
  char m[300] = "{\"type\":\"notify\",\"level\":\"";
  s_cat(m, level, sizeof(m));
  s_cat(m, "\",\"title\":\"Chat\",\"body\":\"", sizeof(m));
  jesc(m, sizeof(m), body);
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
static void screen_open(const char *name) {
  char m[100] = "{\"type\":\"ui.screen.open\",\"name\":\"";
  s_cat(m, name, sizeof(m)); s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
static void screen_close(void) {
  const char *m = "{\"type\":\"ui.screen.close\"}";
  hal_msg_send(m, s_len(m));
}
/* Tell the host's profile panel whether we block [call]. */
static void host_block_state(const char *call, int on) {
  char m[120] = "{\"type\":\"social.blockstate\",\"callsign\":\"";
  jesc(m, sizeof(m), call);
  s_cat(m, "\",\"on\":", sizeof(m));
  s_cat(m, on ? "true}" : "false}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* The one key left in KV: the Local room switch, which the host's settings
 * form drives through chan_apply. */
static void chan_save(void) { hal_kv_set("chan", 4, g_chan_local ? "1" : "0", 1); }
static void chan_load(void) {
  char b[4];
  if (hal_kv_get("chan", 4, b, 3) >= 1) g_chan_local = b[0] == '1';
}

/* ── Closed groups (26): the roster is the core's ─────────────────────
 *
 * A closed group's id here is "#" + its X5 callsign. Membership is a replay
 * of signed acts against a key map the host owns; hal_xprs_groups hands the
 * answer over and this wapp only renders it and refuses to compose where it
 * has no standing. It does NOT create a room per group: a room appears when
 * somebody speaks in it or the user opens it. */
#define XGROUP_MAX 16
static struct { char call[8]; char nick[24]; char role[8]; } g_xgroup[XGROUP_MAX];
static int g_xgroup_n;

static int xgroup_is(const char *id) {
  return id[0] == '#' && id[1] == 'X' && id[2] == '5' && s_len(id) == 7;
}
static int xgroup_find(const char *call) {
  for (int i = 0; i < g_xgroup_n; i++) if (s_eq(g_xgroup[i].call, call)) return i;
  return -1;
}
/* 26.3.1: a grant confers nothing until the person accepts it. */
static int xgroup_may_post(const char *call) {
  int i = xgroup_find(call);
  if (i < 0) return 0;
  return s_eq(g_xgroup[i].role, "member") || s_eq(g_xgroup[i].role, "mod") ||
         s_eq(g_xgroup[i].role, "admin");
}
/* What a room is called on the rail. */
static void room_title(const char *id, char *out, unsigned osz) {
  if (xroom_is(id)) { s_cpy(out, "Local chat", osz); return; }
  if (xgroup_is(id)) {
    /* Name AND callsign: the name is a label anybody can choose; the X5
     * callsign is derived from the key and is what identifies the group. */
    int i = xgroup_find(id + 1);
    if (i >= 0 && g_xgroup[i].nick[0]) {
      s_cpy(out, g_xgroup[i].nick, osz);
      s_cat(out, " (", osz); s_cat(out, id + 1, osz); s_cat(out, ")", osz);
      return;
    }
  }
  s_cpy(out, id, osz);
}
static void xgroups_refresh(void) {
  static char gb[4096];
  int n = hal_xprs_groups(gb, sizeof(gb) - 1);
  if (n <= 0 || n >= (int)sizeof(gb)) return;
  gb[n] = 0;
  g_xgroup_n = 0;
  const char *cur = gb; char obj[600];
  while (g_xgroup_n < XGROUP_MAX && next_object(&cur, obj, sizeof(obj))) {
    int k = g_xgroup_n;
    jstr(obj, "call", g_xgroup[k].call, sizeof(g_xgroup[k].call));
    jstr(obj, "nick", g_xgroup[k].nick, sizeof(g_xgroup[k].nick));
    jstr(obj, "role", g_xgroup[k].role, sizeof(g_xgroup[k].role));
    if (!g_xgroup[k].call[0]) continue;
    g_xgroup_n++;
    /* A room that already exists learns its name; none is created. */
    char id[10] = "#"; s_cat(id, g_xgroup[k].call, sizeof(id));
    if (room_known(id)) { char t[48]; room_title(id, t, sizeof(t)); room_set_title(id, t); }
  }
}

/* ── Read receipts (13.7): the one half a wapp owns ─────────────────────
 * The core composes and signs the receipt; what it cannot observe is that a
 * person opened the thread. Ids wait here until they do. RAM on purpose: a
 * receipt lost to a restart is a tick that stays grey, not a message lost. */
#define RPEND_MAX 64
static char g_rpend_convo[RPEND_MAX][24];
static char g_rpend_id[RPEND_MAX][8];
static int  g_rpend_n;
static void rpend_add(const char *convo, const char *id) {
  if (!id || !id[0] || convo[0] == '#') return;
  for (int i = 0; i < g_rpend_n; i++) if (s_eq(g_rpend_id[i], id)) return;
  if (g_rpend_n >= RPEND_MAX) {
    for (int i = 1; i < RPEND_MAX; i++) {
      s_cpy(g_rpend_convo[i - 1], g_rpend_convo[i], sizeof(g_rpend_convo[0]));
      s_cpy(g_rpend_id[i - 1], g_rpend_id[i], 8);
    }
    g_rpend_n = RPEND_MAX - 1;
  }
  s_cpy(g_rpend_convo[g_rpend_n], convo, sizeof(g_rpend_convo[0]));
  s_cpy(g_rpend_id[g_rpend_n], id, 8);
  g_rpend_n++;
}
static void rpend_flush_read(const char *convo) {
  if (!convo[0] || convo[0] == '#') return;
  int w = 0;
  for (int i = 0; i < g_rpend_n; i++) {
    if (s_eq(g_rpend_convo[i], convo)) {
      hal_xprs_read(g_rpend_id[i], s_len(g_rpend_id[i]));
    } else {
      if (w != i) {
        s_cpy(g_rpend_convo[w], g_rpend_convo[i], sizeof(g_rpend_convo[0]));
        s_cpy(g_rpend_id[w], g_rpend_id[i], 8);
      }
      w++;
    }
  }
  g_rpend_n = w;
}

/* ── Admitting, in this file's vocabulary ─────────────────────────────── */
static int admit(const char *room, const char *mid, const char *dir,
                 const char *sender, const char *body, const char *parent,
                 const char *via, const char *auth, int enc, uint64_t ts,
                 const char *rid, const char *status, int replay) {
  char title[48]; room_title(room, title, sizeof(title));
  room_msg_t m = { room, title, mid, dir, sender, body, parent, via, auth,
                   rid, status, ts, enc, 0, replay };
  return room_admit(&m);
}
/* A muted, centered line inside a conversation -- not words anybody said. */
static void sysnote(const char *room, const char *text) {
  char h[5]; msg_id(room, text, h);
  char mid[40] = "sys:"; char nb[24]; u_lltoa(hal_time_epoch(), nb);
  s_cat(mid, nb, sizeof(mid)); s_cat(mid, ":", sizeof(mid)); s_cat(mid, h, sizeof(mid));
  room_msg_t m = { room, 0, mid, "in", "", text, "", "", "", "", "", 0, 0, 1, 0 };
  room_admit(&m);
}
/* A reply marker on a wire with room for a section 5 id: "+<6hex> text". */
static int strip_reply6(char *text, char parent[8]) {
  parent[0] = 0;
  if (!(text[0] == '+' && s_len(text) > 8 && text[7] == ' ')) return 0;
  for (int i = 1; i <= 6; i++) {
    char c = text[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
  }
  for (int i = 0; i < 6; i++) parent[i] = text[1 + i];
  parent[6] = 0;
  unsigned k = 0, n = 8;
  while (text[n]) text[k++] = text[n++];
  text[k] = 0;
  return 1;
}

/* ── Refilling #LOCAL from the core's archive ─────────────────────────
 *
 * XprsArchive keeps every heard packet for a year, and a scope:local message
 * is stored there like any other undirected packet. Whatever arrived while
 * this wapp was not running is read back from there -- once at init, and
 * again when a person opens the room, which is the one moment they are
 * asking to see it. Never on a clock. The read starts where the room's own
 * copy ends (max ts, five minutes of overlap for clock skew between sender
 * stamp and archive stamp); anything re-read is a duplicate by primary key
 * and costs one indexed lookup. */
static char g_hist[32768];

/* The value of `key:` in an XPRS wire, up to the next space (section 4). */
static int wire_key(const char *wire, const char *key, char *out, unsigned cap) {
  out[0] = 0;
  unsigned kl = s_len(key);
  for (const char *p = wire; *p; p++) {
    if (p != wire && p[-1] != ' ') continue;
    unsigned i = 0;
    while (i < kl && p[i] == key[i]) i++;
    if (i != kl || p[kl] != ':') continue;
    p += kl + 1;
    unsigned o = 0;
    while (*p && *p != ' ' && o < cap - 1) out[o++] = *p++;
    out[o] = 0;
    return 1;
  }
  return 0;
}
/* Everything after " m:" -- greedy, because m: is last. */
static int wire_body(const char *wire, char *out, unsigned cap) {
  out[0] = 0;
  for (const char *p = wire; *p; p++) {
    if (p != wire && p[-1] != ' ') continue;
    if (p[0] == 'm' && p[1] == ':') { s_cpy(out, p + 2, cap); return out[0] != 0; }
  }
  return 0;
}

static void xroom_backfill(void) {
  char q[160] = "{\"limit\":60,\"types\":[\"message\"],\"to\":[\"\"]";
  uint64_t since = room_max_ts(XROOM_LOCAL);
  if (since > 300) {
    char st[24]; xprs_stamp(st, sizeof(st), since - 300);
    s_cat(q, ",\"since\":\"", sizeof(q)); s_cat(q, st, sizeof(q)); s_cat(q, "\"", sizeof(q));
  }
  s_cat(q, "}", sizeof(q));
  int32_t n = hal_xprs_history(q, s_len(q), g_hist, sizeof(g_hist) - 1);
  if (n < 0) {
    char lg[112] = "[chat] local backfill: reply needs ";
    char nb[16]; u_itoa((unsigned)(-n), nb);
    s_cat(lg, nb, sizeof(lg)); s_cat(lg, " bytes, buffer is 32767 -- skipped", sizeof(lg));
    log1(lg);
    return;
  }
  if (n <= 0 || n >= (int)sizeof(g_hist)) return;
  g_hist[n] = 0;

  /* The archive answers newest first; the room stores in arrival order.
   * Find every row, then walk them backwards. */
  static const char *at[60]; int rows = 0;
  { const char *cur = g_hist;
    while (rows < 60) {
      while (*cur && *cur != '{') cur++;
      if (!*cur) break;
      at[rows++] = cur;
      char skip[1400]; const char *c2 = cur;
      if (!next_object(&c2, skip, sizeof(skip))) break;
      cur = c2;
    } }
  int kept = 0;
  for (int k = rows - 1; k >= 0; k--) {
    static char slice[1400];
    const char *c2 = at[k];
    if (!next_object(&c2, slice, sizeof(slice))) continue;
    char wire[400] = "", from[24] = "", id[24] = "", bearer[12] = "", sig[16] = "";
    jstr(slice, "wire", wire, sizeof(wire));
    jstr(slice, "from", from, sizeof(from));
    jstr(slice, "id", id, sizeof(id));
    jstr(slice, "bearer", bearer, sizeof(bearer));
    jstr(slice, "sig", sig, sizeof(sig));
    if (!wire[0] || !from[0] || !id[0]) continue;
    /* The same tests the live path applies (on_core_packet). */
    char tmp[24];
    if (wire_key(wire, "n", tmp, sizeof(tmp))) continue;   /* a part (6.6) */
    if (wire_key(wire, "x", tmp, sizeof(tmp))) continue;   /* sealed (9.2) */
    if (wire_key(wire, "d", tmp, sizeof(tmp))) continue;   /* addressed */
    if (!wire_key(wire, "scope", tmp, sizeof(tmp)) || !s_eq(tmp, "local")) continue;
    int mine = jbool_def(slice, "own", 0) || is_self_call(from);
    char body[400];
    if (!wire_body(wire, body, sizeof(body))) continue;
    char parent[8] = ""; wire_key(wire, "r", parent, sizeof(parent));
    if (admit(XROOM_LOCAL, id, mine ? "out" : "in", mine ? g_call : from, body, parent,
              bearer, s_eq(sig, "verified") ? "verified" : "", 0,
              (uint64_t)jint(slice, "ts"), "", "", 1) == 1)
      kept++;
  }
  char lg[96] = "[chat] local backfill: read=";
  char nb[16]; u_itoa((unsigned)rows, nb); s_cat(lg, nb, sizeof(lg));
  s_cat(lg, " kept=", sizeof(lg)); u_itoa((unsigned)kept, nb); s_cat(lg, nb, sizeof(lg));
  log1(lg);
}

/* ── Sending ──────────────────────────────────────────────────────────── */
static void send_message(const char *id, const char *text_in) {
  char text[900];
  s_cpy(text, text_in, sizeof(text));
  if (!id[0] || !text[0] || !room_renderable(id)) return;

  /* The Local room: a t:message broadcast the core composes, or a
   * t:reaction (6.5) naming a bubble's section 5 id. */
  if (xroom_is(id)) {
    char lmid[70]; int unlike; const char *ck;
    if (votemark_parse(text, lmid, &unlike, &ck)) {
      char ts[24]; xprs_stamp(ts, sizeof(ts), hal_time_epoch());
      char wire[300] = "t:reaction f:";
      s_cat(wire, g_call, sizeof(wire));
      s_cat(wire, " ts:", sizeof(wire)); s_cat(wire, ts, sizeof(wire));
      s_cat(wire, " scope:local", sizeof(wire));
      s_cat(wire, unlike ? " remove:like" : " add:like", sizeof(wire));
      s_cat(wire, " r:", sizeof(wire)); s_cat(wire, lmid, sizeof(wire));
      if (hal_xprs_send(wire, s_len(wire)) != 0) { notify("warning", "Could not send"); return; }
      room_react(id, lmid, g_call, unlike, 1);
      return;
    }
    char parent[8]; strip_reply6(text, parent);
    if (!text[0]) return;
    char mid[8] = "";
    if (hal_xprs_broadcast(text, s_len(text), "local", 5, parent, s_len(parent),
                           mid, sizeof(mid)) != 2 || !mid[0]) {
      notify("warning", "Could not send");
      return;
    }
    admit(id, mid, "out", g_call, text, parent, "", "verified", 0, 0, "", "", 0);
    return;
  }

  /* A closed group (26): an ordinary t:message addressed to the GROUP's
   * callsign. The core signs after us and the section 5 id is taken with
   * sig: removed, so these bytes are the id. */
  if (xgroup_is(id)) {
    if (!xgroup_may_post(id + 1)) {
      notify("warning", "You can post here once you accept the invitation");
      return;
    }
    char parent[8]; strip_reply6(text, parent);
    if (!text[0]) return;
    char ts[24]; xprs_stamp(ts, sizeof(ts), hal_time_epoch());
    char wire[400] = "t:message f:";
    s_cat(wire, g_call, sizeof(wire));
    s_cat(wire, " d:", sizeof(wire)); s_cat(wire, id + 1, sizeof(wire));
    s_cat(wire, " ts:", sizeof(wire)); s_cat(wire, ts, sizeof(wire));
    if (parent[0]) { s_cat(wire, " r:", sizeof(wire)); s_cat(wire, parent, sizeof(wire)); }
    s_cat(wire, " m:", sizeof(wire)); s_cat(wire, text, sizeof(wire));
    if (s_len(wire) > 250) { notify("warning", "Message too long"); return; }
    if (hal_xprs_send(wire, s_len(wire)) != 0) { notify("warning", "Could not send"); return; }
    char gmid[7]; xprs_id(wire, s_len(wire), gmid);
    admit(id, gmid, "out", g_call, text, parent, "", "verified", 0, 0, "", "", 0);
    return;
  }

  /* An open group (6.3): a bulletin addressed to the group name. A heart
   * rides the same path as text ("<id>:like"), and is a reaction, not words. */
  if (id[0] == '#') {
    char wire[900];
    if (!xprs_pack(wire, sizeof(wire), g_call, id, text, hal_time_epoch())) {
      notify("warning", "Could not send"); return;
    }
    if (hal_xprs_send(wire, s_len(wire)) != 0) { notify("warning", "Could not send"); return; }
    char lmid[70]; int unlike; const char *ck; char tgt[5];
    if (votemark_parse(text, lmid, &unlike, &ck)) { room_react(id, lmid, g_call, unlike, 1); return; }
    if (like_parse(text, tgt, &unlike)) { room_react(id, tgt, g_call, unlike, 1); return; }
    char gmid[7]; xprs_id(wire, s_len(wire), gmid);
    char parent[5]; const char *disp;
    thread_parse(text, parent, &disp);
    admit(id, gmid, "out", g_call, disp, parent, "", "verified", 0, 0, "", "", 0);
    return;
  }

  /* A 1:1. The return value says what ACTUALLY happened, and the bubble is
   * labelled with that: a message that could not be sealed is never drawn
   * as private (36.8). */
  int priv = room_is_private(id);
  char mid[8] = "";
  int32_t form = hal_xprs_message(id, s_len(id), text, s_len(text),
                                  priv ? 1u : 0u, mid, sizeof(mid));
  if (form == -1) {
    sysnote(id, "No key for this contact yet - asked for it. Your message was "
                "NOT sent; try again in a moment.");
    notify("warning", "No key for this contact yet");
    return;
  }
  if (form <= 0) { notify("warning", "Could not send"); return; }
  if (mid[0]) room_tx_note(mid, id);
  admit(id, mid[0] ? mid : "", "out", g_call, text, "", "", "verified", form == 1, 0,
        mid, "sent", 0);
}

/* ── What the core routes to us ───────────────────────────────────────── */

/* A packet delivered as HEARD: the wire's fields, its section 5 identifier
 * and its provenance. Undirected traffic and open-group bulletins render from
 * here. A 1:1 and a closed-group post arrive in the other shape, with
 * `content`, after the core has reassembled and unsealed them. */
static void on_core_packet(const char *topic, const char *row) {
  char from[24] = "", to[40] = "", id[24] = "", bearer[12] = "", sigv[12] = "";
  jstr(row, "from", from, sizeof(from));
  jstr(row, "to", to, sizeof(to));
  jstr(row, "id", id, sizeof(id));
  jstr(row, "bearer", bearer, sizeof(bearer));
  jstr(row, "sig", sigv, sizeof(sigv));
  if (!from[0] || is_self_call(from)) return;
  /* A part is not a message (6.6) and a sealed body is not readable: both
   * are the core's to finish, and it re-delivers the result. */
  { char n[8]; if (jfield(row, "n", n, sizeof(n))) return; }
  if (jbool(row, "sealed")) return;
  if (!id[0]) return;   /* no identifier, no dedup, no bubble */
  const char *auth = s_eq(sigv, "verified") ? "verified" : "";

  if (s_eq(topic, "xprs.reaction")) {
    /* A vote on a Local-room bubble (6.5), named by its section 5 id. */
    char scope[16] = "", r[16] = "", add[16] = "", rem[16] = "";
    jstr(row, "scope", scope, sizeof(scope));
    jfield(row, "r", r, sizeof(r));
    jfield(row, "add", add, sizeof(add));
    jfield(row, "remove", rem, sizeof(rem));
    if (to[0] || !s_eq(scope, "local") || !r[0]) return;
    if (!(s_eq(add, "like") || s_eq(rem, "like"))) return;
    if (is_blocked(from)) return;
    room_react(XROOM_LOCAL, r, from, rem[0] ? 1 : 0, 0);
    return;
  }
  if (!s_eq(topic, "xprs.message")) return;
  char m[900] = "";
  if (!jfield(row, "m", m, sizeof(m)) || !m[0]) return;
  /* Addressed to a station -- including a closed group, which is a station
   * by 6.3's naming rule -- is correspondence and renders from the other
   * shape. */
  if (xprs_is_station(to)) return;
  char parent[8] = ""; jfield(row, "r", parent, sizeof(parent));
  uint64_t ts = 0;
  { char t[24]; if (jfield(row, "ts", t, sizeof(t))) ts = xprs_parse_stamp(t); }

  if (!to[0]) {
    /* UNDIRECTED TRAFFIC IS THE LOCAL ROOM'S, OR NOBODY'S (13.11.1). With
     * no d: there is no custody, no ack and no retry; `local` is the room
     * where that is honest, because everyone in it is in earshot. Say why a
     * packet stayed quiet -- only on a drop. */
    char scope[16] = "";
    jstr(row, "scope", scope, sizeof(scope));
    if (!s_eq(scope, "local")) {
      char lg[128] = "[chat] local dropped: not scope:local from=";
      s_cat(lg, from, sizeof(lg)); s_cat(lg, " id=", sizeof(lg)); s_cat(lg, id, sizeof(lg));
      log1(lg);
      return;
    }
    admit(XROOM_LOCAL, id, "in", from, m, parent, bearer, auth, 0, ts, "", "", 0);
    return;
  }

  /* An open-group bulletin. Only for a group we are in: the room exists. */
  char gid[24] = "#"; s_cat(gid, to, sizeof(gid));
  if (!room_known(gid)) return;
  char lmid[70]; int unlike; const char *ck; char tgt[5];
  if (votemark_parse(m, lmid, &unlike, &ck)) { room_react(gid, lmid, from, unlike, 0); return; }
  if (like_parse(m, tgt, &unlike)) { room_react(gid, tgt, from, unlike, 0); return; }
  char par4[5]; const char *disp;
  thread_parse(m, par4, &disp);
  admit(gid, id, "in", from, disp, parent[0] ? parent : par4, bearer, auth, 0, ts, "", "", 0);
}

static void on_core_event(const char *topic, const char *row) {
  if (!topic[0] || !row[0]) return;

  /* The outbound side of 13.7: what the core learned about a message WE
   * sent, keyed on the same section 5 identifier the bubble carries. */
  if (s_eq(topic, "xprs.status.tx")) {
    char sid[24] = "", state[16] = "";
    jstr(row, "id", sid, sizeof(sid));
    jstr(row, "state", state, sizeof(state));
    room_status(sid, state);
    return;
  }

  static char content[900];
  char from[24] = "", title[40] = "", id[24] = "", call[24] = "", sigv[12] = "";
  char bearer[12] = "";
  jstr(row, "from", from, sizeof(from));
  jstr(row, "title", title, sizeof(title));
  jstr(row, "id", id, sizeof(id));
  jstr(row, "call", call, sizeof(call));
  jstr(row, "sig", sigv, sizeof(sigv));
  /* The lane the core says it arrived on -- ble, lan, lora, rns. NOT a
   * literal: this branch used to hardcode "rns", so every 1:1 read
   * "Reticulum" whatever carried it. Empty = no tag, never a guess. */
  jstr(row, "bearer", bearer, sizeof(bearer));
  /* Two shapes reach us on xprs.message. `content` marks the finished one:
   * a 1:1 or a closed-group post the core reassembled and unsealed. */
  if (!jstr(row, "content", content, sizeof(content)) || !content[0]) {
    on_core_packet(topic, row);
    return;
  }
  /* XPRS only: the sender is a station with a callsign, or nobody. */
  if (!call[0] || !xprs_is_station(call) || is_self_call(call)) return;
  /* Control traffic from an older build's private-mode handshake is not
   * correspondence. */
  if (s_eq(content, "?PRIV1") || s_eq(content, "?PRIV0") || s_pre(content, "?ACK ")) return;

  const char *room;
  if (title[0] == '#') {
    /* A closed group's post, addressed to the group. Membership is the
     * core's answer; a post it delivered is one we may see. */
    if (!xgroup_is(title)) return;
    room = title;
  } else {
    room = call;
  }
  char mid[24];
  if (id[0]) s_cpy(mid, id, sizeof(mid));
  else { char dk[5]; msg_id(call, content, dk); s_cpy(mid, "c:", sizeof(mid)); s_cat(mid, dk, sizeof(mid)); }
  uint64_t ts = (uint64_t)jint(row, "ts");
  if (ts > 100000000000ULL) ts /= 1000;   /* milliseconds from some sources */
  char parent[8] = "";
  static char body[900];
  s_cpy(body, content, sizeof(body));
  if (room[0] == '#') strip_reply6(body, parent);
  int r = admit(room, mid, "in", call, body, parent, bearer,
                s_eq(sigv, "verified") ? "verified" : "", jbool(row, "sealed"), ts, "", "", 0);
  /* The READ half of 13.7 fires when the user opens this thread. */
  if (r == 1 && room[0] != '#') rpend_add(room, id);
}

static void drain_core_events(void) {
  static char row[3200];
  char topic[64];
  for (int guard = 0; guard < 32; guard++) {
    if (hal_event_available() == 0) break;
    uint32_t n = hal_event_recv(topic, sizeof(topic) - 1, row, sizeof(row) - 1);
    if (n == 0) break;
    row[n] = 0;
    if (s_eq(topic, "core.groups")) { xgroups_refresh(); continue; }
    on_core_event(topic, row);
  }
}

/* ── Finding somebody: the New chat and Search screens ──────────────────
 *
 * ONE question, ONE door: who has this device heard? The core keeps that
 * table (XprsMonitor -- every reachability decision it makes reads the same
 * one) and hands it over as hal_xprs_stations: stations in earshot now, then
 * stations heard this hour but quiet since -- both on a radio or the local
 * network, a relayed packet counting for the radio it arrived on -- then
 * XPRS stations that reached us over Reticulum this hour. Newest first, each
 * row tagged `seen Xm ago` and its bearer. This wapp draws the first two as
 * "Nearby" and the third as "On Reticulum", and derives nothing.
 *
 * It used to ask hal_mesh_devices and hal_people_directory. The first is fed
 * by a beacon phones do not air, the second is a Reticulum view that lists a
 * station only after an LXMF announce -- so on a phone hearing two others
 * over Bluetooth the panel was empty. A callsign is all Chat needs
 * (XPRS.md 3); the lane and the key are the core's business at send time.
 * In Search, "#GROUP" typed in opens that group. A tap starts the room. */
static char g_find_q[64], g_sa_q[64];

#define PEOPLE_MAX 128
typedef struct { char call[16]; char seen[24]; char bearer[12]; int local; } person_t;
static person_t g_people[PEOPLE_MAX];
static int g_people_n;

/* First occurrence of [needle] in [hay], or 0. */
static const char *s_find(const char *hay, const char *needle) {
  for (const char *p = hay; *p; p++) if (s_pre(p, needle)) return p;
  return 0;
}
/* The [idx]-th string of the JSON array under [key]: "tags":["a","b"]. */
static int jarr_str(const char *obj, const char *key, int idx, char *out, unsigned cap) {
  out[0] = 0;
  char pat[32] = "\"";
  s_cat(pat, key, sizeof(pat)); s_cat(pat, "\":[", sizeof(pat));
  const char *p = s_find(obj, pat);
  if (!p) return 0;
  p += s_len(pat);
  for (int i = 0; ; i++) {
    while (*p == ' ' || *p == ',') p++;
    if (*p != '"') return 0;
    p++;
    unsigned o = 0;
    while (*p && *p != '"') {
      if (*p == '\\' && p[1]) p++;
      if (i == idx && o < cap - 1) out[o++] = *p;
      p++;
    }
    if (*p == '"') p++;
    if (i == idx) { out[o] = 0; return 1; }
  }
}

static void people_collect(const char *q) {
  g_people_n = 0;
  char want[24] = ""; int j = 0;
  for (int i = 0; q[i] && j < 23; i++) if (q[i] != ' ') want[j++] = s_up(q[i]);
  want[j] = 0;
  static char st[16384];
  int n = hal_xprs_stations(st, sizeof(st) - 1);
  if (n < 0) {
    /* The host answers -required when the buffer is too small. */
    static int said;
    if (!said) { said = 1;
      char lg[96] = "[chat] stations reply needs "; char nb[16]; u_itoa((unsigned)(-n), nb);
      s_cat(lg, nb, sizeof(lg)); s_cat(lg, " bytes, buffer is 16383", sizeof(lg)); log1(lg); }
    return;
  }
  if (n <= 0) return;
  st[n] = 0;
  /* Sections, by title: the two local ones ("Heard over the air", "Heard
   * this hour") and "On Reticulum". Each holds an "items" array; walk it to
   * its ']'. */
  const char *p = st; int section = 0;
  while ((p = s_find(p, "\"title\":\"")) != 0 && section < 3) {
    int local = !s_pre(p + 9, "On Reticulum");
    p = s_find(p, "\"items\":[");
    if (!p) break;
    p += 9;
    char row[600];
    while (*p) {
      while (*p == ' ' || *p == ',') p++;
      if (*p == ']' || !*p) break;
      const char *cur = p;
      if (!next_object(&cur, row, sizeof(row))) break;
      p = cur;
      char call[24]; jstr(row, "id", call, sizeof(call));
      if (!call[0] || !xprs_is_station(call) || is_self_call(call)) continue;
      if (want[0]) {
        char up_call[24]; s_cpy(up_call, call, sizeof(up_call));
        for (int i = 0; up_call[i]; i++) up_call[i] = s_up(up_call[i]);
        if (!s_find(up_call, want)) continue;
      }
      int dup = 0;
      for (int i = 0; i < g_people_n; i++) if (s_eq(g_people[i].call, call)) { dup = 1; break; }
      if (dup || g_people_n >= PEOPLE_MAX) continue;
      person_t *e = &g_people[g_people_n++];
      s_cpy(e->call, call, sizeof(e->call));
      jarr_str(row, "tags", 0, e->seen, sizeof(e->seen));
      jarr_str(row, "tags", 1, e->bearer, sizeof(e->bearer));
      e->local = local;
    }
    section++;
  }
}
static void people_row(char *o, unsigned sz, const person_t *e) {
  s_cat(o, "{\"id\":\"go:", sz); jesc(o, sz, e->call);
  s_cat(o, "\",\"title\":\"", sz); jesc(o, sz, e->call);
  s_cat(o, "\",\"subtitle\":\"", sz);
  if (e->seen[0]) jesc(o, sz, e->seen); else s_cat(o, "heard", sz);
  if (e->bearer[0]) { s_cat(o, " - ", sz); jesc(o, sz, e->bearer); }
  s_cat(o, "\",\"icon\":\"person\"}", sz);
}
static void render_people(const char *field, const char *q, int allow_group) {
  static char o[16384]; const unsigned sz = sizeof(o);
  char want[24] = ""; int j = 0;
  for (int i = 0; q[i] && j < 23; i++) if (q[i] != ' ') want[j++] = s_up(q[i]);
  want[j] = 0;
  people_collect(q);
  s_cpy(o, "{\"type\":\"ui.people.set\",\"field\":\"", sz);
  s_cat(o, field, sz);
  s_cat(o, "\",\"sections\":[", sz);
  int first_section = 1;
  if (allow_group && want[0] == '#' && want[1]) {
    s_cat(o, "{\"title\":\"Group\",\"items\":[{\"id\":\"go:", sz); jesc(o, sz, want);
    s_cat(o, "\",\"title\":\"", sz); jesc(o, sz, want);
    s_cat(o, "\",\"subtitle\":\"Open this group\",\"icon\":\"tag\"}]}", sz);
    first_section = 0;
  }
  for (int pass = 1; pass >= 0; pass--) {   /* nearby first, then Reticulum */
    int any = 0;
    for (int i = 0; i < g_people_n; i++) if (g_people[i].local == pass) { any = 1; break; }
    /* A callsign nobody has heard yet is still somebody: custody waits. */
    int typed = pass == 0 && want[0] != '#' && xprs_is_station(want) && !is_self_call(want);
    if (typed) { for (int i = 0; i < g_people_n; i++) if (s_eq(g_people[i].call, want)) typed = 0; }
    if (!any && !typed) continue;
    if (!first_section) s_cat(o, ",", sz);
    first_section = 0;
    s_cat(o, pass ? "{\"title\":\"Nearby\",\"items\":[" : "{\"title\":\"On Reticulum\",\"items\":[", sz);
    int first = 1;
    for (int i = 0; i < g_people_n; i++) {
      if (g_people[i].local != pass) continue;
      if (!first) s_cat(o, ",", sz);
      first = 0;
      people_row(o, sz, &g_people[i]);
    }
    if (typed) {
      if (!first) s_cat(o, ",", sz);
      s_cat(o, "{\"id\":\"go:", sz); jesc(o, sz, want);
      s_cat(o, "\",\"title\":\"Message ", sz); jesc(o, sz, want);
      s_cat(o, "\",\"subtitle\":\"Not heard yet - delivery waits for them\",\"icon\":\"person_add\"}", sz);
    }
    s_cat(o, "]}", sz);
  }
  s_cat(o, "]}", sz);
  hal_msg_send(o, s_len(o));
}
static void go_tap(const char *buf, const char *key) {
  char id[64] = "";
  jstr(buf, key, id, sizeof(id));
  if (!s_pre(id, "go:") || !room_renderable(id + 3)) return;
  char title[48]; room_title(id + 3, title, sizeof(title));
  room_start(id + 3, title);
  screen_close();
}

/* The sender's name tapped in a room: block or unblock them. */
static void profile_show(const char *call) {
  char c[16]; int j = 0;
  for (int i = 0; call[i] && j < 15; i++) c[j++] = s_up(call[i]);
  c[j] = 0;
  if (!c[0] || is_self_call(c)) return;
  int blocked = is_blocked(c);
  char m[400] = "{\"type\":\"ui.prompt\",\"id\":\"prof:";
  jesc(m, sizeof(m), c);
  s_cat(m, "\",\"title\":\"", sizeof(m)); jesc(m, sizeof(m), c);
  s_cat(m, "\",\"body\":\"", sizeof(m));
  s_cat(m, blocked ? "Blocked - their messages are hidden." : "An XPRS station.", sizeof(m));
  s_cat(m, "\",\"chips\":[", sizeof(m));
  s_cat(m, blocked ? "{\"label\":\"Unblock\",\"value\":\"unblock\"}"
                   : "{\"label\":\"Block\",\"value\":\"block\"}", sizeof(m));
  s_cat(m, "]}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
static void do_block(const char *call) {
  if (block_add(call)) { host_block_state(call, 1); notify("info", "Blocked - you won't see their messages"); }
}
static void do_unblock(const char *call) {
  if (block_remove(call)) { host_block_state(call, 0); notify("info", "Unblocked"); }
}

/* THE WIDGET IS CALLED "rooms", SO ITS FIELDS ARE rooms_*. The older
 * conversations_* names stay accepted so nothing driving this wapp from
 * outside has to change. */
static int cmd_field(const char *buf, const char *key, char *out, unsigned m) {
  char k[48] = "rooms_"; s_cat(k, key, sizeof(k));
  if (jstr(buf, k, out, m) && out[0]) return 1;
  s_cpy(k, "conversations_", sizeof(k)); s_cat(k, key, sizeof(k));
  return jstr(buf, k, out, m);
}

static void do_open(const char *buf) {
  char convo[48] = ""; cmd_field(buf, "convo", convo, sizeof(convo));
  if (!convo[0]) return;
  room_open(convo);
  rpend_flush_read(convo);
  /* Opening the room is the one moment somebody is asking to SEE it. The
   * guard is a memory of the last refill, not a schedule. */
  if (xroom_is(convo)) {
    uint64_t now = hal_time_epoch();
    if (now - g_xroom_fill_at >= 60) { g_xroom_fill_at = now; xroom_backfill(); }
  }
}

/* ── module entry points ──────────────────────────────────────────────── */
void module_init(void) {
  static const char *topics[] = { "xprs.message", "xprs.reaction", "xprs.status.tx", "core.groups" };
  for (unsigned i = 0; i < sizeof(topics) / sizeof(topics[0]); i++)
    hal_event_subscribe(topics[i], s_len(topics[i]));
  char id[16];
  uint32_t n = hal_identity(id, sizeof(id) - 1);
  if (n > 0 && n < sizeof(id)) { id[n] = 0; if (id[0]) s_cpy(g_call, id, sizeof(g_call)); }
  chan_load();
  room_set_local_enabled(g_chan_local);
  room_init(g_call);
  xgroups_refresh();
  /* The archive first, so the first frame already holds what was said
   * while we were not listening; then the view. */
  g_xroom_fill_at = hal_time_epoch();
  xroom_backfill();
  room_hydrate();
}

/* NO CLOCK. Everything is a call: the core delivering, or a person typing. */
void module_tick(void) {}
int32_t module_tick_interval_ms(void) { return 0; }

void module_handle_event(void) {
  static char buf[4096];
  /* What the core routed to us, first: a host command is not a precondition
   * for a packet. */
  drain_core_events();
  if (hal_msg_available() == 0) return;
  uint32_t n = hal_msg_recv(buf, sizeof(buf) - 1);
  if (n == 0) return;
  buf[n] = 0;

  /* A form command is {command:name}; an app-bar or popup-menu item is
   * {type:"action",action:name}. Resolve the name, THEN dispatch. */
  char cmd[40] = "";
  if (!jstr(buf, "command", cmd, sizeof(cmd)) || !cmd[0]) {
    char typ[24] = ""; jstr(buf, "type", typ, sizeof(typ));
    if (!s_eq(typ, "action") || !jstr(buf, "action", cmd, sizeof(cmd))) return;
  }

  if (s_eq(cmd, "rooms_send") || s_eq(cmd, "conversations_send")) {
    char id[48] = ""; static char text[900];
    cmd_field(buf, "convo", id, sizeof(id));
    cmd_field(buf, "input", text, sizeof(text));
    if (id[0] && text[0]) send_message(id, text);
  }
  else if (s_eq(cmd, "rooms_open") || s_eq(cmd, "conversations_open")) do_open(buf);
  else if (s_eq(cmd, "nav_back")) room_left();
  else if (s_eq(cmd, "rooms_close") || s_eq(cmd, "conversations_close")) {
    char id[48] = ""; cmd_field(buf, "convo", id, sizeof(id));
    if (id[0]) room_close(id);
  }
  else if (s_eq(cmd, "rooms_hide") || s_eq(cmd, "conversations_hide")) {
    char id[48] = "", key[40] = "";
    cmd_field(buf, "convo", id, sizeof(id));
    cmd_field(buf, "hidekey", key, sizeof(key));
    if (id[0] && key[0]) room_hide(id, key);
  }
  else if (s_eq(cmd, "rooms_block") || s_eq(cmd, "conversations_block")) {
    char c[16] = ""; cmd_field(buf, "blockcall", c, sizeof(c));
    if (c[0]) do_block(c);
  }
  else if (s_eq(cmd, "rooms_private") || s_eq(cmd, "conversations_private")) {
    /* Privacy is a property of each packet (9.2): nothing to negotiate with
     * the peer, and the wire says which form was used. */
    char id[48] = ""; cmd_field(buf, "convo", id, sizeof(id));
    if (id[0] && id[0] != '#') {
      int on = room_set_private(id, !room_is_private(id));
      if (on >= 0) notify("info", on ? "Messages to this station are sealed"
                                    : "Messages to this station are plain text");
    }
  }
  else if (s_eq(cmd, "rooms_newchat")) {
    g_find_q[0] = 0;
    render_people("finduser", "", 0);
    screen_open("New chat");
  }
  else if (s_eq(cmd, "finduser_search")) {
    jstr(buf, "finduser_query", g_find_q, sizeof(g_find_q));
    render_people("finduser", g_find_q, 0);
  }
  else if (s_eq(cmd, "finduser_tap")) go_tap(buf, "finduser_id");
  else if (s_eq(cmd, "rooms_search")) {
    g_sa_q[0] = 0;
    render_people("searchall", "", 1);
    screen_open("Search");
  }
  else if (s_eq(cmd, "searchall_search")) {
    jstr(buf, "searchall_query", g_sa_q, sizeof(g_sa_q));
    render_people("searchall", g_sa_q, 1);
  }
  else if (s_eq(cmd, "searchall_tap")) go_tap(buf, "searchall_id");
  else if (s_eq(cmd, "rooms_settings")) screen_open("Settings");
  else if (s_eq(cmd, "profile")) {
    char c[16] = ""; jstr(buf, "profile_call", c, sizeof(c));
    profile_show(c);
  }
  else if (s_eq(cmd, "profile_block")) {
    char c[16] = ""; jstr(buf, "profile_target", c, sizeof(c));
    if (c[0]) do_block(c);
  }
  else if (s_eq(cmd, "profile_unblock")) {
    char c[16] = ""; jstr(buf, "profile_target", c, sizeof(c));
    if (c[0]) do_unblock(c);
  }
  else if (s_eq(cmd, "prompt")) {
    char pid[24] = "", val[24] = "";
    jstr(buf, "prompt_id", pid, sizeof(pid));
    jstr(buf, "prompt_value", val, sizeof(val));
    if (s_pre(pid, "prof:")) {
      if (s_eq(val, "block")) do_block(pid + 5);
      else if (s_eq(val, "unblock")) do_unblock(pid + 5);
    }
  }
  else if (s_eq(cmd, "chan_apply")) {
    /* Explicit apply, so an unset checkbox serialised as false on some other
     * command never clobbers the on-by-default switch. */
    g_chan_local = jbool_def(buf, "chan_local", 1);
    chan_save();
    room_set_local_enabled(g_chan_local);
    notify("info", g_chan_local ? "Local chat is on" : "Local chat is off");
  }
}

void module_destroy(void) { room_destroy(); }
