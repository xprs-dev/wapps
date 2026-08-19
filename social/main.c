/*
 * social — a social-feed wapp on the NOSTR protocol, laid out like the Chat wapp.
 *
 *   Activity  ($type:"chat")          kind-1 notes from the accounts you follow
 *   Messages  ($type:"conversations") kind-4 encrypted DMs, per-peer threads
 *   Following ($type:"people")        who you follow (+ add / explicit unfollow)
 *   Relay servers (menu panel)        relay list + reachability + add / remove
 *
 * All relay/crypto/signing/decryption is host-side via hal.nostr; the transport
 * of each relay (wss:// internet, rns:// Reticulum, local device) is invisible
 * here. This module just drives the UI.
 *
 * Build: cd wapps/social && WASI_SDK_PATH=~/wasi-sdk make
 */
#include "../hal/xprs_wasm_hal.h"

/* ── String helpers ──────────────────────────────────────────────────── */
static unsigned str_len(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static int str_eq(const char *a, const char *b) { while (*a && *b && *a == *b) { a++; b++; } return *a == *b; }
static void str_copy(char *d, const char *s, unsigned m) { unsigned i = 0; while (i < m - 1 && s[i]) { d[i] = s[i]; i++; } d[i] = '\0'; }
static void str_cat(char *d, const char *s, unsigned m) { unsigned l = str_len(d); unsigned i = 0; while (l + i < m - 1 && s[i]) { d[l + i] = s[i]; i++; } d[l + i] = '\0'; }

/* Find "key":<value> in flat JSON; copy the raw value. Escape-aware inside
 * strings so a note with a quote is not truncated. */
static int json_raw(const char *json, const char *key, char *out, unsigned m) {
    char pat[48];
    str_copy(pat, "\"", sizeof(pat));
    str_cat(pat, key, sizeof(pat));
    str_cat(pat, "\":", sizeof(pat));
    unsigned pl = str_len(pat);
    for (const char *p = json; *p; p++) {
        unsigned i = 0;
        while (i < pl && p[i] == pat[i]) i++;
        if (i != pl) continue;
        p += pl;
        unsigned o = 0;
        int instr = 0;
        if (*p == '"') { instr = 1; p++; }
        while (*p && o < m - 1) {
            if (instr) {
                if (*p == '\\' && p[1]) { out[o++] = *p++; if (o < m - 1) out[o++] = *p++; continue; }
                if (*p == '"') break;
            } else if (*p == ',' || *p == '}' || *p == ']') break;
            out[o++] = *p++;
        }
        out[o] = '\0';
        return 1;
    }
    return 0;
}


/* Append raw string as JSON string body (escaped). */
static void json_escape_cat(char *dst, const char *s, unsigned m) {
    unsigned l = str_len(dst);
    for (unsigned i = 0; s[i] && l < m - 2; i++) {
        char c = s[i];
        if (c == '"' || c == '\\') { dst[l++] = '\\'; dst[l++] = c; }
        else if (c == '\n') { dst[l++] = '\\'; dst[l++] = 'n'; }
        else if (c == '\r') { dst[l++] = '\\'; dst[l++] = 'r'; }
        else if (c == '\t') { dst[l++] = '\\'; dst[l++] = 't'; }
        else if ((unsigned char)c < 0x20) { continue; }
        else dst[l++] = c;
    }
    dst[l] = '\0';
}

static void send_msg(const char *json) { hal_msg_send(json, str_len(json)); }
static void short12(const char *hex, char *out) { str_copy(out, hex, 13); }

static void u64_str(unsigned long long v, char *out) {
    char tmp[24]; int n = 0;
    if (v == 0) { out[0] = '0'; out[1] = '\0'; return; }
    while (v > 0 && n < 23) { tmp[n++] = '0' + (int)(v % 10); v /= 10; }
    int i = 0; while (n > 0) out[i++] = tmp[--n]; out[i] = '\0';
}

/* "HH:MM" (UTC) from a unix-seconds string. */
static void fmt_hhmm(const char *unix_s, char *out) {
    long v = 0;
    for (const char *p = unix_s; *p >= '0' && *p <= '9'; p++) v = v * 10 + (*p - '0');
    int hh = (int)((v / 3600) % 24), mm = (int)((v / 60) % 60);
    out[0] = '0' + hh / 10; out[1] = '0' + hh % 10; out[2] = ':';
    out[3] = '0' + mm / 10; out[4] = '0' + mm % 10; out[5] = '\0';
}

/* Append the display-time fields the host feed wants: "time" (HH:MM clock) +
 * "t" (absolute epoch, MILLISECONDS) so older posts get a date prefix. */
static void cat_time_fields(char *dst, const char *ts, unsigned m) {
    str_cat(dst, "\"time\":\"", m);
    if (ts[0]) { char hm[8]; fmt_hhmm(ts, hm); str_cat(dst, hm, m); }
    str_cat(dst, "\",\"t\":", m);
    str_cat(dst, ts[0] ? ts : "0", m);
    str_cat(dst, "000", m); /* seconds → ms */
}

/* ── State ───────────────────────────────────────────────────────────── */
static char g_self[80] = "";       /* our x-only pubkey (hex)              */
static char g_sub_disc[64] = "";   /* popular (>2-like) sub — ranking, NOT "All" */
static char g_sub_fire[64] = "";   /* LIVE firehose (spam-gated) — the "All" tab */
static char g_sub_follows[64] = ""; /* kind-1 from direct follows — "Following" */
static char g_sub_search[64] = ""; /* NIP-50 search sub (posts + profiles)  */
static int  g_search_media = 0;    /* "Only posts with media" filter is on   */
static char g_query[128] = "";     /* what the user typed — results are checked
                                    * against it here, because a relay that does
                                    * not speak NIP-50 happily ignores "search"
                                    * and streams us its whole feed instead.    */
static char g_sub_post[64] = "";   /* one publication + its replies — host
                                    * deep-link (launcher hero card tap)   */
static char g_evt[65536];          /* one drained event JSON (see below)   */
/* 8192 was too small: the host pops an event off the queue, finds it does not
 * fit, and drops it — silently and permanently. A NOSTR note with tags, an imeta
 * and a signature routinely runs past 8 KB, so most of the firehose died right
 * here while the gate reported it as "kept". */
static char g_relays[8192];        /* hal_nostr_relays output              */
static char g_msg[16384];          /* outbound UI message                  */
static char g_follows[4096];       /* followed pubkeys JSON array          */
static char g_direct[48128];       /* direct-follow author set JSON        */
static char g_feedfilter[52224];   /* built kind-1 direct-follow filter    */
static char g_pids[96][66];        /* recent post ids (ring) for stats     */
static int  g_npids = 0;
static char g_track[7168];         /* built ids JSON array for tracking    */
static char g_stat[128];           /* one hal_nostr_stats result           */
static char g_authors[96][66];     /* recent author pubkeys (ring)         */
static char g_adone[96];           /* 1 once a profile was pushed          */
static int  g_nauth = 0;
static char g_prof[2560];          /* one hal_nostr_profile result         */
static char g_rdone[96];           /* 1 once replies pushed for post i     */
static char g_replies[8192];       /* one hal_nostr_replies result         */
static int  g_ticks = 0;
static int  g_activity_all = 1;   /* current activity filter is All */

/* ── Subscriptions ───────────────────────────────────────────────────── */
static void subscribe_all(void) {
    if (!g_self[0]) {
        int sn = hal_nostr_self(g_self, sizeof(g_self) - 1);
        if (sn > 0) g_self[sn] = '\0';
    }
    /* (a) LIVE firehose — this is the "All" tab. kind-1 as the relays push it,
     * within a second of being posted, spam-gated by the host (hashtag walls,
     * link-only adverts, copy-paste bot rings, flooding authors, and accounts
     * with no profile are never delivered).
     *
     * The All tab used to be fed by discovery (below), which is a feed of
     * REACTIONS: it can only surface a post once that post has already collected
     * likes, so the newest thing on screen was routinely an hour or two old. A
     * feed for discovering people you don't follow yet has to be live, or it is
     * not doing the job. */
    /* ONLY WITH A UI. A firehose has no reason to exist when nobody is looking:
     * with the screen off we want the people you follow, your replies and your
     * messages — not the public feed of strangers. The host refuses it to a
     * headless engine anyway; this just saves the call. */
    if (g_activity_all && !g_sub_fire[0] && hal_ui_attached()) {
        int n = hal_nostr_firehose(g_sub_fire, sizeof(g_sub_fire) - 1);
        if (n > 0) g_sub_fire[n] = '\0';
    }
    /* (b) Popular — global kind-1 posts that gathered at least 2 distinct likes
     * (host-side reaction gate). Still worth having: it is the "what is worth
     * reading" signal, and posts arriving here are marked pop=1 so the UI can
     * rank them. It is no longer pretending to be the live feed. */
    /* Discovery is intentionally not opened for All. Its events bypass the
     * curated 100-note batch and made stale promotional links look selected. */
    /* (c) Direct follows — every kind-1 root/reply from an author the user
     * explicitly follows. This feed is not curated and never includes followers
     * or follows-of-follows. */
    if (!g_sub_follows[0]) {
        int fn = hal_nostr_follows(g_direct, sizeof(g_direct) - 1);
        if (fn > 0) g_direct[fn] = '\0'; else str_copy(g_direct, "[]", sizeof(g_direct));
        if (str_len(g_direct) > 2) {
            str_copy(g_feedfilter, "{\"kinds\":[1],\"authors\":", sizeof(g_feedfilter));
            str_cat(g_feedfilter, g_direct, sizeof(g_feedfilter));
            str_cat(g_feedfilter, ",\"limit\":200}", sizeof(g_feedfilter));
            int n = hal_nostr_subscribe(g_feedfilter, str_len(g_feedfilter),
                                        g_sub_follows, sizeof(g_sub_follows) - 1);
            if (n > 0) g_sub_follows[n] = '\0';
        }
    }
}

/* ── Activity feed ───────────────────────────────────────────────────── */
/* The first e-tag id (replied-to / referenced event) inside the event's "tags"
 * array, or "" if none. A kind-1 with an e-tag is treated as a reply so it stays
 * out of the roots-only "All" tab (but still shows under "Following"). */
static void first_etag(const char *evt, char *out, unsigned cap) {
    out[0] = '\0';
    const char *p = evt;
    for (const char *q = evt; q[0]; q++) {
        if (q[0]=='"'&&q[1]=='t'&&q[2]=='a'&&q[3]=='g'&&q[4]=='s'&&q[5]=='"') { p = q; break; }
    }
    for (; p[0]; p++) {
        if (p[0]=='['&&p[1]=='"'&&p[2]=='e'&&p[3]=='"'&&p[4]==','&&p[5]=='"') {
            const char *s = p + 6; unsigned o = 0;
            while (*s && *s != '"' && o < cap - 1) out[o++] = *s++;
            out[o] = '\0';
            return;
        }
    }
}

/* pop=1 marks a post that came from the discovery (>=2 likes) feed, so the host
 * keeps it in the All tab even after the transient like count resets.
 * field routes the post to a chat feed ("activity" = main stream,
 * "search_results" = the Search panel). */
static void feed_append_to(const char *evt, int pop, const char *field,
                           const char *source) {
    char pubkey[80] = "", content[6000] = "", ts[24] = "", id[80] = "";
    json_raw(evt, "pubkey", pubkey, sizeof(pubkey));
    json_raw(evt, "content", content, sizeof(content)); /* still escaped */
    json_raw(evt, "created_at", ts, sizeof(ts));
    json_raw(evt, "id", id, sizeof(id));
    if (!content[0]) return;
    char parent[80] = ""; first_etag(evt, parent, sizeof(parent));
    if (id[0]) { g_rdone[g_npids % 96] = 0; str_copy(g_pids[g_npids % 96], id, 66); g_npids++; }
    if (pubkey[0]) {                                        /* track author */
        str_copy(g_authors[g_nauth % 96], pubkey, 66);
        g_adone[g_nauth % 96] = 0;
        g_nauth++;
    }
    char from[16] = ""; short12(pubkey, from);
    str_copy(g_msg, "{\"type\":\"ui.chat.append\",\"field\":\"", sizeof(g_msg));
    str_cat(g_msg, field, sizeof(g_msg));
    str_cat(g_msg, "\",\"message\":{\"dir\":\"in\",\"from\":\"", sizeof(g_msg));
    str_cat(g_msg, from, sizeof(g_msg));
    str_cat(g_msg, "\",\"author\":\"", sizeof(g_msg));
    str_cat(g_msg, pubkey, sizeof(g_msg));
    str_cat(g_msg, "\",\"text\":\"", sizeof(g_msg));
    str_cat(g_msg, content, sizeof(g_msg));      /* already-escaped body */
    /* The event id becomes the post's mid so the host can count likes/replies
     * and attach a reaction to it. */
    str_cat(g_msg, "\",\"mid\":\"", sizeof(g_msg));
    str_cat(g_msg, id, sizeof(g_msg));
    str_cat(g_msg, "\",\"parent\":\"", sizeof(g_msg));
    str_cat(g_msg, parent, sizeof(g_msg)); /* reply target, or "" for a root */
    str_cat(g_msg, "\",\"pop\":", sizeof(g_msg));
    str_cat(g_msg, pop ? "1" : "0", sizeof(g_msg));
    str_cat(g_msg, ",\"source\":\"", sizeof(g_msg));
    str_cat(g_msg, source, sizeof(g_msg));
    str_cat(g_msg, "\"", sizeof(g_msg));
    if (str_eq(source, "firehose")) {
        char batch[24] = "", mode[24] = "", index[16] = "", size[16] = "";
        json_raw(evt, "_xprs_batch", batch, sizeof(batch));
        json_raw(evt, "_xprs_batch_mode", mode, sizeof(mode));
        json_raw(evt, "_xprs_batch_index", index, sizeof(index));
        json_raw(evt, "_xprs_batch_size", size, sizeof(size));
        if (batch[0]) {
            str_cat(g_msg, ",\"batch\":", sizeof(g_msg)); str_cat(g_msg, batch, sizeof(g_msg));
            str_cat(g_msg, ",\"batch_mode\":\"", sizeof(g_msg)); str_cat(g_msg, mode, sizeof(g_msg));
            str_cat(g_msg, "\",\"batch_index\":", sizeof(g_msg)); str_cat(g_msg, index[0] ? index : "0", sizeof(g_msg));
            str_cat(g_msg, ",\"batch_size\":", sizeof(g_msg)); str_cat(g_msg, size[0] ? size : "0", sizeof(g_msg));
        }
    }
    str_cat(g_msg, ",", sizeof(g_msg));
    cat_time_fields(g_msg, ts, sizeof(g_msg));
    str_cat(g_msg, "}}", sizeof(g_msg));
    send_msg(g_msg);
}

/* Lowercase ASCII compare: does haystack contain needle, ignoring case? */
static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int contains_ci(const char *hay, const char *needle) {
    if (!needle[0]) return 1;
    for (const char *h = hay; *h; h++) {
        unsigned i = 0;
        while (needle[i] && h[i] && lc(h[i]) == lc(needle[i])) i++;
        if (!needle[i]) return 1;
    }
    return 0;
}

/* Does the result actually match what the user typed? A NIP-50 relay answers
 * with matches; a relay that does not support it answers with EVERYTHING, and
 * the user then watched a feed of strangers scroll past under their query. */
static int matches_query(const char *evt) {
    if (!g_query[0]) return 0;
    char content[6000] = "";
    json_raw(evt, "content", content, sizeof(content));
    return contains_ci(content, g_query);
}

/* Does the note carry a picture or a video? NOSTR has no media flag — the
 * media IS the body: an http(s) link ending in an image/video extension, or a
 * host `file:<sha>.<ext>` token. Used by the search panel's "only with media"
 * filter. */
static int has_media(const char *evt) {
    char content[6000] = "";
    json_raw(evt, "content", content, sizeof(content));
    for (const char *p = content; *p; p++) {
        if (p[0]=='f'&&p[1]=='i'&&p[2]=='l'&&p[3]=='e'&&p[4]==':') return 1;
        if (p[0]=='.') {
            const char *e = p + 1;
            if ((e[0]=='j'&&e[1]=='p'&&e[2]=='g') ||
                (e[0]=='j'&&e[1]=='p'&&e[2]=='e'&&e[3]=='g') ||
                (e[0]=='p'&&e[1]=='n'&&e[2]=='g') ||
                (e[0]=='g'&&e[1]=='i'&&e[2]=='f') ||
                (e[0]=='w'&&e[1]=='e'&&e[2]=='b'&&e[3]=='p') ||
                (e[0]=='m'&&e[1]=='p'&&e[2]=='4') ||
                (e[0]=='w'&&e[1]=='e'&&e[2]=='b'&&e[3]=='m')) return 1;
        }
    }
    return 0;
}

/* A kind-0 profile hit in search → a "person" result card. The header name +
 * avatar are resolved host-side from the stored profile (by short12 pubkey);
 * tapping the author opens the full profile. */
static void search_profile(const char *evt) {
    char pubkey[80] = "";
    json_raw(evt, "pubkey", pubkey, sizeof(pubkey));
    if (!pubkey[0]) return;
    char from[16] = ""; short12(pubkey, from);
    str_copy(g_msg,
        "{\"type\":\"ui.chat.append\",\"field\":\"search_results\",\"message\":"
        "{\"dir\":\"in\",\"from\":\"", sizeof(g_msg));
    str_cat(g_msg, from, sizeof(g_msg));
    /* 👤 = 👤 ; label marks it as a person, not a post. */
    str_cat(g_msg,
        "\",\"text\":\"\\uD83D\\uDC64 Profile — tap the name to open\","
        "\"mid\":\"\",\"parent\":\"\",\"pop\":0,\"t\":0}}", sizeof(g_msg));
    send_msg(g_msg);
}

/* ── Direct messages (kind-4) ────────────────────────────────────────── */



static void drain(void) {
    /* The live firehose feeds the "All" tab. Drained first and harder than the
     * others — it is the only sub that carries what is happening RIGHT NOW. */
    /* 40 per tick (≈26/s) could not keep up with what the host hands us (up to
     * 150/s), so the wapp was permanently draining a backlog and the newest post
     * on screen was minutes old. Drain hard: the feed is the point. */
    for (int i = 0; i < 300 && g_sub_fire[0]; i++) {
        int n = hal_nostr_event_recv(g_sub_fire, str_len(g_sub_fire), g_evt, sizeof(g_evt) - 1);
        if (n <= 0) break;
        g_evt[n] = '\0'; feed_append_to(g_evt, 0, "activity", "firehose");
    }
    for (int i = 0; i < 20 && g_sub_disc[0]; i++) {
        int n = hal_nostr_event_recv(g_sub_disc, str_len(g_sub_disc), g_evt, sizeof(g_evt) - 1);
        if (n <= 0) break;
        g_evt[n] = '\0'; feed_append_to(g_evt, 1, "activity", "discovery");
    }
    for (int i = 0; i < 20 && g_sub_follows[0]; i++) {
        int n = hal_nostr_event_recv(g_sub_follows, str_len(g_sub_follows), g_evt, sizeof(g_evt) - 1);
        if (n <= 0) break;
        g_evt[n] = '\0'; feed_append_to(g_evt, 0, "activity", "following");
    }
    /* Deep-linked publication (host view.open): the post and its replies go
     * into the activity feed like a follow's post — the host is polling the
     * archive and opens the thread page the moment the root arrives. */
    for (int i = 0; i < 20 && g_sub_post[0]; i++) {
        int n = hal_nostr_event_recv(g_sub_post, str_len(g_sub_post), g_evt, sizeof(g_evt) - 1);
        if (n <= 0) break;
        g_evt[n] = '\0'; feed_append_to(g_evt, 0, "activity", "thread");
    }
    /* Search results (NIP-50): kind-1 posts → result cards; kind-0 profiles →
     * a "person" card (the host resolves name+avatar by pubkey). */
    for (int i = 0; i < 30 && g_sub_search[0]; i++) {
        int n = hal_nostr_event_recv(g_sub_search, str_len(g_sub_search), g_evt, sizeof(g_evt) - 1);
        if (n <= 0) break;
        g_evt[n] = '\0';
        char k[8] = ""; json_raw(g_evt, "kind", k, sizeof(k));
        /* kind-0 content IS the profile JSON (name, about…), so the same
         * substring test finds people by name. */
        if (!matches_query(g_evt)) continue;
        if (str_eq(k, "0")) search_profile(g_evt);
        else if (!g_search_media || has_media(g_evt))
            feed_append_to(g_evt, 0, "search_results", "search");
    }
}

/* The host identifies a post's author by the first 12 hex chars of the pubkey
 * (that is what a card's `from` carries). Following someone needs the WHOLE
 * key, so look it up in the ring of authors we have actually seen. Without
 * this, tapping Follow did nothing at all: the wapp had no key to follow with,
 * so no kind-3 was ever published, and the Follows list stayed empty. */
static int author_for_short(const char *shortkey, char *out, unsigned cap) {
    out[0] = '\0';
    if (!shortkey[0]) return 0;
    for (int i = 0; i < 96; i++) {
        const char *a = g_authors[i];
        if (!a[0]) continue;
        unsigned n = 0;
        while (shortkey[n] && a[n] && lc(a[n]) == lc(shortkey[n])) n++;
        if (!shortkey[n]) { str_copy(out, a, cap); return 1; }
    }
    return 0;
}

/* ── Follows list ────────────────────────────────────────────────────── */
static void push_follows(void) {
    int fn = hal_nostr_follows(g_follows, sizeof(g_follows) - 1);
    if (fn > 0) g_follows[fn] = '\0'; else str_copy(g_follows, "[]", sizeof(g_follows));
    str_copy(g_msg, "{\"type\":\"ui.people.set\",\"field\":\"follows_list\",\"sections\":[{\"title\":\"Following\",\"items\":[", sizeof(g_msg));
    int first = 1, shown = 0;
    for (char *p = g_follows; *p; p++) {
        if (*p != '"') continue;
        char hex[80] = ""; unsigned o = 0; p++;
        while (*p && *p != '"' && o < sizeof(hex) - 1) hex[o++] = *p++;
        hex[o] = '\0';
        if (o < 32) continue;   /* skip non-key tokens */
        if (shown >= 20) break; /* keep the UI message below its fixed cap */
        shown++;
        char title[256] = "", pic[512] = "", npub[80] = "";
        int pn = hal_nostr_profile(hex, str_len(hex), g_prof, sizeof(g_prof) - 1);
        if (pn > 0) {
            g_prof[pn] = '\0';
            json_raw(g_prof, "name", title, sizeof(title));
            json_raw(g_prof, "pic", pic, sizeof(pic));
            json_raw(g_prof, "npub", npub, sizeof(npub));
        }
        if (!title[0]) short12(hex, title);
        if (!first) str_cat(g_msg, ",", sizeof(g_msg));
        first = 0;
        str_cat(g_msg, "{\"id\":\"", sizeof(g_msg)); str_cat(g_msg, hex, sizeof(g_msg));
        str_cat(g_msg, "\",\"title\":\"", sizeof(g_msg)); str_cat(g_msg, title, sizeof(g_msg));
        str_cat(g_msg, "\",\"subtitle\":\"", sizeof(g_msg));
        str_cat(g_msg, npub[0] ? npub : hex, sizeof(g_msg));
        str_cat(g_msg, "\"", sizeof(g_msg));
        if (pic[0]) { str_cat(g_msg, ",\"avatar\":\"", sizeof(g_msg)); str_cat(g_msg, pic, sizeof(g_msg)); str_cat(g_msg, "\"", sizeof(g_msg)); }
        str_cat(g_msg, ",\"action\":\"follows_list_unfollow\",\"actionLabel\":\"Unfollow\"}", sizeof(g_msg));
    }
    str_cat(g_msg, "]}]}", sizeof(g_msg));
    send_msg(g_msg);
}

/* ── Relay panel ─────────────────────────────────────────────────────── */
static void push_relays(void) {
    int n = hal_nostr_relays(g_relays, sizeof(g_relays) - 1);
    if (n <= 0) g_relays[0] = '\0'; else g_relays[n] = '\0';
    str_copy(g_msg, "{\"type\":\"ui.people.set\",\"field\":\"relays\",\"sections\":[{\"title\":\"Relays\",\"items\":[", sizeof(g_msg));
    int first = 1;
    for (char *p = g_relays; *p; p++) {
        if (*p != '{') continue;
        char uri[256] = "", scheme[24] = "", status[24] = "";
        json_raw(p, "uri", uri, sizeof(uri));
        json_raw(p, "scheme", scheme, sizeof(scheme));
        json_raw(p, "status", status, sizeof(status));
        if (!uri[0]) continue;
        if (!first) str_cat(g_msg, ",", sizeof(g_msg));
        first = 0;
        str_cat(g_msg, "{\"id\":\"", sizeof(g_msg)); json_escape_cat(g_msg, uri, sizeof(g_msg));
        str_cat(g_msg, "\",\"title\":\"", sizeof(g_msg)); json_escape_cat(g_msg, uri, sizeof(g_msg));
        str_cat(g_msg, "\",\"subtitle\":\"", sizeof(g_msg)); str_cat(g_msg, scheme, sizeof(g_msg));
        str_cat(g_msg, "\",\"tags\":[\"", sizeof(g_msg)); str_cat(g_msg, status[0] ? status : "?", sizeof(g_msg));
        str_cat(g_msg, "\"]}", sizeof(g_msg));
        while (*p && *p != '}') p++;
        if (!*p) break;
    }
    str_cat(g_msg, "]}]}", sizeof(g_msg));
    send_msg(g_msg);
}

/* ── Replies (threads) ───────────────────────────────────────────────── */
/* Append a reply as a parented activity post (hidden from the main stream,
 * shown inside the parent's thread). [content] must already be JSON-escaped. */
static void reply_append(const char *parent, const char *rid, const char *pubkey,
                         const char *content, const char *ts) {
    char from[16] = ""; short12(pubkey, from);
    str_copy(g_msg, "{\"type\":\"ui.chat.append\",\"field\":\"activity\",\"message\":{\"dir\":\"in\",\"from\":\"", sizeof(g_msg));
    str_cat(g_msg, from, sizeof(g_msg));
    str_cat(g_msg, "\",\"text\":\"", sizeof(g_msg));
    str_cat(g_msg, content, sizeof(g_msg));
    str_cat(g_msg, "\",\"mid\":\"", sizeof(g_msg));
    str_cat(g_msg, rid, sizeof(g_msg));
    str_cat(g_msg, "\",\"parent\":\"", sizeof(g_msg));
    str_cat(g_msg, parent, sizeof(g_msg));
    str_cat(g_msg, "\",", sizeof(g_msg));
    cat_time_fields(g_msg, ts, sizeof(g_msg));
    str_cat(g_msg, "}}", sizeof(g_msg));
    send_msg(g_msg);
}

/* Fetch stored replies for a post and push them into its thread. */
static void push_replies_for(const char *postid) {
    int n = hal_nostr_replies(postid, str_len(postid), g_replies, sizeof(g_replies) - 1);
    if (n <= 0) return;
    g_replies[n] = '\0';
    for (char *p = g_replies; *p; p++) {
        if (*p != '{') continue;
        char rid[80] = "", pk[80] = "", content[4000] = "", ts[24] = "";
        json_raw(p, "id", rid, sizeof(rid));
        json_raw(p, "pubkey", pk, sizeof(pk));
        json_raw(p, "content", content, sizeof(content)); /* stays escaped */
        json_raw(p, "ts", ts, sizeof(ts));
        if (pk[0]) { /* fetch the reply author's profile too */
            str_copy(g_authors[g_nauth % 96], pk, 66);
            g_adone[g_nauth % 96] = 0;
            g_nauth++;
        }
        if (rid[0]) { /* track the reply for its own like/reply counts */
            g_rdone[g_npids % 96] = 0;
            str_copy(g_pids[g_npids % 96], rid, 66);
            g_npids++;
        }
        if (content[0]) reply_append(postid, rid, pk, content, ts);
        while (*p && *p != '}') p++;
        if (!*p) break;
    }
}

/* ── Engagement (likes/replies) ──────────────────────────────────────── */
/* Ask the host to count reactions/replies for the posts on screen, then push
 * the counts back as generic ui.activity.stats messages the feed renders. */
/* Push ONE post's like/reply counts right now, regardless of whether it's in
 * g_pids and regardless of the zero-engagement skip. Used the instant the user
 * likes/replies so the heart/count reflects immediately (the optimistic tally
 * lives in the host engine, keyed by this exact mid). */
static void push_one_stat(const char* mid) {
    if (!mid || !mid[0]) return;
    int n = hal_nostr_stats(mid, str_len(mid), g_stat, sizeof(g_stat) - 1);
    if (n <= 0) return;
    g_stat[n] = '\0';
    char likes[12] = "", replies[12] = "", mine[8] = "";
    json_raw(g_stat, "likes", likes, sizeof(likes));
    json_raw(g_stat, "replies", replies, sizeof(replies));
    json_raw(g_stat, "mine", mine, sizeof(mine));
    str_copy(g_msg, "{\"type\":\"ui.activity.stats\",\"mid\":\"", sizeof(g_msg));
    str_cat(g_msg, mid, sizeof(g_msg));
    str_cat(g_msg, "\",\"likes\":", sizeof(g_msg));
    str_cat(g_msg, likes[0] ? likes : "0", sizeof(g_msg));
    str_cat(g_msg, ",\"replies\":", sizeof(g_msg));
    str_cat(g_msg, replies[0] ? replies : "0", sizeof(g_msg));
    str_cat(g_msg, ",\"mine\":", sizeof(g_msg));
    str_cat(g_msg, (mine[0] == 't') ? "true" : "false", sizeof(g_msg));
    str_cat(g_msg, "}", sizeof(g_msg));
    send_msg(g_msg);
}

static void push_stats(void) {
    int valid = g_npids < 96 ? g_npids : 96;
    if (valid == 0) return;
    int rfetch = 0; /* bound reply store-queries per poll (main thread) */
    str_copy(g_track, "[", sizeof(g_track));
    for (int i = 0; i < valid; i++) {
        if (i) str_cat(g_track, ",", sizeof(g_track));
        str_cat(g_track, "\"", sizeof(g_track));
        str_cat(g_track, g_pids[i], sizeof(g_track));
        str_cat(g_track, "\"", sizeof(g_track));
    }
    str_cat(g_track, "]", sizeof(g_track));
    hal_nostr_track(g_track, str_len(g_track));

    for (int i = 0; i < valid; i++) {
        int n = hal_nostr_stats(g_pids[i], str_len(g_pids[i]), g_stat, sizeof(g_stat) - 1);
        if (n <= 0) continue;
        g_stat[n] = '\0';
        char likes[12] = "", replies[12] = "", mine[8] = "";
        json_raw(g_stat, "likes", likes, sizeof(likes));
        json_raw(g_stat, "replies", replies, sizeof(replies));
        json_raw(g_stat, "mine", mine, sizeof(mine));
        int noLikes = (!likes[0] || (likes[0] == '0' && !likes[1]));
        int noReplies = (!replies[0] || (replies[0] == '0' && !replies[1]));
        /* Pull replies into the post's thread once it has any (bounded/poll). */
        if (!noReplies && !g_rdone[i] && rfetch < 3) {
            push_replies_for(g_pids[i]); g_rdone[i] = 1; rfetch++;
        }
        if (noLikes && noReplies) continue; /* skip zero-engagement (default) */
        str_copy(g_msg, "{\"type\":\"ui.activity.stats\",\"mid\":\"", sizeof(g_msg));
        str_cat(g_msg, g_pids[i], sizeof(g_msg));
        str_cat(g_msg, "\",\"likes\":", sizeof(g_msg));
        str_cat(g_msg, likes[0] ? likes : "0", sizeof(g_msg));
        str_cat(g_msg, ",\"replies\":", sizeof(g_msg));
        str_cat(g_msg, replies[0] ? replies : "0", sizeof(g_msg));
        str_cat(g_msg, ",\"mine\":", sizeof(g_msg));
        str_cat(g_msg, (mine[0] == 't') ? "true" : "false", sizeof(g_msg));
        str_cat(g_msg, "}", sizeof(g_msg));
        send_msg(g_msg);
    }
}

/* Fetch kind-0 profiles for post authors and push them (name/avatar/bio) as
 * generic ui.profile.set messages, so the feed shows names instead of npubs. */
static void push_profiles(void) {
    int valid = g_nauth < 96 ? g_nauth : 96;
    int did = 0;
    for (int i = 0; i < valid; i++) {
        if (g_adone[i]) continue;
        if (did >= 30) break; /* profile lookups are cache reads now (off-thread) */
        did++;
        int n = hal_nostr_profile(g_authors[i], str_len(g_authors[i]),
                                  g_prof, sizeof(g_prof) - 1);
        if (n <= 0) continue;
        g_prof[n] = '\0';
        char name[256] = "";
        json_raw(g_prof, "name", name, sizeof(name));
        if (!name[0]) continue; /* kind-0 not in yet — retry next poll */
        char pic[512] = "", about[1024] = "", nip05[128] = "", npub[80] = "";
        char website[256] = "", lud16[128] = "", banner[512] = "";
        json_raw(g_prof, "pic", pic, sizeof(pic));
        json_raw(g_prof, "about", about, sizeof(about));
        json_raw(g_prof, "nip05", nip05, sizeof(nip05));
        json_raw(g_prof, "npub", npub, sizeof(npub));
        json_raw(g_prof, "website", website, sizeof(website));
        json_raw(g_prof, "lud16", lud16, sizeof(lud16));
        json_raw(g_prof, "banner", banner, sizeof(banner));
        char key[16] = ""; str_copy(key, g_authors[i], 13); /* short12 = from */
        /* json_raw values keep their escaping, so embed them verbatim. */
        str_copy(g_msg, "{\"type\":\"ui.profile.set\",\"key\":\"", sizeof(g_msg));
        str_cat(g_msg, key, sizeof(g_msg));
        str_cat(g_msg, "\",\"name\":\"", sizeof(g_msg)); str_cat(g_msg, name, sizeof(g_msg));
        str_cat(g_msg, "\",\"pic\":\"", sizeof(g_msg)); str_cat(g_msg, pic, sizeof(g_msg));
        str_cat(g_msg, "\",\"about\":\"", sizeof(g_msg)); str_cat(g_msg, about, sizeof(g_msg));
        str_cat(g_msg, "\",\"nip05\":\"", sizeof(g_msg)); str_cat(g_msg, nip05, sizeof(g_msg));
        str_cat(g_msg, "\",\"npub\":\"", sizeof(g_msg)); str_cat(g_msg, npub, sizeof(g_msg));
        str_cat(g_msg, "\",\"website\":\"", sizeof(g_msg)); str_cat(g_msg, website, sizeof(g_msg));
        str_cat(g_msg, "\",\"lud16\":\"", sizeof(g_msg)); str_cat(g_msg, lud16, sizeof(g_msg));
        str_cat(g_msg, "\",\"banner\":\"", sizeof(g_msg)); str_cat(g_msg, banner, sizeof(g_msg));
        str_cat(g_msg, "\"}", sizeof(g_msg));
        send_msg(g_msg);
        g_adone[i] = 1;
    }
}

/* ── Module entry points ─────────────────────────────────────────────── */
int32_t module_init(void) {
    hal_log(6, "[social] up", 11);
    subscribe_all();
    /* The relay list is host-managed now (the "NOSTR on Internet" panel);
     * pushing our own view of it would fight with the host's. */
    push_follows();
    return 0;
}

int32_t module_tick(void) {
    subscribe_all();
    drain();
    g_ticks++;
    // The direct contact snapshot can arrive after startup, so re-open the
    // follows feed a couple of times early to pick it up promptly.
    // (Discovery is author-independent — leave it live.)
    if (g_ticks == 10 || g_ticks == 30) {
        if (g_sub_follows[0]) {
            hal_nostr_unsubscribe(g_sub_follows, str_len(g_sub_follows));
            g_sub_follows[0] = '\0';
        }
        subscribe_all();
    }
    /* Relay live-push is not reliable after Android has suspended sockets.
     * Re-issue the bounded follows query every ten minutes so Following cannot
     * sit on an hour-old snapshot. The wapp itself runs off the Flutter UI
     * isolate, and the relay work stays in the NOSTR engine isolate. */
    if (g_ticks > 0 && g_ticks % 857 == 0) {
        if (g_sub_follows[0]) {
            hal_nostr_unsubscribe(g_sub_follows, str_len(g_sub_follows));
            g_sub_follows[0] = '\0';
        }
        subscribe_all();
    }
    if (g_ticks % 8 == 0) push_relays();
    if (g_ticks % 5 == 0) push_stats();      /* refresh like/reply counts */
    if (g_ticks % 3 == 2) push_profiles();   /* fetch + show author names */
    return 0;
}

int32_t module_handle_event(void) {
    static char buf[6144];
    uint32_t n = hal_msg_recv(buf, sizeof(buf) - 1);
    if (n == 0) return 0;
    buf[n] = '\0';
    /* Host deep-link: {"type":"view.open","view":"post:<id>"} — the launcher
     * hero card asks for one publication. Subscribe to that event id AND the
     * kind-1 replies referencing it (#e), so the thread fills fast; the host
     * opens the thread page once the root post lands in the archive. */
    {
        char type[24] = "";
        if (json_raw(buf, "type", type, sizeof(type)) &&
            str_eq(type, "view.open")) {
            char view[96] = "";
            if (json_raw(buf, "view", view, sizeof(view)) &&
                view[0] == 'p' && view[1] == 'o' && view[2] == 's' &&
                view[3] == 't' && view[4] == ':' && view[5]) {
                const char *mid = view + 5;
                if (g_sub_post[0]) {
                    hal_nostr_unsubscribe(g_sub_post, str_len(g_sub_post));
                    g_sub_post[0] = '\0';
                }
                char filter[320];
                str_copy(filter, "[{\"ids\":[\"", sizeof(filter));
                str_cat(filter, mid, sizeof(filter));
                str_cat(filter, "\"]},{\"kinds\":[1],\"#e\":[\"", sizeof(filter));
                str_cat(filter, mid, sizeof(filter));
                str_cat(filter, "\"],\"limit\":100}]", sizeof(filter));
                int m = hal_nostr_subscribe(filter, str_len(filter),
                                            g_sub_post, sizeof(g_sub_post) - 1);
                if (m > 0) g_sub_post[m] = '\0';
                drain(); /* the local relay answers immediately — pull it now */
            }
            return 0;
        }
    }
    char cmd[64] = "";
    if (!json_raw(buf, "command", cmd, sizeof(cmd))) return 0;

    if (str_eq(cmd, "ready") || str_eq(cmd, "refresh")) {
        if (g_sub_follows[0]) {
            hal_nostr_unsubscribe(g_sub_follows, str_len(g_sub_follows));
            g_sub_follows[0] = '\0';
        }
        subscribe_all(); push_relays(); push_follows();
    } else if (str_eq(cmd, "activity_send")) {
        char text[6000] = "";
        if (json_raw(buf, "activity_input", text, sizeof(text)) && text[0]) {
            hal_nostr_post(1, text, str_len(text), "[]", 2);
            /* The same words also go on the AIR as an XPRS t:status: the
             * core picks the bearers (BLE now, LoRa when it exists,
             * Reticulum), splits and signs. This wapp only supplies the
             * content — it neither knows nor chooses how the bytes travel. */
            hal_xprs_status(text, str_len(text), 0, 0);
        }
    } else if (str_eq(cmd, "activity_refresh")) {
        /* Pull-to-refresh. The host hands over the best of what the curator is
         * holding (100 notes) — we do NOT tear the firehose/discovery subs down
         * and re-open them. Churning a subscription is precisely what makes a
         * relay quietly stop answering it, and the feed then dies in silence. */
        (void)0;
        if (g_sub_follows[0]) {
            hal_nostr_unsubscribe(g_sub_follows, str_len(g_sub_follows));
            g_sub_follows[0] = '\0';
        }
        subscribe_all();
    } else if (str_eq(cmd, "activity_filter_changed")) {
        char filter[24] = "";
        json_raw(buf, "activity_filter", filter, sizeof(filter));
        g_activity_all = str_eq(filter, "all");
        if (!g_activity_all) {
            /* Keep the page's firehose subscription alive while another feed
             * filter is selected.  Tab rebuilds and delayed filter callbacks
             * used to tear it down even though All was still visible, which
             * also cancelled the host's ten-minute curator.  The foreground
             * engine owns this subscription and disposes it with the page. */
            if (g_sub_disc[0]) {
                hal_nostr_unsubscribe(g_sub_disc, str_len(g_sub_disc));
                g_sub_disc[0] = '\0';
            }
            if (str_eq(filter, "following")) {
                if (g_sub_follows[0]) {
                    hal_nostr_unsubscribe(g_sub_follows, str_len(g_sub_follows));
                    g_sub_follows[0] = '\0';
                }
                subscribe_all();
            }
        } else {
            subscribe_all();
        }
    } else if (str_eq(cmd, "clear_feed")) {
        send_msg("{\"type\":\"ui.chat.clear\",\"field\":\"activity\"}");
    } else if (str_eq(cmd, "search_go") || str_eq(cmd, "search_input_changed") ||
               str_eq(cmd, "search_kind_changed") ||
               str_eq(cmd, "search_when_changed") ||
               str_eq(cmd, "search_media_changed")) {
        /* Search, run on every keystroke (the host debounces "live" fields) and
         * shaped by the panel's filters:
         *
         *   Find  — Everything (kinds 0+1) / Posts (1) / People (0)
         *   When  — a "since" bound in epoch seconds
         *   Media — kept, but applied to the RESULTS (a NIP-50 relay has no
         *           media flag; g_search_media makes the drain drop posts with
         *           no image/url in them)
         *
         * One subscribe fans out to the local FTS index AND every connected
         * relay, internet and Reticulum alike. */
        char q[256] = "";
        json_raw(buf, "search_input", q, sizeof(q));
        char kind[24] = "", when[24] = "", media[8] = "";
        json_raw(buf, "search_kind", kind, sizeof(kind));
        json_raw(buf, "search_when", when, sizeof(when));
        json_raw(buf, "search_media", media, sizeof(media));
        g_search_media = str_eq(media, "true") || str_eq(media, "1");
        str_copy(g_query, q, sizeof(g_query));

        send_msg("{\"type\":\"ui.chat.clear\",\"field\":\"search_results\"}");
        if (g_sub_search[0]) {
            hal_nostr_unsubscribe(g_sub_search, str_len(g_sub_search));
            g_sub_search[0] = '\0';
        }
        /* One letter is not a search — it is every post on every relay. */
        if (str_len(q) >= 2) {
            char filter[480];
            str_copy(filter, "{\"kinds\":", sizeof(filter));
            if (str_eq(kind, "posts"))       str_cat(filter, "[1]", sizeof(filter));
            else if (str_eq(kind, "people")) str_cat(filter, "[0]", sizeof(filter));
            else                             str_cat(filter, "[0,1]", sizeof(filter));
            str_cat(filter, ",\"search\":\"", sizeof(filter));
            json_escape_cat(filter, q, sizeof(filter));
            str_cat(filter, "\"", sizeof(filter));

            unsigned long long span = 0;
            if (str_eq(when, "day")) span = 86400ULL;
            else if (str_eq(when, "week")) span = 7ULL * 86400ULL;
            else if (str_eq(when, "month")) span = 30ULL * 86400ULL;
            if (span) {
                unsigned long long now = hal_time_epoch();
                unsigned long long since = now > span ? now - span : 0;
                char sbuf[24]; u64_str(since, sbuf);
                str_cat(filter, ",\"since\":", sizeof(filter));
                str_cat(filter, sbuf, sizeof(filter));
            }
            str_cat(filter, ",\"limit\":40}", sizeof(filter));
            int m = hal_nostr_subscribe(filter, str_len(filter),
                                        g_sub_search, sizeof(g_sub_search) - 1);
            if (m > 0) g_sub_search[m] = '\0';
        }
    } else if (str_eq(cmd, "activity_like")) {
        char mid[80] = "";
        if (json_raw(buf, "activity_mid", mid, sizeof(mid)) && mid[0]) {
            hal_nostr_react(mid, str_len(mid));
            push_one_stat(mid); /* reflect THIS like immediately */
        }
    } else if (str_eq(cmd, "activity_repost")) {
        char mid[80] = "", author[80] = "";
        json_raw(buf, "activity_mid", mid, sizeof(mid));
        json_raw(buf, "activity_author", author, sizeof(author));
        if (mid[0]) hal_nostr_repost(mid, str_len(mid), author, str_len(author));
    } else if (str_eq(cmd, "activity_reply")) {
        char target[80] = "", text[6000] = "";
        json_raw(buf, "activity_target_mid", target, sizeof(target));
        json_raw(buf, "activity_input", text, sizeof(text));
        if (target[0] && text[0]) {
            hal_nostr_reply(target, str_len(target), text, str_len(text));
            char esc[6100] = ""; json_escape_cat(esc, text, sizeof(esc));
            char now[24]; u64_str(hal_time_epoch(), now); /* real ts, not epoch 0 */
            reply_append(target, "", g_self, esc, now);   /* local echo */
            push_one_stat(target); /* bump the parent's reply count now */
        }
    } else if (str_eq(cmd, "profile_follow") || str_eq(cmd, "profile_unfollow")) {
        /* The Follow button on a profile / a post's author. The host sends the
         * author's 12-char prefix; we follow the full key. */
        char target[80] = "", full[80] = "";
        json_raw(buf, "profile_target", target, sizeof(target));
        if (target[0]) {
            if (!author_for_short(target, full, sizeof(full)))
                str_copy(full, target, sizeof(full)); /* already a full key/npub */
            if (str_eq(cmd, "profile_follow")) hal_nostr_follow(full, str_len(full));
            else hal_nostr_unfollow(full, str_len(full));
            if (g_sub_follows[0]) {   /* re-open the follows feed with the new set */
                hal_nostr_unsubscribe(g_sub_follows, str_len(g_sub_follows));
                g_sub_follows[0] = '\0';
            }
            subscribe_all();
            push_follows();
        }
    } else if (str_eq(cmd, "follow_add")) {
        char key[128] = "";
        if (json_raw(buf, "follow_input", key, sizeof(key)) && key[0]) {
            hal_nostr_follow(key, str_len(key));
            if (g_sub_follows[0]) {  /* re-open follows feed with new author set */
                hal_nostr_unsubscribe(g_sub_follows, str_len(g_sub_follows));
                g_sub_follows[0] = '\0';
            }
            subscribe_all(); push_follows();
        }
    } else if (str_eq(cmd, "follows_list_unfollow") || str_eq(cmd, "follows_list")) {
        char key[128] = "";
        if (json_raw(buf, "follows_list_id", key, sizeof(key)) && key[0]) {
            hal_nostr_unfollow(key, str_len(key));
            if (g_sub_follows[0]) {
                hal_nostr_unsubscribe(g_sub_follows, str_len(g_sub_follows));
                g_sub_follows[0] = '\0';
            }
            subscribe_all(); push_follows();
        }
    } else if (str_eq(cmd, "relay_add")) {
        char uri[256] = "";
        if (json_raw(buf, "new_relay", uri, sizeof(uri)) && uri[0]) {
            hal_nostr_relay_add(uri, str_len(uri)); push_relays();
        }
    } else if (str_eq(cmd, "relays_tap") || str_eq(cmd, "relays")) {
        char uri[256] = "";
        if (json_raw(buf, "relays_id", uri, sizeof(uri)) && uri[0]) {
            hal_nostr_relay_remove(uri, str_len(uri)); push_relays();
        }
    }
    return 0;
}

int32_t module_tick_interval_ms(void) { return 700; }

void module_destroy(void) {}
