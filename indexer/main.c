/*
 * indexer — volunteer this device as an Indexer, and watch what the network
 * does with the offer.
 *
 * An Indexer tells the network which devices hold the posts and files of each
 * npub: signed pointers (~176 bytes) from data hash / author key to the devices
 * that have them. Addresses, never the content — one that vanishes costs the
 * network a directory, not an archive.
 *
 * The screen is a DASHBOARD first, because a role nobody can inspect is a role
 * nobody trusts: pointers held, queries answered per hour (with the 48-hour
 * shape, not just a lifetime total), sync activity, hygiene. Then the two
 * decisions the owner actually makes — volunteer on/off, and what they are
 * comfortable indexing — and previewed maintenance that removes exactly what it
 * said it would.
 *
 * Host HAL:
 *   hal_node_status   → JSON: serving, pointers, authors, query rates, spark…
 *   hal_node_maint    → JSON tiles: previewed sweeps (id, label, −N)
 *   hal_node_set_pref → volunteer=off|auto|always, topics=csv
 *   hal_node_sweep    → run a previewed sweep by tile id
 *
 * Build: cd wapps/indexer && make
 */

#include "../hal/xprs_wasm_hal.h"

/* ── String helpers ──────────────────────────────────────────────────── */
static unsigned str_len(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static int str_eq(const char *a, const char *b) { while (*a && *b && *a == *b) { a++; b++; } return *a == *b; }
static void str_copy(char *d, const char *s, unsigned m) { unsigned i = 0; while (i < m - 1 && s[i]) { d[i] = s[i]; i++; } d[i] = '\0'; }
static void str_cat(char *d, const char *s, unsigned m) { unsigned l = str_len(d); unsigned i = 0; while (l + i < m - 1 && s[i]) { d[l + i] = s[i]; i++; } d[l + i] = '\0'; }

/* Minimal scanner: find "key":<value> in flat JSON, copy the raw value.
 * For arrays ("key":[...]) it copies up to the matching ']'. */
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
static char g_msg[24576];
static char g_spark[1024];

static void send_msg(const char *json) { hal_msg_send(json, str_len(json)); }

/* Set a field to a RAW JSON value (true/false/number) — bool switches need a
 * real bool, not the string "true". */
static void set_field_raw(const char *name, const char *raw) {
    str_copy(g_msg, "{\"type\":\"ui.field.set\",\"field\":\"", sizeof(g_msg));
    str_cat(g_msg, name, sizeof(g_msg));
    str_cat(g_msg, "\",\"value\":", sizeof(g_msg));
    str_cat(g_msg, raw, sizeof(g_msg));
    str_cat(g_msg, "}", sizeof(g_msg));
    send_msg(g_msg);
}

/* Append one stat tile to g_msg (comma handling is the caller's). */
static void tile(const char *id, const char *label, const char *value,
                 const char *unit, const char *hint, int alert) {
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
    if (alert) str_cat(g_msg, ",\"alert\":true", sizeof(g_msg));
    str_cat(g_msg, "}", sizeof(g_msg));
}

/* The dashboard: the numbers first, and the SHAPE of the traffic, because a
 * lifetime total tells the owner nothing about whether anyone came today. */
static void push_dashboard(void) {
    int n = hal_node_status(g_status, sizeof(g_status) - 1);
    if (n <= 0) return;
    g_status[n] = '\0';

    char vol[16] = "auto", serving[8] = "false";
    char pointers[16] = "0", authors[16] = "0", peers[16] = "0";
    char demoted[16] = "0", rejected[16] = "0";
    char qLast[16] = "0", qAvg[16] = "0";
    char applied[16] = "0", removed[16] = "0", exch[16] = "0";
    char topics[256] = "";
    char wide[8] = "false";
    char ixKnown[16] = "0", peersKnown[16] = "0";
    json_raw(g_status, "volunteer", vol, sizeof(vol));
    json_raw(g_status, "serving", serving, sizeof(serving));
    json_raw(g_status, "pointers", pointers, sizeof(pointers));
    json_raw(g_status, "authors", authors, sizeof(authors));
    json_raw(g_status, "syncPeers", peers, sizeof(peers));
    json_raw(g_status, "demoted", demoted, sizeof(demoted));
    json_raw(g_status, "rejected", rejected, sizeof(rejected));
    json_raw(g_status, "queriesLastHour", qLast, sizeof(qLast));
    json_raw(g_status, "queriesAvgPerHour", qAvg, sizeof(qAvg));
    json_raw(g_status, "querySpark", g_spark, sizeof(g_spark));
    json_raw(g_status, "syncApplied", applied, sizeof(applied));
    json_raw(g_status, "syncRemoved", removed, sizeof(removed));
    json_raw(g_status, "syncExchanges", exch, sizeof(exch));
    json_raw(g_status, "topicsCsv", topics, sizeof(topics));
    json_raw(g_status, "wideActive", wide, sizeof(wide));
    json_raw(g_status, "indexersKnown", ixKnown, sizeof(ixKnown));
    json_raw(g_status, "peersKnown", peersKnown, sizeof(peersKnown));

    int on = str_eq(serving, "true");

    str_copy(g_msg, "{\"type\":\"ui.stats.set\",\"field\":\"dashboard\",\"tiles\":[", sizeof(g_msg));

    /* Queries per hour first: the one number that says whether the offer is
     * being used. Wide tile, 48h sparkline. */
    {
        char hint[64];
        str_copy(hint, qLast, sizeof(hint));
        str_cat(hint, " in the last hour", sizeof(hint));
        str_cat(g_msg, "{\"id\":\"rate\",\"label\":\"Queries per hour\",\"value\":\"", sizeof(g_msg));
        str_cat(g_msg, qAvg, sizeof(g_msg));
        str_cat(g_msg, "\",\"unit\":\"avg\",\"hint\":\"", sizeof(g_msg));
        str_cat(g_msg, hint, sizeof(g_msg));
        str_cat(g_msg, "\",\"spark\":", sizeof(g_msg));
        str_cat(g_msg, g_spark[0] ? g_spark : "[]", sizeof(g_msg));
        str_cat(g_msg, "}", sizeof(g_msg));
    }
    str_cat(g_msg, ",", sizeof(g_msg));
    tile("pointers", "Links", pointers, "", "", 0);
    str_cat(g_msg, ",", sizeof(g_msg));
    tile("authors", "Authors", authors, "", "", 0);
    str_cat(g_msg, ",", sizeof(g_msg));
    tile("sync", "Synced with", peers, "indexers", "", 0);
    str_cat(g_msg, ",", sizeof(g_msg));
    tile("hygiene", "Pruned", demoted, "", "", 0);
    str_cat(g_msg, ",", sizeof(g_msg));
    tile("network", "Indexers", ixKnown, "", "", 0);

    str_cat(g_msg, "]}", sizeof(g_msg));
    send_msg(g_msg);

    /* The two switches reflect the real state: enabled = not off;
     * plugged-only = the auto mode. */
    set_field_raw("enabled", str_eq(vol, "off") ? "false" : "true");
    set_field_raw("plugged", str_eq(vol, "always") ? "false" : "true");
    (void)on; (void)wide; (void)topics; (void)applied; (void)removed;
    (void)exch; (void)rejected; (void)peersKnown;
}

static void refresh(void) {
    push_dashboard();
}

/* ── Module entry points ─────────────────────────────────────────────── */
int32_t module_init(void) {
    hal_log(6, "[indexer] up", 12);
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
    } else if (str_eq(cmd, "enabled_changed") || str_eq(cmd, "plugged_changed")) {
        /* Two switches map onto the three states: off / auto (plugged-only) /
         * always. Read both, set exactly what they mean. */
        char en[8] = "", pl[8] = "";
        json_raw(buf, "enabled", en, sizeof(en));
        json_raw(buf, "plugged", pl, sizeof(pl));
        const char *state = str_eq(en, "true")
                                ? (str_eq(pl, "false") ? "always" : "auto")
                                : "off";
        char kv[32];
        str_copy(kv, "volunteer=", sizeof(kv));
        str_cat(kv, state, sizeof(kv));
        hal_node_set_pref(kv, str_len(kv));
        refresh();
    }
    return 0;
}

/* Counters, not a feed. Five seconds is plenty. */
int32_t module_tick_interval_ms(void) { return 5000; }

int32_t module_destroy(void) { return 0; }
