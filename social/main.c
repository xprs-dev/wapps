/*
 * social — the XPRS feed. Everything you read here was heard on the air.
 *
 *   Activity  ($type:"chat")    t:status packets (section 27) this station
 *                               heard, plus the ones it sent
 *   Following ($type:"people")  callsigns you follow; the Following tab is
 *                               this feed narrowed to them
 *   Search                      the local spool, by words and by callsign
 *
 * There is NO internet here and no NOSTR. A post becomes a `t:status` on
 * every bearer the core has active, and the feed is built from what the
 * station archived (`hal_xprs_history`). The wapp supplies words and reads
 * rows; it does not know or choose how bytes travel.
 *
 * Two consequences worth stating, because they are design and not omission:
 *
 *  - A like is a t:reaction (section 6.5) and a reply is a status carrying
 *    r: (section 27) — both real packets, aired here. A repost has no XPRS
 *    packet, so that command is answered with a note in the log rather than
 *    pretending to work.
 *  - Identity is a callsign, not an npub. A callsign is a label (section 3),
 *    so the `sig` the archive verified travels with every post and the feed
 *    says which ones are signed.
 *
 * Build: cd wapps/social && WASI_SDK_PATH=~/wasi-sdk make
 */
#include "../hal/xprs_wasm_hal.h"

