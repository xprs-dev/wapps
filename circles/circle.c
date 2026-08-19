#include <stdint.h>
#include "xprs_wasm_hal.h"
#include "circle.h"
#include "db.h"
#include "util.h"

/* The per-wapp datagram tag is the wapp id; the host adds it automatically, so
 * here we only ever build/parse the inner JSON datagram. */

#define MAXC        64      /* max circles tracked at once */
#define TAGLEN      16      /* circle routing tag = first 16 hex of circleId */
#define MAX_TEXT    200     /* message text cap (keeps a datagram in one packet) */

/* ── this device's identity (x-only pubkey, base64url) ────────────────── */
static char g_self[80];
static void ensure_self(void) {
  if (g_self[0]) return;
  hal_identity_pubkey(g_self, sizeof(g_self));
}

/* ── this device's RNS dests, for addressed member-to-member sync ──────── *
 * deliv = where peers hal_rns_send_to us; prop = where peers hal_rns_pull our
 * store-and-forwarded events. Advertised in the keyset so members can address
 * each other (reliable + NAT/inbound-tolerant) instead of best-effort cast. */
static char g_deliv[80];
static char g_prop[80];
static void ensure_dests(void) {
  if (!g_deliv[0]) hal_rns_delivery_dest(g_deliv, sizeof(g_deliv));
  if (!g_prop[0]) hal_rns_prop_dest(g_prop, sizeof(g_prop));
}

/* ── circle cache ─────────────────────────────────────────────────────── */
static int  g_index = -1;
static char c_id[MAXC][72];   /* circleId hex */
static char c_name[MAXC][48];
static int  c_h[MAXC];        /* open per-circle db handle (-1 = closed) */
static int  c_n = 0;

static void circle_short_code(const char *id, char *out, unsigned cap);

static int idx_by_id(const char *id) {
  for (int i = 0; i < c_n; i++) if (s_eq(c_id[i], id)) return i;
  return -1;
}
static int idx_by_tag(const char *tag) {
  for (int i = 0; i < c_n; i++) {
    int eq = 1;
    for (int j = 0; j < TAGLEN; j++) if (c_id[i][j] != tag[j]) { eq = 0; break; }
    if (eq) return i;
  }
  return -1;
}

