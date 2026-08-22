/*
 * archiver — hold other people's things so they survive the device that made
 * them going offline, and tell the network where things are.
 *
 * Two jobs, two screens, one wapp. They used to be two:
 *
 *   Storage    keeps COPIES. Costs disk, and giving it up loses content.
 *   Directory  keeps POINTERS -- signed ~176-byte records saying which devices
 *              hold a given npub's posts and files. Addresses, never content,
 *              so a directory that vanishes costs the network a phone book
 *              rather than a library.
 *
 * They were separate wapps ("Archiver" and "Indexer") and that split asked the
 * owner to learn a distinction that only matters once they have already agreed
 * to help. Both answer the same question -- what is this device willing to do
 * for other people -- so they are answered in one place, with the two offers
 * still granted and revoked independently: enabling storage does not volunteer
 * the directory, and neither implies the other.
 *
 * Storage's two rules are the whole contract:
 *   - a device that never volunteered holds nothing for anybody (silence is not
 *     consent, so it starts disabled);
 *   - the limit is a CEILING, not a target. Full is full, and nothing of the
 *     owner's is ever deleted to make room for somebody else's.
 *
 * The screen is a dashboard: how full, how many files, how much of it anyone
 * ever actually wanted, how much could be freed right now. Then the handful of
 * decisions the owner makes, and one button to reclaim the space.
 *
 * Host HAL:
 *   hal_archive_status   → JSON: quota, used, items, served, freeable, switches
 *   hal_archive_drop     → run a cleanup by id ("sweep:strangers")
 *   hal_archive_set_pref → quotaGb, followed, fromNearby, mirrorSmall
 *   hal_node_status      → JSON: serving, pointers, authors, query rates, spark…
 *   hal_node_set_pref    → volunteer=off|auto|always
 *
 * Build: cd wapps/archiver && make
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
        if (*p == '"') {
            p++;
            while (*p && *p != '"' && o < m - 1) out[o++] = *p++;
        } else if (*p == '[') {
            int depth = 0;
            while (*p && o < m - 1) {
                if (*p == '[') depth++;
                if (*p == ']') { depth--; out[o++] = *p++; if (!depth) break; continue; }
                out[o++] = *p++;
            }
        } else {
            while (*p && *p != ',' && *p != '}' && o < m - 1) out[o++] = *p++;
        }
        out[o] = '\0';
        return 1;
    }
    return 0;
}

/* ── Buffers ─────────────────────────────────────────────────────────── */
static char g_status[4096];
static char g_msg[12288];
static char g_reqSpark[1024];
static char g_bwSpark[1024];
static char g_qSpark[1024];

static void send_msg(const char *json) { hal_msg_send(json, str_len(json)); }

/* Bool switches need a real bool, not the string "true". */
static void set_field_raw(const char *name, const char *raw) {
    str_copy(g_msg, "{\"type\":\"ui.field.set\",\"field\":\"", sizeof(g_msg));
    str_cat(g_msg, name, sizeof(g_msg));
    str_cat(g_msg, "\",\"value\":", sizeof(g_msg));
    str_cat(g_msg, raw, sizeof(g_msg));
    str_cat(g_msg, "}", sizeof(g_msg));
    send_msg(g_msg);
}

static void set_field(const char *name, const char *value) {
    str_copy(g_msg, "{\"type\":\"ui.set_field\",\"name\":\"", sizeof(g_msg));
    str_cat(g_msg, name, sizeof(g_msg));
    str_cat(g_msg, "\",\"value\":\"", sizeof(g_msg));
    str_cat(g_msg, value, sizeof(g_msg));
    str_cat(g_msg, "\"}", sizeof(g_msg));
    send_msg(g_msg);
}