/* ── String helpers ──────────────────────────────────────────────────── */
static unsigned str_len(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static int str_eq(const char *a, const char *b) { while (*a && *b && *a == *b) { a++; b++; } return *a == *b; }
static void str_copy(char *d, const char *s, unsigned m) { unsigned i = 0; while (i < m - 1 && s[i]) { d[i] = s[i]; i++; } d[i] = '\0'; }
static void str_cat(char *d, const char *s, unsigned m) { unsigned l = str_len(d); unsigned i = 0; while (l + i < m - 1 && s[i]) { d[l + i] = s[i]; i++; } d[l + i] = '\0'; }

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

static void send_msg(const char *json) { hal_msg_send(json, str_len(json)); }

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static char uc(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static int contains_ci(const char *hay, const char *needle) {
    if (!needle[0]) return 1;
    for (const char *h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b && lc(*a) == lc(*b)) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

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

/* ── Walking a JSON array of objects ─────────────────────────────────── */

/* Copy the next top-level {...} out of [s] starting at *pos into [out].
 * Brace-counting that respects strings and escapes, so a `wire` containing a
 * brace cannot end the object early. Returns 1 when one was copied. */
static int next_obj(const char *s, unsigned *pos, char *out, unsigned cap) {
    unsigned i = *pos;
    while (s[i] && s[i] != '{') i++;
    if (!s[i]) return 0;
    unsigned depth = 0, o = 0;
    int instr = 0;
    for (; s[i]; i++) {
        char c = s[i];
        if (o < cap - 1) out[o++] = c;
        if (instr) {
            if (c == '\\' && s[i + 1]) { if (o < cap - 1) out[o++] = s[++i]; continue; }
            if (c == '"') instr = 0;
            continue;
        }
        if (c == '"') { instr = 1; continue; }
        if (c == '{') depth++;
        else if (c == '}') { depth--; if (!depth) { i++; break; } }
    }
    out[o] = '\0';
    *pos = i;
    return o > 0;
}

/* ── Reading an XPRS wire ────────────────────────────────────────────── */

/* Value of `key:` in a wire, up to the next space. `m:` is greedy to the end
 * of the packet (section 2), which is what makes a status body able to hold
 * spaces at all — so it is handled separately by wire_body(). */
static int wire_key(const char *wire, const char *key, char *out, unsigned cap) {
    unsigned kl = str_len(key);
    for (const char *p = wire; *p; p++) {
        if (p != wire && p[-1] != ' ') continue;
        unsigned i = 0;
        while (i < kl && p[i] == key[i]) i++;
        if (i != kl || p[kl] != ':') continue;
        p += kl + 1;
        unsigned o = 0;
        while (*p && *p != ' ' && o < cap - 1) out[o++] = *p++;
        out[o] = '\0';
        return 1;
    }
    out[0] = '\0';
    return 0;
}

/* Everything after " m:" — the status text. Stays JSON-escaped, exactly as the
 * archive handed it over, because it is written straight back into a JSON
 * string. Returns 0 when the packet carries no body. */
static int wire_body(const char *wire, char *out, unsigned cap) {
    for (const char *p = wire; *p; p++) {
        if (!(p == wire || p[-1] == ' ')) continue;
        if (p[0] == 'm' && p[1] == ':') {
            str_copy(out, p + 2, cap);
            return out[0] ? 1 : 0;
        }
    }
    out[0] = '\0';
    return 0;
}

/* ── State ───────────────────────────────────────────────────────────── */
#define SEEN_MAX    256
#define FOLLOW_MAX   64
#define CALL_MAX     16
#define PART_MAX      8

static char g_hist[60000];              /* one hal_xprs_history reply        */
static char g_row[2048];                /* one row out of it                 */
static char g_msg[16384];               /* outbound UI message               */
static char g_seen[SEEN_MAX][20];       /* packet ids already shown (ring)   */
static int  g_nseen = 0;
static char g_follow[FOLLOW_MAX][CALL_MAX];
static int  g_nfollow = 0;
static char g_query[128] = "";          /* Search box                        */
static char g_kv[1600];                 /* follow list as stored             */

/* Multi-part statuses (section 6.6) arrive as separate packets sharing a head.
 * Hold the pieces until the set is complete, keyed by sender + ts. */
typedef struct {
    char from[CALL_MAX];
    char ts[24];
    int  total;
    int  have;
    char part[9][260];
} group_t;
static group_t g_grp[PART_MAX];
static int g_ngrp = 0;

/* ── Seen ring ───────────────────────────────────────────────────────── */
static int seen(const char *id) {
    for (int i = 0; i < g_nseen && i < SEEN_MAX; i++)
        if (str_eq(g_seen[i], id)) return 1;
    return 0;
}
static void mark_seen(const char *id) {
    str_copy(g_seen[g_nseen % SEEN_MAX], id, 20);
    g_nseen++;
}

/* ── Speaking on the air ─────────────────────────────────────────────────
 * Likes and replies go OUT from here: a like is a t:reaction (section 6.5)
 * naming the status's section-5 id, a reply is itself a t:status carrying
 * r: (section 27). The core signs and picks the bearers; the archive is the
 * core's too — this wapp only composes words. */
static char g_call[CALL_MAX] = "";
static const char *my_call(void) {
    if (!g_call[0]) {
        unsigned n = hal_identity(g_call, sizeof(g_call) - 1);
        if (n >= sizeof(g_call)) n = sizeof(g_call) - 1;
        g_call[n] = '\0';
        for (int i = 0; g_call[i]; i++) g_call[i] = uc(g_call[i]);
    }
    return g_call;
}

/* Epoch -> "YYYY-MM-DD_hh:mm:ss" UTC (section 4.8). Same civil-date
 * arithmetic the chat wapp uses; exact for any date this will ever stamp. */
static void two(char *d, int v) { d[0] = (char)('0' + (v / 10) % 10); d[1] = (char)('0' + v % 10); }
static void stamp_now(char *out, unsigned cap) {
    if (cap < 20) { out[0] = '\0'; return; }
    unsigned long long e = hal_time_epoch();
    long z = (long)(e / 86400ULL);
    unsigned s = (unsigned)(e % 86400ULL);
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned long doe = (unsigned long)(z - era * 146097);
    unsigned long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long yy = (long)yoe + era * 400;
    unsigned long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned long mp = (5 * doy + 2) / 153;
    unsigned long dd = doy - (153 * mp + 2) / 5 + 1;
    unsigned long mm = mp < 10 ? mp + 3 : mp - 9;
    int y = (int)(yy + (mm <= 2 ? 1 : 0));
    out[0] = (char)('0' + (y / 1000) % 10);
    out[1] = (char)('0' + (y / 100) % 10);
    out[2] = (char)('0' + (y / 10) % 10);
    out[3] = (char)('0' + y % 10);
    out[4] = '-'; two(out + 5, (int)mm);
    out[7] = '-'; two(out + 8, (int)dd);
    out[10] = '_'; two(out + 11, (int)(s / 3600));
    out[13] = ':'; two(out + 14, (int)((s / 60) % 60));
    out[16] = ':'; two(out + 17, (int)(s % 60));
    out[19] = '\0';
}

/* A JSON string value out of json_raw still carries its escapes; a wire is
 * one plain line, so undo them (and flatten whatever control characters an
 * input box smuggled in into spaces). */
static void unescape(char *s) {
    unsigned o = 0;
    for (unsigned i = 0; s[i]; i++) {
        char c = s[i];
        if (c == '\\' && s[i + 1]) {
            char n = s[++i];
            c = n == 'n' || n == 't' || n == 'r' ? ' ' : n;
        }
        if ((unsigned char)c < 32) c = ' ';
        s[o++] = c;
    }
    s[o] = '\0';
}

/* Tell the host one like vote landed on one post: it tallies per liker in
 * the core activity archive (idempotent), so replays cost nothing. */
static void push_react(const char *mid, const char *from, int like, int mine) {
    str_copy(g_msg, "{\"type\":\"ui.activity.react\",\"mid\":\"", sizeof(g_msg));
    str_cat(g_msg, mid, sizeof(g_msg));
    str_cat(g_msg, "\",\"from\":\"", sizeof(g_msg));
    str_cat(g_msg, from, sizeof(g_msg));
    str_cat(g_msg, "\",\"like\":", sizeof(g_msg));
    str_cat(g_msg, like ? "true" : "false", sizeof(g_msg));
    str_cat(g_msg, ",\"mine\":", sizeof(g_msg));
    str_cat(g_msg, mine ? "true" : "false", sizeof(g_msg));
    str_cat(g_msg, "}", sizeof(g_msg));
    send_msg(g_msg);
}

/* ── Follows (callsigns, kept in host kv) ────────────────────────────── */
#define FOLLOW_KEY "xprs.follow.calls"

static void follows_load(void) {
    g_nfollow = 0;
    uint32_t n = hal_kv_get(FOLLOW_KEY, str_len(FOLLOW_KEY), g_kv, sizeof(g_kv) - 1);
    if (n == 0 || n >= sizeof(g_kv)) { g_kv[0] = '\0'; return; }
    g_kv[n] = '\0';
    /* Stored as "CALL,CALL,CALL" — a list this small does not need JSON. */
    char one[CALL_MAX]; unsigned o = 0;
    for (const char *p = g_kv; ; p++) {
        if (*p && *p != ',') { if (o < sizeof(one) - 1) one[o++] = uc(*p); continue; }
        one[o] = '\0';
        if (one[0] && g_nfollow < FOLLOW_MAX) str_copy(g_follow[g_nfollow++], one, CALL_MAX);
        o = 0;
        if (!*p) break;
    }
}

static void follows_save(void) {
    g_kv[0] = '\0';
    for (int i = 0; i < g_nfollow; i++) {
        if (i) str_cat(g_kv, ",", sizeof(g_kv));
        str_cat(g_kv, g_follow[i], sizeof(g_kv));
    }
    hal_kv_set(FOLLOW_KEY, str_len(FOLLOW_KEY), g_kv, str_len(g_kv));
}

static int followed(const char *call) {
    for (int i = 0; i < g_nfollow; i++) if (str_eq(g_follow[i], call)) return 1;
    return 0;
}

static void follow_add_call(const char *raw) {
    char call[CALL_MAX]; unsigned o = 0;
    /* A suffix names a device; following is a person (section 3.1). */
    for (const char *p = raw; *p && *p != '-' && o < sizeof(call) - 1; p++) {
        if (*p == ' ') continue;
        call[o++] = uc(*p);
    }
    call[o] = '\0';
    if (!call[0] || followed(call) || g_nfollow >= FOLLOW_MAX) return;
    str_copy(g_follow[g_nfollow++], call, CALL_MAX);
    follows_save();
}

static void follow_remove_call(const char *raw) {
    char call[CALL_MAX]; unsigned o = 0;
    for (const char *p = raw; *p && *p != '-' && o < sizeof(call) - 1; p++) call[o++] = uc(*p);
    call[o] = '\0';
    for (int i = 0; i < g_nfollow; i++) {
        if (!str_eq(g_follow[i], call)) continue;
        for (int j = i; j + 1 < g_nfollow; j++) str_copy(g_follow[j], g_follow[j + 1], CALL_MAX);
        g_nfollow--;
        follows_save();
        return;
    }
}

/* ── The feed ────────────────────────────────────────────────────────── */

/* One post into a chat field. [body] is already JSON-escaped (it came out of
 * the archive's JSON and goes straight back into ours). */
static void feed_append(const char *field, const char *from, const char *body,
                        const char *mid, const char *parent, const char *ts,
                        const char *sig, const char *bearer, int own,
                        const char *source) {
    str_copy(g_msg, "{\"type\":\"ui.chat.append\",\"field\":\"", sizeof(g_msg));
    str_cat(g_msg, field, sizeof(g_msg));
    str_cat(g_msg, "\",\"message\":{\"dir\":\"", sizeof(g_msg));
    str_cat(g_msg, own ? "out" : "in", sizeof(g_msg));
    str_cat(g_msg, "\",\"from\":\"", sizeof(g_msg));
    str_cat(g_msg, from, sizeof(g_msg));
    str_cat(g_msg, "\",\"author\":\"", sizeof(g_msg));
    str_cat(g_msg, from, sizeof(g_msg));
    str_cat(g_msg, "\",\"text\":\"", sizeof(g_msg));
    str_cat(g_msg, body, sizeof(g_msg));           /* already escaped */
    str_cat(g_msg, "\",\"mid\":\"", sizeof(g_msg));
    str_cat(g_msg, mid, sizeof(g_msg));
    /* A reply to a status is itself a status carrying r: (section 27) — the
     * parent is that id, and the host threads on it. */
    str_cat(g_msg, "\",\"parent\":\"", sizeof(g_msg));
    str_cat(g_msg, parent, sizeof(g_msg));
    str_cat(g_msg, "\",\"pop\":0,\"source\":\"", sizeof(g_msg));
    str_cat(g_msg, source, sizeof(g_msg));
    str_cat(g_msg, "\"", sizeof(g_msg));
    /* Whether the archive could check the signature is part of the post: a
     * callsign is a label, so "who said this" is the signature's answer. */
    if (sig[0]) {
        str_cat(g_msg, ",\"tags\":[\"", sizeof(g_msg));
        str_cat(g_msg, sig, sizeof(g_msg));
        if (bearer[0]) { str_cat(g_msg, "\",\"", sizeof(g_msg)); str_cat(g_msg, bearer, sizeof(g_msg)); }
        str_cat(g_msg, "\"]", sizeof(g_msg));
    }
    str_cat(g_msg, ",", sizeof(g_msg));
    cat_time_fields(g_msg, ts, sizeof(g_msg));
    str_cat(g_msg, "}}", sizeof(g_msg));
    send_msg(g_msg);
}

/* Find (or start) the part-group a multi-part status belongs to. */
static group_t *group_for(const char *from, const char *ts, int total) {
    for (int i = 0; i < g_ngrp; i++)
        if (str_eq(g_grp[i].from, from) && str_eq(g_grp[i].ts, ts)) return &g_grp[i];
    group_t *g = &g_grp[g_ngrp % PART_MAX];
    if (g_ngrp < PART_MAX) g_ngrp++;
    str_copy(g->from, from, CALL_MAX);
    str_copy(g->ts, ts, sizeof(g->ts));
    g->total = total; g->have = 0;
    for (int i = 0; i < 9; i++) g->part[i][0] = '\0';
    return g;
}

/* Read the spool and push anything new into [field].
 *
 * Every status goes out once. A post from a followed callsign is labelled
 * `source:"following"`, everything else `source:"xprs"` -- the host files
 * those into its two archives and the Activity tabs read them, so the
 * narrowing lives where the tabs already are rather than being duplicated
 * here. [match] narrows by words (Search); pass "" for none. */
static void feed_from_spool(const char *field, const char *query,
                            const char *match) {
    int n = hal_xprs_history(query, str_len(query), g_hist, sizeof(g_hist) - 1);
    if (n <= 0) return;                 /* negative = our buffer is too small */
    g_hist[n] = '\0';

    /* The spool is newest first; walk it backwards is not possible in one
     * pass over a flat string, so collect ids first and emit oldest-last —
     * the host feed sorts by "t" anyway, so order here only affects which
     * ones win the seen-ring when the window is larger than the ring. */
    unsigned pos = 0;
    while (next_obj(g_hist, &pos, g_row, sizeof(g_row))) {
        char type[24] = "";
        json_raw(g_row, "type", type, sizeof(type));
        /* A reaction (6.5) is a tally on a post, never a row of its own. */
        if (str_eq(type, "reaction")) {
            char rid[20] = "", rfrom[CALL_MAX] = "", rwire[300] = "";
            json_raw(g_row, "id", rid, sizeof(rid));
            json_raw(g_row, "from", rfrom, sizeof(rfrom));
            json_raw(g_row, "wire", rwire, sizeof(rwire));
            if (!rid[0] || !rfrom[0] || seen(rid)) continue;
            char tgt[12] = "", act[12] = "";
            wire_key(rwire, "r", tgt, sizeof(tgt));
            int add = wire_key(rwire, "add", act, sizeof(act)) && str_eq(act, "like");
            int rem = !add && wire_key(rwire, "remove", act, sizeof(act)) && str_eq(act, "like");
            if (!tgt[0] || (!add && !rem)) continue;
            mark_seen(rid);
            char person[CALL_MAX]; unsigned o = 0;
            for (const char *p = rfrom; *p && *p != '-' && o < sizeof(person) - 1; p++) person[o++] = uc(*p);
            person[o] = '\0';
            push_react(tgt, person, add, str_eq(person, my_call()));
            continue;
        }
        if (!str_eq(type, "status")) continue;

        char id[20] = "", from[CALL_MAX] = "", ts[24] = "", sig[16] = "",
             bearer[12] = "", wire[1100] = "", own[8] = "";
        json_raw(g_row, "id", id, sizeof(id));
        json_raw(g_row, "from", from, sizeof(from));
        json_raw(g_row, "ts", ts, sizeof(ts));
        json_raw(g_row, "sig", sig, sizeof(sig));
        json_raw(g_row, "bearer", bearer, sizeof(bearer));
        json_raw(g_row, "own", own, sizeof(own));
        json_raw(g_row, "wire", wire, sizeof(wire));
        if (!id[0] || !wire[0]) continue;

        /* The bare callsign is the person (section 3.1). */
        char person[CALL_MAX]; unsigned o = 0;
        for (const char *p = from; *p && *p != '-' && o < sizeof(person) - 1; p++) person[o++] = uc(*p);
        person[o] = '\0';

        const char *source = followed(person) ? "following" : "xprs";

        char body[1100] = "";
        if (!wire_body(wire, body, sizeof(body))) continue;
        if (match[0] && !contains_ci(body, match) && !contains_ci(person, match)) continue;

        /* r: names the status this one replies to (section 27). */
        char parent[12] = "";
        wire_key(wire, "r", parent, sizeof(parent));

        /* Section 6.6: `n:i/k` means this is one piece of a longer status. */
        char nfield[12] = "";
        if (wire_key(wire, "n", nfield, sizeof(nfield)) && nfield[0]) {
            int idx = 0, tot = 0; const char *p = nfield;
            while (*p >= '0' && *p <= '9') idx = idx * 10 + (*p++ - '0');
            if (*p == '/') { p++; while (*p >= '0' && *p <= '9') tot = tot * 10 + (*p++ - '0'); }
            if (idx >= 1 && idx <= 9 && tot >= 1 && tot <= 9) {
                char pts[24] = ""; wire_key(wire, "ts", pts, sizeof(pts));
                group_t *g = group_for(person, pts, tot);
                if (!g->part[idx - 1][0]) { str_copy(g->part[idx - 1], body, sizeof(g->part[0])); g->have++; }
                if (g->have < g->total) continue;    /* still incomplete */
                if (seen(id)) continue;
                char whole[2200] = "";
                for (int i = 0; i < g->total; i++) str_cat(whole, g->part[i], sizeof(whole));
                mark_seen(id);
                feed_append(field, person, whole, id, parent, ts, sig, bearer, str_eq(own, "true"), source);
                continue;
            }
        }

        if (seen(id)) continue;
        mark_seen(id);
        feed_append(field, person, body, id, parent, ts, sig, bearer, str_eq(own, "true"), source);
    }
}

/* ── Following panel ─────────────────────────────────────────────────── */
static void push_follows(void) {
    str_copy(g_msg, "{\"type\":\"ui.people.set\",\"field\":\"follows_list\",\"sections\":"
                    "[{\"title\":\"Following (", sizeof(g_msg));
    char cnt[8]; int v = g_nfollow, o = 0; char tmp[8];
    if (!v) { cnt[0] = '0'; cnt[1] = '\0'; }
    else { while (v) { tmp[o++] = (char)('0' + v % 10); v /= 10; } int k = 0; while (o) cnt[k++] = tmp[--o]; cnt[k] = '\0'; }
    str_cat(g_msg, cnt, sizeof(g_msg));
    str_cat(g_msg, ")\",\"items\":[", sizeof(g_msg));
    for (int i = 0; i < g_nfollow; i++) {
        if (i) str_cat(g_msg, ",", sizeof(g_msg));
        str_cat(g_msg, "{\"id\":\"", sizeof(g_msg));
        str_cat(g_msg, g_follow[i], sizeof(g_msg));
        str_cat(g_msg, "\",\"title\":\"", sizeof(g_msg));
        str_cat(g_msg, g_follow[i], sizeof(g_msg));
        str_cat(g_msg, "\",\"subtitle\":\"heard over the air\"}", sizeof(g_msg));
    }
    str_cat(g_msg, "]}]}", sizeof(g_msg));
    send_msg(g_msg);
}

static void clear_field(const char *field) {
    str_copy(g_msg, "{\"type\":\"ui.chat.clear\",\"field\":\"", sizeof(g_msg));
    str_cat(g_msg, field, sizeof(g_msg));
    str_cat(g_msg, "\"}", sizeof(g_msg));
    send_msg(g_msg);
}

/* ── Module ──────────────────────────────────────────────────────────── */
int32_t module_init(void) {
    hal_log(6, "[social] up — XPRS only", 24);
    /* The feed's source is the spool, and the core says when the spool grew --
     * once per flush, which is also once per burst. This used to be a sqlite
     * read every 2.8 seconds on a 700ms clock, whose answer on a quiet radio
     * is always the same sixty rows. */
    {
        static const char *t = "core.archive";
        hal_event_subscribe(t, str_len(t));
    }
    follows_load();
    /* Nothing is pushed here. A ui.* message sent before the page has
     * attached is read by nobody, and the seen-ring would still have marked
     * those posts as shown — the feed would then be permanently empty with
     * the spool full. The first push happens on `ready`. */
    return 0;
}

/* The spool grew: pull what is new into the feed. */
static void drain_core_events(void) {
    static char topic[48];
    static char data[256];
    int any = 0;
    for (int guard = 0; guard < 16; guard++) {
        if (hal_event_available() == 0) break;
        if (hal_event_recv(topic, sizeof(topic) - 1, data, sizeof(data) - 1) == 0)
            break;
        any = 1;
    }
    if (!any) return;
    /* Only while somebody is looking: with the page detached these appends go
     * nowhere, and the seen-ring would eat them (see module_init). */
    if (!hal_ui_attached()) return;
    feed_from_spool("activity",
        "{\"limit\":60,\"types\":[\"status\",\"reaction\"]}", "");
}

/* No clock: a feed changes when a packet is spooled, and the core says so. */
int32_t module_tick(void) {
    return 0;
}

int32_t module_handle_event(void) {
    /* The host bundles EVERY scalar field into each command message, so this
     * buffer sizes the whole field map and not the one value we want. At 6 KB
     * a long-enough map pushed `activity_input` past the end and the post was
     * read as empty: no status, no error, nothing anywhere. Sized for the map
     * with room to spare, and a truncated read is now visible rather than
     * silently short. */
    drain_core_events();
    static char buf[24576];
    uint32_t n = hal_msg_recv(buf, sizeof(buf) - 1);
    if (n == 0) return 0;
    if (n >= sizeof(buf) - 1) hal_log(4, "[social] event truncated", 25);
    buf[n] = '\0';

    char cmd[64] = "";
    if (!json_raw(buf, "command", cmd, sizeof(cmd))) return 0;

    if (str_eq(cmd, "ready") || str_eq(cmd, "refresh") ||
        str_eq(cmd, "activity_refresh")) {
        follows_load();
        push_follows();
        /* The page is (re)attached and its buffer starts empty, so forget what
         * we think it has already been shown and fill it from the spool. */
        g_nseen = 0;
        g_ngrp = 0;
        feed_from_spool("activity",
            "{\"limit\":120,\"types\":[\"status\",\"reaction\"]}", "");

    /* `activity_send` is deliberately NOT handled here.
     *
     * The host airs the status itself, at the composer, because the wapp
     * round-trip is not reliable enough to be the only path: the same lesson
     * the NOSTR post took, recorded in wapp_page.dart — a wapp that does not
     * invoke the publish HAL drops the post silently and the user sees it
     * vanish. Measured here too: `ready` and the tick both run and fill the
     * feed, and the very same handler never reaches this branch. One
     * publisher, host-side, and our own packet returns through the spool like
     * anyone else's.
     */

    } else if (str_eq(cmd, "clear_feed")) {
        g_nseen = 0;
        clear_field("activity");

    } else if (str_eq(cmd, "search_go") || str_eq(cmd, "search_input_changed") ||
               str_eq(cmd, "search_kind_changed") ||
               str_eq(cmd, "search_when_changed")) {
        json_raw(buf, "search_input", g_query, sizeof(g_query));
        clear_field("search_results");
        int keep = g_nseen; g_nseen = 0;   /* search has its own field */
        feed_from_spool("search_results",
            "{\"limit\":200,\"types\":[\"status\"]}", g_query);
        g_nseen = keep;

    } else if (str_eq(cmd, "follow_add")) {
        /* The host names an action's companion input after the field. */
        char v[64] = "";
        if (json_raw(buf, "follow_input", v, sizeof(v)) && v[0]) follow_add_call(v);
        push_follows();

    } else if (str_eq(cmd, "profile_follow") || str_eq(cmd, "profile_unfollow")) {
        /* The Follow button on a post's author. For an XPRS feed the author
         * IS the callsign, so there is no short-key to resolve back. */
        char v[64] = "";
        if (json_raw(buf, "profile_target", v, sizeof(v)) && v[0]) {
            if (str_eq(cmd, "profile_follow")) follow_add_call(v);
            else follow_remove_call(v);
        }
        push_follows();

    } else if (str_eq(cmd, "follows_list_unfollow")) {
        char v[64] = "";
        if (json_raw(buf, "follows_list_id", v, sizeof(v)) && v[0]) follow_remove_call(v);
        push_follows();

    } else if (str_eq(cmd, "follows_list")) {
        push_follows();

    } else if (str_eq(cmd, "activity_like")) {
        /* A like on a post: t:reaction add:like r:<id> (6.5); retracting one
         * is remove:like. The core signs it and picks the bearers; the vote
         * also lands in the local tally NOW rather than after the spool
         * round trip. */
        char mid[20] = "", set[8] = "";
        json_raw(buf, "activity_mid", mid, sizeof(mid));
        json_raw(buf, "activity_set", set, sizeof(set));
        if (mid[0] && my_call()[0]) {
            int unlike = set[0] == '0';
            char ts[24]; stamp_now(ts, sizeof(ts));
            char wire[160];
            str_copy(wire, "t:reaction f:", sizeof(wire));
            str_cat(wire, my_call(), sizeof(wire));
            str_cat(wire, " ts:", sizeof(wire)); str_cat(wire, ts, sizeof(wire));
            str_cat(wire, unlike ? " remove:like r:" : " add:like r:", sizeof(wire));
            str_cat(wire, mid, sizeof(wire));
            if (hal_xprs_send(wire, str_len(wire)) == 0)
                push_react(mid, my_call(), !unlike, 1);
            else
                hal_log(4, "[social] like refused by the core", 34);
        }

    } else if (str_eq(cmd, "activity_reply")) {
        /* A reply is itself a status carrying r: (section 27). */
        char mid[20] = "", text[400] = "";
        json_raw(buf, "activity_target_mid", mid, sizeof(mid));
        json_raw(buf, "activity_input", text, sizeof(text));
        unescape(text);
        if (mid[0] && text[0] && my_call()[0]) {
            char ts[24]; stamp_now(ts, sizeof(ts));
            char wire[600];
            str_copy(wire, "t:status f:", sizeof(wire));
            str_cat(wire, my_call(), sizeof(wire));
            str_cat(wire, " ts:", sizeof(wire)); str_cat(wire, ts, sizeof(wire));
            str_cat(wire, " r:", sizeof(wire)); str_cat(wire, mid, sizeof(wire));
            str_cat(wire, " m:", sizeof(wire)); str_cat(wire, text, sizeof(wire));
            if (str_len(wire) > 250)
                hal_log(4, "[social] reply too long for one packet", 38);
            else if (hal_xprs_send(wire, str_len(wire)) != 0)
                hal_log(4, "[social] reply refused by the core", 34);
            /* No local echo: the reply returns through the spool like any
             * other status and threads onto its parent there. */
        }

    } else if (str_eq(cmd, "activity_repost")) {
        /* Still honest: section 27 has no repost packet. */
        hal_log(4, "[social] no XPRS packet for repost", 34);
    }
    return 0;
}

/* 0 = no clock: see module_tick. */
int32_t module_tick_interval_ms(void) { return 0; }

void module_destroy(void) {}
