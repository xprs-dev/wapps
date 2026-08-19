/*
 * APRS station wapp — Map / Messenger / Beacon / Settings.
 *
 * Mirrors the mature XPRS APRS UI on top of Aurora primitives:
 *  - Map      : pins for stations/messages received in the filter area
 *               (host renders ui.map.marker pushed from parsed packets)
 *  - Messenger: chat view of APRS text messages addressed to us
 *  - Beacon   : craft a position / status / emergency / timed beacon
 *  - Settings : callsign, position, network, filter, path, tags
 *
 * Networking is the reusable aprs.{h,c} library over the hal_socket_*
 * HAL. The APRS-IS passcode is computed (aprs_passcode) so we can TX.
 */
#include <stdint.h>
#include "xprs_wasm_hal.h"
#include "chat.h"
#include "thread.h"
#include "ble.h"
#include "room.h"
#include "xprs.h"

/* ── tiny libc ──────────────────────────────────────────────────────── */
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
static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
/* APRS message acks/rejects: the WHOLE body is "ack<id>" or "rej<id>" where
 * <id> is 1-5 ALPHANUMERIC chars (APRS spec) — bots like MPAD use letter ids
 * ("ackKU"). Full-match only: "acknowledge this" or "ack123 extra" is chat
 * text, not an ack. A letter-id ack that slipped through here used to be
 * bridged BLE<->IS and re-originated under the addressee's callsign with a
 * bogus {0 id — spoofed malformed acks that message bots answered forever. */
static int is_ack_text(const char *t) {
  if (!((t[0] == 'a' && t[1] == 'c' && t[2] == 'k') ||
        (t[0] == 'r' && t[1] == 'e' && t[2] == 'j'))) return 0;
  int n = 0;
  for (; t[3 + n]; n++) {
    char c = t[3 + n];
    int alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                (c >= 'A' && c <= 'Z');
    if (!alnum || n >= 5) return 0;
  }
  return n >= 1;
}

/* extract "key":"value" from buf; returns 1 if found */
/* hex digit -> value, or -1 */
static int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
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
        /* Decode \uXXXX -> one byte. The host JSON-encodes received BLE bytes,
         * so the 0x1f field separator arrives as ""; without this it was
         * copied as the literal text "u001f" and frames couldn't be split. */
        if (*p == 'u' && hexv(p[1]) >= 0 && hexv(p[2]) >= 0 &&
            hexv(p[3]) >= 0 && hexv(p[4]) >= 0) {
          int v = (hexv(p[1]) << 12) | (hexv(p[2]) << 8) |
                  (hexv(p[3]) << 4) | hexv(p[4]);
          p += 5;
          out[i++] = (char)(v & 0xff);
        } else {
          out[i++] = *p++;
        }
      } else out[i++] = *p++;
    }
    out[i] = 0; return 1;
  }
  out[0] = 0; return 0;
}
/* read a JSON bool: matches "key":true / "key":1 (host sends bools unquoted) */
/* Parse a boolean field; return `def` when the key is absent (so callers can
 * have a true default that an explicit "false" still overrides). */
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
static int jbool(const char *buf, const char *key) { return jbool_def(buf, key, 0); }
/* read a JSON integer: "key":123 (bare number). 0 when absent. */
static int jint(const char *buf, const char *key) {
  char pat[80]; pat[0] = '"'; pat[1] = 0;
  s_cat(pat, key, sizeof(pat)); s_cat(pat, "\":", sizeof(pat));
  unsigned pl = s_len(pat);
  for (const char *p = buf; *p; p++) {
    int ok = 1;
    for (unsigned i = 0; i < pl; i++) { if (p[i] != pat[i]) { ok = 0; break; } }
    if (!ok) continue;
    p += pl; while (*p == ' ') p++;
    int v = 0; while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    return v;
  }
  return 0;
}
/* First value of a NOSTR event tag ["<name>","<value>", …]. Returns 1 on hit. */
static int evt_tag(const char *evt, const char *name, char *out, unsigned cap) {
  out[0] = 0;
  char pat[16] = "[\"";
  s_cat(pat, name, sizeof(pat)); s_cat(pat, "\",\"", sizeof(pat));
  unsigned pl = s_len(pat);
  for (const char *p = evt; *p; p++) {
    int ok = 1;
    for (unsigned i = 0; i < pl; i++) { if (p[i] != pat[i]) { ok = 0; break; } }
    if (!ok) continue;
    p += pl;
    unsigned i = 0;
    while (*p && *p != '"' && i < cap - 1) out[i++] = *p++;
    out[i] = 0;
    return 1;
  }
  return 0;
}
static double to_dbl(const char *s) {
  int neg = 0; if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
  double v = 0; while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
  if (*s == '.') { s++; double f = 0.1; while (*s >= '0' && *s <= '9') { v += (*s - '0') * f; f *= 0.1; s++; } }
  return neg ? -v : v;
}
static int to_int(const char *s) { return (int)to_dbl(s); }

/* append a number with 4 decimals to dst */
static void append_dbl(char *dst, unsigned m, double v) {
  unsigned l = s_len(dst);
  if (v < 0 && l < m - 1) { dst[l++] = '-'; v = -v; }
  int w = (int)v;
  int f = (int)((v - w) * 10000.0 + 0.5);
  if (f >= 10000) { w += 1; f -= 10000; }
  char wb[16]; int wi = 0;
  if (w == 0) wb[wi++] = '0';
  while (w > 0 && wi < 15) { wb[wi++] = (char)('0' + w % 10); w /= 10; }
  while (wi > 0 && l < m - 1) dst[l++] = wb[--wi];
  if (l < m - 6) {
    dst[l++] = '.';
    dst[l++] = (char)('0' + (f / 1000) % 10);
    dst[l++] = (char)('0' + (f / 100) % 10);
    dst[l++] = (char)('0' + (f / 10) % 10);
    dst[l++] = (char)('0' + f % 10);
  }
  dst[l] = 0;
}

/* JSON-escape src into dst (for embedding text in our outbox messages) */
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

/* ── state ──────────────────────────────────────────────────────────── */
static int   g_sock = -1;
static int   g_logged = 0;
static int   g_seq = 1;
static char  g_call[16] = "N0CALL";  /* replaced at init by hal_identity() */
/* The device profile's own callsign, snapshotted at init and NEVER overridden
 * by settings. g_call can drift (a stale saved Settings callsign fed to a
 * background engine); self-checks must match EITHER, or the station starts
 * showing + notifying its own boomeranged posts as if from someone else. */
static char  g_idcall[16] = "";
/* Is [c] one of OUR callsigns (case-insensitive): the working g_call OR the
 * immutable profile identity g_idcall? Used to drop our own frames looping
 * back over any transport (APRS-IS echo, BLE iGate relay, digipeat). */
static int is_self_call(const char *c) {
  const char *own[2] = { g_call, g_idcall };
  for (int k = 0; k < 2; k++) {
    if (!own[k][0]) continue;
    int eq = 1;
    for (int i = 0; own[k][i] || c[i]; i++)
      if (up(own[k][i]) != up(c[i])) { eq = 0; break; }
    if (eq) return 1;
  }
  return 0;
}

/* ── APRS-IS access control ────────────────────────────────────────────────
 * APRS-IS feeds licensed amateur-radio networks, so it may only be used with
 * a callsign assigned by a radio authority. XPRS's auto-generated X1/X3
 * identities are NOT valid there: the connection is OFF by default and comes
 * up only after the user supplies a licensed callsign + its (verified)
 * APRS-IS passcode in the APRS panel. While enabled, that callsign IS the
 * station's working callsign (g_call) on every transport, so signatures and
 * key beacons stay consistent. */
static int  g_aprsis_on = 0;            /* master switch (default OFF) */
static char g_aprsis_call[16] = "";     /* licensed callsign for APRS-IS */
static int  g_aprsis_pass = -1;         /* verified APRS-IS passcode */

/* A XPRS auto-generated callsign: "X1…"/"X3…". The X1/X3 prefixes are not
 * allocated by the ITU, so they can't be authority-assigned — frames from
 * such calls must NEVER be originated onto APRS-IS (own or relayed). */
static int is_autogen_call(const char *c) {
  return c[0] && up(c[0]) == 'X' && (c[1] == '1' || c[1] == '3');
}

/* Transport chip shown on messages: the internal token for APRS-IS-arrived
 * traffic is "NET" (compared all over the routing code); show it to the user
 * as "APRS-IS" so the origin is explicit; "RET" (Reticulum, the primary
 * transport) is spelled out. Other tags (BLE/RLY) pass through unchanged. */
static const char *via_label(const char *via) {
  if (via && via[0]=='N' && via[1]=='E' && via[2]=='T' && !via[3]) return "APRS-IS";
  if (via && via[0]=='R' && via[1]=='E' && via[2]=='T' && !via[3]) return "Reticulum";
  return via;
}
static double g_lat = 0, g_lon = 0;
static int   g_radius = 100;
static char  g_symbol[8] = "/>";
static char  g_path[64] = "WIDE1-1,WIDE2-1";
static int   g_auto = 0;
static int   g_interval = 600;          /* seconds */
static int   g_mail_days = 7;           /* ?MAIL look-back window sent to iGates */
static uint64_t g_last_beacon = 0;
/* Auto-connect / auto-reconnect state. */
static int   g_want_connect = 0;        /* keep a connection alive */
static char  g_host[64] = APRS_DEFAULT_HOST;
static int   g_port = APRS_DEFAULT_PORT;
static uint64_t g_last_reconnect = 0;

/* BLE transport (shared adapter via hal_ble_*). g_ble_on = exchange enabled
 * (on by default — matches the "Exchange over Bluetooth" default in
 * screens/home.ui.json); g_ble_relay = act as a full iGate, bridging frames
 * both ways between BLE and APRS-IS (ON by default; persisted in KV "igate");
 * g_ble_started tracks whether we've told the HAL to scan. */
static int g_ble_on = 1, g_ble_relay = 1, g_ble_started = 0;

/* Which broadcast-channel classes this device listens to. All ON by default —
 * the point is the OFF switch: a user who wants only the moderated rooms can
 * silence the local chatter, the worldwide firehose, or the NomadNet bridge
 * without blocking people one by one. Persisted in KV "chan" as 3 chars.
 *   local  — nearby groups (BLE / local Reticulum, ids like "#NAME")
 *   global — worldwide groups over internet Reticulum nodes ("#NAME*")
 *   nomad  — the #NOMADNET bridge (LXMF messages from non-xprs nodes)  */
static int g_chan_local = 1, g_chan_global = 1, g_chan_nomad = 1;
static void chan_save(void) {
  char b[4] = { g_chan_local ? '1' : '0', g_chan_global ? '1' : '0',
                g_chan_nomad ? '1' : '0', 0 };
  hal_kv_set("chan", 4, b, 3);
}
static void chan_load(void) {
  char b[4];
  if (hal_kv_get("chan", 4, b, 3) >= 3) {
    g_chan_local = b[0] == '1';
    g_chan_global = b[1] == '1';
    g_chan_nomad = b[2] == '1';
  }
}
/* Is channel [id] enabled under the current toggles? Rooms always are. */
static int chan_enabled(const char *id) {
  if (!id || id[0] != '#') return 1;
  if (s_eq(id, "#NOMADNET")) return g_chan_nomad;
  for (int i = 1; id[i]; i++) if (id[i] == '*') return g_chan_global;
  return g_chan_local;
}
static uint64_t g_ble_last_beacon = 0;
static uint64_t g_ble_last_hello = 0;   /* last lightweight BLE presence beacon */
/* compact BLE senders, defined with the module entry points */
static void ble_tx_msg(const char *to, const char *text);
/* Best-hope custody: air a 1:1 only when nothing else can carry it. Defined
 * with the other BLE senders; declared here because send_message is above. */
/* Substring search — defined with the nearby-list helpers, used earlier by the
 * custody identity lookup. */
static const char *s_find(const char *hay, const char *needle);
#define REACH_NONE  0   /* nothing anywhere → best hope: air it */
#define REACH_LOCAL 1   /* BLE/radio neighbour → air it, arrives directly */
#define REACH_NET   2   /* LAN or live internet path → do NOT air it */

/* Where can this callsign be reached right now? Defined next to the nearby
 * table it reads. */
static int reach_class(const char *call);
static void ble_tx_pos(double lat, double lon, const char *comment);
static void log_line(const char *field, const char *text);
/* Build "label/value" chips for callsigns heard over BLE within REACH_WINDOW
 * (most-recent first). Returns the number of chips written (defined with the
 * seen-over-BLE registry, far below). */
static int ble_reach_chips(char *out, unsigned max);
/* Reticulum 1:1 sender (defined after the BLE frame packer); fans the same frame
 * out to every RNS delivery dest advertised under the recipient's npub. */
static int rns_tx_msg(const char *to, const char *wire);
static int rns_tx_public(const char *to, const char *wire);
/* Reticulum is the PRIMARY transport (APRS-IS is legacy/opt-in and requires a
 * licensed callsign): 1 when the local RNS node is up. Defined with rns_tx_msg. */
static int rns_up(void);
/* Broadcast a position over the licence-free paths (BLE if on, Reticulum if up).
 * Defined with the BLE frame packer. */
static void pos_broadcast(double lat, double lon, const char *comment);
/* Broadcast a group bulletin / geo-chat frame over Reticulum (same compact
 * frame as BLE, so receivers dedup cross-transport copies). No-op when the
 * RNS node is down. Defined with the BLE frame packer. */
static void rns_tx_bulletin(const char *to, const char *text);

/* ── Public-key beacon ───────────────────────────────────────────────────
 * Periodically broadcast this station's public key so peers can map our
 * callsign -> pubkey and later send us encrypted messages. The host hands us
 * the key as base64url of the raw 32 bytes (43 chars — an npub bech32 string
 * would be 63, too tight for one 67-char APRS message and bulky on a BLE
 * advert). Sent as an APRS bulletin to the well-known group "NOSTR" (the
 * addressee BLN..NOSTR is the "code"; the frame's from-field is the callsign;
 * the text is the base64 key) and, identically, over BLE as "#NOSTR". The key
 * never changes, so the rate is low. ON by default; the user can disable it in
 * Settings (persisted to KV). Receivers base64url-decode it back to 32 bytes.
 */
#define PKBEACON_GROUP    "NOSTR"
#define PKBEACON_INTERVAL 3600            /* seconds (hourly) */
#define RNS_PULL_INTERVAL 20              /* seconds: pull store-and-forwarded 1:1 mail */
static char  g_pubkey[80] = "";           /* our pubkey (base64url), cached at init */
static int   g_pubkey_beacon = 1;         /* broadcast it? (default on) */
static uint64_t g_last_pkbeacon = 0;
static uint64_t g_last_rnspull = 0;

/* ── Message signing (XPRS verifiable authorship) ────────────────────────
 * When enabled, outgoing messages carry a short-Schnorr signature so peers can
 * verify the author. The signature is 48 bytes -> 60 base85 chars, appended as
 * " ~<sig>" (one extra APRS line). Verification needs the sender's pubkey, kept
 * in a callsign->pubkey map filled from received NOSTR beacons (§10). Both the
 * crypto and the base85 live host-side (hal_identity_sign / hal_verify); the
 * private key never reaches the wapp. OFF by default (a signature ~doubles a
 * short message); persisted in KV. */
static int g_sign_msgs = 0;
#define PK_MAX 64
static char g_pk_call[PK_MAX][16];        /* callsign -> */
static char g_pk_key[PK_MAX][48];         /* pubkey (base64url, ~43 chars) */
static uint64_t g_pk_ts[PK_MAX];          /* last time we heard this station's key */
static int  g_pk_n = 0;
static void pk_render(void);              /* fwd: refresh the Keys list view */

/* ── followed callsigns (Activity feed) ─────────────────────────────────────
 * A Twitter-style stream: callsigns we "follow" have their public activity
 * (posts, replies, likes, status) surfaced in the Activity tab. We pull their
 * packets from APRS-IS with a b/ budlist filter (every packet FROM them) and,
 * over BLE, hear them whenever they're in range. The list persists in KV
 * "follows" (";"-joined). Our own micro-posts go out as bulletins to a shared
 * feed group so followers see them too. */
#define FOLLOW_MAX 32
#define FEED_GROUP  "FEED"                 /* shared micro-blog group for posts */
static char g_follow[FOLLOW_MAX][16];
static char g_ftag[FOLLOW_MAX][48];        /* space-separated tags, per follow */
static int  g_follow_n = 0;
/* Stations that follow US — learned from directed "?FOLLOW"/"?UNFOLLOW"
 * control messages peers send when they (un)follow a callsign. */
static char g_follower[FOLLOW_MAX][16];
static int  g_follower_n = 0;
static void follow_render(void);          /* fwd: push the people list */
static void profile_show(const char *call);   /* fwd: station profile sheet */
static void prompt_ftag(const char *call);    /* fwd: edit-tags prompt */
static void host_state_emit(const char *kind, const char *call, int on); /* fwd */
static int is_following(const char *call) {
  for (int i = 0; i < g_follow_n; i++) if (s_eq(g_follow[i], call)) return 1;
  return 0;
}
static int is_follower(const char *call) {
  for (int i = 0; i < g_follower_n; i++) if (s_eq(g_follower[i], call)) return 1;
  return 0;
}

/* ── BLE ping (Tools tab): local reach test across digipeaters ──────────
 * A ping is a BLE-only broadcast (never APRS-IS, never shown on the Live
 * feed). Every BLE station answers once with its callsign + position and
 * forwards the ping (ttl) so it travels further; replies are forwarded back
 * (pttl) so multi-hop responders still reach the pinger. */
#define PING_TO "?PING"
#define PONG_TO "?PONG"
#define PING_DEFAULT_TTL 3
#define GPS_NA (-2147483647 - 1)   /* hal_sensor_gps_* "unavailable" sentinel */
static int      g_ping_active = 0;   /* collecting replies for our ping */
static unsigned g_ping_id = 0;
static uint64_t g_ping_start = 0;
static unsigned g_ping_seq = 0;

/* Recurring group bulletins: re-broadcast the same text every 5 minutes
 * for a chosen period (APRS is transient and most clients keep no history,
 * so periodic re-sends let late joiners catch important news). In-memory:
 * a restart clears the schedule (the pinned copy on receivers persists). */
#define RECUR_MAX 8
#define RECUR_INTERVAL 300            /* 5 minutes between re-sends */
typedef struct {
  int active;
  char group[8];
  char text[80];
  uint64_t end;                       /* stop re-sending at this epoch */
  uint64_t last;                      /* epoch of the last send */
} recur_t;
static recur_t g_recur[RECUR_MAX];

static void notify(const char *level, const char *body) {
  char m[256] = "{\"type\":\"notify\",\"level\":\"";
  s_cat(m, level, sizeof(m));
  s_cat(m, "\",\"title\":\"Chat\",\"body\":\"", sizeof(m));
  jesc(m, sizeof(m), body);
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

static void status(const char *text) {
  char m[400] = "{\"type\":\"ui.log.append\",\"field\":\"status\",\"line\":\"";
  jesc(m, sizeof(m), text);
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* Persistent transport indicators on the map (replaces flickering toasts):
 * Reticulum up? APRS-IS connected? BLE active? Pushed only when a value
 * changes, so a flapping link never spams. -1 = nothing pushed yet. */
static int g_ind_net = -1, g_ind_ble = -1, g_ind_adapter = -1, g_ind_ret = -1;
static void push_status(void) {
  int net = (g_sock >= 0 && g_logged) ? 1 : 0;
  int ret = rns_up() ? 1 : 0;
  /* The physical Bluetooth adapter state (the user can turn Bluetooth off at the
   * OS level at any time). BLE is "on" only when our setting is enabled AND the
   * adapter is actually powered. */
  int adapter = hal_ble_available() ? 1 : 0;
  int ble = (g_ble_on && adapter) ? 1 : 0;
  if (net == g_ind_net && ble == g_ind_ble && adapter == g_ind_adapter &&
      ret == g_ind_ret) return;
  g_ind_net = net; g_ind_ble = ble; g_ind_adapter = adapter; g_ind_ret = ret;
  char m[256];
  /* Reticulum first — it is the primary transport. */
  s_cpy(m, "{\"type\":\"ui.map.status\",\"items\":["
           "{\"id\":\"ret\",\"label\":\"RET\",\"on\":", sizeof(m));
  s_cat(m, ret ? "true" : "false", sizeof(m));
  s_cat(m, "},{\"id\":\"aprsis\",\"label\":\"NET\",\"on\":", sizeof(m));
  s_cat(m, net ? "true" : "false", sizeof(m));
  s_cat(m, "}", sizeof(m));
  /* Only advertise the BLE channel when Bluetooth is actually on. With the
   * adapter off the channel doesn't exist, so hide the chip entirely rather
   * than showing it (which wrongly implied BLE was available). */
  if (adapter) {
    s_cat(m, ",{\"id\":\"ble\",\"label\":\"BLE\",\"on\":", sizeof(m));
    s_cat(m, ble ? "true" : "false", sizeof(m));
    s_cat(m, "}", sizeof(m));
  }
  s_cat(m, "]}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* Minutes east of UTC, read once and reused.
 *
 * A message carries the SENDER's UTC epoch (see the stamp note below), which is
 * the only value two phones in different zones can agree on. Rendering that
 * epoch directly prints UTC, so every bubble on a CEST phone read two hours
 * early — 15:56 was sent and "13:56" was drawn. Cached because a thread redraw
 * formats every row and this must never become a per-row host call
 * (docs/performance.md section 4.2). */
static int  g_tz_min = 0;
static int  g_tz_known = 0;
static int tz_offset_min(void) {
  if (!g_tz_known) { g_tz_min = hal_time_utc_offset(); g_tz_known = 1; }
  return g_tz_min;
}

static void fmt_time_at(char *b, uint64_t e) {
  /* Local wall clock. The offset can be negative, so work in signed seconds
   * and wrap into [0,86400) — west of UTC would otherwise underflow the day. */
  long long secs = (long long)e + (long long)tz_offset_min() * 60;
  long long day = secs % 86400;
  if (day < 0) day += 86400;
  int hh = (int)(day / 3600), mm = (int)((day / 60) % 60);
  b[0] = (char)('0' + hh / 10); b[1] = (char)('0' + hh % 10); b[2] = ':';
  b[3] = (char)('0' + mm / 10); b[4] = (char)('0' + mm % 10); b[5] = 0;
}
static void fmt_time(char *b) { fmt_time_at(b, hal_time_epoch()); }

/* When set, the NEXT convo_msg stamps this epoch instead of "now".
 *
 * A message's time is the SENDER's, not ours: an LXMF message can sit in a
 * propagation mailbox for hours, and stamping arrival made the two devices
 * show different minutes for the same message. That matters beyond looks —
 * a like names its target by content AND minute, so a disagreement there is a
 * like that lands on nothing. The envelope carries the sender's timestamp;
 * this is how it reaches the bubble. */
static uint64_t g_msg_epoch = 0;

/* kind: "pos" (position beacon) or "msg" (text message). The host uses
 * it to detect repeats — positions repeat per callsign (telemetry varies),
 * messages repeat by exact text. */
/* convo: conversation id for the Messenger (callsign for 1:1, "#GROUP" for a
 * bulletin room). Pass "" for the geo-chat feed (it isn't grouped). */
/* fwd decl: append "lat":..,"lon":.. when known */
static void cat_pos(char *m, unsigned sz, double lat, double lon);
/* via: transport the message arrived on ("BLE"/"NET"); "" for our own sends.
 * The host renders it as a small origin chip so users can tell where a
 * received message came from. */
static void chat_append(const char *field, const char *convo, const char *dir,
                        const char *from, const char *text, const char *kind,
                        int recur, const char *meta, double lat, double lon,
                        const char *via) {
  char t[8]; fmt_time(t);
  char m[500] = "{\"type\":\"ui.chat.append\",\"field\":\"";
  s_cat(m, field, sizeof(m));
  s_cat(m, "\",\"message\":{\"dir\":\"", sizeof(m));
  s_cat(m, dir, sizeof(m));
  s_cat(m, "\",\"convo\":\"", sizeof(m)); jesc(m, sizeof(m), convo);
  s_cat(m, "\",\"from\":\"", sizeof(m)); jesc(m, sizeof(m), from);
  s_cat(m, "\",\"text\":\"", sizeof(m)); jesc(m, sizeof(m), text);
  s_cat(m, "\",\"kind\":\"", sizeof(m)); s_cat(m, kind, sizeof(m));
  s_cat(m, "\",\"meta\":\"", sizeof(m)); jesc(m, sizeof(m), meta);
  s_cat(m, "\"", sizeof(m));
  if (via && via[0]) {
    s_cat(m, ",\"via\":\"", sizeof(m)); jesc(m, sizeof(m), via_label(via));
    s_cat(m, "\"", sizeof(m));
  }
  if (recur) s_cat(m, ",\"recur\":true,\"time\":\"", sizeof(m));
  else s_cat(m, ",\"time\":\"", sizeof(m));
  s_cat(m, t, sizeof(m));
  s_cat(m, "\"", sizeof(m)); cat_pos(m, sizeof(m), lat, lon);
  s_cat(m, "}}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* detail = the station's latest comment/message ("" if none). The host shows
 * it, plus lat/lon and a relative "last heard" time, in the marker popup —
 * so we send the heard epoch (seconds) and let the host format it. */
static void u_itoa(unsigned v, char *out);   /* defined with the messenger code */
static void push_marker(const char *id, double lat, double lon,
                        const char *color, const char *detail) {
  char m[360] = "{\"type\":\"ui.map.marker\",\"id\":\"";
  jesc(m, sizeof(m), id);
  s_cat(m, "\",\"label\":\"", sizeof(m)); jesc(m, sizeof(m), id);
  s_cat(m, "\",\"lat\":", sizeof(m)); append_dbl(m, sizeof(m), lat);
  s_cat(m, ",\"lon\":", sizeof(m)); append_dbl(m, sizeof(m), lon);
  s_cat(m, ",\"heard\":", sizeof(m));
  { char hb[12]; u_itoa((unsigned)hal_time_epoch(), hb); s_cat(m, hb, sizeof(m)); }
  if (detail && detail[0]) {
    s_cat(m, ",\"detail\":\"", sizeof(m)); jesc(m, sizeof(m), detail);
    s_cat(m, "\"", sizeof(m));
  }
  if (color && color[0]) {
    s_cat(m, ",\"color\":\"", sizeof(m)); s_cat(m, color, sizeof(m));
    s_cat(m, "\"", sizeof(m));
  }
  s_cat(m, "}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

static void center_map(void) {
  char m[160] = "{\"type\":\"ui.map.viewport\",\"lat\":";
  append_dbl(m, sizeof(m), g_lat);
  s_cat(m, ",\"lon\":", sizeof(m)); append_dbl(m, sizeof(m), g_lon);
  s_cat(m, ",\"zoom\":9}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* Tell the host the coverage circle: my station + the filter radius. */
static void push_radius(void) {
  char m[160] = "{\"type\":\"ui.map.radius\",\"lat\":";
  append_dbl(m, sizeof(m), g_lat);
  s_cat(m, ",\"lon\":", sizeof(m)); append_dbl(m, sizeof(m), g_lon);
  s_cat(m, ",\"km\":", sizeof(m));
  char nb[12]; int v = g_radius, j = 0, k = 0; char t[12];
  if (v == 0) t[j++] = '0'; while (v > 0) { t[j++] = (char)('0' + v % 10); v /= 10; }
  while (j > 0) nb[k++] = t[--j]; nb[k] = 0;
  s_cat(m, nb, sizeof(m)); s_cat(m, "}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* Drop the old area's pins + geo-chat when the radius changes. */
static void clear_area(void) {
  const char *a = "{\"type\":\"ui.map.markers.clear\"}";
  hal_msg_send(a, s_len(a));
  const char *b = "{\"type\":\"ui.chat.clear\",\"field\":\"geochat\"}";
  hal_msg_send(b, s_len(b));
}

/* Ask the host to replay archived Live geo-chat for the current area into the
 * Live tab. The host persists every geo-tagged Live message and answers this
 * by centre+radius, so opening the wapp (or changing the radius) brings back
 * the older messages that happened in the selected region. */
static void request_history(void) {
  char m[200] = "{\"type\":\"ui.chat.history\",\"field\":\"geochat\",\"lat\":";
  append_dbl(m, sizeof(m), g_lat);
  s_cat(m, ",\"lon\":", sizeof(m)); append_dbl(m, sizeof(m), g_lon);
  s_cat(m, ",\"radius_km\":", sizeof(m));
  { char nb[12]; u_itoa((unsigned)g_radius, nb); s_cat(m, nb, sizeof(m)); }
  s_cat(m, ",\"limit\":200}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* read shared config from a command's bundled fields */
static void read_config(const char *buf) {
  char v[64];
  /* NOTE: no "callsign" override here any more. The working callsign is owned
   * by the profile identity (module_init) and the APRS panel (aprs_apply):
   * a free-text override let stale saved fields silently re-callsign a
   * background engine, and let unverified calls onto APRS-IS. */
  if (jstr(buf, "my_lat", v, sizeof(v))) g_lat = to_dbl(v);
  if (jstr(buf, "my_lon", v, sizeof(v))) g_lon = to_dbl(v);
  if (jstr(buf, "radius_km", v, sizeof(v)) && v[0]) g_radius = to_int(v);
  if (jstr(buf, "symbol", v, sizeof(v)) && s_len(v) >= 2) s_cpy(g_symbol, v, sizeof(g_symbol));
  if (jstr(buf, "path", v, sizeof(v))) s_cpy(g_path, v, sizeof(g_path));
  if (jstr(buf, "beacon_interval", v, sizeof(v)) && v[0]) g_interval = to_int(v);
  if (jstr(buf, "mail_days", v, sizeof(v)) && v[0]) { g_mail_days = to_int(v); if (g_mail_days < 1) g_mail_days = 1; }
  /* NOTE: BLE on/off is intentionally NOT read here. read_config runs on
   * every command (connect, sends, …) and the host serialises an unset
   * checkbox as false, which would clobber the on-by-default state before the
   * user ever touches Settings. BLE state is owned by init (default on) and
   * the explicit "Apply Bluetooth" action instead — see the ble_apply cmd. */
}

static void do_connect(const char *buf) {
  read_config(buf);
  request_history();   /* bring back this area's older Live messages on open */
  /* APRS-IS is opt-in: without a verified licensed callsign there is no
   * connection at all (Bluetooth + Reticulum keep working). */
  if (!g_aprsis_on) {
    status("APRS-IS is off - enable it in the APRS panel (licensed callsign required)");
    log_line("aprs_status", "APRS-IS is off. Enable the switch above with your "
                            "licensed callsign and passcode.");
    return;
  }
  s_cpy(g_host, APRS_DEFAULT_HOST, sizeof(g_host));
  g_port = APRS_DEFAULT_PORT;
  char v[64];
  if (jstr(buf, "server", v, sizeof(v)) && v[0]) s_cpy(g_host, v, sizeof(g_host));
  if (jstr(buf, "port", v, sizeof(v)) && v[0]) g_port = to_int(v);
  g_want_connect = 1;                 /* keep it connected (auto-reconnect) */
  if (g_sock >= 0) { aprs_disconnect(g_sock); g_sock = -1; }
  g_logged = 0;
  g_last_reconnect = hal_time_epoch();
  g_sock = aprs_connect(g_host, g_port);
  if (g_sock < 0) { status("connect: socket error (will retry)"); return; }
  char line[128] = "Connecting to "; s_cat(line, g_host, sizeof(line));
  status(line);
  log_line("aprs_status", line);
}

/* Persistence + validation for the APRS panel — declared with the other KV
 * savers (see aprsis_save/aprsis_load near igate_save). */
static void aprsis_save(void);
static void aprsis_load(void);

/* A plausible authority-assigned callsign: 3-7 alphanumeric chars containing
 * at least one digit and one letter, optionally "-<1..2 digit SSID>". This is
 * a sanity filter, not a licence check — the passcode match is the gate the
 * APRS-IS servers themselves enforce. */
static int aprs_call_valid(const char *c) {
  int base = 0, digits = 0, letters = 0;
  int i = 0;
  for (; c[i] && c[i] != '-'; i++) {
    char u = up(c[i]);
    if (u >= '0' && u <= '9') digits++;
    else if (u >= 'A' && u <= 'Z') letters++;
    else return 0;
    base++;
  }
  if (base < 3 || base > 7 || !digits || !letters) return 0;
  if (c[i] == '-') {                       /* optional SSID */
    int sn = 0;
    for (i++; c[i]; i++) { if (c[i] < '0' || c[i] > '9') return 0; sn++; }
    if (sn < 1 || sn > 2) return 0;
  }
  return 1;
}

/* APRS panel "Verify & apply": validate the licensed callsign + passcode and
 * flip the APRS-IS switch. Everything is checked BEFORE anything is enabled —
 * a wrong passcode (or an auto-generated X1/X3 call) leaves APRS-IS off. */
static void do_aprs_apply(const char *buf) {
  read_config(buf);
  int want = jbool(buf, "aprsis_enabled");
  char call[16] = "", pass[16] = "";
  jstr(buf, "aprsis_call", call, sizeof(call));
  jstr(buf, "aprsis_pass", pass, sizeof(pass));
  for (int i = 0; call[i]; i++) call[i] = up(call[i]);

  if (!want) {                       /* switch off: sever APRS-IS entirely */
    g_aprsis_on = 0;
    if (call[0]) s_cpy(g_aprsis_call, call, sizeof(g_aprsis_call));
    aprsis_save();
    g_want_connect = 0;
    if (g_sock >= 0) { aprs_disconnect(g_sock); g_sock = -1; g_logged = 0; }
    if (g_idcall[0]) s_cpy(g_call, g_idcall, sizeof(g_call));  /* back to X1 identity */
    status("APRS-IS disabled");
    log_line("aprs_status", "APRS-IS disabled. No traffic is sent or received "
                            "on APRS-IS; Bluetooth/Reticulum stay active.");
    notify("info", "APRS-IS disabled");
    return;
  }

  if (!call[0]) {
    notify("error", "Enter your licensed callsign first");
    return;
  }
  if (is_autogen_call(call)) {
    notify("error", "X1/X3 callsigns are auto-generated by XPRS and not "
                    "licensed - APRS-IS needs a callsign assigned by your "
                    "radio authority");
    log_line("aprs_status", "Rejected: auto-generated callsigns are not valid "
                            "on APRS-IS.");
    return;
  }
  if (!aprs_call_valid(call)) {
    notify("error", "That does not look like a licensed callsign "
                    "(e.g. N0CALL or N0CALL-9)");
    return;
  }
  int digitsOnly = pass[0] != 0;
  for (int i = 0; pass[i]; i++)
    if (pass[i] < '0' || pass[i] > '9') { digitsOnly = 0; break; }
  int entered = digitsOnly ? to_int(pass) : -1;
  if (entered < 0) {
    notify("error", "Enter the numeric APRS-IS passcode for your callsign");
    return;
  }
  if (entered != aprs_passcode(call)) {
    notify("error", "Wrong APRS-IS passcode for that callsign - APRS-IS stays "
                    "disabled");
    log_line("aprs_status", "Passcode check FAILED - APRS-IS stays disabled.");
    return;
  }

  /* Verified: persist + switch the station to the licensed callsign. */
  g_aprsis_on = 1;
  s_cpy(g_aprsis_call, call, sizeof(g_aprsis_call));
  g_aprsis_pass = entered;
  aprsis_save();
  s_cpy(g_call, g_aprsis_call, sizeof(g_call));   /* licensed call everywhere */
  {
    char l[96] = "Passcode verified. Station callsign is now ";
    s_cat(l, g_aprsis_call, sizeof(l));
    log_line("aprs_status", l);
  }
  notify("success", "APRS-IS enabled - connecting");
  do_connect(buf);                    /* (re)connect under the licensed call */
}

static void do_beacon(const char *buf, int emergency) {
  read_config(buf);
  int net = (g_sock >= 0 && g_logged);
  if (!net && !g_ble_on && !rns_up()) {
    notify("warning", "Enable Reticulum or Bluetooth first");
    return;
  }
  char typ[16] = "position", comment[120] = "";
  jstr(buf, "beacon_type", typ, sizeof(typ));
  jstr(buf, "beacon_comment", comment, sizeof(comment));
  if (emergency || s_eq(typ, "emergency")) {
    char c[140] = "EMERGENCY "; s_cat(c, comment, sizeof(c));
    pos_broadcast(g_lat, g_lon, c);
    if (net) aprs_send_beacon(g_sock, g_call, g_lat, g_lon, "\\!", "TCPIP*", c);
    push_marker(g_call, g_lat, g_lon, "red", c);
    status("TX emergency beacon");
    notify("warning", "Emergency beacon sent");
  } else {
    pos_broadcast(g_lat, g_lon, comment);
    if (net) aprs_send_beacon(g_sock, g_call, g_lat, g_lon, g_symbol, "TCPIP*", comment);
    push_marker(g_call, g_lat, g_lon, "blue", comment);
    status("TX position beacon");
  }
  g_last_beacon = hal_time_epoch();
}

/* ── Messenger conversations ────────────────────────────────────────────
 * The host ConversationsField is app-agnostic: this wapp owns every bit of
 * semantics — what a conversation is (a callsign for 1:1, "#GROUP" for a
 * bulletin room), its title/icon/distance badge, dedup of repeated bulletins,
 * pinning, and the recurring schedule. We drive the host via ui.convo.* and
 * ui.prompt; the host only renders and reports taps back. */

static void u_itoa(unsigned v, char *out) {
  char t[12]; int j = 0;
  if (v == 0) t[j++] = '0';
  while (v > 0) { t[j++] = (char)('0' + v % 10); v /= 10; }
  int k = 0; while (j > 0) out[k++] = t[--j]; out[k] = 0;
}

/* base64url -> bytes (tolerates standard alphabet + padding). Used to decode the
 * payload of an inbound Reticulum datagram (hal_rns_recv returns it base64url). */
static int b64v(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '-' || c == '+') return 62;
  if (c == '_' || c == '/') return 63;
  return -1;
}
static int b64url_decode(const char *in, unsigned char *out, unsigned maxout) {
  unsigned acc = 0, bits = 0, o = 0;
  for (const char *p = in; *p; p++) {
    if (*p == '=' || *p == '\n' || *p == '\r') continue;
    int v = b64v(*p);
    if (v < 0) return -1;
    acc = (acc << 6) | (unsigned)v;
    bits += 6;
    if (bits >= 8) { bits -= 8; if (o >= maxout) return -1; out[o++] = (unsigned char)((acc >> bits) & 0xff); }
  }
  return (int)o;
}

/* FNV-1a over convo|from|text — a stable content signature (and the pin key
 * shared between a message and its later repeat so the host can promote it). */
static unsigned sig_hash(const char *a, const char *b, const char *c) {
  unsigned h = 2166136261u;
  for (const char *p = a; *p; p++) { h ^= (unsigned char)*p; h *= 16777619u; }
  h ^= 1u; h *= 16777619u;
  for (const char *p = b; *p; p++) { h ^= (unsigned char)*p; h *= 16777619u; }
  h ^= 1u; h *= 16777619u;
  for (const char *p = c; *p; p++) { h ^= (unsigned char)*p; h *= 16777619u; }
  return h;
}

/* Conversation-message dedup. A plain count ring used to evict a hash after 128
 * other messages, so a station that re-broadcasts the SAME bulletin on a
 * schedule (e.g. KA2DDO's "test of multi-line bulletins." every 30 min) kept
 * reappearing once enough other traffic cycled the ring. Time-windowed with
 * refresh-on-hit instead: an identical message is suppressed for CSEEN_WINDOW
 * after it was LAST seen, so a recurring bulletin that keeps arriving inside the
 * window is shown exactly once — only a genuinely new copy (text changed, or a
 * quiet gap longer than the window) gets through. */
#define CSEEN_MAX 256
#define CSEEN_WINDOW 5400   /* 90 min; > any sane re-broadcast interval */
static struct { unsigned h; uint64_t t; } g_seen[CSEEN_MAX];
static int seen_has(unsigned h) {
  uint64_t now = hal_time_epoch();
  for (unsigned i = 0; i < CSEEN_MAX; i++)
    if (g_seen[i].t && g_seen[i].h == h && now - g_seen[i].t < CSEEN_WINDOW) {
      g_seen[i].t = now;   /* refresh: keep suppressing while it keeps arriving */
      return 1;
    }
  return 0;
}
static void seen_add(unsigned h) {
  uint64_t now = hal_time_epoch();
  unsigned oldest = 0;
  for (unsigned i = 0; i < CSEEN_MAX; i++) {
    if (!g_seen[i].t || now - g_seen[i].t >= CSEEN_WINDOW) {   /* free/expired slot */
      g_seen[i].h = h; g_seen[i].t = now; return;
    }
    if (g_seen[i].t < g_seen[oldest].t) oldest = i;
  }
  g_seen[oldest].h = h; g_seen[oldest].t = now;   /* all fresh: drop oldest */
}

/* Persistent dedup ring for relay-backed messages. The per-message id (rmid,
 * embedded in the encrypted plaintext) is remembered ACROSS restarts so a relay
 * copy fetched after the directly-delivered copy — possibly in a later session,
 * after the in-memory g_seen ring was lost — doesn't show twice. Stored as a
 * space-joined ring in KV "midseen". */
#define MIDSEEN_MAX 128
static char g_midseen[MIDSEEN_MAX][12];
static int g_midseen_n = 0;     /* entries in use (<= MIDSEEN_MAX) */
static int g_midseen_head = 0;  /* ring write cursor once full */
static int midseen_has(const char *m) {
  if (!m[0]) return 0;
  for (int i = 0; i < g_midseen_n; i++) if (s_eq(g_midseen[i], m)) return 1;
  return 0;
}
static void midseen_save(void) {
  char b[MIDSEEN_MAX * 12]; b[0] = 0;
  for (int i = 0; i < g_midseen_n; i++) { s_cat(b, g_midseen[i], sizeof(b)); s_cat(b, " ", sizeof(b)); }
  hal_kv_set("midseen", 7, b, s_len(b));
}
static void midseen_add(const char *m) {
  if (!m[0] || midseen_has(m)) return;
  if (g_midseen_n < MIDSEEN_MAX) s_cpy(g_midseen[g_midseen_n++], m, 12);
  else { s_cpy(g_midseen[g_midseen_head], m, 12); g_midseen_head = (g_midseen_head + 1) % MIDSEEN_MAX; }
  midseen_save();
}
static void midseen_load(void) {
  char b[MIDSEEN_MAX * 12];
  uint32_t n = hal_kv_get("midseen", 7, b, sizeof(b) - 1);
  if (n == 0) return;
  b[n] = 0; char m[12]; int k = 0;
  for (unsigned i = 0; i <= n; i++) {
    char c = (i < n) ? b[i] : ' ';
    if (c == ' ') { if (k > 0 && g_midseen_n < MIDSEEN_MAX) { m[k] = 0; s_cpy(g_midseen[g_midseen_n++], m, 12); } k = 0; }
    else if (k < 11) m[k++] = c;
  }
  g_midseen_head = g_midseen_n % MIDSEEN_MAX;
}

/* Separate raw-frame dedup (cross-transport + relay loop guard), kept apart
 * from the conversation seen-ring above so it can't evict pin-detection keys.
 * Time-windowed: a frame is suppressed for FSEEN_WINDOW after it is first seen,
 * so a message re-broadcast many times (BLE adverts repeat for their TTL, and
 * the mesh relays them) is shown only once. A plain count ring evicted recent
 * hashes once enough other frames arrived, letting duplicates reappear. */
#define FSEEN_MAX 256
#define FSEEN_WINDOW 3600   /* 60 minutes */
static struct { unsigned h; uint64_t t; } g_fseen[FSEEN_MAX];
static int fseen_has(unsigned h) {
  uint64_t now = hal_time_epoch();
  for (unsigned i = 0; i < FSEEN_MAX; i++)
    if (g_fseen[i].t && g_fseen[i].h == h && now - g_fseen[i].t < FSEEN_WINDOW)
      return 1;
  return 0;
}
static void fseen_add(unsigned h) {
  uint64_t now = hal_time_epoch();
  unsigned oldest = 0;
  for (unsigned i = 0; i < FSEEN_MAX; i++) {
    /* reuse a free or expired slot so an hour of distinct frames can't evict
     * still-valid entries */
    if (!g_fseen[i].t || now - g_fseen[i].t >= FSEEN_WINDOW) {
      g_fseen[i].h = h; g_fseen[i].t = now; return;
    }
    if (g_fseen[i].t < g_fseen[oldest].t) oldest = i;
  }
  g_fseen[oldest].h = h; g_fseen[oldest].t = now;  /* all fresh: drop oldest */
}

/* Content dedup for the Live/Beacons geo-chat. The same message reaches us as
 * different raw frames — over BLE and over APRS-IS — and APRS-IS itself can
 * deliver duplicates via multiple IGates, so the per-frame fseen ring above
 * can't catch them. Dedup on sender+text (transport-independent) so a message
 * shows once per 60 min. Returns 1 if it's a duplicate to drop. */
static int geo_dup(const char *from, const char *text) {
  unsigned h = sig_hash("g", from, text);
  if (fseen_has(h)) return 1;
  fseen_add(h);
  return 0;
}

/* Popup notification for an incoming chat message. Only for: Live-tab geo-chat
 * messages, direct messages addressed to us, and group/bulletin messages for a
 * group we are subscribed to (i.e. in our conversation list). NOT beacons. The
 * host shows it as a system notification when we're in the background (so the
 * user is alerted with the screen off / app closed) and an in-app card when the
 * page is open. Content-deduped (own ring) so the same message arriving via both
 * APRS-IS and BLE — or a repeated bulletin — pops only once. */
/* Alert the user about a message they cannot see, because the app is in the
 * background or on another screen.
 *
 * This was a no-op — disabled wholesale on the reasoning that "Mail raises the
 * notification for a direct message", which is true only for the NOSTR kind-4
 * inbox Mail owns. Everything Chat itself renders — LXMF peer DMs, rooms,
 * groups, Activity — notified NOBODY. A phone in someone's pocket received
 * messages in complete silence, which is the same as not receiving them.
 *
 * The boundary is exactly the one convo_msg draws: notify for what Chat shows,
 * stay quiet for the plain 1:1 that belongs to Mail (which notifies it), so a
 * single message never produces two notifications. */
static uint32_t g_notif_seen[16];
static uint64_t g_notif_time[16];
static int g_notif_w = 0;

/* Suppress only a REAL duplicate: the same message arriving again over another
 * transport, which happens within seconds. A plain content ring with no clock
 * silenced a person genuinely saying the same thing twice — the second "ok" of
 * the day never notified. 60s covers every multi-transport race we have. */
#define NOTIF_DUP_WINDOW_SEC 60
static int notif_dup(const char *from, const char *text) {
  uint32_t h = 5381;
  for (int i = 0; from && from[i]; i++) h = h * 33u + (unsigned char)from[i];
  for (int i = 0; text && text[i]; i++) h = h * 33u + (unsigned char)text[i];
  if (!h) h = 1;
  uint64_t now = hal_time_epoch();
  for (int i = 0; i < 16; i++) {
    if (g_notif_seen[i] == h && now - g_notif_time[i] < NOTIF_DUP_WINDOW_SEC) {
      return 1;
    }
  }
  g_notif_seen[g_notif_w] = h;
  g_notif_time[g_notif_w] = now;
  g_notif_w = (g_notif_w + 1) % 16;
  return 0;
}

static int is_group(const char *id);       /* '#' prefix; defined below */
static void notify_msg(const char *title, const char *from, const char *text,
                       const char *body) {
  if (!title || !title[0]) return;
  /* Only what this wapp can actually open and show. A 1:1 keyed by callsign is
   * Mail's; notifying it here would double up and then land the user in a wapp
   * with no thread to show them. */
  if (!(is_group(title) || room_is_room(title) || s_pre(title, "lxmf:") ||
        s_eq(title, "Activity"))) {
    return;
  }
  if (notif_dup(from, text)) return;
  /* Show WHO wrote, not the address they wrote from: an LXMF thread id is
   * "lxmf:" plus 32 hex characters, which told the user nothing except that
   * something happened. Groups and rooms keep their own id — "#NEWS" reads
   * correctly on its own. */
  const char *shown = title;
  if (s_pre(title, "lxmf:") && from && from[0]) shown = from;
  char m[560] = "{\"type\":\"notify\",\"level\":\"info\",\"title\":\"";
  jesc(m, sizeof(m), shown);
  s_cat(m, "\",\"body\":\"", sizeof(m));
  jesc(m, sizeof(m), body);
  /* The conversation this is about — the host routes a notification TAP
   * through xprs://open?wapp=chat&convo=<id> straight into this thread.
   * Without it a tap opened the app on whatever screen it was left on, which
   * reads as "notifications don't work". */
  s_cat(m, "\",\"convo\":\"", sizeof(m));
  jesc(m, sizeof(m), title);
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* BLE mesh repeater: rebroadcast each received frame once, suppressing any
 * content already repeated within the last 10 minutes (loop/storm control). */
#define RPT_MAX 64
static struct { unsigned h; uint64_t t; } g_rpt[RPT_MAX];
static unsigned g_rpt_cnt = 0;
static int rpt_recent(unsigned h, uint64_t now) {
  unsigned n = g_rpt_cnt < RPT_MAX ? g_rpt_cnt : RPT_MAX;
  for (unsigned i = 0; i < n; i++)
    if (g_rpt[i].h == h && now - g_rpt[i].t < 600) return 1;
  return 0;
}
static void rpt_mark(unsigned h, uint64_t now) {
  g_rpt[g_rpt_cnt % RPT_MAX].h = h; g_rpt[g_rpt_cnt % RPT_MAX].t = now; g_rpt_cnt++;
}

/* Last-known station positions, for the 1:1 distance badge. */
typedef struct { char call[16]; double lat, lon; int used; } pos_t;
static pos_t g_pos[64];
static unsigned g_pos_evict = 0;   /* rotating eviction cursor when the table is full */
static void pos_set(const char *call, double lat, double lon) {
  int free_i = -1;
  for (int i = 0; i < 64; i++) {
    if (g_pos[i].used) {
      if (s_eq(g_pos[i].call, call)) { g_pos[i].lat = lat; g_pos[i].lon = lon; return; }
    } else if (free_i < 0) free_i = i;
  }
  if (free_i < 0) free_i = (int)(g_pos_evict++ % 64);
  s_cpy(g_pos[free_i].call, call, sizeof(g_pos[free_i].call));
  g_pos[free_i].lat = lat; g_pos[free_i].lon = lon; g_pos[free_i].used = 1;
}
static int pos_get(const char *call, double *lat, double *lon) {
  for (int i = 0; i < 64; i++)
    if (g_pos[i].used && s_eq(g_pos[i].call, call)) {
      *lat = g_pos[i].lat; *lon = g_pos[i].lon; return 1;
    }
  return 0;
}
/* cos via Taylor (lat in radians, |x| < pi/2 — well within range) */
static double m_cos(double x) {
  double x2 = x * x;
  return 1.0 - x2 / 2.0 + x2 * x2 / 24.0 - x2 * x2 * x2 / 720.0
         + x2 * x2 * x2 * x2 / 40320.0;
}
/* Equirectangular distance; writes "<n> km"/"<n> m" badge, 1 if known. */
/* Distance from our position (the map pinpoint) to lat/lon, as "<n> km"/"m". */
static int distance_to(double lat, double lon, char *out, unsigned osz) {
  out[0] = 0;
  if (g_lat == 0 && g_lon == 0) return 0;
  const double D2R = 0.0174532925199433;
  double x = (lon - g_lon) * D2R * m_cos((g_lat + lat) * 0.5 * D2R);
  double y = (lat - g_lat) * D2R;
  double km = 6371.0 * __builtin_sqrt(x * x + y * y);
  if (km < 1.0) { u_itoa((unsigned)(km * 1000.0 + 0.5), out); s_cat(out, " m", osz); }
  else { u_itoa((unsigned)(km + 0.5), out); s_cat(out, " km", osz); }
  return 1;
}
/* Distance to a callsign's last-known position (1 if known). */
static int distance_badge(const char *call, char *out, unsigned osz) {
  out[0] = 0;
  double lat, lon;
  if (!pos_get(call, &lat, &lon)) return 0;
  return distance_to(lat, lon, out, osz);
}
/* km from us to a callsign's last-known position, or -1 if unknown. */
static double km_to_call(const char *call) {
  double lat, lon;
  if (!pos_get(call, &lat, &lon)) return -1.0;
  if (g_lat == 0 && g_lon == 0) return -1.0;
  const double D2R = 0.0174532925199433;
  double x = (lon - g_lon) * D2R * m_cos((g_lat + lat) * 0.5 * D2R);
  double y = (lat - g_lat) * D2R;
  return 6371.0 * __builtin_sqrt(x * x + y * y);
}
/* True only when we positively know the sender sits inside our coverage radius
 * (so a local group bulletin can be filed as "local"); unknown position = no. */
static int within_radius(const char *call) {
  double km = km_to_call(call);
  return km >= 0 && km <= (double)g_radius;
}

/* Conversations the host knows about, so we can refresh the distance badge
 * when a contact's position arrives. */
static char g_convo_ids[32][40];
static int g_convo_n = 0;
static void convo_remember(const char *id) {
  for (int i = 0; i < g_convo_n; i++) if (s_eq(g_convo_ids[i], id)) return;
  if (g_convo_n < 32) s_cpy(g_convo_ids[g_convo_n++], id, 40);
}
/* Drop [id] from the subscribed set (so we stop listening to that group/DM). */
static void convo_forget(const char *id) {
  for (int i = 0; i < g_convo_n; i++) {
    if (s_eq(g_convo_ids[i], id)) {
      for (int j = i; j < g_convo_n - 1; j++) s_cpy(g_convo_ids[j], g_convo_ids[j + 1], 40);
      g_convo_n--;
      return;
    }
  }
}
static void groups_subscribe(void);   /* group set changed -> refresh the NOSTR filter */
static void group_tag(const char *gname, char *out, unsigned cap);
static void group_convo_id(const char *gname, char *out, unsigned cap);

static int convo_known(const char *id) {
  for (int i = 0; i < g_convo_n; i++) if (s_eq(g_convo_ids[i], id)) return 1;
  return 0;
}
static void groups_save(void);   /* fwd: persist subscribed groups to KV */

/* ── generic ui.convo.* senders ── */
/* append "lat":..,"lon":.. to m when the position is known (not 0,0). */
static void cat_pos(char *m, unsigned sz, double lat, double lon) {
  if (lat == 0 && lon == 0) return;
  s_cat(m, ",\"lat\":", sz); append_dbl(m, sz, lat);
  s_cat(m, ",\"lon\":", sz); append_dbl(m, sz, lon);
}
/* Append optional thread fields ("mid" = this message's 4-hex id, "parent" =
 * the id it replies to). Empty values are omitted so non-threaded chats and the
 * host's generic store are unaffected. */
static void cat_thread(char *m, unsigned sz, const char *mid, const char *parent,
                       const char *auth, int enc) {
  if (mid && mid[0]) { s_cat(m, ",\"mid\":\"", sz); s_cat(m, mid, sz); s_cat(m, "\"", sz); }
  if (parent && parent[0]) { s_cat(m, ",\"parent\":\"", sz); s_cat(m, parent, sz); s_cat(m, "\"", sz); }
  /* Signature verdict (XPRS): verified / bad / unverified. Empty = unsigned. */
  if (auth && auth[0]) { s_cat(m, ",\"auth\":\"", sz); s_cat(m, auth, sz); s_cat(m, "\"", sz); }
  /* Encrypted (XPRS 1:1): host shows a lock badge. */
  if (enc) s_cat(m, ",\"enc\":true", sz);
}
/* Set just before the outgoing local-echo convo_deliver of a 1:1 message so the
 * emitted bubble carries its receipt id (rid = am) + initial tick state
 * ("sent"); cleared immediately after so no other bubble picks them up. */
static char g_send_rid[8] = "";
static char g_send_status[12] = "";
static int is_group(const char *id);   /* '#' prefix; defined below */

static void convo_msg(const char *id, const char *dir, const char *from,
                      const char *text, const char *key, const char *meta,
                      double lat, double lon, const char *via,
                      const char *mid, const char *parent, const char *auth, int enc,
                      int priv) {
  /* GROUPS ONLY (plus rooms, plus LXMF peers). A NOSTR 1:1 message still
   * reaches this wapp over BLE/APRS (the radios do not know the difference),
   * but rendering it here would rebuild the second inbox we just removed — the
   * Mail wapp owns kind-4 1:1. LXMF peers are the exception: NomadNet
   * users have no kind-4 inbox anywhere, so their DMs render here. */
  uint64_t stamp = g_msg_epoch; g_msg_epoch = 0;  /* one message, one stamp */
  if (!is_group(id) && !room_is_room(id) && !s_pre(id, "lxmf:")) return;
  char t[8];
  if (stamp) fmt_time_at(t, stamp); else fmt_time(t);
  char m[640] = "{\"type\":\"ui.convo.msg\",\"id\":\"";
  jesc(m, sizeof(m), id);
  s_cat(m, "\",\"dir\":\"", sizeof(m)); s_cat(m, dir, sizeof(m));
  s_cat(m, "\",\"from\":\"", sizeof(m)); jesc(m, sizeof(m), from);
  s_cat(m, "\",\"text\":\"", sizeof(m)); jesc(m, sizeof(m), text);
  s_cat(m, "\",\"key\":\"", sizeof(m)); s_cat(m, key, sizeof(m));
  s_cat(m, "\",\"meta\":\"", sizeof(m)); jesc(m, sizeof(m), meta);
  s_cat(m, "\"", sizeof(m));
  if (via && via[0]) {
    s_cat(m, ",\"via\":\"", sizeof(m)); jesc(m, sizeof(m), via_label(via));
    s_cat(m, "\"", sizeof(m));
  }
  s_cat(m, ",\"time\":\"", sizeof(m)); s_cat(m, t, sizeof(m));
  s_cat(m, "\"", sizeof(m)); cat_pos(m, sizeof(m), lat, lon);
  cat_thread(m, sizeof(m), mid, parent, auth, enc);
  /* Private = this message went Reticulum-only (never APRS) — the host tags the
   * bubble so it's clearly distinct from public APRS traffic. */
  if (priv) s_cat(m, ",\"private\":true", sizeof(m));
  /* WhatsApp-style receipt id + tick state (1:1 outgoing only; globals set by
   * do_convo_send around the local echo). */
  if (g_send_rid[0]) { s_cat(m, ",\"rid\":\"", sizeof(m)); s_cat(m, g_send_rid, sizeof(m)); s_cat(m, "\"", sizeof(m)); }
  if (g_send_status[0]) { s_cat(m, ",\"status\":\"", sizeof(m)); s_cat(m, g_send_status, sizeof(m)); s_cat(m, "\"", sizeof(m)); }
  s_cat(m, "}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
/* Ask the host to drop already-shown bubbles. Two forms (both local-only, never
 * transmitted): {id,key} removes one message from one conversation (hide); {from}
 * removes every message by a sender across all conversations (block). */
static void convo_remove_key(const char *id, const char *key) {
  char m[160] = "{\"type\":\"ui.convo.remove\",\"id\":\"";
  jesc(m, sizeof(m), id);
  s_cat(m, "\",\"key\":\"", sizeof(m)); s_cat(m, key, sizeof(m));
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
static void convo_remove_from(const char *from) {
  char m[160] = "{\"type\":\"ui.convo.remove\",\"from\":\"";
  jesc(m, sizeof(m), from);
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* ── 1:1 delivery/read receipts (WhatsApp-style ticks) ────────────────────
 * Every 1:1 message carries a small "am:<6hex>" correlation token. The receiver
 * echoes it so the sender advances the bubble sent -> delivered -> read:
 *   - delivered over APRS = the STANDARD APRS ack<seq> (APRSdroid-compatible,
 *     handled in route_frame); over BLE/RNS = a "?ACK <am> d" control frame.
 *   - read = a "?ACK <am> r" control frame sent when the user opens the chat.
 * Receipt frames are consumed by rcpt_intercept (never rendered). Status is
 * pushed to the host via ui.convo.status keyed by the message's rid (= am). */

/* Advance a message's tick state on the host (keyed by its rid). */
static void convo_status_emit(const char *rid, const char *status) {
  if (!rid || !rid[0]) return;
  char m[96] = "{\"type\":\"ui.convo.status\",\"rid\":\"";
  s_cat(m, rid, sizeof(m));
  s_cat(m, "\",\"status\":\"", sizeof(m)); s_cat(m, status, sizeof(m));
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* seq -> am, so an incoming APRS ack<seq> maps back to the message we sent. */
#define ACKMAP_MAX 48
static int  g_ackseq[ACKMAP_MAX];
static char g_ackam[ACKMAP_MAX][8];
static int  g_ackmap_w = 0;
static void ackmap_add(int seq, const char *am) {
  int i = g_ackmap_w++ % ACKMAP_MAX;
  g_ackseq[i] = seq; s_cpy(g_ackam[i], am, 8);
}
static const char *ackmap_get(int seq) {
  for (int i = 0; i < ACKMAP_MAX; i++)
    if (g_ackam[i][0] && g_ackseq[i] == seq) return g_ackam[i];
  return 0;
}

/* Received-but-unread 1:1 messages awaiting a read receipt on convo open. */
#define RPEND_MAX 64
/* Wide enough for an LXMF conversation id ("lxmf:" + 32 hex = 37), not just a
 * callsign. At 16 it silently truncated one to 15 characters, so the read
 * receipt for a NomadNet/Bluetooth thread never matched the conversation the
 * user opened and the second tick never appeared. */
static char g_rpend_convo[RPEND_MAX][48];
static char g_rpend_am[RPEND_MAX][8];
static char g_rpend_via[RPEND_MAX][8];
static int  g_rpend_n = 0;
static void rpend_add(const char *convo, const char *am, const char *via) {
  if (!am[0]) return;
  for (int i = 0; i < g_rpend_n; i++) if (s_eq(g_rpend_am[i], am)) return;
  if (g_rpend_n >= RPEND_MAX) {                 /* drop oldest */
    for (int i = 1; i < RPEND_MAX; i++) {
      s_cpy(g_rpend_convo[i-1], g_rpend_convo[i], sizeof(g_rpend_convo[0]));
      s_cpy(g_rpend_am[i-1], g_rpend_am[i], 8);
      s_cpy(g_rpend_via[i-1], g_rpend_via[i], 8);
    }
    g_rpend_n = RPEND_MAX - 1;
  }
  s_cpy(g_rpend_convo[g_rpend_n], convo, sizeof(g_rpend_convo[0]));
  s_cpy(g_rpend_am[g_rpend_n], am, 8);
  s_cpy(g_rpend_via[g_rpend_n], via, 8);
  g_rpend_n++;
}

/* Raw APRS message to [to] carrying [body], WITHOUT a {seq (so the recipient
 * doesn't ack it) — same envelope as send_ack. */
static void aprs_msg_noseq(const char *to, const char *body) {
  if (!to[0] || g_sock < 0 || !g_logged) return;
  char dest[10]; int i = 0;
  for (; to[i] && i < 9; i++) dest[i] = up(to[i]);
  dest[i] = 0;
  while (s_len(dest) < 9) s_cat(dest, " ", sizeof(dest));
  char line[128];
  s_cpy(line, g_call, sizeof(line));
  s_cat(line, ">APRS,TCPIP*::", sizeof(line));
  s_cat(line, dest, sizeof(line));
  s_cat(line, ":", sizeof(line));
  s_cat(line, body, sizeof(line));
  aprs_send_raw(g_sock, line);
}

/* Send a "?ACK <am> <state>" receipt to [to] over the arrival transport [via]
 * plus the Reticulum backstop. state = 'd' delivered / 'r' read. */
static void send_receipt(const char *to, const char *am, char state, const char *via) {
  if (!to || !to[0] || to[0] == '#' || !am || !am[0]) return;
  char body[24]; s_cpy(body, "?ACK ", sizeof(body)); s_cat(body, am, sizeof(body));
  char sp[3] = {' ', state, 0}; s_cat(body, sp, sizeof(body));
  /* A NomadNet/Bluetooth peer is addressed by its delivery destination, not by
   * a callsign — the conversation id carries it as "lxmf:<dest>". Without this
   * the read receipt for those threads was aired at a callsign nobody has. */
  if (s_pre(to, "lxmf:")) {
    const char *d = to + 5;
    hal_lxmf_send(d, s_len(d), "", 0, body, s_len(body));
    return;
  }
  if (s_eq(via, "NET")) aprs_msg_noseq(to, body);
  else if (g_ble_on && s_eq(via, "BLE")) ble_tx_msg(to, body);
  rns_tx_msg(to, body);   /* backstop (covers RET/RLY arrival + reliability) */
}

/* Intercept an inbound "?ACK <am> <d|r>" receipt: advance our bubble state and
 * consume the frame (return 1) so it never renders as a chat message. */
static int rcpt_intercept(const char *from, const char *text) {
  (void)from;
  if (!(text[0]=='?'&&text[1]=='A'&&text[2]=='C'&&text[3]=='K'&&text[4]==' ')) return 0;
  const char *p = text + 5;
  char am[8]; int i = 0;
  while (*p && *p != ' ' && i < 7) am[i++] = *p++;
  am[i] = 0;
  if (*p == ' ') p++;
  if (am[0] && (*p == 'd' || *p == 'r'))
    convo_status_emit(am, *p == 'r' ? "read" : "delivered");
  return 1;
}

/* Extract + strip an "am:<6hex>" token from [s] in place. Returns 1 and writes
 * the 6-hex id to [am] when present. */
static int extract_am(char *s, char *am) {
  am[0] = 0;
  for (char *p = s; *p; p++) {
    if (!(p[0]=='a' && p[1]=='m' && p[2]==':')) continue;
    if (p > s && p[-1] != ' ') continue;         /* must start a token */
    int ok = 1;
    for (int i = 0; i < 6; i++) {
      char c = p[3+i];
      if (!((c>='0'&&c<='9') || (c>='a'&&c<='f'))) { ok = 0; break; }
    }
    char after = p[9];
    if (!ok || (after != 0 && after != ' ')) continue;
    for (int i = 0; i < 6; i++) am[i] = p[3+i];
    am[6] = 0;
    char *start = (p > s && p[-1] == ' ') ? p - 1 : p;   /* eat a leading space */
    char *end = p + 9;
    if (start == p && *end == ' ') end++;                /* front token: eat trailing space */
    unsigned n = 0; while (end[n]) { start[n] = end[n]; n++; } start[n] = 0;
    return 1;
  }
  return 0;
}

/* Extract + strip an "np:<npub>" token — the recipient identity a best-hope
 * custody copy carries. Returns 1 and writes the key when present.
 *
 * A carrier delivers on the callsign, which is short and derived from the npub;
 * this token is what makes that safe. Taking somebody's callsign is easy, and
 * without the npub a device could collect mail addressed to a name it merely
 * claims. The receiver checks the token against its OWN key and ignores the
 * message when it does not match. */
static int extract_np(char *s, char *np, unsigned np_sz) {
  np[0] = 0;
  for (char *p = s; *p; p++) {
    if (!(p[0]=='n' && p[1]=='p' && p[2]==':')) continue;
    if (p > s && p[-1] != ' ') continue;          /* must start a token */
    char *v = p + 3, *e = v;
    while (*e && *e != ' ') e++;
    unsigned n = (unsigned)(e - v);
    if (n < 8 || n >= np_sz) continue;            /* not a key */
    for (unsigned i = 0; i < n; i++) np[i] = v[i];
    np[n] = 0;
    char *start = (p > s && p[-1] == ' ') ? p - 1 : p;
    char *end = e;
    if (start == p && *end == ' ') end++;
    unsigned k = 0; while (end[k]) { start[k] = end[k]; k++; } start[k] = 0;
    return 1;
  }
  return 0;
}

/* Opening a 1:1 conversation IS reading it: send the read receipt for every
 * message of it we are still holding one for, and keep the rest.
 *
 * Lives on its own because the thread is opened by TWO different commands and
 * only one of them used to call it. The chat screen is the ROOMS field, so a
 * Bluetooth/NomadNet thread arrives as `rooms_open` — which returned early —
 * while this flush sat behind `conversations_open`, a command that screen never
 * sends. The second tick was therefore unreachable on exactly the conversations
 * that matter, no matter what else was fixed. */
static void rpend_flush_read(const char *convo) {
  if (!convo || !convo[0] || convo[0] == '#') return;  /* groups: no receipts */
  if (room_is_room(convo)) return;                     /* rooms: no 1:1 receipts */
  int w = 0;
  for (int i = 0; i < g_rpend_n; i++) {
    if (s_eq(g_rpend_convo[i], convo)) {
      send_receipt(convo, g_rpend_am[i], 'r', g_rpend_via[i]);
    } else {
      if (w != i) {
        /* sizeof, not 16: an id kept here used to be cut to 15 characters,
         * which is half of why a second open never found its entry. */
        s_cpy(g_rpend_convo[w], g_rpend_convo[i], sizeof(g_rpend_convo[0]));
        s_cpy(g_rpend_am[w], g_rpend_am[i], 8);
        s_cpy(g_rpend_via[w], g_rpend_via[i], 8);
      }
      w++;
    }
  }
  g_rpend_n = w;
}

/* User opened a 1:1 conversation → send read receipts for its pending msgs. */
static void do_convo_open(const char *buf) {
  /* 48, not 16: an LXMF conversation id is "lxmf:" + 32 hex. */
  char convo[48] = ""; jstr(buf, "conversations_convo", convo, sizeof(convo));
  rpend_flush_read(convo);
}

/* ── Local hide / block (never transmitted) ───────────────────────────────
 * Two purely-local filters the user controls per device:
 *  - blocked callsigns: we drop every message from them, on any transport, and
 *    hide their conversation. Persisted in KV "blocked" (";"-joined).
 *  - hidden messages: a single message the user dismissed, keyed by the same
 *    content signature ("key") the host shows the bubble under, so it stays gone
 *    even if the same frame arrives again on another transport. KV "hidden". */
#define BLOCK_MAX 64
#define HIDE_MAX  128
static char g_blocked[BLOCK_MAX][16];
static int  g_blocked_n = 0;
/* Muted callsigns: a lighter filter than block — we simply stop SHOWING their
 * new messages (Activity + groups + DMs); we don't discard their conversation or
 * existing bubbles. Persisted in KV "muted". */
static char g_muted[BLOCK_MAX][16];
static int  g_muted_n = 0;
static char g_hidden[HIDE_MAX][16];   /* sig_hash keys (decimal) */
static int  g_hidden_n = 0;

static int is_blocked(const char *call) {
  for (int i = 0; i < g_blocked_n; i++) if (s_eq(g_blocked[i], call)) return 1;
  return 0;
}
static int is_muted(const char *call) {
  for (int i = 0; i < g_muted_n; i++) if (s_eq(g_muted[i], call)) return 1;
  return 0;
}
static int is_hidden_key(const char *key) {
  for (int i = 0; i < g_hidden_n; i++) if (s_eq(g_hidden[i], key)) return 1;
  return 0;
}
static void blocked_save(void) {
  char buf[BLOCK_MAX * 17]; buf[0] = 0;
  for (int i = 0; i < g_blocked_n; i++) { s_cat(buf, g_blocked[i], sizeof(buf)); s_cat(buf, ";", sizeof(buf)); }
  hal_kv_set("blocked", 7, buf, s_len(buf));
}
static void muted_save(void) {
  char buf[BLOCK_MAX * 17]; buf[0] = 0;
  for (int i = 0; i < g_muted_n; i++) { s_cat(buf, g_muted[i], sizeof(buf)); s_cat(buf, ";", sizeof(buf)); }
  hal_kv_set("muted", 5, buf, s_len(buf));
}
/* Tell the host which callsigns to hide from the Activity feed (blocked + muted),
 * so existing posts disappear too — not just future ones. The host filters its
 * activity list by this set. Re-sent whenever the lists change (and on init). */
static void emit_activity_filter(void) {
  char m[BLOCK_MAX * 2 * 19 + 64];
  s_cpy(m, "{\"type\":\"ui.activity.filter\",\"calls\":[", sizeof(m));
  int first = 1;
  for (int i = 0; i < g_blocked_n; i++) {
    if (!first) s_cat(m, ",", sizeof(m)); first = 0;
    s_cat(m, "\"", sizeof(m)); jesc(m, sizeof(m), g_blocked[i]); s_cat(m, "\"", sizeof(m));
  }
  for (int i = 0; i < g_muted_n; i++) {
    if (!first) s_cat(m, ",", sizeof(m)); first = 0;
    s_cat(m, "\"", sizeof(m)); jesc(m, sizeof(m), g_muted[i]); s_cat(m, "\"", sizeof(m));
  }
  s_cat(m, "]}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
static void hidden_save(void) {
  char buf[HIDE_MAX * 17]; buf[0] = 0;
  for (int i = 0; i < g_hidden_n; i++) { s_cat(buf, g_hidden[i], sizeof(buf)); s_cat(buf, ";", sizeof(buf)); }
  hal_kv_set("hidden", 6, buf, s_len(buf));
}
static void csv_load(const char *kv, int klen, char dst[][16], int *cnt, int cap) {
  char buf[HIDE_MAX * 17];
  uint32_t n = hal_kv_get(kv, (uint32_t)klen, buf, sizeof(buf) - 1);
  if (n == 0) return;
  buf[n] = 0;
  char c[16]; int j = 0;
  for (unsigned i = 0; i <= n; i++) {
    char ch = (i < n) ? buf[i] : ';';
    if (ch == ';') { c[j] = 0; if (c[0] && *cnt < cap) s_cpy(dst[(*cnt)++], c, 16); j = 0; }
    else if (j < 15) c[j++] = ch;
  }
}
static void blockhide_load(void) {
  csv_load("blocked", 7, g_blocked, &g_blocked_n, BLOCK_MAX);
  csv_load("muted", 5, g_muted, &g_muted_n, BLOCK_MAX);
  csv_load("hidden", 6, g_hidden, &g_hidden_n, HIDE_MAX);
}
static void block_add(const char *call) {
  char up_call[16]; int j = 0;
  for (int i = 0; call[i] && j < 15; i++) up_call[j++] = up(call[i]);
  up_call[j] = 0;
  if (!up_call[0] || s_eq(up_call, g_call) || is_blocked(up_call)) return;
  if (g_blocked_n >= BLOCK_MAX) { notify("warning", "Block list is full"); return; }
  s_cpy(g_blocked[g_blocked_n++], up_call, 16);
  blocked_save();
  convo_remove_from(up_call);   /* drop their already-shown bubbles + DM convo */
  host_state_emit("block", up_call, 1);
  emit_activity_filter();       /* hide their existing Activity posts too */
}
/* Mute: hide a callsign's NEW messages (Activity + groups + DMs) without
 * discarding their conversation or existing bubbles. Local + persisted. */
static void mute_add(const char *call) {
  char up_call[16]; int j = 0;
  for (int i = 0; call[i] && j < 15; i++) up_call[j++] = up(call[i]);
  up_call[j] = 0;
  if (!up_call[0] || s_eq(up_call, g_call) || is_muted(up_call)) return;
  if (g_muted_n >= BLOCK_MAX) { notify("warning", "Mute list is full"); return; }
  s_cpy(g_muted[g_muted_n++], up_call, 16);
  muted_save();
  host_state_emit("mute", up_call, 1);
  emit_activity_filter();
  notify("info", "Muted — their new messages are hidden");
}
static void block_remove(const char *call) {
  char up_call[16]; int j = 0;
  for (int i = 0; call[i] && j < 15; i++) up_call[j++] = up(call[i]);
  up_call[j] = 0;
  for (int i = 0; i < g_blocked_n; i++) if (s_eq(g_blocked[i], up_call)) {
    for (int k = i; k < g_blocked_n - 1; k++) s_cpy(g_blocked[k], g_blocked[k + 1], 16);
    g_blocked_n--; blocked_save();
    host_state_emit("block", up_call, 0);
    emit_activity_filter();
    return;
  }
}
static void hide_add(const char *id, const char *key) {
  if (!key[0]) return;
  if (!is_hidden_key(key)) {
    if (g_hidden_n >= HIDE_MAX) {            /* drop the oldest to make room */
      for (int k = 0; k < g_hidden_n - 1; k++) s_cpy(g_hidden[k], g_hidden[k + 1], 16);
      g_hidden_n--;
    }
    s_cpy(g_hidden[g_hidden_n++], key, 16);
    hidden_save();
  }
  convo_remove_key(id, key);
}
/* A "like" vote on message [mid] by station [from]. The host owns the tally:
 * it keeps the set of likers per message id (so each callsign counts once) and
 * derives the count + whether *we* liked it. [remove] retracts the like;
 * [mine] flags our own vote so the host can light the heart. App-agnostic on
 * the host side (a generic reaction by an opaque actor id). */
static void convo_react_of(const char *ck, const char *id, const char *mid,
                           const char *from, int remove, int mine);
static void convo_react(const char *id, const char *mid, const char *from,
                        int remove, int mine) {
  convo_react_of("", id, mid, from, remove, mine);
}
/* As convo_react, plus a CONTENT KEY for what was voted on. Two devices that
 * never agreed on an id — anything sent before ids were derived, or a peer
 * that numbers messages differently — resolve the target by content instead.
 * The key is opaque here: the host computes it from the message it holds, and
 * this wapp only carries it. Without it a like on an older message reached the
 * other side and matched nothing. */
static void convo_react_of(const char *ck, const char *id, const char *mid,
                           const char *from, int remove, int mine) {
  char m[260] = "{\"type\":\"ui.convo.react\",\"id\":\"";
  jesc(m, sizeof(m), id);
  s_cat(m, "\",\"mid\":\"", sizeof(m)); s_cat(m, mid, sizeof(m));
  s_cat(m, "\",\"from\":\"", sizeof(m)); jesc(m, sizeof(m), from);
  s_cat(m, "\"", sizeof(m));
  if (ck && ck[0]) {
    s_cat(m, ",\"ck\":\"", sizeof(m)); jesc(m, sizeof(m), ck);
    s_cat(m, "\"", sizeof(m));
  }
  if (remove) s_cat(m, ",\"remove\":true", sizeof(m));
  if (mine) s_cat(m, ",\"mine\":true", sizeof(m));
  s_cat(m, "}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
/* ── Recency + people-seen, per conversation ──────────────────────────────
 *
 * Two things the rail could not answer: which conversations you actually use,
 * and how many people are in one. Neither is derivable from the protocols —
 * a broadcast channel has no roster and a NIP-72 room publishes no join/leave
 * — so both are OBSERVED here and labelled as such.
 *
 * recency: last time a conversation was opened or carried a message, so the
 * rail and the search results can lead with what you talk to.
 * people: distinct senders seen per channel (rooms count theirs from the
 * op-log DB instead — see room_people_seen). */
#define RECENT_MAX 48
#define CHANPPL_MAX 16      /* channels tracked */
#define CHANPPL_WHO 24      /* senders remembered per channel */
static char g_recent_id[RECENT_MAX][80];
static uint64_t g_recent_ts[RECENT_MAX];
static int g_recent_n = 0;

static void recent_save(void) {
  char b[RECENT_MAX * 96]; b[0] = 0;
  for (int i = 0; i < g_recent_n; i++) {
    char nb[24]; u_itoa((unsigned)g_recent_ts[i], nb);
    s_cat(b, g_recent_id[i], sizeof(b)); s_cat(b, "=", sizeof(b));
    s_cat(b, nb, sizeof(b)); s_cat(b, ";", sizeof(b));
  }
  hal_kv_set("recent", 6, b, s_len(b));
}
static void recent_touch(const char *id) {
  if (!id || !id[0]) return;
  uint64_t now = hal_time_epoch();
  for (int i = 0; i < g_recent_n; i++) {
    if (s_eq(g_recent_id[i], id)) { g_recent_ts[i] = now; recent_save(); return; }
  }
  int slot = g_recent_n;
  if (g_recent_n < RECENT_MAX) g_recent_n++;
  else {                                   /* evict the stalest */
    slot = 0;
    for (int i = 1; i < g_recent_n; i++)
      if (g_recent_ts[i] < g_recent_ts[slot]) slot = i;
  }
  s_cpy(g_recent_id[slot], id, sizeof(g_recent_id[0]));
  g_recent_ts[slot] = now;
  recent_save();
}
static uint64_t recent_of(const char *id) {
  for (int i = 0; i < g_recent_n; i++)
    if (s_eq(g_recent_id[i], id)) return g_recent_ts[i];
  return 0;
}
static void recent_load(void) {
  char b[RECENT_MAX * 96];
  uint32_t n = hal_kv_get("recent", 6, b, sizeof(b) - 1);
  if (n == 0) return;
  b[n] = 0;
  char id[80]; char ts[24]; int j = 0, in_ts = 0;
  id[0] = 0; ts[0] = 0;
  for (unsigned i = 0; i <= n; i++) {
    char ch = (i < n) ? b[i] : ';';
    if (ch == '=') { id[j] = 0; j = 0; in_ts = 1; }
    else if (ch == ';') {
      if (in_ts) {
        ts[j] = 0;
        if (id[0] && g_recent_n < RECENT_MAX) {
          s_cpy(g_recent_id[g_recent_n], id, sizeof(g_recent_id[0]));
          g_recent_ts[g_recent_n] = (uint64_t)to_int(ts);
          g_recent_n++;
        }
      }
      j = 0; in_ts = 0; id[0] = 0; ts[0] = 0;
    } else if (!in_ts && j < 79) id[j++] = ch;
    else if (in_ts && j < 23) ts[j++] = ch;
  }
}

/* Distinct senders seen per channel. */
static char g_cp_id[CHANPPL_MAX][40];
static char g_cp_who[CHANPPL_MAX][CHANPPL_WHO][16];
static int g_cp_n[CHANPPL_MAX];
static int g_cp_used = 0;

static void chanppl_save(void) {
  char b[CHANPPL_MAX * (40 + CHANPPL_WHO * 17)]; b[0] = 0;
  for (int i = 0; i < g_cp_used; i++) {
    s_cat(b, g_cp_id[i], sizeof(b)); s_cat(b, "=", sizeof(b));
    for (int k = 0; k < g_cp_n[i]; k++) {
      s_cat(b, g_cp_who[i][k], sizeof(b)); s_cat(b, ",", sizeof(b));
    }
    s_cat(b, ";", sizeof(b));
  }
  hal_kv_set("chanppl", 7, b, s_len(b));
}
static void chanppl_add(const char *chan, const char *who) {
  if (!chan || chan[0] != '#' || !who || !who[0]) return;
  int slot = -1;
  for (int i = 0; i < g_cp_used; i++) if (s_eq(g_cp_id[i], chan)) { slot = i; break; }
  if (slot < 0) {
    if (g_cp_used >= CHANPPL_MAX) return;
    slot = g_cp_used++;
    s_cpy(g_cp_id[slot], chan, sizeof(g_cp_id[0]));
    g_cp_n[slot] = 0;
  }
  for (int k = 0; k < g_cp_n[slot]; k++)
    if (s_eq(g_cp_who[slot][k], who)) return;          /* already counted */
  if (g_cp_n[slot] >= CHANPPL_WHO) return;             /* "24+" is enough */
  s_cpy(g_cp_who[slot][g_cp_n[slot]++], who, sizeof(g_cp_who[0][0]));
  chanppl_save();
}
static int chanppl_count(const char *chan) {
  for (int i = 0; i < g_cp_used; i++) if (s_eq(g_cp_id[i], chan)) return g_cp_n[i];
  return 0;
}
static void chanppl_load(void) {
  static char b[CHANPPL_MAX * (40 + CHANPPL_WHO * 17)];
  uint32_t n = hal_kv_get("chanppl", 7, b, sizeof(b) - 1);
  if (n == 0) return;
  b[n] = 0;
  char id[40], who[16]; int j = 0, in_who = 0, slot = -1;
  id[0] = 0; who[0] = 0;
  for (unsigned i = 0; i <= n; i++) {
    char ch = (i < n) ? b[i] : ';';
    if (ch == '=' && !in_who) {
      id[j] = 0; j = 0; in_who = 1;
      if (id[0] && g_cp_used < CHANPPL_MAX) {
        slot = g_cp_used++;
        s_cpy(g_cp_id[slot], id, sizeof(g_cp_id[0]));
        g_cp_n[slot] = 0;
      } else slot = -1;
    } else if (ch == ',' && in_who) {
      who[j] = 0; j = 0;
      if (slot >= 0 && who[0] && g_cp_n[slot] < CHANPPL_WHO)
        s_cpy(g_cp_who[slot][g_cp_n[slot]++], who, sizeof(g_cp_who[0][0]));
    } else if (ch == ';') {
      j = 0; in_who = 0; slot = -1; id[0] = 0; who[0] = 0;
    } else if (!in_who && j < 39) id[j++] = ch;
    else if (in_who && j < 15) who[j++] = ch;
  }
}

/* Display title for a conversation row. Groups show the bare name; local vs
 * global (trailing '*') is conveyed by the row icon (campaign vs public/globe)
 * plus a " · global"/" · local" tag (ASCII — the host renders titles as latin1,
 * so no emoji). 1:1 chats keep the callsign. */
static void lxmf_title(const char *id, char *out, unsigned osz);
static void convo_title(const char *id, char *out, unsigned osz) {
  if (s_pre(id, "lxmf:")) { lxmf_title(id, out, osz); return; }
  if (id[0] != '#') { s_cpy(out, id, osz); return; }
  char name[8]; int j = 0;
  for (int i = 1; id[i] && id[i] != '*' && j < 6; i++) name[j++] = id[i];
  name[j] = 0;
  int global = 0; for (int i = 1; id[i]; i++) if (id[i] == '*') global = 1;
  /* Keep the leading '#' so groups are instantly distinguishable from people. */
  s_cpy(out, "#", osz);
  s_cat(out, name, osz);
  s_cat(out, global ? " (global)" : " (local)", osz);   /* ASCII-only tag */
}
/* Refresh a conversation row (title/preview/icon + distance badge). */
/* Chat is a GROUP client now: 1:1 moved to the Mail wapp, which owns the
 * NOSTR kind-4 inbox. A 1:1 row appearing here would be a second, worse inbox
 * for the same message — the duplication the merge set out to end. Everything
 * that would open a conversation goes through convo_touch/convo_ensure, so one
 * guard in each is enough. */
static int is_group(const char *id) { return id && id[0] == '#'; }

/* An LXMF direct conversation with one NomadNet/Sideband peer:
 * "lxmf:<32-hex delivery dest>". Sends go out with hal_lxmf_send; inbound DMs
 * (hal_lxmf_recv, `from` = that same dest) land in the matching row. The
 * NOSTR-1:1 exclusion above does not apply: LXMF peers have no kind-4 inbox
 * anywhere — Chat IS their conversation surface. */
static int is_lxmf(const char *id) { return id && s_pre(id, "lxmf:"); }

/* Our own LXMF delivery dest — what a peer sees as `from`, and therefore half
 * of the thread id of everything we send (msg_id("<from>|<text>")). Deriving
 * ids the same way on both ends is what lets a reply or a heart name its
 * target on a transport with no room for extra fields. Cached: the dest is
 * fixed while the node runs, and the call returns 0 until it is up. */
static char g_self_dest[80] = "";
static const char *lxmf_self_dest(void) {
  if (!g_self_dest[0]) {
    uint32_t n = hal_rns_delivery_dest(g_self_dest, sizeof(g_self_dest) - 1);
    if (n > 0 && n < sizeof(g_self_dest)) g_self_dest[n] = 0;
    else g_self_dest[0] = 0;
  }
  return g_self_dest;
}

/* dest(32hex) -> announced display name (from the New-chat picker / the
 * announce registry at first contact). Persisted in KV "lxnames". */
#define LXN_MAX 32
static char g_lxn_dest[LXN_MAX][66];
static char g_lxn_name[LXN_MAX][34];
static int g_lxn_n = 0;
static const char *lxname_get(const char *dest) {
  for (int i = 0; i < g_lxn_n; i++)
    if (s_eq(g_lxn_dest[i], dest)) return g_lxn_name[i];
  return 0;
}

/* Peers whose name we have asked the directory for, and when.
 *
 * The lookup used to happen ONCE, at first contact. If the directory did not
 * know that delivery dest yet — which is the normal case, because a message can
 * arrive before the sender's announce does — the row kept the raw hash prefix
 * ("3b02bb89") FOREVER. Two phones showed the same peer under two different
 * names, and the wrong one never healed.
 *
 * So it is retried. Slowly, on purpose: a display name is cosmetic, and
 * docs/performance.md is blunt that a cosmetic value never deserves a hot loop
 * ("a HAL call that does more than it says"). Once a minute per peer, and only
 * while the name is still missing. */
#define LXQ_MAX 16
#define LXQ_RETRY_S 60
static char g_lxq_dest[LXQ_MAX][66];
static uint64_t g_lxq_at[LXQ_MAX];
static int g_lxq_n = 0;

/* True when we may ask the directory about [dest] right now. */
static int lxname_may_ask(const char *dest) {
  uint64_t now = hal_time_epoch();
  for (int i = 0; i < g_lxq_n; i++) {
    if (!s_eq(g_lxq_dest[i], dest)) continue;
    if (now - g_lxq_at[i] < LXQ_RETRY_S) return 0;
    g_lxq_at[i] = now;
    return 1;
  }
  if (g_lxq_n >= LXQ_MAX) {          /* forget the oldest ask */
    for (int i = 0; i < g_lxq_n - 1; i++) {
      s_cpy(g_lxq_dest[i], g_lxq_dest[i + 1], sizeof(g_lxq_dest[i]));
      g_lxq_at[i] = g_lxq_at[i + 1];
    }
    g_lxq_n--;
  }
  s_cpy(g_lxq_dest[g_lxq_n], dest, sizeof(g_lxq_dest[0]));
  g_lxq_at[g_lxq_n] = now;
  g_lxq_n++;
  return 1;
}

static void lxname_save(void) {
  char b[LXN_MAX * 100]; b[0] = 0;
  for (int i = 0; i < g_lxn_n; i++) {
    s_cat(b, g_lxn_dest[i], sizeof(b)); s_cat(b, "=", sizeof(b));
    s_cat(b, g_lxn_name[i], sizeof(b)); s_cat(b, ";", sizeof(b));
  }
  hal_kv_set("lxnames", 7, b, s_len(b));
}
static void lxname_set(const char *dest, const char *name) {
  if (!dest[0] || !name || !name[0]) return;
  for (int i = 0; i < g_lxn_n; i++) {
    if (s_eq(g_lxn_dest[i], dest)) {
      s_cpy(g_lxn_name[i], name, sizeof(g_lxn_name[i]));
      lxname_save();
      return;
    }
  }
  if (g_lxn_n >= LXN_MAX) {   /* drop the oldest */
    for (int i = 0; i < g_lxn_n - 1; i++) {
      s_cpy(g_lxn_dest[i], g_lxn_dest[i + 1], sizeof(g_lxn_dest[i]));
      s_cpy(g_lxn_name[i], g_lxn_name[i + 1], sizeof(g_lxn_name[i]));
    }
    g_lxn_n--;
  }
  s_cpy(g_lxn_dest[g_lxn_n], dest, sizeof(g_lxn_dest[0]));
  s_cpy(g_lxn_name[g_lxn_n], name, sizeof(g_lxn_name[0]));
  g_lxn_n++;
  lxname_save();
}
static void lxname_load(void) {
  char b[LXN_MAX * 100];
  uint32_t n = hal_kv_get("lxnames", 7, b, sizeof(b) - 1);
  if (n == 0) return;
  b[n] = 0;
  char dest[66], name[34]; int j = 0, in_name = 0;
  dest[0] = 0; name[0] = 0;
  for (unsigned i = 0; i <= n; i++) {
    char ch = (i < n) ? b[i] : ';';
    if (ch == '=') { dest[j] = 0; j = 0; in_name = 1; }
    else if (ch == ';') {
      if (in_name) { name[j] = 0; if (dest[0] && name[0] && g_lxn_n < LXN_MAX) {
          s_cpy(g_lxn_dest[g_lxn_n], dest, sizeof(g_lxn_dest[0]));
          s_cpy(g_lxn_name[g_lxn_n], name, sizeof(g_lxn_name[0]));
          g_lxn_n++; } }
      j = 0; in_name = 0; dest[0] = 0; name[0] = 0;
    } else if (!in_name && j < 65) dest[j++] = ch;
    else if (in_name && j < 33) name[j++] = ch;
  }
}
/* Title for an lxmf: conversation — the peer's announced name, else a short
 * address ("LXMF 89b4e176"). */
static void lxmf_title(const char *id, char *out, unsigned osz) {
  const char *dest = id + 5;
  const char *nm = lxname_get(dest);
  if (nm) { s_cpy(out, nm, osz); return; }
  s_cpy(out, "LXMF ", osz);
  char sh[9]; s_cpy(sh, dest, sizeof(sh));
  s_cat(out, sh, osz);
}

/* Chat renders exactly three kinds of conversation: a #group, a room, and an
 * "lxmf:" thread (see convo_msg, which drops everything else). An id outside
 * that set can therefore NEVER hold a message — and two emitters below used to
 * upsert one keyed by a bare CALLSIGN, which produced a row in the host list
 * that looked like a conversation, carried a preview, and opened to "No
 * messages yet" forever. The real thread sat right beside it under its
 * "lxmf:<dest>" id. Refuse those ids, and remove any ghost an older build
 * already wrote into the host's store. */
static int convo_renderable(const char *id) {
  return id && id[0] && (is_group(id) || is_lxmf(id) || room_is_room(id));
}

static void convo_drop_ghost(const char *id) {
  if (!id || !id[0]) return;
  char m[140] = "{\"type\":\"ui.convo.remove\",\"id\":\"";
  jesc(m, sizeof(m), id);
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

static void convo_touch(const char *id, const char *preview, int select) {
  if (!is_group(id) && !is_lxmf(id) && !room_is_room(id)) return;
  convo_remember(id);
  recent_touch(id);   /* traffic counts as recency, and survives a restart */
  int global = 0; for (int i = 1; id[i]; i++) if (id[i] == '*') global = 1;
  const char *icon = (id[0] == '#') ? (global ? "public" : "campaign") : "person";
  char badge[24] = "";
  if (id[0] != '#') distance_badge(id, badge, sizeof(badge));
  char title[24]; convo_title(id, title, sizeof(title));
  char m[600] = "{\"type\":\"ui.convo.upsert\",\"id\":\"";
  jesc(m, sizeof(m), id);
  s_cat(m, "\",\"title\":\"", sizeof(m)); jesc(m, sizeof(m), title);
  s_cat(m, "\",\"subtitle\":\"", sizeof(m)); jesc(m, sizeof(m), preview);
  s_cat(m, "\",\"badge\":\"", sizeof(m)); jesc(m, sizeof(m), badge);
  s_cat(m, "\",\"icon\":\"", sizeof(m)); s_cat(m, icon, sizeof(m));
  if (select) s_cat(m, "\",\"select\":true,\"bump\":true}", sizeof(m));
  else s_cat(m, "\",\"bump\":true}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
/* Distance-only refresh (when a known contact beacons a new position). */
static void convo_badge_only(const char *id) {
  if (id[0] == '#') return;
  if (!convo_renderable(id)) { convo_drop_ghost(id); return; }
  char badge[24] = ""; distance_badge(id, badge, sizeof(badge));
  if (!badge[0]) return;
  char m[160] = "{\"type\":\"ui.convo.upsert\",\"id\":\"";
  jesc(m, sizeof(m), id);
  s_cat(m, "\",\"badge\":\"", sizeof(m)); jesc(m, sizeof(m), badge);
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* ── XPRS message signatures ──────────────────────────────────────────── */
/* base85 alphabet — must match the host (lib/util/xprs_crypto.dart). */
static int is_b85(char c) {
  if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
    return 1;
  const char *p = ".-+=^!/*?&<>()[]%$#@,;_";
  for (; *p; p++) if (*p == c) return 1;
  return 0;
}
/* A signed message ends with " ~<60 base85 chars>". If present, copy the body
 * (without that suffix) into [core] and the 60-char signature into [sig], and
 * return 1; else 0. */
#define SIG_B85_LEN 60
static int sig_split(const char *text, char *core, unsigned coresz,
                     char *sig, unsigned sigsz) {
  int n = (int)s_len(text);
  if (n < SIG_B85_LEN + 2) return 0;
  int s0 = n - SIG_B85_LEN;
  if (text[s0 - 1] != '~' || text[s0 - 2] != ' ') return 0;
  for (int i = s0; i < n; i++) if (!is_b85(text[i])) return 0;
  int clen = s0 - 2;
  if ((unsigned)clen >= coresz) clen = (int)coresz - 1;
  for (int i = 0; i < clen; i++) core[i] = text[i];
  core[clen] = 0;
  unsigned j = 0;
  for (int i = s0; i < n && j + 1 < sigsz; i++) sig[j++] = text[i];
  sig[j] = 0;
  return 1;
}
/* canonical signed bytes = "<from>|<core>" (must match the signer) */
static void sig_canon(char *out, unsigned sz, const char *from, const char *core) {
  out[0] = 0; s_cat(out, from, sz); s_cat(out, "|", sz); s_cat(out, core, sz);
}

/* ── callsign -> pubkey map (filled from received NOSTR beacons) ───────── */
static char g_pk_scratch[PK_MAX * 64];
static const char *pk_get(const char *call) {
  for (int i = 0; i < g_pk_n; i++) if (s_eq(g_pk_call[i], call)) return g_pk_key[i];
  return 0;
}
/* Bridge a callsign follow to the host's generic NOSTR follow set: when we know
 * a followed callsign's public key, tell the host to host that pubkey's notes/
 * files with the "followed" retention tier. [follow]=1 follow, 0 unfollow. The
 * host normalises the base64url key to hex. No-op without a known key. */
static void host_follow_emit(const char *call, int follow) {
  const char *key = pk_get(call);
  if (!key || !key[0]) return;
  char m[160] = "{\"type\":\"social.";
  s_cat(m, follow ? "follow" : "unfollow", sizeof(m));
  s_cat(m, "\",\"pubkey\":\"", sizeof(m));
  jesc(m, sizeof(m), key);
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
/* Ask the host to store one of OUR posts (a public group bulletin or an Activity
 * message) as a signed NOSTR note, so peers can request our posts later. [topic]
 * tags the group/context. Only for public content — never 1:1 DMs. */
/* Tell the host a callsign's public key (from its NOSTR beacon) so the Activity
 * feed + profile screen can show the npub. Sent for every key we learn. */
static void host_identity_emit(const char *call, const char *key) {
  if (!call || !call[0] || !key || !key[0]) return;
  char m[160] = "{\"type\":\"social.identity\",\"callsign\":\"";
  jesc(m, sizeof(m), call);
  s_cat(m, "\",\"pubkey\":\"", sizeof(m));
  jesc(m, sizeof(m), key);
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
/* Tell the host whether we follow / have blocked [call], so its profile UI shows
 * the right buttons. [kind]="follow" or "block"; [on]=1/0. */
static void host_state_emit(const char *kind, const char *call, int on) {
  if (!call || !call[0]) return;
  char m[120] = "{\"type\":\"social.";
  s_cat(m, kind, sizeof(m));
  s_cat(m, "state\",\"callsign\":\"", sizeof(m));
  jesc(m, sizeof(m), call);
  s_cat(m, "\",\"on\":", sizeof(m));
  s_cat(m, on ? "true" : "false", sizeof(m));
  s_cat(m, "}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
static void host_note_emit(const char *text, const char *topic, const char *parent) {
  if (!text || !text[0]) return;
  char m[640] = "{\"type\":\"social.note\",\"text\":\"";
  jesc(m, sizeof(m), text);
  s_cat(m, "\",\"topic\":\"", sizeof(m));
  if (topic) jesc(m, sizeof(m), topic);
  if (parent && parent[0]) { s_cat(m, "\",\"parent\":\"", sizeof(m)); s_cat(m, parent, sizeof(m)); }
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
static void pk_save(void) {
  g_pk_scratch[0] = 0;
  for (int i = 0; i < g_pk_n; i++) {
    char tb[12]; u_itoa((unsigned)g_pk_ts[i], tb);
    s_cat(g_pk_scratch, g_pk_call[i], sizeof(g_pk_scratch));
    s_cat(g_pk_scratch, "=", sizeof(g_pk_scratch));
    s_cat(g_pk_scratch, g_pk_key[i], sizeof(g_pk_scratch));
    s_cat(g_pk_scratch, "=", sizeof(g_pk_scratch));   /* base64url has no '=' */
    s_cat(g_pk_scratch, tb, sizeof(g_pk_scratch));
    s_cat(g_pk_scratch, ";", sizeof(g_pk_scratch));
  }
  hal_kv_set("pubkeys", 7, g_pk_scratch, s_len(g_pk_scratch));
}
static void pk_store(const char *call, const char *key) {
  if (!call[0] || !key[0] || s_eq(call, g_call)) return;
  for (int i = 0; i < g_pk_n; i++) if (s_eq(g_pk_call[i], call)) {
    g_pk_ts[i] = hal_time_epoch();
    if (!s_eq(g_pk_key[i], key)) { s_cpy(g_pk_key[i], key, sizeof(g_pk_key[i])); pk_save(); }
    pk_render();
    host_identity_emit(call, key);
    if (is_following(call)) host_follow_emit(call, 1);
    return;
  }
  if (g_pk_n >= PK_MAX) return;
  s_cpy(g_pk_call[g_pk_n], call, sizeof(g_pk_call[0]));
  s_cpy(g_pk_key[g_pk_n], key, sizeof(g_pk_key[0]));
  g_pk_ts[g_pk_n] = hal_time_epoch();
  g_pk_n++;
  pk_save();
  pk_render();
  host_identity_emit(call, key);
  /* Key arrived for a callsign we already follow → bridge the follow now. */
  if (is_following(call)) host_follow_emit(call, 1);
}
static void pk_load(void) {
  uint32_t n = hal_kv_get("pubkeys", 7, g_pk_scratch, sizeof(g_pk_scratch) - 1);
  if (n == 0) return;
  g_pk_scratch[n] = 0;
  char call[16], key[48], ts[12]; int ci = 0, ki = 0, ti = 0, stage = 0;
  for (unsigned i = 0; i <= n; i++) {
    char c = (i < n) ? g_pk_scratch[i] : ';';
    if (c == ';') {
      if (ci > 0 && ki > 0 && g_pk_n < PK_MAX) {
        call[ci] = 0; key[ki] = 0; ts[ti] = 0;
        s_cpy(g_pk_call[g_pk_n], call, 16); s_cpy(g_pk_key[g_pk_n], key, 48);
        g_pk_ts[g_pk_n] = ti > 0 ? (uint64_t)to_int(ts) : 0;   /* legacy: no ts */
        g_pk_n++;
      }
      ci = 0; ki = 0; ti = 0; stage = 0;
    } else if (c == '=' && stage < 2) stage++;
    else if (stage == 0) { if (ci < 15) call[ci++] = c; }
    else if (stage == 1) { if (ki < 47) key[ki++] = c; }
    else { if (ti < 11) ts[ti++] = c; }
  }
}
/* ── npub -> {RNS delivery dests} (multi-device), from extended NOSTR beacons
 * "<npub>|<deliv-hex>". One user may run several devices on one npub (different
 * RNS dests) — keep them all and send to every one. Dests are routing-only;
 * confidentiality comes from encrypting to the npub, so a stale/spoofed dest just
 * yields an undecryptable copy. KV "rnsdest" = "npub=dest=ts;…". ───────────── */
#define RNS_MAX 96
#define RNS_TTL 172800   /* 48h: skip a dest not re-beaconed within this window */
static char g_rns_npub[RNS_MAX][48];
static char g_rns_dest[RNS_MAX][40];   /* delivery dest (peers send_to here) */
static char g_rns_prop[RNS_MAX][40];   /* propagation dest (we pull store-and-forward from here) */
static uint64_t g_rns_dts[RNS_MAX];
static int g_rns_n = 0;
static char g_rns_scratch[RNS_MAX * 144];
static void rns_dest_save(void) {
  g_rns_scratch[0] = 0;
  for (int i = 0; i < g_rns_n; i++) {
    char tb[12]; u_itoa((unsigned)g_rns_dts[i], tb);
    s_cat(g_rns_scratch, g_rns_npub[i], sizeof(g_rns_scratch)); s_cat(g_rns_scratch, "=", sizeof(g_rns_scratch));
    s_cat(g_rns_scratch, g_rns_dest[i], sizeof(g_rns_scratch)); s_cat(g_rns_scratch, "=", sizeof(g_rns_scratch));
    s_cat(g_rns_scratch, g_rns_prop[i], sizeof(g_rns_scratch)); s_cat(g_rns_scratch, "=", sizeof(g_rns_scratch));
    s_cat(g_rns_scratch, tb, sizeof(g_rns_scratch)); s_cat(g_rns_scratch, ";", sizeof(g_rns_scratch));
  }
  hal_kv_set("rnsdest", 7, g_rns_scratch, s_len(g_rns_scratch));
}
static void rns_dest_store(const char *npub, const char *dest, const char *prop) {
  if (!npub[0] || !dest[0]) return;
  uint64_t now = hal_time_epoch();
  for (int i = 0; i < g_rns_n; i++)
    if (s_eq(g_rns_npub[i], npub) && s_eq(g_rns_dest[i], dest)) {
      g_rns_dts[i] = now;
      if (prop && prop[0]) s_cpy(g_rns_prop[i], prop, sizeof(g_rns_prop[0]));
      rns_dest_save(); return;
    }
  int slot;
  if (g_rns_n < RNS_MAX) slot = g_rns_n++;
  else { slot = 0; for (int i = 1; i < g_rns_n; i++) if (g_rns_dts[i] < g_rns_dts[slot]) slot = i; }
  s_cpy(g_rns_npub[slot], npub, sizeof(g_rns_npub[0]));
  s_cpy(g_rns_dest[slot], dest, sizeof(g_rns_dest[0]));
  s_cpy(g_rns_prop[slot], (prop && prop[0]) ? prop : "", sizeof(g_rns_prop[0]));
  g_rns_dts[slot] = now;
  rns_dest_save();
}
static void rns_dest_load(void) {
  uint32_t n = hal_kv_get("rnsdest", 7, g_rns_scratch, sizeof(g_rns_scratch) - 1);
  if (n == 0) return;
  g_rns_scratch[n] = 0;
  char np[48], de[40], pr[40], ts[12]; int pi = 0, di = 0, ri = 0, ti = 0, stage = 0;
  for (unsigned i = 0; i <= n; i++) {
    char c = (i < n) ? g_rns_scratch[i] : ';';
    if (c == ';') {
      if (pi > 0 && di > 0 && g_rns_n < RNS_MAX) {
        np[pi] = 0; de[di] = 0; pr[ri] = 0; ts[ti] = 0;
        s_cpy(g_rns_npub[g_rns_n], np, 48); s_cpy(g_rns_dest[g_rns_n], de, 40);
        s_cpy(g_rns_prop[g_rns_n], pr, 40);
        g_rns_dts[g_rns_n] = ti > 0 ? (uint64_t)to_int(ts) : 0; g_rns_n++;
      }
      pi = 0; di = 0; ri = 0; ti = 0; stage = 0;
    } else if (c == '=' && stage < 3) stage++;
    else if (stage == 0) { if (pi < 47) np[pi++] = c; }
    else if (stage == 1) { if (di < 39) de[di++] = c; }
    else if (stage == 2) { if (ri < 39) pr[ri++] = c; }
    else { if (ti < 11) ts[ti++] = c; }
  }
}

/* ── per-conversation "private (Reticulum-only)" mode. When on, a 1:1 with this
 * callsign goes ONLY over Reticulum (never APRS-IS/BLE) and the peer's side is
 * auto-flipped via a ?PRIV control. KV "cpriv" = "CALL;CALL;…". ─────────────── */
#define CPRIV_MAX 64
static char g_cpriv[CPRIV_MAX][40];
static int g_cpriv_n = 0;
static int convo_is_private(const char *call) {
  for (int i = 0; i < g_cpriv_n; i++) if (s_eq(g_cpriv[i], call)) return 1;
  return 0;
}
static void cpriv_save(void) {
  char buf[CPRIV_MAX * 40]; buf[0] = 0;
  for (int i = 0; i < g_cpriv_n; i++) { s_cat(buf, g_cpriv[i], sizeof(buf)); s_cat(buf, ";", sizeof(buf)); }
  hal_kv_set("cpriv", 5, buf, s_len(buf));
}
static void cpriv_load(void) {
  char buf[CPRIV_MAX * 40];
  uint32_t n = hal_kv_get("cpriv", 5, buf, sizeof(buf) - 1);
  if (n == 0) return;
  buf[n] = 0; char c[40]; int ci = 0;
  for (unsigned i = 0; i <= n; i++) {
    char ch = (i < n) ? buf[i] : ';';
    if (ch == ';') { if (ci > 0 && g_cpriv_n < CPRIV_MAX) { c[ci] = 0; s_cpy(g_cpriv[g_cpriv_n++], c, 40); } ci = 0; }
    else if (ci < 39) c[ci++] = ch;
  }
}
/* Show/hide the private (off-grid) badge on a conversation row in the host UI. */
static void convo_priv_emit(const char *call, int on) {
  if (!convo_renderable(call)) { convo_drop_ghost(call); return; }
  char m[120] = "{\"type\":\"ui.convo.upsert\",\"id\":\"";
  jesc(m, sizeof(m), call);
  s_cat(m, "\",\"private\":", sizeof(m));
  s_cat(m, on ? "true" : "false", sizeof(m));
  s_cat(m, "}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
static void cpriv_set(const char *call, int on) {
  if (!call[0] || call[0] == '#') return;
  int idx = -1;
  for (int i = 0; i < g_cpriv_n; i++) if (s_eq(g_cpriv[i], call)) { idx = i; break; }
  if (on && idx < 0) { if (g_cpriv_n >= CPRIV_MAX) return; s_cpy(g_cpriv[g_cpriv_n++], call, 40); cpriv_save(); }
  else if (!on && idx >= 0) {
    for (int j = idx; j < g_cpriv_n - 1; j++) s_cpy(g_cpriv[j], g_cpriv[j + 1], 40);
    g_cpriv_n--; cpriv_save();
  } else return; /* no change */
  convo_priv_emit(call, on);
}

/* ── interaction-scoped pubkey capture ────────────────────────────────────
 * We only persist the public keys of callsigns we actually interact with (chat
 * with, or follow) — not every station whose hourly NOSTR beacon we overhear.
 * A NOSTR beacon from a station we don't (yet) interact with is parked in a
 * small in-memory pending cache; the moment we interact with that callsign it is
 * promoted into the persistent store. Interaction noticed first, beacon later,
 * works too: the callsign is marked "wanted" and the next beacon stores it. */
#define PEER_MAX 64
static char g_peer[PEER_MAX][16];          /* callsigns we interact with */
static int  g_peer_n = 0;
#define PEND_MAX 24
static char g_pend_call[PEND_MAX][16];     /* heard-but-not-yet-wanted keys */
static char g_pend_key[PEND_MAX][48];
static int  g_pend_n = 0;
static int  g_pend_head = 0;               /* ring write cursor */

static int peer_known(const char *call) {
  for (int i = 0; i < g_peer_n; i++) if (s_eq(g_peer[i], call)) return 1;
  return 0;
}
static const char *pend_get(const char *call) {
  for (int i = 0; i < g_pend_n; i++) if (s_eq(g_pend_call[i], call)) return g_pend_key[i];
  return 0;
}
static void pend_set(const char *call, const char *key) {
  for (int i = 0; i < g_pend_n; i++) if (s_eq(g_pend_call[i], call)) {
    s_cpy(g_pend_key[i], key, sizeof(g_pend_key[0])); return;
  }
  int slot = (g_pend_n < PEND_MAX) ? g_pend_n++ : g_pend_head;   /* ring-evict oldest */
  g_pend_head = (g_pend_head + 1) % PEND_MAX;
  s_cpy(g_pend_call[slot], call, sizeof(g_pend_call[0]));
  s_cpy(g_pend_key[slot], key, sizeof(g_pend_key[0]));
}
/* Note that we interact with [call]: remember it and, if its key was parked,
 * promote it to the persistent store now. */
static void peer_note(const char *call) {
  if (!call[0] || call[0] == '#' || s_eq(call, g_call)) return;
  if (!peer_known(call) && g_peer_n < PEER_MAX) s_cpy(g_peer[g_peer_n++], call, 16);
  const char *k = pend_get(call);
  if (k && !pk_get(call)) pk_store(call, k);
}
/* Intercept a NOSTR key beacon (group "NOSTR"): record from->pubkey only for
 * callsigns we interact with (others are parked); report it was handled so it is
 * never shown as a chat message. */
static int pk_intercept(const char *group, const char *from, const char *text) {
  if (!s_eq(group, "NOSTR")) return 0;
  /* Extended beacon "<npub>|<rns-deliv-hex>|<rns-prop-hex>"; legacy forms are
   * "<npub>|<deliv>" and just "<npub>". deliv = where we send_to this user; prop =
   * its propagation mailbox we pull store-and-forwarded messages from (the NAT-
   * tolerant path: WE initiate the pull). Learn both keyed by npub (all devices). */
  char npub[48] = "", deliv[40] = "", prop[40] = "";
  { int fld = 0, j = 0;
    for (int i = 0; ; i++) {
      char c = text[i];
      if (c == '|' || c == 0) {
        if (fld == 0) npub[j < 47 ? j : 47] = 0;
        else if (fld == 1) deliv[j < 39 ? j : 39] = 0;
        else if (fld == 2) { prop[j < 39 ? j : 39] = 0; }
        if (c == 0 || fld >= 2) break;
        fld++; j = 0; continue;
      }
      if (fld == 0) { if (j < 47) npub[j++] = c; }
      else if (fld == 1) { if (j < 39) deliv[j++] = c; }
      else { if (j < 39) prop[j++] = c; }
    }
  }
  if (deliv[0]) rns_dest_store(npub, deliv, prop);
  if (peer_known(from) || pk_get(from)) pk_store(from, npub);   /* interacting -> keep */
  else pend_set(from, npub);                                    /* overheard -> park */
  return 1;
}

/* ── follow list persistence + mutation ─────────────────────────────────── */
/* KV "follows": "CALL=tag1 tag2;CALL;…" — '=' starts the optional tag list
 * (callsigns never contain '='); the legacy "CALL;" form still parses. */
static void follows_save(void) {
  char buf[FOLLOW_MAX * 64]; buf[0] = 0;
  for (int i = 0; i < g_follow_n; i++) {
    s_cat(buf, g_follow[i], sizeof(buf));
    if (g_ftag[i][0]) { s_cat(buf, "=", sizeof(buf)); s_cat(buf, g_ftag[i], sizeof(buf)); }
    s_cat(buf, ";", sizeof(buf));
  }
  hal_kv_set("follows", 7, buf, s_len(buf));
}
static void follows_load(void) {
  char buf[FOLLOW_MAX * 64];
  uint32_t n = hal_kv_get("follows", 7, buf, sizeof(buf) - 1);
  if (n == 0) return;
  buf[n] = 0;
  char c[16], t[48]; int j = 0, ti = 0, stage = 0;
  for (unsigned i = 0; i <= n; i++) {
    char ch = (i < n) ? buf[i] : ';';
    if (ch == ';') {
      c[j] = 0; t[ti] = 0;
      if (c[0] && g_follow_n < FOLLOW_MAX && !is_following(c)) {
        s_cpy(g_follow[g_follow_n], c, 16);
        s_cpy(g_ftag[g_follow_n], t, 48);
        g_follow_n++;
      }
      j = 0; ti = 0; stage = 0;
    } else if (ch == '=' && stage == 0) stage = 1;
    else if (stage == 0) { if (j < 15) c[j++] = ch; }
    else { if (ti < 47) t[ti++] = ch; }
  }
}
static void followers_save(void) {
  char buf[FOLLOW_MAX * 17]; buf[0] = 0;
  for (int i = 0; i < g_follower_n; i++) {
    s_cat(buf, g_follower[i], sizeof(buf)); s_cat(buf, ";", sizeof(buf));
  }
  hal_kv_set("followers", 9, buf, s_len(buf));
}
static void followers_load(void) {
  char buf[FOLLOW_MAX * 17];
  uint32_t n = hal_kv_get("followers", 9, buf, sizeof(buf) - 1);
  if (n == 0) return;
  buf[n] = 0;
  char c[16]; int j = 0;
  for (unsigned i = 0; i <= n; i++) {
    char ch = (i < n) ? buf[i] : ';';
    if (ch == ';') { c[j] = 0; if (c[0] && g_follower_n < FOLLOW_MAX && !is_follower(c)) s_cpy(g_follower[g_follower_n++], c, 16); j = 0; }
    else if (j < 15) c[j++] = ch;
  }
}
static void follow_add(const char *call) {
  char up_call[16]; int j = 0;            /* callsigns are upper-case on the wire */
  for (int i = 0; call[i] && j < 15; i++) up_call[j++] = up(call[i]);
  up_call[j] = 0;
  if (!up_call[0] || s_eq(up_call, g_call) || is_following(up_call)) return;
  if (g_follow_n >= FOLLOW_MAX) { notify("warning", "Following list is full"); return; }
  s_cpy(g_follow[g_follow_n], up_call, 16);
  g_ftag[g_follow_n][0] = 0;
  g_follow_n++;
  follows_save();
  follow_render();
  peer_note(up_call);   /* following counts as interaction: keep their key */
  host_follow_emit(up_call, 1);   /* bridge to host NOSTR-follow tier (if key known) */
  host_state_emit("follow", up_call, 1);   /* profile UI state */
  /* Tell the station (Twitter-style): a directed ?FOLLOW control message on
   * both transports; their wapp records us in its Followers list. */
  if (g_sock >= 0 && g_logged)
    aprs_send_message_multi(g_sock, g_call, up_call, "?FOLLOW", APRS_MAX_MSG_LEN, &g_seq);
  if (g_ble_on) ble_tx_msg(up_call, "?FOLLOW");
  { char b[40] = "Following "; s_cat(b, up_call, sizeof(b)); notify("info", b); }
}
static void follow_remove(const char *call) {
  for (int i = 0; i < g_follow_n; i++) if (s_eq(g_follow[i], call)) {
    char gone[16]; s_cpy(gone, g_follow[i], sizeof(gone));
    for (int k = i; k < g_follow_n - 1; k++) {
      s_cpy(g_follow[k], g_follow[k + 1], 16);
      s_cpy(g_ftag[k], g_ftag[k + 1], 48);
    }
    g_follow_n--;
    follows_save();
    follow_render();
    host_follow_emit(gone, 0);   /* drop from host NOSTR-follow tier (if key known) */
    host_state_emit("follow", gone, 0);   /* profile UI state */
    if (g_sock >= 0 && g_logged)
      aprs_send_message_multi(g_sock, g_call, gone, "?UNFOLLOW", APRS_MAX_MSG_LEN, &g_seq);
    if (g_ble_on) ble_tx_msg(gone, "?UNFOLLOW");
    { char b[40] = "Unfollowed "; s_cat(b, gone, sizeof(b)); notify("info", b); }
    return;
  }
}
/* Set (or clear) the space-separated tags on a followed callsign. */
static void ftag_set(const char *call, const char *tags) {
  for (int i = 0; i < g_follow_n; i++) if (s_eq(g_follow[i], call)) {
    s_cpy(g_ftag[i], tags, sizeof(g_ftag[i]));
    follows_save();
    follow_render();
    return;
  }
}
/* A peer announced they (un)followed us. Update the Followers list; this is
 * control traffic, never shown as a chat message. */
static void follower_add(const char *call) {
  if (!call[0] || s_eq(call, g_call) || is_follower(call)) return;
  if (g_follower_n >= FOLLOW_MAX) return;
  s_cpy(g_follower[g_follower_n++], call, 16);
  followers_save();
  follow_render();
  { char b[48] = ""; s_cat(b, call, sizeof(b));
    s_cat(b, " started following you", sizeof(b)); notify("info", b); }
}
static void follower_remove(const char *call) {
  for (int i = 0; i < g_follower_n; i++) if (s_eq(g_follower[i], call)) {
    for (int k = i; k < g_follower_n - 1; k++) s_cpy(g_follower[k], g_follower[k + 1], 16);
    g_follower_n--;
    followers_save();
    follow_render();
    return;
  }
}
/* Intercept a directed ?FOLLOW / ?UNFOLLOW control message (returns 1). */
static int follow_intercept(const char *from, const char *text) {
  if (s_eq(text, "?FOLLOW"))   { follower_add(from);    return 1; }
  if (s_eq(text, "?UNFOLLOW")) { follower_remove(from); return 1; }
  return 0;
}
/* Intercept a directed ?PRIV1 / ?PRIV0 control: the peer toggled private
 * (Reticulum-only) mode for our shared 1:1 — mirror it locally so both sides go
 * off-APRS together (auto-negotiate). Consumed (never shown as a message). These
 * arrive only over Reticulum. */
static int priv_intercept(const char *from, const char *text) {
  if (s_eq(text, "?PRIV1")) { cpriv_set(from, 1); return 1; }
  if (s_eq(text, "?PRIV0")) { cpriv_set(from, 0); return 1; }
  return 0;
}

/* Activity dedup: the same packet can reach us twice (APRS-IS + a BLE iGate), so
 * collapse identical (sender,line) entries to one feed item. */
#define ACT_SEEN 64
static unsigned g_act_seen[ACT_SEEN];
static unsigned g_act_seen_n = 0;
static int act_seen_has(unsigned h) {
  unsigned n = g_act_seen_n < ACT_SEEN ? g_act_seen_n : ACT_SEEN;
  for (unsigned i = 0; i < n; i++) if (g_act_seen[i] == h) return 1;
  return 0;
}
static void act_seen_add(unsigned h) { g_act_seen[g_act_seen_n % ACT_SEEN] = h; g_act_seen_n++; }

/* Surface one item in the Activity feed — the unified stream of everything that
 * happens: every incoming group bulletin and direct message, plus BLE-spot
 * events. [convo] is the conversation the item belongs to ("#GROUP" or a
 * callsign), so tapping it in the host jumps straight to that conversation; ""
 * for an item with no conversation (e.g. a followed station's status). Deduped
 * on the sender + rendered line so dual-path delivery (NET + a BLE iGate) shows
 * once. A group prefix ("#NAME: ") is added so the feed reads at a glance. */
static void activity_feed(const char *convo, const char *from,
                          const char *text, const char *via,
                          double lat, double lon, const char *parent) {
  /* Blocked: discard (never shown, never archived). Muted: don't show new ones.
   * Either way, drop the post before it reaches the host/Activity archive. */
  if (is_blocked(from) || is_muted(from)) return;
  char line[300]; line[0] = 0;
  if (convo && convo[0] == '#') {            /* group context, scope star dropped */
    char g[10]; int j = 0;
    for (int i = 1; convo[i] && convo[i] != '*' && j < 8; i++) g[j++] = convo[i];
    g[j] = 0;
    s_cat(line, "#", sizeof(line)); s_cat(line, g, sizeof(line)); s_cat(line, ": ", sizeof(line));
  }
  s_cat(line, text, sizeof(line));
  unsigned h = sig_hash("actf", from, line);
  if (act_seen_has(h)) return;
  act_seen_add(h);
  char meta[24] = ""; if (lat != 0 || lon != 0) distance_to(lat, lon, meta, sizeof(meta));
  /* Stable per-post id (same scheme as group threading) so Like/Reply work: it
   * is derived from the author + body, so every device computes the same id. */
  char mid[5]; msg_id(from, text, mid);
  char t[8]; fmt_time(t);
  char m[520] = "{\"type\":\"ui.chat.append\",\"field\":\"activity\",\"message\":{\"dir\":\"in\",\"convo\":\"";
  jesc(m, sizeof(m), convo ? convo : "");
  s_cat(m, "\",\"from\":\"", sizeof(m)); jesc(m, sizeof(m), from);
  s_cat(m, "\",\"text\":\"", sizeof(m)); jesc(m, sizeof(m), line);
  s_cat(m, "\",\"kind\":\"msg\",\"mid\":\"", sizeof(m)); s_cat(m, mid, sizeof(m));
  if (parent && parent[0]) { s_cat(m, "\",\"parent\":\"", sizeof(m)); s_cat(m, parent, sizeof(m)); }
  s_cat(m, "\",\"meta\":\"", sizeof(m)); jesc(m, sizeof(m), meta);
  s_cat(m, "\"", sizeof(m));
  if (via && via[0]) { s_cat(m, ",\"via\":\"", sizeof(m)); jesc(m, sizeof(m), via_label(via)); s_cat(m, "\"", sizeof(m)); }
  s_cat(m, ",\"time\":\"", sizeof(m)); s_cat(m, t, sizeof(m)); s_cat(m, "\"", sizeof(m));
  cat_pos(m, sizeof(m), lat, lon);
  s_cat(m, "}}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
/* Tell the host about a like vote on an Activity post (so it can tally it). */
static void activity_react_emit(const char *mid, const char *from, int like, int mine) {
  char m[160] = "{\"type\":\"ui.activity.react\",\"mid\":\"";
  s_cat(m, mid, sizeof(m));
  s_cat(m, "\",\"from\":\"", sizeof(m)); jesc(m, sizeof(m), from);
  s_cat(m, "\",\"like\":", sizeof(m)); s_cat(m, like ? "true" : "false", sizeof(m));
  s_cat(m, ",\"mine\":", sizeof(m)); s_cat(m, mine ? "true" : "false", sizeof(m));
  s_cat(m, "}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
/* Echo one of OUR Activity posts (dir "out") with a mid so it can receive likes
 * + replies like any other post. */
static void activity_echo_self(const char *text, const char *parent) {
  /* Seed the incoming-feed dedup with our own post: if a copy of it ever
   * boomerangs past the self-checks (relay/digipeat edge case), it collapses
   * against this echo instead of appearing as a second feed item. */
  act_seen_add(sig_hash("actf", g_call, text));
  char mid[5]; msg_id(g_call, text, mid);
  char t[8]; fmt_time(t);
  char m[520] = "{\"type\":\"ui.chat.append\",\"field\":\"activity\",\"message\":{\"dir\":\"out\",\"convo\":\"\",\"from\":\"";
  jesc(m, sizeof(m), g_call);
  s_cat(m, "\",\"text\":\"", sizeof(m)); jesc(m, sizeof(m), text);
  s_cat(m, "\",\"kind\":\"msg\",\"mid\":\"", sizeof(m)); s_cat(m, mid, sizeof(m));
  if (parent && parent[0]) { s_cat(m, "\",\"parent\":\"", sizeof(m)); s_cat(m, parent, sizeof(m)); }
  s_cat(m, "\",\"meta\":\"\",\"time\":\"", sizeof(m)); s_cat(m, t, sizeof(m)); s_cat(m, "\"", sizeof(m));
  cat_pos(m, sizeof(m), g_lat, g_lon);
  s_cat(m, "}}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
/* A followed station's non-message activity (status / geo-chat post). [grp] is
 * the group context ("" for a status). Kept follow-gated so the feed isn't
 * flooded by every station's position comment. */
static void activity_capture(const char *from, const char *grp,
                             const char *text, const char *via) {
  if (!is_following(from)) return;
  char convo[10] = "";
  if (grp && grp[0]) { convo[0] = '#'; s_cpy(convo + 1, grp, sizeof(convo) - 1); }
  double lat = 0, lon = 0; pos_get(from, &lat, &lon);
  activity_feed(convo, from, text, via, lat, lon, "");
}

/* Deliver one conversation message: dedup by signature — first time shows in
 * the flow, a repeat is promoted to a pinned item (and further repeats are
 * ignored as updates of the same pin). [forcePin] is set for our own
 * recurring sends (pinned from the first beat). */
/* Returns 1 if a message bubble was delivered, 0 if dropped (a like vote or a
 * repeated/duplicate message) — callers gate notifications on this so recurring
 * bulletins/duplicates don't re-notify. */
static void trc(const char *tag, const char *a, const char *b);

static int convo_deliver(const char *id, const char *dir, const char *from,
                          const char *text, const char *preview,
                          const char *via) {
  /* Local block: never show anything from a blocked station (their own echoes of
   * our messages — dir "out" from g_call — are unaffected). */
  if (s_eq(dir, "in") && (is_blocked(from) || is_muted(from))) {
    trc("drop:blocked", from, "");
    return 0;
  }
  /* Receipt correlation id: pull `am:<6hex>` out of the wire + strip it so it
   * never displays (1:1 only; groups never carry it). */
  char am[8] = ""; char ambuf[720]; char pvbuf[720];
  if (id[0] != '#') {
    s_cpy(ambuf, text, sizeof(ambuf));
    extract_am(ambuf, am);
    /* A best-hope custody copy names the identity it is FOR. Anyone can put our
     * callsign on an envelope; only mail carrying our own key is ours. */
    { char np[64];
      if (extract_np(ambuf, np, sizeof(np)) && s_eq(dir, "in")) {
        const char *mine = g_pubkey;
        if (mine[0] && !s_eq(np, mine)) {
          trc("drop:not-our-npub", from, "");
          return 0;
        }
      }
    }
    text = ambuf;
    /* Strip the token from the list-preview too so the row subtitle is clean. */
    char tmp[8]; s_cpy(pvbuf, preview, sizeof(pvbuf));
    extract_am(pvbuf, tmp);
    preview = pvbuf;
  }
  /* Interacting with this callsign: capture its public key if we'd parked one. */
  if (s_eq(dir, "in")) peer_note(from);
  else if (id[0] != '#') peer_note(id);
  /* Threading is group-only: derive this message's id from the wire text and,
   * if it carries a "+<4hex> " reply marker, split off the parent + show the
   * text without the marker. 1:1 chats are untouched. */
  char mid[5] = "", parent[5] = "";
  /* XPRS signature: split off a trailing " ~<sig>" and verify it. The core
   * (sig stripped) is what we thread/id/display; the sig never affects mid. */
  char core[700]; char sigstr[80]; char auth[12] = ""; int have_sig = 0;
  const char *body = text;
  if (sig_split(text, core, sizeof(core), sigstr, sizeof(sigstr))) { body = core; have_sig = 1; }

  /* Encrypted 1:1 message ("ENC1:<base64>"): canonicalise (strip spaces that
   * multi-line reassembly inserts into the space-less base64) so the signature
   * matches, then decrypt with the peer's key (sender for incoming, recipient
   * for our own echo). Groups are never encrypted. */
  int enc = 0; char plain[460]; char canon_content[700];
  s_cpy(canon_content, body, sizeof(canon_content));
  const char *disp = body;
  if (id[0] != '#' && s_len(body) > 5 &&
      body[0]=='E'&&body[1]=='N'&&body[2]=='C'&&body[3]=='1'&&body[4]==':') {
    enc = 1;
    char b64[680]; unsigned bi = 0;
    for (const char *p = body + 5; *p; p++) if (*p != ' ' && bi + 1 < sizeof(b64)) b64[bi++] = *p;
    b64[bi] = 0;
    s_cpy(canon_content, "ENC1:", sizeof(canon_content)); s_cat(canon_content, b64, sizeof(canon_content));
    const char *peer = s_eq(dir, "out") ? id : from;
    const char *ppk = pk_get(peer);
    plain[0] = 0;
    if (ppk) {
      uint32_t pn = hal_decrypt(ppk, s_len(ppk), b64, s_len(b64), plain, sizeof(plain) - 1);
      if (pn > 0 && pn < sizeof(plain)) plain[pn] = 0;
      else s_cpy(plain, "[encrypted - cannot decrypt]", sizeof(plain));
    } else {
      s_cpy(plain, "[encrypted - no key]", sizeof(plain));
    }
    disp = plain;
  }

  /* Relay-dedup id: an encrypted 1:1 (and its NOSTR-relay copy) carries a
   * "\x01<rmid>\x02" prefix in the plaintext. Pull it out + strip it from the
   * display text; the dedup below keys on it so the directly-delivered copy and
   * the relay copy of one message collapse to a single bubble. */
  char rmid[12] = "";
  if (id[0] != '#' && disp[0] == '\x01') {
    int i = 1, j = 0;
    while (disp[i] && disp[i] != '\x02' && j < 11) rmid[j++] = disp[i++];
    rmid[j] = 0;
    if (disp[i] == '\x02') disp = disp + i + 1; else rmid[0] = 0;
  }

  /* Verify the signature over the canonical (space-normalised) content. */
  if (have_sig) {
    if (s_eq(dir, "out")) {
      s_cpy(auth, "verified", sizeof(auth));     /* we signed it ourselves */
    } else {
      const char *pk = pk_get(from);
      if (!pk) {
        s_cpy(auth, "unverified", sizeof(auth)); /* sender's key not known yet */
      } else {
        char canon[760]; sig_canon(canon, sizeof(canon), from, canon_content);
        int ok = hal_verify(pk, s_len(pk), canon, s_len(canon), sigstr, s_len(sigstr));
        s_cpy(auth, ok ? "verified" : "bad", sizeof(auth));
      }
    }
  }
  /* A relay-delivered DM (via "RLY") arrives already-decrypted: the host did the
   * NIP-04 decryption AND verified the kind-4 BIP-340 signature before handing it
   * to us (forgeries are dropped host-side). Reflect that so it shows the same
   * encrypted + verified badges as a directly-delivered signed ENC1 message. */
  if (s_eq(via, "RLY")) { enc = 1; s_cpy(auth, "verified", sizeof(auth)); }

  if (id[0] == '#') {
    /* A like vote ("<4hex>:like") is not a chat message: register the reaction
     * and stop (no bubble). Works for our own echo (mine) and others' votes. */
    char tgt[70]; int unlike; const char *vtext;
    if (votemark_parse(body, tgt, &unlike, &vtext)) {
      convo_react_of(vtext, id, tgt, from, unlike, s_eq(from, g_call));
      return 0;
    }
    char stgt[5];
    if (like_parse(body, stgt, &unlike)) {
      convo_react(id, stgt, from, unlike, s_eq(from, g_call));
      return 0;
    }
    msg_id(from, body, mid);
    thread_parse(body, parent, &disp);
  }
  /* Dedup on the signature-stripped (and for encrypted, space-normalised) core,
   * so the SAME message arriving via two transports (APRS-IS + a BLE iGate), or
   * signed vs unsigned forms, collapses to one. */
  /* When the message carries a relay-dedup id, key the dedup on it (the direct
   * and relay copies have DIFFERENT ciphertexts but the same rmid); otherwise
   * fall back to the content hash (collapses dual-transport copies of one wire). */
  unsigned h = rmid[0] ? sig_hash("r", from, rmid)
                       : sig_hash(id, from, enc ? canon_content : body);
  char key[16]; u_itoa(h, key);
  /* Locally hidden message: stays gone even if it arrives again on another
   * transport (the key is the same content signature the host hid it under). */
  if (is_hidden_key(key)) { trc("drop:hidden", from, ""); return 0; }
  /* Distance + position of the sender (incoming only), so the host can show
   * them on the map when the distance is tapped. */
  char meta[24] = "";
  double lat = 0, lon = 0;
  if (s_eq(dir, "in") && pos_get(from, &lat, &lon)) {
    distance_to(lat, lon, meta, sizeof(meta));
  }
  /* Relay-backed messages dedup on the persistent rmid ring (survives restarts,
   * so a late relay copy of an already-shown direct message is dropped); all
   * others use the in-memory content ring. */
  int rep;
  if (rmid[0]) { rep = midseen_has(rmid); if (!rep) midseen_add(rmid); }
  else { rep = seen_has(h); if (!rep) seen_add(h); }
  /* 1:1: also fold the key-unknown double — a public copy (no rmid, content
   * keyed) and its later encrypted backup (rmid keyed) share no dedup key but the
   * same plaintext. Collapse them on (id,from,plaintext). Encrypted messages no
   * longer ride APRS, so every copy decrypts to the same text (no undecryptable
   * twin with a divergent plaintext). Groups keep their own dedup. */
  /* Only when the primary key above hashed something OTHER than the final
   * plaintext (encrypted content or an rmid) — for a plain message h already
   * IS sig_hash(id,from,disp), and re-checking it here self-collides with the
   * seen_add above, dropping every plain 1:1 as its own duplicate. */
  if (!rep && id[0] != '#' && (enc || rmid[0])) {
    unsigned ph = sig_hash(id, from, disp);
    if (seen_has(ph)) rep = 1; else seen_add(ph);
  }
  /* A repeated INCOMING message (direct OR a recurring bulletin) is a duplicate
   * — dual-path delivery (APRS-IS + a BLE iGate), a resend, or a station
   * re-broadcasting the same bulletin on a schedule. Drop it so the chat shows
   * each distinct message once and recurring bulletins don't pile up or get
   * auto-pinned (that banner was just noise). Our own sends are never dropped. */
  if (rep && s_eq(dir, "in")) { trc("drop:dup", from, ""); return 0; }
  convo_msg(id, dir, from, disp, key, meta, lat, lon, via, mid, parent, auth, enc,
            (id[0] != '#') && convo_is_private(id));
  convo_touch(id, enc ? disp : preview, 0);   /* show decrypted text in the list */
  /* One notification per freshly-delivered INCOMING 1:1 message — fired HERE,
   * after multi-line reassembly + decryption, so a long/signed/encrypted DM
   * (which arrives as several APRS lines) alerts once with readable text instead
   * of once per line. The content dedup above means a message arriving over two
   * transports notifies only once. Group bulletins notify via their own caller;
   * our own echoes (dir "out") never notify. */
  if (s_eq(dir, "in") && id[0] != '#') notify_msg(from, from, disp, disp);
  /* WhatsApp-style receipts: a freshly-delivered inbound 1:1 that carried an `am`
   * confirms DELIVERED to the sender (over BLE/RNS; APRS uses the native ack sent
   * by route_frame) and queues a READ receipt for when the user opens the chat. */
  if (s_eq(dir, "in") && id[0] != '#' && am[0]) {
    if (!s_eq(via, "NET")) send_receipt(from, am, 'd', via);
    rpend_add(id, am, via);
  }
  /* NOTE: group/DM conversation messages are deliberately NOT mirrored into the
   * Activity feed. The Activity tab is the micro-blog stream (FEED group) only —
   * group chatter belongs in Messages, not the public stream. FEED posts reach
   * Activity directly in deliver_bulletin; followed-station status/likes reach it
   * via activity_capture. */
  return 1;
}

/* ── media share helper (XPRS section 16 + BitTorrent) ───────────────────────────
 * When a message we send embeds a media token (file:<sha256>.<ext>) for a file
 * we host, append the deterministic torrent infohash ("ih:<40hex>") to the
 * SAME message so receivers can join the swarm and fetch it over BitTorrent.
 * One message — no separate discovery traffic. */
static int find_file_token(const char *text, char *out, unsigned max) {
  for (const char *p = text; *p; p++) {
    if (p[0]=='f'&&p[1]=='i'&&p[2]=='l'&&p[3]=='e'&&p[4]==':') {
      const char *q = p + 5; int n = 0;
      while (((*q>='A'&&*q<='Z')||(*q>='a'&&*q<='z')||(*q>='0'&&*q<='9')||
              *q=='-'||*q=='_') && n < 43) { q++; n++; }
      if (n != 43 || *q != '.') continue;
      const char *r = q + 1; int e = 0;
      while (((*r>='a'&&*r<='z')||(*r>='0'&&*r<='9')) && e < 18) { r++; e++; }
      if (e < 1) continue;
      unsigned len = (unsigned)(r - p);
      if (len >= max) return 0;
      for (unsigned i = 0; i < len; i++) out[i] = p[i];
      out[len] = 0;
      return 1;
    }
  }
  return 0;
}
/* If [text] already carries a media token (and no ih: yet), append our
 * infohash for it. Mutates [text] in place (buffer must have room). */
static void add_infohash(char *text, unsigned sz) {
  char token[80];
  if (!find_file_token(text, token, sizeof(token))) return;
  for (const char *p = text; *p; p++)        /* already has an ih:? leave it */
    if (p[0]=='i'&&p[1]=='h'&&p[2]==':') return;
  char ih[48] = "";
  hal_media_infohash(token, s_len(token), ih, sizeof(ih) - 1);
  if (s_len(ih) < 32) return;                /* not ready / not hosted */
  s_cat(text, " ih:", sz); s_cat(text, ih, sz);
  /* Only the content hash (file:) + the BitTorrent infohash (ih:) ride on the
   * radio line — both are short and meaningful anywhere. Peer discovery is the
   * receiver's job: the swarm (DHT/trackers) over the internet, or a Blossom
   * LAN scan on the same network. No IP addresses go on the air. */
}

/* ── NOSTR-relay store-and-forward DM backup ──────────────────────────────
 * Each 1:1 message is ALSO published to up to 3 NOSTR relays reachable over
 * Reticulum as a kind-4 (NIP-04) encrypted DM, so it still arrives if the sender
 * reached the relays before becoming unreachable. The host (hal_relay_*) owns the
 * NOSTR work; here we pick relays, tell the peer where we back up (?RLY), poll
 * the relays peers told us about, deliver+dedup, and delete what we received.
 * A per-message id (rmid) is embedded INSIDE the encrypted plaintext so the relay
 * copy dedups against the directly-delivered copy (see convo_deliver). */
#define RELAY_MAX 3
#define RELAY_POLL_INTERVAL 60          /* seconds between relay polls */
#define POLLRELAY_MAX 8
static char g_myrelay[RELAY_MAX][72]; static int g_myrelay_n = 0;       /* our backup relays */
static char g_pollrelay[POLLRELAY_MAX][72]; static int g_pollrelay_n = 0; /* relays peers told us to poll */
static char g_rly_told[CPRIV_MAX][16]; static int g_rly_told_n = 0;     /* callsigns told our ?RLY (session) */

/* Cold-start 1:1: when sending to a callsign whose key we don't know yet, the
 * message goes out as PUBLIC APRS and we ask the relays to resolve callsign→npub
 * (hal_relay_resolve). The text waits here until a resolution arrives (or expires)
 * so we can then place an encrypted backup at the relays. */
#define PSEND_MAX 8
#define RESOLVE_TTL 90                 /* seconds to await a callsign→npub resolve */
static char g_psend_call[PSEND_MAX][16];
static char g_psend_text[PSEND_MAX][400];
static uint64_t g_psend_ts[PSEND_MAX]; static int g_psend_n = 0;
static char g_pubnote[CPRIV_MAX][16]; static int g_pubnote_n = 0; /* convos shown the "public only" note */

/* Case-insensitive callsign compare. */
static int s_eq_ci(const char *a, const char *b) {
  int i = 0; for (; a[i] && b[i]; i++) if (up(a[i]) != up(b[i])) return 0;
  return a[i] == b[i];
}

/* Build a JSON array ["h1","h2",…] from [arr][n]. */
static void relays_json(char arr[][72], int n, char *out, unsigned cap) {
  out[0] = '['; out[1] = 0;
  for (int i = 0; i < n; i++) {
    if (i) s_cat(out, ",", cap);
    s_cat(out, "\"", cap); s_cat(out, arr[i], cap); s_cat(out, "\"", cap);
  }
  s_cat(out, "]", cap);
}

/* Reverse pubkey lookup: the callsign whose stored npub == [npub], or NULL. */

/* Refresh our backup relays from the currently-reachable set (≤RELAY_MAX). */
static void relay_pick(void) {
  static char j[RELAY_MAX * 80 + 16];
  uint32_t n = hal_relay_reachable(j, sizeof(j) - 1);
  if (n == 0 || n >= sizeof(j)) return;
  j[n] = 0;
  int cnt = 0; const char *p = j;
  while (*p && cnt < RELAY_MAX) {            /* extract each "quoted" hash */
    while (*p && *p != '"') p++;
    if (!*p) break;
    p++;
    int k = 0; while (*p && *p != '"' && k < 71) g_myrelay[cnt][k++] = *p++;
    g_myrelay[cnt][k] = 0;
    if (*p == '"') p++;
    if (k > 0) cnt++;
  }
  g_myrelay_n = cnt;
}

/* Rendezvous relay set (host-ranked by sha256(relay|pubkey)): the sender
 * publishes to the RECIPIENT's set and the receiver polls its OWN set, so the
 * two meet without the one-shot ?RLY announce (which an offline receiver —
 * the whole point of the relay backup — never hears). */

static int relay_rendezvous(const char *np, char dst[][72], int max) {
  if (!np || !np[0]) return 0;
  static char j[RELAY_MAX * 80 + 16];
  int32_t n = hal_relay_for(np, s_len(np), j, sizeof(j) - 1);
  if (n <= 0 || n >= (int32_t)sizeof(j)) return 0;
  j[n] = 0;
  int cnt = 0; const char *p = j;
  while (*p && cnt < max) {
    while (*p && *p != '"') p++;
    if (!*p) break;
    p++;
    int k = 0; while (*p && *p != '"' && k < 71) dst[cnt][k++] = *p++;
    dst[cnt][k] = 0;
    if (*p == '"') p++;
    if (k > 0) cnt++;
  }
  return cnt;
}

/* Tell [call] which relays we back up to (once per session) — a control frame
 * "?RLY h1 h2 h3" so the peer knows where to poll for messages from us. */
static void relay_announce_to(const char *call) {
  if (g_myrelay_n == 0 || !call[0] || call[0] == '#') return;
  for (int i = 0; i < g_rly_told_n; i++) if (s_eq(g_rly_told[i], call)) return;
  if (g_rly_told_n < CPRIV_MAX) s_cpy(g_rly_told[g_rly_told_n++], call, 16);
  char m[300]; s_cpy(m, "?RLY", sizeof(m));
  for (int i = 0; i < g_myrelay_n; i++) { s_cat(m, " ", sizeof(m)); s_cat(m, g_myrelay[i], sizeof(m)); }
  rns_tx_msg(call, m);
}

static void pollrelay_save(void) {
  char b[POLLRELAY_MAX * 73]; b[0] = 0;
  for (int i = 0; i < g_pollrelay_n; i++) { s_cat(b, g_pollrelay[i], sizeof(b)); s_cat(b, " ", sizeof(b)); }
  hal_kv_set("pollrelays", 10, b, s_len(b));
}
static void pollrelay_load(void) {
  char b[POLLRELAY_MAX * 73];
  uint32_t n = hal_kv_get("pollrelays", 10, b, sizeof(b) - 1);
  if (n == 0) return;
  b[n] = 0; char h[72]; int k = 0;
  for (unsigned i = 0; i <= n; i++) {
    char c = (i < n) ? b[i] : ' ';
    if (c == ' ') { if (k > 0 && g_pollrelay_n < POLLRELAY_MAX) { h[k] = 0; s_cpy(g_pollrelay[g_pollrelay_n++], h, 72); } k = 0; }
    else if (k < 71) h[k++] = c;
  }
}
static void pollrelay_add(const char *h) {
  if (!h[0]) return;
  for (int i = 0; i < g_pollrelay_n; i++) if (s_eq(g_pollrelay[i], h)) return;
  int slot = (g_pollrelay_n < POLLRELAY_MAX) ? g_pollrelay_n++ : 0;  /* cap: overwrite oldest */
  s_cpy(g_pollrelay[slot], h, 72);
  pollrelay_save();
}

/* Intercept "?RLY <h1> <h2> …" — a peer telling us where it backs up; remember
 * those relays so we poll them for that peer's messages. Consume (not chat). */
static int rly_intercept(const char *from, const char *text) {
  (void)from;
  if (!(text[0] == '?' && text[1] == 'R' && text[2] == 'L' && text[3] == 'Y' &&
        (text[4] == ' ' || text[4] == 0)))
    return 0;
  const char *p = text + 4;
  while (*p) {
    while (*p == ' ') p++;
    if (!*p) break;
    char h[72]; int k = 0; while (*p && *p != ' ' && k < 71) h[k++] = *p++;
    h[k] = 0; pollrelay_add(h);
  }
  return 1;
}

/* Drain relay-fetched DMs (queued by hal_relay_dm_fetch): deliver each through
 * convo_deliver (which extracts the embedded rmid + dedups against the direct
 * copy), then DROP the received ids from the relays to reclaim space. */
/* JSON array of the relays we poll: those peers told us about (?RLY) UNION the
 * ones we ourselves can reach. A sender that reached us as a relay published to
 * OUR relay, so our own reachable set (+ the local store, always queried by the
 * host) catches a message even before any ?RLY arrives. */



/* A muted, centered status line shown inside conversation [id] (not a real
 * message). Used to tell the user e.g. that a send went out public-only. */
static void convo_sysnote(const char *id, const char *text) {
  char t[8]; fmt_time(t);
  char m[640] = "{\"type\":\"ui.convo.msg\",\"id\":\"";
  jesc(m, sizeof(m), id);
  s_cat(m, "\",\"dir\":\"in\",\"from\":\"\",\"sys\":true,\"text\":\"", sizeof(m));
  jesc(m, sizeof(m), text);
  s_cat(m, "\",\"time\":\"", sizeof(m)); s_cat(m, t, sizeof(m));
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* Show the "public only" note at most once per conversation. */
static int pubnote_once(const char *call) {
  for (int i = 0; i < g_pubnote_n; i++) if (s_eq(g_pubnote[i], call)) return 0;
  if (g_pubnote_n < CPRIV_MAX) s_cpy(g_pubnote[g_pubnote_n++], call, 16);
  return 1;
}

/* Queue a public send awaiting a callsign→npub resolution (ring, evict oldest). */
static void pendsend_add(const char *call, const char *text) {
  if (!call[0]) return;
  int slot = (g_psend_n < PSEND_MAX) ? g_psend_n++ : 0;
  s_cpy(g_psend_call[slot], call, sizeof(g_psend_call[0]));
  s_cpy(g_psend_text[slot], text, sizeof(g_psend_text[0]));
  g_psend_ts[slot] = hal_time_epoch();
}

/* Place an encrypted (NIP-04 kind-4) store-and-forward backup of [text] for
 * [call] at our relays (so the recipient can pick it up later), announce our
 * relays to them, and push a direct encrypted Reticulum copy now that the key is
 * known. Requires pk_get(call). Mirrors do_convo_send's encrypted 1:1 path; the
 * shared rmid lets the receiver dedup the relay + direct copies. */
static void deliver_1to1_backup(const char *call, const char *text) {
  const char *np = pk_get(call);
  if (!np || !np[0]) return;
  char rmid[12]; unsigned char rb[4]; hal_crypto_random((char *)rb, 4);
  static const char hx[] = "0123456789abcdef";
  for (int i = 0; i < 4; i++) { rmid[i*2] = hx[rb[i] >> 4]; rmid[i*2+1] = hx[rb[i] & 15]; }
  rmid[8] = 0;
  char relaypt[680]; int k = 0; relaypt[k++] = '\x01';
  for (int i = 0; rmid[i]; i++) relaypt[k++] = rmid[i];
  relaypt[k++] = '\x02';
  for (int i = 0; text[i] && k < (int)sizeof(relaypt) - 1; i++) relaypt[k++] = text[i];
  relaypt[k] = 0;
  if (g_myrelay_n == 0) relay_pick();
  /* Publish where the RECIPIENT will look: their rendezvous set (host-ranked
   * by their npub). Fall back to our own picks when the directory is empty. */
  {
    char rdv[RELAY_MAX][72];
    int rn = relay_rendezvous(np, rdv, RELAY_MAX);
    if (rn > 0 || g_myrelay_n > 0) {
      relay_announce_to(call);
      char rj[RELAY_MAX * 80 + 16];
      if (rn > 0) relays_json(rdv, rn, rj, sizeof(rj));
      else relays_json(g_myrelay, g_myrelay_n, rj, sizeof(rj));
      hal_relay_dm_send(np, s_len(np), relaypt, s_len(relaypt), rj, s_len(rj), rmid, s_len(rmid));
    }
  }
  /* Direct, encrypted Reticulum copy (the dest came with the resolution). */
  char ct[640];
  uint32_t cn = hal_encrypt(np, s_len(np), relaypt, s_len(relaypt), ct, sizeof(ct) - 1);
  if (cn > 0 && cn < sizeof(ct)) {
    ct[cn] = 0;
    char core[700]; s_cpy(core, "ENC1:", sizeof(core)); s_cat(core, ct, sizeof(core));
    char wire[800]; s_cpy(wire, core, sizeof(wire));
    char canon[720]; sig_canon(canon, sizeof(canon), g_call, core);
    char sg[80]; uint32_t sn = hal_identity_sign(canon, s_len(canon), sg, sizeof(sg) - 1);
    if (sn > 0 && sn < sizeof(sg)) { sg[sn] = 0; s_cat(wire, " ~", sizeof(wire)); s_cat(wire, sg, sizeof(wire)); }
    rns_tx_msg(call, wire);
  }
}

/* Remove pending-send entry [i] (compacting the ring). */
static void pendsend_remove(int i) {
  for (int j = i + 1; j < g_psend_n; j++) {
    s_cpy(g_psend_call[j-1], g_psend_call[j], 16);
    s_cpy(g_psend_text[j-1], g_psend_text[j], 400);
    g_psend_ts[j-1] = g_psend_ts[j];
  }
  g_psend_n--;
}

/* Drain async callsign→npub resolutions (from hal_relay_resolve). For each: store
 * the key + Reticulum dest, then flush any queued public sends to that callsign as
 * encrypted relay backups. Also expires pending sends that were never resolved. */
static void resolve_drain(void) {
  char buf[400];
  for (int guard = 0; guard < 8; guard++) {
    uint32_t n = hal_relay_resolve_recv(buf, sizeof(buf) - 1);
    if (n == 0) break;
    buf[n] = 0;
    char call[16] = "", npub[48] = "", deliv[40] = "", prop[40] = "";
    jstr(buf, "callsign", call, sizeof(call));
    jstr(buf, "npub", npub, sizeof(npub));
    jstr(buf, "deliv", deliv, sizeof(deliv));
    jstr(buf, "prop", prop, sizeof(prop));
    if (!call[0] || !npub[0]) continue;
    /* Prefer the conversation's own spelling of the callsign when we queued a send. */
    const char *store_call = call;
    for (int i = 0; i < g_psend_n; i++) if (s_eq_ci(g_psend_call[i], call)) { store_call = g_psend_call[i]; break; }
    pk_store(store_call, npub);
    if (deliv[0]) rns_dest_store(npub, deliv, prop);
    int found = 0;
    for (int i = 0; i < g_psend_n; ) {
      if (s_eq_ci(g_psend_call[i], call)) { deliver_1to1_backup(g_psend_call[i], g_psend_text[i]); found++; pendsend_remove(i); }
      else i++;
    }
    if (found) {
      char note[96]; s_cpy(note, "Found ", sizeof(note)); s_cat(note, store_call, sizeof(note));
      s_cat(note, "'s key - message also queued at relays for delivery.", sizeof(note));
      convo_sysnote(store_call, note);
    }
  }
  uint64_t now = hal_time_epoch();
  for (int i = 0; i < g_psend_n; ) {
    if (now - g_psend_ts[i] > RESOLVE_TTL) pendsend_remove(i); else i++;
  }
}

/* The send pipeline, with the target and text supplied by the caller — the
 * conversations layout parses them from its own field names, the rooms rail
 * from its (a channel on the rail is the same group underneath). [buf] still
 * carries the settings fields (read_config, include_location). */
static void convo_send_core(const char *buf, const char *id_in,
                            const char *text_in) {
  read_config(buf);
  char id[40] = "", text[400] = "";
  s_cpy(id, id_in, sizeof(id));
  s_cpy(text, text_in, sizeof(text));
  if (!id[0] || !text[0]) return;
  /* A room message is a NIP-72 community post (kind-1 tagged to the room); it
   * does not ride APRS/BLE/encryption. Post it and echo locally. */
  if (room_is_room(id)) {
    if (!room_self_can_post(id)) {
      notify("warning", "You can't post here right now (suspended, banned, or the room is closed)");
      return;
    }
    if (room_post(id, text)) {
      char from[16]; s_cpy(from, g_pubkey[0] ? g_pubkey : g_call, 13);
      convo_msg(id, "out", from, text, "", "", 0, 0, "NOS", "", "", "verified", 0, 0);
    } else {
      notify("warning", "Couldn't post to this room");
    }
    return;
  }
  int net = (g_sock >= 0 && g_logged);
  int rns = rns_up();
  /* Private (Reticulum-only) 1:1 rides Reticulum alone; a normal message needs
   * a live path — Reticulum (primary), BLE (local) or legacy APRS-IS. */
  int priv = (id[0] != '#') && convo_is_private(id);
  if (!priv && !net && !g_ble_on && !rns) {
    /* No live path. We can still try the NOSTR-relay backstop (resolve the
     * recipient's key, then queue an encrypted copy at relays for later pickup);
     * only give up entirely if no relays are reachable either. */
    if (g_myrelay_n == 0) relay_pick();
    if (g_myrelay_n == 0 || id[0] == '#') {
      notify("warning", "Enable Reticulum (or Bluetooth) first");
      return;
    }
  }
  /* Optionally share our location — never in private mode (a position beacon is a
   * broadcast that would leak the private thread). */
  int loc = !priv && jbool(buf, "include_location") && (g_lat != 0 || g_lon != 0);
  if (loc) {
    pos_broadcast(g_lat, g_lon, "");
    if (net) aprs_send_beacon(g_sock, g_call, g_lat, g_lon, g_symbol, "TCPIP*", "");
    push_marker(g_call, g_lat, g_lon, "blue", "");
  }
  /* Encrypt a 1:1 message to a callsign whose public key we know (ENC1: + a
   * base64url AES blob); group messages are never encrypted. The encrypted body
   * is what gets signed + transmitted, so only the recipient can read it but
   * anyone can still verify who sent it. */
  char core[700]; s_cpy(core, text, sizeof(core));
  int encrypted = 0;
  /* relaypt = the plaintext we actually encrypt — for an encrypted 1:1 it carries
   * a per-message id "\x01<rmid>\x02" prefix so the directly-delivered copy and
   * the NOSTR-relay copy (both encrypt the SAME plaintext) dedup on receipt. */
  char relaypt[680] = ""; char rmid[12] = "";
  if (id[0] != '#') {
    const char *rpk = pk_get(id);
    if (rpk) {
      unsigned char rb[4];
      hal_crypto_random((char *)rb, 4);
      static const char hx[] = "0123456789abcdef";
      for (int i = 0; i < 4; i++) { rmid[i * 2] = hx[rb[i] >> 4]; rmid[i * 2 + 1] = hx[rb[i] & 15]; }
      rmid[8] = 0;
      int k = 0; relaypt[k++] = '\x01';
      for (int i = 0; rmid[i]; i++) relaypt[k++] = rmid[i];
      relaypt[k++] = '\x02';
      for (int i = 0; text[i] && k < (int)sizeof(relaypt) - 1; i++) relaypt[k++] = text[i];
      relaypt[k] = 0;
      char ct[640];
      uint32_t cn = hal_encrypt(rpk, s_len(rpk), relaypt, s_len(relaypt), ct, sizeof(ct) - 1);
      if (cn > 0 && cn < sizeof(ct)) {
        ct[cn] = 0;
        s_cpy(core, "ENC1:", sizeof(core)); s_cat(core, ct, sizeof(core));
        encrypted = 1;
      }
    }
  }

  /* Sign (XPRS) when enabled OR when encrypted (encryption always carries a
   * signature). The signed body is word-split by the multi-line senders so the
   * 60-char signature lands on its own final APRS line; the receiver
   * reassembles and verifies. Likes are left unsigned. */
  char wire[800];
  s_cpy(wire, core, sizeof(wire));
  {
    char tgt[5]; int ul;
    if ((g_sign_msgs || encrypted) && !like_parse(text, tgt, &ul)) {
      char canon[720]; sig_canon(canon, sizeof(canon), g_call, core);
      char sg[80];
      uint32_t sn = hal_identity_sign(canon, s_len(canon), sg, sizeof(sg) - 1);
      if (sn > 0 && sn < sizeof(sg)) {
        sg[sn] = 0;
        s_cat(wire, " ~", sizeof(wire));
        s_cat(wire, sg, sizeof(wire));
      }
    }
  }
  /* If this message references a media file we host, append the BitTorrent
   * infohash (cleartext, unsigned) so the receiver can fetch it. The content
   * is still verified against the signed file: sha256 token. */
  add_infohash(wire, sizeof(wire));
  /* 1:1 receipts: stamp a correlation id (am:<6hex>) on the wire so the peer can
   * echo delivered/read back for WhatsApp-style ticks. Groups get no receipts.
   * PREPEND (not append): the message may end in a signature line ("~<60>") that
   * multi-line reassembly relies on being alone on the last line, and the sig is
   * computed over the (unmodified) core — an appended token would corrupt both.
   * The receiver strips am before verifying/decrypting. */
  char am[8] = "";
  if (id[0] != '#') {
    unsigned char rb[3]; hal_crypto_random((char *)rb, 3);
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 3; i++) { am[i*2] = hx[rb[i] >> 4]; am[i*2+1] = hx[rb[i] & 15]; }
    am[6] = 0;
    char w2[820];
    s_cpy(w2, "am:", sizeof(w2)); s_cat(w2, am, sizeof(w2)); s_cat(w2, " ", sizeof(w2));
    s_cat(w2, wire, sizeof(w2));
    s_cpy(wire, w2, sizeof(wire));
  }
  if (id[0] == '#') {
    /* Strip the scope marker: a global group "#NEWS*" transmits the same
     * "NEWS" bulletin as the local "#NEWS" — scope is only a local view. */
    char gname[8]; int gj = 0;
    for (int i = 1; id[i] && id[i] != '*' && gj < 6; i++) gname[gj++] = id[i];
    gname[gj] = 0;
    char bid[10]; bid[0] = '#'; s_cpy(bid + 1, gname, sizeof(bid) - 1);
    /* Primary: Reticulum broadcast — same compact frame as BLE, so the
     * receiver's ble_handle/deliver_bulletin path and content dedup treat it
     * identically to an APRS/BLE copy (see pkbeacon_send). */
    rns_tx_bulletin(bid, wire);
    if (g_ble_on) ble_tx_msg(bid, wire); /* compact BLE: to = "#group" (no scope) */
    /* Legacy APRS-IS (opt-in, licensed callsign only). */
    if (net) aprs_send_bulletin_multi(g_sock, g_call, gname, wire, APRS_MAX_MSG_LEN);
    /* Public group post → also store as our own NOSTR note (peers can request
     * it later). Not for 1:1 DMs, which are private. */
    {
      char tag[24]; group_tag(gname, tag, sizeof(tag));
      host_note_emit(text, tag, "");
    }
  } else if (priv) {
    /* Private: Reticulum first, and never APRS-IS (a 7-bit protocol mangles
     * ciphertext). BLE only as BEST HOPE — when Reticulum has nowhere to send,
     * airing the already-encrypted wire is the difference between a message
     * that waits for a custodian and a message that never leaves. The envelope
     * (from, to) is readable so a carrier knows whom it is holding for; the
     * body stays ENC1. */
    int sent = rns_tx_msg(id, wire);
    /* Air the already-encrypted wire too: the envelope (from, to) is readable
     * so a carrier knows whom it holds this for, the body stays ENC1. Whether
     * anyone needs to CARRY it is the core's call — MeshCourier parks what it
     * hears and hands it on (docs/mesh.md §6). */
    if (g_ble_on) ble_tx_msg(id, wire);
    if (sent <= 0 && !g_ble_on) {
      notify("warning", "No Reticulum address for this contact yet");
      return; /* don't echo a private message that reached nobody */
    }
  } else {
    /* Primary: Reticulum — directed to every known device of the recipient plus
     * an encrypted-safe broadcast; store-and-forward holds it for an offline
     * peer. Copies arriving over more than one transport dedup on receipt. */
    rns_tx_public(id, wire);
    /* Air a copy only when Bluetooth can add something. A contact sitting on
     * the same LAN, or reachable over a hub, is already getting this message;
     * a second copy on the air only burns the single advertising slot. */
    if (g_ble_on && reach_class(id) != REACH_NET) ble_tx_msg(id, wire);
    /* Legacy APRS-IS (opt-in, licensed callsign only). Encrypted (ENC1) messages
     * are NEVER sent over APRS-IS: APRS is a 7-bit text protocol and mangles the
     * base64 ciphertext (multi-line reassembly + charset), so the recipient gets
     * an undecryptable "[encrypted - cannot decrypt]" copy alongside the good
     * RNS/BLE one. Only PUBLIC (plaintext) messages use APRS. */
    int seq0 = g_seq;
    if (net && !encrypted) aprs_send_message_multi(g_sock, g_call, id, wire, APRS_MAX_MSG_LEN, &g_seq);
    /* Map each APRS part-seq to this message's am so an incoming ack<seq> (the
     * standard APRS ack, APRSdroid included) marks the bubble delivered. */
    if (am[0]) for (int s = seq0; s < g_seq; s++) ackmap_add(s, am);
    /* Unknown recipient key: the message went out only as PUBLIC (unencrypted)
     * broadcast. Tell the user in-chat, and ask the NOSTR relays to resolve the
     * callsign→npub so we can ALSO queue an encrypted backup for later pickup. */
    if (!encrypted && id[0] != '#') {
      if (g_myrelay_n == 0) relay_pick();
      if (g_myrelay_n > 0) {
        if (pubnote_once(id))
          convo_sysnote(id, "Key unknown - sent unencrypted (no key for this "
                            "contact yet). Checking NOSTR relays to deliver privately too.");
        char rj[RELAY_MAX * 80 + 16]; relays_json(g_myrelay, g_myrelay_n, rj, sizeof(rj));
        hal_relay_resolve(id, s_len(id), rj, s_len(rj));
        pendsend_add(id, text);
      } else if (pubnote_once(id)) {
        convo_sysnote(id, "Key unknown - sent unencrypted (no key for this "
                          "contact yet; no relays reachable for a private backup).");
      }
    }
  }
  /* NOSTR-relay store-and-forward backup: also publish this DM (kind-4 NIP-04)
   * to up to 3 reachable relays and tell the peer where to poll. Only when
   * encrypted (we have the recipient's npub); the relay copy carries the same
   * rmid so it dedups against the direct copy above. */
  if (encrypted && id[0] != '#') {
    if (g_myrelay_n == 0) relay_pick();
    const char *np = pk_get(id);
    if (np && np[0]) {
      /* Publish where the RECIPIENT will look: their rendezvous set (host-
       * ranked by their npub); fall back to our own picks if none known. */
      char rdv[RELAY_MAX][72];
      int rn = relay_rendezvous(np, rdv, RELAY_MAX);
      if (rn > 0 || g_myrelay_n > 0) {
        relay_announce_to(id);
        char rj[RELAY_MAX * 80 + 16];
        if (rn > 0) relays_json(rdv, rn, rj, sizeof(rj));
        else relays_json(g_myrelay, g_myrelay_n, rj, sizeof(rj));
        hal_relay_dm_send(np, s_len(np), relaypt, s_len(relaypt),
                          rj, s_len(rj), rmid, s_len(rmid));
      }
    }
  }
  /* Stamp the local-echo bubble with its receipt id + "sent" tick. */
  if (am[0]) { s_cpy(g_send_rid, am, sizeof(g_send_rid)); s_cpy(g_send_status, "sent", sizeof(g_send_status)); }
  convo_deliver(id, "out", g_call, wire, text, "");
  g_send_rid[0] = 0; g_send_status[0] = 0;
  status(priv ? "TX (private/Reticulum)" : (loc ? "TX message + position" : "TX message"));
}

static void do_convo_send(const char *buf) {
  char id[40] = "", text[400] = "";
  jstr(buf, "conversations_convo", id, sizeof(id));
  jstr(buf, "conversations_input", text, sizeof(text));
  if (!id[0] || !text[0]) return;
  convo_send_core(buf, id, text);
}

/* Toggle private (Reticulum-only) mode for the open 1:1 conversation. Requires the
 * contact's npub (so the off-APRS traffic is encrypted to them). Auto-negotiates
 * by signalling the peer's devices over Reticulum (?PRIV1/?PRIV0) so their side
 * flips too. */
static void do_convo_private(const char *buf) {
  char id[40] = "";
  jstr(buf, "conversations_convo", id, sizeof(id));
  if (!id[0] || id[0] == '#') return;       /* 1:1 only */
  peer_note(id);                             /* opting in is an interaction: promote a key
                                                we only overheard (parked) over RNS/APRS */
  int on = !convo_is_private(id);            /* the button toggles current state */
  if (on && !pk_get(id)) {
    notify("warning", "No Reticulum key for this contact yet");
    return;
  }
  cpriv_set(id, on);                          /* persists + emits the lock badge */
  rns_tx_msg(id, on ? "?PRIV1" : "?PRIV0");  /* auto-negotiate the peer (best effort) */
  status(on ? "Private mode ON (only internet)" : "Private mode OFF");
}

/* Change the coverage radius: re-filter by reconnecting APRS-IS, and
 * drop the old area's pins/geo-chat so only the new area shows. */
static void do_set_radius(const char *buf) {
  read_config(buf);
  char v[16];
  if (jstr(buf, "map_radius", v, sizeof(v)) && v[0]) g_radius = to_int(v);
  if (g_radius < 1) g_radius = 1;
  clear_area();
  push_radius();
  request_history();   /* reload archived Live messages for the new area */
  if (g_sock >= 0) {
    aprs_disconnect(g_sock);
    char host[64] = APRS_DEFAULT_HOST; int port = APRS_DEFAULT_PORT;
    if (jstr(buf, "server", v, sizeof(v)) && v[0]) s_cpy(host, v, sizeof(host));
    { char pv[16]; if (jstr(buf, "port", pv, sizeof(pv)) && pv[0]) port = to_int(pv); }
    g_logged = 0;
    g_sock = aprs_connect(host, port);
  }
  { char b[48] = "radius "; char nb[12]; int x = g_radius, j = 0, k = 0; char t[12];
    if (x == 0) t[j++] = '0'; while (x > 0) { t[j++] = (char)('0' + x % 10); x /= 10; }
    while (j > 0) nb[k++] = t[--j]; nb[k] = 0;
    s_cat(b, nb, sizeof(b)); s_cat(b, " km", sizeof(b)); status(b); }
}

/* Send a geo-chat: a position beacon carrying the typed comment. Rides
 * Reticulum (primary) + BLE; also APRS-IS when the legacy opt-in is on. */
static void do_geochat_send(const char *buf) {
  read_config(buf);
  int net = (g_sock >= 0 && g_logged);
  if (!net && !g_ble_on && !rns_up()) {
    notify("warning", "Enable Reticulum or Bluetooth first");
    return;
  }
  char text[400] = "";
  jstr(buf, "geochat_input", text, sizeof(text));
  if (!text[0]) return;
  /* Drop any leading ">>" the user typed; we add it to each chunk. */
  const char *body = text;
  if (body[0] == '>' && body[1] == '>') { body += 2; while (*body == ' ') body++; }
  if (!body[0]) return;
  /* Long geo-chat is sent as several position beacons, each comment chunk
   * prefixed ">>" so every part lands on the Live tab (here and on other
   * Aurora stations). Reserve 2 chars of the comment budget for ">>". */
  int avail = APRS_MAX_MSG_LEN - 2;
  char chunk[80];
  int n = 0;
  while (n < 12 && aprs_split_text(body, avail, n, chunk, sizeof(chunk))) {
    char tagged[80];
    s_cpy(tagged, ">>", sizeof(tagged));
    s_cat(tagged, chunk, sizeof(tagged));
    rns_tx_bulletin("", tagged);            /* primary: Reticulum broadcast */
    if (g_ble_on) ble_tx_msg("", tagged);   /* compact BLE: area/geo-chat text */
    if (net) aprs_send_beacon(g_sock, g_call, g_lat, g_lon, g_symbol, "TCPIP*", tagged);
    n++;
  }
  /* Local echo: the whole message as one Live bubble. */
  char echo[420];
  s_cpy(echo, ">>", sizeof(echo));
  s_cat(echo, body, sizeof(echo));
  /* Geo-tag our own message with our position so it is archived for this
   * area and reappears in the Live history later. */
  chat_append("geochat", "", "out", g_call, echo, "msg", 0, "", g_lat, g_lon, "");
  status("TX geo-chat");
}

/* Post a micro-update to the shared feed group (FEED): a Twitter-style status
 * that everyone following us sees in their Activity tab. Sent as a bulletin
 * (multi-line, optionally signed) over Reticulum + BLE (and legacy APRS-IS when
 * opted in), then echoed into our own Activity feed. */
static void do_activity_send(const char *buf) {
  read_config(buf);
  int net = (g_sock >= 0 && g_logged);
  if (!net && !g_ble_on && !rns_up()) {
    notify("warning", "Enable Reticulum or Bluetooth first");
    return;
  }
  char text[400] = "";
  jstr(buf, "activity_input", text, sizeof(text));
  if (!text[0]) return;
  /* Sign when enabled (the receiver reassembles + verifies the trailing line). */
  char wire[560]; s_cpy(wire, text, sizeof(wire));
  if (g_sign_msgs) {
    char canon[480]; sig_canon(canon, sizeof(canon), g_call, text);
    char sg[80];
    uint32_t sn = hal_identity_sign(canon, s_len(canon), sg, sizeof(sg) - 1);
    if (sn > 0 && sn < sizeof(sg)) { sg[sn] = 0; s_cat(wire, " ~", sizeof(wire)); s_cat(wire, sg, sizeof(wire)); }
  }
  /* Append the BitTorrent infohash if this post references media we host. */
  add_infohash(wire, sizeof(wire));
  rns_tx_bulletin("#" FEED_GROUP, wire);   /* primary: Reticulum broadcast */
  if (g_ble_on) ble_tx_msg("#" FEED_GROUP, wire);
  if (net) aprs_send_bulletin_multi(g_sock, g_call, FEED_GROUP, wire, APRS_MAX_MSG_LEN);
  /* Local echo of our own post (with a mid, so it can receive likes/replies). */
  activity_echo_self(text, "");
  /* Store the post as our own NOSTR note so peers can request it later. */
  host_note_emit(text, "activity", "");
  status("TX post");
}

/* Like / unlike an Activity post (a "<mid>:like" vote to the FEED group). */
static void do_activity_like(const char *buf) {
  read_config(buf);
  char mid[6] = ""; jstr(buf, "activity_mid", mid, sizeof(mid));
  if (!mid[0]) return;
  int unlike = jbool(buf, "activity_unlike");
  char wire[16]; s_cpy(wire, mid, sizeof(wire));
  s_cat(wire, unlike ? ":unlike" : ":like", sizeof(wire));
  rns_tx_bulletin("#" FEED_GROUP, wire);   /* primary: Reticulum broadcast */
  if (g_ble_on) ble_tx_msg("#" FEED_GROUP, wire);
  if (g_sock >= 0 && g_logged)
    aprs_send_bulletin_multi(g_sock, g_call, FEED_GROUP, wire, APRS_MAX_MSG_LEN);
  activity_react_emit(mid, g_call, !unlike, 1);   /* our own vote tallies now */
}

/* Reply to an Activity post: a threaded "+<mid> text" to the FEED group. */
static void do_activity_reply(const char *buf) {
  read_config(buf);
  int net = (g_sock >= 0 && g_logged);
  if (!net && !g_ble_on && !rns_up()) { notify("warning", "Enable Reticulum or Bluetooth first"); return; }
  char mid[6] = "", text[400] = "";
  jstr(buf, "activity_target_mid", mid, sizeof(mid));
  jstr(buf, "activity_input", text, sizeof(text));
  if (!mid[0] || !text[0]) return;
  char wire[480] = "+"; s_cat(wire, mid, sizeof(wire));
  s_cat(wire, " ", sizeof(wire)); s_cat(wire, text, sizeof(wire));
  add_infohash(wire, sizeof(wire));
  rns_tx_bulletin("#" FEED_GROUP, wire);   /* primary: Reticulum broadcast */
  if (g_ble_on) ble_tx_msg("#" FEED_GROUP, wire);
  if (net) aprs_send_bulletin_multi(g_sock, g_call, FEED_GROUP, wire, APRS_MAX_MSG_LEN);
  activity_echo_self(text, mid);       /* our reply, threaded under its parent */
  host_note_emit(text, "activity", mid); /* note carries the parent for backfill */
  status("TX reply");
}

/* Prompt to follow a callsign. */
static void prompt_follow(void) {
  const char *m = "{\"type\":\"ui.prompt\",\"id\":\"follow\",\"title\":\"Follow a callsign\","
    "\"body\":\"Enter a callsign to follow. Their posts, replies, likes and status "
    "will appear in your Activity feed.\","
    "\"input\":{\"hint\":\"Callsign e.g. N0CALL\",\"max\":15},\"confirm\":\"Follow\"}";
  hal_msg_send(m, s_len(m));
}
/* Prompt to unfollow: chips of the currently-followed callsigns. */
static void prompt_unfollow(void) {
  if (g_follow_n == 0) { notify("info", "You aren't following anyone yet"); return; }
  char m[900] = "{\"type\":\"ui.prompt\",\"id\":\"unfollow\",\"title\":\"Unfollow\","
                "\"body\":\"Pick a callsign to stop following.\",\"chips\":[";
  for (int i = 0; i < g_follow_n; i++) {
    if (i) s_cat(m, ",", sizeof(m));
    s_cat(m, "{\"label\":\"", sizeof(m)); jesc(m, sizeof(m), g_follow[i]);
    s_cat(m, "\",\"value\":\"", sizeof(m)); jesc(m, sizeof(m), g_follow[i]);
    s_cat(m, "\"}", sizeof(m));
  }
  s_cat(m, "],\"chipMode\":\"select\",\"confirm\":\"Unfollow\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* Transmit one recurring bulletin. [echo] shows it once in our own room (only on
 * the first send); the periodic re-broadcasts transmit silently so our view
 * doesn't fill with copies (receivers dedup the repeats). */
static void recur_broadcast(recur_t *r, int echo) {
  char convo[40];
  convo[0] = '#'; int j = 1;
  for (int i = 0; r->group[i] && j < 39; i++) convo[j++] = r->group[i];
  convo[j] = 0;
  /* Primary: Reticulum broadcast (same compact frame as BLE — receivers dedup). */
  rns_tx_bulletin(convo, r->text);
  if (g_ble_on) ble_tx_msg(convo, r->text);
  /* Legacy APRS-IS (opt-in, licensed callsign only). */
  if (g_sock >= 0 && g_logged)
    aprs_send_bulletin_multi(g_sock, g_call, r->group, r->text, APRS_MAX_MSG_LEN);
  if (echo) convo_deliver(convo, "out", g_call, r->text, r->text, "");
}

/* Begin a recurring bulletin into [group] (re-broadcast every 5 min for
 * [secs], first one now). Reuses the slot for the same group if present. */
static void recur_begin(const char *group, const char *text, int secs) {
  int net = (g_sock >= 0 && g_logged);
  if (!net && !g_ble_on && !rns_up()) {
    notify("warning", "Enable Reticulum, Bluetooth or APRS-IS first"); return;
  }
  if (!group[0] || !text[0]) { notify("warning", "Pick a group and message"); return; }
  if (secs < RECUR_INTERVAL) secs = RECUR_INTERVAL;
  if (secs > 172800) secs = 172800;             /* 48h cap */
  int slot = -1;
  for (int i = 0; i < RECUR_MAX; i++) {
    if (g_recur[i].active) {
      int same = 1;
      for (int k = 0; group[k] || g_recur[i].group[k]; k++)
        if (up(group[k]) != g_recur[i].group[k]) { same = 0; break; }
      if (same) { slot = i; break; }
    } else if (slot < 0) slot = i;
  }
  if (slot < 0) { notify("warning", "Too many recurring messages"); return; }
  recur_t *r = &g_recur[slot];
  r->active = 1;
  int gi = 0; for (; group[gi] && gi < 5; gi++) r->group[gi] = up(group[gi]);
  r->group[gi] = 0;
  s_cpy(r->text, text, sizeof(r->text));
  uint64_t now = hal_time_epoch();
  r->end = now + (uint64_t)secs;
  r->last = now;
  recur_broadcast(r, 1);
  status("Recurring bulletin every 5 min");
  notify("info", "Recurring bulletin started");
}

/* Stop any recurring bulletin for [group]. */
static void recur_stop_group(const char *group) {
  for (int i = 0; i < RECUR_MAX; i++) {
    if (!g_recur[i].active) continue;
    int gmatch = 1;
    for (int k = 0; group[k] || g_recur[i].group[k]; k++)
      if (up(group[k]) != g_recur[i].group[k]) { gmatch = 0; break; }
    if (gmatch) { g_recur[i].active = 0; status("Recurring bulletin stopped"); }
  }
}
/* True if [group] (no '#') currently has an active recurring bulletin. */
static int recur_active_group(const char *group) {
  for (int i = 0; i < RECUR_MAX; i++) {
    if (!g_recur[i].active) continue;
    int gmatch = 1;
    for (int k = 0; group[k] || g_recur[i].group[k]; k++)
      if (up(group[k]) != g_recur[i].group[k]) { gmatch = 0; break; }
    if (gmatch) return 1;
  }
  return 0;
}

/* normalise to a 1-5 char uppercased alnum group name (no leading '#') */
static void norm_group(const char *src, char *out) {
  int j = 0; const char *p = src; if (*p == '#') p++;
  for (; *p && j < 5; p++) {
    char c = up(*p);
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) out[j++] = c;
  }
  out[j] = 0;
}

/* "+" add-group: ask the host to show the preset/custom group picker. */
static const char *PRESET_GROUPS[] = {
  "ALL", "DEV", "MISC", "TECH", "FUN", "WARN", "INFO", "NEWS", "TRADE",
  "WX", "EMCOM", "ARES", "NET", "DX", "EVENT", "HELP", "SOS",
  "CHILL", "MEMES", "HELLO", "BIZ", "SPAM",
  /* 4chan-style boards */
  "B", "POL", "FIN", "G"
};
static void prompt_group(void) {
  char chips[1000] = "";
  for (unsigned i = 0; i < sizeof(PRESET_GROUPS) / sizeof(PRESET_GROUPS[0]); i++) {
    if (i) s_cat(chips, ",", sizeof(chips));
    s_cat(chips, "{\"label\":\"#", sizeof(chips));
    s_cat(chips, PRESET_GROUPS[i], sizeof(chips));
    s_cat(chips, "\",\"value\":\"", sizeof(chips));
    s_cat(chips, PRESET_GROUPS[i], sizeof(chips));
    s_cat(chips, "\"}", sizeof(chips));
  }
  char m[1600] = "{\"type\":\"ui.prompt\",\"id\":\"group\",\"fullscreen\":true,"
                 "\"title\":\"Add a group\","
                 "\"body\":\"Pick or type a group (max 5 letters). Global follows it "
                 "worldwide; local follows it only within your radius.\",\"chips\":[";
  s_cat(m, chips, sizeof(m));
  s_cat(m, "],\"chipMode\":\"select\",\"input\":{\"hint\":\"Custom\",\"max\":5,"
          "\"prefix\":\"#\"},"
          "\"toggle\":{\"label\":\"Global (worldwide)\",\"default\":true},"
          "\"confirm\":\"Add\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
static void prompt_newchat(void) {
  /* Full-screen panel; offer Private (Reticulum-only) from the start so a 1:1 can
   * begin off-APRS. The toggle comes back as prompt_toggle (ignored for #groups,
   * and only honoured when we already know the contact's npub).
   *
   * Below the field we list the stations currently reachable over BLE (heard
   * within REACH_WINDOW) as instant chips — one tap opens a 1:1 with them. */
  char chips[700];
  int nchips = ble_reach_chips(chips, sizeof(chips));
  char m[1400];
  s_cpy(m, "{\"type\":\"ui.prompt\",\"id\":\"newchat\",\"title\":\"New message\","
           "\"fullscreen\":true,\"body\":\"", sizeof(m));
  if (nchips > 0)
    s_cat(m, "Enter a callsign or #group, or tap a station heard over BLE below.",
          sizeof(m));
  else
    s_cat(m, "Enter a callsign for a 1:1 chat, or #group. "
             "(No stations heard over BLE yet.)", sizeof(m));
  s_cat(m, "\",", sizeof(m));
  if (nchips > 0) {
    s_cat(m, "\"chips\":[", sizeof(m));
    s_cat(m, chips, sizeof(m));
    s_cat(m, "],\"chipMode\":\"instant\",", sizeof(m));
  }
  s_cat(m, "\"input\":{\"hint\":\"Callsign or #group\",\"max\":20},"
           "\"toggle\":{\"label\":\"Private (only internet)\",\"default\":false},"
           "\"confirm\":\"Open\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
static void prompt_recur(const char *convo) {
  char m[700] = "{\"type\":\"ui.prompt\",\"id\":\"recur\",\"title\":\"Recurring bulletin\","
                "\"body\":\"Repeat every 5 min into ";
  jesc(m, sizeof(m), convo);
  s_cat(m, " until the period ends.\",\"chips\":["
          "{\"label\":\"1 hour\",\"value\":\"3600\"},"
          "{\"label\":\"2 hours\",\"value\":\"7200\"},"
          "{\"label\":\"4 hours\",\"value\":\"14400\"},"
          "{\"label\":\"8 hours\",\"value\":\"28800\"},"
          "{\"label\":\"1 day\",\"value\":\"86400\"},"
          "{\"label\":\"2 days\",\"value\":\"172800\"}],"
          "\"chipMode\":\"select\",\"input\":{\"hint\":\"Message to repeat\",\"max\":67},"
          "\"confirm\":\"Start\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* "+"/✎/↻ header actions from the conversations widget. */
static void do_new_chat(void) { prompt_newchat(); }
static void do_add_group(void) { prompt_group(); }
static void do_recur(const char *buf) {
  char id[40] = ""; jstr(buf, "conversations_convo", id, sizeof(id));
  if (id[0] != '#') { notify("info", "Recurring is for groups only"); return; }
  /* Toggle: a second tap on a group that already has a recurring bulletin stops
   * it (this used to be the pinned banner's dismiss button). */
  char g[8]; norm_group(id, g);
  if (recur_active_group(g)) { recur_stop_group(g); notify("info", "Recurring bulletin stopped"); return; }
  prompt_recur(id);
}

/* Local message actions from the chat bubble menu (host-driven, never on the
 * wire): hide one message, block / unblock a station. */
static void do_convo_hide(const char *buf) {
  char id[40] = "", key[16] = "";
  jstr(buf, "conversations_convo", id, sizeof(id));
  jstr(buf, "conversations_hidekey", key, sizeof(key));
  hide_add(id, key);
}
static void do_convo_block(const char *buf) {
  char c[16] = ""; jstr(buf, "conversations_blockcall", c, sizeof(c));
  if (!c[0]) return;
  block_add(c);
  notify("info", "Blocked — you won't see their messages");
}
/* Block / mute a callsign from the Activity feed's per-post menu. */
static void do_activity_block(const char *buf) {
  char c[16] = ""; jstr(buf, "activity_call", c, sizeof(c));
  if (!c[0]) return;
  block_add(c);
  notify("info", "Blocked — their messages are discarded");
}
static void do_activity_mute(const char *buf) {
  char c[16] = ""; jstr(buf, "activity_call", c, sizeof(c));
  if (!c[0]) return;
  mute_add(c);
}
/* Close a conversation: unsubscribe so we stop receiving its messages. For a
 * group we forget both the local (#NAME) and global (#NAME*) variants and
 * persist, so the APRS-IS filter drops it and deliver_bulletin no longer
 * delivers it. The host hides the row on its side. */
static void do_convo_close(const char *buf) {
  char id[40] = ""; jstr(buf, "conversations_convo", id, sizeof(id));
  if (!id[0]) return;
  convo_forget(id);
  if (id[0] == '#') {
    /* also forget the paired scope variant */
    char other[40];
    int n = s_len(id);
    if (n > 0 && id[n - 1] == '*') { s_cpy(other, id, sizeof(other)); other[n - 1] = 0; }
    else { s_cpy(other, id, sizeof(other)); if (n < 38) { other[n] = '*'; other[n + 1] = 0; } }
    convo_forget(other);
    groups_save();
    groups_subscribe();
  }
}

static char g_cur_room[80] = "";       /* the room whose members panel is open */
static char g_new_parent[80] = "";     /* parent for a room being created */
static char g_mod_target[80] = "";     /* member a moderation prompt targets */
static char g_appr_target[80] = "";    /* proposal id an approval prompt targets */

/* Result of a ui.prompt the host showed for us. */
static void do_prompt_result(const char *buf) {
  char pid[24] = "", val[40] = "", inp[80] = "";
  jstr(buf, "prompt_id", pid, sizeof(pid));
  jstr(buf, "prompt_value", val, sizeof(val));
  jstr(buf, "prompt_input", inp, sizeof(inp));
  if (s_eq(pid, "rmod")) {
    /* Moderation action on g_mod_target in g_cur_room (both set at tap time). */
    const char *rid = g_cur_room, *tp = g_mod_target;
    long now = (long)hal_time_epoch();
    if (s_eq(val, "award")) room_moderate(rid, "award", tp, 0, 5, "");
    else if (s_eq(val, "deduct")) room_moderate(rid, "deduct", tp, 0, 5, "");
    else if (s_eq(val, "suspend")) room_moderate(rid, "suspend", tp, now + 86400, 0, "");
    else if (s_eq(val, "unsuspend")) room_moderate(rid, "unsuspend", tp, 0, 0, "");
    else if (s_eq(val, "kick")) room_moderate(rid, "kick", tp, 0, 0, "");
    else if (s_eq(val, "ban")) room_moderate(rid, "ban", tp, 0, 0, "");
    else if (s_eq(val, "banwapp")) room_ban_wapp(tp);
    if (val[0]) {
      notify("info", "Moderation action sent");
      if (g_cur_room[0]) room_render_members(g_cur_room);
    }
    return;
  }
  if (s_eq(pid, "newroom")) {
    if (inp[0]) {
      const char *par = g_new_parent[0] ? g_new_parent : "main";
      if (room_propose(par, inp)) {
        if (room_self_authority(par)) notify("info", "Creating the room...");
        else notify("info", "Room requested — waiting for a moderator to approve");
      }
    }
    return;
  }
  if (s_eq(pid, "rappr")) {
    if (s_eq(val, "approve") && g_appr_target[0]) {
      if (room_approve(g_appr_target)) notify("info", "Approved — creating the room");
      else notify("warning", "Couldn't approve (not authorised?)");
    }
    g_appr_target[0] = 0;
    return;
  }
  if (s_eq(pid, "newchat")) {
    /* Typed text wins; otherwise a tapped reachable-station chip (its callsign
     * arrives in prompt_value). */
    const char *src = inp[0] ? inp : val;
    if (src[0] == '#') {
      char g[8]; norm_group(src, g);
      if (g[0]) { char id[10]; id[0] = '#'; s_cpy(id + 1, g, sizeof(id) - 1); convo_touch(id, "", 1); }
    } else if (src[0]) {
      char id[24]; int j = 0; for (int i = 0; src[i] && j < 23; i++) id[j++] = up(src[i]); id[j] = 0;
      convo_touch(id, "", 1);
      /* Start private straight away if the user asked and we already hold the
       * contact's npub (promote a parked key first). Otherwise open normally and
       * note that private needs the key — they can toggle it once it arrives. */
      if (jbool(buf, "prompt_toggle")) {
        peer_note(id);
        if (pk_get(id)) { cpriv_set(id, 1); rns_tx_msg(id, "?PRIV1"); }
        else notify("warning", "Opened — Private needs this contact's Reticulum key first");
      }
    }
  } else if (s_eq(pid, "group")) {
    char g[8]; norm_group(val[0] ? val : inp, g);
    if (g[0]) {
      char id[12]; id[0] = '#'; s_cpy(id + 1, g, sizeof(id) - 1);
      if (jbool(buf, "prompt_toggle")) s_cat(id, "*", sizeof(id));   /* global */
      convo_touch(id, "", 1);
      groups_save();
      groups_subscribe();
    }
  } else if (s_eq(pid, "recur")) {
    char id[40] = ""; jstr(buf, "conversations_convo", id, sizeof(id));
    if (id[0] == '#' && inp[0]) recur_begin(id + 1, inp, to_int(val));
  } else if (s_eq(pid, "follow")) {
    if (inp[0]) follow_add(inp);
  } else if (s_eq(pid, "unfollow")) {
    if (val[0]) follow_remove(val);
  } else if (pid[0]=='p'&&pid[1]=='r'&&pid[2]=='o'&&pid[3]=='f'&&pid[4]==':') {
    /* Profile sheet action for pid "prof:<CALL>". */
    const char *call = pid + 5;
    if (s_eq(val, "follow")) follow_add(call);
    else if (s_eq(val, "unfollow")) follow_remove(call);
    else if (s_eq(val, "tags")) prompt_ftag(call);
    else if (s_eq(val, "block")) { block_add(call); notify("info", "Blocked — you won't see their messages"); }
    else if (s_eq(val, "unblock")) { block_remove(call); notify("info", "Unblocked"); }
  } else if (pid[0]=='f'&&pid[1]=='t'&&pid[2]=='a'&&pid[3]=='g'&&pid[4]==':') {
    ftag_set(pid + 5, inp);    /* empty input clears the tags */
  }
}

/* Parse one TNC2 line and route it to the UI; bridge across transports when
 * relaying is on. via_ble = 1 if the frame arrived over BLE, 0 over APRS-IS.
 * A raw-frame dedup makes a message heard on both transports show once and
 * guards the relay against loops. */
/* ── Compact BLE frame ──────────────────────────────────────────────────
 * BLE legacy advertising only fits ~31 bytes, far less than a TNC2 frame, so
 * over BLE we use a compact form: "<from>\x1f<to>\x1f<text>" where `to` is a
 * callsign (1:1), "#GRP" (group), "!" (position; text = "lat,lon[,comment]"),
 * or "" (area/geo-chat text). Receivers (incl. ESP32) reconstruct routing
 * from these fields. The HAL just carries the bytes. */
#define BLE_SEP '\x1f'

/* Build the frame chat airs.
 *
 * XPRS (docs/XPRS.md) is what the device speaks now, so that is what goes out:
 * the same content as `key:value` fields any XPRS station can read, including
 * one that has never heard of this wapp. What chat puts INSIDE `m:` — an ENC1
 * ciphertext, a ~signature, a reply marker — is untouched.
 *
 * The compact form stays as the fallback for the frames XPRS has no words for
 * yet (?MAIL, ?IGATE, ?PING) and for anything that would not fit, and every
 * receiver still reads both, so a device on the old build keeps working. */
static void ble_pack(char *out, unsigned max, const char *from,
                     const char *to, const char *text) {
  /* 250 bytes is the XPRS limit (section 4), not our buffer's: a longer body
   * is not an XPRS packet at all, and section 6.6 (parts) is the answer to it
   * rather than a packet nobody is required to accept. Until that is wired,
   * a long message keeps the compact frame, which the Reticulum path carries
   * whole. */
  unsigned cap = max > 251 ? 251 : max;
  if (xprs_pack(out, cap, from, to, text, hal_time_epoch())) return;
  char sep[2] = { BLE_SEP, 0 };
  out[0] = 0;
  s_cat(out, from, max); s_cat(out, sep, max);
  s_cat(out, to, max);   s_cat(out, sep, max);
  s_cat(out, text, max);
}
static void ble_tx_from(const char *from, const char *to, const char *text) {
  if (!g_ble_on) return;
  char buf[220];
  ble_pack(buf, sizeof(buf), from, to, text);
  fseen_add(sig_hash("b", "", buf));   /* don't re-handle our own advert */
  ble_send(buf);
}
static void ble_tx_msg(const char *to, const char *text) {
  ble_tx_from(g_call, to, text);
}

/* ---- The best-hope wire ------------------------------------------------
 *
 * A message handed to strangers to carry needs three things the normal path
 * does not: an address that cannot be spoofed, a body they cannot read, and a
 * receipt id they can key on. A Reticulum delivery hash is none of these — it
 * is ephemeral and means nothing outside Reticulum. The npub is the constant,
 * and the callsign is derived from it, so:
 *
 *   from \x1F to \x1F  np:<npub> am:<6hex> ENC1:<ciphertext> ~<sig>
 *
 * `to` (callsign) is what a custodian stores and matches on — short, and enough
 * to recognise the recipient. `np:` is the recipient's npub, so the person who
 * receives it can prove the mail is genuinely addressed to their identity and
 * not to somebody who took their callsign. The body is encrypted to that same
 * npub, and the signature proves who sent it. Everything a carrier needs is
 * public; nothing it must not read is.
 *
 * Returns 0 when we cannot build one — no key for that npub, or the result
 * would not fit a carrier's frame.
 */
/* Who is behind an LXMF delivery dest? The dest is Reticulum's ephemeral
 * routing handle; the npub behind it is the identity, and the callsign is
 * derived from that npub. Both come from the beacons we already keep
 * (rns_dest_store fills npub -> dest; the pubkey store fills callsign -> npub),
 * so this is a lookup, not a network call. Returns 0 when we do not know the
 * identity — in which case there is no spoof-proof way to address custody and
 * we simply do not air one. */
static void ble_tx_pos(double lat, double lon, const char *comment) {
  char t[96] = "";
  append_dbl(t, sizeof(t), lat); s_cat(t, ",", sizeof(t));
  append_dbl(t, sizeof(t), lon);
  if (comment && comment[0]) { s_cat(t, ",", sizeof(t)); s_cat(t, comment, sizeof(t)); }
  ble_tx_msg("!", t);
}

/* Is the local Reticulum node up? Probes hal_rns_delivery_dest (returns 0 while
 * the node is down — same check pkbeacon_send uses). Cached for one second so
 * the per-tick status indicator and the send gates don't hammer the HAL. */
static int rns_up(void) {
  static uint64_t probed = 0;
  static int up = 0;
  uint64_t now = hal_time_epoch();
  if (!probed || now != probed) {
    char d[80];
    up = hal_rns_delivery_dest(d, sizeof(d) - 1) > 0;
    probed = now;
  }
  return up;
}

/* Broadcast a group bulletin / geo-chat frame over Reticulum. The frame reuses
 * the compact BLE format, so the receiver's RNS drain feeds it into ble_handle
 * and it dedups against BLE/APRS copies of the same content. */
static void rns_tx_bulletin(const char *to, const char *text) {
  if (!rns_up()) return;
  char frame[900];
  ble_pack(frame, sizeof(frame), g_call, to, text);
  hal_rns_broadcast(frame, s_len(frame));
}

/* Manual/emergency position beacons over the licence-free transports: BLE (local
 * radio) and a Reticulum broadcast (crosses NATs via the hubs). Automatic
 * interval beacons deliberately stay off RNS to limit hub flood traffic. */
static void pos_broadcast(double lat, double lon, const char *comment) {
  char t[96] = "";
  append_dbl(t, sizeof(t), lat); s_cat(t, ",", sizeof(t));
  append_dbl(t, sizeof(t), lon);
  if (comment && comment[0]) { s_cat(t, ",", sizeof(t)); s_cat(t, comment, sizeof(t)); }
  if (g_ble_on) ble_tx_msg("!", t);
  rns_tx_bulletin("!", t);
}

/* Send a 1:1 over Reticulum to EVERY device advertising the recipient's npub
 * (multi-device). Reuses the BLE frame format so the receiver's ble_handle +
 * content dedup treat it identically to an APRS/BLE copy — a message that also
 * arrived over APRS-IS/BLE is shown once. [wire] is already ENC1-encrypted to the
 * npub when known, so a wrong/forged/stale dest gets an undecryptable blob.
 * Returns the number of devices it queued to (0 = no key/dest → no RNS path). */
static int rns_tx_msg(const char *to, const char *wire) {
  const char *npub = pk_get(to);
  if (!npub || !npub[0]) return 0;
  char frame[900];
  ble_pack(frame, sizeof(frame), g_call, to, wire);
  uint64_t now = hal_time_epoch();
  int sent = 0;
  /* Directed delivery to each of the recipient's known devices — best for
   * privacy and works when a direct LXMF path/link can be established. */
  for (int i = 0; i < g_rns_n; i++) {
    if (!s_eq(g_rns_npub[i], npub)) continue;
    if (g_rns_dts[i] && now - g_rns_dts[i] > RNS_TTL) continue;   /* stale device */
    if (hal_rns_send_to(g_rns_dest[i], s_len(g_rns_dest[i]), frame, s_len(frame)) == 1) sent++;
  }
  /* Reliable cross-network backstop: also flood the frame as a Reticulum
   * broadcast. Broadcasts are announce-relayed by the public hubs, so they reach
   * a peer behind NAT on a different network where a direct LXMF link to its
   * delivery dest can't be opened. Safe to flood: the body is ENC1-encrypted to
   * the recipient's npub (only they can read it) and only the addressed callsign
   * handles it as a 1:1 — every other node drops it. The receiver dedups this
   * against the directed copy by content hash, so it still shows once. */
  if (hal_rns_broadcast(frame, s_len(frame)) == 1) sent++;
  return sent;
}

/* PUBLIC 1:1 send. Same as rns_tx_msg, except that a recipient whose pubkey we
 * have never heard still gets the frame — as a plaintext Reticulum broadcast,
 * which is what the message already is when there is no key to encrypt it to.
 *
 * rns_tx_msg returns 0 the moment pk_get() comes up empty, BEFORE the broadcast
 * backstop, so writing to a contact whose pubkey beacon had not arrived yet
 * transmitted NOTHING: no directed copy, no broadcast, and with BLE and APRS-IS
 * off (the normal case) the message left the device by no path at all while the
 * UI happily echoed it into the thread. Private conversations keep the strict
 * rule — they must never fall back to plaintext — so they still call
 * rns_tx_msg directly. */
static int rns_tx_public(const char *to, const char *wire) {
  int sent = rns_tx_msg(to, wire);
  if (sent > 0) return sent;
  const char *npub = pk_get(to);
  if (npub && npub[0]) return sent;   /* key known: rns_tx_msg already broadcast */
  char frame[900];
  ble_pack(frame, sizeof(frame), g_call, to, wire);
  return hal_rns_broadcast(frame, s_len(frame)) == 1 ? 1 : 0;
}

/* ── Store-and-forward: BLE iGate mailbox for heard stations ──────────────
 * When this station is online (APRS-IS up) it acts as an iGate for nearby
 * BLE-only stations: it remembers the callsigns it hears over BLE in a
 * persistent registry (<=100, 1-year LRU), adds them to the APRS-IS `g/`
 * filter so the server pushes messages addressed to them, and holds those
 * messages in a per-callsign mailbox. A BLE-only station pulls its mail by
 * broadcasting "?MAIL <call>" every 5 min while an iGate (?IGATE beacon) is in
 * reach; we reply with each held message and clear the mailbox. No UI. */
#define MAIL_TO       "?MAIL"
#define IGATE_TO      "?IGATE"
#define HELLO_TO      "?HELLO"               /* lightweight BLE presence beacon */
#define PRESENCE_INTERVAL 30                 /* re-announce presence every 30 s */
#define REACH_WINDOW  180                    /* "reachable now" = heard within 3 min */
#define SDEV_MAX      100
#define SDEV_TTL      (365ULL * 24 * 3600)   /* 1 year */
#define GFILTER_CAP   30                      /* heard calls put in the g/ filter */

typedef struct { char call[12]; uint64_t ts; } sdev_t;
static sdev_t g_sdev[SDEV_MAX];
static int g_sdev_n = 0;
static int g_sdev_dirty = 0;

static uint64_t g_last_igate_heard  = 0;   /* we (client) heard an iGate beacon */
static uint64_t g_last_igate_beacon = 0;   /* we (iGate) last announced ourselves */
static uint64_t g_last_mail_query   = 0;
static uint64_t g_last_filter_check = 0;
static uint64_t g_sdev_saved        = 0;
static char g_gfilter[600] = "";           /* g/ + b/ extra filter currently in use */

static int sdev_find(const char *c) {
  for (int i = 0; i < g_sdev_n; i++) if (s_eq(g_sdev[i].call, c)) return i;
  return -1;
}
static int sdev_has(const char *c) { return sdev_find(c) >= 0; }
static void mailbox_clear(const char *call);   /* fwd */

/* Remember a callsign heard over BLE (not us): refresh its timestamp, add it,
 * or evict the least-recently-seen when full. */
static void sdev_touch(const char *c) {
  if (!c || !c[0] || s_eq(c, g_call)) return;
  uint64_t now = hal_time_epoch();
  int i = sdev_find(c);
  if (i >= 0) { g_sdev[i].ts = now; g_sdev_dirty = 1; return; }
  if (g_sdev_n < SDEV_MAX) { i = g_sdev_n++; }
  else {
    int lru = 0;
    for (int k = 1; k < g_sdev_n; k++) if (g_sdev[k].ts < g_sdev[lru].ts) lru = k;
    mailbox_clear(g_sdev[lru].call);
    i = lru;
  }
  s_cpy(g_sdev[i].call, c, sizeof(g_sdev[i].call));
  g_sdev[i].ts = now; g_sdev_dirty = 1;
  /* (No "spotted on Bluetooth" Activity entry — it fired for plain iGated
   * traffic too and was just noise. The Activity feed is messages only.) */
}

static void sdev_save(void) {
  char buf[1800]; buf[0] = 0;
  for (int i = 0; i < g_sdev_n; i++) {
    s_cat(buf, g_sdev[i].call, sizeof(buf)); s_cat(buf, ",", sizeof(buf));
    char tb[20]; u_itoa((unsigned)g_sdev[i].ts, tb); s_cat(buf, tb, sizeof(buf));
    s_cat(buf, ";", sizeof(buf));
  }
  hal_kv_set("seendev", 7, buf, s_len(buf));
}
static void sdev_load(void) {
  char buf[1800];
  uint32_t n = hal_kv_get("seendev", 7, buf, sizeof(buf) - 1);
  if (n == 0) return;
  buf[n] = 0;
  uint64_t now = hal_time_epoch();
  g_sdev_n = 0;
  char *p = buf;
  while (*p && g_sdev_n < SDEV_MAX) {
    char call[12]; int ci = 0;
    while (*p && *p != ',' && ci < 11) call[ci++] = *p++;
    call[ci] = 0;
    if (*p == ',') p++;
    uint64_t ts = 0;
    while (*p >= '0' && *p <= '9') { ts = ts * 10 + (uint64_t)(*p - '0'); p++; }
    if (*p == ';') p++;
    if (call[0] && (now < SDEV_TTL || ts >= now - SDEV_TTL)) {   /* prune >1yr */
      s_cpy(g_sdev[g_sdev_n].call, call, sizeof(g_sdev[g_sdev_n].call));
      g_sdev[g_sdev_n].ts = ts; g_sdev_n++;
    }
  }
}

/* Build chips of callsigns heard over BLE within REACH_WINDOW, most-recent
 * first, capped to fit [out]. Each chip is {"label":"CALL","value":"CALL"}.
 * Returns the number written; used by the "New message" prompt to offer the
 * locally-reachable stations. */
#define REACH_CHIPS_MAX 12
/* A callsign safe to show/route: 1..15 printable callsign chars only. Rejects
 * empties and any malformed entry carrying control/separator bytes (those would
 * also break the prompt JSON). */
static int valid_call(const char *c) {
  if (!c || !c[0]) return 0;
  int n = 0;
  for (const char *p = c; *p; p++, n++) {
    if (n >= 15) return 0;
    char ch = *p;
    int ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
             (ch >= '0' && ch <= '9') || ch == '-' || ch == '/';
    if (!ok) return 0;
  }
  return 1;
}
static int ble_reach_chips(char *out, unsigned max) {
  out[0] = 0;
  uint64_t now = hal_time_epoch();
  /* Collect recent indices, then selection-sort by ts (newest first). A copy of
   * the timestamps lets us mark picked entries without touching the registry. */
  int idx[SDEV_MAX]; uint64_t ts[SDEV_MAX]; int n = 0;
  for (int i = 0; i < g_sdev_n; i++) {
    if (now >= REACH_WINDOW && g_sdev[i].ts < now - REACH_WINDOW) continue;
    if (s_eq(g_sdev[i].call, g_call)) continue;
    if (!valid_call(g_sdev[i].call)) continue;   /* skip malformed entries */
    idx[n] = i; ts[n] = g_sdev[i].ts; n++;
  }
  int written = 0;
  for (int k = 0; k < n && written < REACH_CHIPS_MAX; k++) {
    int best = -1; uint64_t bts = 0;
    for (int j = 0; j < n; j++) {
      if (ts[j] == (uint64_t)-1) continue;          /* already taken */
      if (best < 0 || ts[j] > bts) { best = j; bts = ts[j]; }
    }
    if (best < 0) break;
    ts[best] = (uint64_t)-1;
    const char *call = g_sdev[idx[best]].call;
    if (written) s_cat(out, ",", max);
    s_cat(out, "{\"label\":\"", max); jesc(out, max, call);
    s_cat(out, "\",\"value\":\"", max); jesc(out, max, call);
    s_cat(out, "\"}", max);
    written++;
  }
  return written;
}

/* g/ extra filter: our own call, the most-recently-seen stations (so APRS-IS
 * pushes their direct messages), and the bulletin addressee pattern for every
 * GLOBAL group we subscribe to (id ending in '*') so we hear that group
 * worldwide. Local groups (no '*') need nothing extra — the always-on r/ range
 * filter already brings in-radius bulletins. */
static void build_gfilter(char *out, unsigned max) {
  out[0] = 0;
  s_cat(out, "g/", max); s_cat(out, g_call, max);
  int used[SDEV_MAX]; for (int i = 0; i < g_sdev_n; i++) used[i] = 0;
  int cnt = g_sdev_n < GFILTER_CAP ? g_sdev_n : GFILTER_CAP;
  for (int k = 0; k < cnt; k++) {
    int best = -1;
    for (int i = 0; i < g_sdev_n; i++)
      if (!used[i] && (best < 0 || g_sdev[i].ts > g_sdev[best].ts)) best = i;
    if (best < 0) break;
    used[best] = 1;
    s_cat(out, "/", max); s_cat(out, g_sdev[best].call, max);
  }
  /* Any GLOBAL group (#NAME*) → pull bulletins worldwide. APRS-IS g/ only
   * supports a trailing '*' (no mid-string wildcard, verified live), and a
   * bulletin's addressee is "BLN<id><GROUP>", so we can't match a specific
   * group server-side. Bulletin volume is tiny (a few per minute globally), so
   * one catch-all "g/BLN*" is fine; deliver_bulletin() then files only the
   * groups we actually subscribed to. */
  int bln_all = 0;
  for (int i = 0; i < g_convo_n; i++) {
    const char *id = g_convo_ids[i];
    unsigned L = s_len(id);
    if (id[0] == '#' && L >= 3 && id[L - 1] == '*') { s_cat(out, "/BLN*", max); bln_all = 1; break; }
  }
  /* Always pull the Activity stream (FEED bulletins) so posts from others show
   * up even without subscribing to any global group. The line id varies 0-9
   * (multi-line) and sits mid-addressee where g/ has no wildcard, so add each
   * BLN<0-9>FEED explicitly. (Skipped when the BLN* catch-all is already on.) */
  if (!bln_all) {
    for (char d = '0'; d <= '9'; d++) {
      char e[10] = "/BLN0FEED"; e[4] = d; s_cat(out, e, max);
    }
  }
  /* Followed stations: a b/ budlist pulls EVERY packet FROM them (posts,
   * replies, likes, status) regardless of group, so their Activity stream
   * arrives even for groups we don't subscribe to. */
  if (g_follow_n) {
    s_cat(out, " b", max);
    for (int i = 0; i < g_follow_n; i++) { s_cat(out, "/", max); s_cat(out, g_follow[i], max); }
  }
}
/* True if any global group (#NAME*) is subscribed — i.e. g/BLN* is active and
 * worldwide bulletins are arriving, so a local group must verify proximity. */
static int any_global_group(void) {
  for (int i = 0; i < g_convo_n; i++) {
    const char *id = g_convo_ids[i];
    unsigned L = s_len(id);
    if (id[0] == '#' && L >= 3 && id[L - 1] == '*') return 1;
  }
  return 0;
}

static void convo_ensure(const char *id);   /* defined below */
static void groups_subscribe(void);         /* defined below */

/* ── Groups over NOSTR ──────────────────────────────────────────────────────
 * A group message IS a NOSTR note: kind-1 tagged with the group name (the host
 * publishes it that way already, see host_note_emit -> social.note). What was
 * missing was the other half — LISTENING. We subscribe to kind-1 carrying a `t`
 * tag for any group we are in, so a note posted from ANY NOSTR client (not just
 * xprs) shows up in the group.
 *
 * Dedup on the event id: the same note reaches us as a NOSTR event AND as an
 * APRS/BLE/Reticulum bulletin, and it must appear once.
 */
/* The NOSTR tag a group's notes carry.
 *
 * NOT the bare group name. "NEWS", "DEV" and "HELP" are among the most-used
 * hashtags on the public relays, so subscribing to t:NEWS subscribed this phone
 * to the WORLD'S #news firehose — the #NEWS group filled with strangers' spam
 * within minutes of shipping it (observed on-device: 22 messages, none of them
 * ours). A group is a xprs room, not a global hashtag, so it gets its own
 * namespace. Notes are still ordinary kind-1 events any NOSTR client can read;
 * they just carry #xprs-NEWS rather than #NEWS. */
#define GROUP_TAG_PREFIX "xprs-"
static void group_tag(const char *gname, char *out, unsigned cap) {
  s_cpy(out, GROUP_TAG_PREFIX, cap);
  s_cat(out, gname, cap);
}

/* The conversation id for a group NAME, preferring the row we already have.
 *
 * A group is seeded as "#NEWS*" (global scope) but the name on the wire is bare
 * "NEWS". Building "#NEWS" from it created a SECOND, local row alongside the
 * subscribed global one — two groups with the same name, one of them a ghost. */
static void group_convo_id(const char *gname, char *out, unsigned cap) {
  for (int i = 0; i < g_convo_n; i++) {
    const char *id = g_convo_ids[i];
    if (!is_group(id)) continue;
    int k = 1;
    while (gname[k - 1] && id[k] && id[k] != '*' && id[k] == gname[k - 1]) k++;
    if (!gname[k - 1] && (id[k] == 0 || id[k] == '*')) {
      s_cpy(out, id, cap);   /* the existing row, scope marker and all */
      return;
    }
  }
  s_cpy(out, "#", cap);
  s_cat(out, gname, cap);
}

static char g_sub_groups[64] = "";
static char g_gseen[64][20];      /* event ids already rendered */
static int  g_gseen_n = 0, g_gseen_w = 0;

/* PERSISTENT, and it has to be.
 *
 * hal_lxmf_recv is a cursor over the HOST's durable inbox, and the cursor is
 * per-engine: it starts at 0 every time an engine starts. Opening the wapp's
 * page starts a fresh engine, so it re-reads the entire inbox and re-emits every
 * message that was ever delivered — the same LXMF message appeared five and six
 * times over (observed on-device). An in-memory seen-ring cannot stop that,
 * because it dies with the engine that held it. */
static int gseen_has(const char *id) {
  for (int i = 0; i < g_gseen_n; i++) if (s_eq(g_gseen[i], id)) return 1;
  return 0;
}
static void gseen_save(void) {
  static char buf[64 * 21];
  buf[0] = 0;
  for (int i = 0; i < g_gseen_n; i++) {
    s_cat(buf, g_gseen[i], sizeof(buf));
    s_cat(buf, ";", sizeof(buf));
  }
  hal_kv_set("gseen", 5, buf, s_len(buf));
}
static void gseen_add(const char *id) {
  if (!id[0] || gseen_has(id)) return;
  s_cpy(g_gseen[g_gseen_w], id, 20);
  g_gseen_w = (g_gseen_w + 1) % 64;
  if (g_gseen_n < 64) g_gseen_n++;
  gseen_save();
}
static void gseen_load(void) {
  static char buf[64 * 21];
  uint32_t n = hal_kv_get("gseen", 5, buf, sizeof(buf) - 1);
  if (n == 0 || n >= sizeof(buf)) return;
  buf[n] = 0;
  char cur[20]; unsigned c = 0;
  for (uint32_t i = 0; buf[i]; i++) {
    if (buf[i] == ';') {
      cur[c] = 0;
      if (c > 0 && g_gseen_n < 64) { s_cpy(g_gseen[g_gseen_n], cur, 20); g_gseen_n++; }
      c = 0;
    } else if (c < sizeof(cur) - 1) {
      cur[c++] = buf[i];
    }
  }
  g_gseen_w = g_gseen_n % 64;
}

/* (Re)subscribe to the notes of every group we are in. Called whenever the group
 * set changes; cheap, and a stale filter would silently miss a whole group. */
static void groups_subscribe(void) {
  if (g_sub_groups[0]) {
    hal_nostr_unsubscribe(g_sub_groups, s_len(g_sub_groups));
    g_sub_groups[0] = 0;
  }
  char f[700];
  s_cpy(f, "{\"kinds\":[1],\"#t\":[", sizeof(f));
  int any = 0;
  for (int i = 0; i < g_convo_n; i++) {
    const char *id = g_convo_ids[i];
    if (!is_group(id)) continue;
    char gname[8]; int gj = 0;
    for (int k = 1; id[k] && id[k] != '*' && gj < 6; k++) gname[gj++] = id[k];
    gname[gj] = 0;
    if (!gname[0]) continue;
    char tag[24]; group_tag(gname, tag, sizeof(tag));
    if (any) s_cat(f, ",", sizeof(f));
    s_cat(f, "\"", sizeof(f)); s_cat(f, tag, sizeof(f)); s_cat(f, "\"", sizeof(f));
    any = 1;
  }
  if (!any) return;                       /* no groups -> no subscription */
  s_cat(f, "],\"limit\":100}", sizeof(f));
  uint32_t n = hal_nostr_subscribe(f, s_len(f), g_sub_groups, sizeof(g_sub_groups) - 1);
  if (n > 0 && n < sizeof(g_sub_groups)) g_sub_groups[n] = 0; else g_sub_groups[0] = 0;
}

/* ── Rooms (NIP-72 communities + moderation op-log; see room.c) ──────────── */
static char g_sub_rooms[64] = "";

/* The live room filter, and when it was last (re)subscribed.
 *
 * A re-subscribe is not cheap: the filter carries limit:500 and no `since`, so
 * every relay replays its whole room history into this wapp. Doing that on each
 * "something changed" signal closed a feedback loop that pegged a core — the
 * replay contains the very events that report a change. So a re-subscribe now
 * has to EARN itself: the filter must actually differ, and never more than once
 * in 30 seconds whatever the events say. */
static char g_sub_rooms_filter[1400] = "";
static uint64_t g_sub_rooms_at = 0;
#define ROOMS_RESUB_MIN_S 30

static void rooms_subscribe(void) {
  char f[1400];
  unsigned fn = room_sub_filter(f, sizeof(f));
  if (!fn) return;
  /* Same question as last time: the answer cannot have changed. */
  if (g_sub_rooms[0] && s_eq(f, g_sub_rooms_filter)) return;
  uint64_t now = hal_time_epoch();
  if (g_sub_rooms[0] && now - g_sub_rooms_at < ROOMS_RESUB_MIN_S) return;
  g_sub_rooms_at = now;
  s_cpy(g_sub_rooms_filter, f, sizeof(g_sub_rooms_filter));
  if (g_sub_rooms[0]) {
    hal_nostr_unsubscribe(g_sub_rooms, s_len(g_sub_rooms));
    g_sub_rooms[0] = 0;
  }
  uint32_t n = hal_nostr_subscribe(f, s_len(f), g_sub_rooms, sizeof(g_sub_rooms) - 1);
  if (n > 0 && n < sizeof(g_sub_rooms)) g_sub_rooms[n] = 0; else g_sub_rooms[0] = 0;
}

/* One event off the room subscription: a room def/op is consumed by room.c; a
 * room message is rendered into its conversation. */
/* Ask the user (a parent authority) to approve the newest pending proposal. */
static void prompt_pending_approval(void) {
  char pid[80], name[80], parent[80];
  if (!room_newest_pending(pid, sizeof(pid), name, sizeof(name), parent, sizeof(parent)))
    return;
  s_cpy(g_appr_target, pid, sizeof(g_appr_target));
  char m[512] = "{\"type\":\"ui.prompt\",\"id\":\"rappr\",\"title\":\"New room request\","
                "\"body\":\"Approve the sub-room \\\"";
  jesc(m, sizeof(m), name);
  s_cat(m, "\\\"?\",\"chips\":[{\"label\":\"Approve\",\"value\":\"approve\"},"
           "{\"label\":\"Dismiss\",\"value\":\"dismiss\"}],\"confirm\":\"Cancel\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* The rail = the moderated room tree PLUS this device's broadcast channels
 * (subscribed groups, the NomadNet bridge). The channels were previously in
 * the conversation store but on no screen at all — their unread counted on
 * the launcher badge while nothing in the UI could show, open or clear them,
 * which is exactly how "7 notifications from nowhere" happens. */
static const char *fnd_next_obj(const char *p, char *slice, unsigned m);

/* One rail row's JSON. [people] < 0 means "do not claim a number". */
static void rail_item(char *out, unsigned cap, const char *id, const char *name,
                      int people, int live) {
  s_cat(out, "{\"id\":\"", cap); jesc(out, cap, id);
  s_cat(out, "\",\"name\":\"", cap); jesc(out, cap, name);
  s_cat(out, "\",\"depth\":0", cap);
  if (people > 0) {
    char nb[12]; u_itoa((unsigned)people, nb);
    s_cat(out, ",\"people\":", cap); s_cat(out, nb, cap);
  }
  if (live) s_cat(out, ",\"live\":true", cap);
  { char tb[16]; u_itoa((unsigned)recent_of(id), tb);
    s_cat(out, ",\"seen\":", cap); s_cat(out, tb, cap); }
  s_cat(out, "}", cap);
}

/* Is this LXMF peer announcing right now? One directory read per rail render;
 * the host caches the registry, and the rail renders on a slow cadence. */
static int lxmf_live(const char *dest) {
  static char dir[8192];
  int32_t n = hal_people_directory(dest, s_len(dest), dir, sizeof(dir) - 1);
  if (n <= 0) return 0;
  dir[n] = 0;
  char slice[900];
  const char *p = fnd_next_obj(dir, slice, sizeof(slice));
  while (p) {
    char d[70]; jstr(slice, "dest", d, sizeof(d));
    if (s_eq(d, dest)) return jbool(slice, "live");
    p = fnd_next_obj(p, slice, sizeof(slice));
  }
  return 0;
}

static void render_rail(void) {
  static char extra[6000];
  extra[0] = 0;
  int first = 1;
  /* Most recently visited/talked first — the rail's whole job is to lead with
   * what you actually use. Rows never touched sort last, in insertion order. */
  int idx[32], cnt = 0;
  for (int i = 0; i < g_convo_n && cnt < 32; i++) idx[cnt++] = i;
  for (int a = 0; a < cnt; a++) {
    for (int b = a + 1; b < cnt; b++) {
      if (recent_of(g_convo_ids[idx[b]]) > recent_of(g_convo_ids[idx[a]])) {
        int t = idx[a]; idx[a] = idx[b]; idx[b] = t;
      }
    }
  }
  for (int k = 0; k < cnt; k++) {
    const char *id = g_convo_ids[idx[k]];
    int lx = is_lxmf(id);
    char title[80];
    if (id[0] != '#' && !lx) {
      /* A room joined from Search that hangs outside the main tree: the rail
       * draws that tree, so without this the room opens and then cannot be
       * found again. */
      if (!room_is_room(id) || room_on_main_tree(id)) continue;
      room_name_of(id, title, sizeof(title));
      if (!first) s_cat(extra, ",", sizeof(extra));
      first = 0;
      rail_item(extra, sizeof(extra), id, title, room_people_seen(id), 0);
      continue;
    }
    if (room_is_room(id)) continue;          /* already on the tree */
    if (lx ? !g_chan_nomad : !chan_enabled(id)) continue; /* switched off */
    convo_title(id, title, sizeof(title));
    if (!first) s_cat(extra, ",", sizeof(extra));
    first = 0;
    /* People seen: only where senders were actually distinguishable (see
     * lxmf_drain). A direct 1:1 is two people by definition, so it claims
     * nothing. */
    int people = lx ? -1 : chanppl_count(id);
    rail_item(extra, sizeof(extra), id, title, people,
              lx ? lxmf_live(id + 5) : 0);
  }
  room_render_tree_with(extra);
}

static void room_event_ingest(const char *evt) {
  /* kind-7 reaction on a room message: tally the heart, never a bubble. Our
   * own federated-back copy is skipped (already tallied on send). */
  if (jint(evt, "kind") == 7) {
    char mid[70], pub[80], content[8], from[16];
    if (!evt_tag(evt, "e", mid, sizeof(mid))) return;
    jstr(evt, "pubkey", pub, sizeof(pub));
    if (!pub[0] || room_is_self(pub)) return;
    s_cpy(from, pub, 13);
    if (is_blocked(from) || is_muted(from)) return;
    char rid[80];
    if (!evt_tag(evt, "h", rid, sizeof(rid)) || !room_is_room(rid)) return;
    jstr(evt, "content", content, sizeof(content));
    convo_react(rid, mid, from, content[0] == '-', 0);
    return;
  }
  int rc = room_ingest(evt);
  if (rc == 3) { prompt_pending_approval(); return; } /* a proposal I can approve */
  if (rc == 2) { rooms_subscribe(); render_rail(); } /* room appeared/changed */
  if (rc) return;
  char rid[80];
  if (!room_note_roomid(evt, rid, sizeof(rid))) return;   /* new msg, or 0 if dup */
  char id[80] = "", pub[80] = "", from[16] = "";
  static char content[900];
  content[0] = 0;
  jstr(evt, "id", id, sizeof(id));
  jstr(evt, "pubkey", pub, sizeof(pub));
  jstr(evt, "content", content, sizeof(content));
  if (!content[0]) return;
  if (room_is_self(pub)) return;   /* our own federated-back copy; already echoed */
  s_cpy(from, pub, 13);
  convo_msg(rid, "in", from, content, "", "", 0, 0, "NOS", id, "", "verified", 0, 0);
  notify_msg(rid, from, content, content);
}

/* ── Rooms widget commands (Discord-like layout) ── */
static void do_rooms_open(const char *buf) {
  char rid[80] = ""; jstr(buf, "rooms_convo", rid, sizeof(rid));
  if (!rid[0]) return;
  recent_touch(rid);   /* opening it IS the visit the rail orders on */
  /* An LXMF thread deep-linked from elsewhere (the Reticulum graph's "message
   * this device"): adopt it as a real conversation so it persists and shows on
   * the rail — otherwise the row vanishes on the next launch. */
  if (is_lxmf(rid)) {
    if (!convo_known(rid)) {
      convo_ensure(rid);
      groups_save();
      render_rail();
    }
    /* Opening it is reading it. This is the command the chat screen actually
     * sends for a 1:1 thread, so without this the read receipt never left. */
    rpend_flush_read(rid);
    return;
  }
  s_cpy(g_cur_room, rid, sizeof(g_cur_room));
  room_render_members(rid);          /* populate the member panel for this room */
}
static void do_rooms_send(const char *buf) {
  char rid[80] = "", text[400] = "";
  jstr(buf, "rooms_convo", rid, sizeof(rid));
  jstr(buf, "rooms_input", text, sizeof(text));
  if (!rid[0] || !text[0]) return;
  if (!room_is_room(rid)) {
    /* An LXMF direct conversation: one NomadNet/Sideband peer, addressed by
     * its 32-hex delivery dest. Fire-and-forget (LXMF stores-and-forwards);
     * the echo bubble appears now, delivery happens when a path exists. */
    if (is_lxmf(rid)) {
      const char *dest = rid + 5;
      /* The heart is not a message. It reaches this path as "<id>:like" text
       * (the host widget only knows how to send text), and without this branch
       * the vote was DELIVERED to the peer as a bubble reading "b9fb:like"
       * while no heart ever lit on either side. */
      char ltgt[70]; int unlike; const char *vtext;
      if (votemark_parse(text, ltgt, &unlike, &vtext) ||
          (vtext = "", anylike_parse(text, ltgt, &unlike))) {
        char from[16]; s_cpy(from, g_call, sizeof(from));
        /* Send the vote WITH the text it voted on, so the peer can find the
         * message even when it never held our id for it — which is every
         * message either side sent before ids were derived. */
        char wire[96];
        votemark_wire(wire, sizeof(wire), ltgt, unlike, vtext);
        hal_lxmf_send(dest, s_len(dest), "", 0, wire, s_len(wire));
        convo_react_of(vtext, rid, ltgt, from, unlike, 1); /* lights it now */
        return;
      }
      /* A reply carries its parent as a "+<4hex> " marker. That is wire
       * syntax, not something the user typed: the peer's drain strips it, and
       * so must the echo — the marker used to be rendered verbatim, so our own
       * bubble read "+9eb53a4a… OK". */
      char parent[5]; const char *disp;
      thread_parse(text, parent, &disp);
      if (!disp[0]) return;
      char mid[5]; msg_id(lxmf_self_dest(), disp, mid);
      /* Correlation id for the ticks. The APRS/BLE path has stamped `am:<6hex>`
       * on every 1:1 message for a long time; this path never did, so a
       * NomadNet/Bluetooth conversation could not produce a receipt and its
       * bubbles never showed a tick at all. Same token, same six hex, so the
       * peer answers it with the same "?ACK <am> d" it already knows how to
       * send. PREPENDED for the reason the other path documents: a signature
       * line must stay alone on the last line. */
      char am[8] = "";
      { unsigned char rb[3]; hal_crypto_random((char *)rb, 3);
        static const char hx[] = "0123456789abcdef";
        for (int i = 0; i < 3; i++) { am[i*2] = hx[rb[i] >> 4]; am[i*2+1] = hx[rb[i] & 15]; }
        am[6] = 0; }
      char lwire[900];
      s_cpy(lwire, "am:", sizeof(lwire));
      s_cat(lwire, am, sizeof(lwire));
      s_cat(lwire, " ", sizeof(lwire));
      s_cat(lwire, text, sizeof(lwire));
      int lx_sent = hal_lxmf_send(dest, s_len(dest), "", 0, lwire, s_len(lwire));
      /* BEST HOPE. LXMF accepts a message it has no path for and holds it, so
       * "sent" says nothing about whether anyone will ever carry it. When the
       * peer is reachable nowhere, air an encrypted copy for nearby devices to
       * hold: addressed by CALLSIGN (short, what a custodian matches on) and
       * carrying the recipient's NPUB, which is the identity that cannot be
       * spoofed and means something outside Reticulum too. Without a known
       * identity there is no honest way to address it, so we do not air one. */
      /* Delivery is the CORE's job, not ours: if this message needs a carrier
       * on the mesh, MeshCourier arms it inside hal_lxmf_send and airs it when
       * the retry queue proves there was no path. A carried message comes back
       * through the ordinary LXMF inbox, so nothing here has to know. */
      if (lx_sent > 0) {
        char from[16]; s_cpy(from, g_call, sizeof(from));
        /* "sent" draws NOTHING (the tick appears on delivered), but it carries
         * the rid the peer's receipt will name. */
        s_cpy(g_send_rid, am, sizeof(g_send_rid));
        s_cpy(g_send_status, "sent", sizeof(g_send_status));
        convo_msg(rid, "out", from, disp, "", "", 0, 0, "LXM", mid, parent, "", 0, 0);
        g_send_rid[0] = 0; g_send_status[0] = 0;
        convo_touch(rid, disp, 0);
        status("TX (LXMF)");
      } else {
        notify("warning", "Couldn't queue the LXMF message");
      }
      return;
    }
    /* A broadcast CHANNEL on the same rail (#group / #NOMADNET): same target,
     * different transport — route through the ordinary group send pipeline
     * (Reticulum bulletin + BLE + optional APRS-IS), which also handles the
     * 4-hex group like votes the heart button emits there. */
    if (rid[0] == '#') convo_send_core(buf, rid, text);
    return;
  }
  /* The heart button rides the send path: a like is a REACTION (kind 7 on the
   * wire, a tally in the UI), never a message bubble. */
  {
    char mid[70]; int unlike; const char *vtext;
    if (votemark_parse(text, mid, &unlike, &vtext) ||
        (vtext = "", roomlike_parse(text, mid, &unlike))) {
      room_react(rid, mid, unlike);
      char from[16]; s_cpy(from, g_pubkey[0] ? g_pubkey : g_call, 13);
      /* Rooms address by NOSTR event id, which both ends do share — the text
       * only rides along so the local tally lands on the right bubble. */
      convo_react_of(vtext, rid, mid, from, unlike, 1);
      return;
    }
  }
  if (!room_self_can_post(rid)) {
    notify("warning", "You can't post here right now (suspended, banned, or closed)");
    return;
  }
  if (room_post(rid, text)) {
    char from[16]; s_cpy(from, g_pubkey[0] ? g_pubkey : g_call, 13);
    convo_msg(rid, "out", from, text, "", "", 0, 0, "NOS", "", "", "verified", 0, 0);
  } else {
    notify("warning", "Couldn't post to this room");
  }
}
/* ── New chat: find a PERSON and start a direct conversation ──────────────
 * Full-screen picker over two worlds:
 *   - NomadNet/LXMF peers from the live announce registry (hal_rns_nodes,
 *     service "lxmf"): matched by announced name, identity hash or LXMF
 *     delivery address; a pasted 32-hex address works even when unheard.
 *   - XPRS/NOSTR people (hal_people_search): callsign / npub / nickname —
 *     these open in the Mail wapp, which owns the one kind-4 inbox. */

/* djb2 + diffed send: an unchanged people list is never re-sent (a re-sent
 * list resets the scroll). */
static uint32_t fnd_djb2(const char *s) {
  uint32_t h = 5381;
  for (; *s; s++) h = ((h << 5) + h) ^ (unsigned char)*s;
  return h;
}
static int fnd_changed_send(const char *m, uint32_t *last) {
  uint32_t h = fnd_djb2(m);
  if (h == *last) return 0;
  *last = h;
  hal_msg_send(m, s_len(m));
  return 1;
}
/* Prefill a host scalar field (the search box). */
static void fnd_field_set(const char *field, const char *value) {
  char m[300] = "{\"type\":\"ui.field.set\",\"field\":\"";
  s_cat(m, field, sizeof(m));
  s_cat(m, "\",\"value\":\"", sizeof(m));
  jesc(m, sizeof(m), value);
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* Copy the next {...} object at/after [p] into [slice]; return cursor past it. */
static const char *fnd_next_obj(const char *p, char *slice, unsigned m) {
  if (!p) return 0;
  while (*p && *p != '{') p++;
  if (!*p) return 0;
  int depth = 0; unsigned i = 0;
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
static int is_hex32(const char *s) {
  int n = 0;
  for (; s[n]; n++) {
    char c = s[n];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
  }
  return n == 32;
}

static char g_find_q[64] = "";
static int g_find_open = 0;   /* the picker is on screen: keep it refreshed */
static char g_find_json[16384];
static char g_find_out[16384];
static uint32_t g_find_hash = 0;

static void render_finduser(void) {
  char *o = g_find_out; const unsigned sz = sizeof(g_find_out);
  o[0] = 0;
  /* ONE list. Splitting NomadNet and XPRS into two tabs made the user hunt
   * for a person across tabs before knowing which network they were on — the
   * thing they are least likely to know. Each row says where it came from
   * instead. */
  s_cat(o, "{\"type\":\"ui.people.set\",\"field\":\"finduser\",\"sections\":["
           "{\"title\":\"People\",\"items\":[", sz);

  int32_t n = hal_people_directory(g_find_q, s_len(g_find_q),
                                   g_find_json, sizeof(g_find_json) - 1);
  if (n < 0) n = 0;
  g_find_json[n] = 0;

  int first = 1, matched_direct = 0;
  char slice[1200];
  const char *p = fnd_next_obj(g_find_json, slice, sizeof(slice));
  while (p) {
    char kind[16], dest[70], name[40], call[24], npub[80], nick[40], via[16];
    jstr(slice, "kind", kind, sizeof(kind));
    jstr(slice, "dest", dest, sizeof(dest));
    jstr(slice, "name", name, sizeof(name));
    jstr(slice, "callsign", call, sizeof(call));
    jstr(slice, "npub", npub, sizeof(npub));
    jstr(slice, "nick", nick, sizeof(nick));
    jstr(slice, "via", via, sizeof(via));
    int live = jbool(slice, "live");
    int hops = jint(slice, "hops");

    if (s_eq(kind, "lxmf") && dest[0]) {
      if (g_find_q[0] && s_eq(dest, g_find_q)) matched_direct = 1;
      const char *disp = name[0] ? name : (call[0] ? call : 0);
      if (!first) s_cat(o, ",", sz);
      first = 0;
      /* Field separator: , NOT \t. jstr() copies an unknown escape's
       * letter verbatim, so a "\t" separator arrived as the letter 't' and
       * welded the name onto the address. */
      s_cat(o, "{\"id\":\"lx:", sz); jesc(o, sz, dest);
      s_cat(o, "\\u001f", sz); if (disp) jesc(o, sz, disp);
      s_cat(o, "\",\"title\":\"", sz);
      if (disp) jesc(o, sz, disp);
      else { s_cat(o, "LXMF ", sz); char sh[9]; s_cpy(sh, dest, sizeof(sh)); s_cat(o, sh, sz); }
      /* Where they are from + how we reach them. */
      s_cat(o, "\",\"subtitle\":\"", sz);
      s_cat(o, jbool(slice, "xprs") ? "XPRS device" : "NomadNet", sz);
      s_cat(o, live ? " - online now" : " - seen earlier", sz);
      if (hops > 0) {
        char nb[12]; u_itoa((unsigned)hops, nb);
        s_cat(o, " - ", sz); s_cat(o, nb, sz);
        s_cat(o, hops == 1 ? " hop" : " hops", sz);
      }
      if (via[0]) { s_cat(o, " via ", sz); jesc(o, sz, via); }
      s_cat(o, " - ", sz);
      { char sh[13]; s_cpy(sh, dest, sizeof(sh)); s_cat(o, sh, sz); s_cat(o, "...", sz); }
      s_cat(o, "\",\"icon\":\"person\"}", sz);
    } else if (s_eq(kind, "xprs")) {
      const char *target = call[0] ? call : npub;
      if (target[0]) {
        if (!first) s_cat(o, ",", sz);
        first = 0;
        s_cat(o, "{\"id\":\"np:", sz); jesc(o, sz, target);
        s_cat(o, "\",\"title\":\"", sz);
        jesc(o, sz, call[0] ? call : npub);
        if (nick[0]) { s_cat(o, " - ", sz); jesc(o, sz, nick); }
        s_cat(o, "\",\"subtitle\":\"XPRS", sz);
        s_cat(o, live ? " - online now" : "", sz);
        { int devices = jint(slice, "devices");
          if (devices > 0) { char nb[12]; u_itoa((unsigned)devices, nb);
            s_cat(o, " - ", sz); s_cat(o, nb, sz);
            s_cat(o, devices == 1 ? " device" : " devices", sz); } }
        s_cat(o, " - opens in Messages\",\"icon\":\"person\"}", sz);
      }
    }
    p = fnd_next_obj(p, slice, sizeof(slice));
  }

  /* A pasted 32-hex LXMF address nobody has announced yet is still valid —
   * LXMF stores-and-forwards, so the message waits for them. */
  if (is_hex32(g_find_q) && !matched_direct) {
    if (!first) s_cat(o, ",", sz);
    first = 0;
    s_cat(o, "{\"id\":\"lx:", sz); s_cat(o, g_find_q, sz);
    s_cat(o, "\\u001f\",\"title\":\"Message ", sz);
    { char sh[9]; s_cpy(sh, g_find_q, sizeof(sh)); s_cat(o, sh, sz); s_cat(o, "...", sz); }
    s_cat(o, "\",\"subtitle\":\"LXMF address - not heard yet, delivery waits for them\","
             "\"icon\":\"person_add\"}", sz);
  }

  s_cat(o, "]}]}", sz);
  fnd_changed_send(o, &g_find_hash);
}

/* ── Search: rooms, channels, open conversations and people, in one panel ──
 * The rail is the whole navigation surface, and it only shows what you are
 * already in. This finds the rest: a room you have not joined, a channel or
 * conversation buried down the rail, or a person anywhere on the mesh. */
static void do_finduser_tap(const char *buf);   /* defined below */
static char g_sa_q[64] = "";
static int g_sa_open = 0;
static char g_sa_out[16384];
static char g_sa_json[16384];
static uint32_t g_sa_hash = 0;

/* Case-insensitive substring. */
static int ci_has(const char *hay, const char *needle) {
  if (!needle[0]) return 1;
  char h[200], n[80];
  unsigned i = 0;
  for (; hay[i] && i < sizeof(h) - 1; i++) {
    char c = hay[i]; h[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
  }
  h[i] = 0;
  for (i = 0; needle[i] && i < sizeof(n) - 1; i++) {
    char c = needle[i]; n[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
  }
  n[i] = 0;
  unsigned nl = s_len(n), hl = s_len(h);
  for (unsigned k = 0; nl && k + nl <= hl; k++) {
    unsigned j = 0;
    for (; j < nl; j++) if (h[k + j] != n[j]) break;
    if (j == nl) return 1;
  }
  return 0;
}

static void render_searchall(void) {
  char *o = g_sa_out; const unsigned sz = sizeof(g_sa_out);
  o[0] = 0;
  s_cat(o, "{\"type\":\"ui.people.set\",\"field\":\"searchall\",\"sections\":[", sz);

  /* ── Rooms (every room known, joined or not) ── */
  s_cat(o, "{\"title\":\"Rooms\",\"items\":[", sz);
  {
    static char rooms[6000];
    rooms[0] = 0;
    room_search(g_sa_q, rooms, sizeof(rooms));
    /* room_search yields {"id","name"}; re-tag each as a room row. */
    char slice[400];
    const char *p = fnd_next_obj(rooms, slice, sizeof(slice));
    int first = 1;
    while (p) {
      char rid[80], name[80];
      jstr(slice, "id", rid, sizeof(rid));
      jstr(slice, "name", name, sizeof(name));
      if (rid[0]) {
        if (!first) s_cat(o, ",", sz);
        first = 0;
        s_cat(o, "{\"id\":\"go:", sz); jesc(o, sz, rid);
        s_cat(o, "\",\"title\":\"", sz); jesc(o, sz, name[0] ? name : rid);
        s_cat(o, "\",\"subtitle\":\"Room", sz);
        if (convo_known(rid)) s_cat(o, " - joined", sz);
        /* A NIP-72 room DOES have per-author messages, so this count is real
         * (still "seen", never "members" — rooms publish no roster either). */
        { int seen = room_people_seen(rid);
          if (seen > 0) {
            char nb[12]; u_itoa((unsigned)seen, nb);
            s_cat(o, " - ", sz); s_cat(o, nb, sz);
            s_cat(o, seen == 1 ? " person seen" : " people seen", sz);
          } }
        s_cat(o, "\",\"icon\":\"forum\"}", sz);
      }
      p = fnd_next_obj(p, slice, sizeof(slice));
    }
  }
  s_cat(o, "]}", sz);

  /* ── Channels and open conversations (what is on your rail) ── */
  s_cat(o, ",{\"title\":\"Channels and conversations\",\"items\":[", sz);
  {
    int first = 1;
    /* Same order as the rail: most recently visited or talked to first. */
    int idx[32], cnt = 0;
    for (int i = 0; i < g_convo_n && cnt < 32; i++) idx[cnt++] = i;
    for (int a = 0; a < cnt; a++)
      for (int b = a + 1; b < cnt; b++)
        if (recent_of(g_convo_ids[idx[b]]) > recent_of(g_convo_ids[idx[a]])) {
          int t = idx[a]; idx[a] = idx[b]; idx[b] = t;
        }
    for (int k = 0; k < cnt; k++) {
      const char *id = g_convo_ids[idx[k]];
      int lx = is_lxmf(id);
      if (id[0] != '#' && !lx) continue;
      if (room_is_room(id)) continue;   /* listed under Rooms */
      char title[40];
      convo_title(id, title, sizeof(title));
      if (!ci_has(title, g_sa_q) && !ci_has(id, g_sa_q)) continue;
      if (!first) s_cat(o, ",", sz);
      first = 0;
      s_cat(o, "{\"id\":\"go:", sz); jesc(o, sz, id);
      s_cat(o, "\",\"title\":\"", sz); jesc(o, sz, title);
      s_cat(o, "\",\"subtitle\":\"", sz);
      if (lx) {
        s_cat(o, "Direct chat - LXMF ", sz);
        char sh[13]; s_cpy(sh, id + 5, sizeof(sh)); s_cat(o, sh, sz);
        s_cat(o, "...", sz);
      } else {
        s_cat(o, "Channel", sz);
        /* Only when senders were actually distinguishable — see lxmf_drain. */
        int seen = chanppl_count(id);
        if (seen > 0) {
          char nb[12]; u_itoa((unsigned)seen, nb);
          s_cat(o, " - ", sz); s_cat(o, nb, sz);
          s_cat(o, seen == 1 ? " person seen" : " people seen", sz);
        }
      }
      s_cat(o, "\",\"icon\":\"", sz);
      s_cat(o, lx ? "person" : "campaign", sz);
      s_cat(o, "\"}", sz);
    }
  }
  s_cat(o, "]}", sz);

  /* ── People (the same directory the New chat picker uses) ── */
  s_cat(o, ",{\"title\":\"People\",\"items\":[", sz);
  if (g_sa_q[0]) {
    int32_t n = hal_people_directory(g_sa_q, s_len(g_sa_q),
                                     g_sa_json, sizeof(g_sa_json) - 1);
    if (n < 0) n = 0;
    g_sa_json[n] = 0;
    char slice[1200];
    const char *p = fnd_next_obj(g_sa_json, slice, sizeof(slice));
    int first = 1, rows = 0;
    while (p && rows < 40) {
      char kind[16], dest[70], name[40], call[24], npub[80];
      jstr(slice, "kind", kind, sizeof(kind));
      jstr(slice, "dest", dest, sizeof(dest));
      jstr(slice, "name", name, sizeof(name));
      jstr(slice, "callsign", call, sizeof(call));
      jstr(slice, "npub", npub, sizeof(npub));
      int live = jbool(slice, "live");
      if (s_eq(kind, "lxmf") && dest[0]) {
        const char *disp = name[0] ? name : (call[0] ? call : 0);
        if (!first) s_cat(o, ",", sz);
        first = 0; rows++;
        s_cat(o, "{\"id\":\"lx:", sz); jesc(o, sz, dest);
        s_cat(o, "\\u001f", sz); if (disp) jesc(o, sz, disp);
        s_cat(o, "\",\"title\":\"", sz);
        if (disp) jesc(o, sz, disp);
        else { s_cat(o, "LXMF ", sz); char sh[9]; s_cpy(sh, dest, sizeof(sh)); s_cat(o, sh, sz); }
        s_cat(o, "\",\"subtitle\":\"", sz);
        s_cat(o, jbool(slice, "xprs") ? "XPRS device" : "NomadNet", sz);
        s_cat(o, live ? " - online now" : " - seen earlier", sz);
        s_cat(o, "\",\"icon\":\"person\"}", sz);
      } else if (s_eq(kind, "xprs")) {
        const char *target = call[0] ? call : npub;
        if (target[0]) {
          if (!first) s_cat(o, ",", sz);
          first = 0; rows++;
          s_cat(o, "{\"id\":\"np:", sz); jesc(o, sz, target);
          s_cat(o, "\",\"title\":\"", sz); jesc(o, sz, call[0] ? call : npub);
          s_cat(o, "\",\"subtitle\":\"XPRS", sz);
          s_cat(o, live ? " - online now" : "", sz);
          s_cat(o, " - opens in Messages\",\"icon\":\"person\"}", sz);
        }
      }
      p = fnd_next_obj(p, slice, sizeof(slice));
    }
  }
  s_cat(o, "]}]}", sz);
  fnd_changed_send(o, &g_sa_hash);
}

static void do_rooms_search(void) {
  g_sa_q[0] = 0;
  g_sa_open = 1;
  fnd_field_set("searchall_query", "");
  render_searchall();
  const char *m = "{\"type\":\"ui.screen.open\",\"name\":\"Search\"}";
  hal_msg_send(m, s_len(m));
}

/* A result row: "go:<conversation or room id>" opens it, the lx:/np: forms
 * behave exactly as they do in the New chat picker. */
static void do_searchall_tap(const char *buf) {
  char id[140] = "";
  jstr(buf, "searchall_id", id, sizeof(id));
  if (s_pre(id, "go:")) {
    const char *target = id + 3;
    /* Register it either way. A room found in Search is normally NOT on the
     * main tree, and the rail draws that tree — without this the room opens
     * once and can never be found again. (The earlier guard skipped exactly
     * the rooms that needed the row.) */
    if (!convo_known(target)) {
      convo_ensure(target);
      groups_save();
      render_rail();
    }
    char m[220] = "{\"type\":\"ui.convo.upsert\",\"id\":\"";
    jesc(m, sizeof(m), target);
    s_cat(m, "\",\"select\":true,\"bump\":true}", sizeof(m));
    hal_msg_send(m, s_len(m));
    const char *c = "{\"type\":\"ui.screen.close\"}";
    hal_msg_send(c, s_len(c));
    return;
  }
  /* People rows share the picker's handler — same ids, same behaviour. */
  char fwd[200] = "{\"finduser_id\":\"";
  jesc(fwd, sizeof(fwd), id);
  s_cat(fwd, "\"}", sizeof(fwd));
  do_finduser_tap(fwd);
}

static void do_rooms_newchat(void) {
  g_find_q[0] = 0;
  fnd_field_set("finduser_query", "");
  g_find_open = 1;   /* keep refreshing: announces arrive while it is on screen */
  render_finduser();
  const char *m = "{\"type\":\"ui.screen.open\",\"name\":\"New chat\"}";
  hal_msg_send(m, s_len(m));
}

static void do_finduser_tap(const char *buf) {
  char id[140] = "";
  jstr(buf, "finduser_id", id, sizeof(id));
  if (s_pre(id, "lx:")) {
    /* "lx:<dest><0x1f><name>" — start (or reopen) the LXMF conversation. */
    char dest[70] = "", name[40] = "";
    const char *r = id + 3; unsigned i = 0;
    while (*r && *r != 0x1f && i < sizeof(dest) - 1) dest[i++] = *r++;
    dest[i] = 0;
    if (*r == 0x1f) r++;
    i = 0;
    while (*r && i < sizeof(name) - 1) name[i++] = *r++;
    name[i] = 0;
    /* A conversation id is exactly "lxmf:" + 32 hex. Anything else is a
     * parsing accident, and one such row poisons the rail and the saved
     * subscription list — refuse it here rather than persist it. */
    if (!is_hex32(dest)) return;
    if (name[0]) lxname_set(dest, name);
    char cid[72] = "lxmf:";
    s_cat(cid, dest, sizeof(cid));
    convo_ensure(cid);
    groups_save();
    render_rail();
    /* Open it: upsert with select so the host focuses the conversation. */
    char m[220] = "{\"type\":\"ui.convo.upsert\",\"id\":\"";
    jesc(m, sizeof(m), cid);
    s_cat(m, "\",\"select\":true,\"bump\":true}", sizeof(m));
    hal_msg_send(m, s_len(m));
    const char *c = "{\"type\":\"ui.screen.close\"}";
    hal_msg_send(c, s_len(c));
  } else if (s_pre(id, "np:")) {
    /* A NOSTR person: their 1:1 lives in the Mail wapp (the ONE kind-4
     * inbox) — jump there instead of growing a second copy here. */
    char m[200] = "{\"type\":\"mail.open\",\"target\":\"";
    jesc(m, sizeof(m), id + 3);
    s_cat(m, "\"}", sizeof(m));
    hal_msg_send(m, s_len(m));
  }
}

static void do_rooms_new(const char *buf) {
  char parent[80] = ""; jstr(buf, "rooms_convo", parent, sizeof(parent));
  s_cpy(g_new_parent, parent[0] ? parent : MAIN_ROOM_ID, sizeof(g_new_parent));
  const char *m = "{\"type\":\"ui.prompt\",\"id\":\"newroom\",\"title\":\"New room\","
                  "\"body\":\"Name the sub-room. It is created under the room you have "
                  "open — if you are not a moderator there, a moderator must approve "
                  "it first.\",\"input\":{\"hint\":\"room name\",\"max\":40},"
                  "\"confirm\":\"Request\"}";
  hal_msg_send(m, s_len(m));
}

/* Open the Members panel for the currently-open room. */
static void do_room_members(const char *buf) {
  char rid[80] = ""; jstr(buf, "conversations_convo", rid, sizeof(rid));
  if (!rid[0] || !room_is_room(rid)) { notify("info", "Open a room first"); return; }
  s_cpy(g_cur_room, rid, sizeof(g_cur_room));
  room_render_members(rid);
  const char *m = "{\"type\":\"ui.screen.open\",\"name\":\"Members\"}";
  hal_msg_send(m, s_len(m));
}

/* A member row tapped: authorities get a moderation prompt; others see the
 * member's reputation level. */
static void do_room_member_tap(const char *buf) {
  char pub[80] = ""; jstr(buf, "room_members_id", pub, sizeof(pub));
  if (!pub[0]) { notify("warning", "member: no id"); return; }
  if (!g_cur_room[0]) s_cpy(g_cur_room, MAIN_ROOM_ID, sizeof(g_cur_room));
  if (!room_self_authority(g_cur_room)) {
    char b[48] = "Level "; char lv[8]; u_itoa((unsigned)room_rep_level(pub), lv);
    s_cat(b, lv, sizeof(b)); s_cat(b, " (view only)", sizeof(b));
    notify("info", b);
    return;
  }
  s_cpy(g_mod_target, pub, sizeof(g_mod_target));
  const char *m =
      "{\"type\":\"ui.prompt\",\"id\":\"rmod\",\"title\":\"Moderate member\","
      "\"body\":\"Choose an action.\",\"chips\":["
      "{\"label\":\"Award +5\",\"value\":\"award\"},"
      "{\"label\":\"Deduct 5\",\"value\":\"deduct\"},"
      "{\"label\":\"Suspend 1 day\",\"value\":\"suspend\"},"
      "{\"label\":\"Unsuspend\",\"value\":\"unsuspend\"},"
      "{\"label\":\"Kick\",\"value\":\"kick\"},"
      "{\"label\":\"Ban from room\",\"value\":\"ban\"},"
      "{\"label\":\"Ban from wapp\",\"value\":\"banwapp\"}],"
      "\"confirm\":\"Cancel\"}";
  hal_msg_send(m, s_len(m));
}

/* Value of the first ["t","<topic>"] tag — which group a note belongs to. */
static int find_t_tag(const char *evt, char *out, unsigned m) {
  for (const char *p = evt; *p; p++) {
    if (p[0] == '[' && p[1] == '"' && p[2] == 't' && p[3] == '"' && p[4] == ',') {
      const char *q = p + 5;
      while (*q == ' ') q++;
      if (*q != '"') continue;
      q++;
      unsigned o = 0;
      while (*q && *q != '"' && o < m - 1) out[o++] = *q++;
      out[o] = 0;
      return o > 0;
    }
  }
  return 0;
}

/* One kind-1 note off the group subscription. */
static void group_note_ingest(const char *evt) {
  char id[80] = "", pub[80] = "", ts[24] = "", topic[16] = "";
  static char content[900];
  content[0] = 0;
  jstr(evt, "id", id, sizeof(id));
  jstr(evt, "pubkey", pub, sizeof(pub));
  jstr(evt, "created_at", ts, sizeof(ts));
  jstr(evt, "content", content, sizeof(content));
  if (!id[0] || !content[0]) return;
  if (!find_t_tag(evt, topic, sizeof(topic))) return;
  if (gseen_has(id)) return;
  gseen_add(id);
  /* Our own note is already on screen from the local echo. */
  if (pub[0] && g_pubkey[0] && s_eq_ci(pub, g_pubkey)) return;

  /* topic is "xprs-NEWS" on the wire; the group is "NEWS". */
  const char *pfx = GROUP_TAG_PREFIX;
  unsigned pl = s_len(pfx);
  if (s_len(topic) <= pl) return;
  for (unsigned i = 0; i < pl; i++) if (topic[i] != pfx[i]) return;
  char cid[16]; group_convo_id(topic + pl, cid, sizeof(cid));
  if (!convo_known(cid)) return;          /* a group we are not in */
  char from[16]; s_cpy(from, pub, 13);    /* short pubkey until a profile lands */
  convo_msg(cid, "in", from, content, "", "", 0, 0, "NOS", id, "", "verified", 0, 0);
  convo_touch(cid, content, 0);
  notify_msg(cid, from, content, content);
}

/* Ask the host directory who [dest] is and remember the answer. Cheap no-op
 * while the name is known or the retry window has not elapsed. */
static void lxname_resolve(const char *dest) {
  if (!dest || !dest[0]) return;
  if (lxname_get(dest)) return;            /* already named */
  if (!lxname_may_ask(dest)) return;       /* asked recently */
  static char dj[1600];
  int32_t dn = hal_people_directory(dest, s_len(dest), dj, sizeof(dj) - 1);
  if (dn <= 0) return;
  dj[dn] = 0;
  char slice[600];
  const char *p = fnd_next_obj(dj, slice, sizeof(slice));
  while (p) {
    char d2[70] = "", nm[40] = "", cs[24] = "";
    jstr(slice, "dest", d2, sizeof(d2));
    jstr(slice, "name", nm, sizeof(nm));
    jstr(slice, "callsign", cs, sizeof(cs));
    if (s_eq(d2, dest) && (cs[0] || nm[0])) {
      lxname_set(dest, cs[0] ? cs : nm);
      render_rail();                       /* the row is wrong until redrawn */
      return;
    }
    p = fnd_next_obj(p, slice, sizeof(slice));
  }
}

/* ── Groups from NomadNet (LXMF) ────────────────────────────────────────────
 * Reticulum already has its own chat, and those people are on the same mesh. An
 * inbound LXMF message whose title names a group (#NEWS) joins that group's
 * conversation; anything else lands in #NOMADNET, so a NomadNet user is never
 * silently dropped. Ingest only — we do not invent an outbound LXMF group
 * protocol here (see the wapp README / commit message). */
#define LXMF_GROUP "#NOMADNET"
/* Does this inbound LXMF JSON carry the group field (LXMF field 11 = 0x0B)?
 * The host now passes the decoded field map through as "fields":{"11":…}. */
static int lxmf_is_group(const char *js) {
  const char *f = js;
  while (*f && !s_pre(f, "\"fields\":")) f++;
  if (!*f) return 0;
  for (const char *p = f; *p && *p != '}'; p++) if (s_pre(p, "\"11\"")) return 1;
  return 0;
}
/* An LXMF field naming the message's author, when the sender supplied one.
 * Field 11 (group) carries the group context; some stacks put a display name
 * there or in a custom string field. Accept a plain string value, reject a
 * hash — a 32/64-hex blob is an address, not a name to show. */
static int lxmf_field_sender(const char *js, char *out, unsigned cap) {
  out[0] = 0;
  const char *f = js;
  while (*f && !s_pre(f, "\"fields\":")) f++;
  if (!*f) return 0;
  /* Look at field 11's value when it is a quoted string. */
  const char *p = f;
  while (*p && *p != '}') {
    if (s_pre(p, "\"11\":\"")) {
      p += 6;
      unsigned i = 0;
      while (*p && *p != '"' && i < cap - 1) out[i++] = *p++;
      out[i] = 0;
      /* A pure hex blob is an address; not a name. */
      int hex = out[0] != 0;
      for (unsigned k = 0; out[k]; k++) {
        char c = out[k];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { hex = 0; break; }
      }
      if (hex || s_len(out) > 32) { out[0] = 0; return 0; }
      return out[0] != 0;
    }
    p++;
  }
  return 0;
}
/* Length of a leading "<name>: " sender prefix, 0 when there is none.
 * NomadNet group software prefixes the author this way. Bounded so a sentence
 * containing a colon ("note: it works") is not mistaken for an author. */
static unsigned sender_prefix_len(const char *text) {
  for (unsigned i = 0; i < 24 && text[i]; i++) {
    if (text[i] == '\n') return 0;
    if (text[i] == ':' && text[i + 1] == ' ' && i > 0) {
      /* A name, not prose: no sentence punctuation before the colon. */
      for (unsigned k = 0; k < i; k++) {
        char c = text[k];
        if (c == ',' || c == '.' || c == '?' || c == '!') return 0;
      }
      return i;
    }
  }
  return 0;
}

static void lxmf_drain(void) {
  for (int guard = 0; guard < 10; guard++) {
    char js[1400];
    uint32_t n = hal_lxmf_recv(js, sizeof(js) - 1);
    if (n == 0) break;
    js[n] = 0;
    char from[80] = "", title[64] = "", hash[80] = "";
    static char content[900];
    content[0] = 0;
    jstr(js, "from", from, sizeof(from));
    jstr(js, "title", title, sizeof(title));
    jstr(js, "content", content, sizeof(content));
    jstr(js, "hash", hash, sizeof(hash));
    if (!content[0]) continue;
    if (gseen_has(hash)) continue;
    gseen_add(hash);

    char cid[72];
    if (title[0] == '#') {
      /* A titled/group LXMF message: the shared bridge channel. */
      group_convo_id(title + 1, cid, sizeof(cid));
    } else {
      /* A personal DM from one NomadNet peer: its own conversation, keyed by
       * the sender's delivery dest — reply lands exactly where it came from.
       * (These used to pile into the #NOMADNET channel, where answering a
       * PERSON was impossible.) */
      s_cpy(cid, "lxmf:", sizeof(cid));
      s_cat(cid, from, sizeof(cid));
    }
    if (!g_chan_nomad) continue;        /* the user switched the bridge off */
    if (cid[0] == '#' && !chan_enabled(cid)) continue;
    if (!convo_known(cid)) {
      /* First contact from this address: ask the host's directory who it is
       * before the row is drawn, so a new thread opens as "X16JK8" rather than
       * "LXMF 85cdc031" and stays that way. */
      if (title[0] != '#') lxname_resolve(from);
      convo_ensure(cid);   /* auto-join, or nobody sees it */
      groups_save();       /* …and PERSIST the join — an unsaved auto-join was
                            * gone after a restart while the host store kept
                            * counting its unread: a badge pointing at nothing */
      render_rail();
    }

    /* WHO wrote this.
     *
     * `from` is the sending NODE. For a distribution group that is the group's
     * own address, so every member arrives under one identity — which is why
     * group bubbles all used to show the same 12-hex prefix and why counting
     * people was impossible. Recover the author where the sender gave us one:
     *   1. an LXMF field naming the originator, else
     *   2. the NomadNet convention of a "<name>: " prefix on the content, else
     *   3. nobody — attribute to the node and count nothing. */
    char who[40] = "";
    const char *body = content;
    int is_group_msg = (title[0] == '#') || lxmf_is_group(js);
    if (is_group_msg) {
      char f[64];
      if (lxmf_field_sender(js, f, sizeof(f)) && f[0]) {
        s_cpy(who, f, sizeof(who));
      } else {
        unsigned cut = sender_prefix_len(content);
        if (cut) {
          unsigned k = 0;
          while (k < cut && k < sizeof(who) - 1) { who[k] = content[k]; k++; }
          who[k] = 0;
          body = content + cut + 2;       /* past ": " */
        }
      }
    }
    int sender_known = who[0] != 0;
    if (!sender_known) {
      /* A 1:1 DM: the sender IS the conversation, so use the name we hold for
       * it. Printing 12 hex characters as the author made every bubble read
       * like a machine ID even when the panel that opened the thread knew the
       * person's callsign. */
      const char *nm = (title[0] == '#') ? 0 : lxname_get(from);
      if (nm && nm[0]) { s_cpy(who, nm, sizeof(who)); sender_known = 1; }
      else s_cpy(who, from, 13);                /* the node, not a person */
    }

    /* People seen: only a DISTINGUISHED sender counts. A group whose messages
     * never name their author must show no number rather than "1 person",
     * which would read as a fact and be an artefact of the addressing. */
    if (is_group_msg && sender_known) chanppl_add(cid, who);

    /* A like vote, not a message — the mirror of the send path above. A peer
     * (or an older build of ours) that puts a vote on the wire must never
     * become a bubble here. */
    {
      char ltgt[70]; int ul; const char *vtext;
      if (votemark_parse(body, ltgt, &ul, &vtext)) {
        convo_react_of(vtext, cid, ltgt, who, ul, 0);
        continue;
      }
      if (anylike_parse(body, ltgt, &ul)) {
        convo_react(cid, ltgt, who, ul, 0);
        continue;
      }
    }
    /* Receipt correlation id, and the receipt itself.
     *
     * `am:<6hex>` rides in front of the text (see the send path). Pull it out
     * and strip it BEFORE anything renders or threads on the body, then answer
     * it: the sender is waiting for exactly this to turn its bubble from
     * pending into a tick. A "?ACK <am> <d|r>" arriving here is that answer
     * coming back, and it is not a message -- rcpt_intercept consumes it. */
    char lam[8] = "";
    { char ambuf[900]; s_cpy(ambuf, body, sizeof(ambuf));
      if (extract_am(ambuf, lam)) {
        static char lbody[900];
        s_cpy(lbody, ambuf, sizeof(lbody));
        body = lbody;
      } }
    if (rcpt_intercept(from, body)) continue;   /* a tick, never a bubble */
    if (lam[0] && !is_group_msg) {
      char rcpt[24];
      s_cpy(rcpt, "?ACK ", sizeof(rcpt));
      s_cat(rcpt, lam, sizeof(rcpt));
      s_cat(rcpt, " d", sizeof(rcpt));
      hal_lxmf_send(from, s_len(from), "", 0, rcpt, s_len(rcpt));
      rpend_add(cid, lam, "RET");   /* the read receipt, when the chat opens */
    }
    /* Strip the reply marker and hand the host the parent separately. The mid
     * is derived from "<sender>|<text>", exactly as the sender derived it, so
     * a reply or a heart resolves on both ends without an id on the wire.
     * (The LXMF envelope hash cannot serve: only the receiver ever sees it.) */
    char parent[5]; const char *disp;
    thread_parse(body, parent, &disp);
    if (!disp[0]) continue;
    char mid[5]; msg_id(is_group_msg ? who : from, disp, mid);
    /* Show the time the sender wrote it (the envelope's), not the time it
     * reached us — see g_msg_epoch. */
    int sent_ts = jint(js, "ts");
    if (sent_ts > 0) g_msg_epoch = (uint64_t)sent_ts;
    convo_msg(cid, "in", who, disp, "", "", 0, 0, "LXM", mid, parent, "", 0, 0);
    convo_touch(cid, disp, 0);
    notify_msg(cid, who, disp, disp);
  }
}

/* Subscribed groups persist in KV "groups" (";"-joined ids) so the APRS-IS
 * filter is correct immediately after a restart, before any row is reopened. */
static void groups_save(void) {
  char buf[1200]; buf[0] = 0;
  for (int i = 0; i < g_convo_n; i++)
    if (g_convo_ids[i][0] == '#' || is_lxmf(g_convo_ids[i])) {
      s_cat(buf, g_convo_ids[i], sizeof(buf)); s_cat(buf, ";", sizeof(buf));
    }
  hal_kv_set("groups", 6, buf, s_len(buf));
}
/* Ensure a conversation ROW exists in the host's Messages list (without bumping
 * it to the top or selecting it), so subscribed groups show every time the page
 * opens — the page runs its own engine, so module_init/groups_load re-runs with
 * the page listening, and the host persists what it receives. Only title+icon
 * are sent so an existing row's subtitle/badge merge through unchanged. */
static void convo_ensure(const char *id) {
  convo_remember(id);
  int global = 0; for (int i = 1; id[i]; i++) if (id[i] == '*') global = 1;
  const char *icon = (id[0] == '#') ? (global ? "public" : "campaign") : "person";
  char title[24]; convo_title(id, title, sizeof(title));
  char m[300] = "{\"type\":\"ui.convo.upsert\",\"id\":\"";
  jesc(m, sizeof(m), id);
  s_cat(m, "\",\"title\":\"", sizeof(m)); jesc(m, sizeof(m), title);
  s_cat(m, "\",\"icon\":\"", sizeof(m)); s_cat(m, icon, sizeof(m));
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* The #topic groups older builds subscribed every install to. Nothing seeds
 * them any more — the rooms rail replaced them — but they are still in KV
 * "groups" on any device that ever ran those builds, so they keep showing up
 * as rows nobody chose. Listed here only to REMOVE them. */
static const char *LEGACY_DEFAULT_GROUPS[] = {
  "#DEV", "#NEWS", "#MISC", "#HELP", "#HELLO", "#CHILL"
};
/* Is [id] one of those, in either scope ("#DEV" or the global "#DEV*")? */
static int is_legacy_default_group(const char *id) {
  if (!id || id[0] != '#') return 0;
  char bare[24];
  s_cpy(bare, id, sizeof(bare));
  unsigned L = s_len(bare);
  if (L && bare[L - 1] == '*') bare[L - 1] = 0;   /* drop the global marker */
  for (unsigned i = 0;
       i < sizeof(LEGACY_DEFAULT_GROUPS) / sizeof(LEGACY_DEFAULT_GROUPS[0]);
       i++) {
    if (s_eq(bare, LEGACY_DEFAULT_GROUPS[i])) return 1;
  }
  return 0;
}
/* One-time: drop the conversation rows this wapp used to own.
 *
 * Chat is groups-only now, but the 1:1 threads it created before that are
 * already persisted in the host's ConversationStore, and a guard on new rows
 * cannot rewrite them — they just sit there as a second, stale inbox. So clear
 * the store once; groups_load() immediately re-creates every group row after.
 *
 * This discards Chat's old 1:1 history. That is the intent, not a side effect:
 * those conversations live in the Mail wapp now, which holds them as NOSTR
 * kind-4 and rehydrates them from the relays. Group history is lost with it,
 * which is a real cost — the groups were near-empty broadcast channels, and
 * carrying a split-brain inbox forward costs more. */
static void convo_purge_legacy(void) {
  char f[2];
  /* Token "2": the first purge ran, but the group subscription then filled
   * #NEWS with the public hashtag firehose. Those rows are on disk and a code
   * fix cannot rewrite them, so clear once more. */
  if (hal_kv_get("grponly", 7, f, sizeof(f) - 1) > 0 && f[0] == '3') return;
  const char *clr = "{\"type\":\"ui.convo.clear\",\"field\":\"conversations\"}";
  hal_msg_send(clr, s_len(clr));
  hal_kv_set("grponly", 7, "3", 1);
  const char *lg = "[chat] cleared legacy 1:1 rows (groups-only now)";
  hal_log(1, lg, s_len(lg));
}

static void groups_load(void) {
  convo_purge_legacy();
  char buf[600];
  uint32_t n = hal_kv_get("groups", 6, buf, sizeof(buf) - 1);
  if (n == 0) return;                       /* clean slate: nothing is seeded */
  buf[n] = 0;
  /* One-time: forget the #topic groups older builds subscribed everyone to.
   * Only those exact ids — a group the user joined themselves stays. */
  int drop_defaults = 0;
  { char f[2];
    if (hal_kv_get("grpclean", 8, f, sizeof(f) - 1) == 0) drop_defaults = 1; }
  char id[40]; int j = 0;
  for (unsigned i = 0; i <= n; i++) {
    char ch = (i < n) ? buf[i] : ';';
    /* convo_ensure (not convo_remember): re-push each subscribed group so it
     * shows in the Messages list on every page open, not only the g/ filter. */
    if (ch == ';') {
      id[j] = 0;
      if (drop_defaults && is_legacy_default_group(id)) {
        char m[160] = "{\"type\":\"ui.convo.remove\",\"id\":\"";
        jesc(m, sizeof(m), id);
        s_cat(m, "\"}", sizeof(m));
        hal_msg_send(m, s_len(m));
        j = 0;
        continue;                      /* not remembered → dropped on save */
      }
      /* Drop malformed lxmf rows written by an earlier build (the separator
       * bug welded the peer's name onto the address). They are unopenable and
       * un-messageable; removing the row also removes its phantom unread. */
      if (is_lxmf(id) && !is_hex32(id + 5)) {
        const char *rm = "{\"type\":\"ui.convo.remove\",\"id\":\"";
        char m[160]; s_cpy(m, rm, sizeof(m));
        jesc(m, sizeof(m), id);
        s_cat(m, "\"}", sizeof(m));
        hal_msg_send(m, s_len(m));
        j = 0;
        continue;
      }
      if (id[0] == '#' || is_lxmf(id)) convo_ensure(id);
      j = 0;
    }
    else if (j < 39) id[j++] = ch;
  }
  if (drop_defaults) {
    groups_save();          /* persist the shorter list, once */
    hal_kv_set("grpclean", 8, "1", 1);
    const char *lg = "[chat] removed the legacy default #topic groups";
    hal_log(1, lg, s_len(lg));
  }
  /* The NomadNet bridge channel is NOT pre-created any more: an empty row for
   * traffic that may never arrive is exactly the clutter a clean slate is
   * meant to avoid. lxmf_drain creates and persists it the moment a message
   * for it actually lands. */
  render_rail();   /* the restored channels belong on the rail immediately */
}

/* The public-key beacon on/off state persists in KV "pkbeacon" ("1"/"0"), so
 * the user's choice survives a restart (unlike the per-session BLE toggle). */
static void pkbeacon_save(void) {
  hal_kv_set("pkbeacon", 8, g_pubkey_beacon ? "1" : "0", 1);
}
static void pkbeacon_load(void) {
  char b[4];
  uint32_t n = hal_kv_get("pkbeacon", 8, b, sizeof(b) - 1);
  if (n >= 1) g_pubkey_beacon = (b[0] != '0');   /* absent -> keep default (on) */
}
/* iGate (BLE ↔ APRS-IS bridge) on/off persists in KV "igate" ("1"/"0"); absent
 * keeps the on-by-default state. */
static void igate_save(void) {
  hal_kv_set("igate", 5, g_ble_relay ? "1" : "0", 1);
}
static void igate_load(void) {
  char b[4];
  uint32_t n = hal_kv_get("igate", 5, b, sizeof(b) - 1);
  if (n >= 1) g_ble_relay = (b[0] != '0');
}
/* APRS-IS access persists in KV "aprsis" as "<0|1>|<call>|<passcode>". Absent
 * or malformed keeps the safe default: disabled, no licensed callsign. */
static void aprsis_save(void) {
  char v[40]; v[0] = g_aprsis_on ? '1' : '0'; v[1] = '|'; v[2] = 0;
  s_cat(v, g_aprsis_call, sizeof(v));
  s_cat(v, "|", sizeof(v));
  if (g_aprsis_pass >= 0) { char nb[12]; u_itoa((unsigned)g_aprsis_pass, nb); s_cat(v, nb, sizeof(v)); }
  hal_kv_set("aprsis", 6, v, s_len(v));
}
static void aprsis_load(void) {
  char v[40];
  uint32_t n = hal_kv_get("aprsis", 6, v, sizeof(v) - 1);
  if (n < 2) return;
  v[n] = 0;
  int on = (v[0] == '1');
  const char *p = v + 2;                  /* past "<0|1>|" */
  int ci = 0;
  while (*p && *p != '|' && ci < 15) g_aprsis_call[ci++] = *p++;
  g_aprsis_call[ci] = 0;
  g_aprsis_pass = (*p == '|' && p[1]) ? to_int(p + 1) : -1;
  /* Never come up enabled without a full, self-consistent record. */
  g_aprsis_on = (on && g_aprsis_call[0] && g_aprsis_pass >= 0 &&
                 !is_autogen_call(g_aprsis_call) &&
                 g_aprsis_pass == aprs_passcode(g_aprsis_call));
}
/* Broadcast our npub once: APRS-IS bulletin to group "NOSTR" + same over BLE.
 * Receivers map the sender callsign (frame from-field) to the npub text. */
static void pkbeacon_send(void) {
  if (!g_pubkey_beacon || !g_pubkey[0]) return;
  /* Advertise "<npub>|<rns-deliv-hex>" so peers can also reach us over Reticulum;
   * each device adds its own dest under the shared npub. Falls back to npub-only
   * when the RNS node is down (legacy parsers also read just the npub). */
  char body[200]; s_cpy(body, g_pubkey, sizeof(body));
  char deliv[80] = "", prop[80] = "";
  uint32_t dn = hal_rns_delivery_dest(deliv, sizeof(deliv) - 1);
  if (dn > 0 && dn < sizeof(deliv)) {
    deliv[dn] = 0;
    s_cat(body, "|", sizeof(body)); s_cat(body, deliv, sizeof(body));
    /* Also advertise our propagation mailbox so peers can pull store-and-forwarded
     * 1:1 messages from us (the path that survives both ends being behind NAT). */
    uint32_t pn = hal_rns_prop_dest(prop, sizeof(prop) - 1);
    if (pn > 0 && pn < sizeof(prop)) {
      prop[pn] = 0;
      s_cat(body, "|", sizeof(body)); s_cat(body, prop, sizeof(body));
    }
  }
  if (g_sock >= 0 && g_logged)
    aprs_send_bulletin_multi(g_sock, g_call, PKBEACON_GROUP, body, APRS_MAX_MSG_LEN);
  if (g_ble_on)
    ble_tx_msg("#" PKBEACON_GROUP, body);
  /* Also broadcast over Reticulum. APRS-IS only carries the beacon to stations
   * whose area/budlist filter overlaps ours — two users on different networks
   * with no shared filter would never learn each other's npub/deliv and could
   * never start an encrypted/private chat. The RNS broadcast crosses NATs via
   * the public hubs; the receiver's RNS drain feeds this exact frame back into
   * ble_handle -> deliver_bulletin -> pk_intercept, same as the BLE path. */
  {
    char frame[220];
    ble_pack(frame, sizeof(frame), g_call, "#" PKBEACON_GROUP, body);
    hal_rns_broadcast(frame, s_len(frame));
  }
  /* Also publish a queryable callsign→npub(+RNS dests) identity to the reachable
   * NOSTR relays, so a peer can resolve us by callsign even if it never heard this
   * beacon — the basis for cold-start 1:1 (see do_convo_send / resolve_drain). */
  if (deliv[0]) {
    if (g_myrelay_n == 0) relay_pick();
    if (g_myrelay_n > 0) {
      char rj[RELAY_MAX * 80 + 16]; relays_json(g_myrelay, g_myrelay_n, rj, sizeof(rj));
      hal_relay_identity_publish(g_call, s_len(g_call), deliv, s_len(deliv),
                                 prop, s_len(prop), rj, s_len(rj));
    }
  }
  g_last_pkbeacon = hal_time_epoch();
}

/* ---- per-callsign mailbox (KV "m.<call>", lines "<from>|<text>") ---- */
static void mailbox_key(char *out, unsigned max, const char *call) {
  out[0] = 0; s_cat(out, "m.", max); s_cat(out, call, max);
}
static void mailbox_clear(const char *call) {
  char key[20]; mailbox_key(key, sizeof(key), call);
  hal_kv_delete(key, s_len(key));
}
/* The "<from>|<text>" body of a stored line is everything after the first '|'
 * (which separates the leading timestamp). Returns NULL if malformed. */
static const char *mail_line_body(const char *line) {
  const char *p = line; while (*p && *p != '|') p++;
  return (*p == '|') ? p + 1 : 0;
}
/* Dedup on the body (sender+text), ignoring the per-line timestamp. */
static int contains_body(const char *buf, const char *body) {
  unsigned bl = s_len(body);
  const char *p = buf;
  while (*p) {
    const char *e = p; while (*e && *e != '\n') e++;
    const char *b = mail_line_body(p);
    if (b && b <= e && (unsigned)(e - b) == bl) {
      int eq = 1; for (unsigned i = 0; i < bl; i++) if (b[i] != body[i]) { eq = 0; break; }
      if (eq) return 1;
    }
    p = (*e == '\n') ? e + 1 : e;
  }
  return 0;
}
/* Hold a message addressed to a heard station until it pulls its mail. Each line
 * is "<ts>|<from>|<text>" (ts = epoch when held) so a ?MAIL can window by age. */
static void mailbox_add(const char *call, const char *from, const char *text) {
  if (!call[0] || !from[0]) return;
  char key[20]; mailbox_key(key, sizeof(key), call);
  char buf[1300];
  uint32_t n = hal_kv_get(key, s_len(key), buf, sizeof(buf) - 1);
  buf[n] = 0;
  char body[420]; body[0] = 0;                    /* "<from>|<text>" (newline-free) */
  s_cat(body, from, sizeof(body)); s_cat(body, "|", sizeof(body));
  for (const char *t = text; *t && s_len(body) < sizeof(body) - 2; t++) {
    char c = (*t == '\n' || *t == '\r') ? ' ' : *t;
    char cc[2] = { c, 0 }; s_cat(body, cc, sizeof(body));
  }
  if (n && contains_body(buf, body)) return;      /* dedup ignoring ts */
  char line[440]; line[0] = 0;
  { char tb[12]; u_itoa((unsigned)hal_time_epoch(), tb); s_cat(line, tb, sizeof(line)); }
  s_cat(line, "|", sizeof(line)); s_cat(line, body, sizeof(line));
  char out[1500]; out[0] = 0;
  if (n) { s_cat(out, buf, sizeof(out)); s_cat(out, "\n", sizeof(out)); }
  s_cat(out, line, sizeof(out));
  char *o = out;                                  /* cap: drop oldest lines */
  while (s_len(o) > 1100) {
    char *nl = o; while (*nl && *nl != '\n') nl++;
    if (*nl == '\n') o = nl + 1; else break;
  }
  hal_kv_set(key, s_len(key), o, s_len(o));
}

#define MAIL_QUERY_CAP 30   /* most-recent messages delivered per ?MAIL pull */

/* A heard station broadcast "?MAIL <days> <nonce>": deliver the messages we hold
 * for it that are within the requested day-window (default 7), newest first,
 * capped, each as a 1:1 frame from the original sender. Delivered lines are
 * removed; out-of-window lines are kept for a possible later, wider pull. */
static void handle_mail_query(const char *from, const char *text) {
  if (s_eq(from, g_call)) return;
  sdev_touch(from);
  int days = text ? to_int(text) : 0;             /* leading integer = look-back days */
  if (days <= 0) days = 7;
  uint64_t now = hal_time_epoch();
  uint64_t cutoff = (now > (uint64_t)days * 86400) ? now - (uint64_t)days * 86400 : 0;

  char key[20]; mailbox_key(key, sizeof(key), from);
  char buf[1400];
  uint32_t n = hal_kv_get(key, s_len(key), buf, sizeof(buf) - 1);
  if (n == 0) return;
  buf[n] = 0;

  /* First pass: split into NUL-terminated lines; collect in-window pointers
   * (oldest..newest as stored), accumulate out-of-window lines into `keep`. */
  const char *lines[64]; int nl = 0;
  char keep[1500]; keep[0] = 0;                   /* out-of-window lines, kept */
  char *p = buf;
  while (*p && nl < 64) {
    char *e = p; while (*e && *e != '\n') e++;
    int had_nl = (*e == '\n');
    *e = 0;                                        /* terminate this line */
    uint64_t ts = (uint64_t)(unsigned)to_int(p);  /* leading ts */
    if (ts >= cutoff) { lines[nl++] = p; }
    else { if (keep[0]) s_cat(keep, "\n", sizeof(keep)); s_cat(keep, p, sizeof(keep)); }
    p = had_nl ? e + 1 : e;
  }
  /* Deliver the newest MAIL_QUERY_CAP in-window (they sit at the tail). */
  int start = (nl > MAIL_QUERY_CAP) ? nl - MAIL_QUERY_CAP : 0;
  for (int i = start; i < nl; i++) {
    const char *body = mail_line_body(lines[i]);
    if (!body) continue;
    char mfrom[16] = ""; const char *bar = body; int bi = 0;
    while (*bar && *bar != '|' && bi < 15) mfrom[bi++] = *bar++;
    mfrom[bi] = 0;
    const char *mtext = (*bar == '|') ? bar + 1 : "";
    if (mfrom[0]) ble_tx_from(mfrom, from, mtext);
  }
  /* Keep out-of-window lines plus any in-window ones we didn't deliver (cap). */
  for (int i = 0; i < start; i++) {
    if (keep[0]) s_cat(keep, "\n", sizeof(keep));
    s_cat(keep, lines[i], sizeof(keep));
  }
  if (keep[0]) hal_kv_set(key, s_len(key), keep, s_len(keep));
  else mailbox_clear(from);
}

/* Route one APRS-IS TNC2 line to the UI; bridge to BLE when relaying. */
/* File a received group bulletin into the right conversation(s):
 *   - global (#NAME*) when subscribed — always (it arrived because we asked for
 *     the group worldwide, or it's in range);
 *   - local (#NAME) when subscribed AND the sender is within radius, OR when
 *     only the local view is subscribed;
 *   - if neither is subscribed, surface it once under the local id (discovery).
 * [within] = sender known to be inside our radius; [via] = "NET"/"BLE". */
static void deliver_bulletin(const char *gname, const char *from,
                             const char *text, int within, const char *via) {
  char nm[8]; int nj = 0;                 /* clean group name (no '#'/'*') */
  for (int i = 0; gname[i] && gname[i] != '*' && nj < 6; i++) nm[nj++] = gname[i];
  nm[nj] = 0;
  if (!nm[0]) return;
  /* Belt-and-braces self-drop: our own bulletin boomeranged by an iGate /
   * digipeater must never render as an incoming post or fire a notification,
   * even if an upstream mine-check missed it (e.g. a stale g_call). */
  if (is_self_call(from)) return;
  /* NOSTR key beacon: record the sender's pubkey and stop (not a chat). */
  if (pk_intercept(nm, from, text)) return;
  /* Strip any XPRS signature for the preview / like detection; convo_deliver
   * still gets the full text and re-verifies the signature. */
  char core[400]; char sg[80]; const char *cbody = text;
  if (sig_split(text, core, sizeof(core), sg, sizeof(sg))) cbody = core;
  /* A like vote is silent (no notification): convo_deliver registers it. */
  int is_like; char ltgt[5]; { int u; is_like = like_parse(cbody, ltgt, &u); }
  char par[5]; const char *disp_body; thread_parse(cbody, par, &disp_body);
  /* A followed station's like is a non-message event, so surface it in the
   * Activity feed here. Posts/replies to FEED reach Activity below; group/DM
   * chatter is intentionally NOT shown in Activity (Messages tab only). */
  (void)par;
  if (is_following(from) && is_like) activity_capture(from, "", "liked a post", via);
  /* The FEED group IS the Activity stream: it is a public broadcast, NOT a
   * Messages conversation, so route it straight to the Activity feed regardless
   * of group subscription (you never "subscribe" to FEED), and notify so a
   * backgrounded device still alerts. Our own posts loop back as `mine` and are
   * dropped upstream in route_frame, so this only fires for others' posts. */
  if (s_eq(nm, FEED_GROUP)) {
    if (is_like) {
      /* A like vote on an Activity post — tally it (don't show as a post). */
      char tg[5]; int ul;
      if (like_parse(cbody, tg, &ul)) activity_react_emit(tg, from, !ul, 0);
    } else {
      double flat = 0, flon = 0; pos_get(from, &flat, &flon);
      activity_feed("", from, disp_body, via, flat, flon, par);
      char fprev[160]; s_cpy(fprev, from, sizeof(fprev));
      s_cat(fprev, ": ", sizeof(fprev)); s_cat(fprev, disp_body, sizeof(fprev));
      notify_msg("Activity", from, disp_body, fprev);
    }
    return;
  }
  char lid[14]; lid[0] = '#'; s_cpy(lid + 1, nm, sizeof(lid) - 1);
  char gid[16]; s_cpy(gid, lid, sizeof(gid)); s_cat(gid, "*", sizeof(gid));
  /* The channel-class switches gate delivery itself, not just the rail: a
   * disabled class must not keep accumulating invisible unread. */
  int has_g = convo_known(gid) && g_chan_global;
  int has_l = convo_known(lid) && g_chan_local;
  if (!has_g && !has_l) return;            /* only listen to groups we subscribed */
  char preview[140] = ""; s_cpy(preview, from, sizeof(preview)); s_cat(preview, ": ", sizeof(preview));
  s_cat(preview, disp_body, sizeof(preview));
  if (has_g) {                              /* global: every bulletin for the group */
    /* Notify only on a freshly-delivered bubble — recurring/duplicate bulletins
     * return 0 and stay silent. */
    if (convo_deliver(gid, "in", from, text, preview, via) && !is_like)
      notify_msg(gid, from, cbody, preview);
  }
  /* Local: a nearby sender, OR — when no global pull is active (g/BLN* off) —
   * trust the region filter that the bulletin is in-range. */
  if (has_l && (within || !any_global_group())) {
    if (convo_deliver(lid, "in", from, text, preview, via) && !is_like)
      notify_msg(lid, from, cbody, preview);
  }
}

/* A standalone XPRS signature line: "~" + exactly 60 base85 chars. The signed
 * body's word-split puts the signature on its own final line, so this marks the
 * end of a multi-line signed message. */
static int is_sig_line(const char *t) {
  if (t[0] != '~' || s_len(t) != SIG_B85_LEN + 1) return 0;
  for (int i = 1; i <= SIG_B85_LEN; i++) if (!is_b85(t[i])) return 0;
  return 1;
}

/* ── Multi-line bulletin reassembly (APRS-IS) ─────────────────────────────
 * aprs_send_bulletin_multi splits a long body (incl. a signed message, whose
 * 60-char signature is its own final line) across BLN0..BLNk; rejoin them.
 * Buffer lines per (from,group), keyed by line id, flush after a brief idle,
 * joining the contiguous run from line 0 with single spaces (matching the
 * splitter). Single-line bulletins flush the same way. BLE arrives whole. */
#define RA_MAX 4
#define RA_FLUSH 2            /* seconds idle before flushing */
typedef struct {
  int used; char from[16]; char grp[8]; char line[10][72];
  int seen; uint64_t t; int within; char via[4];
} ra_t;
static ra_t g_ra[RA_MAX];
static void ra_emit(ra_t *e) {
  char full[720]; full[0] = 0;
  for (int i = 0; i < 10; i++) {
    if (!(e->seen & (1 << i))) break;
    if (full[0]) s_cat(full, " ", sizeof(full));
    s_cat(full, e->line[i], sizeof(full));
  }
  e->used = 0; e->seen = 0;
  if (!full[0]) return;
  /* iGate IS->BLE: relay the REASSEMBLED bulletin as ONE compact frame (BLE
   * receivers deliver bulletins whole — a per-line relay showed each APRS
   * 67-char fragment as its own post). Only for NET-arrived bulletins (a
   * BLE-arrived one is already on the air; re-airing it would loop). Skip a
   * frame that doesn't fit the compact BLE buffer whole: truncating would
   * re-create exactly the corrupted-fragment posts this path is fixing. */
  if (g_ble_relay && g_ble_on && s_eq(e->via, "NET")) {
    char convo[12]; convo[0] = '#'; s_cpy(convo + 1, e->grp, sizeof(convo) - 1);
    if (s_len(e->from) + 1 + s_len(convo) + 1 + s_len(full) < 218)
      ble_tx_from(e->from, convo, full);
  }
  deliver_bulletin(e->grp, e->from, full, e->within, e->via);
}
static void ra_add(const char *grp, const char *from, char line_id,
                   const char *text, int within, const char *via) {
  int idx = (int)(line_id - '0'); if (idx < 0 || idx > 9) idx = 0;
  ra_t *e = 0;
  for (int i = 0; i < RA_MAX; i++)
    if (g_ra[i].used && s_eq(g_ra[i].from, from) && s_eq(g_ra[i].grp, grp)) { e = &g_ra[i]; break; }
  if (!e) {
    for (int i = 0; i < RA_MAX; i++) if (!g_ra[i].used) { e = &g_ra[i]; break; }
    if (!e) { e = &g_ra[0]; for (int i = 1; i < RA_MAX; i++) if (g_ra[i].t < e->t) e = &g_ra[i]; ra_emit(e); }
    e->used = 1; e->seen = 0; e->within = 0;
    s_cpy(e->from, from, sizeof(e->from)); s_cpy(e->grp, grp, sizeof(e->grp));
    s_cpy(e->via, via, sizeof(e->via));
  }
  s_cpy(e->line[idx], text, sizeof(e->line[idx]));
  e->seen |= (1 << idx); e->t = hal_time_epoch();
  if (within) e->within = 1;
}
/* Flush after a brief idle (RA_FLUSH seconds with no new line for that entry):
 * the lines of a multi-line bulletin (e.g. "…file:…" on BLN0 and "ih:… pa:…" on
 * BLN1) can arrive in DIFFERENT poll cycles over APRS-IS, so flushing every
 * cycle would emit BLN0 alone — splitting a media token from its ih:/pa: hints
 * and breaking the auto-fetch. Waiting for the entry to go idle lets the
 * remaining lines arrive and reassemble; a single-line bulletin just waits the
 * same short idle. */
static void ra_flush(void) {
  uint64_t now = hal_time_epoch();
  for (int i = 0; i < RA_MAX; i++)
    if (g_ra[i].used && now - g_ra[i].t >= RA_FLUSH) ra_emit(&g_ra[i]);
}

/* ── Multi-line direct-message reassembly (APRS-IS) ───────────────────────
 * aprs_send_message_multi splits a long DM into parts with consecutive seq;
 * a signed DM's last part is a pure signature line. Buffer parts per sender,
 * flush after a brief idle: each run from a body up to a signature line is one
 * signed message (rejoined); any trailing run with no signature line is
 * delivered as separate plain messages (no spurious merging of normal chat). */
#define DA_MAX 6
#define DA_PARTS 16        /* an encrypted message can span ~10 APRS lines */
/* part[] holds each received line. APRS-IS/BLE split a long message into ≤67-char
 * lines (each fits easily), but a Reticulum datagram carries the WHOLE wire
 * ("ENC1:<blob> ~<60-char sig>", ~110+ chars) in ONE frame — so a part must be
 * big enough to hold a full single-frame wire, else it is truncated and the
 * signature/ciphertext is corrupted (decrypt fails). 256 covers it. */
typedef struct { int used; char from[16]; char via[4]; char part[DA_PARTS][256]; int n; uint64_t t; } da_t;
static da_t g_da[DA_MAX];
static void trc(const char *tag, const char *a, const char *b) {
  char t[160]; s_cpy(t, "[trc] ", sizeof(t)); s_cat(t, tag, sizeof(t));
  s_cat(t, " ", sizeof(t)); s_cat(t, a, sizeof(t));
  if (b && b[0]) { s_cat(t, " / ", sizeof(t)); s_cat(t, b, sizeof(t)); }
  hal_log(6, t, s_len(t));
}

static void da_emit_one(const char *from, const char *full, const char *via) {
  char prev[256], sg[80]; const char *pv = full;
  if (sig_split(full, prev, sizeof(prev), sg, sizeof(sg))) pv = prev;
  trc("da_emit", from, full);
  int r = convo_deliver(from, "in", from, full, pv, via);
  trc(r ? "delivered" : "DROPPED", from, full);
}
/* Buffer one direct-message part, keyed by (from, transport). A message that
 * arrives over BOTH transports (directly from APRS-IS AND re-broadcast by a BLE
 * iGate) is reassembled per-transport and dedups in convo_deliver — shown once. */
static void da_add(const char *from, const char *text, const char *via) {
  da_t *e = 0;
  for (int i = 0; i < DA_MAX; i++)
    if (g_da[i].used && s_eq(g_da[i].from, from) && s_eq(g_da[i].via, via)) { e = &g_da[i]; break; }
  if (!e) {
    for (int i = 0; i < DA_MAX; i++) if (!g_da[i].used) { e = &g_da[i]; break; }
    if (!e) { e = &g_da[0]; e->used = 0; e->n = 0; }   /* spill: drop oldest slot (rare) */
    e->used = 1; e->n = 0;
    s_cpy(e->from, from, sizeof(e->from)); s_cpy(e->via, via, sizeof(e->via));
  }
  if (e->n < DA_PARTS) s_cpy(e->part[e->n++], text, sizeof(e->part[0]));
  e->t = hal_time_epoch();
}
/* Decide whether a buffered entry is ready to deliver. A complete single plain
 * message (one short, non-ENC, non-signature part) flushes immediately — no
 * delay. A multi-part message (signed/encrypted, whose parts may arrive across
 * poll cycles via APRS-IS) is held until its trailing signature line arrives;
 * an idle safety net flushes anything stuck after ~2s. */
static int da_ready(da_t *d, uint64_t now) {
  if (d->n == 0) return 1;
  const char *last = d->part[d->n - 1];
  if (is_sig_line(last)) return 1;                 /* complete signed/encrypted */
  int enc_head = (s_len(d->part[0]) > 5 && d->part[0][0]=='E' && d->part[0][1]=='N'
                  && d->part[0][2]=='C' && d->part[0][3]=='1' && d->part[0][4]==':');
  if (d->n == 1 && !enc_head && s_len(last) < 66) return 1; /* plain short single */
  return now - d->t >= 2;                           /* idle safety net */
}
static void da_flush(void) {
  uint64_t now = hal_time_epoch();
  for (int x = 0; x < DA_MAX; x++) {
    if (!g_da[x].used || !da_ready(&g_da[x], now)) continue;
    da_t *d = &g_da[x]; int i = 0;
    while (i < d->n) {
      int j = i; while (j < d->n && !is_sig_line(d->part[j])) j++;
      if (j < d->n) {                       /* parts i..j-1 = body, j = signature */
        if (j == i) { i = j + 1; continue; }  /* lone signature fragment → drop */
        char full[1200]; full[0] = 0;
        for (int k = i; k <= j; k++) { if (full[0]) s_cat(full, " ", sizeof(full)); s_cat(full, d->part[k], sizeof(full)); }
        da_emit_one(d->from, full, d->via); i = j + 1;
      } else {                              /* no signature → deliver separately */
        for (int k = i; k < d->n; k++) da_emit_one(d->from, d->part[k], d->via);
        i = d->n;
      }
    }
    d->used = 0; d->n = 0;
  }
}

/* Acknowledge a received line-numbered direct message so the sender's client
 * stops retransmitting it (APRS clients resend the same message ~5x until they
 * receive an ack — that repetition was firing repeated arrivals/notifications).
 * The ack carries NO message number itself:
 *   "<me>>APRS,TCPIP*::<SENDER padded to 9>:ack<msgid>" */
static void send_ack(const char *to, const char *msgid) {
  if (!to[0] || !msgid[0] || g_sock < 0 || !g_logged) return;
  char dest[10]; int i = 0;
  for (; to[i] && i < 9; i++) dest[i] = up(to[i]);
  dest[i] = 0;
  while (s_len(dest) < 9) s_cat(dest, " ", sizeof(dest));
  char line[64];
  s_cpy(line, g_call, sizeof(line));
  s_cat(line, ">APRS,TCPIP*::", sizeof(line));
  s_cat(line, dest, sizeof(line));
  s_cat(line, ":ack", sizeof(line));
  s_cat(line, msgid, sizeof(line));
  aprs_send_raw(g_sock, line);
}

static void route_frame(const char *line) {
  unsigned fh = sig_hash("f", "", line);
  if (fseen_has(fh)) return;
  fseen_add(fh);

  aprs_packet_t p;
  if (!aprs_parse(line, &p)) return;
  if (is_self_call(p.from)) return;

  if (p.type == APRS_POSITION && p.has_pos) {
    push_marker(p.from, p.lat, p.lon, 0, p.comment);
    pos_set(p.from, p.lat, p.lon);
    if (convo_known(p.from)) convo_badge_only(p.from);
    if (p.comment[0]) {
      char meta[24] = ""; distance_to(p.lat, p.lon, meta, sizeof(meta));
      if (!geo_dup(p.from, p.comment))
        chat_append("geochat", "", "in", p.from, p.comment, "pos", 0, meta, p.lat, p.lon, "NET");
      /* Followed station's status/geo-chat comment → Activity feed. A ">>"
       * geo-chat message is shown as a plain post; anything else as a status. */
      if (is_following(p.from)) {
        const char *c = p.comment;
        if (c[0] == '>' && c[1] == '>') { c += 2; while (*c == ' ') c++; if (c[0]) activity_capture(p.from, "", c, "NET"); }
        else { char t[300]; s_cpy(t, "status: ", sizeof(t)); s_cat(t, c, sizeof(t)); activity_capture(p.from, "", t, "NET"); }
      }
    }
  } else if (p.type == APRS_MESSAGE) {
    /* An incoming ack<seq> for a message WE sent → mark that bubble delivered
     * (standard APRS ack; APRSdroid speaks this too). rej<seq> is not delivery. */
    if (is_ack_text(p.text)) {
      int forme = 1;
      for (int i = 0; g_call[i] || p.addressee[i]; i++)
        if (up(g_call[i]) != up(p.addressee[i])) { forme = 0; break; }
      if (forme && p.text[0] == 'a') {
        const char *am = ackmap_get(to_int(p.text + 3));
        if (am) convo_status_emit(am, "delivered");
      }
      return;   /* acks never route further / render */
    }
    if (p.text[0] && !is_ack_text(p.text)) {
      if (p.is_bulletin) {
        /* Buffer the line; multi-line bulletins are reassembled before delivery. */
        ra_add(p.group, p.from, p.bulletin_id ? p.bulletin_id : '0', p.text,
               within_radius(p.from), "NET");
        /* iGate IS->BLE relay happens in ra_emit, AFTER reassembly: relaying
         * each BLNx line as its own BLE frame turned one long post into
         * several fragment posts on every BLE receiver (the BLE path delivers
         * bulletins whole, with no line id to rejoin them). */
      } else {
        int amine = 1;
        for (int i = 0; g_call[i] || p.addressee[i]; i++) {
          if (up(g_call[i]) != up(p.addressee[i])) { amine = 0; break; }
        }
        /* ?FOLLOW / ?UNFOLLOW notifications are control traffic — record the
         * follower and keep them off the Live tab / chat / notifications. */
        if (amine && follow_intercept(p.from, p.text)) return;
        if (amine && priv_intercept(p.from, p.text)) return;
        if (amine && rly_intercept(p.from, p.text)) return;
        /* Receipts are control traffic addressed to the SENDER, but broadcast
         * transports (BLE/RNS) let any station overhear them — consume them for
         * everyone (not just the addressee) so they never render as Live/chat. */
        if (rcpt_intercept(p.from, p.text)) return;
        /* A bare signature line is a continuation fragment, not a message:
         * keep it off the Live tab + notifications; da_ reassembles it. */
        int sigln = is_sig_line(p.text);
        char meta[24] = ""; double slat = 0, slon = 0;
        if (pos_get(p.from, &slat, &slon)) distance_to(slat, slon, meta, sizeof(meta));
        if (!sigln && !geo_dup(p.from, p.text))
          chat_append("geochat", "", "in", p.from, p.text, "msg", 0, meta, slat, slon, "NET");
        /* Buffer DM parts; multi-line (incl. signed) messages reassemble in da_.
         * The notification fires once after reassembly (in convo_deliver), not
         * here per-line, so a multi-line/encrypted DM alerts once. */
        if (amine) {
          da_add(p.from, p.text, "NET");
          /* Acknowledge the message so the sender's client stops retransmitting
           * it (APRS messages are resent ~5x until acked — that was the source
           * of repeated arrivals). Only line-numbered, non-ack messages. */
          if (p.msgid[0] && !sigln) send_ack(p.from, p.msgid);
        } else if (sdev_has(p.addressee))   /* store-and-forward for a heard station */
          mailbox_add(p.addressee, p.from, p.text);
        /* Bridge to BLE only for messages NOT addressed to us — we are the
         * endpoint of our own mail, so re-broadcasting it would just echo back
         * as a duplicate. Relay (general bridge) or store-and-forward to a heard
         * station are the only reasons to put a message on BLE. */
        if (g_ble_on && !amine && (g_ble_relay || sdev_has(p.addressee)))
          ble_tx_from(p.from, p.addressee, p.text);
      }
    }
  }
  /* NOTE: we deliberately do NOT relay APRS-IS position beacons onto BLE. The
   * iGate bridges MESSAGES (group bulletins + directed messages), not position
   * telemetry — flooding every internet beacon onto BLE made internet-only
   * stations appear on neighbours as if they were local BLE stations (their
   * geo-chat/beacon then carried a BLE origin tag, which was wrong). Messages
   * still gate both ways above; positions stay on the transport they arrived on. */
}

/* Handle one compact frame received over BLE; bridge to APRS-IS when relaying. */
/* ── Ping reach-test helpers ──────────────────────────────────────────── */

/* Per-id / per-(responder,id) dedup so each station answers + forwards a
 * given ping once, and forwards each pong once. */
#define PSEEN_MAX 96
static unsigned g_pseen[PSEEN_MAX];
static unsigned g_pseen_cnt = 0;
static int pseen_has(unsigned h) {
  unsigned n = g_pseen_cnt < PSEEN_MAX ? g_pseen_cnt : PSEEN_MAX;
  for (unsigned i = 0; i < n; i++) if (g_pseen[i] == h) return 1;
  return 0;
}
static void pseen_add(unsigned h) { g_pseen[g_pseen_cnt % PSEEN_MAX] = h; g_pseen_cnt++; }

/* Deferred digipeat queue: instead of rebroadcasting a received frame
 * immediately, hold it a short, per-frame-staggered delay (a few ticks) and
 * re-advertise when due. The stagger (derived from the frame hash so peers
 * pick different delays) cuts collisions and widens effective reach. */
#define RQ_MAX 16
static struct { char frame[300]; uint64_t due; int used; } g_rq[RQ_MAX];
static void rq_push(const char *frame, uint64_t due) {
  for (int i = 0; i < RQ_MAX; i++)
    if (!g_rq[i].used) { s_cpy(g_rq[i].frame, frame, sizeof(g_rq[i].frame)); g_rq[i].due = due; g_rq[i].used = 1; return; }
  /* queue full: drop (storm protection) */
}
static void rq_flush(uint64_t now) {
  for (int i = 0; i < RQ_MAX; i++)
    if (g_rq[i].used && now >= g_rq[i].due) { ble_send(g_rq[i].frame); g_rq[i].used = 0; }
}

/* Best position: live device GPS (hal_sensor_gps_*) if the host provides it,
 * else the configured station position. Returns 1 when a position is known. */
static int my_position(double *lat, double *lon) {
  int32_t la = hal_sensor_gps_lat();
  int32_t lo = hal_sensor_gps_lon();
  if (la != GPS_NA && lo != GPS_NA) {
    *lat = (double)la / 1e7; *lon = (double)lo / 1e7; return 1;
  }
  if (g_lat != 0.0 || g_lon != 0.0) { *lat = g_lat; *lon = g_lon; return 1; }
  *lat = 0; *lon = 0; return 0;
}

/* Append one line to a $type:"log" field. */
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

/* Compact "time since" into out (e.g. "12s", "5m", "3h", "2d", "-" if unknown). */
static void rel_time(uint64_t ts, char *out, unsigned sz) {
  if (ts == 0) { s_cpy(out, "-", sz); return; }
  uint64_t now = hal_time_epoch();
  uint64_t d = now > ts ? now - ts : 0;
  unsigned v; char unit;
  if (d < 60) { v = (unsigned)d; unit = 's'; }
  else if (d < 3600) { v = (unsigned)(d / 60); unit = 'm'; }
  else if (d < 86400) { v = (unsigned)(d / 3600); unit = 'h'; }
  else { v = (unsigned)(d / 86400); unit = 'd'; }
  char nb[12]; u_itoa(v, nb);
  out[0] = 0; s_cat(out, nb, sz); { char u[2] = { unit, 0 }; s_cat(out, u, sz); }
}

/* Rebuild the Keys list view from the callsign->pubkey database. Per station two
 * lines: "<callsign>  (<age>)" then the full npub. The host encodes the stored
 * base64url key to npub; the raw key stays in KV for encryption. */
static void pk_render(void) {
  log_clear("keys_list");
  if (g_pk_n == 0) { log_line("keys_list", "No public keys received yet."); return; }
  for (int i = 0; i < g_pk_n; i++) {
    char hdr[40]; hdr[0] = 0;
    s_cat(hdr, g_pk_call[i], sizeof(hdr));
    char age[12]; rel_time(g_pk_ts[i], age, sizeof(age));
    s_cat(hdr, "  (", sizeof(hdr)); s_cat(hdr, age, sizeof(hdr)); s_cat(hdr, ")", sizeof(hdr));
    log_line("keys_list", hdr);
    char npub[72];
    uint32_t nn = hal_npub(g_pk_key[i], s_len(g_pk_key[i]), npub, sizeof(npub) - 1);
    if (nn > 0 && nn < sizeof(npub)) { npub[nn] = 0; log_line("keys_list", npub); }
    else log_line("keys_list", g_pk_key[i]);   /* fallback: raw base64url key */
  }
}

/* One row of the Follows people list. [following] selects the trailing
 * button: "Following" (outlined, unfollows) vs "Follow back" (filled). */
static void people_item(char *m, unsigned sz, const char *call, int following) {
  s_cat(m, "{\"id\":\"", sz); jesc(m, sz, call);
  s_cat(m, "\",\"title\":\"", sz); jesc(m, sz, call);
  /* subtitle: known pubkey -> npub prefix + how long since their key beacon */
  char sub[64] = "";
  for (int k = 0; k < g_pk_n; k++) if (s_eq(g_pk_call[k], call)) {
    char npub[72];
    uint32_t nn = hal_npub(g_pk_key[k], s_len(g_pk_key[k]), npub, sizeof(npub) - 1);
    if (nn > 14) { npub[14] = 0; s_cat(sub, npub, sizeof(sub)); s_cat(sub, "...", sizeof(sub)); }
    char age[12]; rel_time(g_pk_ts[k], age, sizeof(age));
    if (sub[0]) s_cat(sub, " - ", sizeof(sub));
    s_cat(sub, "heard ", sizeof(sub)); s_cat(sub, age, sizeof(sub));
    s_cat(sub, " ago", sizeof(sub));
    break;
  }
  s_cat(m, "\",\"subtitle\":\"", sz); jesc(m, sz, sub);
  s_cat(m, "\",\"tags\":[", sz);
  for (int k = 0; k < g_follow_n; k++) if (s_eq(g_follow[k], call)) {
    const char *t = g_ftag[k]; int first = 1; char one[48]; int oi = 0;
    for (int x = 0;; x++) {
      char ch = t[x];
      if (ch == ' ' || ch == 0) {
        if (oi) {
          one[oi] = 0;
          if (!first) s_cat(m, ",", sz);
          s_cat(m, "\"", sz); jesc(m, sz, one); s_cat(m, "\"", sz);
          first = 0; oi = 0;
        }
        if (!ch) break;
      } else if (oi < 47) one[oi++] = ch;
    }
    break;
  }
  s_cat(m, "],", sz);
  if (following)
    s_cat(m, "\"action\":\"row_unfollow\",\"actionLabel\":\"Following\","
             "\"actionStyle\":\"outlined\"}", sz);
  else
    s_cat(m, "\"action\":\"row_follow\",\"actionLabel\":\"Follow back\","
             "\"actionStyle\":\"filled\"}", sz);
}

/* Push the Follows people list (Following | Followers sections) to the host's
 * social-style list view. */
static char g_people[8192];
static void follow_render(void) {
  char *m = g_people; const unsigned sz = sizeof(g_people);
  m[0] = 0;
  s_cat(m, "{\"type\":\"ui.people.set\",\"field\":\"follows_list\",\"sections\":[", sz);
  s_cat(m, "{\"title\":\"Following\",\"items\":[", sz);
  for (int i = 0; i < g_follow_n; i++) {
    if (i) s_cat(m, ",", sz);
    people_item(m, sz, g_follow[i], 1);
  }
  s_cat(m, "]},{\"title\":\"Followers\",\"items\":[", sz);
  for (int i = 0; i < g_follower_n; i++) {
    if (i) s_cat(m, ",", sz);
    people_item(m, sz, g_follower[i], is_following(g_follower[i]));
  }
  s_cat(m, "]}]}", sz);
  hal_msg_send(m, s_len(m));
}

/* Station profile sheet: identity facts + instant Follow/Unfollow/tags
 * actions. Rendered by the host's generic prompt (chips act on tap). */
/* ── Nearby: who is within LOCAL reach, and when they were last seen ─────
 *
 * A mesh radio's first question is not "who exists" but "who can I touch from
 * here, right now". That answer lives in three places and nowhere together:
 *
 *   1. hal_rns_nodes with {"localOnly":true} — THE SAME CALL the Reticulum
 *      wapp makes for its graph, so the two can never disagree about who is
 *      local. The host decides locality (see rns_iface_kind.dart): anything
 *      heard on something other than the internet, xprs or not.
 *   2. hal_mesh_devices     — the BLE street mesh's own neighbour registry.
 *   3. the pubkey beacons this wapp already hears over BLE broadcast / APRS /
 *      RNS (g_pk_call + g_pk_ts) — the radio side, callsign-keyed.
 *
 * NOT hal_people_directory. That answers "who can I message" — deliberately
 * not liveness-gated, and gated instead on having announced an LXMF delivery
 * address — which is the wrong question here and is why a LAN device could sit
 * on the graph and be missing from this list. It is still the right source for
 * the New-chat picker and search.
 *
 * They are merged into ONE table keyed by identity (callsign, else key), each
 * row carrying the LATEST sighting and the union of the transports it arrived
 * on. Presence is per-person, not per-radio: the same neighbour heard over BLE
 * and over LAN is one row with two tags, not two rows.
 *
 * Nobody is dropped for going quiet. Active = seen within ten minutes; older
 * rows stay, greyed, with the age spelled out ("seen 40m ago") — "who was here
 * earlier" is the second question a presence list has to answer, and deleting
 * the row answers it with a lie. The table is persisted for the same reason.
 */
#define NEAR_MAX 64
#define NEAR_ACTIVE_SEC 600       /* "active" = heard within ten minutes */
#define NEAR_KEEP_SEC (7 * 24 * 3600)  /* a week; older sightings are history */
/* How long a local sighting keeps counting as "here". Longer than the active
 * window on purpose: a neighbour stays audible through a hub after they leave
 * the room, and a lost announce race must never move them out of it. */
#define NEAR_LOCAL_STICKY_SEC 1800
#define NEAR_SCAN_SEC 5           /* rescan cadence while the screen is open */
#define TR_LAN   1
#define TR_BLE   2
#define TR_RNS   4
#define TR_LXMF  8
#define TR_RADIO 16

static char     g_near_call[NEAR_MAX][24];
static char     g_near_key[NEAR_MAX][80];
static char     g_near_name[NEAR_MAX][40];
static char     g_near_id[NEAR_MAX][36];   /* RNS identity hex, when known    */
static uint64_t g_near_ts[NEAR_MAX];
/* Last sighting on a LOCAL path. Separate from g_near_ts because a neighbour
 * stays audible through a hub after it leaves the room: "still reachable" and
 * "still HERE" are different facts and the list means the second one. */
static uint64_t g_near_lts[NEAR_MAX];
static unsigned g_near_tr[NEAR_MAX];
static int      g_near_hops[NEAR_MAX];
static int      g_near_n = 0;

static char     g_near_q[40] = "";    /* the list's search box */
static int      g_near_open = 0;      /* screen on screen: keep it refreshed */
static uint64_t g_near_scan_at = 0;
static uint32_t g_near_hash = 0;
static uint32_t g_near_stats_hash = 0;
static char     g_near_json[51200];
static char     g_near_out[16384];

/* A JSON number as 64 bits: `lastSeen` is epoch MILLIseconds on the host side,
 * which overflows the 32-bit jint() and lands in 1970. */
static uint64_t jint64(const char *buf, const char *key) {
  char pat[80]; pat[0] = '"'; pat[1] = 0;
  s_cat(pat, key, sizeof(pat)); s_cat(pat, "\":", sizeof(pat));
  unsigned pl = s_len(pat);
  for (const char *p = buf; *p; p++) {
    int ok = 1;
    for (unsigned i = 0; i < pl; i++) if (p[i] != pat[i]) { ok = 0; break; }
    if (!ok) continue;
    p += pl;
    while (*p == ' ' || *p == '"') p++;
    uint64_t v = 0; int any = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (uint64_t)(*p - '0'); p++; any = 1; }
    return any ? v : 0;
  }
  return 0;
}

/* Host timestamps arrive in seconds OR milliseconds depending on the source.
 * Normalise to seconds — a millisecond value read as seconds puts the sighting
 * fifty thousand years from now, and every row would read "active". */
static uint64_t near_secs(uint64_t t) { return t > 100000000000ULL ? t / 1000 : t; }

static void near_upper(const char *in, char *out, unsigned sz) {
  unsigned j = 0;
  for (unsigned i = 0; in[i] && j < sz - 1; i++) out[j++] = up(in[i]);
  out[j] = 0;
}

/* Merge one sighting into the table. Identity is the callsign when we have it
 * (the human name for a person) and the key otherwise; a row learns its key or
 * callsign later without splitting in two. */
static void near_touch(const char *call, const char *key, const char *name,
                       uint64_t ts, unsigned tr, int hops, const char *idhex,
                       uint64_t local_ts, const char *alias) {
  char uc[24]; near_upper(call ? call : "", uc, sizeof(uc));
  if (!uc[0] && !(key && key[0]) && !(idhex && idhex[0])) return;
  if (uc[0] && s_eq(uc, g_call)) return;            /* ourselves is not "nearby" */
  int idx = -1;
  /* Callsign first: it is the PERSON, and it is what lets a row learned from
   * the graph (identity-keyed) merge with one heard as a beacon (callsign
   * only) instead of showing the same neighbour twice. */
  for (int i = 0; i < g_near_n; i++) {
    if (uc[0] && s_eq(g_near_call[i], uc)) { idx = i; break; }
    if (key && key[0] && s_eq(g_near_key[i], key)) { idx = i; break; }
    if (idhex && idhex[0] && s_eq(g_near_id[i], idhex)) { idx = i; break; }
    /* A device has several addresses (npub, LXMF dest, identity). A row we
     * stored under one of them is the SAME device arriving under another —
     * match on any of them or the user sees one neighbour twice. */
    if (alias && alias[0] && s_eq(g_near_key[i], alias)) { idx = i; break; }
  }
  if (idx < 0) {
    if (g_near_n >= NEAR_MAX) {
      /* Full: evict the oldest sighting, which is the least useful row. */
      int oldest = 0;
      for (int i = 1; i < g_near_n; i++) if (g_near_ts[i] < g_near_ts[oldest]) oldest = i;
      idx = oldest;
      g_near_call[idx][0] = 0; g_near_key[idx][0] = 0; g_near_name[idx][0] = 0;
      g_near_id[idx][0] = 0;
      g_near_ts[idx] = 0; g_near_lts[idx] = 0; g_near_tr[idx] = 0;
      g_near_hops[idx] = 0;
    } else {
      idx = g_near_n++;
      g_near_call[idx][0] = 0; g_near_key[idx][0] = 0; g_near_name[idx][0] = 0;
      g_near_id[idx][0] = 0;
      g_near_ts[idx] = 0; g_near_lts[idx] = 0; g_near_tr[idx] = 0;
      g_near_hops[idx] = 0;
    }
  }
  if (uc[0] && !g_near_call[idx][0]) s_cpy(g_near_call[idx], uc, sizeof(g_near_call[0]));
  if (key && key[0] && !g_near_key[idx][0]) s_cpy(g_near_key[idx], key, sizeof(g_near_key[0]));
  if (name && name[0] && !g_near_name[idx][0]) s_cpy(g_near_name[idx], name, sizeof(g_near_name[0]));
  if (idhex && idhex[0] && !g_near_id[idx][0])
    s_cpy(g_near_id[idx], idhex, sizeof(g_near_id[0]));
  if (ts > g_near_ts[idx]) g_near_ts[idx] = ts;
  if (local_ts > g_near_lts[idx]) g_near_lts[idx] = local_ts;
  g_near_tr[idx] |= tr;
  if (hops > 0 && (g_near_hops[idx] == 0 || hops < g_near_hops[idx])) g_near_hops[idx] = hops;
}

/* Fold rows that turn out to be the same device.
 *
 * A row can be created before we know its callsign (identity-keyed, from the
 * graph) and another after (callsign-keyed, from a beacon or an older build's
 * saved table). Once both are present the duplicate is visible to the user —
 * the same neighbour listed twice, one fresh and one stale — so collapse them
 * here rather than hoping every source arrives in a lucky order. */
static void near_dedupe(void) {
  for (int i = 0; i < g_near_n; i++) {
    for (int j = i + 1; j < g_near_n; j++) {
      int same = 0;
      if (g_near_call[i][0] && s_eq(g_near_call[i], g_near_call[j])) same = 1;
      else if (g_near_key[i][0] && s_eq(g_near_key[i], g_near_key[j])) same = 1;
      else if (g_near_id[i][0] && s_eq(g_near_id[i], g_near_id[j])) same = 1;
      /* Legacy rows carry no callsign at all — only an LXMF dest and the name
       * the peer announced, which IS its callsign. That is the only thread
       * tying them to the row the graph now provides. */
      else if (g_near_call[i][0] && s_eq(g_near_call[i], g_near_name[j])) same = 1;
      else if (g_near_call[j][0] && s_eq(g_near_call[j], g_near_name[i])) same = 1;
      if (!same) continue;
      /* Keep i, absorb j: newest timestamps, union of transports, and any
       * field the survivor is still missing. */
      if (g_near_ts[j] > g_near_ts[i]) g_near_ts[i] = g_near_ts[j];
      if (g_near_lts[j] > g_near_lts[i]) g_near_lts[i] = g_near_lts[j];
      g_near_tr[i] |= g_near_tr[j];
      if (!g_near_call[i][0]) s_cpy(g_near_call[i], g_near_call[j], sizeof(g_near_call[0]));
      if (!g_near_key[i][0]) s_cpy(g_near_key[i], g_near_key[j], sizeof(g_near_key[0]));
      if (!g_near_name[i][0]) s_cpy(g_near_name[i], g_near_name[j], sizeof(g_near_name[0]));
      if (!g_near_id[i][0]) s_cpy(g_near_id[i], g_near_id[j], sizeof(g_near_id[0]));
      if (g_near_hops[j] > 0 &&
          (g_near_hops[i] == 0 || g_near_hops[j] < g_near_hops[i])) {
        g_near_hops[i] = g_near_hops[j];
      }
      for (int k = j; k < g_near_n - 1; k++) {
        s_cpy(g_near_call[k], g_near_call[k + 1], sizeof(g_near_call[0]));
        s_cpy(g_near_key[k], g_near_key[k + 1], sizeof(g_near_key[0]));
        s_cpy(g_near_name[k], g_near_name[k + 1], sizeof(g_near_name[0]));
        s_cpy(g_near_id[k], g_near_id[k + 1], sizeof(g_near_id[0]));
        g_near_ts[k] = g_near_ts[k + 1];
        g_near_lts[k] = g_near_lts[k + 1];
        g_near_tr[k] = g_near_tr[k + 1];
        g_near_hops[k] = g_near_hops[k + 1];
      }
      g_near_n--;
      j--;
    }
  }
}

/* Anything not heard for a week is not "seen before", it is history: drop it
 * so the list stays a picture of this place rather than an ever-growing log. */
static void near_expire(void) {
  uint64_t now = hal_time_epoch();
  int w = 0;
  for (int i = 0; i < g_near_n; i++) {
    if (g_near_ts[i] && now > g_near_ts[i] &&
        now - g_near_ts[i] > NEAR_KEEP_SEC) {
      continue;
    }
    if (w != i) {
      s_cpy(g_near_call[w], g_near_call[i], sizeof(g_near_call[0]));
      s_cpy(g_near_key[w], g_near_key[i], sizeof(g_near_key[0]));
      s_cpy(g_near_name[w], g_near_name[i], sizeof(g_near_name[0]));
      s_cpy(g_near_id[w], g_near_id[i], sizeof(g_near_id[0]));
      g_near_ts[w] = g_near_ts[i];
      g_near_lts[w] = g_near_lts[i];
      g_near_tr[w] = g_near_tr[i];
      g_near_hops[w] = g_near_hops[i];
    }
    w++;
  }
  g_near_n = w;
}

/* Persisted so "seen before" survives a restart — a presence list that forgets
 * everyone on launch can only ever answer the easy half of the question. */
static void near_save(void) {
  static char buf[NEAR_MAX * 180];
  buf[0] = 0;
  for (int i = 0; i < g_near_n; i++) {
    char nb[24];
    s_cat(buf, g_near_call[i], sizeof(buf)); s_cat(buf, "|", sizeof(buf));
    s_cat(buf, g_near_key[i], sizeof(buf));  s_cat(buf, "|", sizeof(buf));
    s_cat(buf, g_near_name[i], sizeof(buf)); s_cat(buf, "|", sizeof(buf));
    u_itoa((unsigned)g_near_ts[i], nb); s_cat(buf, nb, sizeof(buf)); s_cat(buf, "|", sizeof(buf));
    u_itoa(g_near_tr[i], nb); s_cat(buf, nb, sizeof(buf)); s_cat(buf, "|", sizeof(buf));
    s_cat(buf, g_near_id[i], sizeof(buf)); s_cat(buf, "|", sizeof(buf));
    u_itoa((unsigned)g_near_lts[i], nb); s_cat(buf, nb, sizeof(buf));
    s_cat(buf, ";", sizeof(buf));
  }
  hal_kv_set("nearby", 6, buf, s_len(buf));
}

static void near_load(void) {
  static char buf[NEAR_MAX * 180];
  uint32_t n = hal_kv_get("nearby", 6, buf, sizeof(buf) - 1);
  if (n == 0 || n >= sizeof(buf)) return;
  buf[n] = 0;
  char rec[200]; unsigned c = 0;
  for (unsigned i = 0; buf[i]; i++) {
    if (buf[i] == ';') {
      rec[c] = 0;
      if (c > 2) {
        char call[24] = "", key[80] = "", name[40] = "", ts[24] = "", tr[16] = "";
        char idh[36] = "", lts[24] = "";
        char *dst[7]; unsigned cap[7];
        dst[0] = call; cap[0] = sizeof(call);
        dst[1] = key;  cap[1] = sizeof(key);
        dst[2] = name; cap[2] = sizeof(name);
        dst[3] = ts;   cap[3] = sizeof(ts);
        dst[4] = tr;   cap[4] = sizeof(tr);
        dst[5] = idh;  cap[5] = sizeof(idh);
        dst[6] = lts;  cap[6] = sizeof(lts);
        int f = 0; unsigned o = 0;
        for (unsigned k = 0; rec[k] && f < 7; k++) {
          if (rec[k] == '|') { dst[f][o] = 0; f++; o = 0; continue; }
          if (o < cap[f] - 1) dst[f][o++] = rec[k];
        }
        if (f < 7) dst[f][o] = 0;
        uint64_t rts = (uint64_t)to_int(ts);
        unsigned rtr = (unsigned)to_int(tr);
        uint64_t rlts = (uint64_t)to_int(lts);
        /* Rows written before locality was tracked: if they were heard on a
         * local transport, that sighting WAS local. Without this every
         * remembered device disappears on the first launch after the update. */
        if (!rlts && (rtr & (TR_LAN | TR_BLE | TR_RADIO))) rlts = rts;
        near_touch(call, key, name, rts, rtr, 0, idh, rlts, "");
      }
      c = 0;
    } else if (c < sizeof(rec) - 1) {
      rec[c++] = buf[i];
    }
  }
}

/* Recover a callsign from a display name.
 *
 * A node whose announce carried no nostr pubkey has no meta.callsign — the
 * host can only compose a label, e.g. "X1RD89" or "Nuno (X1RD89)". Without
 * this the same device arrives keyed by identity, fails to match the row we
 * already hold for that callsign, and the list shows one neighbour twice. */
static void near_callsign_from(const char *name, char *out, unsigned osz) {
  out[0] = 0;
  if (!name || !name[0]) return;
  const char *start = name;
  const char *open = 0;
  for (const char *p = name; *p; p++) if (*p == '(') open = p;
  if (open) start = open + 1;
  unsigned n = 0;
  char buf[16];
  for (const char *p = start; *p && *p != ')' && n < sizeof(buf) - 1; p++) {
    char c = up(*p);
    int alnum = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    if (!alnum) { n = 0; break; }          /* spaces etc: not a callsign */
    buf[n++] = c;
  }
  buf[n] = 0;
  if (n < 4 || n > 12 || buf[0] != 'X') return;
  s_cpy(out, buf, osz);
}

/* First occurrence of [needle] in [hay], or NULL. (No libc here.) */
static const char *s_find(const char *hay, const char *needle) {
  if (!needle[0]) return hay;
  for (const char *p = hay; *p; p++) {
    unsigned i = 0;
    while (needle[i] && p[i] == needle[i]) i++;
    if (!needle[i]) return p;
  }
  return 0;
}

/* The host already decided which kind of interface this was (one rule, shared
 * with the Reticulum graph — see rns_iface_kind.dart). Here we only pick the
 * chip to show for it. */
static unsigned near_iface_bits(const char *kind) {
  if (s_eq(kind, "ble")) return TR_BLE;
  if (s_eq(kind, "lan")) return TR_LAN;
  if (s_eq(kind, "lora") || s_eq(kind, "radio")) return TR_RADIO;
  return TR_RNS;
}

/* One sweep of all three sources. Cheap enough for a 5s cadence: two HAL reads
 * and a walk over a table we already keep. */
static void near_scan(void) {
  uint64_t now = hal_time_epoch();

  /* 1. Reticulum: the SAME snapshot the Reticulum wapp graphs, asked for the
   * local half of it. The host owns what "local" means (anything not reached
   * over the internet) so the graph and this list cannot drift apart. */
  {
    static const char kFilter[] = "{\"localOnly\":true,\"limit\":64}";
    int32_t n = hal_rns_nodes(kFilter, s_len(kFilter), g_near_json,
                              sizeof(g_near_json) - 1);
    if (n < 0) {
      /* Negated required size: the snapshot did not fit. Say so once instead
       * of silently losing every Reticulum row (the old `if (n > 0)` did). */
      static int moaned = 0;
      if (!moaned) {
        moaned = 1;
        hal_log(1, "[chat] nearby: rns snapshot too big for the buffer", 47);
      }
    } else if (n > 0) {
      g_near_json[n] = 0;
      /* Walk ONLY nodes[]. The snapshot is an object, so the first '{' is the
       * whole document; and edges/stats are objects too. Cut the buffer at
       * "edges" and start after "nodes":[ . Neither literal can appear inside
       * a JSON string value — the host escapes quotes. */
      char *ns = (char *)s_find(g_near_json, "\"nodes\":[");
      if (ns) {
        ns += 9;
        char *ne = (char *)s_find(ns, "\"edges\":");
        if (ne) *ne = 0;
        /* meta{} is the LAST field of a node, and a short slice truncates it
         * silently — taking callsign, npub and lastSeen with it. */
        char slice[1536];
        const char *p = fnd_next_obj(ns, slice, sizeof(slice));
        while (p) {
          char kind[12], id[40], label[48], call[24], nick[40], npub[80];
          char dest[40], iface[12], dm[8];
          jstr(slice, "kind", kind, sizeof(kind));
          jstr(slice, "id", id, sizeof(id));
          jstr(slice, "label", label, sizeof(label));
          jstr(slice, "callsign", call, sizeof(call));
          jstr(slice, "nickname", nick, sizeof(nick));
          jstr(slice, "npub", npub, sizeof(npub));
          jstr(slice, "lxmfDest", dest, sizeof(dest));
          jstr(slice, "ifaceKind", iface, sizeof(iface));
          jstr(slice, "dm", dm, sizeof(dm));
          if (s_eq(kind, "self")) { p = fnd_next_obj(p, slice, sizeof(slice)); continue; }
          uint64_t seen = near_secs(jint64(slice, "lastSeen"));
          /* Being in the snapshot already means the host judged it fresh, so a
           * missing stamp is a parse artefact, not an absent device. */
          if (!seen) seen = now;
          unsigned tr = near_iface_bits(iface);
          if (s_eq(dm, "lxmf")) tr |= TR_LXMF;
          /* The key is what we can ADDRESS them by: their npub, else their
           * LXMF delivery dest. The identity hex is the fallback join. */
          const char *key = npub[0] ? npub : (dest[0] ? dest : "");
          const char *name = nick[0] ? nick : label;
          /* No announced callsign? It is still in the label the host composed
           * — and it is what merges this row with the one we already hold. */
          if (!call[0]) near_callsign_from(name, call, sizeof(call));
          near_touch(call, key, name, seen, tr, jint(slice, "hops"), id, seen,
                     dest);
          p = fnd_next_obj(p, slice, sizeof(slice));
        }
      }
    }
  }

  /* 2. BLE street mesh neighbours (direct radio reach, right now). Sections
   * come back people-widget shaped; only the item objects carry an "id". */
  int32_t m = hal_mesh_devices(g_near_json, sizeof(g_near_json) - 1);
  if (m > 0) {
    g_near_json[m] = 0;
    char slice[600];
    const char *p = fnd_next_obj(g_near_json, slice, sizeof(slice));
    while (p) {
      char id[80], title[40];
      jstr(slice, "id", id, sizeof(id));
      jstr(slice, "title", title, sizeof(title));
      if (id[0]) near_touch(title[0] ? title : id, id, title, now, TR_BLE, 1, "", now, "");
      p = fnd_next_obj(p, slice, sizeof(slice));
    }
  }

  /* 3. The radio side we hear ourselves: pubkey beacons carry a callsign and
   * the time we last heard it, over BLE broadcast / APRS / RNS alike. */
  for (int i = 0; i < g_pk_n; i++) {
    if (!g_pk_ts[i]) continue;
    /* A beacon we heard OURSELVES is local by definition — that is what
     * hearing it means. */
    near_touch(g_pk_call[i], "", "", g_pk_ts[i], TR_RADIO, 1, "", g_pk_ts[i], "");
  }

  near_dedupe();
  near_expire();
  near_save();
}

/* ---- Which transport should carry this message? -------------------------
 *
 * Bluetooth store-and-forward is a LAST RESORT, not a second copy. When the
 * recipient is on the LAN or reachable over the internet, the message is
 * already going to arrive: airing it as well only burns the one advertising
 * slot this radio has and fills every nearby device's custody store with mail
 * nobody needs to carry.
 *
 * When there is NO path at all, the opposite is true: airing it is the only
 * hope. A neighbour that hears it holds it, and delivers when it later meets
 * the recipient. The envelope (from, to) stays readable so a custodian knows
 * whom it is for; the body is already ENC1 ciphertext when the contact is
 * encrypted, so "public metadata, private content" costs nothing extra.
 */
static char     g_reach_call[24] = "";
static int      g_reach_val = REACH_NONE;
static uint64_t g_reach_at = 0;

/* Is this callsign in the host's live node snapshot? Membership IS the
 * freshness test: the host only lists nodes heard inside its own online window
 * (~11 min). Deliberately NOT g_rns_dts/RNS_TTL, which is 48 h — a phone that
 * died yesterday would count as reachable and silence the whole feature. */
static int reach_on_net(const char *call) {
  char req[96];
  s_cpy(req, "{\"search\":\"", sizeof(req));
  s_cat(req, call, sizeof(req));
  s_cat(req, "\",\"xprsOnly\":true,\"limit\":8}", sizeof(req));
  int n = hal_rns_nodes(req, (int)s_len(req), g_near_json, sizeof(g_near_json));
  if (n <= 0) return 0;                 /* no answer, or overflow: assume not */
  g_near_json[n < (int)sizeof(g_near_json) ? n : (int)sizeof(g_near_json) - 1] = 0;
  /* Find the node, then check WHEN it was last heard. Membership alone is not
   * reachability: the host lists every node it has ever observed, so a peer
   * whose phone died yesterday is still in the array. Treating that as
   * "reachable over the internet" silences best-hope custody exactly for the
   * people who need it — which is what it did on the first live run. */
  char pat[40];
  s_cpy(pat, "\"callsign\":\"", sizeof(pat));
  s_cat(pat, call, sizeof(pat));
  s_cat(pat, "\"", sizeof(pat));
  const char *at = s_find(g_near_json, pat);
  if (!at) return 0;
  const char *ls = s_find(at, "\"lastSeen\":");
  if (!ls) return 0;
  ls += 11;
  uint64_t seen = 0;
  while (*ls >= '0' && *ls <= '9') { seen = seen * 10 + (uint64_t)(*ls - '0'); ls++; }
  if (!seen) return 0;
  uint64_t now_ms = (uint64_t)hal_time_epoch() * 1000ULL;
  /* The host's own online window: heard inside the last eleven minutes. */
  return now_ms > seen && (now_ms - seen) <= 11ULL * 60ULL * 1000ULL;
}

static int reach_class(const char *call) {
  if (!call || !call[0] || call[0] == '#') return REACH_NONE;
  uint64_t now = hal_time_epoch();

  /* The nearby table is only pumped while the screen is open (see the tick),
   * so a send from a retry path would otherwise read a cold table. */
  if (!g_near_scan_at || now - g_near_scan_at >= NEAR_SCAN_SEC) {
    near_scan();
    g_near_scan_at = now;
  }

  for (int i = 0; i < g_near_n; i++) {
    if (!s_eq_ci(g_near_call[i], call)) continue;
    unsigned tr = g_near_tr[i];
    uint64_t lts = g_near_lts[i];
    if ((tr & TR_LAN) && lts && now - lts <= NEAR_LOCAL_STICKY_SEC) {
      return REACH_NET;                  /* same network: no need for the air */
    }
    if ((tr & (TR_BLE | TR_RADIO)) && lts && now - lts <= NEAR_ACTIVE_SEC) {
      return REACH_LOCAL;                /* in the room: air it, it arrives */
    }
    break;
  }

  /* One cached probe — this one costs a HAL round trip. */
  if (s_eq_ci(g_reach_call, call) && g_reach_at && now - g_reach_at < 30) {
    return g_reach_val;
  }
  int v = reach_on_net(call) ? REACH_NET : REACH_NONE;
  s_cpy(g_reach_call, call, sizeof(g_reach_call));
  g_reach_val = v;
  g_reach_at = now;
  return v;
}

static void near_tags(unsigned tr, char *out, unsigned sz) {
  out[0] = 0;
  int first = 1;
  const char *names[5] = { "LAN", "BLE", "Reticulum", "LXMF", "Radio" };
  unsigned bits[5] = { TR_LAN, TR_BLE, TR_RNS, TR_LXMF, TR_RADIO };
  for (int i = 0; i < 5; i++) {
    if (!(tr & bits[i])) continue;
    if (!first) s_cat(out, "\",\"", sz);
    s_cat(out, names[i], sz);
    first = 0;
  }
}

/* Order by recency: the most recently heard is the one you can still catch. */
static void near_order(int *idx, int n) {
  for (int i = 0; i < n; i++) idx[i] = i;
  for (int i = 0; i < n; i++)
    for (int j = i + 1; j < n; j++)
      if (g_near_ts[idx[j]] > g_near_ts[idx[i]]) { int t = idx[i]; idx[i] = idx[j]; idx[j] = t; }
}

static void near_row(char *o, unsigned sz, int i, int stale) {
  char age[12]; rel_time(g_near_ts[i], age, sizeof(age));
  const char *disp = g_near_call[i][0] ? g_near_call[i]
                   : (g_near_name[i][0] ? g_near_name[i] : g_near_key[i]);
  s_cat(o, "{\"id\":\"", sz); jesc(o, sz, g_near_call[i][0] ? g_near_call[i] : g_near_key[i]);
  s_cat(o, "\",\"title\":\"", sz); jesc(o, sz, disp);
  if (g_near_call[i][0] && g_near_name[i][0] && !s_eq(g_near_call[i], g_near_name[i])) {
    s_cat(o, " - ", sz); jesc(o, sz, g_near_name[i]);
  }
  s_cat(o, "\",\"subtitle\":\"", sz);
  if (g_near_ts[i] == 0) s_cat(o, "never heard", sz);
  else if (stale) { s_cat(o, "last seen ", sz); s_cat(o, age, sz); s_cat(o, " ago", sz); }
  else { s_cat(o, "seen ", sz); s_cat(o, age, sz); s_cat(o, " ago", sz); }
  if (!stale && g_near_hops[i] == 1) s_cat(o, " - direct", sz);
  s_cat(o, "\",\"tags\":[\"", sz);
  { char tags[80]; near_tags(g_near_tr[i], tags, sizeof(tags));
    /* Never an empty chip: a row with no transport bits is still a sighting,
     * and "" reads as a rendering bug. */
    s_cat(o, tags[0] ? tags : "Local", sz); }
  s_cat(o, "\"]", sz);
  if (stale) s_cat(o, ",\"dim\":true", sz);
  s_cat(o, "}", sz);
}

static void render_nearby(void) {
  uint64_t now = hal_time_epoch();
  int idx[NEAR_MAX];
  near_order(idx, g_near_n);

  int active = 0;
  for (int i = 0; i < g_near_n; i++)
    if (g_near_ts[i] && now - g_near_ts[i] <= NEAR_ACTIVE_SEC &&
        g_near_lts[i] && now - g_near_lts[i] <= NEAR_LOCAL_STICKY_SEC) {
      active++;
    }

  char *o = g_near_out; const unsigned sz = sizeof(g_near_out);
  o[0] = 0;
  /* ONE list, not two tabs. "Within reach" and "seen before" are the same
   * question asked of the same people — splitting them made you click to find
   * out whether somebody is here. Reachable first, most recent at the top,
   * everyone else greyed underneath. */
  s_cat(o, "{\"type\":\"ui.people.set\",\"field\":\"nearby\",\"sections\":["
           "{\"title\":\"Devices\",\"items\":[", sz);
  int first = 1;
  for (int pass = 0; pass < 2; pass++) {
    for (int k = 0; k < g_near_n; k++) {
      int i = idx[k];
      const int fresh = g_near_ts[i] && now - g_near_ts[i] <= NEAR_ACTIVE_SEC &&
                        g_near_lts[i] &&
                        now - g_near_lts[i] <= NEAR_LOCAL_STICKY_SEC;
      if ((pass == 0) != (fresh != 0)) continue;
      if (g_near_q[0]) {
        /* Search across every name we hold for them, and the key — you may
         * only remember one of the three. */
        if (!ci_has(g_near_call[i], g_near_q) &&
            !ci_has(g_near_name[i], g_near_q) &&
            !ci_has(g_near_key[i], g_near_q)) continue;
      }
      if (!first) s_cat(o, ",", sz);
      first = 0;
      near_row(o, sz, i, !fresh);
    }
  }
  s_cat(o, "]}]}", sz);
  fnd_changed_send(o, &g_near_hash);

  /* The count on the app-bar icon: how many are within reach, without having
   * to open the panel to find out. Diffed — a scalar set every few seconds
   * would rebuild the host's field map for nothing. */
  {
    static int last_active = -1;
    if (active != last_active) {
      last_active = active;
      char m[120] = "{\"type\":\"ui.field.set\",\"field\":\"nearby_count\",\"value\":\"";
      char nb[12]; u_itoa((unsigned)active, nb);
      s_cat(m, nb, sizeof(m));
      s_cat(m, "\"}", sizeof(m));
      hal_msg_send(m, s_len(m));
    }
  }

  /* The two numbers, said once: within reach now, and how many the list
   * remembers from the past week. */
  { char m[300] = "{\"type\":\"ui.stats.set\",\"field\":\"nearby_stats\",\"tiles\":[";
    char nb[12];
    u_itoa((unsigned)active, nb);
    s_cat(m, "{\"id\":\"active\",\"label\":\"Within reach\",\"value\":\"", sizeof(m));
    s_cat(m, nb, sizeof(m));
    s_cat(m, "\"},", sizeof(m));
    /* EVERYONE the list holds, not just the stale half: the table only keeps a
     * week (near_expire), so this is literally "seen this week" — and a device
     * heard a minute ago was obviously seen this week too. Counting only the
     * unreachable ones made the tile read 0 while a device sat in the list. */
    u_itoa((unsigned)g_near_n, nb);
    s_cat(m, "{\"id\":\"week\",\"label\":\"Seen this week\",\"value\":\"", sizeof(m));
    s_cat(m, nb, sizeof(m));
    s_cat(m, "\"}]}", sizeof(m));
    fnd_changed_send(m, &g_near_stats_hash);
  }
}

/* Forget everyone who is not within reach right now. The reachable rows are
 * left alone — they are not history, they are the room you are standing in. */
static void near_clear_old(void) {
  uint64_t now = hal_time_epoch();
  int w = 0;
  for (int i = 0; i < g_near_n; i++) {
    if (!(g_near_ts[i] && now - g_near_ts[i] <= NEAR_ACTIVE_SEC &&
          g_near_lts[i] && now - g_near_lts[i] <= NEAR_LOCAL_STICKY_SEC)) {
      continue;
    }
    if (w != i) {
      s_cpy(g_near_call[w], g_near_call[i], sizeof(g_near_call[0]));
      s_cpy(g_near_key[w], g_near_key[i], sizeof(g_near_key[0]));
      s_cpy(g_near_name[w], g_near_name[i], sizeof(g_near_name[0]));
      s_cpy(g_near_id[w], g_near_id[i], sizeof(g_near_id[0]));
      g_near_ts[w] = g_near_ts[i];
      g_near_lts[w] = g_near_lts[i];
      g_near_tr[w] = g_near_tr[i];
      g_near_hops[w] = g_near_hops[i];
    }
    w++;
  }
  g_near_n = w;
  near_save();
  render_nearby();
}

static void do_nearby_open(void) {
  g_near_open = 1;
  near_scan();
  g_near_scan_at = hal_time_epoch();
  render_nearby();
  const char *m = "{\"type\":\"ui.screen.open\",\"name\":\"Nearby devices\"}";
  hal_msg_send(m, s_len(m));
}

/* A row tap opens the host's full profile screen for that person — where the
 * choice between "chat here" and "send them mail" belongs. */
static void do_nearby_tap(const char *buf) {
  char id[96] = "";
  jstr(buf, "nearby_id", id, sizeof(id));
  if (!id[0]) return;
  int idx = -1;
  for (int i = 0; i < g_near_n; i++) {
    if ((g_near_call[i][0] && s_eq(g_near_call[i], id)) ||
        (g_near_key[i][0] && s_eq(g_near_key[i], id)) ||
        (g_near_id[i][0] && s_eq(g_near_id[i], id))) { idx = i; break; }
  }
  if (idx < 0) return;
  const char *disp = g_near_call[idx][0] ? g_near_call[idx]
                   : (g_near_name[idx][0] ? g_near_name[idx] : g_near_key[idx]);

  /* A row is one of three things, and each wants a different door.
   *
   * 1. Somebody with a key — open their profile, where Chat and Mail live.
   * 2. A NomadNet/Sideband peer, addressable only by its LXMF destination —
   *    open that conversation. A profile page for them would be blank.
   * 3. A plain Reticulum device that announced no way to message it — say so.
   *    Doing nothing on tap reads as a broken row. */
  const char *pk = g_near_call[idx][0] ? pk_get(g_near_call[idx]) : 0;
  char npub[72] = "";
  if (s_pre(g_near_key[idx], "npub1")) {
    s_cpy(npub, g_near_key[idx], sizeof(npub));
  } else if (pk) {
    uint32_t nn = hal_npub(pk, s_len(pk), npub, sizeof(npub) - 1);
    if (nn > 0 && nn < sizeof(npub)) npub[nn] = 0; else npub[0] = 0;
  }

  if (npub[0] || g_near_call[idx][0]) {
    char m[300] = "{\"type\":\"ui.profile.open\",\"callsign\":\"";
    jesc(m, sizeof(m), disp);
    s_cat(m, "\",\"npub\":\"", sizeof(m));
    if (npub[0]) jesc(m, sizeof(m), npub);
    s_cat(m, "\"}", sizeof(m));
    hal_msg_send(m, s_len(m));
    return;
  }

  if (is_hex32(g_near_key[idx])) {
    /* Same path the New-chat picker uses for an LXMF address. */
    if (g_near_name[idx][0]) lxname_set(g_near_key[idx], g_near_name[idx]);
    char cid[72] = "lxmf:";
    s_cat(cid, g_near_key[idx], sizeof(cid));
    convo_ensure(cid);
    groups_save();
    render_rail();
    char m[220] = "{\"type\":\"ui.convo.upsert\",\"id\":\"";
    jesc(m, sizeof(m), cid);
    s_cat(m, "\",\"select\":true,\"bump\":true}", sizeof(m));
    hal_msg_send(m, s_len(m));
    hal_msg_send("{\"type\":\"ui.screen.close\"}", 27);
    return;
  }

  notify("info", "This device announced no messaging address yet");
}

static void profile_show(const char *call) {
  char up_call[16]; int j = 0;
  for (int i = 0; call[i] && j < 15; i++) up_call[j++] = up(call[i]);
  up_call[j] = 0;
  if (!up_call[0] || s_eq(up_call, g_call)) return;
  int fol = is_following(up_call);
  int fan = is_follower(up_call);
  char body[420] = "";
  if (fol && fan) s_cat(body, "You follow each other.", sizeof(body));
  else if (fol)   s_cat(body, "You are following.", sizeof(body));
  else if (fan)   s_cat(body, "Follows you.", sizeof(body));
  else            s_cat(body, "Not following.", sizeof(body));
  for (int i = 0; i < g_follow_n; i++)
    if (s_eq(g_follow[i], up_call) && g_ftag[i][0]) {
      s_cat(body, "\nTags: ", sizeof(body));
      s_cat(body, g_ftag[i], sizeof(body));
      break;
    }
  const char *pk = pk_get(up_call);
  if (pk) {
    char npub[72];
    uint32_t nn = hal_npub(pk, s_len(pk), npub, sizeof(npub) - 1);
    if (nn > 0 && nn < sizeof(npub)) {
      npub[nn] = 0;
      s_cat(body, "\nKey: ", sizeof(body)); s_cat(body, npub, sizeof(body));
    }
    for (int i = 0; i < g_pk_n; i++) if (s_eq(g_pk_call[i], up_call)) {
      char age[12]; rel_time(g_pk_ts[i], age, sizeof(age));
      s_cat(body, "\nKey heard ", sizeof(body)); s_cat(body, age, sizeof(body));
      s_cat(body, " ago", sizeof(body));
      break;
    }
  } else {
    s_cat(body, "\nNo public key received yet.", sizeof(body));
  }
  { double la, lo; char d[24];
    if (pos_get(up_call, &la, &lo) && distance_to(la, lo, d, sizeof(d))) {
      s_cat(body, "\nDistance: ", sizeof(body)); s_cat(body, d, sizeof(body));
    } }
  char m[1000] = "{\"type\":\"ui.prompt\",\"id\":\"prof:";
  jesc(m, sizeof(m), up_call);
  s_cat(m, "\",\"title\":\"", sizeof(m)); jesc(m, sizeof(m), up_call);
  if (is_blocked(up_call)) s_cat(body, "\nBlocked — their messages are hidden.", sizeof(body));
  s_cat(m, "\",\"body\":\"", sizeof(m)); jesc(m, sizeof(m), body);
  s_cat(m, "\",\"chips\":[", sizeof(m));
  if (fol)
    s_cat(m, "{\"label\":\"Unfollow\",\"value\":\"unfollow\"},"
             "{\"label\":\"Edit tags\",\"value\":\"tags\"},", sizeof(m));
  else
    s_cat(m, "{\"label\":\"Follow\",\"value\":\"follow\"},", sizeof(m));
  if (is_blocked(up_call))
    s_cat(m, "{\"label\":\"Unblock\",\"value\":\"unblock\"}", sizeof(m));
  else
    s_cat(m, "{\"label\":\"Block\",\"value\":\"block\"}", sizeof(m));
  s_cat(m, "]}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* Edit the tags on a followed callsign (result handled as "ftag:<call>"). */
static void prompt_ftag(const char *call) {
  char m[420] = "{\"type\":\"ui.prompt\",\"id\":\"ftag:";
  jesc(m, sizeof(m), call);
  s_cat(m, "\",\"title\":\"Tags for ", sizeof(m)); jesc(m, sizeof(m), call);
  s_cat(m, "\",\"body\":\"Space-separated tags, e.g. dx friend club. "
           "Leave empty to clear.\","
           "\"input\":{\"hint\":\"tags\",\"max\":40},\"confirm\":\"Save\"}",
        sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* Extract the idx-th comma-separated field of s into out (NUL-terminated). */
static void csv_field(const char *s, int idx, char *out, unsigned osz) {
  out[0] = 0;
  int f = 0;
  const char *start = s;
  for (const char *p = s;; p++) {
    if (*p == ',' || *p == 0) {
      if (f == idx) {
        unsigned n = (unsigned)(p - start);
        if (n >= osz) n = osz - 1;
        for (unsigned i = 0; i < n; i++) out[i] = start[i];
        out[n] = 0;
        return;
      }
      if (*p == 0) return;
      f++; start = p + 1;
    }
  }
}

/* RSSI -> rough distance (metres) via log-distance path loss:
 *   d = 10^((TXREF - rssi)/(10*N)),  TXREF ~ RSSI at 1 m, N ~ path-loss exp.
 * Coarse, but close enough for a direct hop. -1 when rssi is unknown. */
static int est_dist_m(int rssi) {
  if (rssi >= 0) return -1;
  double d = __builtin_pow(10.0, (double)(-59 - rssi) / 25.0);
  if (d < 1.0) d = 1.0;
  if (d > 5000.0) d = 5000.0;
  return (int)(d + 0.5);
}

/* Straight-line distance from our position to lat/lon in metres, or -1 when
 * our own position is unknown. (Metres twin of distance_to.) */
static int dist_m_to(double lat, double lon) {
  if (g_lat == 0 && g_lon == 0) return -1;
  const double D2R = 0.0174532925199433;
  double x = (lon - g_lon) * D2R * m_cos((g_lat + lat) * 0.5 * D2R);
  double y = (lat - g_lat) * D2R;
  double km = 6371.0 * __builtin_sqrt(x * x + y * y);
  return (int)(km * 1000.0 + 0.5);
}

/* Format metres as "<n> m" (<1 km) or "<n> km". */
static void fmt_dist_m(int m, char *out, unsigned osz) {
  if (m < 1000) { u_itoa((unsigned)m, out); s_cat(out, " m", osz); }
  else { u_itoa((unsigned)((m + 500) / 1000), out); s_cat(out, " km", osz); }
}

/* Per-ping responder results, so we can keep the best route per responder and
 * re-render the list as replies arrive. by_pos = distance came from a real
 * position (accurate); else it's an RF (RSSI) estimate. */
#define PRES_MAX 32
static struct { char call[16]; int hops; int dist_m; int by_pos; int used; } g_pres[PRES_MAX];
static int g_pres_n = 0;
static void pres_reset(void) { g_pres_n = 0; for (int i = 0; i < PRES_MAX; i++) g_pres[i].used = 0; }
/* Best route: prefer a position fix; among RF estimates keep the smallest
 * (most-direct) one; track the fewest hops seen. */
static void pres_update(const char *call, int hops, int dist_m, int by_pos) {
  int idx = -1;
  for (int i = 0; i < g_pres_n; i++)
    if (g_pres[i].used && s_eq(g_pres[i].call, call)) { idx = i; break; }
  if (idx < 0) {
    if (g_pres_n >= PRES_MAX) return;
    idx = g_pres_n++;
    s_cpy(g_pres[idx].call, call, sizeof(g_pres[idx].call));
    g_pres[idx].used = 1; g_pres[idx].hops = hops;
    g_pres[idx].dist_m = dist_m; g_pres[idx].by_pos = by_pos;
    return;
  }
  if (by_pos && !g_pres[idx].by_pos) { g_pres[idx].by_pos = 1; g_pres[idx].dist_m = dist_m; }
  else if (by_pos == g_pres[idx].by_pos && dist_m >= 0 &&
           (g_pres[idx].dist_m < 0 || dist_m < g_pres[idx].dist_m)) {
    g_pres[idx].dist_m = dist_m;
  }
  if (hops >= 0 && hops < g_pres[idx].hops) g_pres[idx].hops = hops;
}
static void pres_render(void) {
  const char *c = "{\"type\":\"ui.log.clear\",\"field\":\"pingresults\"}";
  hal_msg_send(c, s_len(c));
  for (int i = 0; i < g_pres_n; i++) {
    if (!g_pres[i].used) continue;
    char line[128] = ""; s_cat(line, g_pres[i].call, sizeof(line)); s_cat(line, "  ", sizeof(line));
    { char t[8]; u_itoa((unsigned)(g_pres[i].hops < 0 ? 0 : g_pres[i].hops), t); s_cat(line, t, sizeof(line)); }
    s_cat(line, (g_pres[i].hops == 1) ? " hop" : " hops", sizeof(line));
    if (g_pres[i].dist_m >= 0) {
      char d[24]; fmt_dist_m(g_pres[i].dist_m, d, sizeof(d));
      s_cat(line, "  -  ", sizeof(line));
      if (!g_pres[i].by_pos) s_cat(line, "~", sizeof(line));   /* RF estimate */
      s_cat(line, d, sizeof(line));
      if (!g_pres[i].by_pos) s_cat(line, " (RF)", sizeof(line));
    }
    log_line("pingresults", line);
  }
}

/* Inbound ping: answer once with our callsign + position, then forward it on
 * (ttl) so it reaches further stations. text = "id,ttl,hops". */
static void handle_ping(const char *from, const char *text) {
  char ids[16], ttls[8], hopss[8];
  csv_field(text, 0, ids, sizeof(ids));
  csv_field(text, 1, ttls, sizeof(ttls));
  csv_field(text, 2, hopss, sizeof(hopss));
  if (!ids[0]) return;
  unsigned key = sig_hash("P", "", ids);
  if (pseen_has(key)) return;
  pseen_add(key);
  int ttl = to_int(ttls), hops = to_int(hopss);
  if (hops < 0) hops = 0;

  /* reply "id,hops,lat,lon,pttl" (lat/lon empty when unknown) */
  double la, lo; int have = my_position(&la, &lo);
  char body[96] = ""; s_cat(body, ids, sizeof(body)); s_cat(body, ",", sizeof(body));
  { char t[8]; u_itoa((unsigned)hops, t); s_cat(body, t, sizeof(body)); }
  s_cat(body, ",", sizeof(body));
  if (have) append_dbl(body, sizeof(body), la);
  s_cat(body, ",", sizeof(body));
  if (have) append_dbl(body, sizeof(body), lo);
  s_cat(body, ",", sizeof(body));
  { char t[8]; u_itoa((unsigned)PING_DEFAULT_TTL, t); s_cat(body, t, sizeof(body)); }
  s_cat(body, ",0", sizeof(body));   /* dM: cumulative RF distance starts at 0 */
  ble_tx_from(g_call, PONG_TO, body);

  if (ttl > 1) {     /* digipeat the ping further */
    char fwd[40] = ""; s_cat(fwd, ids, sizeof(fwd)); s_cat(fwd, ",", sizeof(fwd));
    { char t[8]; u_itoa((unsigned)(ttl - 1), t); s_cat(fwd, t, sizeof(fwd)); }
    s_cat(fwd, ",", sizeof(fwd));
    { char t[8]; u_itoa((unsigned)(hops + 1), t); s_cat(fwd, t, sizeof(fwd)); }
    ble_tx_from(from, PING_TO, fwd);   /* keep the original pinger as 'from' */
  }
}

/* Inbound pong: if it answers our active ping, record it (best route) + drop a
 * map marker; forward it back across the mesh, accumulating an RF distance.
 * text = "id,hops,lat,lon,pttl,dM". [rssi] = strength we received it at.
 *
 * Distance estimate per responder:
 *  - if the reply carries a position AND we know ours -> exact (by_pos);
 *  - else RF: dM (sum of prior hops' RSSI distances) + this hop's RSSI distance.
 * For multi-hop, several routes may arrive; we keep the smallest (best). */
static void handle_pong(const char *from, const char *text, int rssi) {
  char ids[16], hopss[8], las[24], los[24], pttls[8], dms[12];
  csv_field(text, 0, ids, sizeof(ids));
  csv_field(text, 1, hopss, sizeof(hopss));
  csv_field(text, 2, las, sizeof(las));
  csv_field(text, 3, los, sizeof(los));
  csv_field(text, 4, pttls, sizeof(pttls));
  csv_field(text, 5, dms, sizeof(dms));
  if (!ids[0]) return;

  int hops = to_int(hopss);
  double lat = to_dbl(las), lon = to_dbl(los);
  int has_pos = (las[0] != 0);
  int dM = to_int(dms);                  /* RF metres accumulated so far */
  int hop_m = est_dist_m(rssi);          /* this hop's RF distance (-1 unknown) */

  /* Record for our active ping — for EVERY arriving copy, so best-route wins. */
  char gids[16]; u_itoa(g_ping_id, gids);
  if (g_ping_active && s_eq(gids, ids)) {
    int by_pos = 0, dist = -1;
    if (has_pos) {
      int dm = dist_m_to(lat, lon);      /* needs our own position */
      if (dm >= 0) { dist = dm; by_pos = 1; }
    }
    if (!by_pos && hop_m >= 0) dist = dM + hop_m;   /* RF total along this route */
    pres_update(from, hops, dist, by_pos);
    if (has_pos) push_marker(from, lat, lon, "green", "ping reply");
    pres_render();
  }

  /* Forward the reply once (per responder+id), adding this hop's RF distance so
   * the running total reflects the path back to the pinger. Skip if we're the
   * pinger (we're the destination). */
  unsigned key = sig_hash("Q", from, ids);
  if (pseen_has(key)) return;
  pseen_add(key);
  int pttl = to_int(pttls);
  if (pttl > 1 && !(g_ping_active && s_eq(gids, ids))) {
    int dM2 = dM + (hop_m >= 0 ? hop_m : 0);
    char fwd[110] = ""; s_cat(fwd, ids, sizeof(fwd)); s_cat(fwd, ",", sizeof(fwd));
    { char t[8]; u_itoa((unsigned)hops, t); s_cat(fwd, t, sizeof(fwd)); }
    s_cat(fwd, ",", sizeof(fwd)); s_cat(fwd, las, sizeof(fwd));
    s_cat(fwd, ",", sizeof(fwd)); s_cat(fwd, los, sizeof(fwd));
    s_cat(fwd, ",", sizeof(fwd));
    { char t[8]; u_itoa((unsigned)(pttl - 1), t); s_cat(fwd, t, sizeof(fwd)); }
    s_cat(fwd, ",", sizeof(fwd));
    { char t[12]; u_itoa((unsigned)dM2, t); s_cat(fwd, t, sizeof(fwd)); }
    ble_tx_from(from, PONG_TO, fwd);   /* keep the responder as 'from' */
  }
}

/* Tools "Send ping": broadcast a fresh ping and start collecting replies. */
static void do_ping(const char *buf) {
  read_config(buf);
  if (!g_ble_on) { notify("warning", "Enable Bluetooth exchange first (Settings)"); return; }
  int ttl = PING_DEFAULT_TTL;
  { char v[8]; if (jstr(buf, "ping_ttl", v, sizeof(v)) && v[0]) ttl = to_int(v); }
  if (ttl < 1) ttl = 1; if (ttl > 8) ttl = 8;

  g_ping_seq++;
  g_ping_id = (unsigned)hal_time_epoch() ^ (g_ping_seq * 2654435761u);
  g_ping_active = 1;
  g_ping_start = hal_time_epoch();
  pres_reset();

  { const char *c = "{\"type\":\"ui.log.clear\",\"field\":\"pingresults\"}";
    hal_msg_send(c, s_len(c)); }

  char ids[16]; u_itoa(g_ping_id, ids);
  pseen_add(sig_hash("P", "", ids));   /* never answer our own ping */

  char body[40] = ""; s_cat(body, ids, sizeof(body)); s_cat(body, ",", sizeof(body));
  { char t[8]; u_itoa((unsigned)ttl, t); s_cat(body, t, sizeof(body)); }
  s_cat(body, ",0", sizeof(body));
  ble_tx_from(g_call, PING_TO, body);

  log_line("pingresults", "Ping sent - waiting for replies...");
  status("TX ping");
}

/* via = the transport this frame actually arrived on ("BLE" for Bluetooth, "RET"
 * for a Reticulum datagram over the internet). The RNS path reuses the BLE frame
 * FORMAT but must NOT be mislabelled as Bluetooth, so the caller passes the real
 * transport and we tag every delivered copy with it. */
static void ble_handle(const char *compact, int rssi, const char *via) {
  char from[16] = "", to[24] = "", text[256] = "";

  /* Two formats on the air: XPRS, which is what we now send, and the compact
   * frame, which older builds and the ESP32 still send. Both land in the same
   * (from, to, text), so everything below this point is unchanged. */
  if (xprs_looks_like(compact)) {
    uint64_t sent = 0;
    if (!xprs_unpack(compact, from, sizeof(from), to, sizeof(to),
                     text, sizeof(text), &sent)) {
      return;   /* a type chat has nothing to show for — the XPRS wapp does */
    }
    /* A message's time is the SENDER's. The compact frame never carried one,
     * so a message that waited in a mailbox was stamped with its arrival;
     * XPRS says when it was written. Ignore a clock that is obviously wrong
     * rather than filing the message in the wrong hour. */
    uint64_t now = hal_time_epoch();
    if (sent && sent <= now + 300 && now - sent < 30ULL * 24 * 3600) {
      g_msg_epoch = sent;
    }
  } else {
    int seg = 0, fi = 0, ti = 0, xi = 0;
    for (const char *q = compact; *q; q++) {
      if (*q == BLE_SEP) { seg++; continue; }
      if (seg == 0) { if (fi < 15) from[fi++] = *q; }
      else if (seg == 1) { if (ti < 23) to[ti++] = *q; }
      else { if (xi < 255) text[xi++] = *q; }
    }
    from[fi] = 0; to[ti] = 0; text[xi] = 0;
  }
  if (!from[0]) return;
  if (is_self_call(from)) return;

  /* Remember every BLE-local station we hear (store-and-forward registry +
   * the "reachable over BLE" list shown in New message). Only for frames that
   * truly arrived over the radio — a Reticulum-over-internet copy (via "RET")
   * is NOT BLE-reachable and must not pollute the registry. */
  if (s_eq(via, "BLE") && valid_call(from)) sdev_touch(from);

  /* Lightweight presence beacon: its only job is the registry touch above, so a
   * BLE-only/GPS-less station is still discoverable. Carries no content — drop
   * it before the dedup/feed path. */
  if (s_eq(to, HELLO_TO)) return;

  /* Control frames are handled on EVERY receipt — BEFORE the content-dedup below
   * — because they carry no unique body: a repeated ?IGATE beacon must keep
   * refreshing g_last_igate_heard (else the client stops pulling mail after the
   * window), and each ?MAIL must be answered. (Our own re-scanned control frames
   * are dropped by the `mine` check above.)
   *  ?IGATE = an online iGate announcing itself -> note it's in reach.
   *  ?MAIL  = a station pulling its held mail (text = "<days> <nonce>"). */
  if (s_eq(to, IGATE_TO)) { g_last_igate_heard = hal_time_epoch(); return; }
  if (s_eq(to, MAIL_TO))  { handle_mail_query(from, text); return; }

  /* Ping reach-test frames (Tools tab): handled here and NEVER digipeated
   * verbatim, relayed to APRS-IS, or shown on the Live feed — they have their
   * own ttl-based forwarding. */
  if (s_eq(to, PING_TO)) { handle_ping(from, text); return; }
  if (s_eq(to, PONG_TO)) { handle_pong(from, text, rssi); return; }

  /* Content frames: dedup so the digipeater and the chat handle each only once. */
  unsigned h = sig_hash("b", "", compact);
  if (fseen_has(h)) return;
  fseen_add(h);

  /* Digipeater: rebroadcast this frame, after a short staggered delay (see
   * rq_*), ignoring content already repeated in the last 10 minutes. A 1:1
   * message gets TWO extra staggered re-airs — the addressee may sit at the
   * fringe of a bridging neighbor (multi-floor/street relay), where a single
   * 120 s advert window routinely loses the scan-batching lottery; repeats
   * multiply the catch probability and receivers dedup by content anyway. */
  {
    uint64_t now = hal_time_epoch();
    if (!rpt_recent(h, now)) {
      rpt_mark(h, now);
      rq_push(compact, now + 1 + (h % 3));
      int one2one = to[0] && to[0] != '#' && to[0] != '!' && to[0] != '?' &&
                    text[0] != '?';
      if (one2one) {
        rq_push(compact, now + 75 + (h % 5));
        rq_push(compact, now + 150 + (h % 7));
      }
    }
  }

  if (s_eq(to, "!")) {                    /* position: "lat,lon[,comment]" */
    char a[24] = "", b[24] = "", comment[80] = "";
    int s2 = 0, ai = 0, bi = 0, ci = 0;
    for (const char *q = text; *q; q++) {
      if (*q == ',' && s2 < 2) { s2++; continue; }
      if (s2 == 0) { if (ai < 23) a[ai++] = *q; }
      else if (s2 == 1) { if (bi < 23) b[bi++] = *q; }
      else { if (ci < 79) comment[ci++] = *q; }
    }
    a[ai] = 0; b[bi] = 0; comment[ci] = 0;
    double lat = to_dbl(a), lon = to_dbl(b);
    push_marker(from, lat, lon, 0, comment);
    pos_set(from, lat, lon);
    if (convo_known(from)) convo_badge_only(from);
    if (comment[0]) {
      char meta[24] = ""; distance_to(lat, lon, meta, sizeof(meta));
      if (!geo_dup(from, comment))
        chat_append("geochat", "", "in", from, comment, "pos", 0, meta, lat, lon, via);
      if (is_following(from)) {
        const char *c = comment;
        if (c[0] == '>' && c[1] == '>') { c += 2; while (*c == ' ') c++; if (c[0]) activity_capture(from, "", c, via); }
        else { char t[300]; s_cpy(t, "status: ", sizeof(t)); s_cat(t, c, sizeof(t)); activity_capture(from, "", t, via); }
      }
    }
  } else if (to[0] == '#') {              /* group bulletin (in range/local for BLE) */
    deliver_bulletin(to + 1, from, text, 1, via);
    /* iGate BLE → APRS-IS: re-originate the bulletin under the sender's
     * callsign with a qAR q-construct (we are the gateway). A clean RF-gated
     * path is essential — a TCPIP* path makes APRS-IS treat it as a loop and
     * drop it, which is why the old third-party form never appeared.
     * NEVER gate an auto-generated X1/X3 callsign onto APRS-IS: those are not
     * authority-assigned, so their traffic stays on Bluetooth/Reticulum. Only
     * frames heard over REAL Bluetooth are gated — Reticulum ("RET") copies
     * come from the whole network (originators presumed unlicensed) and would
     * flood APRS-IS under our callsign. */
    if (g_ble_relay && g_logged && s_eq(via, "BLE") && !is_autogen_call(from)) {
      char via[24]; s_cpy(via, "qAR,", sizeof(via)); s_cat(via, g_call, sizeof(via));
      char line[260]; aprs_build_bulletin_via(line, sizeof(line), from, to + 1, '0', text, via);
      aprs_send_raw(g_sock, line);
    }
  } else if (!to[0]) {                    /* area / geo-chat broadcast text */
    char meta[24] = ""; double slat = 0, slon = 0;
    if (pos_get(from, &slat, &slon)) distance_to(slat, slon, meta, sizeof(meta));
    if (!geo_dup(from, text))
      chat_append("geochat", "", "in", from, text, "msg", 0, meta, slat, slon, via);
    if (is_following(from)) {
      const char *c = text;
      if (c[0] == '>' && c[1] == '>') { c += 2; while (*c == ' ') c++; }
      if (c[0]) activity_capture(from, "", c, via);
    }
    /* Geochat/Live-tab broadcast: shown on the Live tab, no notification. */
  } else {                               /* 1:1 to a callsign */
    int amine = 1;
    for (int i = 0; g_call[i] || to[i]; i++) {
      if (up(g_call[i]) != up(to[i])) { amine = 0; break; }
    }
    /* Follow notifications are control traffic, not chat (see route_frame). */
    if (amine && follow_intercept(from, text)) return;
    if (amine && priv_intercept(from, text)) return;
    if (amine && rly_intercept(from, text)) return;
    /* Consume receipts for any overhearer (broadcast BLE/RNS), not just the
     * addressee, so a "?ACK …" frame never surfaces as a Live/chat message. */
    if (rcpt_intercept(from, text)) return;
    /* Same for native APRS ack/rej lines bridged onto BLE (e.g. by an older
     * relay): control traffic, never chat and never re-originated. */
    if (is_ack_text(text)) return;
    if (amine) {
      /* Buffer through the same reassembler as APRS-IS: a multi-line message
       * forwarded by a BLE iGate as separate parts is rejoined, and a message
       * also received directly over APRS-IS dedups (shown once). */
      trc("da_add", from, text);
      da_add(from, text, via);
    } else {
      char meta[24] = ""; double slat = 0, slon = 0;
      if (pos_get(from, &slat, &slon)) distance_to(slat, slon, meta, sizeof(meta));
      if (!geo_dup(from, text))
        chat_append("geochat", "", "in", from, text, "msg", 0, meta, slat, slon, via);
    }
    /* Notification fires once after reassembly in convo_deliver (not here per
     * BLE frame), so a multi-line/encrypted DM alerts once with readable text. */
    /* iGate BLE -> APRS-IS, but never for a message addressed to us (we are the
     * endpoint; re-injecting it would loop back as a duplicate). Gated under the
     * sender's call with a qAR q-construct (clean RF path, not TCPIP*).
     * NEVER gate an ack/rej: an ack is point-to-point between the original
     * endpoints; re-originating one puts a spoofed ack on APRS-IS under someone
     * else's callsign. seq -1 = no {n — the copy must not solicit acks (a
     * fabricated {0 turned bridged acks into "messages" that bots answered,
     * looping forever under a third party's callsign). And NEVER gate an
     * auto-generated X1/X3 callsign onto APRS-IS — not authority-assigned.
     * Only genuine-Bluetooth ("BLE") arrivals are gated: a Reticulum ("RET")
     * copy is network-wide traffic from presumed-unlicensed originators. */
    if (g_ble_relay && g_logged && !amine && !is_ack_text(text) &&
        s_eq(via, "BLE") && !is_autogen_call(from)) {
      char via[24]; s_cpy(via, "qAR,", sizeof(via)); s_cat(via, g_call, sizeof(via));
      char line[260]; aprs_build_message_via(line, sizeof(line), from, to, text, -1, via);
      aprs_send_raw(g_sock, line);
    }
  }
}

/* Reconcile the BLE transport with the g_ble_on setting (start/stop scan). */
static void ble_reconcile(void) {
  if (g_ble_on && !g_ble_started) {
    ble_start();
    g_ble_started = 1;
    status("Bluetooth on");
    /* No toast: the BLE channel availability is shown by the BLE chip in the
     * AppBar (see push_status -> ui.map.status). */
  } else if (!g_ble_on && g_ble_started) {
    ble_stop();
    g_ble_started = 0;
    status("Bluetooth off");
  }
}

/* ── module entry points ────────────────────────────────────────────── */
void module_init(void) {
  hal_log(1, "[aprs] init", 11);
  /* Default callsign = THIS device's profile callsign (so each device
   * transmits as itself, not a hardcoded one). The user's Settings callsign,
   * if set, overrides this via read_config. */
  char id[16];
  uint32_t n = hal_identity(id, sizeof(id) - 1);
  if (n > 0 && n < sizeof(id)) {
    id[n] = 0;
    if (id[0]) { s_cpy(g_call, id, sizeof(g_call)); s_cpy(g_idcall, id, sizeof(g_idcall)); }
  }
  sdev_load();     /* restore the seen-devices registry (store-and-forward) */
  gseen_load();    /* before groups_load: the LXMF cursor restarts at 0 per engine */
  chan_load();     /* before groups_load: the rail render honours the switches */
  lxname_load();   /* before groups_load: lxmf rows render with their names */
  recent_load();   /* …and in the order you last used them, not insertion order */
  chanppl_load();  /* distinct senders seen per channel, for the people count */
  near_load();     /* who was within reach before this launch (greyed, kept) */
  groups_load();   /* restore subscribed groups so the g/ filter is correct now */
  /* Cache our public key (base64url) and the persisted pubkey-beacon pref. */
  { uint32_t pn = hal_identity_pubkey(g_pubkey, sizeof(g_pubkey) - 1);
    if (pn < sizeof(g_pubkey)) g_pubkey[pn] = 0; else g_pubkey[0] = 0; }
  pkbeacon_load();
  igate_load();    /* restore iGate (BLE ↔ APRS-IS bridge) on/off (default on) */
  blockhide_load(); /* restore local block list + hidden-message keys */
  emit_activity_filter(); /* re-apply Activity hide set (blocked + muted) */
  pk_load();       /* restore known callsign -> pubkey map (for verification) */
  rns_dest_load(); /* restore npub -> {RNS delivery dests} (Reticulum addressing) */
  cpriv_load();    /* restore which 1:1 conversations are private (Reticulum-only) */
  /* Sweep out callsign-keyed ghost rows written by older builds: every id we
   * hold as a bare callsign is, by definition, not something this wapp can
   * render. Cheap, idempotent, and it heals a store the user is looking at. */
  for (int i = 0; i < g_cpriv_n; i++) convo_drop_ghost(g_cpriv[i]);
  for (int i = 0; i < g_pk_n; i++) convo_drop_ghost(g_pk_call[i]);
  pollrelay_load(); /* restore NOSTR relays peers told us to poll for DM backups */
  midseen_load();   /* restore the persistent relay-message dedup ring */
  pk_render();     /* populate the Keys list view from the restored database */
  /* Bridge restored callsign->pubkey to the host so the Activity feed/profile
   * show npubs immediately, not only after the next live beacon. */
  for (int i = 0; i < g_pk_n; i++) host_identity_emit(g_pk_call[i], g_pk_key[i]);
  follows_load();  /* restore followed callsigns so the b/ filter is correct now */
  followers_load();
  follow_render(); /* push the Follows people list (Following | Followers) */
  /* Bridge restored follow/block state to the host so the profile UI is correct
   * from the first open. */
  for (int i = 0; i < g_follow_n; i++) host_state_emit("follow", g_follow[i], 1);
  for (int i = 0; i < g_blocked_n; i++) host_state_emit("block", g_blocked[i], 1);
  { char b[4]; uint32_t n = hal_kv_get("signmsgs", 8, b, sizeof(b) - 1);
    if (n >= 1) g_sign_msgs = (b[0] != '0'); }
  /* APRS-IS is strictly opt-in (licensed callsign + verified passcode, set in
   * the APRS panel). Only then auto-connect; otherwise stay off-grid — the
   * X1/X3 identity must never appear on APRS-IS. */
  aprsis_load();
  if (g_aprsis_on) {
    s_cpy(g_call, g_aprsis_call, sizeof(g_call));   /* licensed call everywhere */
    status("APRS ready - connecting to APRS-IS automatically...");
    /* Ask the host to run our "connect" command with the current settings
     * (auto-connect on load; no manual Connect needed). */
    const char *m = "{\"type\":\"host.run_command\",\"command\":\"connect\"}";
    hal_msg_send(m, s_len(m));
  } else {
    status("APRS-IS off (enable it in the APRS panel) - Bluetooth/Reticulum active");
  }
}

/* Legacy APRS-IS housekeeping: auto-reconnect, login, drop detection, inbound
 * drain and the timed APRS auto-beacon. Extracted from module_tick so its
 * early returns only skip APRS work — with the APRS-IS switch off (the
 * default), the Reticulum pull / relay polling in module_tick still runs. */
static void aprs_tick(void) {
  /* Auto-reconnect: keep retrying (5s backoff) while we want a link. Hard-
   * gated on the APRS-IS switch: with it off there is never a connection. */
  if (g_sock < 0) {
    if (!g_want_connect || !g_aprsis_on) return;
    uint64_t now = hal_time_epoch();
    if (now - g_last_reconnect < 5) return;
    g_last_reconnect = now;
    g_logged = 0;
    g_sock = aprs_connect(g_host, g_port);
    if (g_sock >= 0) status("Reconnecting to APRS-IS...");
    return;
  }

  if (!g_logged) {
    int st = hal_socket_status(g_sock);
    if (st == 1) {
      /* Login with the user-verified APRS-IS passcode (licensed callsign is
       * already the working g_call while the APRS panel switch is on). */
      int pass = (g_aprsis_pass >= 0) ? g_aprsis_pass : aprs_passcode(g_call);
      /* Include the heard stations in the server-side g/ filter so APRS-IS
       * pushes messages addressed to them (store-and-forward iGate). */
      build_gfilter(g_gfilter, sizeof(g_gfilter));
      aprs_login_ex(g_sock, g_call, pass, g_lat, g_lon, g_radius, g_gfilter);
      g_logged = 1;
      char b[64] = "Connected. passcode "; char nb[16];
      { int v = pass, j = 0; char t[12]; if (v == 0) t[j++]='0'; while (v>0){t[j++]=(char)('0'+v%10);v/=10;} int k=0; while(j>0)nb[k++]=t[--j]; nb[k]=0; }
      s_cat(b, nb, sizeof(b)); status(b);
      /* No toast on (re)connect — the APRS-IS indicator shows the state and a
       * flapping link would otherwise flicker notifications. */
      center_map();
      push_radius();
    } else if (st == 2) {
      /* connect failed — drop and let the reconnect path retry */
      aprs_disconnect(g_sock); g_sock = -1;
      status("Connection failed - retrying...");
    }
    return;
  }

  /* logged in: detect a dropped connection and reconnect */
  if (hal_socket_status(g_sock) == 2) {
    aprs_disconnect(g_sock); g_sock = -1; g_logged = 0;
    status("Connection lost - reconnecting...");
    /* No toast — the APRS-IS indicator turns grey; reconnection is automatic. */
    return;
  }

  /* drain inbound packets from APRS-IS */
  char line[512];
  for (int guard = 0; guard < 40; guard++) {
    int n = aprs_poll_line(g_sock, line, sizeof(line));
    if (n <= 0) break;
    route_frame(line);
  }
  ra_flush();   /* deliver multi-line bulletins once their parts have arrived */
  da_flush();   /* deliver multi-line direct messages (reassemble signed ones) */

  /* timed beacon */
  if (g_auto && g_logged) {
    uint64_t now = hal_time_epoch();
    if (now - g_last_beacon >= (uint64_t)g_interval) {
      aprs_send_beacon(g_sock, g_call, g_lat, g_lon, g_symbol, "TCPIP*", "Aurora auto-beacon");
      push_marker(g_call, g_lat, g_lon, "blue", "Aurora auto-beacon");
      g_last_beacon = now;
      status("TX auto-beacon");
    }
  }
}

void module_tick(void) {
  /* A conversation still showing a raw address asks the directory again, one
   * peer per sweep, once a minute each (lxname_resolve throttles). A name that
   * missed at first contact used to stay wrong forever — the peer that shows as
   * "3b02bb89" on one phone and its callsign on the other. Cosmetic, so it gets
   * a slow lane, never a hot loop (docs/performance.md section 4.2). */
  {
    static unsigned name_tick = 0;
    static int name_next = 0;
    if ((++name_tick % 30) == 0 && g_convo_n > 0) {
      for (int i = 0; i < g_convo_n; i++) {
        int k = (name_next + i) % g_convo_n;
        const char *id = g_convo_ids[k];
        if (id[0] != 'l' || id[1] != 'x') continue;     /* "lxmf:" rows only */
        if (lxname_get(id + 5)) continue;               /* already named */
        lxname_resolve(id + 5);
        name_next = (k + 1) % g_convo_n;
        break;                                          /* one per sweep */
      }
    }
  }

  /* BLE runs independently of the internet link (off-grid). Reconcile the
   * scan/advertise state, drain inbound frames, and beacon our position. */
  ble_reconcile();
  push_status();   /* refresh APRS-IS / BLE indicators (only on change) */

  /* The people picker is open: announces keep arriving (a hub replays its
   * whole cached table over the first minutes of a link), so re-render on a
   * slow cadence. Diffed, so a quiet poll costs one HAL read and no rebuild. */
  {
    static unsigned find_tick = 0;
    if ((++find_tick % 4) == 0 && hal_ui_attached()) {
      if (g_find_open) render_finduser();
      if (g_sa_open) render_searchall();
      /* The Nearby list is a TAB, not a screen we are told about: the host
       * switches tabs on its own, so there is no "opened" event to hang a
       * refresh on. Keep it warm whenever a UI is attached — the render is
       * diffed, so a quiet poll costs two HAL reads and nothing else. */
      if (hal_ui_attached()) {
        /* Presence goes stale by the second; rescan on a slow cadence and
         * re-render only when the picture actually changed (fnd_changed_send
         * diffs it), so an open screen costs two HAL reads a few times a
         * minute. */
        uint64_t now = hal_time_epoch();
        if (now - g_near_scan_at >= NEAR_SCAN_SEC) { near_scan(); g_near_scan_at = now; }
        render_nearby();
      }
    }
  }

  /* Flush any digipeat rebroadcasts whose staggered delay is now due. */
  rq_flush(hal_time_epoch());

  /* Close the ping collection window. */
  if (g_ping_active && hal_time_epoch() - g_ping_start > 12) {
    g_ping_active = 0;
    log_line("pingresults", "Ping complete.");
  }

  /* ── Store-and-forward housekeeping (automatic, no UI) ── */
  {
    uint64_t now = hal_time_epoch();
    int online = (g_sock >= 0 && g_logged);   /* we are an APRS-IS iGate */

    /* iGate beacon: announce ourselves so BLE-local stations know to pull mail. */
    if (online && g_ble_on && now - g_last_igate_beacon >= 120) {
      ble_tx_from(g_call, IGATE_TO, "");
      g_last_igate_beacon = now;
    }
    /* Presence beacon: a tiny callsign-only advert every PRESENCE_INTERVAL so
     * nearby stations learn we're reachable over BLE even with no GPS fix and no
     * APRS-IS uplink (Wi-Fi off). Only when Bluetooth is actually powered. */
    if (g_ble_on && hal_ble_available() &&
        now - g_ble_last_hello >= PRESENCE_INTERVAL) {
      ble_tx_from(g_call, HELLO_TO, "");
      g_ble_last_hello = now;
    }
    /* Client: while an iGate is in reach, pull our mail every 5 minutes. */
    if (g_ble_on && now - g_last_igate_heard < 600 && now - g_last_mail_query >= 300) {
      /* text = "<days> <nonce>": the look-back window the iGate should honour,
       * plus a nonce so each query is distinct on the wire. */
      char mq[24]; u_itoa((unsigned)g_mail_days, mq); s_cat(mq, " ", sizeof(mq));
      { char nb[12]; u_itoa((unsigned)now, nb); s_cat(mq, nb, sizeof(mq)); }
      ble_tx_from(g_call, MAIL_TO, mq);
      g_last_mail_query = now;
    }
    /* Persist the seen registry (debounced). */
    if (g_sdev_dirty && now - g_sdev_saved >= 60) {
      sdev_save(); g_sdev_saved = now; g_sdev_dirty = 0;
    }
    /* Re-evaluate the APRS-IS g/ filter; reconnect to apply it if it changed. */
    if (online && now - g_last_filter_check >= 30) {
      g_last_filter_check = now;
      char nf[600]; build_gfilter(nf, sizeof(nf));
      if (!s_eq(nf, g_gfilter)) {
        aprs_disconnect(g_sock); g_sock = -1; g_logged = 0;   /* re-login w/ new filter */
      }
    }
  }

  if (g_ble_on) {
    char rec[400];
    for (int guard = 0; guard < 20; guard++) {
      if (ble_poll(rec, sizeof(rec)) <= 0) break;
      char frame[300]; jstr(rec, "data", frame, sizeof(frame));
      int rssi = 0; { char rv[12]; if (jstr(rec, "rssi", rv, sizeof(rv))) rssi = to_int(rv); }
      { /* trace: prove what the wapp actually sees off the radio (sep -> '|') */
        char t[96]; s_cpy(t, "[chat] ble rx: ", sizeof(t));
        unsigned tl = s_len(t);
        for (unsigned k = 0; frame[k] && tl < sizeof(t) - 1 && k < 60; k++)
          t[tl++] = (frame[k] == 0x1f) ? '|' : frame[k];
        t[tl] = 0;
        hal_log(6, t, s_len(t));
      }
      if (frame[0]) ble_handle(frame, rssi, "BLE");   /* real Bluetooth radio */
    }
    if (g_lat != 0 || g_lon != 0) {
      uint64_t now = hal_time_epoch();
      int iv = g_interval > 0 ? g_interval : 600;
      if (now - g_ble_last_beacon >= (uint64_t)iv) {
        ble_tx_pos(g_lat, g_lon, "");   /* keep it short to fit legacy adverts */
        g_ble_last_beacon = now;
      }
    }
  }

  /* Drain inbound Reticulum datagrams (1:1 backstop + private-mode messages +
   * ?PRIV controls). Independent of APRS-IS/BLE. The payload reuses the BLE frame
   * FORMAT, so ble_handle parses + dedups it exactly like a BLE/APRS copy — shown
   * once — but it arrived over the internet via Reticulum, so it is tagged "RET"
   * (NOT "BLE": no Bluetooth radio was involved). If the same frame also arrives
   * over real Bluetooth, whichever copy lands first wins the dedup and sets the
   * tag, so a "[BLE]" tag now means it genuinely came over Bluetooth. */
  {
    static char env[1200];
    static char payb64[800];
    unsigned char frame[700];
    for (int guard = 0; guard < 20; guard++) {
      if (hal_rns_available() == 0) break;
      uint32_t n = hal_rns_recv(env, sizeof(env) - 1);
      if (n == 0) break;
      env[n] = 0;
      if (!jstr(env, "payload", payb64, sizeof(payb64))) continue;
      int fn = b64url_decode(payb64, frame, sizeof(frame) - 1);
      if (fn <= 0) continue;
      frame[fn] = 0;
      ble_handle((const char *)frame, 0, "RET");   /* rssi 0 — Reticulum over internet, no RF */
    }
  }

  /* Legacy APRS-IS connection management + drain. Runs LAST of the transports
   * and never early-returns out of module_tick: the Reticulum/relay machinery
   * below must run even with APRS-IS off (the default — no ham licence). */
  aprs_tick();

  /* Public-key beacon: broadcast our pubkey on whatever transport is up. */
  if (g_pubkey_beacon && g_pubkey[0] && (g_logged || g_ble_on || rns_up())) {
    uint64_t now = hal_time_epoch();
    if (now - g_last_pkbeacon >= (uint64_t)PKBEACON_INTERVAL) pkbeacon_send();
  }

  /* Pull store-and-forwarded 1:1 messages from every contact's propagation
   * mailbox. This is the NAT-tolerant receive path: WE initiate the outbound
   * link to pull, so a message reaches us even when both ends are behind NAT and
   * a sender's direct push to our delivery dest can't open an inbound link.
   * Pulled datagrams land on the same RNS inbox the drain below feeds to
   * ble_handle, so they flow through convo_deliver and dedup like any other. */
  {
    uint64_t now = hal_time_epoch();
    if (now - g_last_rnspull >= (uint64_t)RNS_PULL_INTERVAL) {
      g_last_rnspull = now;
      for (int i = 0; i < g_rns_n; i++) {
        if (!g_rns_prop[i][0]) continue;
        if (g_rns_dts[i] && now - g_rns_dts[i] > RNS_TTL) continue;   /* stale contact */
        hal_rns_pull(g_rns_prop[i], s_len(g_rns_prop[i]));
      }
      /* …and from everyone we have an LXMF THREAD with, contact or not.
       * Pulling only from known contacts is why a message from someone we just
       * met never arrived: they held it for us, and nobody ever asked. The
       * delivery dest resolves to the same identity as their propagation dest,
       * so it is a valid pull target. */
      for (int i = 0; i < g_convo_n; i++) {
        if (!is_lxmf(g_convo_ids[i])) continue;
        hal_rns_pull(g_convo_ids[i] + 5, s_len(g_convo_ids[i] + 5));
      }
    }
  }

  /* 1:1 messaging moved to the Mail wapp (tools.xprs.mail), which
   * owns the NOSTR kind-4 inbox. This wapp no longer polls the relays for DMs:
   * doing so delivered a SECOND copy of every message and raised a SECOND
   * notification for it, and cost a relay round-trip every 60s for a UI that no
   * longer exists here. relay_tick() is gone with it.
   */
  /* Group notes off the NOSTR subscription, and anything a NomadNet (LXMF) user
   * sent us. Both funnel into the same group conversations. */
  if (!g_sub_groups[0]) groups_subscribe();   /* the hub is not up at init */
  for (int i = 0; i < 20 && g_sub_groups[0]; i++) {
    static char evt[1600];
    int n = hal_nostr_event_recv(g_sub_groups, s_len(g_sub_groups), evt, sizeof(evt) - 1);
    if (n <= 0) break;
    evt[n] = 0;
    group_note_ingest(evt);
  }

  /* Rooms: seed the main room once our key is known, subscribe, draw the tree
   * once, then drain room defs / ops / messages. */
  room_init();
  {
    static int tree_drawn = 0;
    if (!g_sub_rooms[0]) rooms_subscribe();
    if (!tree_drawn && room_is_room(MAIN_ROOM_ID)) {
      render_rail();
      s_cpy(g_cur_room, MAIN_ROOM_ID, sizeof(g_cur_room));
      room_render_members(MAIN_ROOM_ID);
      tree_drawn = 1;
    }
  }
  for (int i = 0; i < 20 && g_sub_rooms[0]; i++) {
    static char rv[8192];
    int n = hal_nostr_event_recv(g_sub_rooms, s_len(g_sub_rooms), rv, sizeof(rv) - 1);
    if (n <= 0) break;
    rv[n] = 0;
    room_event_ingest(rv);
  }

  lxmf_drain();

  /* Cold-start 1:1: drain callsign→npub resolutions and flush queued public
   * sends as encrypted relay backups. */
  resolve_drain();

  /* recurring group bulletins: re-broadcast every 5 min until the period ends */
  if (g_logged || g_ble_on || rns_up()) {
    uint64_t now = hal_time_epoch();
    for (int i = 0; i < RECUR_MAX; i++) {
      recur_t *r = &g_recur[i];
      if (!r->active) continue;
      if (now >= r->end) { r->active = 0; continue; }
      if (now - r->last >= RECUR_INTERVAL) {
        recur_broadcast(r, 0);   /* silent re-broadcast; no self-echo */
        r->last = now;
      }
    }
  }
}

void module_handle_event(void) {
  char buf[4096];
  if (hal_msg_available() == 0) return;
  uint32_t n = hal_msg_recv(buf, sizeof(buf) - 1);
  if (n == 0) return;
  buf[n] = 0;

  char cmd[40];
  /* An app-bar / popup-menu item arrives as {type:"action",action:name} rather
   * than {command:name}. Fold it into cmd so the handlers below fire for it —
   * without this, every app-bar action was silently dropped. */
  if (!jstr(buf, "command", cmd, sizeof(cmd))) {
    char typ[24] = "";
    jstr(buf, "type", typ, sizeof(typ));
    if (!s_eq(typ, "action") || !jstr(buf, "action", cmd, sizeof(cmd))) return;
  }
  if (s_eq(cmd, "connect")) do_connect(buf);
  else if (s_eq(cmd, "disconnect")) {
    g_want_connect = 0;            /* stop auto-reconnect */
    if (g_sock >= 0) { aprs_disconnect(g_sock); g_sock = -1; g_logged = 0; }
    status("Disconnected"); notify("info", "Disconnected");
  } else if (s_eq(cmd, "center")) { read_config(buf); center_map(); }
  else if (s_eq(cmd, "send_beacon")) do_beacon(buf, 0);
  else if (s_eq(cmd, "send_emergency")) do_beacon(buf, 1);
  else if (s_eq(cmd, "toggle_timed")) {
    read_config(buf); g_auto = !g_auto; g_last_beacon = 0;
    status(g_auto ? "Auto-beacon ON" : "Auto-beacon OFF");
    notify("info", g_auto ? "Auto-beacon enabled" : "Auto-beacon disabled");
  } else if (s_eq(cmd, "conversations_send")) do_convo_send(buf);
  else if (s_eq(cmd, "conversations_open")) do_convo_open(buf);
  else if (s_eq(cmd, "conversations_private")) do_convo_private(buf);
  else if (s_eq(cmd, "conversations_hide")) do_convo_hide(buf);
  else if (s_eq(cmd, "conversations_block")) do_convo_block(buf);
  else if (s_eq(cmd, "conversations_close")) do_convo_close(buf);
  else if (s_eq(cmd, "new_chat")) do_new_chat();
  else if (s_eq(cmd, "add_group")) do_add_group();
  else if (s_eq(cmd, "room_members")) do_room_members(buf);
  else if (s_eq(cmd, "room_members_tap")) do_room_member_tap(buf);
  else if (s_eq(cmd, "rooms_open")) do_rooms_open(buf);
  else if (s_eq(cmd, "rooms_send")) do_rooms_send(buf);
  else if (s_eq(cmd, "rooms_new")) do_rooms_new(buf);
  else if (s_eq(cmd, "rooms_newchat")) do_rooms_newchat();
  else if (s_eq(cmd, "rooms_search")) do_rooms_search();
  else if (s_eq(cmd, "searchall_search")) {
    jstr(buf, "searchall_query", g_sa_q, sizeof(g_sa_q));
    render_searchall();
  }
  else if (s_eq(cmd, "searchall_tap")) do_searchall_tap(buf);
  else if (s_eq(cmd, "finduser_search")) {
    jstr(buf, "finduser_query", g_find_q, sizeof(g_find_q));
    /* lowercase: registry matching is case-insensitive, hex is lowercase */
    for (int i = 0; g_find_q[i]; i++)
      if (g_find_q[i] >= 'A' && g_find_q[i] <= 'Z')
        g_find_q[i] = (char)(g_find_q[i] + 32);
    render_finduser();
  }
  else if (s_eq(cmd, "finduser_tap")) do_finduser_tap(buf);
  /* The host knows who this conversation is with (e.g. the Reticulum graph
   * panel opened X16JK8) and the wapp does not — file the alias so the rail,
   * the header and every future row say the person's name instead of
   * "LXMF 85cdc031". */
  else if (s_eq(cmd, "convo_name")) {
    char id[96] = "", name[40] = "";
    jstr(buf, "convo_name_id", id, sizeof(id));
    jstr(buf, "convo_name", name, sizeof(name));
    if (is_lxmf(id) && name[0]) {
      lxname_set(id + 5, name);
      convo_ensure(id);
      groups_save();
      render_rail();
    }
  }
  else if (s_eq(cmd, "nearby_open")) do_nearby_open();
  else if (s_eq(cmd, "nearby_refresh")) { near_scan(); g_near_scan_at = hal_time_epoch(); render_nearby(); }
  else if (s_eq(cmd, "nearby_search")) {
    jstr(buf, "nearby_query", g_near_q, sizeof(g_near_q));
    render_nearby();
  }
  else if (s_eq(cmd, "nearby_clear")) near_clear_old();
  else if (s_eq(cmd, "nearby_tap")) do_nearby_tap(buf);
  else if (s_eq(cmd, "rooms_settings")) {
    const char *m = "{\"type\":\"ui.screen.open\",\"name\":\"Settings\"}";
    hal_msg_send(m, s_len(m));
  }
  else if (s_eq(cmd, "recur")) do_recur(buf);
  else if (s_eq(cmd, "prompt")) do_prompt_result(buf);
  else if (s_eq(cmd, "set_radius")) do_set_radius(buf);
  else if (s_eq(cmd, "ping")) do_ping(buf);
  else if (s_eq(cmd, "geochat_send")) do_geochat_send(buf);
  else if (s_eq(cmd, "activity_send")) do_activity_send(buf);
  else if (s_eq(cmd, "activity_like")) do_activity_like(buf);
  else if (s_eq(cmd, "activity_block")) do_activity_block(buf);
  else if (s_eq(cmd, "activity_mute")) do_activity_mute(buf);
  else if (s_eq(cmd, "activity_reply")) do_activity_reply(buf);
  else if (s_eq(cmd, "follow")) prompt_follow();
  else if (s_eq(cmd, "unfollow")) prompt_unfollow();
  else if (s_eq(cmd, "profile")) {            /* sender name tapped in a chat */
    char c[16] = ""; jstr(buf, "profile_call", c, sizeof(c));
    profile_show(c);
  }
  /* Follow/block actions from the host profile UI panel (operate on a callsign
   * in "profile_target"). */
  else if (s_eq(cmd, "profile_follow")) {
    char c[16] = ""; jstr(buf, "profile_target", c, sizeof(c)); if (c[0]) follow_add(c);
  } else if (s_eq(cmd, "profile_unfollow")) {
    char c[16] = ""; jstr(buf, "profile_target", c, sizeof(c)); if (c[0]) follow_remove(c);
  } else if (s_eq(cmd, "profile_block")) {
    char c[16] = ""; jstr(buf, "profile_target", c, sizeof(c));
    if (c[0]) { block_add(c); notify("info", "Blocked — you won't see their messages"); }
  } else if (s_eq(cmd, "profile_unblock")) {
    char c[16] = ""; jstr(buf, "profile_target", c, sizeof(c));
    if (c[0]) { block_remove(c); notify("info", "Unblocked"); }
  } else if (s_eq(cmd, "follows_list_tap")) { /* people-list row tapped */
    char c[16] = ""; jstr(buf, "follows_list_id", c, sizeof(c));
    profile_show(c);
  } else if (s_eq(cmd, "row_follow")) {       /* trailing button on a row */
    char c[16] = ""; jstr(buf, "follows_list_id", c, sizeof(c));
    follow_add(c);
  } else if (s_eq(cmd, "row_unfollow")) {
    char c[16] = ""; jstr(buf, "follows_list_id", c, sizeof(c));
    follow_remove(c);
  }
  else if (s_eq(cmd, "aprs_apply")) do_aprs_apply(buf);
  else if (s_eq(cmd, "ble_apply")) {
    read_config(buf);
    g_ble_on = jbool_def(buf, "ble_enabled", 1);
    g_ble_relay = jbool_def(buf, "ble_relay", 1);   /* iGate on by default */
    igate_save();
    ble_reconcile();
    status(g_ble_relay ? "iGate ON (bridging Bluetooth ↔ APRS-IS)"
                       : "iGate OFF");
  }
  else if (s_eq(cmd, "chan_apply")) {
    /* Explicit apply (like ble_apply) so the on-by-default switches aren't
     * clobbered by an unset checkbox serialised as false on other commands. */
    g_chan_local = jbool_def(buf, "chan_local", 1);
    g_chan_global = jbool_def(buf, "chan_global", 1);
    g_chan_nomad = jbool_def(buf, "chan_nomad", 1);
    chan_save();
    /* A channel switched off must also stop counting: zero the unread on every
     * hidden conversation, or the launcher badge keeps pointing at rows the
     * rail no longer shows — the "7 notifications from nowhere" bug. */
    for (int i = 0; i < g_convo_n; i++) {
      const char *cid = g_convo_ids[i];
      if (cid[0] != '#' || chan_enabled(cid)) continue;
      char m[160] = "{\"type\":\"ui.convo.upsert\",\"id\":\"";
      jesc(m, sizeof(m), cid);
      s_cat(m, "\",\"unread\":0}", sizeof(m));
      hal_msg_send(m, s_len(m));
    }
    render_rail();
    status("Channel switches applied");
    notify("info", "Channels updated");
  }
  else if (s_eq(cmd, "pubkey_apply")) {
    /* Explicit apply (like ble_apply) so the on-by-default state isn't clobbered
     * by an unset checkbox serialised as false on unrelated commands. */
    g_pubkey_beacon = jbool_def(buf, "pubkey_beacon", 1);
    pkbeacon_save();
    if (g_pubkey_beacon) {
      if (!g_pubkey[0]) { notify("warning", "No profile public key to broadcast"); }
      else { g_last_pkbeacon = 0; pkbeacon_send();    /* send one now */
             status("Public-key broadcast ON");
             notify("success", "Broadcasting your public key"); }
    } else {
      status("Public-key broadcast OFF");
      notify("info", "Public-key broadcast disabled");
    }
  }
  else if (s_eq(cmd, "keys_refresh")) pk_render();
  else if (s_eq(cmd, "sign_apply")) {
    g_sign_msgs = jbool_def(buf, "sign_msgs", 0);
    hal_kv_set("signmsgs", 8, g_sign_msgs ? "1" : "0", 1);
    if (g_sign_msgs && !g_pubkey[0])
      notify("warning", "No profile key — messages can't be signed");
    else {
      status(g_sign_msgs ? "Message signing ON" : "Message signing OFF");
      notify("info", g_sign_msgs ? "Signing outgoing messages"
                                 : "Message signing disabled");
    }
  }
  else if (s_eq(cmd, "marker_tap")) {
    char id[24] = ""; jstr(buf, "id", id, sizeof(id));
    char b[64] = "Station: "; s_cat(b, id, sizeof(b)); status(b);
  }
}

void module_destroy(void) {
  if (g_sock >= 0) { aprs_disconnect(g_sock); g_sock = -1; }
}

int32_t module_tick_interval_ms(void) { return 1000; }