/* Append one stat tile (commas are the caller's problem). */
static void tile(const char *id, const char *label, const char *value,
                 const char *unit, const char *hint, const char *progress,
                 int alert) {
    str_cat(g_msg, "{\"id\":\"", sizeof(g_msg));
    str_cat(g_msg, id, sizeof(g_msg));
    str_cat(g_msg, "\",\"label\":\"", sizeof(g_msg));
    str_cat(g_msg, label, sizeof(g_msg));
    str_cat(g_msg, "\",\"value\":\"", sizeof(g_msg));
    str_cat(g_msg, value, sizeof(g_msg));
    str_cat(g_msg, "\"", sizeof(g_msg));
    if (unit && unit[0]) {
        str_cat(g_msg, ",\"unit\":\"", sizeof(g_msg));
        str_cat(g_msg, unit, sizeof(g_msg));
        str_cat(g_msg, "\"", sizeof(g_msg));
    }
    if (hint && hint[0]) {
        str_cat(g_msg, ",\"hint\":\"", sizeof(g_msg));
        str_cat(g_msg, hint, sizeof(g_msg));
        str_cat(g_msg, "\"", sizeof(g_msg));
    }
    if (progress && progress[0]) {
        str_cat(g_msg, ",\"progress\":", sizeof(g_msg));
        str_cat(g_msg, progress, sizeof(g_msg));
    }
    if (alert) str_cat(g_msg, ",\"alert\":true", sizeof(g_msg));
    str_cat(g_msg, "}", sizeof(g_msg));
}

static void push_dashboard(void) {
    int n = hal_archive_status(g_status, sizeof(g_status) - 1);
    if (n <= 0) return;
    g_status[n] = '\0';

    char quota[16] = "0", usedText[24] = "0 B", quotaText[16] = "off";
    char items[16] = "0", served[16] = "0", freeable[24] = "0 B";
    char full[16] = "0";
    char followed[8] = "true", nearby[8] = "true", mirror[8] = "true";
    char reqAvg[16] = "0", reqLast[16] = "0";
    char bwAvg[24] = "0 B", bwLast[24] = "0 B";
    json_raw(g_status, "quotaGb", quota, sizeof(quota));
    json_raw(g_status, "usedText", usedText, sizeof(usedText));
    json_raw(g_status, "quotaText", quotaText, sizeof(quotaText));
    json_raw(g_status, "items", items, sizeof(items));
    json_raw(g_status, "servedItems", served, sizeof(served));
    json_raw(g_status, "freeableText", freeable, sizeof(freeable));
    json_raw(g_status, "fullFrac", full, sizeof(full));
    json_raw(g_status, "followed", followed, sizeof(followed));
    json_raw(g_status, "fromNearby", nearby, sizeof(nearby));
    json_raw(g_status, "mirrorSmall", mirror, sizeof(mirror));
    json_raw(g_status, "reqAvgPerHour", reqAvg, sizeof(reqAvg));
    json_raw(g_status, "reqLastHour", reqLast, sizeof(reqLast));
    json_raw(g_status, "bwPerHourText", bwAvg, sizeof(bwAvg));
    json_raw(g_status, "bwLastHourText", bwLast, sizeof(bwLast));
    json_raw(g_status, "reqSpark", g_reqSpark, sizeof(g_reqSpark));
    json_raw(g_status, "bwSpark", g_bwSpark, sizeof(g_bwSpark));

    int on = !str_eq(quota, "0");

    str_copy(g_msg, "{\"type\":\"ui.stats.set\",\"field\":\"dashboard\",\"tiles\":[", sizeof(g_msg));

    /* How full — the one number that matters, with the bar under it. */
    {
        char hint[64];
        str_copy(hint, "of ", sizeof(hint));
        str_cat(hint, quotaText, sizeof(hint));
        tile("used", "Storage used", usedText, "", hint, on ? full : "", 0);
    }
    str_cat(g_msg, ",", sizeof(g_msg));

    /* What other people asked for, over the last 48 hours. */
    {
        char hint[64];
        str_copy(hint, reqLast, sizeof(hint));
        str_cat(hint, " in the last hour", sizeof(hint));
        str_cat(g_msg, "{\"id\":\"req\",\"label\":\"Requests per hour\",\"value\":\"", sizeof(g_msg));
        str_cat(g_msg, reqAvg, sizeof(g_msg));
        str_cat(g_msg, "\",\"unit\":\"avg\",\"hint\":\"", sizeof(g_msg));
        str_cat(g_msg, hint, sizeof(g_msg));
        str_cat(g_msg, "\",\"spark\":", sizeof(g_msg));
        str_cat(g_msg, g_reqSpark[0] ? g_reqSpark : "[]", sizeof(g_msg));
        str_cat(g_msg, "}", sizeof(g_msg));
    }
    str_cat(g_msg, ",", sizeof(g_msg));

    /* And what it cost the uplink to give it to them. */
    {
        char hint[64];
        str_copy(hint, bwLast, sizeof(hint));
        str_cat(hint, " in the last hour", sizeof(hint));
        str_cat(g_msg, "{\"id\":\"bw\",\"label\":\"Bandwidth per hour\",\"value\":\"", sizeof(g_msg));
        str_cat(g_msg, bwAvg, sizeof(g_msg));
        str_cat(g_msg, "\",\"unit\":\"avg\",\"hint\":\"", sizeof(g_msg));
        str_cat(g_msg, hint, sizeof(g_msg));
        str_cat(g_msg, "\",\"spark\":", sizeof(g_msg));
        str_cat(g_msg, g_bwSpark[0] ? g_bwSpark : "[]", sizeof(g_msg));
        str_cat(g_msg, "}", sizeof(g_msg));
    }
    str_cat(g_msg, ",", sizeof(g_msg));
    tile("files", "Files kept", items, "", "", "", 0);
    str_cat(g_msg, ",", sizeof(g_msg));
    tile("served", "Ever fetched", served, "", "", "", 0);
    str_cat(g_msg, ",", sizeof(g_msg));
    tile("freeable", "Can be freed", freeable, "", "held for others", "", 0);

    str_cat(g_msg, "]}", sizeof(g_msg));
    send_msg(g_msg);

    set_field_raw("enabled", on ? "true" : "false");
    if (on) set_field("quota", quota);
    set_field_raw("followed", str_eq(followed, "true") ? "true" : "false");
    set_field_raw("nearby", str_eq(nearby, "true") ? "true" : "false");
    set_field_raw("mirror", str_eq(mirror, "true") ? "true" : "false");
}

