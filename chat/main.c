/*
 * chat — conversations, and nothing else.
 *
 * A 1:1 with a station, an open group (XPRS.md 6.3), a closed group (26).
 * Threading, blocking, muting, read receipts, the tick on a bubble.
 *
 * ── THIS WAPP OWNS NO TRANSPORT ─────────────────────────────────────────
 *
 * It does not read a radio, open a socket, name a Reticulum destination, or
 * put a byte on the air. It says what it wants said and to whom:
 *
 *     hal_xprs_message(to, text, private, &id)   a 1:1
 *     hal_xprs_send(wire)                        a composed packet
 *     hal_xprs_read(id)                          a person opened a message
 *
 * and the core composes, seals (9.2), signs (9.1), splits (6.6), ranks the
 * bearers (36.0), parks a custody copy and reports back. What arrives comes on
 * the event bus, already reassembled, unsealed, verified and deduped, and only
 * for the packet types this wapp subscribed to.
 *
 * It used to do all of it itself, and the list is worth keeping because every
 * item was a bug waiting: a raw BLE scan with the advertiser's address and
 * RSSI; adverts under a subtype the core had to GUESS from their content; a
 * digipeater re-airing other stations' frames with no `via:` and no hop
 * budget; frames aired under callsigns that were not this station's; an
 * APRS-IS socket; two NOSTR subscriptions; a Reticulum destination named
 * directly; a private ENC1 encryption format and a `~<sig>` signature scheme
 * layered inside `m:`, beside the seal and signature the core had already
 * done; an `am:` correlation token competing with 5's identifier; and a
 * `?PING`/`?PONG` reachability dialect the core now answers itself.
 *
 * There is no clock. module_tick is empty and the interval is 0: everything
 * here starts with something happening — a packet from the core, or a person
 * typing.
 */
#include <stdint.h>
#include "xprs_wasm_hal.h"
#include "thread.h"
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
/* One field of a packet the core delivered.
 *
 * The row carries the packet's fields verbatim and IN ORDER, as [key, value]
 * pairs -- pairs and not an object because XPRS allows a key to repeat and the
 * section 5 identifier is derived from the order, so a map would quietly
 * rename the packet. Scans that array for `["<key>",` and reads the value with
 * the same escape handling jstr uses. */
static int jfield(const char *row, const char *key, char *out, unsigned m) {
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
  /* WALK THE PAIRS. The scan used to be `for (q = arr; *q && *q != ']'; q++)`,
   * and `]` is the end of the FIRST PAIR -- `["t","message"],["f","X3WWAJ"]…`
   * -- so it could only ever find a key in field one. Every caller reading `m:`
   * got nothing, and on_core_packet's very first act is
   *
   *     if (!jfield(row, "m", m, sizeof(m)) || !m[0]) return;
   *
   * so EVERY packet the core delivered was dropped without a word. The room
   * looked empty because nothing could reach it, not because nobody spoke.
   *
   * Values are quoted and may contain a `]`, so the walk skips strings rather
   * than scanning bytes: a `]` inside somebody's message must not end the
   * array. Two `]` in a row -- the last pair's and the array's -- do. */
  if (*arr == ']') return 0;                    /* "fields":[] — no fields */
  for (const char *q = arr; *q; q++) {
    if (*q == ']' && q[1] == ']') break;        /* end of the pair array */
    if (*q == '"') {                            /* skip a quoted value whole */
      for (q++; *q && *q != '"'; q++) if (*q == '\\' && q[1]) q++;
      if (!*q) break;
      continue;
    }
    if (*q != '[') continue;                    /* only a pair can start here */
    unsigned i = 0;
    while (i < pl && q[i] == pat[i]) i++;
    if (i != pl) continue;
    q += pl;
    unsigned o = 0;
    while (*q && *q != '"' && o < m - 1) {
      if (*q == '\\' && *(q + 1)) {
        q++;
        if (*q == 'u' && hexv(q[1]) >= 0 && hexv(q[2]) >= 0 &&
            hexv(q[3]) >= 0 && hexv(q[4]) >= 0) {
          int v = (hexv(q[1]) << 12) | (hexv(q[2]) << 8) |
                  (hexv(q[3]) << 4) | hexv(q[4]);
          q += 5;
          out[o++] = (char)(v & 0xff);
        } else out[o++] = *q++;
      } else out[o++] = *q++;
    }
    out[o] = 0;
    return 1;
  }
  return 0;
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
static double to_dbl(const char *s) {
  int neg = 0; if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
  double v = 0; while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
  if (*s == '.') { s++; double f = 0.1; while (*s >= '0' && *s <= '9') { v += (*s - '0') * f; f *= 0.1; s++; } }
  return neg ? -v : v;
}
static int to_int(const char *s) { return (int)to_dbl(s); }

/* append a number with 4 decimals to dst */
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
/* A XPRS auto-generated callsign: "X1…"/"X3…". The X1/X3 prefixes are not
 * allocated by the ITU, so they can't be authority-assigned — frames from
 * such calls must NEVER be originated onto APRS-IS (own or relayed). */
/* Transport chip shown on messages. Two vocabularies arrive here and both
 * have to come out as something a person can read.
 *
 * The routing code's own tokens are uppercase and short, because they are
 * compared all over it: "NET" (APRS-IS), "RET" (Reticulum), "BLE", "RLY",
 * "LXM". The host's archive rows carry the OTHER vocabulary -- the bearer a
 * packet was actually heard on, lowercase, from XprsMonitor.kBearers: ble,
 * lan, espnow, lora, wifi, vhf, uhf, hf, plus rns and custody from the
 * courier. Map both, in one place, because a chip is the only thing that
 * tells the user whether a message walked in over Bluetooth or arrived off
 * a station's archive over the LAN.
 *
 * Anything unrecognised passes through: a new bearer should show its own
 * name rather than be silently relabelled as something it is not. */
static const char *via_label(const char *via) {
  if (!via || !via[0]) return via;
  /* routing tokens */
  if (s_eq(via, "NET"))     return "APRS-IS";
  if (s_eq(via, "RET"))     return "Reticulum";
  /* bearers, as the host's archive spells them */
  if (s_eq(via, "ble"))     return "BLE";
  if (s_eq(via, "lan"))     return "LAN";
  if (s_eq(via, "espnow"))  return "ESP-NOW";
  if (s_eq(via, "lora"))    return "LoRa";
  if (s_eq(via, "wifi"))    return "WiFi";
  if (s_eq(via, "vhf"))     return "VHF";
  if (s_eq(via, "uhf"))     return "UHF";
  if (s_eq(via, "hf"))      return "HF";
  if (s_eq(via, "radio"))   return "Radio";
  if (s_eq(via, "rns"))     return "Reticulum";
  /* Not a bearer: a packet handed over by a station that had been holding it
   * for us. Where it travelled last is less interesting than the fact that
   * nobody was in earshot when it was sent. */
  if (s_eq(via, "custody")) return "Carried";
  return via;
}
static double g_lat = 0, g_lon = 0;
static int   g_radius = 100;
static char  g_symbol[8] = "/>";
static char  g_path[64] = "WIDE1-1,WIDE2-1";
static int   g_interval = 600;          /* seconds */
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
/* compact BLE senders, defined with the module entry points */
/* Best-hope custody: air a 1:1 only when nothing else can carry it. Defined
 * with the other BLE senders; declared here because send_message is above. */
/* Substring search — defined with the nearby-list helpers, used earlier by the
 * custody identity lookup. */
#define REACH_NONE  0   /* nothing anywhere → best hope: air it */
#define REACH_LOCAL 1   /* BLE/radio neighbour → air it, arrives directly */
#define REACH_NET   2   /* LAN or live internet path → do NOT air it */

/* Where can this callsign be reached right now? Defined next to the nearby
 * table it reads. */
static void log_line(const char *field, const char *text);
/* Build "label/value" chips for callsigns heard over BLE within REACH_WINDOW
 * (most-recent first). Returns the number of chips written (defined with the
 * seen-over-BLE registry, far below). */
/* Reticulum 1:1 sender (defined after the BLE frame packer); fans the same frame
 * out to every RNS delivery dest advertised under the recipient's npub. */
static int rns_tx_msg(const char *to, const char *wire);
/* Air one packet and report its section 5 identifier -- what the core keys its
 * outbox on, and what a receipt names in `r:`. Defined with the BLE packer. */
static int xprs_air_id(const char *to, const char *text, char rid[7]);
/* Reticulum is the PRIMARY transport (APRS-IS is legacy/opt-in and requires a
 * licensed callsign): 1 when the local RNS node is up. Defined with rns_tx_msg. */
/* Broadcast a position over the licence-free paths (BLE if on, Reticulum if up).
 * Defined with the BLE frame packer. */
/* Air a bulletin / area post: one hal_xprs_send, which the core fans out over
 * every bearer it has evidence for. Defined with the BLE frame packer. */
static int air_bulletin(const char *to, const char *text);

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
#define RNS_PULL_INTERVAL 20              /* seconds: pull store-and-forwarded 1:1 mail */
static char  g_pubkey[80] = "";           /* our pubkey (base64url), cached at init */
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
/* Stations that follow US — learned from directed "?FOLLOW"/"?UNFOLLOW"
 * control messages peers send when they (un)follow a callsign. */
static void profile_show(const char *call);   /* fwd: station profile sheet */
static void host_state_emit(const char *kind, const char *call, int on); /* fwd */
/* ── BLE ping (Tools tab): local reach test across digipeaters ──────────
 * A ping is a BLE-only broadcast (never APRS-IS, never shown on the Live
 * feed). Every BLE station answers once with its callsign + position and
 * forwards the ping (ttl) so it travels further; replies are forwarded back
 * (pttl) so multi-hop responders still reach the pinger. */
#define PING_TO "?PING"
#define PONG_TO "?PONG"
#define PING_DEFAULT_TTL 3
#define GPS_NA (-2147483647 - 1)   /* hal_sensor_gps_* "unavailable" sentinel */
#define RECUR_MAX 8
#define RECUR_INTERVAL 300            /* 5 minutes between re-sends */
typedef struct {
  int active;
  char group[8];
  char text[80];
  uint64_t end;                       /* stop re-sending at this epoch */
  uint64_t last;                      /* epoch of the last send */
} recur_t;
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

/* The transport indicators (`ui.map.status` -> the green RET / NET / BLE chips
 * in the AppBar) are gone. They sat in the one place a chat screen has for its
 * title and its way out, and told the reader something they cannot act on:
 * which of three transports happened to be up. Where a transport matters it is
 * said where it matters -- the per-message bearer chip says how THAT message
 * arrived, and Settings says what is switched on. The host still renders
 * `ui.map.status` for any wapp that pushes it (docs/aprs.md section 5); this
 * one no longer does. */

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
/* via: transport the message arrived on ("BLE"/"NET"); "" for our own sends.
 * The host renders it as a small origin chip so users can tell where a
 * received message came from. */
/* detail = the station's latest comment/message ("" if none). The host shows
 * it, plus lat/lon and a relative "last heard" time, in the marker popup —
 * so we send the heard epoch (seconds) and let the host format it. */
static void u_itoa(unsigned v, char *out);   /* defined with the messenger code */
/* Tell the host the coverage circle: my station + the filter radius. */
/* Drop the old area's pins + geo-chat when the radius changes. */
/* Ask the host to replay archived Live geo-chat for the current area into the
 * Live tab. The host persists every geo-tagged Live message and answers this
 * by centre+radius, so opening the wapp (or changing the radius) brings back
 * the older messages that happened in the selected region. */
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
  /* NOTE: BLE on/off is intentionally NOT read here. read_config runs on
   * every command (connect, sends, …) and the host serialises an unset
   * checkbox as false, which would clobber the on-by-default state before the
   * user ever touches Settings. BLE state is owned by init (default on) and
   * the explicit "Apply Bluetooth" action instead — see the ble_apply cmd. */
}

/* Persistence + validation for the APRS panel — declared with the other KV
 * savers (see aprsis_save/aprsis_load near igate_save). */
/* A plausible authority-assigned callsign: 3-7 alphanumeric chars containing
 * at least one digit and one letter, optionally "-<1..2 digit SSID>". This is
 * a sanity filter, not a licence check — the passcode match is the gate the
 * APRS-IS servers themselves enforce. */
/* APRS panel "Verify & apply": validate the licensed callsign + passcode and
 * flip the APRS-IS switch. Everything is checked BEFORE anything is enabled —
 * a wrong passcode (or an auto-generated X1/X3 call) leaves APRS-IS off. */
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

/* Separate raw-frame dedup (cross-transport + relay loop guard), kept apart
 * from the conversation seen-ring above so it can't evict pin-detection keys.
 * Time-windowed: a frame is suppressed for FSEEN_WINDOW after it is first seen,
 * so a message re-broadcast many times (BLE adverts repeat for their TTL, and
 * the mesh relays them) is shown only once. A plain count ring evicted recent
 * hashes once enough other frames arrived, letting duplicates reappear. */
#define FSEEN_MAX 256
#define FSEEN_WINDOW 3600   /* 60 minutes */
/* Content dedup for the Live/Beacons geo-chat. The same message reaches us as
 * different raw frames — over BLE and over APRS-IS — and APRS-IS itself can
 * deliver duplicates via multiple IGates, so the per-frame fseen ring above
 * can't catch them. Dedup on sender+text (transport-independent) so a message
 * shows once per 60 min. Returns 1 if it's a duplicate to drop. */
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
static uint32_t notif_hash(const char *from, const char *text) {
  uint32_t h = 5381;
  for (int i = 0; from && from[i]; i++) h = h * 33u + (unsigned char)from[i];
  for (int i = 0; text && text[i]; i++) h = h * 33u + (unsigned char)text[i];
  return h ? h : 1u;
}

