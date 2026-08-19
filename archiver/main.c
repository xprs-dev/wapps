/*
 * archiver — keep copies of other people's posts and files, so they survive the
 * device that made them going offline.
 *
 * Two rules are the whole contract:
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
    push_dashboard();
    return 0;
}

int32_t module_tick(void) {
    push_dashboard();
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
        push_dashboard();
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