/* ── Directory: the pointer role, formerly the Indexer wapp ──────────────
 *
 * A dashboard first, because a role nobody can inspect is a role nobody
 * trusts: how often anyone actually asks (with the 48-hour shape, not just a
 * lifetime total that cannot say whether anyone came today), how many links
 * are held, for how many authors, and what the hygiene sweep removed. */
static void push_directory(void) {
    int n = hal_node_status(g_status, sizeof(g_status) - 1);
    if (n <= 0) return;
    g_status[n] = '\0';

    char vol[16] = "auto";
    char pointers[16] = "0", authors[16] = "0", peers[16] = "0";
    char demoted[16] = "0";
    char qLast[16] = "0", qAvg[16] = "0";
    char ixKnown[16] = "0";
    json_raw(g_status, "volunteer", vol, sizeof(vol));
    json_raw(g_status, "pointers", pointers, sizeof(pointers));
    json_raw(g_status, "authors", authors, sizeof(authors));
    json_raw(g_status, "syncPeers", peers, sizeof(peers));
    json_raw(g_status, "demoted", demoted, sizeof(demoted));
    json_raw(g_status, "queriesLastHour", qLast, sizeof(qLast));
    json_raw(g_status, "queriesAvgPerHour", qAvg, sizeof(qAvg));
    json_raw(g_status, "querySpark", g_qSpark, sizeof(g_qSpark));
    json_raw(g_status, "indexersKnown", ixKnown, sizeof(ixKnown));

    str_copy(g_msg, "{\"type\":\"ui.stats.set\",\"field\":\"dir_dashboard\",\"tiles\":[",
             sizeof(g_msg));
    {
        char hint[64];
        str_copy(hint, qLast, sizeof(hint));
        str_cat(hint, " in the last hour", sizeof(hint));
        str_cat(g_msg, "{\"id\":\"rate\",\"label\":\"Queries per hour\",\"value\":\"",
                sizeof(g_msg));
        str_cat(g_msg, qAvg, sizeof(g_msg));
        str_cat(g_msg, "\",\"unit\":\"avg\",\"hint\":\"", sizeof(g_msg));
        str_cat(g_msg, hint, sizeof(g_msg));
        str_cat(g_msg, "\",\"spark\":", sizeof(g_msg));
        str_cat(g_msg, g_qSpark[0] ? g_qSpark : "[]", sizeof(g_msg));
        str_cat(g_msg, "}", sizeof(g_msg));
    }
    str_cat(g_msg, ",", sizeof(g_msg));
    tile("pointers", "Links", pointers, "", "", "", 0);
    str_cat(g_msg, ",", sizeof(g_msg));
    tile("authors", "Authors", authors, "", "", "", 0);
    str_cat(g_msg, ",", sizeof(g_msg));
    tile("sync", "Synced with", peers, "peers", "", "", 0);
    str_cat(g_msg, ",", sizeof(g_msg));
    tile("hygiene", "Pruned", demoted, "", "", "", 0);
    str_cat(g_msg, ",", sizeof(g_msg));
    tile("network", "Directories", ixKnown, "", "", "", 0);
    str_cat(g_msg, "]}", sizeof(g_msg));
    send_msg(g_msg);

    /* Two switches, three states: off / auto (plugged-only) / always. */
    set_field_raw("dir_enabled", str_eq(vol, "off") ? "false" : "true");
    set_field_raw("dir_plugged", str_eq(vol, "always") ? "false" : "true");
}