/* ── tiny number helpers ──────────────────────────────────────────────── */
static void num(char *out, long v) {
  char tmp[24]; int n = 0; int neg = v < 0;
  unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
  if (u == 0) tmp[n++] = '0';
  while (u) { tmp[n++] = (char)('0' + (u % 10)); u /= 10; }
  int o = 0; if (neg) out[o++] = '-';
  while (n) out[o++] = tmp[--n];
  out[o] = 0;
}
static void pad2(char *out, int v) { out[0] = (char)('0' + (v / 10) % 10); out[1] = (char)('0' + v % 10); out[2] = 0; }
static void fmt_time(char *out, long ts) {
  long s = ((ts % 86400) + 86400) % 86400;
  char a[3], b[3], c[3];
  pad2(a, (int)(s / 3600)); pad2(b, (int)((s % 3600) / 60)); pad2(c, (int)(s % 60));
  out[0] = 0; s_cat(out, a, 12); s_cat(out, ":", 12); s_cat(out, b, 12);
  s_cat(out, ":", 12); s_cat(out, c, 12);
}
static const char *find_sub(const char *hay, const char *needle) {
  unsigned nl = s_len(needle);
  for (const char *p = hay; *p; p++) {
    unsigned i = 0; while (i < nl && p[i] == needle[i]) i++;
    if (i == nl) return p;
  }
  return 0;
}
/* ── UI (ui.convo.*) ──────────────────────────────────────────────────── */
static void notify(const char *level, const char *body) {
  char m[300] = "{\"type\":\"notify\",\"level\":\"";
  s_cat(m, level, sizeof(m));
  s_cat(m, "\",\"title\":\"Circles\",\"body\":\"", sizeof(m));
  jesc(m, sizeof(m), body);
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
static void convo_upsert(const char *id, const char *name, const char *preview,
                         int select) {
  char m[700] = "{\"type\":\"ui.convo.upsert\",\"id\":\"";
  jesc(m, sizeof(m), id);
  s_cat(m, "\",\"title\":\"", sizeof(m)); jesc(m, sizeof(m), name);
  /* Only set the subtitle when we actually have a preview — an empty one would
   * OVERWRITE the row's last-message text with nothing (the cause of every
   * circle showing "No messages yet" after a relaunch / circle edit). */
  if (preview && preview[0]) {
    s_cat(m, "\",\"subtitle\":\"", sizeof(m)); jesc(m, sizeof(m), preview);
  }
  s_cat(m, "\",\"icon\":\"group", sizeof(m));
  if (select) s_cat(m, "\",\"select\":true,\"bump\":true}", sizeof(m));
  else s_cat(m, "\",\"bump\":true}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
/* Short, friendly sender label from an author's base64url pubkey. */
static void author_label(const char *authb64, char *out, unsigned cap) {
  if (s_eq(authb64, g_self)) { s_cpy(out, "You", cap); return; }
  char np[80]; np[0] = 0;
  if (hal_npub(authb64, s_len(authb64), np, sizeof(np)) && np[0]) {
    /* npub1xxxx… — show a short, stable handle */
    s_cpy(out, np, cap);
    if (s_len(out) > 12) out[12] = 0;
  } else {
    s_cpy(out, authb64, cap);
    if (s_len(out) > 8) out[8] = 0;
  }
}
static void render_msg(const char *circleId, const char *authb64,
                       const char *text, long ts) {
  char from[40]; author_label(authb64, from, sizeof(from));
  char t[16]; fmt_time(t, ts);
  char m[1024] = "{\"type\":\"ui.convo.msg\",\"id\":\"";
  jesc(m, sizeof(m), circleId);
  s_cat(m, "\",\"dir\":\"", sizeof(m));
  s_cat(m, s_eq(authb64, g_self) ? "out" : "in", sizeof(m));
  s_cat(m, "\",\"from\":\"", sizeof(m)); jesc(m, sizeof(m), from);
  s_cat(m, "\",\"text\":\"", sizeof(m)); jesc(m, sizeof(m), text);
  s_cat(m, "\",\"time\":\"", sizeof(m)); s_cat(m, t, sizeof(m));
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* ── circle DB helpers ────────────────────────────────────────────────── */
static int open_circle_db(const char *id) {
  char path[120]; path[0] = 0;
  s_cat(path, "circles/c_", sizeof(path));
  s_cat(path, id, sizeof(path));
  s_cat(path, ".sqlite3", sizeof(path));
  int h = db_open(path);
  if (h >= 0) db_init_circle(h);
  return h;
}
static int handle_for(int i) {
  if (i < 0 || i >= c_n) return -1;
  if (c_h[i] < 0) c_h[i] = open_circle_db(c_id[i]);
  return c_h[i];
}
/* Register a circle in the cache (and open its db). Returns its index. */
static int cache_add(const char *id, const char *name) {
  int i = idx_by_id(id);
  if (i >= 0) { s_cpy(c_name[i], name, sizeof(c_name[i])); return i; }
  if (c_n >= MAXC) return -1;
  i = c_n++;
  s_cpy(c_id[i], id, sizeof(c_id[i]));
  s_cpy(c_name[i], name, sizeof(c_name[i]));
  c_h[i] = -1;
  return i;
}

/* Refresh circle [i]'s conversation-list row from its LAST decrypted message:
 * subtitle = the message text, badge = its time. So the list shows real recent
 * content (and orders by it) instead of "No messages yet" on every relaunch. If
 * the circle has no readable message yet, leaves the existing subtitle untouched
 * (an "Invited"/"New circle" placeholder or empty -> "No messages yet"). */
static void convo_refresh_last(int i) {
  int h = handle_for(i); if (h < 0) return;
  char rows[800];
  if (db_query(h, "SELECT body,ts FROM events WHERE body IS NOT NULL "
                  "ORDER BY ts DESC LIMIT 1", 0, rows, sizeof(rows)) <= 0) {
    convo_upsert(c_id[i], c_name[i], "", 0);   /* no message: keep current subtitle */
    return;
  }
  const char *cur = rows; char obj[800];
  if (!next_object(&cur, obj, sizeof(obj))) { convo_upsert(c_id[i], c_name[i], "", 0); return; }
  char body[600] = ""; long ts = 0;
  jstr(obj, "body", body, sizeof(body)); jint(obj, "ts", &ts);
  char t[16]; fmt_time(t, ts);
  char m[900] = "{\"type\":\"ui.convo.upsert\",\"id\":\"";
  jesc(m, sizeof(m), c_id[i]);
  s_cat(m, "\",\"title\":\"", sizeof(m)); jesc(m, sizeof(m), c_name[i]);
  s_cat(m, "\",\"subtitle\":\"", sizeof(m)); jesc(m, sizeof(m), body);
  s_cat(m, "\",\"badge\":\"", sizeof(m)); s_cat(m, t, sizeof(m));
  s_cat(m, "\",\"icon\":\"group\",\"bump\":true}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

static long meta_epoch(int h) {
  char out[128];
  if (db_query(h, "SELECT v FROM meta WHERE k='epoch'", 0, out, sizeof(out)) > 0) {
    char v[24]; if (jstr(out, "v", v, sizeof(v)) && v[0]) {
      long e = 0; for (const char *p = v; *p >= '0' && *p <= '9'; p++) e = e * 10 + (*p - '0');
      return e;
    }
  }
  return 0;
}
static void meta_set(int h, const char *k, const char *v) {
  char p[256] = "[\""; jesc(p, sizeof(p), k);
  s_cat(p, "\",\"", sizeof(p)); jesc(p, sizeof(p), v);
  s_cat(p, "\"]", sizeof(p));
  db_exec(h, "INSERT OR REPLACE INTO meta(k,v) VALUES(?,?)", p);
}
static void meta_set_i(int h, const char *k, long v) {
  char b[24]; num(b, v); meta_set(h, k, b);
}
static int meta_get(int h, const char *k, char *out, unsigned cap);
static long meta_get_i(int h, const char *k) {
  char v[24]; if (!meta_get(h, k, v, sizeof(v)) || !v[0]) return 0;
  long e = 0; int neg = (v[0] == '-'); const char *p = neg ? v + 1 : v;
  for (; *p >= '0' && *p <= '9'; p++) e = e * 10 + (*p - '0');
  return neg ? -e : e;
}
static int meta_get(int h, const char *k, char *out, unsigned cap) {
  out[0] = 0;
  char par[80] = "[\""; jesc(par, sizeof(par), k); s_cat(par, "\"]", sizeof(par));
  char row[2048];
  if (db_query(h, "SELECT v FROM meta WHERE k=?", par, row, sizeof(row)) <= 0)
    return 0;
  return jstr(row, "v", out, cap);
}

/* ── host UI helpers (panels) ─────────────────────────────────────────── */
static void field_set(const char *field, const char *val) {
  char m[2200] = "{\"type\":\"ui.field.set\",\"field\":\"";
  s_cat(m, field, sizeof(m));
  s_cat(m, "\",\"value\":\"", sizeof(m));
  jesc(m, sizeof(m), val);
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
static void field_set_raw(const char *field, const char *raw) {
  char m[256] = "{\"type\":\"ui.field.set\",\"field\":\"";
  s_cat(m, field, sizeof(m));
  s_cat(m, "\",\"value\":", sizeof(m));
  s_cat(m, raw, sizeof(m));
  s_cat(m, "}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
static void screen_open(const char *name) {
  char m[160] = "{\"type\":\"ui.screen.open\",\"name\":\"";
  s_cat(m, name, sizeof(m));
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
/* Open a panel screen with a dynamic AppBar title (e.g. the folder name). */
static void screen_open_title(const char *name, const char *title) {
  char m[256] = "{\"type\":\"ui.screen.open\",\"name\":\"";
  s_cat(m, name, sizeof(m));
  s_cat(m, "\",\"title\":\"", sizeof(m)); jesc(m, sizeof(m), title);
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
static void screen_close(void) {
  const char *m = "{\"type\":\"ui.screen.close\"}";
  hal_msg_send(m, s_len(m));
}

/* Do we hold the master key for circle [i] (i.e. own it)? */
static int owned(int i) {
  char par[80] = "[\""; s_cat(par, c_id[i], sizeof(par)); s_cat(par, "\"]", sizeof(par));
  char row[256];
  if (db_query(g_index, "SELECT master_priv FROM circles WHERE id=?", par, row,
               sizeof(row)) <= 0)
    return 0;
  char priv[80];
  return jstr(row, "master_priv", priv, sizeof(priv)) && priv[0];
}

/* Seed the default role set for a freshly-owned circle (idempotent). */
static void roles_seed_default(int i) {
  int h = handle_for(i); if (h < 0) return;
  char out[256];
  if (db_query(h, "SELECT name FROM roles LIMIT 1", 0, out, sizeof(out)) > 0 &&
      out[0] == '[' && out[1] != ']')
    return; /* already has roles */
  db_exec(h, "INSERT OR REPLACE INTO roles(name,created) VALUES('admin',0)", 0);
  db_exec(h, "INSERT OR REPLACE INTO roles(name,created) VALUES('member',0)", 0);
  meta_set(h, "default_role", "member");
}

static void people_set_members(int i); /* defined with the panel functions */
/* folder handlers + helpers, defined in the virtual-folders section below */
static void handle_fm(const char *json);
static void handle_fk(const char *json);
static void handle_fkr(const char *json);
static void handle_jr(const char *json);
static void handle_cd(const char *json);   /* short-code discovery request */
static void handle_co(const char *json);   /* circle offer (discovery reply) */
static int  deliver_to_members(int i, const char *d); /* addressed gossip */
static int  member_deliv(int i, const char *pub, char *out, unsigned cap);
static void folder_request_missing(int i);
static int  add_member_pub(int i, const char *memb);
static void rotate_and_distribute(int i);
static void audit_add(int i, const char *action, const char *who);
static int  member_status(int i, const char *pub, char *out, unsigned cap);

/* Fetch the 32-byte key for [epoch] of circle [i]. Returns 1 if held. */
static int epoch_key(int i, long epoch, unsigned char key[32]) {
  int h = handle_for(i); if (h < 0) return 0;
  char eb[24]; num(eb, epoch);
  char par[32] = "["; s_cat(par, eb, sizeof(par)); s_cat(par, "]", sizeof(par));
  char out[128];
  if (db_query(h, "SELECT key FROM epochs WHERE epoch=?", par, out, sizeof(out)) <= 0)
    return 0;
  char kb[64]; if (!jstr(out, "key", kb, sizeof(kb)) || !kb[0]) return 0;
  return b64url_decode(kb, key, 32) == 32;
}
static void epoch_store(int i, long epoch, const unsigned char key[32]) {
  int h = handle_for(i); if (h < 0) return;
  char kb[64]; if (b64url_encode(key, 32, kb, sizeof(kb)) < 0) return;
  char eb[24]; num(eb, epoch);
  char par[128] = "["; s_cat(par, eb, sizeof(par));
  s_cat(par, ",\"", sizeof(par)); s_cat(par, kb, sizeof(par)); s_cat(par, "\"]", sizeof(par));
  db_exec(h, "INSERT OR REPLACE INTO epochs(epoch,key) VALUES(?,?)", par);
}

/* Is [authb64] a member of circle [i]? If so and [added] != 0, set its added epoch. */
static int is_member(int i, const char *authb64, long *added) {
  int h = handle_for(i); if (h < 0) return 0;
  char par[80] = "[\""; jesc(par, sizeof(par), authb64); s_cat(par, "\"]", sizeof(par));
  char out[160];
  if (db_query(h, "SELECT added_epoch FROM members WHERE pub=?", par, out, sizeof(out)) <= 0)
    return 0;
  if (out[0] != '[' || out[1] == ']') return 0;
  if (added) { long a = 0; jint(out, "added_epoch", &a); *added = a; }
  return 1;
}

/* ── keyset (membership) ──────────────────────────────────────────────── */
/* Build the canonical keyset JSON the master signs: {"n":..,"e":..,"m":[..]} */
static void build_keyset_json(int i, char *out, unsigned cap) {
  int h = handle_for(i);
  out[0] = 0;
  s_cat(out, "{\"n\":\"", cap); jesc(out, cap, c_name[i]);
  char v[1024];
  s_cat(out, "\",\"d\":\"", cap);
  if (meta_get(h, "description", v, sizeof(v))) jesc(out, cap, v);
  s_cat(out, "\",\"pic\":\"", cap);
  if (meta_get(h, "picture", v, sizeof(v))) jesc(out, cap, v);
  s_cat(out, "\",\"dr\":\"", cap);
  if (meta_get(h, "default_role", v, sizeof(v))) jesc(out, cap, v);
  s_cat(out, "\",\"e\":", cap);
  char eb[24]; num(eb, meta_epoch(h)); s_cat(out, eb, cap);
  /* roles list */
  s_cat(out, ",\"r\":[", cap);
  char rrows[4096];
  int rfirst = 1;
  if (db_query(h, "SELECT name,description FROM roles ORDER BY name", 0, rrows, sizeof(rrows)) > 0) {
    const char *cur = rrows; char obj[512];
    while (next_object(&cur, obj, sizeof(obj))) {
      char nm[48], de[200]; if (!jstr(obj, "name", nm, sizeof(nm))) continue;
      de[0] = 0; jstr(obj, "description", de, sizeof(de));
      if (!rfirst) s_cat(out, ",", cap);
      rfirst = 0;
      s_cat(out, "{\"n\":\"", cap); jesc(out, cap, nm);
      s_cat(out, "\",\"d\":\"", cap); jesc(out, cap, de); s_cat(out, "\"}", cap);
    }
  }
  s_cat(out, "],\"m\":[", cap);
  char rows[8192];
  int first = 1;
  if (db_query(h, "SELECT pub,added_epoch,role,status,deliv,prop FROM members ORDER BY pub", 0,
               rows, sizeof(rows)) > 0) {
    const char *cur = rows; char obj[512];
    while (next_object(&cur, obj, sizeof(obj))) {
      char pub[64], role[48], st[24], dl[80], pp[80]; long a = 0;
      if (!jstr(obj, "pub", pub, sizeof(pub))) continue;
      jint(obj, "added_epoch", &a);
      role[0] = 0; jstr(obj, "role", role, sizeof(role));
      st[0] = 0; jstr(obj, "status", st, sizeof(st));
      dl[0] = 0; jstr(obj, "deliv", dl, sizeof(dl));
      pp[0] = 0; jstr(obj, "prop", pp, sizeof(pp));
      /* For our own entry (we build keysets only for circles we own), always
       * advertise our LIVE dests — the stored row may have been written before
       * the RNS node had a delivery dest, leaving members unable to reach us. */
      if (s_eq(pub, g_self)) {
        ensure_dests();
        if (g_deliv[0]) s_cpy(dl, g_deliv, sizeof(dl));
        if (g_prop[0]) s_cpy(pp, g_prop, sizeof(pp));
      }
      if (!first) s_cat(out, ",", cap);
      first = 0;
      s_cat(out, "{\"p\":\"", cap); jesc(out, cap, pub);
      s_cat(out, "\",\"a\":", cap); char ab[24]; num(ab, a); s_cat(out, ab, cap);
      s_cat(out, ",\"ro\":\"", cap); jesc(out, cap, role);
      s_cat(out, "\",\"st\":\"", cap); jesc(out, cap, st);
      s_cat(out, "\",\"dl\":\"", cap); jesc(out, cap, dl);
      s_cat(out, "\",\"pp\":\"", cap); jesc(out, cap, pp);
      s_cat(out, "\"}", cap);
    }
  }
  /* folders (structure + permissions; the per-folder KEYS travel separately) */
  s_cat(out, "],\"fo\":[", cap);
  static char frows[8192];
  int ffirst = 1;
  if (db_query(h, "SELECT id,parent,name,type,description,icon,roles,allow,deny,epoch "
                  "FROM folders ORDER BY created", 0, frows, sizeof(frows)) > 0) {
    const char *cur = frows; char fobj[6000];
    while (next_object(&cur, fobj, sizeof(fobj))) {
      char id[40], pa[40], nm[64], ty[16], dsc[256], ic[256], ro[256], al[2048], de[2048]; long ep = 0;
      if (!jstr(fobj, "id", id, sizeof(id))) continue;
      pa[0] = 0; jstr(fobj, "parent", pa, sizeof(pa));
      nm[0] = 0; jstr(fobj, "name", nm, sizeof(nm));
      ty[0] = 0; jstr(fobj, "type", ty, sizeof(ty));
      dsc[0] = 0; jstr(fobj, "description", dsc, sizeof(dsc));
      ic[0] = 0; jstr(fobj, "icon", ic, sizeof(ic));
      ro[0] = 0; jstr(fobj, "roles", ro, sizeof(ro));
      al[0] = 0; jstr(fobj, "allow", al, sizeof(al));
      de[0] = 0; jstr(fobj, "deny", de, sizeof(de));
      jint(fobj, "epoch", &ep);
      if (!ffirst) s_cat(out, ",", cap);
      ffirst = 0;
      s_cat(out, "{\"id\":\"", cap); jesc(out, cap, id);
      s_cat(out, "\",\"pa\":\"", cap); jesc(out, cap, pa);
      s_cat(out, "\",\"n\":\"", cap); jesc(out, cap, nm);
      s_cat(out, "\",\"ty\":\"", cap); jesc(out, cap, ty);
      s_cat(out, "\",\"ds\":\"", cap); jesc(out, cap, dsc);
      s_cat(out, "\",\"ic\":\"", cap); jesc(out, cap, ic);
      s_cat(out, "\",\"ro\":\"", cap); jesc(out, cap, ro);
      s_cat(out, "\",\"al\":\"", cap); jesc(out, cap, al);
      s_cat(out, "\",\"de\":\"", cap); jesc(out, cap, de);
      char eb[24]; num(eb, ep);
      s_cat(out, "\",\"e\":", cap); s_cat(out, eb, cap);
      s_cat(out, "}", cap);
    }
  }
  s_cat(out, "]}", cap);
}

/* Sign + broadcast the current keyset of circle [i] (owner only). */
static void broadcast_keyset(int i) {
  /* master priv from the index */
  char par[80] = "[\""; s_cat(par, c_id[i], sizeof(par)); s_cat(par, "\"]", sizeof(par));
  char row[256];
  if (db_query(g_index, "SELECT master_priv FROM circles WHERE id=?", par, row, sizeof(row)) <= 0)
    return;
  char priv[80]; if (!jstr(row, "master_priv", priv, sizeof(priv)) || !priv[0]) return;
  /* static scratch — the keyset (now carrying folders too) can be large; keep it
   * off the small wasm stack. */
  static char ks[16384];
  build_keyset_json(i, ks, sizeof(ks));
  char sig[160];
  if (hal_crypto_sign(priv, s_len(priv), ks, s_len(ks), sig, sizeof(sig)) == 0) return;
  static char ksb64[22000];
  if (b64url_encode((const unsigned char *)ks, s_len(ks), ksb64, sizeof(ksb64)) < 0) return;
  static char d[24000];
  s_cpy(d, "{\"k\":\"ks\",\"cid\":\"", sizeof(d));
  s_cat(d, c_id[i], sizeof(d));
  s_cat(d, "\",\"ks\":\"", sizeof(d)); s_cat(d, ksb64, sizeof(d));
  s_cat(d, "\",\"sig\":\"", sizeof(d)); s_cat(d, sig, sizeof(d));
  s_cat(d, "\"}", sizeof(d));
  hal_rns_broadcast(d, s_len(d));
  deliver_to_members(i, d);   /* reliably push the keyset to known members */
}

/* Look up a member's delivery dest (hex) into [out]; 1 if found+non-empty. */
static int member_deliv(int i, const char *pub, char *out, unsigned cap) {
  out[0] = 0;
  int h = handle_for(i); if (h < 0) return 0;
  char par[80] = "[\""; jesc(par, sizeof(par), pub); s_cat(par, "\"]", sizeof(par));
  char row[160];
  if (db_query(h, "SELECT deliv FROM members WHERE pub=?", par, row, sizeof(row)) <= 0)
    return 0;
  return jstr(row, "deliv", out, cap) && out[0];
}

/* Wrap epoch [e]'s key to member [tob64] and send a "key" datagram (addressed
 * to the member if we know its dest, plus broadcast as fallback). */
static void send_wrapped_key(int i, long e, const char *tob64) {
  if (s_eq(tob64, g_self)) return; /* we already hold our own keys */
  unsigned char key[32]; if (!epoch_key(i, e, key)) return;
  char blob[200];
  if (hal_encrypt(tob64, s_len(tob64), (const char *)key, 32, blob, sizeof(blob)) == 0)
    return;
  char eb[24]; num(eb, e);
  char d[512] = "{\"k\":\"key\",\"c\":\"";
  for (int j = 0; j < TAGLEN; j++) { char ch[2] = { c_id[i][j], 0 }; s_cat(d, ch, sizeof(d)); }
  s_cat(d, "\",\"e\":", sizeof(d)); s_cat(d, eb, sizeof(d));
  s_cat(d, ",\"to\":\"", sizeof(d)); s_cat(d, tob64, sizeof(d));
  s_cat(d, "\",\"frm\":\"", sizeof(d)); s_cat(d, g_self, sizeof(d));
  s_cat(d, "\",\"b\":\"", sizeof(d)); s_cat(d, blob, sizeof(d));
  s_cat(d, "\"}", sizeof(d));
  hal_rns_broadcast(d, s_len(d));
  char dl[80];
  if (member_deliv(i, tob64, dl, sizeof(dl)))
    hal_rns_send_to(dl, s_len(dl), d, s_len(d)); /* reliable to that member */
}

/* ── message events ───────────────────────────────────────────────────── */
static void canonical(char *out, unsigned cap, const char *tag, long e,
                      const char *author, long ts, const char *ct) {
  out[0] = 0;
  for (int j = 0; j < TAGLEN; j++) { char ch[2] = { tag[j], 0 }; s_cat(out, ch, cap); }
  char b[24];
  s_cat(out, "|", cap); num(b, e); s_cat(out, b, cap);
  s_cat(out, "|", cap); s_cat(out, author, cap);
  s_cat(out, "|", cap); num(b, ts); s_cat(out, b, cap);
  s_cat(out, "|", cap); s_cat(out, ct, cap);
}
static void event_id(const char *sig, char *out) {
  for (int j = 0; j < 16; j++) out[j] = sig[j];
  out[16] = 0;
}
static int event_exists(int h, const char *id) {
  char par[40] = "[\""; s_cat(par, id, sizeof(par)); s_cat(par, "\"]", sizeof(par));
  char out[64];
  return db_query(h, "SELECT id FROM events WHERE id=?", par, out, sizeof(out)) > 0
         && out[0] == '[' && out[1] != ']';
}
/* Store one event row. body may be NULL (not yet decryptable). */
static void event_store(int h, const char *id, long e, const char *author,
                        long ts, const char *ct, const char *sig,
                        const char *body) {
  char par[2048] = "[\""; jesc(par, sizeof(par), id);
  s_cat(par, "\",", sizeof(par)); char b[24]; num(b, e); s_cat(par, b, sizeof(par));
  s_cat(par, ",\"", sizeof(par)); jesc(par, sizeof(par), author);
  s_cat(par, "\",", sizeof(par)); num(b, ts); s_cat(par, b, sizeof(par));
  s_cat(par, ",\"", sizeof(par)); jesc(par, sizeof(par), ct);
  s_cat(par, "\",\"", sizeof(par)); jesc(par, sizeof(par), sig);
  s_cat(par, "\",", sizeof(par));
  if (body) { s_cat(par, "\"", sizeof(par)); jesc(par, sizeof(par), body); s_cat(par, "\"", sizeof(par)); }
  else s_cat(par, "null", sizeof(par));
  s_cat(par, "]", sizeof(par));
  db_exec(h, "INSERT OR REPLACE INTO events(id,epoch,author,ts,ct,sig,body) "
             "VALUES(?,?,?,?,?,?,?)", par);
}
/* Try to decrypt ct (base64url IV||ct) of epoch e for circle i. 1 on success. */
static int try_decrypt(int i, long e, const char *ctb64, char *out, unsigned cap) {
  unsigned char key[32]; if (!epoch_key(i, e, key)) return 0;
  unsigned char blob[1024];
  int bn = b64url_decode(ctb64, blob, sizeof(blob));
  if (bn <= 16) return 0;
  uint32_t n = hal_crypto_aes_decrypt((const char *)key, 32, (const char *)blob,
                                      (uint32_t)bn, out, cap - 1);
  if (n == 0) return 0;
  out[n] = 0;
  return 1;
}
/* After learning epoch [e]'s key, decrypt+render any pending events. */
static void decrypt_pending(int i, long e) {
  int h = handle_for(i); if (h < 0) return;
  char eb[24]; num(eb, e);
  char par[32] = "["; s_cat(par, eb, sizeof(par)); s_cat(par, "]", sizeof(par));
  char rows[8192];
  if (db_query(h, "SELECT id,author,ts,ct FROM events "
                  "WHERE epoch=? AND body IS NULL ORDER BY ts", par, rows, sizeof(rows)) <= 0)
    return;
  const char *cur = rows; char obj[1024];
  while (next_object(&cur, obj, sizeof(obj))) {
    char id[40], author[64], ct[800]; long ts = 0;
    if (!jstr(obj, "id", id, sizeof(id))) continue;
    jstr(obj, "author", author, sizeof(author));
    jstr(obj, "ct", ct, sizeof(ct));
    jint(obj, "ts", &ts);
    char body[600];
    if (!try_decrypt(i, e, ct, body, sizeof(body))) continue;
    char up[640] = "[\""; jesc(up, sizeof(up), body); s_cat(up, "\",\"", sizeof(up));
    jesc(up, sizeof(up), id); s_cat(up, "\"]", sizeof(up));
    db_exec(h, "UPDATE events SET body=? WHERE id=?", up);
    render_msg(c_id[i], author, body, ts);
  }
}

/* ── datagram handlers ────────────────────────────────────────────────── */
static void send_keyreq(int i) {
  char d[256] = "{\"k\":\"kr\",\"c\":\"";
  for (int j = 0; j < TAGLEN; j++) { char ch[2] = { c_id[i][j], 0 }; s_cat(d, ch, sizeof(d)); }
  s_cat(d, "\",\"frm\":\"", sizeof(d)); s_cat(d, g_self, sizeof(d));
  s_cat(d, "\"}", sizeof(d));
  hal_rns_broadcast(d, s_len(d));
  deliver_to_members(i, d);   /* reliable: stored for the owner to pull if needed */
}

/* Ask the circle (the owner replays past events via handle_req) for history
 * since [since]. Needed after a fresh join: a brand-new member holds no event
 * rows yet, so the key-driven catch-up in circle_tick alone never fires. */
static void send_history_req(int i, long since) {
  char d[256] = "{\"k\":\"rq\",\"c\":\"";
  for (int j = 0; j < TAGLEN; j++) { char ch[2] = { c_id[i][j], 0 }; s_cat(d, ch, sizeof(d)); }
  char b[24]; s_cat(d, "\",\"since\":", sizeof(d)); num(b, since); s_cat(d, b, sizeof(d));
  s_cat(d, "}", sizeof(d));
  hal_rns_broadcast(d, s_len(d));
  deliver_to_members(i, d);   /* reliable: stored for members to pull if needed */
}

/* Deliver datagram [d] ADDRESSED to every other member whose delivery dest we
 * know (reliable + store-and-forward), so the circle syncs member-to-member
 * regardless of who is online or whose inbound is reachable. Returns the count
 * addressed. Callers still broadcast as a best-effort fallback. */
static int deliver_to_members(int i, const char *d) {
  int h = handle_for(i); if (h < 0) return 0;
  static char rows[16384];
  if (db_query(h, "SELECT pub,deliv FROM members "
                  "WHERE deliv IS NOT NULL AND deliv!=''", 0, rows, sizeof(rows)) <= 0)
    return 0;
  const char *cur = rows; char obj[256]; int n = 0;
  while (next_object(&cur, obj, sizeof(obj))) {
    char pub[64], dl[80];
    if (!jstr(obj, "pub", pub, sizeof(pub))) continue;
    if (s_eq(pub, g_self)) continue;            /* never to ourselves */
    if (!jstr(obj, "deliv", dl, sizeof(dl)) || !dl[0]) continue;
    hal_rns_send_to(dl, s_len(dl), d, s_len(d));
    n++;
  }
  return n;
}

/* Pull store-and-forwarded events from every member's mailbox so we converge
 * even when peers couldn't reach us directly (we initiate, so inbound asymmetry
 * doesn't matter). Called from circle_tick. */
static void pull_from_members(int i) {
  int h = handle_for(i); if (h < 0) return;
  static char rows[16384];
  if (db_query(h, "SELECT pub,prop FROM members "
                  "WHERE prop IS NOT NULL AND prop!=''", 0, rows, sizeof(rows)) <= 0)
    return;
  const char *cur = rows; char obj[256];
  while (next_object(&cur, obj, sizeof(obj))) {
    char pub[64], pp[80];
    if (!jstr(obj, "pub", pub, sizeof(pub))) continue;
    if (s_eq(pub, g_self)) continue;
    if (!jstr(obj, "prop", pp, sizeof(pp)) || !pp[0]) continue;
    hal_rns_pull(pp, s_len(pp));
  }
}

static void handle_msg(const char *json) {
  char tag[24], author[64], ct[800], sig[160]; long e = 0, ts = 0;
  if (!jstr(json, "c", tag, sizeof(tag))) return;
  int i = idx_by_tag(tag); if (i < 0) return;
  jint(json, "e", &e); jint(json, "t", &ts);
  jstr(json, "a", author, sizeof(author));
  jstr(json, "x", ct, sizeof(ct));
  jstr(json, "s", sig, sizeof(sig));
  if (!author[0] || !sig[0]) return;
  if (s_eq(author, g_self)) return;            /* our own echo */
  if (!is_member(i, author, 0)) return;        /* not an authorised author */
  char canon[1024]; canonical(canon, sizeof(canon), tag, e, author, ts, ct);
  if (!hal_verify(author, s_len(author), canon, s_len(canon), sig, s_len(sig))) return;
  int h = handle_for(i); if (h < 0) return;
  char id[20]; event_id(sig, id);
  if (event_exists(h, id)) return;             /* dedup */
  char body[600];
  int dec = try_decrypt(i, e, ct, body, sizeof(body));
  event_store(h, id, e, author, ts, ct, sig, dec ? body : 0);
  if (dec) {
    render_msg(c_id[i], author, body, ts);
    convo_refresh_last(i);
  } else {
    send_keyreq(i);                            /* missing key — ask for it */
  }
}

static void handle_keyset(const char *json) {
  char cid[80], sig[160];
  static char ksb64[22000];
  if (!jstr(json, "cid", cid, sizeof(cid))) return;
  if (!jstr(json, "ks", ksb64, sizeof(ksb64))) return;
  if (!jstr(json, "sig", sig, sizeof(sig))) return;
  static unsigned char ksbytes[16384];
  int kn = b64url_decode(ksb64, ksbytes, sizeof(ksbytes) - 1);
  if (kn <= 0) return;
  ksbytes[kn] = 0;
  const char *ks = (const char *)ksbytes;
  if (!hal_crypto_verify(cid, s_len(cid), sig, s_len(sig), ks, (unsigned)kn)) return;
  char name[48]; long e = 0;
  jstr(ks, "n", name, sizeof(name));
  jint(ks, "e", &e);
  /* Are we a member? */
  int self_member = 0;
  const char *m = find_sub(ks, "\"m\":[");
  if (!m) return;
  const char *cur = m + 4; char obj[256];
  /* first pass: membership check */
  while (next_object(&cur, obj, sizeof(obj))) {
    char p[64]; if (jstr(obj, "p", p, sizeof(p)) && s_eq(p, g_self)) { self_member = 1; }
  }
  int i = idx_by_id(cid);
  if (i < 0) {
    if (!self_member) return;
    /* Join: create local circle (no master key — we don't own it). */
    char par[160] = "[\""; jesc(par, sizeof(par), cid);
    s_cat(par, "\",\"", sizeof(par)); jesc(par, sizeof(par), name);
    s_cat(par, "\",\"\",", sizeof(par)); char tb[24]; num(tb, (long)hal_time_epoch());
    s_cat(par, tb, sizeof(par)); s_cat(par, "]", sizeof(par));
    db_exec(g_index, "INSERT OR REPLACE INTO circles(id,name,master_priv,created) "
                     "VALUES(?,?,?,?)", par);
    i = cache_add(cid, name);
    if (i < 0) return;
    convo_upsert(cid, name, "Invited", 0);
    /* Fresh join: pull the existing message history (we hold no events yet, so
     * the key-only catch-up in circle_tick would never request it). */
    send_history_req(i, 0);
  }
  int h = handle_for(i); if (h < 0) return;
  if (e < meta_epoch(h)) return;               /* stale keyset */
  /* Replace membership + meta (with roles). */
  db_exec(h, "DELETE FROM members", 0);
  cur = m + 4;
  while (next_object(&cur, obj, sizeof(obj))) {
    char p[64], role[48], st[24], dl[80], pp[80]; long a = 0;
    if (!jstr(obj, "p", p, sizeof(p))) continue;
    jint(obj, "a", &a);
    role[0] = 0; jstr(obj, "ro", role, sizeof(role));
    st[0] = 0; jstr(obj, "st", st, sizeof(st));
    dl[0] = 0; jstr(obj, "dl", dl, sizeof(dl));
    pp[0] = 0; jstr(obj, "pp", pp, sizeof(pp));
    char par[420] = "[\""; jesc(par, sizeof(par), p); s_cat(par, "\",", sizeof(par));
    char ab[24]; num(ab, a); s_cat(par, ab, sizeof(par));
    s_cat(par, ",\"", sizeof(par)); jesc(par, sizeof(par), role);
    s_cat(par, "\",\"", sizeof(par)); jesc(par, sizeof(par), st);
    s_cat(par, "\",\"", sizeof(par)); jesc(par, sizeof(par), dl);
    s_cat(par, "\",\"", sizeof(par)); jesc(par, sizeof(par), pp); s_cat(par, "\"]", sizeof(par));
    db_exec(h, "INSERT OR REPLACE INTO members(pub,added_epoch,revoked_epoch,role,status,deliv,prop) "
               "VALUES(?,?,NULL,?,?,?,?)", par);
  }
  /* Replace the role set from the keyset's "r":[{n,d},..] array. Copy the
   * balanced [..] segment first so the object iterator doesn't run on into the
   * following "m" array. */
  const char *rp = find_sub(ks, "\"r\":[");
  if (rp) {
    char rseg[4096]; unsigned ri = 0; int rdepth = 0, rinstr = 0;
    for (const char *q = rp + 4; *q; q++) {
      char c = *q;
      if (ri < sizeof(rseg) - 1) rseg[ri++] = c;
      if (rinstr) {
        if (c == '\\' && q[1]) { if (ri < sizeof(rseg) - 1) rseg[ri++] = q[1]; q++; continue; }
        if (c == '"') rinstr = 0;
      } else {
        if (c == '"') rinstr = 1;
        else if (c == '[') rdepth++;
        else if (c == ']') { rdepth--; if (rdepth == 0) break; }
      }
    }
    rseg[ri] = 0;
    db_exec(h, "DELETE FROM roles", 0);
    const char *cur2 = rseg; char robj[512];
    while (next_object(&cur2, robj, sizeof(robj))) {
      char nm[48], de[200]; if (!jstr(robj, "n", nm, sizeof(nm))) continue;
      de[0] = 0; jstr(robj, "d", de, sizeof(de));
      char par[400] = "[\""; jesc(par, sizeof(par), nm); s_cat(par, "\",\"", sizeof(par));
      jesc(par, sizeof(par), de); s_cat(par, "\"]", sizeof(par));
      db_exec(h, "INSERT OR REPLACE INTO roles(name,description,created) VALUES(?,?,0)", par);
    }
  }
  /* Replace folders metadata from "fo":[..] (per-folder keys/events live in
   * separate tables keyed by folder id and are preserved across keyset reloads). */
  const char *fp = find_sub(ks, "\"fo\":[");
  if (fp) {
    static char fseg[8192]; unsigned fi = 0; int fdepth = 0, finstr = 0;
    for (const char *q = fp + 5; *q; q++) {
      char c = *q;
      if (fi < sizeof(fseg) - 1) fseg[fi++] = c;
      if (finstr) {
        if (c == '\\' && q[1]) { if (fi < sizeof(fseg) - 1) fseg[fi++] = q[1]; q++; continue; }
        if (c == '"') finstr = 0;
      } else {
        if (c == '"') finstr = 1;
        else if (c == '[') fdepth++;
        else if (c == ']') { fdepth--; if (fdepth == 0) break; }
      }
    }
    fseg[fi] = 0;
    db_exec(h, "DELETE FROM folders", 0);
    const char *fc = fseg; static char fobj[6000];
    while (next_object(&fc, fobj, sizeof(fobj))) {
      char id[40], pa[40], nm[64], ty[16], dsc[256], ic[256], ro[256], al[2048], de[2048]; long ep = 0;
      if (!jstr(fobj, "id", id, sizeof(id))) continue;
      pa[0] = 0; jstr(fobj, "pa", pa, sizeof(pa));
      nm[0] = 0; jstr(fobj, "n", nm, sizeof(nm));
      ty[0] = 0; jstr(fobj, "ty", ty, sizeof(ty));
      dsc[0] = 0; jstr(fobj, "ds", dsc, sizeof(dsc));
      ic[0] = 0; jstr(fobj, "ic", ic, sizeof(ic));
      ro[0] = 0; jstr(fobj, "ro", ro, sizeof(ro));
      al[0] = 0; jstr(fobj, "al", al, sizeof(al));
      de[0] = 0; jstr(fobj, "de", de, sizeof(de));
      jint(fobj, "e", &ep);
      static char fpar[5200];
      s_cpy(fpar, "[\"", sizeof(fpar)); jesc(fpar, sizeof(fpar), id);
      s_cat(fpar, "\",\"", sizeof(fpar)); jesc(fpar, sizeof(fpar), pa);
      s_cat(fpar, "\",\"", sizeof(fpar)); jesc(fpar, sizeof(fpar), nm);
      s_cat(fpar, "\",\"", sizeof(fpar)); jesc(fpar, sizeof(fpar), ty);
      s_cat(fpar, "\",\"", sizeof(fpar)); jesc(fpar, sizeof(fpar), dsc);
      s_cat(fpar, "\",\"", sizeof(fpar)); jesc(fpar, sizeof(fpar), ic);
      s_cat(fpar, "\",\"", sizeof(fpar)); jesc(fpar, sizeof(fpar), ro);
      s_cat(fpar, "\",\"", sizeof(fpar)); jesc(fpar, sizeof(fpar), al);
      s_cat(fpar, "\",\"", sizeof(fpar)); jesc(fpar, sizeof(fpar), de);
      s_cat(fpar, "\",", sizeof(fpar)); char eb[24]; num(eb, ep); s_cat(fpar, eb, sizeof(fpar));
      s_cat(fpar, ",0]", sizeof(fpar));
      db_exec(h, "INSERT OR REPLACE INTO folders"
                 "(id,parent,name,type,description,icon,roles,allow,deny,epoch,created) "
                 "VALUES(?,?,?,?,?,?,?,?,?,?,?)", fpar);
    }
  }
  char dv[1024];
  meta_set(h, "name", name);
  dv[0] = 0; jstr(ks, "d", dv, sizeof(dv)); meta_set(h, "description", dv);
  dv[0] = 0; jstr(ks, "pic", dv, sizeof(dv)); meta_set(h, "picture", dv);
  dv[0] = 0; jstr(ks, "dr", dv, sizeof(dv)); if (dv[0]) meta_set(h, "default_role", dv);
  meta_set_i(h, "epoch", e);
  s_cpy(c_name[i], name, sizeof(c_name[i]));
  /* Ask for any epoch keys we still lack (covers the offline-rejoin case). */
  send_keyreq(i);
  /* And request keys for any folders we're now permitted to enter. */
  folder_request_missing(i);
  /* A refreshed keyset may carry member dests we lacked before (e.g. the owner's
   * live delivery dest), so reset the history retry budget and ask again now that
   * we can actually reach a member — even if we already hold some events (our own
   * posts), we may still be missing the owner's earlier history. */
  if (!owned(i)) { meta_set_i(h, "histtries", 0); send_history_req(i, 0); }
}

static void handle_key(const char *json) {
  char tag[24], to[64], frm[64], blob[300]; long e = 0;
  if (!jstr(json, "c", tag, sizeof(tag))) return;
  jstr(json, "to", to, sizeof(to));
  if (!s_eq(to, g_self)) return;               /* not addressed to us */
  jint(json, "e", &e);
  jstr(json, "frm", frm, sizeof(frm));
  jstr(json, "b", blob, sizeof(blob));
  int i = idx_by_tag(tag); if (i < 0 || !frm[0] || !blob[0]) return;
  char key[64];
  uint32_t n = hal_decrypt(frm, s_len(frm), blob, s_len(blob), key, sizeof(key));
  if (n != 32) return;
  epoch_store(i, e, (const unsigned char *)key);
  decrypt_pending(i, e);
}

static void handle_keyreq(const char *json) {
  char tag[24], frm[64];
  if (!jstr(json, "c", tag, sizeof(tag))) return;
  jstr(json, "frm", frm, sizeof(frm));
  int i = idx_by_tag(tag); if (i < 0 || !frm[0]) return;
  if (s_eq(frm, g_self)) return;
  long added = 0;
  if (!is_member(i, frm, &added)) return;      /* only serve authorised members */
  int h = handle_for(i); if (h < 0) return;
  (void)added;
  long top = meta_epoch(h);
  /* Serve the FULL epoch-key history (from 1), not just from the member's
   * added epoch: a newly-approved member is expected to read the circle's
   * previous messages, which were encrypted under earlier epoch keys.
   * send_wrapped_key skips epochs whose key we don't hold. */
  for (long e = 1; e <= top; e++) send_wrapped_key(i, e, frm);
}

static void handle_req(const char *json) {
  char tag[24]; long since = 0;
  if (!jstr(json, "c", tag, sizeof(tag))) return;
  jint(json, "since", &since);
  int i = idx_by_tag(tag); if (i < 0) return;
  int h = handle_for(i); if (h < 0) return;
  char sb[24]; num(sb, since);
  char par[32] = "["; s_cat(par, sb, sizeof(par)); s_cat(par, "]", sizeof(par));
  char rows[16384];
  if (db_query(h, "SELECT epoch,author,ts,ct,sig FROM events WHERE ts>? "
                  "ORDER BY ts LIMIT 50", par, rows, sizeof(rows)) <= 0)
    return;
  const char *cur = rows; char obj[1024];
  while (next_object(&cur, obj, sizeof(obj))) {
    char author[64], ct[800], sig[160]; long e = 0, ts = 0;
    jint(obj, "epoch", &e); jint(obj, "ts", &ts);
    jstr(obj, "author", author, sizeof(author));
    jstr(obj, "ct", ct, sizeof(ct));
    jstr(obj, "sig", sig, sizeof(sig));
    char d[1200] = "{\"k\":\"msg\",\"c\":\"";
    for (int j = 0; j < TAGLEN; j++) { char ch[2] = { c_id[i][j], 0 }; s_cat(d, ch, sizeof(d)); }
    char b[24];
    s_cat(d, "\",\"e\":", sizeof(d)); num(b, e); s_cat(d, b, sizeof(d));
    s_cat(d, ",\"a\":\"", sizeof(d)); s_cat(d, author, sizeof(d));
    s_cat(d, "\",\"t\":", sizeof(d)); num(b, ts); s_cat(d, b, sizeof(d));
    s_cat(d, ",\"x\":\"", sizeof(d)); s_cat(d, ct, sizeof(d));
    s_cat(d, "\",\"s\":\"", sizeof(d)); s_cat(d, sig, sizeof(d));
    s_cat(d, "\"}", sizeof(d));
    hal_rns_broadcast(d, s_len(d));
    deliver_to_members(i, d);   /* reliable history replay to members */
  }
}

/* ── public API ───────────────────────────────────────────────────────── */
void circle_init(void) {
  ensure_self();
  g_index = db_open("circles/index.sqlite3");
  if (g_index < 0) { notify("error", "Storage unavailable"); return; }
  db_init_index(g_index);
  char rows[8192];
  if (db_query(g_index, "SELECT id,name FROM circles ORDER BY created", 0,
               rows, sizeof(rows)) > 0) {
    const char *cur = rows; char obj[256];
    while (next_object(&cur, obj, sizeof(obj))) {
      char id[72], name[48];
      if (!jstr(obj, "id", id, sizeof(id))) continue;
      jstr(obj, "name", name, sizeof(name));
      int i = cache_add(id, name);
      if (i < 0) continue;
      convo_refresh_last(i);   /* show the last message, not "No messages yet" */
      /* Backfill roles for circles we own that predate the roles feature. */
      if (owned(i)) {
        roles_seed_default(i);
        int h = handle_for(i);
        if (h >= 0) {
          char par[80] = "[\""; jesc(par, sizeof(par), g_self); s_cat(par, "\"]", sizeof(par));
          db_exec(h, "UPDATE members SET role='admin' "
                     "WHERE pub=? AND (role IS NULL OR role='')", par);
        }
      } else {
        /* Re-arm the bounded history pull on every launch so a member that
         * exhausted its retries earlier (e.g. before it could reach the owner)
         * converges on the next start. circle_tick does the actual requests;
         * handle_msg dedups, so re-pulling is safe. */
        int h = handle_for(i);
        if (h >= 0) meta_set_i(h, "histtries", 0);
      }
    }
  }
}

int circle_create(const char *name) {
  ensure_self();
  if (g_index < 0 || !g_self[0]) { notify("error", "No identity / storage"); return 0; }
  char kg[300];
  if (hal_crypto_keygen(kg, sizeof(kg)) == 0) { notify("error", "Key generation failed"); return 0; }
  char priv[80], pub[80];
  if (!jstr(kg, "priv", priv, sizeof(priv)) || !jstr(kg, "pub", pub, sizeof(pub))) return 0;
  long now = (long)hal_time_epoch();
  /* index row (we own it → store master_priv) */
  char par[300] = "[\""; jesc(par, sizeof(par), pub);
  s_cat(par, "\",\"", sizeof(par)); jesc(par, sizeof(par), name);
  s_cat(par, "\",\"", sizeof(par)); jesc(par, sizeof(par), priv);
  s_cat(par, "\",", sizeof(par)); char nb[24]; num(nb, now); s_cat(par, nb, sizeof(par));
  s_cat(par, "]", sizeof(par));
  db_exec(g_index, "INSERT OR REPLACE INTO circles(id,name,master_priv,created) "
                   "VALUES(?,?,?,?)", par);
  int i = cache_add(pub, name);
  if (i < 0) return 0;
  int h = handle_for(i); if (h < 0) return 0;
  meta_set(h, "name", name);
  meta_set_i(h, "epoch", 1);
  /* epoch 1 key */
  unsigned char key[32]; hal_crypto_random((char *)key, 32);
  epoch_store(i, 1, key);
  /* default roles; the creator is the admin */
  roles_seed_default(i);
  /* self as first member (admin), advertising our RNS dests */
  ensure_dests();
  char mp[280] = "[\""; jesc(mp, sizeof(mp), g_self);
  s_cat(mp, "\",\"admin\",\"", sizeof(mp)); jesc(mp, sizeof(mp), g_deliv);
  s_cat(mp, "\",\"", sizeof(mp)); jesc(mp, sizeof(mp), g_prop); s_cat(mp, "\"]", sizeof(mp));
  db_exec(h, "INSERT OR REPLACE INTO members(pub,added_epoch,revoked_epoch,role,deliv,prop) "
             "VALUES(?,1,NULL,?,?,?)", mp);
  convo_upsert(pub, name, "New circle", 1);
  notify("info", "Circle created");
  return 1;
}

/* Members entitled to keys: active/inactive (NOT suspended/banned). */
#define ELIGIBLE_WHERE \
  "(status IS NULL OR status='' OR status='active' OR status='inactive')"

static void audit_add(int i, const char *action, const char *who) {
  int h = handle_for(i); if (h < 0) return;
  char par[200] = "["; char tb[24]; num(tb, (long)hal_time_epoch());
  s_cat(par, tb, sizeof(par)); s_cat(par, ",\"", sizeof(par)); jesc(par, sizeof(par), action);
  s_cat(par, "\",\"", sizeof(par)); jesc(par, sizeof(par), who); s_cat(par, "\"]", sizeof(par));
  db_exec(h, "INSERT INTO audit(ts,action,who) VALUES(?,?,?)", par);
}
static int member_status(int i, const char *pub, char *out, unsigned cap) {
  out[0] = 0; int h = handle_for(i); if (h < 0) return 0;
  char par[80] = "[\""; jesc(par, sizeof(par), pub); s_cat(par, "\"]", sizeof(par));
  char row[160];
  if (db_query(h, "SELECT status FROM members WHERE pub=?", par, row, sizeof(row)) <= 0) return 0;
  if (row[0] != '[' || row[1] == ']') return 0;
  jstr(row, "status", out, cap);
  if (!out[0]) s_cpy(out, "active", cap);
  return 1;
}
/* Bump the circle epoch to a fresh key and hand it only to ELIGIBLE members
 * (active/inactive) — so suspended/banned members lose forward access. */
static void rotate_and_distribute(int i) {
  int h = handle_for(i); if (h < 0) return;
  long newE = meta_epoch(h) + 1;
  unsigned char key[32]; hal_crypto_random((char *)key, 32);
  epoch_store(i, newE, key);
  meta_set_i(h, "epoch", newE);
  char rows[8192];
  if (db_query(h, "SELECT pub FROM members WHERE " ELIGIBLE_WHERE, 0, rows, sizeof(rows)) > 0) {
    const char *cur = rows; char obj[128];
    while (next_object(&cur, obj, sizeof(obj))) {
      char p[64]; if (jstr(obj, "pub", p, sizeof(p))) send_wrapped_key(i, newE, p);
    }
  }
}

/* Add a member by their x-only pubkey (base64url): insert active with the default
 * role, then rotate the key to everyone eligible and republish the keyset (which
 * lets the new member auto-join). Caller must have verified ownership. */
static int add_member_pub2(int i, const char *memb,
                           const char *deliv, const char *prop) {
  int h = handle_for(i); if (h < 0) return 0;
  char drole[48]; if (!meta_get(h, "default_role", drole, sizeof(drole)) || !drole[0])
    s_cpy(drole, "member", sizeof(drole));
  long newE = meta_epoch(h) + 1;
  char mp[400] = "[\""; jesc(mp, sizeof(mp), memb);
  s_cat(mp, "\",", sizeof(mp)); char eb[24]; num(eb, newE); s_cat(mp, eb, sizeof(mp));
  s_cat(mp, ",\"", sizeof(mp)); jesc(mp, sizeof(mp), drole);
  s_cat(mp, "\",\"", sizeof(mp)); jesc(mp, sizeof(mp), deliv ? deliv : "");
  s_cat(mp, "\",\"", sizeof(mp)); jesc(mp, sizeof(mp), prop ? prop : ""); s_cat(mp, "\"]", sizeof(mp));
  db_exec(h, "INSERT OR REPLACE INTO members(pub,added_epoch,revoked_epoch,role,status,deliv,prop) "
             "VALUES(?,?,NULL,?,'active',?,?)", mp);
  rotate_and_distribute(i);   /* epoch became newE; wraps to all eligible incl. new */
  broadcast_keyset(i);        /* now addressed to the new member too (dests known) */
  return 1;
}
static int add_member_pub(int i, const char *memb) {
  return add_member_pub2(i, memb, 0, 0);
}

/* Our identity + RNS dests as "pub|deliv|prop" (for the cooperative-sync
 * bootstrap: hand these to a circle owner so they can add us addressed). */
int circle_add_member(const char *circleId, const char *npub) {
  int i = idx_by_id(circleId); if (i < 0) return 0;
  if (!owned(i)) { notify("error", "Only the owner can add members"); return 0; }
  unsigned char pk[32];
  if (npub_decode(npub, pk) != 0) { notify("error", "Invalid npub"); return 0; }
  char memb[64];
  if (b64url_encode(pk, 32, memb, sizeof(memb)) < 0) return 0;
  add_member_pub(i, memb);
  people_set_members(i); /* refresh the People panel if it's open */
  notify("info", "Member added");
  return 1;
}

/* Remove a circle from THIS device: drops the index row + in-memory cache entry
 * and tells the host to drop the conversation. For an owned circle this deletes
 * it locally (other members keep their copy); for a joined one it's "leave". The
 * per-circle sqlite file is closed and orphaned (circle_init only loads circles
 * still in the index). Returns 1 on success. */
int circle_delete(const char *circleId) {
  int i = idx_by_id(circleId);
  if (i >= 0) {
    char par[80] = "[\""; jesc(par, sizeof(par), c_id[i]); s_cat(par, "\"]", sizeof(par));
    db_exec(g_index, "DELETE FROM circles WHERE id=?", par);
    if (c_h[i] >= 0) { db_close(c_h[i]); c_h[i] = -1; }
    /* compact the cache arrays over the removed slot */
    for (int j = i; j < c_n - 1; j++) {
      s_cpy(c_id[j], c_id[j + 1], sizeof(c_id[j]));
      s_cpy(c_name[j], c_name[j + 1], sizeof(c_name[j]));
      c_h[j] = c_h[j + 1];
    }
    c_n--;
  }
  /* Always drop the host conversation row — even for a circle no longer in our
   * cache, so a stale list entry (e.g. a circle removed in a prior session whose
   * persisted convo row lingered) can still be cleaned from the UI. */
  char m[120] = "{\"type\":\"ui.convo.remove\",\"id\":\"";
  s_cat(m, circleId, sizeof(m)); s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
  notify("info", "Circle removed");
  return 1;
}

int circle_send(const char *circleId, const char *text) {
  int i = idx_by_id(circleId); if (i < 0) return 0;
  ensure_self();
  int h = handle_for(i); if (h < 0) return 0;
  long e = meta_epoch(h);
  unsigned char key[32];
  if (!epoch_key(i, e, key)) { notify("error", "No key for this circle yet"); return 0; }
  char body[MAX_TEXT + 1]; s_cpy(body, text, sizeof(body));
  if (!body[0]) return 0;
  /* encrypt */
  char ctb[1024];
  unsigned char ct[1024];
  uint32_t n = hal_crypto_aes_encrypt((const char *)key, 32, body, s_len(body),
                                      (char *)ct, sizeof(ct));
  if (n == 0) { notify("error", "Encrypt failed"); return 0; }
  if (b64url_encode(ct, n, ctb, sizeof(ctb)) < 0) return 0;
  long ts = (long)hal_time_epoch();
  char canon[1024]; canonical(canon, sizeof(canon), c_id[i], e, g_self, ts, ctb);
  char sig[160];
  if (hal_identity_sign(canon, s_len(canon), sig, sizeof(sig)) == 0) {
    notify("error", "Sign failed"); return 0;
  }
  char id[20]; event_id(sig, id);
  event_store(h, id, e, g_self, ts, ctb, sig, body);
  /* broadcast */
  char d[2048] = "{\"k\":\"msg\",\"c\":\"";
  for (int j = 0; j < TAGLEN; j++) { char ch[2] = { c_id[i][j], 0 }; s_cat(d, ch, sizeof(d)); }
  char b[24];
  s_cat(d, "\",\"e\":", sizeof(d)); num(b, e); s_cat(d, b, sizeof(d));
  s_cat(d, ",\"a\":\"", sizeof(d)); s_cat(d, g_self, sizeof(d));
  s_cat(d, "\",\"t\":", sizeof(d)); num(b, ts); s_cat(d, b, sizeof(d));
  s_cat(d, ",\"x\":\"", sizeof(d)); s_cat(d, ctb, sizeof(d));
  s_cat(d, "\",\"s\":\"", sizeof(d)); s_cat(d, sig, sizeof(d));
  s_cat(d, "\"}", sizeof(d));
  hal_rns_broadcast(d, s_len(d));         /* best-effort fast path */
  deliver_to_members(i, d);               /* reliable addressed + store-and-forward */
  render_msg(c_id[i], g_self, body, ts);
  convo_refresh_last(i);
  return 1;
}

void circle_render(const char *circleId) {
  int i = idx_by_id(circleId); if (i < 0) return;
  int h = handle_for(i); if (h < 0) return;
  char rows[16384];
  if (db_query(h, "SELECT author,ts,body FROM events WHERE body IS NOT NULL "
                  "ORDER BY ts LIMIT 200", 0, rows, sizeof(rows)) <= 0)
    return;
  const char *cur = rows; char obj[1024];
  while (next_object(&cur, obj, sizeof(obj))) {
    char author[64], body[600]; long ts = 0;
    jstr(obj, "author", author, sizeof(author));
    jstr(obj, "body", body, sizeof(body));
    jint(obj, "ts", &ts);
    render_msg(circleId, author, body, ts);
  }
}

void circle_on_datagram(const char *from, const char *json) {
  (void)from;
  char k[8]; if (!jstr(json, "k", k, sizeof(k))) return;
  if (s_eq(k, "msg")) handle_msg(json);
  else if (s_eq(k, "ks")) handle_keyset(json);
  else if (s_eq(k, "key")) handle_key(json);
  else if (s_eq(k, "kr")) handle_keyreq(json);
  else if (s_eq(k, "rq")) handle_req(json);
  else if (s_eq(k, "fm")) handle_fm(json);   /* folder message */
  else if (s_eq(k, "fk")) handle_fk(json);   /* folder key (wrapped) */
  else if (s_eq(k, "fkr")) handle_fkr(json); /* folder key request */
  else if (s_eq(k, "jr")) handle_jr(json);   /* join request (application) */
  else if (s_eq(k, "cd")) handle_cd(json);   /* short-code discovery request */
  else if (s_eq(k, "co")) handle_co(json);   /* circle offer (discovery reply) */
}

/* Pending short-code rendezvous resolve + owner rendezvous beacons. Defined with
 * the discovery handlers below. */
static void discovery_tick(void);
static void rv_announce_owned(void);

void circle_tick(void) {
  /* Periodic catch-up: for any circle holding messages we couldn't decrypt yet
   * (e.g. we just came back online), re-request the missing circle/folder keys. */
  for (int i = 0; i < c_n; i++) {
    int h = handle_for(i); if (h < 0) continue;
    char out[64];
    if (db_query(h, "SELECT id FROM events WHERE body IS NULL LIMIT 1", 0,
                 out, sizeof(out)) > 0 && out[0] == '[' && out[1] != ']') {
      send_keyreq(i);
    }
    /* Members of a circle we just joined may still have zero events if the
     * owner was offline when we first asked: retry history a bounded number of
     * times until something lands. */
    if (!owned(i)) {
      /* Pull the circle's full message history a bounded number of times after
       * joining. NOT gated on "no events yet" — once we post our own message the
       * events table is non-empty, but we may still be missing the owner's
       * earlier history. handle_msg dedups by event id, so re-pulling is safe. */
      long tries = meta_get_i(h, "histtries");
      if (tries < 8) { send_history_req(i, 0); meta_set_i(h, "histtries", tries + 1); }
    }
    folder_request_missing(i);
    /* Cooperative sync: pull store-and-forwarded events from member mailboxes
     * (we initiate, so this works even when our own inbound is unreachable). */
    pull_from_members(i);
  }
  discovery_tick();
  rv_announce_owned();   /* keep rendezvous beacons fresh for short-code joiners */
}

/* ── management panels ────────────────────────────────────────────────── */

void circle_open_edit(const char *circleId) {
  int i = idx_by_id(circleId); if (i < 0) return;
  if (!owned(i)) { notify("error", "Only the owner can edit this circle"); return; }
  int h = handle_for(i); if (h < 0) return;
  char v[1024];
  v[0] = 0; meta_get(h, "picture", v, sizeof(v)); field_set("edit_pic", v);
  field_set("edit_name", c_name[i]);
  v[0] = 0; meta_get(h, "description", v, sizeof(v)); field_set("edit_desc", v);
  screen_open("Edit circle");
}

void circle_save_edit(const char *circleId, const char *name, const char *desc) {
  int i = idx_by_id(circleId); if (i < 0) return;
  if (!owned(i)) { notify("error", "Only the owner can edit this circle"); return; }
  int h = handle_for(i); if (h < 0) return;
  if (name[0]) { meta_set(h, "name", name); s_cpy(c_name[i], name, sizeof(c_name[i])); }
  char d[260]; s_cpy(d, desc, sizeof(d)); if (s_len(d) > 250) d[250] = 0;
  meta_set(h, "description", d);
  convo_upsert(c_id[i], c_name[i], "", 0);
  broadcast_keyset(i);
  notify("info", "Saved");
}

/* ── roles ────────────────────────────────────────────────────────────── */
static void roles_populate(int i) {
  int h = handle_for(i); if (h < 0) return;
  char defr[48]; if (!meta_get(h, "default_role", defr, sizeof(defr))) defr[0] = 0;
  char m[8192] = "{\"type\":\"ui.people.set\",\"field\":\"roles\",\"sections\":"
                 "[{\"title\":\"Roles\",\"items\":[";
  s_cat(m, "{\"id\":\"__add\",\"title\":\"Add role\",\"subtitle\":"
           "\"Create a new role\",\"avatar\":\"+\",\"action\":\"role_add\","
           "\"actionLabel\":\"Add\",\"actionStyle\":\"filled\"}", sizeof(m));
  char rows[4096];
  if (db_query(h, "SELECT name,description FROM roles ORDER BY name", 0, rows, sizeof(rows)) > 0) {
    const char *cur = rows; char obj[512];
    while (next_object(&cur, obj, sizeof(obj))) {
      char nm[48], de[200]; if (!jstr(obj, "name", nm, sizeof(nm))) continue;
      de[0] = 0; jstr(obj, "description", de, sizeof(de));
      char sub[280]; sub[0] = 0; s_cat(sub, de, sizeof(sub));
      if (s_eq(nm, defr)) { if (sub[0]) s_cat(sub, "  ", sizeof(sub)); s_cat(sub, "• default", sizeof(sub)); }
      s_cat(m, ",{\"id\":\"", sizeof(m)); jesc(m, sizeof(m), nm);
      s_cat(m, "\",\"title\":\"", sizeof(m)); jesc(m, sizeof(m), nm);
      s_cat(m, "\",\"subtitle\":\"", sizeof(m)); jesc(m, sizeof(m), sub);
      s_cat(m, "\",\"buttons\":[{\"icon\":\"edit\",\"action\":\"role_edit\",\"tip\":\"Edit\"},"
               "{\"icon\":\"delete\",\"action\":\"role_del\",\"tip\":\"Remove\"}]}", sizeof(m));
    }
  }
  s_cat(m, "]}]}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

void circle_open_roles(const char *circleId) {
  int i = idx_by_id(circleId); if (i < 0) return;
  if (!owned(i)) { notify("error", "Only the owner can manage roles"); return; }
  roles_populate(i);
  screen_open("Roles");
}

void circle_role_open_edit(const char *circleId, const char *roleName) {
  int i = idx_by_id(circleId); if (i < 0) return;
  int h = handle_for(i); if (h < 0) return;
  char desc[256] = ""; int isdef = 0;
  if (roleName[0]) {
    char par[80] = "[\""; jesc(par, sizeof(par), roleName); s_cat(par, "\"]", sizeof(par));
    char row[512];
    if (db_query(h, "SELECT description FROM roles WHERE name=?", par, row, sizeof(row)) > 0)
      jstr(row, "description", desc, sizeof(desc));
    char defr[48]; if (meta_get(h, "default_role", defr, sizeof(defr)) && s_eq(defr, roleName)) isdef = 1;
  }
  field_set("role_name", roleName);
  field_set("role_desc", desc);
  field_set_raw("role_default", isdef ? "true" : "false");
  screen_open("Edit role");
}

void circle_role_save(const char *circleId, const char *oldName,
                      const char *newName, const char *desc, int makeDefault) {
  int i = idx_by_id(circleId); if (i < 0) return;
  if (!owned(i)) { notify("error", "Only the owner can manage roles"); return; }
  int h = handle_for(i); if (h < 0) return;
  if (!newName[0]) { notify("error", "Role needs a name"); return; }
  char par[400] = "[\""; jesc(par, sizeof(par), newName); s_cat(par, "\",\"", sizeof(par));
  jesc(par, sizeof(par), desc); s_cat(par, "\"]", sizeof(par));
  db_exec(h, "INSERT OR REPLACE INTO roles(name,description,created) VALUES(?,?,0)", par);
  if (oldName[0] && !s_eq(oldName, newName)) {
    char p2[200] = "[\""; jesc(p2, sizeof(p2), newName); s_cat(p2, "\",\"", sizeof(p2));
    jesc(p2, sizeof(p2), oldName); s_cat(p2, "\"]", sizeof(p2));
    db_exec(h, "UPDATE members SET role=? WHERE role=?", p2);
    char defr[48]; if (meta_get(h, "default_role", defr, sizeof(defr)) && s_eq(defr, oldName))
      meta_set(h, "default_role", newName);
    char pd[80] = "[\""; jesc(pd, sizeof(pd), oldName); s_cat(pd, "\"]", sizeof(pd));
    db_exec(h, "DELETE FROM roles WHERE name=?", pd);
  }
  if (makeDefault) meta_set(h, "default_role", newName);
  broadcast_keyset(i);
  roles_populate(i);
  screen_open("Roles");
  notify("info", "Role saved");
}

void circle_role_remove(const char *circleId, const char *roleName) {
  int i = idx_by_id(circleId); if (i < 0) return;
  if (!owned(i)) { notify("error", "Only the owner can manage roles"); return; }
  int h = handle_for(i); if (h < 0) return;
  char pd[80] = "[\""; jesc(pd, sizeof(pd), roleName); s_cat(pd, "\"]", sizeof(pd));
  db_exec(h, "DELETE FROM roles WHERE name=?", pd);
  char fb[48] = ""; char r1[256];
  if (db_query(h, "SELECT name FROM roles ORDER BY name LIMIT 1", 0, r1, sizeof(r1)) > 0)
    jstr(r1, "name", fb, sizeof(fb));
  if (fb[0]) {
    char p2[200] = "[\""; jesc(p2, sizeof(p2), fb); s_cat(p2, "\",\"", sizeof(p2));
    jesc(p2, sizeof(p2), roleName); s_cat(p2, "\"]", sizeof(p2));
    db_exec(h, "UPDATE members SET role=? WHERE role=?", p2);
  }
  char defr[48];
  if (meta_get(h, "default_role", defr, sizeof(defr)) && s_eq(defr, roleName) && fb[0])
    meta_set(h, "default_role", fb);
  broadcast_keyset(i);
  roles_populate(i);
  screen_open("Roles");
  notify("info", "Role removed");
}

void circle_set_picture(const char *circleId, const char *token) {
  int i = idx_by_id(circleId); if (i < 0) return;
  if (!owned(i)) return;
  int h = handle_for(i); if (h < 0) return;
  meta_set(h, "picture", token);
  broadcast_keyset(i);
}

/* Compact, human-friendly reference for a circle: "circle/<first3>-<last3>".
 * NOT a secret or a security token — it's only ~24 bits, so it's for quick
 * reference and local lookup, never for proving which circle is genuine. */
static void circle_short_code(const char *id, char *out, unsigned cap) {
  out[0] = 0;
  unsigned n = s_len(id);
  if (n < 6) { s_cat(out, "circle/", cap); s_cat(out, id, cap); return; }
  char a[4] = { id[0], id[1], id[2], 0 };
  char b[4] = { id[n - 3], id[n - 2], id[n - 1], 0 };
  s_cat(out, "circle/", cap); s_cat(out, a, cap);
  s_cat(out, "-", cap); s_cat(out, b, cap);
}

void circle_open_share(const char *circleId) {
  /* A deep link that carries the full key: scanning the QR or tapping the link
   * opens the app straight on the join screen. */
  char url[140] = "https://xprs.dev/circle/";
  s_cat(url, circleId, sizeof(url));
  field_set("share_qr", url);          /* QR encodes the link → scan opens the app */
  field_set("share_link", url);        /* copyable / tappable link */
  /* Full key (authoritative) for manual entry. */
  char fullkey[96] = "circle:";
  s_cat(fullkey, circleId, sizeof(fullkey));
  field_set("share_id", fullkey);
  /* Short reference (human shorthand). */
  char shortc[40]; circle_short_code(circleId, shortc, sizeof(shortc));
  field_set("share_short", shortc);
  screen_open("Share circle");
}

/* Resolve a circle reference to a full circleId among the circles this device
 * knows. Accepts the full form ("circle:<hex>" or bare 64-hex) and the short
 * form ("circle/abc-xyz", matched by first-3 + last-3). Returns 1 and writes the
 * full id only on a UNIQUE match; 0 if unknown or ambiguous (collision → caller
 * must use the full key). */
int circle_resolve_short(const char *code, char *outId, unsigned cap) {
  outId[0] = 0;
  /* strip a leading "circle:" or "circle/" label */
  const char *p = code;
  const char *sl = code;
  for (const char *q = code; *q; q++) if (*q == ':' || *q == '/') sl = q + 1;
  if (sl != code) p = sl;
  /* full key form: 64 hex chars → match an existing circle exactly */
  unsigned plen = s_len(p);
  if (plen == 64) {
    if (idx_by_id(p) >= 0) { s_cpy(outId, p, cap); return 1; }
    return 0;
  }
  /* short form: first3-last3 */
  char first[4] = "", last[4] = "";
  const char *dash = 0;
  for (const char *q = p; *q; q++) if (*q == '-') dash = q;
  if (!dash) return 0;
  unsigned fl = (unsigned)(dash - p);
  if (fl == 0 || fl > 3) return 0;
  for (unsigned k = 0; k < fl && k < 3; k++) first[k] = p[k];
  first[fl < 3 ? fl : 3] = 0;
  const char *lp = dash + 1; unsigned ll = s_len(lp);
  if (ll == 0 || ll > 3) return 0;
  s_cpy(last, lp, sizeof(last));
  /* unique match over known circles (first3 prefix + last3 suffix) */
  if (g_index < 0) return 0;
  static char rows[16384];
  if (db_query(g_index, "SELECT id FROM circles", 0, rows, sizeof(rows)) <= 0) return 0;
  const char *cur = rows; char obj[128]; int matches = 0;
  while (next_object(&cur, obj, sizeof(obj))) {
    char id[80]; if (!jstr(obj, "id", id, sizeof(id))) continue;
    unsigned n = s_len(id); if (n < 6) continue;
    int pre = (id[0] == first[0] && id[1] == first[1] && id[2] == first[2]);
    int suf = (id[n - 3] == last[0] && id[n - 2] == last[1] && id[n - 1] == last[2]);
    if (pre && suf) { matches++; s_cpy(outId, id, cap); }
  }
  if (matches == 1) return 1;   /* unique */
  outId[0] = 0;                 /* 0 = unknown, >1 = ambiguous (collision) */
  return 0;
}

/* Friendly display name for a member/applicant pubkey. */
static void member_disp(const char *pub, char *out, unsigned cap) {
  if (s_eq(pub, g_self)) { s_cpy(out, "You", cap); return; }
  char np[80];
  if (hal_npub(pub, s_len(pub), np, sizeof(np)) && np[0]) {
    s_cpy(out, np, cap); if (s_len(out) > 16) out[16] = 0;
  } else {
    s_cpy(out, pub, cap); if (s_len(out) > 10) out[10] = 0;
  }
}

/* Emit the members of one status into a section; returns the count written. The
 * caller wraps the section. [menu] is the JSON `"menu":[...]` string for rows. */
static int people_section(int h, char *m, unsigned cap, const char *status,
                          const char *menu) {
  char par[40] = "[\""; s_cat(par, status, sizeof(par)); s_cat(par, "\"]", sizeof(par));
  static char rows[8192];
  const char *q = (s_eq(status, "active"))
      ? "SELECT pub,role FROM members WHERE status IS NULL OR status='' OR status='active' ORDER BY pub"
      : "SELECT pub,role FROM members WHERE status=? ORDER BY pub";
  int n = db_query(h, q, s_eq(status, "active") ? 0 : par, rows, sizeof(rows));
  if (n <= 0) return 0;
  int count = 0; const char *cur = rows; char obj[256];
  while (next_object(&cur, obj, sizeof(obj))) {
    char pub[64], role[48]; if (!jstr(obj, "pub", pub, sizeof(pub))) continue;
    role[0] = 0; jstr(obj, "role", role, sizeof(role));
    char disp[80]; member_disp(pub, disp, sizeof(disp));
    s_cat(m, ",{\"id\":\"", cap); jesc(m, cap, pub);
    s_cat(m, "\",\"title\":\"", cap); jesc(m, cap, disp);
    s_cat(m, "\",\"subtitle\":\"", cap); jesc(m, cap, role[0] ? role : "member");
    s_cat(m, "\",", cap); s_cat(m, menu, cap);
    s_cat(m, "}", cap);
    count++;
  }
  return count;
}

/* The Members review panel: Candidates / Active / Inactive / Suspended / Banned
 * tabs (the people widget shows one section at a time with counts), plus a
 * History tab of recent member actions. */
static void people_set_members(int i) {
  int h = handle_for(i); if (h < 0) return;
  static char m[16384];
  s_cpy(m, "{\"type\":\"ui.people.set\",\"field\":\"members\",\"sections\":[", sizeof(m));
  int sfirst = 1;

  /* Candidates (pending applications) — Approve / Reject. */
  s_cat(m, "{\"title\":\"Candidates\",\"items\":[", sizeof(m)); sfirst = 0;
  char qrows[8192];
  if (db_query(h, "SELECT pub,nick FROM requests ORDER BY ts", 0, qrows, sizeof(qrows)) > 0) {
    const char *cur = qrows; char obj[256]; int first = 1;
    while (next_object(&cur, obj, sizeof(obj))) {
      char pub[64], nk[80]; if (!jstr(obj, "pub", pub, sizeof(pub))) continue;
      nk[0] = 0; jstr(obj, "nick", nk, sizeof(nk));
      char disp[80]; if (nk[0]) { s_cpy(disp, nk, sizeof(disp)); if (s_len(disp) > 18) disp[18] = 0; }
      else member_disp(pub, disp, sizeof(disp));
      if (!first) s_cat(m, ",", sizeof(m)); first = 0;
      s_cat(m, "{\"id\":\"", sizeof(m)); jesc(m, sizeof(m), pub);
      s_cat(m, "\",\"title\":\"", sizeof(m)); jesc(m, sizeof(m), disp);
      s_cat(m, "\",\"subtitle\":\"wants to join\",\"buttons\":["
               "{\"icon\":\"add\",\"action\":\"req_approve\",\"tip\":\"Approve\"},"
               "{\"icon\":\"delete\",\"action\":\"req_reject\",\"tip\":\"Reject\"}]}", sizeof(m));
    }
  }
  s_cat(m, "]}", sizeof(m));

  /* Active — Add row + members, each with a management menu. */
  s_cat(m, ",{\"title\":\"Active\",\"items\":[", sizeof(m));
  s_cat(m, "{\"id\":\"__add\",\"title\":\"Add member\",\"subtitle\":"
           "\"Invite by contact, callsign or npub\",\"avatar\":\"+\","
           "\"action\":\"add_member\",\"actionLabel\":\"Add\"}", sizeof(m));
  people_section(h, m, sizeof(m), "active",
      "\"menu\":[{\"label\":\"Change role\",\"value\":\"mrole\"},"
      "{\"label\":\"Make inactive\",\"value\":\"mstat:inactive\"},"
      "{\"label\":\"Suspend\",\"value\":\"mstat:suspended\"},"
      "{\"label\":\"Ban\",\"value\":\"mstat:banned\"},"
      "{\"label\":\"Remove\",\"value\":\"mremove\"}]");
  s_cat(m, "]}", sizeof(m));

  /* Inactive / Suspended / Banned — shown only when they have members. */
  static char tmp[12288];
  tmp[0] = 0;
  if (people_section(h, tmp, sizeof(tmp), "inactive",
      "\"menu\":[{\"label\":\"Reactivate\",\"value\":\"mstat:active\"},"
      "{\"label\":\"Suspend\",\"value\":\"mstat:suspended\"},"
      "{\"label\":\"Ban\",\"value\":\"mstat:banned\"},"
      "{\"label\":\"Remove\",\"value\":\"mremove\"}]") > 0) {
    s_cat(m, ",{\"title\":\"Inactive\",\"items\":[", sizeof(m));
    s_cat(m, tmp[0] == ',' ? tmp + 1 : tmp, sizeof(m)); s_cat(m, "]}", sizeof(m));
  }
  tmp[0] = 0;
  if (people_section(h, tmp, sizeof(tmp), "suspended",
      "\"menu\":[{\"label\":\"Reactivate\",\"value\":\"mstat:active\"},"
      "{\"label\":\"Ban\",\"value\":\"mstat:banned\"},"
      "{\"label\":\"Remove\",\"value\":\"mremove\"}]") > 0) {
    s_cat(m, ",{\"title\":\"Suspended\",\"items\":[", sizeof(m));
    s_cat(m, tmp[0] == ',' ? tmp + 1 : tmp, sizeof(m)); s_cat(m, "]}", sizeof(m));
  }
  tmp[0] = 0;
  if (people_section(h, tmp, sizeof(tmp), "banned",
      "\"menu\":[{\"label\":\"Unban\",\"value\":\"mstat:active\"},"
      "{\"label\":\"Remove\",\"value\":\"mremove\"}]") > 0) {
    s_cat(m, ",{\"title\":\"Banned\",\"items\":[", sizeof(m));
    s_cat(m, tmp[0] == ',' ? tmp + 1 : tmp, sizeof(m)); s_cat(m, "]}", sizeof(m));
  }

  /* History — recent member-management actions (info rows, no controls). */
  s_cat(m, ",{\"title\":\"History\",\"items\":[", sizeof(m));
  char arows[8192]; int afirst = 1;
  if (db_query(h, "SELECT ts,action,who FROM audit ORDER BY id DESC LIMIT 50", 0,
               arows, sizeof(arows)) > 0) {
    const char *cur = arows; char obj[256];
    while (next_object(&cur, obj, sizeof(obj))) {
      char act[48], who[64]; long ts = 0;
      jint(obj, "ts", &ts); act[0] = 0; jstr(obj, "action", act, sizeof(act));
      who[0] = 0; jstr(obj, "who", who, sizeof(who));
      char disp[80]; disp[0] = 0; if (who[0]) member_disp(who, disp, sizeof(disp));
      char t[16]; fmt_time(t, ts);
      if (!afirst) s_cat(m, ",", sizeof(m)); afirst = 0;
      s_cat(m, "{\"id\":\"a", sizeof(m)); char nb[24]; num(nb, ts); s_cat(m, nb, sizeof(m));
      s_cat(m, "\",\"avatar\":\"•\",\"title\":\"", sizeof(m)); jesc(m, sizeof(m), act);
      if (disp[0]) { s_cat(m, ": ", sizeof(m)); jesc(m, sizeof(m), disp); }
      s_cat(m, "\",\"subtitle\":\"", sizeof(m)); s_cat(m, t, sizeof(m)); s_cat(m, "\"}", sizeof(m));
    }
  }
  s_cat(m, "]}", sizeof(m));

  s_cat(m, "]}", sizeof(m));
  (void)sfirst;
  hal_msg_send(m, s_len(m));
}

void circle_open_people(const char *circleId) {
  int i = idx_by_id(circleId); if (i < 0) return;
  people_set_members(i);
  screen_open("People");
}

void circle_roles_csv(const char *circleId, char *out, unsigned cap) {
  out[0] = 0;
  int i = idx_by_id(circleId); if (i < 0) return;
  int h = handle_for(i); if (h < 0) return;
  char rows[2048];
  if (db_query(h, "SELECT name FROM roles ORDER BY name", 0, rows, sizeof(rows)) > 0) {
    const char *cur = rows; char obj[128]; int first = 1;
    while (next_object(&cur, obj, sizeof(obj))) {
      char nm[48]; if (!jstr(obj, "name", nm, sizeof(nm))) continue;
      if (!first) s_cat(out, ",", cap);
      first = 0; s_cat(out, nm, cap);
    }
  }
}

void circle_member_set_role(const char *circleId, const char *memberPub,
                            const char *role) {
  int i = idx_by_id(circleId); if (i < 0) return;
  if (!owned(i)) { notify("error", "Only the owner can change roles"); return; }
  int h = handle_for(i); if (h < 0) return;
  char par[200] = "[\""; jesc(par, sizeof(par), role); s_cat(par, "\",\"", sizeof(par));
  jesc(par, sizeof(par), memberPub); s_cat(par, "\"]", sizeof(par));
  db_exec(h, "UPDATE members SET role=? WHERE pub=?", par);
  broadcast_keyset(i);
  people_set_members(i);
  notify("info", "Role updated");
}

void circle_member_remove(const char *circleId, const char *memberPub) {
  int i = idx_by_id(circleId); if (i < 0) return;
  if (!owned(i)) { notify("error", "Only the owner can remove people"); return; }
  if (s_eq(memberPub, g_self)) { notify("error", "You can't remove yourself"); return; }
  int h = handle_for(i); if (h < 0) return;
  char par[80] = "[\""; jesc(par, sizeof(par), memberPub); s_cat(par, "\"]", sizeof(par));
  db_exec(h, "DELETE FROM members WHERE pub=?", par);
  rotate_and_distribute(i);   /* removed member can't read future messages */
  audit_add(i, "removed", memberPub);
  broadcast_keyset(i);
  people_set_members(i);
  notify("info", "Removed");
}

/* Change a member's status (active | inactive | suspended | banned). Suspending
 * or banning rotates the key so they lose forward access; reactivating hands
 * them the current key again. */
void circle_member_set_status(const char *circleId, const char *memberPub,
                             const char *status) {
  int i = idx_by_id(circleId); if (i < 0) return;
  if (!owned(i)) { notify("error", "Only the owner can do this"); return; }
  if (s_eq(memberPub, g_self)) { notify("error", "You can't change your own status"); return; }
  int h = handle_for(i); if (h < 0) return;
  char par[160] = "[\""; jesc(par, sizeof(par), status); s_cat(par, "\",\"", sizeof(par));
  jesc(par, sizeof(par), memberPub); s_cat(par, "\"]", sizeof(par));
  db_exec(h, "UPDATE members SET status=? WHERE pub=?", par);
  int cut = (s_eq(status, "suspended") || s_eq(status, "banned"));
  if (cut) {
    rotate_and_distribute(i);                 /* exclude them from the new key */
  } else {
    /* re-grant access: give them the current epoch key */
    long e = meta_epoch(h);
    send_wrapped_key(i, e, memberPub);
  }
  audit_add(i, status, memberPub);
  broadcast_keyset(i);
  people_set_members(i);
  char msg[64] = "Member "; s_cat(msg, status, sizeof(msg));
  notify("info", msg);
}

/* ── Virtual folders ─────────────────────────────────────────────────────
 * A permissioned, nestable tree inside a circle. Each folder is its own
 * access-controlled space (chat now; container/blog/gallery/forum later).
 * Entry is crypto-enforced: each folder has its own key, handed only to members
 * whose role (or individual allow) permits entry, and rotated when permissions
 * change. Folder STRUCTURE + permissions ride in the owner-signed keyset; the
 * per-folder KEYS travel as wrapped "fk" datagrams and are requested with "fkr".
 */

static char g_browse[40];     /* current folder being browsed ("" = root) */
static char g_chatfolder[40]; /* folder whose chat screen is open */

/* forward declarations (callees referenced before their definitions) */
static void chat_append(const char *field, const char *dir, const char *author,
                        const char *text, long ts);
static void folder_decrypt_pending(int i, const char *fid, long epoch);
static void folder_canonical(char *out, unsigned cap, const char *tag,
                             const char *fid, long e, const char *author,
                             long ts, const char *ct);
static void folder_event_store(int h, const char *id, const char *fid, long e,
                               const char *author, long ts, const char *ct,
                               const char *sig, const char *body);
static void rail_set(int i);
static void faccess_populate(int i, const char *folderId);

/* ── csv helpers (role / pubkey lists stored comma-separated) ──────────── */
static int csv_has(const char *csv, const char *item) {
  unsigned il = s_len(item);
  const char *p = csv;
  while (*p) {
    const char *st = p; while (*p && *p != ',') p++;
    unsigned len = (unsigned)(p - st);
    if (len == il) {
      int eq = 1; for (unsigned k = 0; k < il; k++) if (st[k] != item[k]) { eq = 0; break; }
      if (eq) return 1;
    }
    if (*p == ',') p++;
  }
  return 0;
}
static void csv_add(char *buf, unsigned cap, const char *item) {
  if (!item[0] || csv_has(buf, item)) return;
  if (buf[0]) s_cat(buf, ",", cap);
  s_cat(buf, item, cap);
}
static void csv_remove(char *buf, unsigned cap, const char *item) {
  static char tmp[4096]; tmp[0] = 0; unsigned il = s_len(item);
  const char *p = buf;
  while (*p) {
    const char *st = p; while (*p && *p != ',') p++;
    unsigned len = (unsigned)(p - st);
    int match = (len == il);
    if (match) for (unsigned k = 0; k < il; k++) if (st[k] != item[k]) { match = 0; break; }
    if (!match && len) {
      if (tmp[0]) s_cat(tmp, ",", sizeof(tmp));
      char one[300]; unsigned c = len < sizeof(one) - 1 ? len : sizeof(one) - 1;
      for (unsigned k = 0; k < c; k++) one[k] = st[k]; one[c] = 0;
      s_cat(tmp, one, sizeof(tmp));
    }
    if (*p == ',') p++;
  }
  s_cpy(buf, tmp, cap);
}

/* ── folder model lookups ─────────────────────────────────────────────── */
static int folder_row(int i, const char *id, char *out, unsigned cap) {
  int h = handle_for(i); if (h < 0) return 0;
  char par[80] = "[\""; jesc(par, sizeof(par), id); s_cat(par, "\"]", sizeof(par));
  if (db_query(h, "SELECT parent,name,type,description,icon,roles,allow,deny,epoch "
                  "FROM folders WHERE id=?", par, out, cap) <= 0) return 0;
  return out[0] == '[' && out[1] != ']';
}
static void member_role(int i, const char *m, char *out, unsigned cap) {
  out[0] = 0; int h = handle_for(i); if (h < 0) return;
  char par[80] = "[\""; jesc(par, sizeof(par), m); s_cat(par, "\"]", sizeof(par));
  char row[160];
  if (db_query(h, "SELECT role FROM members WHERE pub=?", par, row, sizeof(row)) > 0)
    jstr(row, "role", out, cap);
}
static int folder_can_enter(int i, const char *id, const char *m) {
  if (owned(i) && s_eq(m, g_self)) return 1;     /* the owner sees everything */
  /* suspended/banned members can't enter any folder */
  char st[24]; if (member_status(i, m, st, sizeof(st)) &&
      (s_eq(st, "suspended") || s_eq(st, "banned"))) return 0;
  static char row[6000]; if (!folder_row(i, id, row, sizeof(row))) return 0;
  char roles[256], allow[2048], deny[2048];
  roles[0] = allow[0] = deny[0] = 0;
  jstr(row, "roles", roles, sizeof(roles));
  jstr(row, "allow", allow, sizeof(allow));
  jstr(row, "deny", deny, sizeof(deny));
  if (csv_has(deny, m)) return 0;
  if (csv_has(allow, m)) return 1;
  char rl[48]; member_role(i, m, rl, sizeof(rl));
  if (rl[0] && csv_has(roles, rl)) return 1;
  if (!roles[0] && !allow[0]) return 1;          /* default: open to all members */
  return 0;
}

/* ── folder keys ──────────────────────────────────────────────────────── */
static int folder_key_get(int i, const char *id, long epoch, unsigned char key[32]) {
  int h = handle_for(i); if (h < 0) return 0;
  char eb[24]; num(eb, epoch);
  char par[80] = "[\""; jesc(par, sizeof(par), id);
  s_cat(par, "\",", sizeof(par)); s_cat(par, eb, sizeof(par)); s_cat(par, "]", sizeof(par));
  char out[128];
  if (db_query(h, "SELECT key FROM folder_keys WHERE folder=? AND epoch=?", par, out, sizeof(out)) <= 0)
    return 0;
  char kb[64]; if (!jstr(out, "key", kb, sizeof(kb)) || !kb[0]) return 0;
  return b64url_decode(kb, key, 32) == 32;
}
static void folder_key_store(int i, const char *id, long epoch, const unsigned char key[32]) {
  int h = handle_for(i); if (h < 0) return;
  char kb[64]; if (b64url_encode(key, 32, kb, sizeof(kb)) < 0) return;
  char eb[24]; num(eb, epoch);
  char par[160] = "[\""; jesc(par, sizeof(par), id);
  s_cat(par, "\",", sizeof(par)); s_cat(par, eb, sizeof(par));
  s_cat(par, ",\"", sizeof(par)); s_cat(par, kb, sizeof(par)); s_cat(par, "\"]", sizeof(par));
  db_exec(h, "INSERT OR REPLACE INTO folder_keys(folder,epoch,key) VALUES(?,?,?)", par);
}
static void send_wrapped_folder_key(int i, const char *id, long epoch, const char *to) {
  if (s_eq(to, g_self)) return;
  unsigned char key[32]; if (!folder_key_get(i, id, epoch, key)) return;
  char blob[200];
  if (hal_encrypt(to, s_len(to), (const char *)key, 32, blob, sizeof(blob)) == 0) return;
  char eb[24]; num(eb, epoch);
  char d[600] = "{\"k\":\"fk\",\"c\":\"";
  for (int j = 0; j < TAGLEN; j++) { char ch[2] = { c_id[i][j], 0 }; s_cat(d, ch, sizeof(d)); }
  s_cat(d, "\",\"f\":\"", sizeof(d)); jesc(d, sizeof(d), id);
  s_cat(d, "\",\"e\":", sizeof(d)); s_cat(d, eb, sizeof(d));
  s_cat(d, ",\"to\":\"", sizeof(d)); s_cat(d, to, sizeof(d));
  s_cat(d, "\",\"frm\":\"", sizeof(d)); s_cat(d, g_self, sizeof(d));
  s_cat(d, "\",\"b\":\"", sizeof(d)); s_cat(d, blob, sizeof(d));
  s_cat(d, "\"}", sizeof(d));
  hal_rns_broadcast(d, s_len(d));
}
static void folder_distribute_key(int i, const char *id, long epoch) {
  int h = handle_for(i); if (h < 0) return;
  static char rows[8192];
  if (db_query(h, "SELECT pub FROM members", 0, rows, sizeof(rows)) > 0) {
    const char *cur = rows; char obj[128];
    while (next_object(&cur, obj, sizeof(obj))) {
      char p[64]; if (!jstr(obj, "pub", p, sizeof(p))) continue;
      if (folder_can_enter(i, id, p)) send_wrapped_folder_key(i, id, epoch, p);
    }
  }
}
/* Rotate to a fresh key (on a permission change) and hand it to the permitted set. */
static long folder_rotate(int i, const char *id) {
  int h = handle_for(i); if (h < 0) return 0;
  static char row[6000]; long cur = 0;
  if (folder_row(i, id, row, sizeof(row))) jint(row, "epoch", &cur);
  long ne = cur + 1;
  unsigned char key[32]; hal_crypto_random((char *)key, 32);
  folder_key_store(i, id, ne, key);
  char eb[24]; num(eb, ne);
  char par[80] = "["; s_cat(par, eb, sizeof(par));
  s_cat(par, ",\"", sizeof(par)); jesc(par, sizeof(par), id); s_cat(par, "\"]", sizeof(par));
  db_exec(h, "UPDATE folders SET epoch=? WHERE id=?", par);
  folder_distribute_key(i, id, ne);
  return ne;
}
static void folder_send_keyreq(int i, const char *fid) {
  char d[256] = "{\"k\":\"fkr\",\"c\":\"";
  for (int j = 0; j < TAGLEN; j++) { char ch[2] = { c_id[i][j], 0 }; s_cat(d, ch, sizeof(d)); }
  s_cat(d, "\",\"f\":\"", sizeof(d)); jesc(d, sizeof(d), fid);
  s_cat(d, "\",\"frm\":\"", sizeof(d)); s_cat(d, g_self, sizeof(d)); s_cat(d, "\"}", sizeof(d));
  hal_rns_broadcast(d, s_len(d));
}
/* Request keys for every folder we may enter but don't yet hold the current key for. */
static void folder_request_missing(int i) {
  int h = handle_for(i); if (h < 0) return;
  static char rows[8192];
  if (db_query(h, "SELECT id,epoch FROM folders", 0, rows, sizeof(rows)) <= 0) return;
  const char *cur = rows; char obj[256];
  while (next_object(&cur, obj, sizeof(obj))) {
    char id[40]; long ep = 0; if (!jstr(obj, "id", id, sizeof(id))) continue; jint(obj, "epoch", &ep);
    if (!folder_can_enter(i, id, g_self)) continue;
    unsigned char k[32]; if (folder_key_get(i, id, ep, k)) continue;
    folder_send_keyreq(i, id);
  }
}

/* ── folder chat events ───────────────────────────────────────────────── */
static void folder_canonical(char *out, unsigned cap, const char *tag,
                             const char *fid, long e, const char *author,
                             long ts, const char *ct) {
  out[0] = 0;
  for (int j = 0; j < TAGLEN; j++) { char ch[2] = { tag[j], 0 }; s_cat(out, ch, cap); }
  char b[24];
  s_cat(out, "|", cap); s_cat(out, fid, cap);
  s_cat(out, "|", cap); num(b, e); s_cat(out, b, cap);
  s_cat(out, "|", cap); s_cat(out, author, cap);
  s_cat(out, "|", cap); num(b, ts); s_cat(out, b, cap);
  s_cat(out, "|", cap); s_cat(out, ct, cap);
}
static void folder_event_store(int h, const char *id, const char *fid, long e,
                               const char *author, long ts, const char *ct,
                               const char *sig, const char *body) {
  static char par[2048]; s_cpy(par, "[\"", sizeof(par)); jesc(par, sizeof(par), id);
  s_cat(par, "\",\"", sizeof(par)); jesc(par, sizeof(par), fid);
  s_cat(par, "\",", sizeof(par)); char b[24]; num(b, e); s_cat(par, b, sizeof(par));
  s_cat(par, ",\"", sizeof(par)); jesc(par, sizeof(par), author);
  s_cat(par, "\",", sizeof(par)); num(b, ts); s_cat(par, b, sizeof(par));
  s_cat(par, ",\"", sizeof(par)); jesc(par, sizeof(par), ct);
  s_cat(par, "\",\"", sizeof(par)); jesc(par, sizeof(par), sig);
  s_cat(par, "\",", sizeof(par));
  if (body) { s_cat(par, "\"", sizeof(par)); jesc(par, sizeof(par), body); s_cat(par, "\"", sizeof(par)); }
  else s_cat(par, "null", sizeof(par));
  s_cat(par, "]", sizeof(par));
  db_exec(h, "INSERT OR REPLACE INTO folder_events(id,folder,epoch,author,ts,ct,sig,body) "
             "VALUES(?,?,?,?,?,?,?,?)", par);
}
static void folder_decrypt_pending(int i, const char *fid, long epoch) {
  int h = handle_for(i); if (h < 0) return;
  unsigned char key[32]; if (!folder_key_get(i, fid, epoch, key)) return;
  char eb[24]; num(eb, epoch);
  char par[80] = "[\""; jesc(par, sizeof(par), fid);
  s_cat(par, "\",", sizeof(par)); s_cat(par, eb, sizeof(par)); s_cat(par, "]", sizeof(par));
  static char rows[16384];
  if (db_query(h, "SELECT id,author,ts,ct FROM folder_events "
                  "WHERE folder=? AND epoch=? AND body IS NULL ORDER BY ts", par, rows, sizeof(rows)) <= 0)
    return;
  const char *cur = rows; char obj[1024];
  while (next_object(&cur, obj, sizeof(obj))) {
    char id[40], author[64], ct[800]; long ts = 0;
    if (!jstr(obj, "id", id, sizeof(id))) continue;
    jstr(obj, "author", author, sizeof(author)); jstr(obj, "ct", ct, sizeof(ct)); jint(obj, "ts", &ts);
    unsigned char blob[1024]; int bn = b64url_decode(ct, blob, sizeof(blob)); if (bn <= 16) continue;
    char body[600];
    uint32_t pn = hal_crypto_aes_decrypt((const char *)key, 32, (const char *)blob, (uint32_t)bn, body, sizeof(body) - 1);
    if (pn == 0) continue; body[pn] = 0;
    char up[640] = "[\""; jesc(up, sizeof(up), body); s_cat(up, "\",\"", sizeof(up));
    jesc(up, sizeof(up), id); s_cat(up, "\"]", sizeof(up));
    db_exec(h, "UPDATE folder_events SET body=? WHERE id=?", up);
    if (s_eq(g_chatfolder, fid)) chat_append("folderchat", s_eq(author, g_self) ? "out" : "in", author, body, ts);
  }
}

/* ── folder datagram handlers ─────────────────────────────────────────── */
static void handle_fm(const char *json) {
  char tag[24], fid[40], author[64], ct[800], sig[160]; long e = 0, ts = 0;
  if (!jstr(json, "c", tag, sizeof(tag))) return;
  int i = idx_by_tag(tag); if (i < 0) return;
  jstr(json, "f", fid, sizeof(fid)); jint(json, "e", &e); jint(json, "t", &ts);
  jstr(json, "a", author, sizeof(author)); jstr(json, "x", ct, sizeof(ct)); jstr(json, "s", sig, sizeof(sig));
  if (!fid[0] || !author[0] || !sig[0]) return;
  if (s_eq(author, g_self)) return;
  if (!is_member(i, author, 0)) return;
  static char canon[1500]; folder_canonical(canon, sizeof(canon), tag, fid, e, author, ts, ct);
  if (!hal_verify(author, s_len(author), canon, s_len(canon), sig, s_len(sig))) return;
  int h = handle_for(i); if (h < 0) return;
  char id[20]; event_id(sig, id);
  char par[40] = "[\""; s_cat(par, id, sizeof(par)); s_cat(par, "\"]", sizeof(par));
  char chk[64];
  if (db_query(h, "SELECT id FROM folder_events WHERE id=?", par, chk, sizeof(chk)) > 0 &&
      chk[0] == '[' && chk[1] != ']') return;
  int dec = 0; char body[600]; unsigned char key[32];
  if (folder_key_get(i, fid, e, key)) {
    unsigned char blob[1024]; int bn = b64url_decode(ct, blob, sizeof(blob));
    if (bn > 16) {
      uint32_t pn = hal_crypto_aes_decrypt((const char *)key, 32, (const char *)blob, (uint32_t)bn, body, sizeof(body) - 1);
      if (pn) { body[pn] = 0; dec = 1; }
    }
  }
  folder_event_store(h, id, fid, e, author, ts, ct, sig, dec ? body : 0);
  if (dec) {
    if (s_eq(g_chatfolder, fid)) chat_append("folderchat", "in", author, body, ts);
  } else if (folder_can_enter(i, fid, g_self)) {
    folder_send_keyreq(i, fid);
  }
}
static void handle_fk(const char *json) {
  char tag[24], fid[40], to[64], frm[64], blob[300]; long e = 0;
  if (!jstr(json, "c", tag, sizeof(tag))) return;
  jstr(json, "to", to, sizeof(to)); if (!s_eq(to, g_self)) return;
  jstr(json, "f", fid, sizeof(fid)); jint(json, "e", &e);
  jstr(json, "frm", frm, sizeof(frm)); jstr(json, "b", blob, sizeof(blob));
  int i = idx_by_tag(tag); if (i < 0 || !fid[0] || !frm[0] || !blob[0]) return;
  char key[64];
  uint32_t n = hal_decrypt(frm, s_len(frm), blob, s_len(blob), key, sizeof(key));
  if (n != 32) return;
  folder_key_store(i, fid, e, (const unsigned char *)key);
  folder_decrypt_pending(i, fid, e);
}
static void handle_fkr(const char *json) {
  char tag[24], fid[40], frm[64];
  if (!jstr(json, "c", tag, sizeof(tag))) return;
  jstr(json, "f", fid, sizeof(fid)); jstr(json, "frm", frm, sizeof(frm));
  int i = idx_by_tag(tag); if (i < 0 || !fid[0] || !frm[0]) return;
  if (s_eq(frm, g_self)) return;
  if (!folder_can_enter(i, fid, frm)) return;
  int h = handle_for(i); if (h < 0) return;
  char par[80] = "[\""; jesc(par, sizeof(par), fid); s_cat(par, "\"]", sizeof(par));
  static char rows[4096];
  if (db_query(h, "SELECT epoch FROM folder_keys WHERE folder=? ORDER BY epoch", par, rows, sizeof(rows)) > 0) {
    const char *cur = rows; char obj[64];
    while (next_object(&cur, obj, sizeof(obj))) {
      long e = 0; if (jint(obj, "epoch", &e)) send_wrapped_folder_key(i, fid, e, frm);
    }
  }
}

/* ── chat UI helpers (folder chat uses the generic $type:"chat" field) ─── */
static void chat_clear(const char *field) {
  char m[128] = "{\"type\":\"ui.chat.clear\",\"field\":\"";
  s_cat(m, field, sizeof(m)); s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
static void chat_append(const char *field, const char *dir, const char *author,
                        const char *text, long ts) {
  char from[40]; author_label(author, from, sizeof(from));
  char t[16]; fmt_time(t, ts);
  static char m[1400]; s_cpy(m, "{\"type\":\"ui.chat.append\",\"field\":\"", sizeof(m));
  s_cat(m, field, sizeof(m));
  s_cat(m, "\",\"message\":{\"dir\":\"", sizeof(m)); s_cat(m, dir, sizeof(m));
  s_cat(m, "\",\"from\":\"", sizeof(m)); jesc(m, sizeof(m), from);
  s_cat(m, "\",\"text\":\"", sizeof(m)); jesc(m, sizeof(m), text);
  s_cat(m, "\",\"time\":\"", sizeof(m)); s_cat(m, t, sizeof(m)); s_cat(m, "\"}}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* ── folder UI population ─────────────────────────────────────────────── */
/* Build a navigation rail of the enterable child folders of [folder] into the
 * named GeoUI rail [field]. Locked folders are omitted. Built-in controls use
 * id tokens (__up/__add/__edit/__access) the host renders as Material icons;
 * each folder carries its stored icon value (emoji / svg:<xml> / empty). When
 * [isPanel] the rail also offers Up (if not at root) and owner Edit/Access. */
static void rail_build(int i, const char *field, const char *folder) {
  int h = handle_for(i); if (h < 0) return;
  static char m[16384];
  s_cpy(m, "{\"type\":\"ui.rail.set\",\"field\":\"", sizeof(m));
  s_cat(m, field, sizeof(m));
  s_cat(m, "\",\"items\":[", sizeof(m));
  int wrote = 0;
  /* Navigation only: the enterable sub-folders. (No "Up" — the panel's back
   * arrow handles that; management lives in the top-right gear menu.) The host
   * hides the whole column when this list is empty. */
  char par[80] = "[\""; jesc(par, sizeof(par), folder); s_cat(par, "\"]", sizeof(par));
  static char rows[12288];
  if (db_query(h, "SELECT id,name,icon FROM folders WHERE parent=? ORDER BY created", par, rows, sizeof(rows)) > 0) {
    const char *cur = rows; char obj[700];
    while (next_object(&cur, obj, sizeof(obj))) {
      char id[40], nm[64], ic[256];
      if (!jstr(obj, "id", id, sizeof(id))) continue;
      if (!folder_can_enter(i, id, g_self)) continue;  /* only show folders we may enter */
      nm[0] = 0; jstr(obj, "name", nm, sizeof(nm)); ic[0] = 0; jstr(obj, "icon", ic, sizeof(ic));
      if (wrote) s_cat(m, ",", sizeof(m));
      wrote = 1;
      s_cat(m, "{\"id\":\"", sizeof(m)); jesc(m, sizeof(m), id);
      s_cat(m, "\",\"name\":\"", sizeof(m)); jesc(m, sizeof(m), nm[0] ? nm : id);
      s_cat(m, "\",\"icon\":\"", sizeof(m)); jesc(m, sizeof(m), ic); s_cat(m, "\"}", sizeof(m));
    }
  }
  s_cat(m, "]}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
static void rail_set(int i) { rail_build(i, "folderrail", g_browse); }

static void faccess_populate(int i, const char *folderId) {
  int h = handle_for(i); if (h < 0) return;
  static char row[6000]; char roles[256] = "", allow[2048] = "", deny[2048] = "";
  if (folder_row(i, folderId, row, sizeof(row))) {
    jstr(row, "roles", roles, sizeof(roles));
    jstr(row, "allow", allow, sizeof(allow));
    jstr(row, "deny", deny, sizeof(deny));
  }
  static char m[16384];
  s_cpy(m, "{\"type\":\"ui.people.set\",\"field\":\"faccess\",\"sections\":[", sizeof(m));
  s_cat(m, "{\"title\":\"Roles\",\"items\":[", sizeof(m));
  char rrows[4096]; int rfirst = 1;
  if (db_query(h, "SELECT name FROM roles ORDER BY name", 0, rrows, sizeof(rrows)) > 0) {
    const char *cur = rrows; char obj[128];
    while (next_object(&cur, obj, sizeof(obj))) {
      char nm[48]; if (!jstr(obj, "name", nm, sizeof(nm))) continue;
      int on = csv_has(roles, nm);
      if (!rfirst) s_cat(m, ",", sizeof(m)); rfirst = 0;
      s_cat(m, "{\"id\":\"", sizeof(m)); jesc(m, sizeof(m), nm);
      s_cat(m, "\",\"title\":\"", sizeof(m)); jesc(m, sizeof(m), nm);
      s_cat(m, "\",\"subtitle\":\"", sizeof(m)); s_cat(m, on ? "can enter" : "blocked", sizeof(m));
      s_cat(m, "\",\"action\":\"frole\",\"actionLabel\":\"", sizeof(m)); s_cat(m, on ? "Allowed" : "Blocked", sizeof(m));
      s_cat(m, "\",\"actionStyle\":\"", sizeof(m)); s_cat(m, on ? "filled" : "outlined", sizeof(m)); s_cat(m, "\"}", sizeof(m));
    }
  }
  s_cat(m, "]},", sizeof(m));
  s_cat(m, "{\"title\":\"People\",\"items\":[", sizeof(m));
  static char mrows[8192]; int mfirst = 1;
  if (db_query(h, "SELECT pub,role FROM members ORDER BY pub", 0, mrows, sizeof(mrows)) > 0) {
    const char *cur = mrows; char obj[256];
    while (next_object(&cur, obj, sizeof(obj))) {
      char p[64], rl[48]; if (!jstr(obj, "pub", p, sizeof(p))) continue; rl[0] = 0; jstr(obj, "role", rl, sizeof(rl));
      const char *state = csv_has(deny, p) ? "Denied" : (csv_has(allow, p) ? "Allowed" : "Auto");
      char disp[40];
      if (s_eq(p, g_self)) s_cpy(disp, "You", sizeof(disp));
      else { char np[80]; if (hal_npub(p, s_len(p), np, sizeof(np)) && np[0]) { s_cpy(disp, np, sizeof(disp)); if (s_len(disp) > 16) disp[16] = 0; } else { s_cpy(disp, p, sizeof(disp)); if (s_len(disp) > 10) disp[10] = 0; } }
      if (!mfirst) s_cat(m, ",", sizeof(m)); mfirst = 0;
      s_cat(m, "{\"id\":\"", sizeof(m)); jesc(m, sizeof(m), p);
      s_cat(m, "\",\"title\":\"", sizeof(m)); jesc(m, sizeof(m), disp);
      s_cat(m, "\",\"subtitle\":\"", sizeof(m)); s_cat(m, rl[0] ? rl : "member", sizeof(m));
      s_cat(m, "\",\"action\":\"fmem\",\"actionLabel\":\"", sizeof(m)); s_cat(m, state, sizeof(m)); s_cat(m, "\"}", sizeof(m));
    }
  }
  s_cat(m, "]}]}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* ── folder public API (rail + chat view) ─────────────────────────────── */
static char g_edit[40];   /* folder being edited in the editor ("" = creating) */
static char g_acc[40];    /* folder whose access editor is open */

/* Render the current folder view: a left rail of permitted sub-folders + the
 * folder's own chat (the "active area"). g_browse is the current folder
 * ("" = circle root, which shows only the rail and no chat). */
void circle_folder_view(const char *circleId, const char *folderId) {
  int i = idx_by_id(circleId); if (i < 0) return;
  if (folderId[0] && !folder_can_enter(i, folderId, g_self)) {
    notify("info", "You don't have access to this folder"); return;
  }
  s_cpy(g_browse, folderId, sizeof(g_browse));
  s_cpy(g_chatfolder, folderId, sizeof(g_chatfolder));
  rail_set(i);
  field_set_raw("folderchat_active", folderId[0] ? "true" : "false");
  chat_clear("folderchat");
  char title[64]; s_cpy(title, "Folders", sizeof(title));
  if (folderId[0]) {
    int h = handle_for(i);
    static char row[6000]; long e = 0;
    if (folder_row(i, folderId, row, sizeof(row))) {
      jint(row, "epoch", &e);
      char nm[64]; if (jstr(row, "name", nm, sizeof(nm)) && nm[0]) s_cpy(title, nm, sizeof(title));
    }
    unsigned char k[32]; if (h >= 0 && e > 0 && !folder_key_get(i, folderId, e, k)) folder_send_keyreq(i, folderId);
    char par[80] = "[\""; jesc(par, sizeof(par), folderId); s_cat(par, "\"]", sizeof(par));
    static char rows[16384];
    if (h >= 0 && db_query(h, "SELECT author,ts,body FROM folder_events "
                  "WHERE folder=? AND body IS NOT NULL ORDER BY ts LIMIT 200", par, rows, sizeof(rows)) > 0) {
      const char *cur = rows; char obj[1024];
      while (next_object(&cur, obj, sizeof(obj))) {
        char author[64], body[600]; long ts = 0;
        jstr(obj, "author", author, sizeof(author)); jstr(obj, "body", body, sizeof(body)); jint(obj, "ts", &ts);
        chat_append("folderchat", s_eq(author, g_self) ? "out" : "in", author, body, ts);
      }
    }
  }
  screen_open_title("Folder view", title);
}
void circle_open_room_rail(const char *circleId) {
  int i = idx_by_id(circleId); if (i < 0) return;
  g_browse[0] = 0;                       /* circle root — Add creates here */
  rail_build(i, "conv_rail", "");     /* root sub-folders, no Up */
}
void circle_open_folders(const char *circleId) {
  circle_folder_view(circleId, "");      /* start at the circle root */
}

void circle_folder_open_edit(const char *circleId, const char *folderId) {
  int i = idx_by_id(circleId); if (i < 0) return;
  if (!owned(i)) { notify("error", "Only the owner can manage folders"); return; }
  s_cpy(g_edit, folderId, sizeof(g_edit));
  char nm[64] = "", de[256] = "", ic[256] = "", ty[16] = "chat";
  if (folderId[0]) {
    static char row[6000];
    if (folder_row(i, folderId, row, sizeof(row))) {
      jstr(row, "name", nm, sizeof(nm)); jstr(row, "description", de, sizeof(de));
      jstr(row, "icon", ic, sizeof(ic));
      jstr(row, "type", ty, sizeof(ty));
    }
  }
  field_set("folder_name", nm);
  field_set("folder_desc", de);
  field_set("folder_icon", ic);
  field_set("folder_type", ty);
  screen_open("Folder editor");
}
void circle_folder_edit_current(const char *circleId) {
  if (!g_browse[0]) { notify("info", "Open a folder first"); return; }
  circle_folder_open_edit(circleId, g_browse);
}
void circle_folder_save(const char *circleId, const char *name, const char *desc,
                        const char *icon, const char *type) {
  int i = idx_by_id(circleId); if (i < 0) return;
  if (!owned(i)) { notify("error", "Only the owner can manage folders"); return; }
  int h = handle_for(i); if (h < 0) return;
  if (!name[0]) { notify("error", "Folder needs a name"); return; }
  char ty[16]; s_cpy(ty, type[0] ? type : "chat", sizeof(ty));
  char ic[256]; s_cpy(ic, icon, sizeof(ic));   /* empty → host shows folder icon */
  if (g_edit[0]) {
    static char par[2048]; s_cpy(par, "[\"", sizeof(par)); jesc(par, sizeof(par), name);
    s_cat(par, "\",\"", sizeof(par)); jesc(par, sizeof(par), desc);
    s_cat(par, "\",\"", sizeof(par)); jesc(par, sizeof(par), ic);
    s_cat(par, "\",\"", sizeof(par)); jesc(par, sizeof(par), ty);
    s_cat(par, "\",\"", sizeof(par)); jesc(par, sizeof(par), g_edit); s_cat(par, "\"]", sizeof(par));
    db_exec(h, "UPDATE folders SET name=?,description=?,icon=?,type=? WHERE id=?", par);
  } else {
    unsigned char rnd[6]; hal_crypto_random((char *)rnd, 6); char fid[16]; hex_encode(rnd, 6, fid);
    static char par[2048]; s_cpy(par, "[\"", sizeof(par)); jesc(par, sizeof(par), fid);
    s_cat(par, "\",\"", sizeof(par)); jesc(par, sizeof(par), g_browse);
    s_cat(par, "\",\"", sizeof(par)); jesc(par, sizeof(par), name);
    s_cat(par, "\",\"", sizeof(par)); jesc(par, sizeof(par), ty);
    s_cat(par, "\",\"", sizeof(par)); jesc(par, sizeof(par), desc);
    s_cat(par, "\",\"", sizeof(par)); jesc(par, sizeof(par), ic);
    s_cat(par, "\",\"\",\"\",\"\",1,", sizeof(par));
    char nb[24]; num(nb, (long)hal_time_epoch()); s_cat(par, nb, sizeof(par)); s_cat(par, "]", sizeof(par));
    db_exec(h, "INSERT OR REPLACE INTO folders"
               "(id,parent,name,type,description,icon,roles,allow,deny,epoch,created) "
               "VALUES(?,?,?,?,?,?,?,?,?,?,?)", par);
    unsigned char key[32]; hal_crypto_random((char *)key, 32); folder_key_store(i, fid, 1, key);
    folder_distribute_key(i, fid, 1);
  }
  broadcast_keyset(i);
  rail_build(i, "conv_rail", "");        /* keep the in-circle rail fresh */
  notify("info", "Folder saved");
  if (g_browse[0]) circle_folder_view(circleId, g_browse); /* back to the folder */
  else screen_close();                      /* created at root → back to circle chat */
}
static void folder_remove_internal(int i, const char *folderId) {
  int h = handle_for(i); if (h < 0) return;
  char par[80] = "[\""; jesc(par, sizeof(par), folderId); s_cat(par, "\"]", sizeof(par));
  db_exec(h, "DELETE FROM folders WHERE id=?", par);
  db_exec(h, "DELETE FROM folder_keys WHERE folder=?", par);
  db_exec(h, "DELETE FROM folder_events WHERE folder=?", par);
}
void circle_folder_delete_current(const char *circleId) {
  int i = idx_by_id(circleId); if (i < 0) return;
  if (!owned(i)) { notify("error", "Only the owner can manage folders"); return; }
  if (!g_edit[0]) return;
  /* where to land after deleting: the deleted folder's parent if we deleted the
   * folder we're viewing, else stay on the current view. */
  static char row[6000]; char pa[40] = "";
  if (folder_row(i, g_edit, row, sizeof(row))) jstr(row, "parent", pa, sizeof(pa));
  int deletingCurrent = s_eq(g_edit, g_browse);
  folder_remove_internal(i, g_edit);
  broadcast_keyset(i);
  rail_build(i, "conv_rail", "");
  notify("info", "Folder removed");
  const char *land = deletingCurrent ? pa : g_browse;
  if (land[0]) circle_folder_view(circleId, land);
  else screen_close();   /* landed at the circle root → back to the circle chat */
}

void circle_folder_open_access(const char *circleId, const char *folderId) {
  int i = idx_by_id(circleId); if (i < 0) return;
  if (!owned(i)) { notify("error", "Only the owner can set access"); return; }
  s_cpy(g_acc, folderId, sizeof(g_acc));
  faccess_populate(i, folderId);
  screen_open("Folder access");
}
void circle_folder_access_current(const char *circleId) {
  if (!g_browse[0]) { notify("info", "Open a folder first"); return; }
  circle_folder_open_access(circleId, g_browse);
}
void circle_folder_role_toggle(const char *circleId, const char *role) {
  int i = idx_by_id(circleId); if (i < 0 || !g_acc[0]) return;
  if (!owned(i)) return;
  int h = handle_for(i); if (h < 0) return;
  static char row[6000]; char roles[256] = ""; if (folder_row(i, g_acc, row, sizeof(row))) jstr(row, "roles", roles, sizeof(roles));
  char nb[256]; s_cpy(nb, roles, sizeof(nb));
  if (csv_has(nb, role)) csv_remove(nb, sizeof(nb), role); else csv_add(nb, sizeof(nb), role);
  char par[400] = "[\""; jesc(par, sizeof(par), nb); s_cat(par, "\",\"", sizeof(par)); jesc(par, sizeof(par), g_acc); s_cat(par, "\"]", sizeof(par));
  db_exec(h, "UPDATE folders SET roles=? WHERE id=?", par);
  folder_rotate(i, g_acc);
  broadcast_keyset(i);
  faccess_populate(i, g_acc);
}
void circle_folder_member_cycle(const char *circleId, const char *m) {
  int i = idx_by_id(circleId); if (i < 0 || !g_acc[0]) return;
  if (!owned(i)) return;
  int h = handle_for(i); if (h < 0) return;
  static char row[6000]; char allow[2048] = "", deny[2048] = "";
  if (folder_row(i, g_acc, row, sizeof(row))) { jstr(row, "allow", allow, sizeof(allow)); jstr(row, "deny", deny, sizeof(deny)); }
  int a = csv_has(allow, m), d = csv_has(deny, m);
  if (!a && !d) { csv_add(allow, sizeof(allow), m); }
  else if (a) { csv_remove(allow, sizeof(allow), m); csv_add(deny, sizeof(deny), m); }
  else { csv_remove(deny, sizeof(deny), m); }
  static char par[5000]; s_cpy(par, "[\"", sizeof(par)); jesc(par, sizeof(par), allow);
  s_cat(par, "\",\"", sizeof(par)); jesc(par, sizeof(par), deny);
  s_cat(par, "\",\"", sizeof(par)); jesc(par, sizeof(par), g_acc); s_cat(par, "\"]", sizeof(par));
  db_exec(h, "UPDATE folders SET allow=?,deny=? WHERE id=?", par);
  folder_rotate(i, g_acc);
  broadcast_keyset(i);
  faccess_populate(i, g_acc);
}
int circle_folder_send(const char *circleId, const char *text) {
  int i = idx_by_id(circleId); if (i < 0) return 0;
  if (!g_chatfolder[0]) { notify("info", "Open a folder to post"); return 0; }
  const char *folderId = g_chatfolder;
  ensure_self();
  int h = handle_for(i); if (h < 0) return 0;
  if (!folder_can_enter(i, folderId, g_self)) { notify("error", "No access to this folder"); return 0; }
  static char row[6000]; long e = 0; if (folder_row(i, folderId, row, sizeof(row))) jint(row, "epoch", &e);
  unsigned char key[32];
  if (e <= 0 || !folder_key_get(i, folderId, e, key)) { notify("error", "No folder key yet"); return 0; }
  char body[MAX_TEXT + 1]; s_cpy(body, text, sizeof(body)); if (!body[0]) return 0;
  unsigned char ct[1024];
  uint32_t n = hal_crypto_aes_encrypt((const char *)key, 32, body, s_len(body), (char *)ct, sizeof(ct));
  if (n == 0) { notify("error", "Encrypt failed"); return 0; }
  char ctb[1400]; if (b64url_encode(ct, n, ctb, sizeof(ctb)) < 0) return 0;
  long ts = (long)hal_time_epoch();
  static char canon[1600]; folder_canonical(canon, sizeof(canon), c_id[i], folderId, e, g_self, ts, ctb);
  char sig[160]; if (hal_identity_sign(canon, s_len(canon), sig, sizeof(sig)) == 0) { notify("error", "Sign failed"); return 0; }
  char id[20]; event_id(sig, id);
  folder_event_store(h, id, folderId, e, g_self, ts, ctb, sig, body);
  static char d[2400]; s_cpy(d, "{\"k\":\"fm\",\"c\":\"", sizeof(d));
  for (int j = 0; j < TAGLEN; j++) { char ch[2] = { c_id[i][j], 0 }; s_cat(d, ch, sizeof(d)); }
  s_cat(d, "\",\"f\":\"", sizeof(d)); jesc(d, sizeof(d), folderId);
  char b[24];
  s_cat(d, "\",\"e\":", sizeof(d)); num(b, e); s_cat(d, b, sizeof(d));
  s_cat(d, ",\"a\":\"", sizeof(d)); s_cat(d, g_self, sizeof(d));
  s_cat(d, "\",\"t\":", sizeof(d)); num(b, ts); s_cat(d, b, sizeof(d));
  s_cat(d, ",\"x\":\"", sizeof(d)); s_cat(d, ctb, sizeof(d));
  s_cat(d, "\",\"s\":\"", sizeof(d)); s_cat(d, sig, sizeof(d)); s_cat(d, "\"}", sizeof(d));
  hal_rns_broadcast(d, s_len(d));
  chat_append("folderchat", "out", g_self, body, ts);
  return 1;
}

/* ── Join requests (anyone scans the QR / pastes the key and applies) ───── */

/* Extract a full 64-hex circleId from any reference ("circle:<hex>", a QR
 * payload, or bare hex). Returns 1 and writes lowercase hex on success. */
static int extract_full_id(const char *s, char *out) {
  int run = 0; const char *start = 0;
  for (const char *p = s;; p++) {
    char c = *p;
    int hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (hex) {
      if (run == 0) start = p;
      run++;
      if (run == 64) {
        for (int k = 0; k < 64; k++) {
          char ch = start[k];
          out[k] = (ch >= 'A' && ch <= 'F') ? (char)(ch + 32) : ch;
        }
        out[64] = 0; return 1;
      }
    } else {
      run = 0;
    }
    if (!c) break;
  }
  return 0;
}

/* Owner side: someone applied to join. Verify the request is signed by the
 * applicant's key, then queue it for approval. */
static void handle_jr(const char *json) {
  char tag[24], cid[80], frm[64], nm[80], sig[160];
  if (!jstr(json, "c", tag, sizeof(tag))) return;
  int i = idx_by_tag(tag); if (i < 0) return;
  if (!owned(i)) return;                 /* only the owner handles applications */
  cid[0] = 0; jstr(json, "cid", cid, sizeof(cid));
  if (!s_eq(cid, c_id[i])) return;       /* full id must match this circle */
  frm[0] = 0; jstr(json, "frm", frm, sizeof(frm)); if (!frm[0]) return;
  nm[0] = 0; jstr(json, "nm", nm, sizeof(nm));
  sig[0] = 0; jstr(json, "s", sig, sizeof(sig));
  char dl[80] = "", pp[80] = "";
  jstr(json, "dl", dl, sizeof(dl));
  jstr(json, "pp", pp, sizeof(pp));
  char canon[160]; canon[0] = 0;
  s_cat(canon, cid, sizeof(canon)); s_cat(canon, "|", sizeof(canon)); s_cat(canon, frm, sizeof(canon));
  if (!sig[0] || !hal_verify(frm, s_len(frm), canon, s_len(canon), sig, s_len(sig))) return;
  if (is_member(i, frm, 0)) return;      /* already a member */
  int h = handle_for(i); if (h < 0) return;
  char par[320] = "[\""; jesc(par, sizeof(par), frm);
  s_cat(par, "\",\"", sizeof(par)); jesc(par, sizeof(par), nm);
  s_cat(par, "\",", sizeof(par)); char tb[24]; num(tb, (long)hal_time_epoch()); s_cat(par, tb, sizeof(par));
  s_cat(par, ",\"", sizeof(par)); jesc(par, sizeof(par), dl);
  s_cat(par, "\",\"", sizeof(par)); jesc(par, sizeof(par), pp); s_cat(par, "\"]", sizeof(par));
  db_exec(h, "INSERT OR REPLACE INTO requests(pub,nick,ts,deliv,prop) VALUES(?,?,?,?,?)", par);
  audit_add(i, "applied", frm);
  notify("info", "Someone applied to join your circle");
  people_set_members(i);                 /* refresh the People panel if open */
}

/* Build + broadcast a signed join request (jr) for the full circle id [cid]. */
/* Build + send a signed join request for [cid]. If [ownerDeliv] is non-empty,
 * deliver it ADDRESSED to the owner (reliable, store-and-forward) in addition to
 * the broadcast fallback — we also include OUR deliv/prop so the owner can reach
 * us back without re-discovery. */
static void short_seed_from_id(const char *id, char *out);

static int send_join_request_to(const char *cid, const char *ownerDeliv) {
  if (idx_by_id(cid) >= 0) { notify("info", "You're already in this circle"); return 0; }
  char canon[160]; canon[0] = 0;
  s_cat(canon, cid, sizeof(canon)); s_cat(canon, "|", sizeof(canon)); s_cat(canon, g_self, sizeof(canon));
  char sig[160]; if (hal_identity_sign(canon, s_len(canon), sig, sizeof(sig)) == 0) {
    notify("error", "Sign failed"); return 0;
  }
  ensure_dests();
  /* No "nm": the owner shows the applicant's npub (derived from "frm"); dropping
   * it keeps the datagram small enough to ride ONE encrypted rendezvous packet. */
  char d[700] = "{\"k\":\"jr\",\"c\":\"";
  for (int j = 0; j < TAGLEN; j++) { char ch[2] = { cid[j], 0 }; s_cat(d, ch, sizeof(d)); }
  s_cat(d, "\",\"cid\":\"", sizeof(d)); s_cat(d, cid, sizeof(d));
  s_cat(d, "\",\"frm\":\"", sizeof(d)); s_cat(d, g_self, sizeof(d));
  s_cat(d, "\",\"dl\":\"", sizeof(d)); s_cat(d, g_deliv, sizeof(d));
  s_cat(d, "\",\"pp\":\"", sizeof(d)); s_cat(d, g_prop, sizeof(d));
  s_cat(d, "\",\"s\":\"", sizeof(d)); s_cat(d, sig, sizeof(d));
  s_cat(d, "\"}", sizeof(d));
  hal_rns_broadcast(d, s_len(d));
  if (ownerDeliv && ownerDeliv[0])
    hal_rns_send_to(ownerDeliv, s_len(ownerDeliv), d, s_len(d));
  /* First-contact channel: also deliver the jr to the circle's RENDEZVOUS dest
   * as one encrypted connectionless packet. The owner listens there (its rv dest
   * is re-announced every ~8s so the hub keeps a fresh route to it), so this
   * lands even when the owner's normal delivery-dest inbound is path-stale and a
   * direct link push fails. */
  char seed[16]; short_seed_from_id(cid, seed);
  hal_rns_rv_send(seed, s_len(seed), d, s_len(d));
  notify("info", "Application sent — waiting for the owner to approve");
  return 1;
}
static int send_join_request(const char *cid) {
  return send_join_request_to(cid, 0);
}

/* Normalized rendezvous seed for a circle: "<first3>-<last3>" of its full id —
 * the SAME value the joiner derives from the short code they typed. */
static void short_seed_from_id(const char *id, char *out) {
  unsigned n = s_len(id);
  if (n < 6) { s_cpy(out, id, 16); return; }
  out[0] = id[0]; out[1] = id[1]; out[2] = id[2]; out[3] = '-';
  out[4] = id[n - 3]; out[5] = id[n - 2]; out[6] = id[n - 1]; out[7] = 0;
}

/* Owner: announce a rendezvous beacon for each circle we own, carrying the full
 * id + our delivery dest, so a joiner holding only the short code can find us. */
static void rv_announce_owned(void) {
  ensure_dests();
  for (int i = 0; i < c_n; i++) {
    if (!owned(i)) continue;
    char seed[16]; short_seed_from_id(c_id[i], seed);
    char app[200]; app[0] = 0;
    s_cat(app, c_id[i], sizeof(app)); s_cat(app, "|", sizeof(app)); s_cat(app, g_deliv, sizeof(app));
    hal_rns_rv_announce(seed, s_len(seed), app, s_len(app));
  }
}

/* ── short-code discovery (find a circle across Reticulum by its short id) ──
 * The short code (circle/<first3>-<last3>) is a lossy ~24-bit reference, not a
 * key, so a joiner who has only the code can't derive the full circleId. They
 * broadcast a discovery request (cd) carrying the first3/last3; the owner whose
 * circle matches answers with a circle offer (co) carrying the authoritative
 * full id, after which the joiner sends a normal signed join request. The full
 * id remains the only thing membership is proven against. */
static char g_find_first[4];
static char g_find_last[4];
static int  g_find_on = 0;
static int  g_find_tries = 0;

/* Parse a short code ("circle/abc-xyz", "circle:abc-xyz" or "abc-xyz") into its
 * first3 + last3 parts. Returns 1 on success. */
static int parse_short(const char *code, char *first, char *last) {
  const char *p = code, *sl = code;
  for (const char *q = code; *q; q++) if (*q == ':' || *q == '/') sl = q + 1;
  if (sl != code) p = sl;
  const char *dash = 0;
  for (const char *q = p; *q; q++) if (*q == '-') dash = q;
  if (!dash) return 0;
  unsigned fl = (unsigned)(dash - p);
  if (fl == 0 || fl > 3) return 0;
  for (unsigned k = 0; k < fl && k < 3; k++) first[k] = p[k];
  first[fl < 3 ? fl : 3] = 0;
  const char *lp = dash + 1; unsigned ll = s_len(lp);
  if (ll == 0 || ll > 3) return 0;
  s_cpy(last, lp, 4);
  return 1;
}

/* Owner side: answer a discovery request for any circle we own whose short code
 * matches, handing back the authoritative full id + name. */
static void handle_cd(const char *json) {
  char f[8] = "", l[8] = "";
  jstr(json, "f", f, sizeof(f));
  jstr(json, "l", l, sizeof(l));
  if (!f[0] || !l[0]) return;
  for (int i = 0; i < c_n; i++) {
    if (!owned(i)) continue;            /* only the owner can approve, so only it answers */
    unsigned n = s_len(c_id[i]); if (n < 6) continue;
    int pre = (c_id[i][0] == f[0] && c_id[i][1] == f[1] && c_id[i][2] == f[2]);
    int suf = (c_id[i][n - 3] == l[0] && c_id[i][n - 2] == l[1] && c_id[i][n - 1] == l[2]);
    if (!(pre && suf)) continue;
    char d[200] = "{\"k\":\"co\",\"cid\":\"";
    s_cat(d, c_id[i], sizeof(d));
    s_cat(d, "\",\"n\":\"", sizeof(d)); jesc(d, sizeof(d), c_name[i]);
    s_cat(d, "\"}", sizeof(d));
    hal_rns_broadcast(d, s_len(d));
  }
}

/* Joiner side: a circle offer arrived. If it matches our pending short-code
 * search, learn the full id and apply to join. */
static void handle_co(const char *json) {
  if (!g_find_on) return;
  char cid[80] = "", nm[64] = "";
  if (!jstr(json, "cid", cid, sizeof(cid))) return;
  unsigned n = s_len(cid); if (n < 6) return;
  int pre = (cid[0] == g_find_first[0] && cid[1] == g_find_first[1] && cid[2] == g_find_first[2]);
  int suf = (cid[n - 3] == g_find_last[0] && cid[n - 2] == g_find_last[1] && cid[n - 1] == g_find_last[2]);
  if (!(pre && suf)) return;           /* not the circle we're looking for */
  g_find_on = 0;
  jstr(json, "n", nm, sizeof(nm));
  char msg[120] = "Found ";
  s_cat(msg, nm[0] ? nm : "the circle", sizeof(msg));
  s_cat(msg, " — applying to join", sizeof(msg));
  notify("info", msg);
  send_join_request(cid);
}

/* Poll the rendezvous for the pending short code: derive the same seed the owner
 * announced under, resolve its address, then send an ADDRESSED join request.
 * Retries (the resolve is a path request that may take a few ticks). */
static void discovery_tick(void) {
  if (!g_find_on) return;
  if (g_find_tries >= 20) {
    g_find_on = 0;
    notify("error", "Couldn't find that circle — check the code or that the owner is online");
    return;
  }
  g_find_tries++;
  char seed[16]; seed[0] = 0;
  s_cat(seed, g_find_first, sizeof(seed)); s_cat(seed, "-", sizeof(seed));
  s_cat(seed, g_find_last, sizeof(seed));
  char app[256];
  uint32_t n = hal_rns_rv_resolve(seed, s_len(seed), app, sizeof(app) - 1);
  if (n == 0) return;                 /* still resolving — try again next tick */
  app[n] = 0;
  /* app = "<fullId>|<ownerDeliv>" */
  char fullId[80] = "", ownerDeliv[80] = "";
  const char *bar = 0;
  for (const char *q = app; *q; q++) if (*q == '|') { bar = q; break; }
  if (!bar) return;
  unsigned idlen = (unsigned)(bar - app);
  if (idlen == 0 || idlen >= sizeof(fullId)) return;
  for (unsigned k = 0; k < idlen; k++) fullId[k] = app[k];
  fullId[idlen] = 0;
  s_cpy(ownerDeliv, bar + 1, sizeof(ownerDeliv));
  /* sanity: the short code must match this full id (guards collisions) */
  unsigned fn = s_len(fullId); if (fn < 6) return;
  int pre = (fullId[0] == g_find_first[0] && fullId[1] == g_find_first[1] && fullId[2] == g_find_first[2]);
  int suf = (fullId[fn - 3] == g_find_last[0] && fullId[fn - 2] == g_find_last[1] && fullId[fn - 1] == g_find_last[2]);
  if (!(pre && suf)) return;
  g_find_on = 0;
  notify("info", "Found the circle — applying to join");
  send_join_request_to(fullId, ownerDeliv);
}

int circle_apply_join(const char *ref) {
  ensure_self();
  if (!g_self[0]) { notify("error", "No identity yet — set up your profile first"); return 0; }
  char cid[80];
  /* Full key (works for any circle, incl. ones we've never seen), or a short
   * code we already know locally — send the join request straight away. */
  if (extract_full_id(ref, cid) || circle_resolve_short(ref, cid, sizeof(cid))) {
    return send_join_request(cid);
  }
  /* Short code for a circle we've never seen: resolve its rendezvous to the
   * owner's address (path request), then apply addressed. discovery_tick polls. */
  if (parse_short(ref, g_find_first, g_find_last)) {
    g_find_on = 1; g_find_tries = 0;
    discovery_tick();   /* kick off the rendezvous resolve immediately */
    notify("info", "Searching for that circle across the network…");
    return 1;
  }
  notify("error", "Couldn't read that circle — enter a short code like ab1-9xz or the full key");
  return 0;
}

void circle_approve_request(const char *circleId, const char *pub) {
  int i = idx_by_id(circleId); if (i < 0) return;
  if (!owned(i)) { notify("error", "Only the owner can approve"); return; }
  int h = handle_for(i); if (h < 0) return;
  /* Pull the applicant's RNS dests from the request so the keyset/key are
   * delivered ADDRESSED to them (reliable). */
  char par[80] = "[\""; jesc(par, sizeof(par), pub); s_cat(par, "\"]", sizeof(par));
  char row[256]; char dl[80] = "", pp[80] = "";
  if (db_query(h, "SELECT deliv,prop FROM requests WHERE pub=?", par, row, sizeof(row)) > 0) {
    jstr(row, "deliv", dl, sizeof(dl));
    jstr(row, "prop", pp, sizeof(pp));
  }
  add_member_pub2(i, pub, dl, pp);        /* bumps epoch, adds + wraps keys, republishes addressed */
  db_exec(h, "DELETE FROM requests WHERE pub=?", par);
  audit_add(i, "approved", pub);
  people_set_members(i);
  notify("info", "Approved — they're in the circle");
}

void circle_reject_request(const char *circleId, const char *pub) {
  int i = idx_by_id(circleId); if (i < 0) return;
  if (!owned(i)) return;
  int h = handle_for(i); if (h < 0) return;
  char par[80] = "[\""; jesc(par, sizeof(par), pub); s_cat(par, "\"]", sizeof(par));
  db_exec(h, "DELETE FROM requests WHERE pub=?", par);
  audit_add(i, "rejected", pub);
  people_set_members(i);
  notify("info", "Request dismissed");
}