static int notif_dup(const char *from, const char *text) {
  uint32_t h = notif_hash(from, text);
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
static int convo_renderable(const char *id);   /* defined with the convo store */
static int cmd_field(const char *buf, const char *key, char *out, unsigned m);
static int lxmf_callsign(const char *dest, char *out, unsigned cap);
/* [mid] is the message's durable id where the caller has one. It becomes the
 * notification's dedupe tag, which the host honours ACROSS RESTARTS -- and a
 * restart is exactly when this matters: every start re-ingests the backlog and
 * re-announced messages the user read days ago, which is how the bell ends up
 * permanently lit. The 60-second notif_dup ring above cannot help there; it
 * lives in RAM and dies with the process. Falls back to a hash of sender +
 * text for the callers that have no id to give. */
static void notify_msg(const char *title, const char *from, const char *text,
                       const char *body, const char *mid) {
  if (!title || !title[0]) return;
  /* Only what this wapp can actually open and show. A 1:1 keyed by callsign is
   * Mail's; notifying it here would double up and then land the user in a wapp
   * with no thread to show them. */
  if (!(convo_renderable(title) || s_eq(title, "Activity"))) return;
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
  s_cat(m, "\",\"tag\":\"chat:", sizeof(m));
  jesc(m, sizeof(m), title);
  s_cat(m, ":", sizeof(m));
  if (mid && mid[0]) {
    jesc(m, sizeof(m), mid);
  } else {
    char hx[12];
    uint32_t h = notif_hash(from, text);
    for (int i = 7; i >= 0; i--) {
      hx[i] = "0123456789abcdef"[h & 0xf];
      h >>= 4;
    }
    hx[8] = 0;
    s_cat(m, hx, sizeof(m));
  }
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* BLE mesh repeater: rebroadcast each received frame once, suppressing any
 * content already repeated within the last 10 minutes (loop/storm control). */
#define RPT_MAX 64
typedef struct { char call[16]; double lat, lon; int used; } pos_t;
/* cos via Taylor (lat in radians, |x| < pi/2 — well within range) */
/* Equirectangular distance; writes "<n> km"/"<n> m" badge, 1 if known. */
/* Distance from our position (the map pinpoint) to lat/lon, as "<n> km"/"m". */
/* Distance to a callsign's last-known position (1 if known). */
/* km from us to a callsign's last-known position, or -1 if unknown. */
/* True only when we positively know the sender sits inside our coverage radius
 * (so a local group bulletin can be filed as "local"); unknown position = no. */
/* Conversations the host knows about, so we can refresh the distance badge
 * when a contact's position arrives. */
static char g_convo_ids[32][40];
static int g_convo_n = 0;

/* ── The scope room (XPRS 13.11): #LOCAL ──────────────────────────────────
 * One built-in conversation carried as plain t:message broadcasts through the
 * host's XPRS lane -- the same wires the ESP32 hotspot chat and every other
 * station speak, so writing here is writing there. Sending is
 * hal_xprs_broadcast(text, "local"): the core composes, signs, splits and
 * keeps it on the short-range bearers, never gated to the internet (13.11.1).
 * Receiving is the event bus, in on_core_packet -- undirected traffic with
 * scope:local. It used to be a poll of hal_xprs_history instead, which is why
 * the room went silent when the polling was removed and nothing replaced it.
 *
 * #GLOBAL is GONE. It was the unmarked room that went everywhere, and an
 * unmarked broadcast is exactly what nothing can repair: no d:, so no custody,
 * no ack, no retry -- one advert into a radio that transmits five seconds a
 * minute. A message reached one phone and not another with nothing to fix it
 * inside ten minutes. It also double-surfaced: a remote geochat frame is an
 * unscoped undirected t:message, so it landed on the Live tab AND here.
 *
 * Undirected SENDING is untouched -- geochat needs it, it is the only class
 * admitted past the host's declaration rule, and it is what catch-up fetches.
 * What is gone is the room that showed it. */
#define XROOM_LOCAL  "#LOCAL"
static int xroom_is(const char *id) { return s_eq(id, XROOM_LOCAL); }

static void xgroups_publish(void);
static void convo_ensure(const char *id);   /* defined further down */

/* ── Closed groups (XPRS 26) as conversations ─────────────────────────────
 *
 * A group IS a room. Its id here is "#" + the group's X5 callsign, so it is a
 * group to every predicate in this file already (is_group, convo_renderable,
 * convo_touch) and needs no new kind of conversation.
 *
 * The host owns the answer to "which groups, and what am I in them": section
 * 26.4 replays signed acts against a callsign->key map that lives in the core.
 * hal_xprs_groups hands that over; this wapp never decides membership, it only
 * renders it and refuses to compose where it has no standing. */
#define XGROUP_MAX 16
static struct {
  char call[8];   /* X5ABCD */
  char nick[24];
  char role[8];   /* admin | mod | member | invited | none */
} g_xgroup[XGROUP_MAX];
static int g_xgroup_n;

/* The list as it was last time, so a room is on screen the instant the page
 * opens instead of a few seconds later.
 *
 * Membership is the host's answer and it takes a HAL round trip; asking for it
 * before drawing anything is what made the rooms rail arrive late and reflow
 * under the reader's eyes. So: paint from KV at init, ask in the background,
 * and only touch the host again when the answer actually changed. The cache
 * being briefly stale is harmless -- the worst case is a room that appears for
 * a few seconds after you were removed, and the post path asks the live table,
 * never this one. */
static char g_xgroup_kv[640];   /* "CALL|nick|role;" repeated; what we last saw */

/* "#X5ABCD" — '#' plus a six-character X5 callsign. */
static int xgroup_is(const char *id) {
  return id && id[0] == '#' && id[1] == 'X' && id[2] == '5' && s_len(id) == 7;
}
static const char *xgroup_call(const char *id) { return id + 1; }

static int xgroup_find(const char *call) {
  for (int i = 0; i < g_xgroup_n; i++)
    if (s_eq(g_xgroup[i].call, call)) return i;
  return -1;
}

/* 26.3.1: a grant confers nothing until the person accepts it, so `invited` is
 * somebody who has been asked and has not answered — they may not post. */
static int xgroup_may_post(const char *call) {
  int i = xgroup_find(call);
  if (i < 0) return 0;
  return s_eq(g_xgroup[i].role, "member") || s_eq(g_xgroup[i].role, "mod") ||
         s_eq(g_xgroup[i].role, "admin");
}

/* The table as one line, so "did anything change?" is a string compare rather
 * than a rescan -- and so the same bytes are what we persist. */
/* Rebuild the table from one of those lines. */
static void xgroup_parse(const char *in) {
  g_xgroup_n = 0;
  int f = 0, k = 0;
  char call[8] = "", nick[24] = "", role[8] = "";
  for (const char *p = in; *p; p++) {
    if (*p == '|') { f++; k = 0; continue; }
    if (*p == ';') {
      if (call[0] && g_xgroup_n < XGROUP_MAX) {
        s_cpy(g_xgroup[g_xgroup_n].call, call, sizeof(g_xgroup[0].call));
        s_cpy(g_xgroup[g_xgroup_n].nick, nick, sizeof(g_xgroup[0].nick));
        s_cpy(g_xgroup[g_xgroup_n].role, role, sizeof(g_xgroup[0].role));
        g_xgroup_n++;
      }
      call[0] = nick[0] = role[0] = 0;
      f = 0; k = 0;
      continue;
    }
    if (f == 0) { if (k < (int)sizeof(call) - 1)  { call[k++] = *p; call[k] = 0; } }
    else if (f == 1) { if (k < (int)sizeof(nick) - 1) { nick[k++] = *p; nick[k] = 0; } }
    else if (f == 2) { if (k < (int)sizeof(role) - 1) { role[k++] = *p; role[k] = 0; } }
  }
}

/* Put a row on screen for every group we may speak in. */
static void xgroups_publish(void) {
  for (int k = 0; k < g_xgroup_n; k++) {
    if (!xgroup_may_post(g_xgroup[k].call)) continue;
    char rid[10] = "#";
    s_cat(rid, g_xgroup[k].call, sizeof(rid));
    convo_ensure(rid);
  }
}

/* Draw from the last known answer, before asking for a new one. */
static void xgroups_restore(void) {
  uint32_t n = hal_kv_get("xgrp", 4, g_xgroup_kv, sizeof(g_xgroup_kv) - 1);
  if (n == 0 || n >= sizeof(g_xgroup_kv)) { g_xgroup_kv[0] = 0; return; }
  g_xgroup_kv[n] = 0;
  xgroup_parse(g_xgroup_kv);
  xgroups_publish();
}
/* Section 5 identifiers already rendered in the Local room, so one message is
 * one bubble.
 *
 * It catches two different repeats with one test. Our OWN post: the core hands
 * the identifier back from hal_xprs_broadcast, we register it here, and the
 * copy that comes off our own radio is recognised rather than drawn twice.
 * And SOMEBODY ELSE'S post heard on two bearers: the identifier is derived
 * from the packet and never transmitted (section 5), so the Bluetooth copy and
 * the LAN copy carry the same one.
 *
 * 192 deep. It used to be sized against a history poll's window -- the poll
 * re-walked the same rows every four seconds, so a ring smaller than a window
 * forgot the oldest row just in time to redraw it. That poll is gone; the ring
 * now only has to outlive the spread between one packet arriving on two
 * bearers, and 192 is generous for that. */
#define XROOM_SEEN 192
static char g_xroom_seen[XROOM_SEEN][12];
static int  g_xroom_seen_n;
static int xroom_seen(const char *mid) {
  if (!mid[0]) return 1;
  for (int i = 0; i < XROOM_SEEN; i++) if (s_eq(g_xroom_seen[i], mid)) return 1;
  s_cpy(g_xroom_seen[g_xroom_seen_n % XROOM_SEEN], mid, 12);
  g_xroom_seen_n++;
  return 0;
}

static void render_rail(void);   /* the rail redraws when this set changes */

static void convo_remember(const char *id) {
  for (int i = 0; i < g_convo_n; i++) if (s_eq(g_convo_ids[i], id)) return;
  if (g_convo_n >= 32) return;
  s_cpy(g_convo_ids[g_convo_n++], id, 40);
  /* A conversation not on the rail cannot be opened, so the two are one act.
   * Here and not in convo_touch: touching happens per MESSAGE, and redrawing
   * the whole rail on every bubble would be six kilobytes of JSON to say
   * nothing changed. */
  render_rail();
}
/* Drop [id] from the subscribed set (so we stop listening to that group/DM). */
static void convo_forget(const char *id) {
  for (int i = 0; i < g_convo_n; i++) {
    if (s_eq(g_convo_ids[i], id)) {
      for (int j = i; j < g_convo_n - 1; j++) s_cpy(g_convo_ids[j], g_convo_ids[j + 1], 40);
      g_convo_n--;
      render_rail();
      return;
    }
  }
}
static void group_convo_id(const char *gname, char *out, unsigned cap);

static int convo_known(const char *id) {
  for (int i = 0; i < g_convo_n; i++) if (s_eq(g_convo_ids[i], id)) return 1;
  return 0;
}
static void groups_save(void);   /* fwd: persist subscribed groups to KV */

/* ── generic ui.convo.* senders ── */
/* append "lat":..,"lon":.. to m when the position is known (not 0,0). */
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
/* The form the NEXT 1:1 goes out in (XPRS.md section 9.2: `x:` sealed, `m:`
 * plain). Private by default, which is what section 9.4 says a direct message
 * is on every band we transmit on.
 *
 * This is a composer setting, not a conversation mode and not something the
 * peer is told: privacy in XPRS is a property of each packet, so either side
 * flips it whenever they like and the wire says which was used. Nothing is
 * negotiated and nothing has to agree. */
static int g_want_private = 1;
static char g_send_rid[8] = "";
static char g_send_status[12] = "";
static int is_group(const char *id);   /* '#' prefix; defined below */

static void convo_msg(const char *id, const char *dir, const char *from,
                      const char *text, const char *key, const char *meta,
                      const char *via,
                      const char *mid, const char *parent, const char *auth, int enc,
                      int priv) {
  /* Only what this wapp can open: a group, a peer by callsign, or a NomadNet
   * peer with no callsign (see is_callsign_id). */
  uint64_t stamp = g_msg_epoch; g_msg_epoch = 0;  /* one message, one stamp */
  if (!convo_renderable(id)) return;
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
  s_cat(m, "\"", sizeof(m));
  cat_thread(m, sizeof(m), mid, parent, auth, enc);
  /* Private = this message went Reticulum-only (never APRS) — the host tags the
   * bubble so it's clearly distinct from public APRS traffic. */
  if (priv) s_cat(m, ",\"private\":true", sizeof(m));
  /* The other half of the statement (XPRS.md section 9.2). Said out loud on a
   * 1:1 so an unlabelled bubble never has to be guessed at: `enc` set means the
   * body was sealed, `plain` set means it was readable, and one of the two is
   * always true of a direct message. */
  else if (s_pre(id, "lxmf:") && !enc) s_cat(m, ",\"plain\":true", sizeof(m));
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

/* ── 1:1 delivery/read receipts (XPRS.md section 13.7) ────────────────────
 *
 * This wapp used to run a receipt protocol of its own: a random "am:<6hex>"
 * token prepended to every 1:1 wire, echoed back inside a "?ACK <am> d|r"
 * control frame. It was a second correlation id competing with the one the
 * format already defines, it was unsigned -- and 13.7.1 is blunt about why
 * that matters, since a receipt is what makes every carrier drop its held copy
 * -- and no station outside this wapp spoke a word of it.
 *
 * 13.7's receipt is `t:receipt r:<section 5 id> s:ack|read`, signed, and the
 * core composes, signs, verifies and releases on it. What is left here is the
 * two ends of that:
 *
 *   - the bubble's rid is the section 5 identifier of the packet we aired, so
 *     the core's outbox and our bubble are keyed on the same thing;
 *   - `xprs.status.tx` reports what the core learned, and we draw the tick;
 *   - hal_xprs_read tells the core a person opened the message, which is the
 *     one half of 13.7 the core cannot observe for itself.
 *
 * Status is still pushed to the host as ui.convo.status keyed by rid. */

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
#define RPEND_MAX 64
/* Wide enough for an LXMF conversation id ("lxmf:" + 32 hex = 37). */
static char g_rpend_convo[RPEND_MAX][48];
static char g_rpend_id[RPEND_MAX][8];
static int  g_rpend_n = 0;
static void rpend_add(const char *convo, const char *id) {
  if (!id || !id[0]) return;
  for (int i = 0; i < g_rpend_n; i++) if (s_eq(g_rpend_id[i], id)) return;
  if (g_rpend_n >= RPEND_MAX) {                 /* drop oldest */
    for (int i = 1; i < RPEND_MAX; i++) {
      s_cpy(g_rpend_convo[i-1], g_rpend_convo[i], sizeof(g_rpend_convo[0]));
      s_cpy(g_rpend_id[i-1], g_rpend_id[i], 8);
    }
    g_rpend_n = RPEND_MAX - 1;
  }
  s_cpy(g_rpend_convo[g_rpend_n], convo, sizeof(g_rpend_convo[0]));
  s_cpy(g_rpend_id[g_rpend_n], id, 8);
  g_rpend_n++;
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
/* The user opened a conversation, so its messages have been read. Report each
 * one to the core BY IDENTIFIER and let it do the rest: 13.7.1's exclusions,
 * the signature, and which lane the receipt goes out on. That this wapp knows
 * a person looked at the screen is the one fact the core cannot observe, and
 * it is the whole of what a wapp owes a receipt. */
static void rpend_flush_read(const char *convo) {
  if (!convo || !convo[0] || convo[0] == '#') return;  /* groups: no receipts */
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

/* User opened a 1:1 conversation → send read receipts for its pending msgs. */
static void do_convo_open(const char *buf) {
  /* 48, not 16: an LXMF conversation id is "lxmf:" + 32 hex. */
  char convo[48] = ""; cmd_field(buf, "convo", convo, sizeof(convo));
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
/* Tell the host which callsigns to hide from the Activity feed (blocked + muted),
 * so existing posts disappear too — not just future ones. The host filters its
 * activity list by this set. Re-sent whenever the lists change (and on init). */
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
}
/* Mute: hide a callsign's NEW messages (Activity + groups + DMs) without
 * discarding their conversation or existing bubbles. Local + persisted. */
static void block_remove(const char *call) {
  char up_call[16]; int j = 0;
  for (int i = 0; call[i] && j < 15; i++) up_call[j++] = up(call[i]);
  up_call[j] = 0;
  for (int i = 0; i < g_blocked_n; i++) if (s_eq(g_blocked[i], up_call)) {
    for (int k = i; k < g_blocked_n - 1; k++) s_cpy(g_blocked[k], g_blocked[k + 1], 16);
    g_blocked_n--; blocked_save();
    host_state_emit("block", up_call, 0);
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
static unsigned recent_of(const char *id) {
  for (int i = 0; i < g_recent_n; i++)
    if (s_eq(g_recent_id[i], id)) return (unsigned)g_recent_ts[i];
  return 0;
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
static void lxmf_title(const char *id, char *out, unsigned osz);
static void convo_title(const char *id, char *out, unsigned osz) {
  if (s_pre(id, "lxmf:")) { lxmf_title(id, out, osz); return; }
  /* The scope room is not a group with a name and a reach tag -- it IS the
   * reach. The tag below reads the '*' that marks a global GROUP, which a
   * scope room does not carry, so say what this one is instead. */
  if (s_eq(id, XROOM_LOCAL))  { s_cpy(out, "Local chat", osz);  return; }
  /* A group carries its own name; the "(local)/(global)" reach tag below is
   * about a NOSTR group's scope and says nothing true about a closed one. */
  if (xgroup_is(id)) {
    /* Name AND callsign, always. The name is a label anybody can choose and
     * two groups may pick the same one; the X5 callsign is derived from the
     * key and is the only half that identifies the group (26.1). Showing the
     * name alone would let a second "lisboa-net" pass for this one. */
    int i = xgroup_find(xgroup_call(id));
    if (i >= 0 && g_xgroup[i].nick[0]) {
      s_cpy(out, g_xgroup[i].nick, osz);
      s_cat(out, " (", osz);
      s_cat(out, xgroup_call(id), osz);
      s_cat(out, ")", osz);
      return;
    }
    s_cpy(out, xgroup_call(id), osz);
    return;
  }
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

/* ── The rail: the ONLY way a conversation is reachable ──────────────────
 *
 * The Chat screen is one widget -- screens/home.ui.json declares a single
 * {"$":"group","name":"rooms","$type":"rooms"} -- and the host fills it from
 * exactly one message, ui.rooms.set. render_rail() and the whole of room.c
 * were deleted with the NOSTR rooms, and nothing emitted that message
 * afterwards, so the screen had NO ROWS AT ALL: every conversation this wapp
 * held was unreachable, the Local room included. That is what "the Local chat
 * is gone" looked like from the outside.
 *
 * This is the old renderer minus the room tree it used to hang under. There is
 * no "Main room" and no parent/child nesting: this client's conversations are
 * a flat list -- the Local room, the groups it has joined, and the peers it
 * talks to -- so the rail is that list, most recently used first.
 *
 * No timer. It is redrawn when the conversation set changes, like everything
 * else in this wapp. */
static void rail_item(char *out, unsigned cap, const char *id,
                      const char *name) {
  s_cat(out, "{\"id\":\"", cap); jesc(out, cap, id);
  s_cat(out, "\",\"name\":\"", cap); jesc(out, cap, name);
  s_cat(out, "\",\"depth\":0", cap);
  { char tb[16]; u_itoa(recent_of(id), tb);
    s_cat(out, ",\"seen\":", cap); s_cat(out, tb, cap); }
  s_cat(out, "}", cap);
}

static void render_rail(void) {
  static char rail[6000];
  s_cpy(rail, "{\"type\":\"ui.rooms.set\",\"field\":\"rooms\",\"rooms\":[",
        sizeof(rail));
  /* Most recently used first -- the rail's whole job is to lead with what you
   * actually open. Insertion sort over at most 32 ids, on an event, so the
   * quadratic shape costs nothing and needs no scratch array. */
  int idx[32], cnt = 0;
  for (int i = 0; i < g_convo_n && cnt < 32; i++) idx[cnt++] = i;
  for (int a = 0; a < cnt; a++)
    for (int b = a + 1; b < cnt; b++)
      if (recent_of(g_convo_ids[idx[b]]) > recent_of(g_convo_ids[idx[a]])) {
        int t = idx[a]; idx[a] = idx[b]; idx[b] = t;
      }
  int first = 1;
  for (int k = 0; k < cnt; k++) {
    const char *id = g_convo_ids[idx[k]];
    /* Exactly what this wapp can draw, and the same test convo_msg applies
     * before it renders a bubble: a row that opens onto nothing is worse than
     * no row, and that pairing is what convo_drop_ghost exists to clean up. */
    if (!convo_renderable(id)) continue;
    /* A channel the user switched off stays off the rail. */
    if (is_lxmf(id) ? !g_chan_nomad : !chan_enabled(id)) continue;
    char title[80];
    convo_title(id, title, sizeof(title));
    if (!first) s_cat(rail, ",", sizeof(rail));
    first = 0;
    rail_item(rail, sizeof(rail), id, title);
  }
  s_cat(rail, "]}", sizeof(rail));
  hal_msg_send(rail, s_len(rail));
  /* Say what was drawn, by id. Three rows titled "X1WATT" are three ids, and
   * the title is the one thing that does not tell them apart. Once per redraw,
   * and a redraw is a set change, not a message. */
  { char lg[900] = "[chat] rail:";
    for (int k = 0; k < cnt; k++) {
      s_cat(lg, " ", sizeof(lg)); s_cat(lg, g_convo_ids[idx[k]], sizeof(lg));
    }
    hal_log(1, lg, s_len(lg)); }
}

/* Our own LXMF delivery dest — what a peer sees as `from`, and therefore half
 * of the thread id of everything we send (msg_id("<from>|<text>")). Deriving
 * ids the same way on both ends is what lets a reply or a heart name its
 * target on a transport with no room for extra fields. Cached: the dest is
 * fixed while the node runs, and the call returns 0 until it is up. */
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
/* A 1:1 IS KEYED BY CALLSIGN.
 *
 * It was keyed two ways at once, and neither worked in both directions. An
 * inbound message opened a row called "lxmf:<delivery dest>" (titled with the
 * callsign, so it LOOKED right); replying from it handed that dest to
 * hal_xprs_message as the recipient, which composed `d:LXMF:9FE0…` -- a packet
 * for nobody. A row opened from a callsign could send, but every render guard
 * in this file refused anything that was not `#…` or `lxmf:…`, so the bubble
 * never appeared, and convo_drop_ghost deleted the row at the next start.
 * The user saw two "X1WATT" rows and could not tell which one worked. Neither.
 *
 * The core addresses a station by callsign (hal_xprs_message) and names the
 * sender's callsign on every delivery row (`call`). So that is the id. An
 * `lxmf:` row survives only for a NomadNet peer that has no callsign at all. */
static int is_callsign_id(const char *id) {
  return id && id[0] != '#' && !is_lxmf(id) && xprs_is_station(id);
}
static int convo_renderable(const char *id) {
  return id && id[0] && (is_group(id) || is_lxmf(id) || is_callsign_id(id));
}

static void convo_drop_ghost(const char *id) {
  if (!id || !id[0]) return;
  char m[140] = "{\"type\":\"ui.convo.remove\",\"id\":\"";
  jesc(m, sizeof(m), id);
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

static void convo_touch(const char *id, const char *preview, int select) {
  if (!convo_renderable(id)) return;
  convo_remember(id);
  recent_touch(id);   /* traffic counts as recency, and survives a restart */
  int global = 0; for (int i = 1; id[i]; i++) if (id[i] == '*') global = 1;
  const char *icon = (id[0] == '#') ? (global ? "public" : "campaign") : "person";
  char badge[24] = "";
  /* Wide enough for "name (X5ABCD)": a group title carries both, and 24
   * cut the callsign off exactly where it stopped being useful. */
  char title[48]; convo_title(id, title, sizeof(title));
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
/* ── XPRS message signatures ──────────────────────────────────────────── */
/* base85 alphabet — must match the host (lib/util/xprs_crypto.dart). */
/* A signed message ends with " ~<60 base85 chars>". If present, copy the body
 * (without that suffix) into [core] and the 60-char signature into [sig], and
 * return 1; else 0. */
/* canonical signed bytes = "<from>|<core>" (must match the signer) */
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
 * works too: the key arrives when the relay resolve answers. */
#define PEER_MAX 64
static char g_peer[PEER_MAX][16];          /* callsigns we interact with */
static int  g_peer_n = 0;
static int peer_known(const char *call) {
  for (int i = 0; i < g_peer_n; i++) if (s_eq(g_peer[i], call)) return 1;
  return 0;
}
/* Note that we interact with [call]. Keys arrive through the relay resolve
 * path (resolve_drain) -- the parked-key table that used to sit here fed off
 * the removed #NOSTR pubkey bulletin. */
static void peer_note(const char *call) {
  if (!call[0] || call[0] == '#' || s_eq(call, g_call)) return;
  if (!peer_known(call) && g_peer_n < PEER_MAX) s_cpy(g_peer[g_peer_n++], call, 16);
}

/* ── follow list persistence + mutation ─────────────────────────────────── */
/* KV "follows": "CALL=tag1 tag2;CALL;…" — '=' starts the optional tag list
 * (callsigns never contain '='); the legacy "CALL;" form still parses. */
/* Set (or clear) the space-separated tags on a followed callsign. */
/* A peer announced they (un)followed us. Update the Followers list; this is
 * control traffic, never shown as a chat message. */
/* Intercept a directed ?FOLLOW / ?UNFOLLOW control message (returns 1). */
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
/* Tell the host about a like vote on an Activity post (so it can tally it). */
/* Echo one of OUR Activity posts (dir "out") with a mid so it can receive likes
 * + replies like any other post. */
/* A followed station's non-message activity (status / geo-chat post). [grp] is
 * the group context ("" for a status). Kept follow-gated so the feed isn't
 * flooded by every station's position comment. */
/* Deliver one conversation message: dedup by signature — first time shows in
 * the flow, a repeat is promoted to a pinned item (and further repeats are
 * ignored as updates of the same pin). [forcePin] is set for our own
 * recurring sends (pinned from the first beat). */
/* Returns 1 if a message bubble was delivered, 0 if dropped (a like vote or a
 * repeated/duplicate message) — callers gate notifications on this so recurring
 * bulletins/duplicates don't re-notify. */
static void trc(const char *tag, const char *a, const char *b);

/* The core's verdict on the message being delivered, handed over on the row
 * rather than recomputed here. Set immediately before convo_deliver and
 * cleared by it, because a verdict that outlived its message would label the
 * next one. */
static char g_row_sig[12] = "";
static int  g_row_sealed = 0;

static int convo_deliver(const char *id, const char *dir, const char *from,
                          const char *text, const char *preview,
                          const char *via) {
  /* Local block: never show anything from a blocked station (their own echoes of
   * our messages — dir "out" from g_call — are unaffected). */
  if (s_eq(dir, "in") && (is_blocked(from) || is_muted(from))) {
    trc("drop:blocked", from, "");
    return 0;
  }
  /* No `am:` to pull off any more: the correlation id is the section 5
   * identifier, which is derived from the packet rather than carried in it, so
   * nothing has to be stripped before display. */
  char ambuf[720];
  if (id[0] != '#') {
    s_cpy(ambuf, text, sizeof(ambuf));
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
  }
  /* Interacting with this callsign: capture its public key if we'd parked one. */
  if (s_eq(dir, "in")) peer_note(from);
  else if (id[0] != '#') peer_note(id);
  /* Threading is group-only: derive this message's id from the wire text and,
   * if it carries a "+<4hex> " reply marker, split off the parent + show the
   * text without the marker. 1:1 chats are untouched. */
  char mid[5] = "", parent[5] = "";
  /* THE WAPP'S OWN MESSAGE FORMAT IS GONE FROM HERE.
   *
   * Four schemes used to be layered inside `m:`, each duplicating something
   * the format already has and the core already does:
   *
   *   ENC1:<base64>        a private encryption scheme, decrypted here with
   *                        hal_decrypt. 9.2's `x:` replaced it, and the core
   *                        unseals before it hands the message over.
   *   ~<sig>               a private signature over a canonical form of this
   *                        wapp's invention, checked with hal_verify --
   *                        running beside 9.1's `sig:`, which the core had
   *                        already verified and simply could not report.
   *   \x01<rmid>\x02       a correlation id for the NOSTR-relay copy of a
   *                        message. That lane is gone; 5's identifier names
   *                        a message on every lane at once.
   *   via "RLY"            the relay lane's own bearer label.
   *
   * What arrives now is what a person wrote, already opened and already
   * checked, with the core's verdict on the row. */
  const char *body = text;
  const char *disp = text;
  /* The core's verdict on this message, carried on the delivered row. Cleared
   * after every delivery so it can never leak onto the next one. */
  char auth[12] = ""; s_cpy(auth, g_row_sig, sizeof(auth));
  int enc = g_row_sealed;
  g_row_sig[0] = 0; g_row_sealed = 0;

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
  /* Dedup on what was said. The same message heard over two bearers is
   * collapsed by the CORE on its section 5 identifier long before it reaches
   * here; this only catches a repeat the core cannot see as one, such as a
   * station re-broadcasting the same bulletin on a schedule. */
  unsigned h = sig_hash(id, from, body);
  char key[16]; u_itoa(h, key);
  /* Locally hidden message: stays gone even if it arrives again on another
   * transport (the key is the same content signature the host hid it under). */
  if (is_hidden_key(key)) { trc("drop:hidden", from, ""); return 0; }
  /* No distance badge: a bubble carries what was said, not where the sender
   * was standing. Positions are t:observation and belong to whatever draws a
   * map, which is not a chat. */
  const char *meta = "";
  int rep = seen_has(h);
  if (!rep) seen_add(h);
  /* A repeated INCOMING message (direct OR a recurring bulletin) is a duplicate
   * — dual-path delivery (APRS-IS + a BLE iGate), a resend, or a station
   * re-broadcasting the same bulletin on a schedule. Drop it so the chat shows
   * each distinct message once and recurring bulletins don't pile up or get
   * auto-pinned (that banner was just noise). Our own sends are never dropped. */
  if (rep && s_eq(dir, "in")) { trc("drop:dup", from, ""); return 0; }
  convo_msg(id, dir, from, disp, key, meta, via, mid, parent, auth, enc,
            (id[0] != '#') && convo_is_private(id));
  convo_touch(id, enc ? disp : preview, 0);   /* show decrypted text in the list */
  /* One notification per freshly-delivered INCOMING 1:1 message — fired HERE,
   * after multi-line reassembly + decryption, so a long/signed/encrypted DM
   * (which arrives as several APRS lines) alerts once with readable text instead
   * of once per line. The content dedup above means a message arriving over two
   * transports notifies only once. Group bulletins notify via their own caller;
   * our own echoes (dir "out") never notify. */
  if (s_eq(dir, "in") && id[0] != '#') notify_msg(from, from, disp, disp, mid);
  /* The DELIVERED half of 13.7 is the core's: it composed and signed an `s:ack`
   * when it handed this message over, which is also where it decided whether
   * 13.7.1 allows a receipt at all. The READ half is queued here and fires when
   * the user opens the conversation -- see rpend_flush_read. The identifier
   * comes from the delivered row, and only the core-delivered path has one, so
   * a message that reached us any other way simply earns no read receipt. */
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
/* If [text] already carries a media token (and no ih: yet), append our
 * infohash for it. Mutates [text] in place (buffer must have room). */
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

/* Cold-start 1:1: when sending to a callsign whose key we don't know yet, the
 * message goes out as PUBLIC APRS and we ask the relays to resolve callsign→npub
 * (hal_relay_resolve). The text waits here until a resolution arrives (or expires)
 * so we can then place an encrypted backup at the relays. */
#define PSEND_MAX 8
#define RESOLVE_TTL 90                 /* seconds to await a callsign→npub resolve */

/* Case-insensitive callsign compare. */
/* Build a JSON array ["h1","h2",…] from [arr][n]. */
/* Reverse pubkey lookup: the callsign whose stored npub == [npub], or NULL. */

/* Refresh our backup relays from the currently-reachable set (≤RELAY_MAX). */
/* Rendezvous relay set (host-ranked by sha256(relay|pubkey)): the sender
 * publishes to the RECIPIENT's set and the receiver polls its OWN set, so the
 * two meet without the one-shot ?RLY announce (which an offline receiver —
 * the whole point of the relay backup — never hears). */

/* Tell [call] which relays we back up to (once per session) — a control frame
 * "?RLY h1 h2 h3" so the peer knows where to poll for messages from us. */
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
/* Queue a public send awaiting a callsign→npub resolution (ring, evict oldest). */
/* Place an encrypted (NIP-04 kind-4) store-and-forward backup of [text] for
 * [call] at our relays (so the recipient can pick it up later), announce our
 * relays to them, and push a direct encrypted Reticulum copy now that the key is
 * known. Requires pk_get(call). Mirrors do_convo_send's encrypted 1:1 path; the
 * shared rmid lets the receiver dedup the relay + direct copies. */
/* Remove pending-send entry [i] (compacting the ring). */
/* Drain async callsign→npub resolutions (from hal_relay_resolve). For each: store
 * the key + Reticulum dest, then flush any queued public sends to that callsign as
 * encrypted relay backups. Also expires pending sends that were never resolved. */
/* The send pipeline, with the target and text supplied by the caller — the
 * conversations layout parses them from its own field names, the rooms rail
 * from its (a channel on the rail is the same group underneath). [buf] still
 * carries the settings fields (read_config, include_location). */
/* Flip the form the next message goes out in (XPRS.md section 9.2).
 *
 * There is nothing to agree with the peer and nothing to store on their side:
 * each packet says whether its body is sealed, so a conversation can alternate
 * freely and both ends simply read what arrived. */
static void do_convo_form(void) {
  g_want_private = !g_want_private;
  notify("info", g_want_private
                     ? "Messages are private (encrypted)"
                     : "Messages are plain text - anyone in range can read them");
}

static void convo_send_core(const char *buf, const char *id_in,
                            const char *text_in) {
  read_config(buf);
  char id[40] = "", text[400] = "";
  s_cpy(id, id_in, sizeof(id));
  s_cpy(text, text_in, sizeof(text));
  if (!id[0] || !text[0]) return;
  /* The scope rooms speak raw XPRS through the host: one t:message
   * broadcast, scope:local for #LOCAL, signed and aired by hal_xprs_send.
   * Everyone on the bearers -- the ESP32 hotspot page included -- hears it. */
  /* A closed group (26): an ordinary t:message addressed to the GROUP. The
   * group callsign in d: is what makes it a group post; there is no separate
   * "group message" packet, which is what lets any station archive and replay
   * one without understanding groups at all. */
  if (xgroup_is(id)) {
    const char *call = xgroup_call(id);
    if (!xgroup_may_post(call)) {
      notify("warning",
             "You can post here once you accept the invitation");
      return;
    }
    char ts[24];
    xprs_stamp(ts, sizeof(ts), hal_time_epoch());
    char parent[8] = "";
    if (text[0] == '+' && s_len(text) > 8 && text[7] == ' ') {
      int hex = 1;
      for (int i = 1; i <= 6; i++) {
        char c = text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { hex = 0; break; }
      }
      if (hex) {
        for (int i = 0; i < 6; i++) parent[i] = text[1 + i];
        parent[6] = 0;
        unsigned k = 0, n = 8;
        while (text[n]) text[k++] = text[n++];
        text[k] = 0;
        if (!text[0]) return;
      }
    }
    char wire[300] = "t:message f:";
    s_cat(wire, g_call, sizeof(wire));
    s_cat(wire, " d:", sizeof(wire)); s_cat(wire, call, sizeof(wire));
    s_cat(wire, " ts:", sizeof(wire)); s_cat(wire, ts, sizeof(wire));
    if (parent[0]) { s_cat(wire, " r:", sizeof(wire)); s_cat(wire, parent, sizeof(wire)); }
    s_cat(wire, " m:", sizeof(wire)); s_cat(wire, text, sizeof(wire));
    if (s_len(wire) > 250) { notify("warning", "Message too long"); return; }
    if (hal_xprs_send(wire, s_len(wire)) != 0) {
      notify("warning", "Could not send");
      return;
    }
    /* Same reasoning as the scope room: the host signs AFTER us and the
     * section 5 id is computed with sig: removed, so these bytes are the id. */
    char gmid[7];
    xprs_id(wire, s_len(wire), gmid);
    xroom_seen(gmid);   /* our own copy off the air is not a second bubble */
    convo_msg(id, "out", g_call, text, "", "", "XPRS",
              gmid, parent, "verified", 0, 0);
    return;
  }
  if (xroom_is(id)) {
    char ts[24];
    xprs_stamp(ts, sizeof(ts), hal_time_epoch());
    /* The heart button rides the send path as "+like:<mid> <ck>". On the
     * XPRS rooms a vote is a t:reaction (section 6.5) named by the target's
     * section-5 id — never message text (that is what the webchat and the
     * ESP32 page read; the "+like:" text form is chat-internal).
     *
     * This one stays on hal_xprs_send while the message below moves to
     * hal_xprs_broadcast, and the line is where it is on purpose: a reaction
     * is not words. It is add:/remove: naming a target, and a door taking
     * those would either be a second verb for one packet type or a generic
     * "compose me a packet", which is hal_xprs_send with extra steps.
     * hal_xprs_broadcast is for what a person wrote; hal_xprs_send is for a
     * typed act the core has no verb for. */
    {
      char lmid[70]; int unlike; const char *ck;
      if (votemark_parse(text, lmid, &unlike, &ck)) {
        char wire[300] = "t:reaction f:";
        s_cat(wire, g_call, sizeof(wire));
        s_cat(wire, " ts:", sizeof(wire)); s_cat(wire, ts, sizeof(wire));
        /* Unconditional: xroom_is() is #LOCAL and nothing else now. */
        s_cat(wire, " scope:local", sizeof(wire));
        s_cat(wire, unlike ? " remove:like" : " add:like", sizeof(wire));
        s_cat(wire, " r:", sizeof(wire)); s_cat(wire, lmid, sizeof(wire));
        if (hal_xprs_send(wire, s_len(wire)) != 0) {
          notify("warning", "Could not send");
          return;
        }
        convo_react_of(ck, id, lmid, g_call, unlike, 1);
        return;
      }
    }
    /* A reply arrives as "+<mid> text" (the host's thread marker). The mid of
     * an XPRS room bubble is its section-5 id (6 hex); on the wire the target
     * travels as r:, and the text goes out clean. */
    char parent[8] = "";
    if (text[0] == '+' && s_len(text) > 8 && text[7] == ' ') {
      int hex = 1;
      for (int i = 1; i <= 6; i++) {
        char c = text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { hex = 0; break; }
      }
      if (hex) {
        for (int i = 0; i < 6; i++) parent[i] = text[1 + i];
        parent[6] = 0;
        unsigned k = 0, n = 8;
        while (text[n]) text[k++] = text[n++];
        text[k] = 0;
        if (!text[0]) return;
      }
    }
    /* ONE CALL. What stood here composed the packet: it stamped its own ts:,
     * concatenated scope:local and r:, checked the 250-byte ceiling and
     * derived the section 5 identifier itself -- five transport decisions in
     * a chat wapp, and the length check was wrong on top, because the core
     * splits at spaces per 6.6 and a long Local post now travels instead of
     * being refused.
     *
     * The core answers with the identifier it derived, so the bubble, a reply's
     * r: and a reaction all name the same value. */
    char mid[8] = "";
    if (hal_xprs_broadcast(text, s_len(text), "local", 5,
                           parent, s_len(parent),
                           mid, sizeof(mid)) != 2) {
      notify("warning", "Could not send");
      return;
    }
    xroom_seen(mid);   /* our own copy off the air is not a second bubble */
    { char lg[64] = "[chat] local sent id="; s_cat(lg, mid, sizeof(lg));
      hal_log(1, lg, s_len(lg)); }
    convo_msg(id, "out", g_call, text, "", "", "XPRS",
              mid, parent, "verified", 0, 0);
    convo_touch(id, text, 0);
    return;
  }
  /* ── One call, because everything below it used to be transport ──────────
   *
   * What stood here was 210 lines of this wapp deciding how a message travels:
   * it encrypted the body itself (ENC1 + a hand-rolled `rmid`), signed it
   * itself, stamped an `am:` correlation token of its own invention, then
   * chose between Reticulum, a BLE advert, APRS-IS and a NOSTR-relay backup by
   * reading a reach class it maintained -- and told the user which of those had
   * worked.
   *
   * Every one of those is a transport decision and the core makes all of them
   * better: 9.2 sealing with the key the recipient published, 9.1 signing,
   * 6.6 splitting, 36.0 bearer ranking on real evidence, and a custody copy
   * parked for a peer who is not there. What is left for a chat wapp is the
   * words, the recipient, and whether the conversation is private.
   */
  int priv = (id[0] != '#') && convo_is_private(id);

  if (id[0] == '#') {
    /* An open group (6.3). Strip the scope marker: "#NEWS*" and "#NEWS" are
     * one bulletin on the air, and scope is a local view. */
    char gname[8]; int gj = 0;
    for (int i = 1; id[i] && id[i] != '*' && gj < 6; i++) gname[gj++] = id[i];
    gname[gj] = 0;
    if (!gname[0]) return;
    char bid[10]; bid[0] = '#'; s_cpy(bid + 1, gname, sizeof(bid) - 1);
    if (!air_bulletin(bid, text)) {
      notify("warning", "Could not send");
      return;
    }
    convo_deliver(id, "out", g_call, text, text, "");
    status("TX post");
    return;
  }

  /* A 1:1. The return value says what ACTUALLY happened, and the bubble is
   * labelled with that rather than with what was asked for: 36.8 makes
   * plaintext a disclosure, so a message that could not be sealed must never
   * be drawn as private. */
  /* The recipient is a CALLSIGN. An "lxmf:<dest>" row -- the id every 1:1
   * used to be opened under -- names a delivery dest, and handing that to the
   * core's send door composed `d:LXMF:9FE0…`, a packet for nobody, reported
   * as "TX message". Resolve it through the host's directory: a peer that has
   * ever beaconed has a callsign there, and the conversation moves to it. */
  char to[24] = "";
  if (is_callsign_id(id)) {
    s_cpy(to, id, sizeof(to));
  } else if (is_lxmf(id) && lxmf_callsign(id + 5, to, sizeof(to))) {
    /* Same peer, right key: from here on the thread lives under the callsign
     * and the lxmf row is retired, so the rail shows one row for one person.
     * select=1 moves the host's open thread to the new row BEFORE the old one
     * goes -- the user is looking at it, and a thread that vanishes under a
     * message they just typed is a message that vanished. */
    convo_touch(to, text, 1);
    convo_forget(id);
    convo_drop_ghost(id);
    groups_save();
    s_cpy(id, to, sizeof(id));
  } else {
    convo_sysnote(id, "This peer has no callsign yet, so there is nowhere to "
                      "send. It appears once they beacon their identity.");
    notify("warning", "No callsign for this peer");
    return;
  }
  char mid[8] = "";
  int32_t form = hal_xprs_message(to, s_len(to), text, s_len(text),
                                  priv ? 1u : 0u, mid, sizeof(mid));
  if (form == -1) {
    /* The core has just asked for the key (18.1). Refusing is the point: the
     * alternative is sending a private message in the clear and saying nothing.
     */
    convo_sysnote(id, "No key for this contact yet - asked for it. Your "
                      "message was NOT sent; try again in a moment.");
    notify("warning", "No key for this contact yet");
    return;
  }
  if (form <= 0) { notify("warning", "Could not send"); return; }

  /* Stamp the bubble with the section 5 identifier the core keyed its outbox
   * on, so the delivered and read ticks find it (13.7, xprs.status.tx). */
  if (mid[0]) {
    s_cpy(g_send_rid, mid, sizeof(g_send_rid));
    s_cpy(g_send_status, "sent", sizeof(g_send_status));
  }
  convo_deliver(id, "out", g_call, text, text, "");
  g_send_rid[0] = 0; g_send_status[0] = 0;
  status(form == 1 ? "TX (private)" : "TX message");
}

/* THE WIDGET IS CALLED "rooms", SO ITS FIELDS ARE rooms_*.
 *
 * home.ui.json declares one group, {"name":"rooms","$type":"rooms"}, and the
 * host derives every command and field from that name: rooms_send with
 * rooms_convo and rooms_input, rooms_open, rooms_close… This wapp read
 * conversations_convo and conversations_input -- the names of the group it
 * used to declare -- so a message typed on the screen reached the dispatcher
 * as rooms_send, matched nothing, and was dropped without a word. The input
 * cleared, no bubble appeared, and the bench passed because the API test
 * sent conversations_send by hand. The old names stay accepted so nothing
 * driving this wapp from outside has to change. */
static int cmd_field(const char *buf, const char *key, char *out, unsigned m) {
  char k[48] = "rooms_"; s_cat(k, key, sizeof(k));
  if (jstr(buf, k, out, m) && out[0]) return 1;
  s_cpy(k, "conversations_", sizeof(k)); s_cat(k, key, sizeof(k));
  return jstr(buf, k, out, m);
}

static void do_convo_send(const char *buf) {
  char id[40] = "", text[400] = "";
  cmd_field(buf, "convo", id, sizeof(id));
  cmd_field(buf, "input", text, sizeof(text));
  if (!id[0] || !text[0]) return;
  convo_send_core(buf, id, text);
}

/* Toggle private (Reticulum-only) mode for the open 1:1 conversation. Requires the
 * contact's npub (so the off-APRS traffic is encrypted to them). Auto-negotiates
 * by signalling the peer's devices over Reticulum (?PRIV1/?PRIV0) so their side
 * flips too. */
static void do_convo_private(const char *buf) {
  char id[40] = "";
  cmd_field(buf, "convo", id, sizeof(id));
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
/* Send a geo-chat: a position beacon carrying the typed comment. Rides
 * Reticulum (primary) + BLE; also APRS-IS when the legacy opt-in is on. */
/* Post a micro-update to the shared feed group (FEED): a Twitter-style status
 * that everyone following us sees in their Activity tab. Sent as a bulletin
 * (multi-line, optionally signed) over Reticulum + BLE (and legacy APRS-IS when
 * opted in), then echoed into our own Activity feed. */
/* Like / unlike an Activity post (a "<mid>:like" vote to the FEED group). */
/* Reply to an Activity post: a threaded "+<mid> text" to the FEED group. */
/* Prompt to follow a callsign. */
/* Prompt to unfollow: chips of the currently-followed callsigns. */
/* Transmit one recurring bulletin. [echo] shows it once in our own room (only on
 * the first send); the periodic re-broadcasts transmit silently so our view
 * doesn't fill with copies (receivers dedup the repeats). */
/* Begin a recurring bulletin into [group] (re-broadcast every 5 min for
 * [secs], first one now). Reuses the slot for the same group if present. */
/* Stop any recurring bulletin for [group]. */
/* True if [group] (no '#') currently has an active recurring bulletin. */
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
  /* No reachability chips: who is in radio range is a transport fact, and this
   * wapp no longer reads a radio. */
  char chips[700] = ""; int nchips = 0;
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
/* "+"/✎/↻ header actions from the conversations widget. */
static void do_new_chat(void) { prompt_newchat(); }
static void do_add_group(void) { prompt_group(); }
/* Local message actions from the chat bubble menu (host-driven, never on the
 * wire): hide one message, block / unblock a station. */
static void do_convo_hide(const char *buf) {
  char id[40] = "", key[16] = "";
  cmd_field(buf, "convo", id, sizeof(id));
  cmd_field(buf, "hidekey", key, sizeof(key));
  hide_add(id, key);
}
static void do_convo_block(const char *buf) {
  char c[16] = ""; cmd_field(buf, "blockcall", c, sizeof(c));
  if (!c[0]) return;
  block_add(c);
  notify("info", "Blocked — you won't see their messages");
}
/* Block / mute a callsign from the Activity feed's per-post menu. */
/* Close a conversation: unsubscribe so we stop receiving its messages. For a
 * group we forget both the local (#NAME) and global (#NAME*) variants and
 * persist, so the APRS-IS filter drops it and deliver_bulletin no longer
 * delivers it. The host hides the row on its side. */
static void do_convo_close(const char *buf) {
  char id[40] = ""; cmd_field(buf, "convo", id, sizeof(id));
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
  }
}

/* Result of a ui.prompt the host showed for us. */
static void do_prompt_result(const char *buf) {
  char pid[24] = "", val[40] = "", inp[80] = "";
  jstr(buf, "prompt_id", pid, sizeof(pid));
  jstr(buf, "prompt_value", val, sizeof(val));
  jstr(buf, "prompt_input", inp, sizeof(inp));
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
        if (pk_get(id)) { cpriv_set(id, 1); }
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
    }
  } else if (pid[0]=='p'&&pid[1]=='r'&&pid[2]=='o'&&pid[3]=='f'&&pid[4]==':') {
    /* Profile sheet action for pid "prof:<CALL>". */
    const char *call = pid + 5;
    if (s_eq(val, "block")) { block_add(call); notify("info", "Blocked — you won't see their messages"); }
    else if (s_eq(val, "unblock")) { block_remove(call); notify("info", "Unblocked"); }
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
/* Is the local Reticulum node up? Probes hal_rns_delivery_dest (returns 0 while
 * the node is down). Cached for one second so
 * the per-tick status indicator and the send gates don't hammer the HAL. */
/* AIR ONE PACKET. THE CORE PICKS THE PATHS.
 *
 * This used to be `hal_rns_broadcast` on this wapp's own Reticulum datagram
 * tag, with the receiving side draining it straight back into ble_handle. That
 * was a complete XPRS lane living inside a wapp: the core never saw a byte of
 * it, so a message that arrived that way was not deduped against the copy that
 * came by radio, paid no hop budget, appended nothing to `via:`, had no
 * section 5 identity and could not be receipted.
 *
 * hal_xprs_send hands the wire to the core, which signs it when we are its
 * author, ranks the bearers on section 36.0 evidence -- LAN, then BLE5, then
 * Reticulum -- and fans out when it has none. Everything the fan-out below
 * used to do by hand (per-device delivery, staleness, a broadcast backstop for
 * a peer behind NAT) is that ranking, done properly and in one place.
 *
 * Returns 1 when the core accepted the wire. A body with no XPRS form -- a
 * control frame, or text too long for one packet -- returns 0 and goes
 * nowhere: it never had a receiver except this wapp's own drain. */
static int xprs_air_id(const char *to, const char *text, char rid[7]) {
  char wire[900];
  rid[0] = 0;
  if (!xprs_pack(wire, sizeof(wire), g_call, to, text, hal_time_epoch())) return 0;
  /* Section 5: derived from the packet, never transmitted. The core keys its
   * outbox on exactly this, and the receiver derives the same value from the
   * bytes it gets -- including after a 6.6 split, because the identifier is
   * taken over the REASSEMBLED packet. So it is the correlation id the `am:`
   * token used to be, except that both ends compute it instead of one end
   * inventing it and the other echoing it back. */
  xprs_id(wire, s_len(wire), rid);
  return hal_xprs_send(wire, s_len(wire)) == 0 ? 1 : 0;
}

static int xprs_air(const char *to, const char *text) {
  char rid[7];
  return xprs_air_id(to, text, rid);
}

/* Manual/emergency position beacon. One call: the core airs the observation on
 * every bearer it has evidence for, which is what the separate BLE advert and
 * Reticulum broadcast were reaching for one at a time. */
/* Send a 1:1 through the core.
 *
 * What stood here was the wapp doing transport policy: a per-device table with
 * its own staleness TTL, a directed copy to each of the recipient's known
 * Reticulum destinations, and a broadcast backstop for a peer behind NAT --
 * all of it on a datagram tag only this wapp could hear. XprsPublisher already
 * makes exactly these decisions, from section 36.0's evidence rather than from
 * a table this wapp kept, and it makes them for every bearer rather than for
 * Reticulum alone.
 *
 * [wire] is already ENC1-encrypted to the recipient's npub when a key is
 * known, so what travels is unchanged; only who chooses the path is.
 * Returns 1 when the core took it. */
static int rns_tx_msg(const char *to, const char *wire) {
  return xprs_air(to, wire);
}

/* A bulletin, an area post, a recurring broadcast. One call: hal_xprs_send airs
 * it on every bearer the core has evidence for, which is what the paired
 * Reticulum-broadcast-plus-BLE-advert did one transport at a time -- and airing
 * both would now be the same packet twice. The compact BLE frame stays as the
 * fallback for a body with no XPRS form. */
/* Air a bulletin to an open group. One hal_xprs_send; the core picks the
 * bearers. There is no fallback lane because there is no other lane: a body
 * XPRS has no words for is not sent, rather than aired in a private dialect. */
static int air_bulletin(const char *to, const char *text) {
  return xprs_air(to, text);
}

/* PUBLIC 1:1 send. Identical now -- the distinction it used to draw was about
 * whether a plaintext BROADCAST was an acceptable fallback, and there is no
 * broadcast lane here any more. A private conversation still refuses to send
 * without a key; that test lives at the composer, not in the transport. */
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

/* Remember a callsign heard over BLE (not us): refresh its timestamp, add it,
 * or evict the least-recently-seen when full. */
/* Build chips of callsigns heard over BLE within REACH_WINDOW, most-recent
 * first, capped to fit [out]. Each chip is {"label":"CALL","value":"CALL"}.
 * Returns the number written; used by the "New message" prompt to offer the
 * locally-reachable stations. */
/* g/ extra filter: our own call, the most-recently-seen stations (so APRS-IS
 * pushes their direct messages), and the bulletin addressee pattern for every
 * GLOBAL group we subscribe to (id ending in '*') so we hear that group
 * worldwide. Local groups (no '*') need nothing extra — the always-on r/ range
 * filter already brings in-radius bulletins. */
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

static char g_gseen[64][20];      /* event ids already rendered */
static int  g_gseen_n = 0, g_gseen_w = 0;

/* PERSISTENT, and it still has to be.
 *
 * The cursor this was written to compensate for is gone with hal_lxmf_recv --
 * it restarted at 0 on every engine, so opening the wapp's page re-emitted
 * every message ever delivered, five and six times over. That is fixed at the
 * source now.
 *
 * It stays because a section 5 identifier is worth remembering across a
 * restart for its own sake: the same packet reaches this station over more
 * than one bearer and again as a custody re-air, and an in-memory ring dies
 * with the engine that held it. */
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
/* ── Rooms (NIP-72 communities + moderation op-log; see room.c) ──────────── */
/* Ask the user (a parent authority) to approve the newest pending proposal. */
/* The rail = the moderated room tree PLUS this device's broadcast channels
 * (subscribed groups, the NomadNet bridge). The channels were previously in
 * the conversation store but on no screen at all — their unread counted on
 * the launcher badge while nothing in the UI could show, open or clear them,
 * which is exactly how "7 notifications from nowhere" happens. */
static const char *fnd_next_obj(const char *p, char *slice, unsigned m);

/* One rail row's JSON. [people] < 0 means "do not claim a number". */
/* The callsign the host's directory knows for an LXMF delivery dest, or 0.
 * One read, one walk; called on a send, never on a clock. */
static int lxmf_callsign(const char *dest, char *out, unsigned cap) {
  static char dir[8192];
  out[0] = 0;
  int32_t n = hal_people_directory(dest, s_len(dest), dir, sizeof(dir) - 1);
  if (n <= 0) return 0;
  dir[n] = 0;
  char slice[900];
  const char *p = fnd_next_obj(dir, slice, sizeof(slice));
  while (p) {
    char d[70]; jstr(slice, "dest", d, sizeof(d));
    if (s_eq(d, dest)) {
      jstr(slice, "callsign", out, cap);
      return out[0] && xprs_is_station(out);
    }
    p = fnd_next_obj(p, slice, sizeof(slice));
  }
  return 0;
}

/* Is this LXMF peer announcing right now? One directory read per rail render;
 * the host caches the registry, and the rail renders on a slow cadence. */
/* ── Rooms widget commands (Discord-like layout) ── */
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
static void do_rooms_search(void) {
  g_sa_q[0] = 0;
  g_sa_open = 1;
  fnd_field_set("searchall_query", "");
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

/* Open the Members panel for the currently-open room. */
/* A member row tapped: authorities get a moderation prompt; others see the
 * member's reputation level. */
/* Value of the first ["t","<topic>"] tag — which group a note belongs to. */
/* One kind-1 note off the group subscription. */
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
      return;
    }
    p = fnd_next_obj(p, slice, sizeof(slice));
  }
}

/* THE LXMF DRAIN IS GONE, AND SO IS hal_lxmf_recv.
 *
 * It was a cursor over the host's whole LXMF inbox -- every private message on
 * the device, with no recipient test -- and it was a SECOND receive door: the
 * same message reached this wapp both here and through the core's event bus,
 * so this file carried a persistent seen-ring whose only job was deduping the
 * core against itself, because the cursor restarted at zero on every engine.
 *
 * What it delivered that the bus does not is foreign LXMF: plain text from a
 * NomadNet or Sideband peer, which is not an XPRS packet at all. That interop
 * ends here, deliberately. The host refuses it at the inbox door.
 *
 * A message now arrives one way: the core delivers it, once, reassembled and
 * unsealed, with its section 5 identifier. See on_core_event. */

/* Subscribed groups persist in KV "groups" (";"-joined ids) so the APRS-IS
 * filter is correct immediately after a restart, before any row is reopened. */
static void groups_save(void) {
  char buf[1200]; buf[0] = 0;
  for (int i = 0; i < g_convo_n; i++)
    if (convo_renderable(g_convo_ids[i])) {
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
  /* Wide enough for "name (X5ABCD)": a group title carries both, and 24
   * cut the callsign off exactly where it stopped being useful. */
  char title[48]; convo_title(id, title, sizeof(title));
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
  /* One-time: forget #GLOBAL. Dropping convo_ensure(XROOM_GLOBAL) at startup
   * is NOT enough on an existing install -- groups_save() persists every
   * id with a leading '#', so the row is read back here and recreated on
   * every launch. Its own KV flag, because a device that already ran
   * grpclean would otherwise never do this one. */
  int drop_global = 0;
  { char f[2];
    if (hal_kv_get("xglobal", 7, f, sizeof(f) - 1) == 0) drop_global = 1; }
  /* One-time: forget every "lxmf:<dest>" row. A 1:1 is keyed by callsign now
   * (is_callsign_id); the rows keyed by delivery dest were the ones that
   * could not reply, and a device that had talked to a peer across two of
   * that peer's identities showed three rows with one name. Its own flag,
   * for the same reason as xglobal. A peer that still has no callsign gets a
   * fresh row the next time they write. */
  int drop_lxmf = 0;
  { char f[2];
    if (hal_kv_get("xlxmf", 5, f, sizeof(f) - 1) == 0) drop_lxmf = 1; }
  char id[40]; int j = 0;
  for (unsigned i = 0; i <= n; i++) {
    char ch = (i < n) ? buf[i] : ';';
    /* convo_ensure (not convo_remember): re-push each subscribed group so it
     * shows in the Messages list on every page open, not only the g/ filter. */
    if (ch == ';') {
      id[j] = 0;
      if (drop_global && s_eq(id, "#GLOBAL")) {
        char m[160] = "{\"type\":\"ui.convo.remove\",\"id\":\"";
        jesc(m, sizeof(m), id);
        s_cat(m, "\"}", sizeof(m));
        hal_msg_send(m, s_len(m));
        j = 0;
        continue;                      /* not remembered -> dropped on save */
      }
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
      if (is_lxmf(id) && (drop_lxmf || !is_hex32(id + 5))) {
        const char *rm = "{\"type\":\"ui.convo.remove\",\"id\":\"";
        char m[160]; s_cpy(m, rm, sizeof(m));
        jesc(m, sizeof(m), id);
        s_cat(m, "\"}", sizeof(m));
        hal_msg_send(m, s_len(m));
        j = 0;
        continue;
      }
      if (convo_renderable(id)) convo_ensure(id);
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
  if (drop_global) {
    groups_save();          /* persist the list without #GLOBAL, once */
    hal_kv_set("xglobal", 7, "1", 1);
    const char *lg = "[chat] removed the Global chat room";
    hal_log(1, lg, s_len(lg));
  }
  if (drop_lxmf) {
    groups_save();          /* persist the list without lxmf: rows, once */
    hal_kv_set("xlxmf", 5, "1", 1);
    const char *lg = "[chat] removed the dest-keyed 1:1 rows (callsign-keyed now)";
    hal_log(1, lg, s_len(lg));
  }
  /* The NomadNet bridge channel is NOT pre-created any more: an empty row for
   * traffic that may never arrive is exactly the clutter a clean slate is
   * meant to avoid. lxmf_drain creates and persists it the moment a message
   * for it actually lands. */
}

/* iGate (BLE ↔ APRS-IS bridge) on/off persists in KV "igate" ("1"/"0"); absent
 * keeps the on-by-default state. */
/* APRS-IS access persists in KV "aprsis" as "<0|1>|<call>|<passcode>". Absent
 * or malformed keeps the safe default: disabled, no licensed callsign. */
/* Publish the queryable callsign->npub(+RNS dests) identity to the reachable
 * NOSTR relays, hourly: the cold-start 1:1 path (do_convo_send/resolve_drain)
 * resolves peers from exactly this record. This is a relay RECORD, not a chat
 * message -- the old #NOSTR pubkey bulletin that also aired here is gone: the
 * XPRS lane already carries the host's signed t:identity (XPRS.md 9.3), and a
 * machine payload has no business in t:message. */
/* ---- per-callsign mailbox (KV "m.<call>", lines "<from>|<text>") ---- */
/* The "<from>|<text>" body of a stored line is everything after the first '|'
 * (which separates the leading timestamp). Returns NULL if malformed. */
/* Dedup on the body (sender+text), ignoring the per-line timestamp. */
/* Hold a message addressed to a heard station until it pulls its mail. Each line
 * is "<ts>|<from>|<text>" (ts = epoch when held) so a ?MAIL can window by age. */
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
  /* No signature to strip: `sig:` is an envelope field, so it never reaches
   * this wapp inside the body, and the core reports its verdict on the row. */
  const char *cbody = text;
  /* A like vote is silent (no notification): convo_deliver registers it. */
  int is_like; char ltgt[5]; { int u; is_like = like_parse(cbody, ltgt, &u); }
  char par[5]; const char *disp_body; thread_parse(cbody, par, &disp_body);
  (void)par;
  /* The FEED group was the Activity micro-blog's stream. Activity is gone: a
   * chat shows conversations, and a public broadcast feed is a different wapp's
   * job. FEED is an ordinary open group here now. */

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
      notify_msg(gid, from, cbody, preview, 0);
  }
  /* Local: a nearby sender, OR — when no global pull is active (g/BLN* off) —
   * trust the region filter that the bulletin is in-range. */
  if (has_l && (within || !any_global_group())) {
    if (convo_deliver(lid, "in", from, text, preview, via) && !is_like)
      notify_msg(lid, from, cbody, preview, 0);
  }
}

/* A standalone XPRS signature line: "~" + exactly 60 base85 chars. The signed
 * body's word-split puts the signature on its own final line, so this marks the
 * end of a multi-line signed message. */
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
  /* Wide enough for the widest bearer name the host reports ("espnow"), not
   * just the 3-letter routing tokens: a truncated bearer is a chip that names
   * a transport nobody has. */
  int seen; uint64_t t; int within; char via[8];
} ra_t;
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
/* via[8]: same reason as ra_t — a bearer name ("espnow") is longer than the
 * routing tokens this used to hold. */
typedef struct { int used; char from[16]; char via[8]; char part[DA_PARTS][256]; int n; uint64_t t; } da_t;
static void trc(const char *tag, const char *a, const char *b) {
  char t[160]; s_cpy(t, "[trc] ", sizeof(t)); s_cat(t, tag, sizeof(t));
  s_cat(t, " ", sizeof(t)); s_cat(t, a, sizeof(t));
  if (b && b[0]) { s_cat(t, " / ", sizeof(t)); s_cat(t, b, sizeof(t)); }
  hal_log(6, t, s_len(t));
}

/* Buffer one direct-message part, keyed by (from, transport). A message that
 * arrives over BOTH transports (directly from APRS-IS AND re-broadcast by a BLE
 * iGate) is reassembled per-transport and dedups in convo_deliver — shown once. */
/* Decide whether a buffered entry is ready to deliver. A complete single plain
 * message (one short, non-ENC, non-signature part) flushes immediately — no
 * delay. A multi-part message (signed/encrypted, whose parts may arrive across
 * poll cycles via APRS-IS) is held until its trailing signature line arrives;
 * an idle safety net flushes anything stuck after ~2s. */
/* Acknowledge a received line-numbered direct message so the sender's client
 * stops retransmitting it (APRS clients resend the same message ~5x until they
 * receive an ack — that repetition was firing repeated arrivals/notifications).
 * The ack carries NO message number itself:
 *   "<me>>APRS,TCPIP*::<SENDER padded to 9>:ack<msgid>" */
/* Handle one compact frame received over BLE; bridge to APRS-IS when relaying. */
/* ── Ping reach-test helpers ──────────────────────────────────────────── */

/* Per-id / per-(responder,id) dedup so each station answers + forwards a
 * given ping once, and forwards each pong once. */
#define PSEEN_MAX 96
#define RQ_MAX 16
/* Best position: live device GPS (hal_sensor_gps_*) if the host provides it,
 * else the configured station position. Returns 1 when a position is known. */
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
/* Push the Follows people list (Following | Followers sections) to the host's
 * social-style list view. */
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
static int      g_near_n = 0;

static char     g_near_q[40] = "";    /* the list's search box */
static int      g_near_open = 0;      /* screen on screen: keep it refreshed */
static uint64_t g_near_scan_at = 0;
/* Host timestamps arrive in seconds OR milliseconds depending on the source.
 * Normalise to seconds — a millisecond value read as seconds puts the sighting
 * fifty thousand years from now, and every row would read "active". */
/* Merge one sighting into the table. Identity is the callsign when we have it
 * (the human name for a person) and the key otherwise; a row learns its key or
 * callsign later without splitting in two. */
/* Fold rows that turn out to be the same device.
 *
 * A row can be created before we know its callsign (identity-keyed, from the
 * graph) and another after (callsign-keyed, from a beacon or an older build's
 * saved table). Once both are present the duplicate is visible to the user —
 * the same neighbour listed twice, one fresh and one stale — so collapse them
 * here rather than hoping every source arrives in a lucky order. */
/* Anything not heard for a week is not "seen before", it is history: drop it
 * so the list stays a picture of this place rather than an ever-growing log. */
/* Persisted so "seen before" survives a restart — a presence list that forgets
 * everyone on launch can only ever answer the easy half of the question. */
/* Recover a callsign from a display name.
 *
 * A node whose announce carried no nostr pubkey has no meta.callsign — the
 * host can only compose a label, e.g. "X1RD89" or "Nuno (X1RD89)". Without
 * this the same device arrives keyed by identity, fails to match the row we
 * already hold for that callsign, and the list shows one neighbour twice. */
/* First occurrence of [needle] in [hay], or NULL. (No libc here.) */
/* The host already decided which kind of interface this was (one rule, shared
 * with the Reticulum graph — see rns_iface_kind.dart). Here we only pick the
 * chip to show for it. */
/* One sweep of all three sources. Cheap enough for a 5s cadence: two HAL reads
 * and a walk over a table we already keep. */
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
/* Forget everyone who is not within reach right now. The reachable rows are
 * left alone — they are not history, they are the room you are standing in. */
static void do_nearby_open(void) {
  g_near_open = 1;
  g_near_scan_at = hal_time_epoch();
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
  char body[420] = "";
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
  char m[1000] = "{\"type\":\"ui.prompt\",\"id\":\"prof:";
  jesc(m, sizeof(m), up_call);
  s_cat(m, "\",\"title\":\"", sizeof(m)); jesc(m, sizeof(m), up_call);
  if (is_blocked(up_call)) s_cat(body, "\nBlocked — their messages are hidden.", sizeof(body));
  s_cat(m, "\",\"body\":\"", sizeof(m)); jesc(m, sizeof(m), body);
  s_cat(m, "\",\"chips\":[", sizeof(m));
  if (is_blocked(up_call))
    s_cat(m, "{\"label\":\"Unblock\",\"value\":\"unblock\"}", sizeof(m));
  else
    s_cat(m, "{\"label\":\"Block\",\"value\":\"block\"}", sizeof(m));
  s_cat(m, "]}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* Edit the tags on a followed callsign (result handled as "ftag:<call>"). */
/* Extract the idx-th comma-separated field of s into out (NUL-terminated). */
/* RSSI -> rough distance (metres) via log-distance path loss:
 *   d = 10^((TXREF - rssi)/(10*N)),  TXREF ~ RSSI at 1 m, N ~ path-loss exp.
 * Coarse, but close enough for a direct hop. -1 when rssi is unknown. */
/* Straight-line distance from our position to lat/lon in metres, or -1 when
 * our own position is unknown. (Metres twin of distance_to.) */
/* Format metres as "<n> m" (<1 km) or "<n> km". */
/* Per-ping responder results, so we can keep the best route per responder and
 * re-render the list as replies arrive. by_pos = distance came from a real
 * position (accurate); else it's an RF (RSSI) estimate. */
#define PRES_MAX 32
/* Inbound ping: answer once with our callsign + position, then forward it on
 * (ttl) so it reaches further stations. text = "id,ttl,hops". */
/* Inbound pong: if it answers our active ping, record it (best route) + drop a
 * map marker; forward it back across the mesh, accumulating an RF distance.
 * text = "id,hops,lat,lon,pttl,dM". [rssi] = strength we received it at.
 *
 * Distance estimate per responder:
 *  - if the reply carries a position AND we know ours -> exact (by_pos);
 *  - else RF: dM (sum of prior hops' RSSI distances) + this hop's RSSI distance.
 * For multi-hop, several routes may arrive; we keep the smallest (best). */
/* Tools "Send ping": broadcast a fresh ping and start collecting replies. */
/* via = the transport this frame actually arrived on ("BLE" for Bluetooth, "RET"
 * for a Reticulum datagram over the internet). The RNS path reuses the BLE frame
 * FORMAT but must NOT be mislabelled as Bluetooth, so the caller passes the real
 * transport and we tag every delivered copy with it. */
/* Undirected text -- the Live tab's area chat. Shown, never notified. Same two
 * lanes as render_position: the compact frame, and a `t:message` with no `d:`. */
/* A position sighting, from whichever lane carried it: the compact `!` frame
 * an ESP32 still airs, or a `t:observation` the core hands us. [text] is the
 * compact body in both cases -- "lat,lon[,comment]" -- because that is what
 * the map, the geo-chat feed and the follow capture already read. */
/* Reconcile the BLE transport with the g_ble_on setting (start/stop scan). */
/* ── module entry points ────────────────────────────────────────────── */
/* ── What the core routes to us (XPRS.md section 4.2 types) ──────────────
 *
 * The core receives every packet, decides which types this wapp registered
 * for, and calls us with them. We do not read a radio, drain a shared inbox
 * or poll a socket to find them -- and because delivery is a call, a message
 * is on screen as soon as the core has it rather than on the next tick.
 *
 * The row carries the packet and its provenance: who sent it, the section 5
 * identifier for dedup, the `via:` relay chain, the bearer it was heard on
 * and the signal where there was one. */
/* A packet the core delivered as HEARD: the wire's own fields, its section 5
 * identifier, and where it came from. This is the shape for traffic addressed
 * to nobody in particular -- a group post, a position -- which is aired (6.3)
 * rather than couriered, so it never passes through the 1:1 delivery below.
 *
 * A finished 1:1 does NOT render from here. It arrives on the same topic in
 * the other shape, with `content`, only after the core has reassembled its
 * parts and opened its seal -- which is the whole reason that shape exists. */
static void on_core_packet(const char *topic, const char *row) {
  char from[24] = "", to[40] = "", id[24] = "", via[12] = "", bearer[12] = "";
  jstr(row, "from", from, sizeof(from));
  jstr(row, "to", to, sizeof(to));
  jstr(row, "id", id, sizeof(id));
  jstr(row, "bearer", bearer, sizeof(bearer));
  if (!from[0] || is_self_call(from)) return;
  /* A part is not a message (6.6) and a sealed body is not readable: both are
   * the core's to finish, and it re-delivers the result. */
  { char n[8]; if (jfield(row, "n", n, sizeof(n))) return; }
  if (jbool(row, "sealed")) return;
  if (id[0]) {
    if (gseen_has(id)) return;
    gseen_add(id);
  }
  /* The bearer in the reader's vocabulary. A packet that crossed the internet
   * says so; anything else names the radio it was heard on. */
  s_cpy(via, s_eq(bearer, "rns") ? "RET" : (bearer[0] ? bearer : "XPRS"),
        sizeof(via));

  if (s_eq(topic, "xprs.observation")) {
    /* pos: is the coordinate pair, m: the human part -- the compact `!` body
     * is the two joined by a comma, which is what render_position reads. */
    char pos[64] = "", m[160] = "", body[240];
    if (!jfield(row, "pos", pos, sizeof(pos))) return;
    jfield(row, "m", m, sizeof(m));
    s_cpy(body, pos, sizeof(body));
    if (m[0]) { s_cat(body, ",", sizeof(body)); s_cat(body, m, sizeof(body)); }
    return;
  }

  if (s_eq(topic, "xprs.reaction")) {
    /* A vote on a Local-room bubble (6.5), named by its section 5 identifier.
     * This wapp has subscribed to the topic since the rooms were written and
     * dropped every frame on the line below, so a heart from another station
     * -- or from the ESP32's own chat page -- lit nothing, while our own like
     * rendered locally and looked like it had worked. */
    char scope[16] = "", r[16] = "", add[16] = "", rem[16] = "";
    jstr(row, "scope", scope, sizeof(scope));
    jfield(row, "r", r, sizeof(r));
    jfield(row, "add", add, sizeof(add));
    jfield(row, "remove", rem, sizeof(rem));
    /* Say what was done with it, every time: a heart that lit nothing and a
     * heart that never arrived look the same, and the host tallies by the
     * target id, so the id is the one thing worth writing down. */
    { char lg[160] = "[chat] reaction from=";
      s_cat(lg, from, sizeof(lg));
      s_cat(lg, " scope=", sizeof(lg)); s_cat(lg, scope[0] ? scope : "-", sizeof(lg));
      s_cat(lg, " to=", sizeof(lg)); s_cat(lg, to[0] ? to : "-", sizeof(lg));
      s_cat(lg, " r=", sizeof(lg)); s_cat(lg, r[0] ? r : "-", sizeof(lg));
      s_cat(lg, add[0] ? " add=" : " remove=", sizeof(lg));
      s_cat(lg, add[0] ? add : rem, sizeof(lg));
      hal_log(1, lg, s_len(lg)); }
    if (to[0] || !s_eq(scope, "local")) return;
    if (!r[0] || !(s_eq(add, "like") || s_eq(rem, "like"))) return;
    if (is_blocked(from) || is_muted(from)) return;
    /* mine=0: is_self_call(from) returned above, so this is never our own. */
    convo_react(XROOM_LOCAL, r, from, rem[0] ? 1 : 0, 0);
    return;
  }

  if (!s_eq(topic, "xprs.message")) return;
  char m[900] = "";
  if (!jfield(row, "m", m, sizeof(m)) || !m[0]) return;
  /* Addressed to a station -- including a closed group, which is a station by
   * 6.3's naming rule -- is correspondence or group business, and neither
   * renders from a packet as heard. What is left is the aired kinds. */
  if (xprs_is_station(to)) return;
  if (!to[0]) {
    /* UNDIRECTED TRAFFIC IS THE LOCAL ROOM'S, OR NOBODY'S (13.11.1).
     *
     * This was the line every scope:local packet died on. The room could send
     * -- and did, in the same wire the ESP32 chat page speaks -- and never
     * listened, so it was write-only and looked deleted. The backfill that
     * used to cover for that was a hal_xprs_history poll, removed with the
     * polling sweep; nothing replaced it, and nothing needs to: the packet is
     * already here, on the bus, and this is where it belongs.
     *
     * Unscoped undirected traffic is still dropped. #GLOBAL was removed for a
     * reason that has not changed (see the scope-room comment above): with no
     * `d:` there is no custody, no ack and no retry, so a message reaches one
     * phone and not another with nothing able to repair it. `local` is the
     * room where that is honest, because everyone in it is in earshot. */
    char scope[16] = "";
    jstr(row, "scope", scope, sizeof(scope));
    /* SAY WHY IT STAYED QUIET.
     *
     * An undirected packet that reaches this wapp and produces no bubble has
     * four different reasons, and from outside they are one symptom: an empty
     * room. Finding out which one cost a bench session, so the wapp now says.
     * Only on a DROP -- a message that renders is its own evidence, and a line
     * per packet on a busy channel is noise. */
    #define LOCAL_DROP(why) do { \
        char lg[128] = "[chat] local dropped: " why " from="; \
        s_cat(lg, from, sizeof(lg)); \
        s_cat(lg, " scope=", sizeof(lg)); \
        s_cat(lg, scope[0] ? scope : "-", sizeof(lg)); \
        s_cat(lg, " id=", sizeof(lg)); s_cat(lg, id[0] ? id : "-", sizeof(lg)); \
        hal_log(1, lg, s_len(lg)); return; } while (0)
    if (!s_eq(scope, "local")) LOCAL_DROP("not scope:local");
    if (is_blocked(from) || is_muted(from)) LOCAL_DROP("blocked or muted");
    /* Our own copy off the air is not a new bubble. Two guards, and they catch
     * different things: is_self_call (above) keys on the callsign, this keys on
     * the section 5 identifier -- which is also what makes a packet heard on
     * Bluetooth AND on the LAN one message rather than two. */
    if (xroom_seen(id)) return;   /* a second copy is not a second message */
    char parent[8] = ""; jfield(row, "r", parent, sizeof(parent));
    char sigv[12] = ""; jstr(row, "sig", sigv, sizeof(sigv));
    unsigned h = sig_hash(XROOM_LOCAL, from, m);
    char key[16]; u_itoa(h, key);
    if (is_hidden_key(key)) LOCAL_DROP("hidden by the user");
    #undef LOCAL_DROP
    convo_msg(XROOM_LOCAL, "in", from, m, key, "", via,
              id, parent, s_eq(sigv, "verified") ? "verified" : "", 0, 0);
    convo_touch(XROOM_LOCAL, m, 0);
    char preview[160]; s_cpy(preview, from, sizeof(preview));
    s_cat(preview, ": ", sizeof(preview)); s_cat(preview, m, sizeof(preview));
    notify_msg(XROOM_LOCAL, from, m, preview, id);
    return;
  }
  deliver_bulletin(to, from, m, s_eq(via, "RET") ? 0 : 1, via);
}

static void on_core_event(const char *topic, const char *row) {
  if (!topic[0] || !row[0]) return;

  /* The outbound side of 13.7: what the core learned about a message WE sent.
   * The tick used to be asserted by this wapp off its own `?ACK` dialect; it
   * is now reported by the core, off a signed t:receipt, keyed on the same
   * section 5 identifier the bubble carries. */
  if (s_eq(topic, "xprs.status.tx")) {
    char sid[24] = "", state[16] = "";
    jstr(row, "id", sid, sizeof(sid));
    jstr(row, "state", state, sizeof(state));
    if (sid[0] && state[0]) convo_status_emit(sid, state);
    return;
  }

  char from[24] = "", content[900] = "", title[40] = "", id[24] = "", call[24] = "";
  jstr(row, "from", from, sizeof(from));
  jstr(row, "title", title, sizeof(title));
  jstr(row, "id", id, sizeof(id));
  jstr(row, "call", call, sizeof(call));
  /* Two shapes reach us on xprs.message. `content` marks the finished one --
   * a 1:1 the core reassembled and unsealed. Without it this is a packet as
   * heard, and only the aired kinds (a group post, an observation) render from
   * that; the rest is on_core_packet's business. */
  if (!jstr(row, "content", content, sizeof(content)) || !content[0]) {
    on_core_packet(topic, row);
    return;
  }
  if (!from[0]) return;
  if (is_self_call(from)) return;

  /* The section 5 identifier is derived from the packet, so the same message
   * heard over two bearers carries the same one and collapses here. */
  if (id[0]) {
    if (gseen_has(id)) return;
    gseen_add(id);
  } else {
    /* No packet behind it: key on the content instead. */
    char dk[8];
    msg_id(from, content, dk);
    if (gseen_has(dk)) return;
    gseen_add(dk);
  }

  /* Control traffic between two chat instances is not correspondence and must
   * never become a bubble. These used to be consumed in ble_handle, on the way
   * up from the radio; a message reaching us through the core arrives here
   * instead, so the same tests belong here. They key on the CALLSIGN, which is
   * why the core puts it on the row -- `from` is a delivery destination. */
  if (call[0]) {
    if (priv_intercept(call, content)) return;
    if (rly_intercept(call, content)) return;
  }
  if (s_pre(content, "?ACK ")) return;   /* a retired dialect, never a bubble */

  /* A group post is addressed to the group; anything else is correspondence
   * with the sender. */
  char cid[72];
  if (title[0] == '#') {
    group_convo_id(title + 1, cid, sizeof(cid));
    if (!g_chan_nomad || !chan_enabled(cid)) return;
  } else if (call[0]) {
    /* A station: the conversation is the callsign, the same value the core
     * takes as a recipient. If an older build opened an "lxmf:<dest>" row for
     * this same peer, it is retired here rather than left beside the real one
     * -- two rows with one title, one of which could not reply, is what this
     * replaces. */
    s_cpy(cid, call, sizeof(cid));
    char old[72] = "lxmf:"; s_cat(old, from, sizeof(old));
    if (convo_known(old)) {
      convo_forget(old);
      convo_drop_ghost(old);
      groups_save();
    }
  } else {
    /* No callsign: a NomadNet peer, addressable only by its delivery dest. */
    s_cpy(cid, "lxmf:", sizeof(cid));
    s_cat(cid, from, sizeof(cid));
  }
  if (!convo_known(cid)) {
    if (title[0] != '#' && !call[0]) lxname_resolve(from);
    convo_ensure(cid);
    groups_save();
  }

  /* The sender's name: the callsign the core resolved, then whatever the host
   * knows for the delivery dest, then the dest itself. */
  const char *who = from;
  const char *nm = (title[0] == '#') ? 0 : lxname_get(from);
  if (nm && nm[0]) who = nm;
  if (call[0]) who = call;

  /* The core's verdict travels with the message (9.1 for a signature, 9.2 for
   * a seal that opened). This wapp does not check either: it was doing both,
   * with schemes of its own, beside the ones the core had already run. */
  jstr(row, "sig", g_row_sig, sizeof(g_row_sig));
  g_row_sealed = jbool(row, "sealed");

  char mid[5];
  msg_id(from, content, mid);
  convo_msg(cid, "in", who, content, "", "",
            "XPRS", mid, "", s_eq(g_row_sig, "verified") ? "verified" : "", 0, 0);
  g_row_sig[0] = 0; g_row_sealed = 0;
  convo_touch(cid, content, 0);
  notify_msg(cid, who, content, content, mid);
  /* Queue the READ half of 13.7. It fires when the user opens this thread --
   * the one fact about this message the core cannot observe for itself. */
  if (title[0] != '#') rpend_add(cid, id);
}

/* Re-read the closed groups we belong to (26) and publish a conversation for
 * each one we may speak in.
 *
 * Membership is the CORE's, not this wapp's: it replays signed acts against a
 * key map the host owns, and every wapp can ask. This runs when the core says
 * it moved -- XprsGroups' own change stream -- and never on a clock. It used
 * to be a 4 KB read and a hand-written JSON walk every thirty seconds, whose
 * normal answer is "the same as last time".
 *
 * Being INVITED is not membership (26.3.1), so a group we have not joined gets
 * no conversation: showing one would present an invitation as a fait accompli.
 * Accepting is a signed act by the person, in Settings -> Groups. */
static void xgroups_refresh(void) {
  static char gb[4096];
  int n = hal_xprs_groups(gb, sizeof(gb) - 1);
  if (n <= 0 || n >= (int)sizeof(gb)) return;
  gb[n] = 0;
  g_xgroup_n = 0;
  int depth = 0, instr = 0, esc = 0, start = -1;
  for (int i = 0; gb[i]; i++) {
    char ch = gb[i];
    if (esc) { esc = 0; continue; }
    if (ch == '\\') { esc = 1; continue; }
    if (ch == '"') { instr = !instr; continue; }
    if (instr) continue;
    if (ch == '{') { if (depth == 0) start = i; depth++; }
    else if (ch == '}') {
      depth--;
      if (depth == 0 && start >= 0) {
        char save = gb[i + 1];
        gb[i + 1] = 0;
        const char *obj = gb + start;
        if (g_xgroup_n < XGROUP_MAX) {
          int k = g_xgroup_n;
          g_xgroup[k].call[0] = g_xgroup[k].nick[0] = 0;
          g_xgroup[k].role[0] = 0;
          jstr(obj, "call", g_xgroup[k].call, sizeof(g_xgroup[k].call));
          jstr(obj, "nick", g_xgroup[k].nick, sizeof(g_xgroup[k].nick));
          jstr(obj, "role", g_xgroup[k].role, sizeof(g_xgroup[k].role));
          if (g_xgroup[k].call[0]) g_xgroup_n++;
        }
        gb[i + 1] = save;
        start = -1;
      }
    }
  }
  xgroups_publish();
}

/* Drain what the core handed us. Called at the top of module_handle_event,
 * which is what the core invokes on delivery. */
static void drain_core_events(void) {
  static char row[3200];
  char topic[64];
  for (int guard = 0; guard < 32; guard++) {
    if (hal_event_available() == 0) break;
    uint32_t n = hal_event_recv(topic, sizeof(topic) - 1, row, sizeof(row) - 1);
    if (n == 0) break;
    row[n] = 0;
    /* Group membership is a core feature every wapp can use, so it arrives
     * like any other core state rather than being polled for. */
    if (s_eq(topic, "core.groups")) { xgroups_refresh(); continue; }
    on_core_event(topic, row);
  }
}

void module_init(void) {
  hal_log(1, "[aprs] init", 11);
  /* Register for the packet types this wapp is about. The core routes by
   * `t:` (XPRS.md 4.2), so a type we do not name is never handed to us and
   * never costs us anything. */
  {
    static const char *topics[] = {
      "xprs.message", "xprs.reaction",
      /* Not `xprs.receipt`: a receipt heard on the air is the core's, and it
       * reports the outcome on `xprs.status.tx` rather than handing us a
       * packet to interpret. Not `xprs.observation` either: a position is not
       * a conversation, and this wapp no longer draws a map. */
      "xprs.status.tx",
      /* Closed-group membership (26), which the core owns and every wapp can
       * ask about. */
      "core.groups",
    };
    for (unsigned i = 0; i < sizeof(topics) / sizeof(topics[0]); i++)
      hal_event_subscribe(topics[i], s_len(topics[i]));
  }
  xgroups_refresh();
  /* Default callsign = THIS device's profile callsign (so each device
   * transmits as itself, not a hardcoded one). The user's Settings callsign,
   * if set, overrides this via read_config. */
  char id[16];
  uint32_t n = hal_identity(id, sizeof(id) - 1);
  if (n > 0 && n < sizeof(id)) {
    id[n] = 0;
    if (id[0]) { s_cpy(g_call, id, sizeof(g_call)); s_cpy(g_idcall, id, sizeof(g_idcall)); }
  }
  gseen_load();    /* before groups_load: the LXMF cursor restarts at 0 per engine */
  chan_load();     /* before groups_load: the rail render honours the switches */
  lxname_load();   /* before groups_load: lxmf rows render with their names */
  recent_load();   /* …and in the order you last used them, not insertion order */
  groups_load();   /* restore subscribed groups so the g/ filter is correct now */
  xgroups_restore(); /* closed-group rooms from cache, before asking the host */
  /* Cache our public key (base64url) and the persisted pubkey-beacon pref. */
  { uint32_t pn = hal_identity_pubkey(g_pubkey, sizeof(g_pubkey) - 1);
    if (pn < sizeof(g_pubkey)) g_pubkey[pn] = 0; else g_pubkey[0] = 0; }
  blockhide_load(); /* restore local block list + hidden-message keys */
  pk_load();       /* restore known callsign -> pubkey map (for verification) */
  rns_dest_load(); /* restore npub -> {RNS delivery dests} (Reticulum addressing) */
  cpriv_load();    /* restore which 1:1 conversations are private (Reticulum-only) */
  convo_ensure(XROOM_LOCAL);   /* the scope room exists before the first word */
  /* NO ghost sweep of callsign rows. There used to be one here, on the theory
   * that a bare callsign "is, by definition, not something this wapp can
   * render" -- and it deleted, at every start, exactly the row a 1:1 now lives
   * in. */
  pk_render();     /* populate the Keys list view from the restored database */
  /* Bridge restored callsign->pubkey to the host so the Activity feed/profile
   * show npubs immediately, not only after the next live beacon. */
  for (int i = 0; i < g_pk_n; i++) host_identity_emit(g_pk_call[i], g_pk_key[i]);
  /* Bridge restored follow/block state to the host so the profile UI is correct
   * from the first open. */
  for (int i = 0; i < g_blocked_n; i++) host_state_emit("block", g_blocked[i], 1);
  /* Draw the rail once everything is restored. convo_remember redraws on each
   * addition above, but the ghost sweep and the group refresh both prune after
   * that, and the last word has to be the true one. */
  render_rail();
}

/* Legacy APRS-IS housekeeping: auto-reconnect, login, drop detection, inbound
 * drain and the timed APRS auto-beacon. Extracted from module_tick so its
 * early returns only skip APRS work — with the APRS-IS switch off (the
 * default), the Reticulum pull / relay polling in module_tick still runs. */
/* Backfill the group and #LOCAL rooms from the host's archive.
 *
 * Called when a page opens, and when the core says the archive grew — never on
 * a clock. This ran every four seconds: a 48-row query, in a pocket, for a room
 * nobody was looking at, whose answer on a quiet radio is the same 48 rows.
 *
 * Live traffic does not come through here at all; it arrives on the event bus
 * as it happens. This is only what was said before the page was opened. */
/* Re-read what the core says we belong to (26), and publish the rooms for the
 * groups we may actually speak in.
 *
 * Membership changes when a signed act arrives and at no other time — which is
 * what XprsGroups' own change stream says — so this is called from
 * `core.groups`, not from a timer. It used to be a 4 KB read and a hand-written
 * JSON walk every thirty seconds to learn that nothing had moved. */
/* NO CLOCK. NOTHING PERIODIC. NOTHING TO POLL.
 *
 * This function was 452 lines and it was almost entirely transport: a BLE scan
 * drained twenty frames at a time, an APRS-IS socket, two NOSTR subscriptions,
 * a Reticulum propagation pull, a digipeat re-air queue, presence beacons, a
 * mailbox query, a reach-test window — plus three polls asking the host whether
 * anything it already knew had changed.
 *
 * A chat has no business owning any of it. What is left of chatting is
 * event-driven by nature: somebody says something, the core hands it over, and
 * a person types a reply. Both arrive as calls.
 */
void module_tick(void) {
}

void module_handle_event(void) {
  char buf[4096];
  /* What the core routed to us, first: this function is what it calls on
   * delivery, and a host command is not a precondition for a packet. */
  drain_core_events();
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
  /* The rooms widget's own names first (see cmd_field), the old ones after. */
  else if (s_eq(cmd, "rooms_send") || s_eq(cmd, "conversations_send")) do_convo_send(buf);
  else if (s_eq(cmd, "rooms_open") || s_eq(cmd, "conversations_open")) do_convo_open(buf);
  else if (s_eq(cmd, "rooms_private") || s_eq(cmd, "conversations_private")) do_convo_private(buf);
  else if (s_eq(cmd, "rooms_form") || s_eq(cmd, "conversations_form")) do_convo_form();
  else if (s_eq(cmd, "rooms_hide") || s_eq(cmd, "conversations_hide")) do_convo_hide(buf);
  else if (s_eq(cmd, "rooms_block") || s_eq(cmd, "conversations_block")) do_convo_block(buf);
  else if (s_eq(cmd, "rooms_close") || s_eq(cmd, "conversations_close")) do_convo_close(buf);
  else if (s_eq(cmd, "new_chat")) do_new_chat();
  else if (s_eq(cmd, "add_group")) do_add_group();
  else if (s_eq(cmd, "rooms_newchat")) do_rooms_newchat();
  else if (s_eq(cmd, "rooms_search")) do_rooms_search();
  else if (s_eq(cmd, "searchall_search")) {
    jstr(buf, "searchall_query", g_sa_q, sizeof(g_sa_q));
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
    }
  }
  else if (s_eq(cmd, "nearby_open")) do_nearby_open();
  else if (s_eq(cmd, "nearby_search")) {
    jstr(buf, "nearby_query", g_near_q, sizeof(g_near_q));
  }
  else if (s_eq(cmd, "nearby_tap")) do_nearby_tap(buf);
  else if (s_eq(cmd, "rooms_settings")) {
    const char *m = "{\"type\":\"ui.screen.open\",\"name\":\"Settings\"}";
    hal_msg_send(m, s_len(m));
  }
  else if (s_eq(cmd, "prompt")) do_prompt_result(buf);
  else if (s_eq(cmd, "profile")) {            /* sender name tapped in a chat */
    char c[16] = ""; jstr(buf, "profile_call", c, sizeof(c));
    profile_show(c);
  }
  /* Block actions from the host profile UI panel (a callsign in
   * "profile_target"). Blocking is conversation hygiene; following was a feed. */
  else if (s_eq(cmd, "profile_block")) {
    char c[16] = ""; jstr(buf, "profile_target", c, sizeof(c));
    if (c[0]) { block_add(c); notify("info", "Blocked — you won't see their messages"); }
  } else if (s_eq(cmd, "profile_unblock")) {
    char c[16] = ""; jstr(buf, "profile_target", c, sizeof(c));
    if (c[0]) { block_remove(c); notify("info", "Unblocked"); }
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
    status("Channel switches applied");
    notify("info", "Channels updated");
  }
  else if (s_eq(cmd, "keys_refresh")) pk_render();
  else if (s_eq(cmd, "sign_apply")) {
    g_sign_msgs = jbool_def(buf, "sign_msgs", 0);
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
  /* Nothing to close. This wapp holds no socket, no radio and no subscription:
   * every one of those belonged to a transport it no longer owns. */
}

/* 0 = no clock. Everything this wapp does starts with something happening to
 * it: a command from its page, or a packet from the core. */
int32_t module_tick_interval_ms(void) { return 0; }