static void refresh(void) {
    push_dashboard();
    push_directory();
}

static void set_pref(const char *kv) {
    hal_archive_set_pref(kv, str_len(kv));
    push_dashboard();
}

/* A switch the user flipped: read its new value, hand it to the host under the
 * name the host knows it by. */
static void toggle_from(const char *buf, const char *field, const char *key) {
    char v[8] = "";
    if (!json_raw(buf, field, v, sizeof(v))) return;
    char kv[48];
    str_copy(kv, key, sizeof(kv));
    str_cat(kv, "=", sizeof(kv));
    str_cat(kv, str_eq(v, "true") ? "1" : "0", sizeof(kv));
    set_pref(kv);
}

/* ── Module entry points ─────────────────────────────────────────────── */
int32_t module_init(void) {
    hal_log(6, "[archiver] up", 13);
    refresh();
    return 0;
}

int32_t module_tick(void) {
    refresh();
    return 0;
}

int32_t module_handle_event(void) {
    static char buf[2048];
    uint32_t n = hal_msg_recv(buf, sizeof(buf) - 1);
    if (n == 0) return 0;
    buf[n] = '\0';

    char cmd[64] = "";
    if (!json_raw(buf, "command", cmd, sizeof(cmd))) return 0;

    if (str_eq(cmd, "ready") || str_eq(cmd, "refresh")) {
        refresh();
    } else if (str_eq(cmd, "dir_enabled_changed") ||
               str_eq(cmd, "dir_plugged_changed")) {
        /* The directory offer, granted and revoked on its own: two switches
         * onto three states. Read BOTH every time -- "enabled" alone cannot
         * tell auto from always, and guessing the other one silently moves a
         * setting the owner did not touch. */
        char en[8] = "", pl[8] = "";
        json_raw(buf, "dir_enabled", en, sizeof(en));
        json_raw(buf, "dir_plugged", pl, sizeof(pl));
        const char *state = str_eq(en, "true")
                                ? (str_eq(pl, "false") ? "always" : "auto")
                                : "off";
        char kv[32];
        str_copy(kv, "volunteer=", sizeof(kv));
        str_cat(kv, state, sizeof(kv));
        hal_node_set_pref(kv, str_len(kv));
        push_directory();
    } else if (str_eq(cmd, "enabled_changed")) {
        /* Enabling takes whatever limit the picker shows; disabling means zero,
         * and zero means this device holds nothing for anybody. */
        char en[8] = "", q[16] = "5";
        json_raw(buf, "enabled", en, sizeof(en));
        json_raw(buf, "quota", q, sizeof(q));
        char kv[32];
        str_copy(kv, "quotaGb=", sizeof(kv));
        str_cat(kv, str_eq(en, "true") ? (q[0] && !str_eq(q, "0") ? q : "5") : "0",
                sizeof(kv));
        set_pref(kv);
    } else if (str_eq(cmd, "quota_changed")) {
        char q[16] = "";
        if (json_raw(buf, "quota", q, sizeof(q)) && q[0]) {
            char kv[32];
            str_copy(kv, "quotaGb=", sizeof(kv));
            str_cat(kv, q, sizeof(kv));
            set_pref(kv);
        }
    } else if (str_eq(cmd, "followed_changed")) {
        toggle_from(buf, "followed", "followed");
    } else if (str_eq(cmd, "nearby_changed")) {
        toggle_from(buf, "nearby", "fromNearby");
    } else if (str_eq(cmd, "mirror_changed")) {
        toggle_from(buf, "mirror", "mirrorSmall");
    } else if (str_eq(cmd, "free_space")) {
        /* Give back everything held for OTHER people — strangers and followed
         * authors alike. The owner's own files, and anything they pinned, are
         * not the archive's to delete. The host raises a notification saying
         * how much came back (including "nothing", which is a real outcome and
         * must not look like a dead button). */
        const char *id = "sweep:all";
        hal_archive_drop(id, str_len(id));
        push_dashboard();
    }
    return 0;
}

int32_t module_tick_interval_ms(void) { return 5000; }

int32_t module_destroy(void) { return 0; }
