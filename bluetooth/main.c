/*
 * bluetooth — the BLE street mesh's face (doc/mesh.md §12, milestone M1).
 *
 * A thin driver around two read-only host HAL calls:
 *   hal_mesh_status  → node status JSON (callsign, advertising, counters)
 *   hal_mesh_devices → devices in reach, pre-shaped as people-widget sections
 *
 * Each tick it fetches the device sections and forwards them verbatim to the
 * native `$type:"people"` list via ui.people.set — refreshing only when the
 * host's revision counter moved, so an idle street costs no UI churn. Row
 * actions come back as devices_* commands (M1: logged; messaging lands in M2).
 *
 * Build: cd wapps/bluetooth && make
 */

#include "../hal/xprs_wasm_hal.h"

/* ── String helpers ──────────────────────────────────────────────────── */
static unsigned str_len(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static int str_eq(const char *a, const char *b) { while (*a && *b && *a == *b) { a++; b++; } return *a == *b; }
static void str_copy(char *d, const char *s, unsigned m) { unsigned i = 0; while (i < m - 1 && s[i]) { d[i] = s[i]; i++; } d[i] = '\0'; }
static void str_cat(char *d, const char *s, unsigned m) { unsigned l = str_len(d); unsigned i = 0; while (l + i < m - 1 && s[i]) { d[l + i] = s[i]; i++; } d[l + i] = '\0'; }

/* Minimal scanner: find "key":<value> in flat JSON, copy the raw value. */
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
            if (instr) { if (*p == '"') break; }
            else if (*p == ',' || *p == '}') break;
            out[o++] = *p++;
        }
        out[o] = '\0';
        return 1;
    }
    return 0;
}

/* ── Buffers ─────────────────────────────────────────────────────────── */
static char g_data[32768];   /* hal_mesh_devices output (sections JSON)     */
static char g_msg[40960];    /* outbound ui.people.set wrapper              */
static char g_status[1024];  /* hal_mesh_status output                      */
static char g_scf[1024];     /* hal_mesh_scf_status output                  */
static char g_xfers[4096];   /* hal_mesh_transfers output                   */
static char g_last_rev[16] = "";

static void send_msg(const char *json) { hal_msg_send(json, str_len(json)); }

/* Append a "Node" section (status/load/custody counters) and, when any bulk
 * transfer exists, a "Transfers" section — reusing the people widget, so no
 * new UI plumbing. The device sections arrive as "[...]": we splice our
 * extra sections before the closing bracket. */
static void append_mesh_sections(void) {
    unsigned l = str_len(g_msg);
    while (l && g_msg[l - 1] != ']') l--;          /* trim to last ']' */
    if (!l) return;
    g_msg[l - 1] = '\0';                           /* drop the ']' */

    /* Node status line from hal_mesh_status + scf counters. */
    char load[16] = "?", polite[16] = "?", pend[16] = "0", spool[16] = "0";
    json_raw(g_status, "channelLoad", load, sizeof(load));
    json_raw(g_status, "politeness", polite, sizeof(polite));
    if (hal_mesh_scf_status(g_scf, sizeof(g_scf) - 1) > 0) {
        json_raw(g_scf, "inTransit", pend, sizeof(pend));
        json_raw(g_scf, "spoolPending", spool, sizeof(spool));
    }
    str_cat(g_msg, ",{\"title\":\"Node\",\"items\":[{\"id\":\"node\","
                   "\"title\":\"This node\",\"subtitle\":\"load ", sizeof(g_msg));
    str_cat(g_msg, load, sizeof(g_msg));
    str_cat(g_msg, "/s - ", sizeof(g_msg));
    str_cat(g_msg, polite, sizeof(g_msg));
    str_cat(g_msg, " - carrying ", sizeof(g_msg));
    str_cat(g_msg, pend, sizeof(g_msg));
    str_cat(g_msg, " msg / ", sizeof(g_msg));
    str_cat(g_msg, spool, sizeof(g_msg));
    str_cat(g_msg, " file\",\"tags\":[\"", sizeof(g_msg));
    str_cat(g_msg, polite, sizeof(g_msg));
    str_cat(g_msg, "\"]}]}", sizeof(g_msg));

    /* Transfers (only when non-empty; the host emits a JSON array). */
    int xn = hal_mesh_transfers(g_xfers, sizeof(g_xfers) - 1);
    if (xn > 4) {                                   /* more than "[]" */
        g_xfers[xn] = '\0';
        /* Build rows: name -> target have/size state. Cheap scan: each object
         * is flat; reuse json_raw per occurrence by splitting on '{'. */
        str_cat(g_msg, ",{\"title\":\"Transfers\",\"items\":[", sizeof(g_msg));
        int first = 1;
        for (char *p = g_xfers; *p; p++) {
            if (*p != '{') continue;
            char name[65] = "", tgt[12] = "", have[16] = "", size[16] = "", st[8] = "";
            json_raw(p, "name", name, sizeof(name));
            json_raw(p, "target", tgt, sizeof(tgt));
            json_raw(p, "have", have, sizeof(have));
            json_raw(p, "size", size, sizeof(size));
            json_raw(p, "state", st, sizeof(st));
            if (!name[0]) continue;
            if (!first) str_cat(g_msg, ",", sizeof(g_msg));
            first = 0;
            str_cat(g_msg, "{\"id\":\"x_", sizeof(g_msg));
            str_cat(g_msg, name, sizeof(g_msg));
            str_cat(g_msg, "\",\"title\":\"", sizeof(g_msg));
            str_cat(g_msg, name, sizeof(g_msg));
            str_cat(g_msg, "\",\"subtitle\":\"-> ", sizeof(g_msg));
            str_cat(g_msg, tgt, sizeof(g_msg));
            str_cat(g_msg, "  ", sizeof(g_msg));
            str_cat(g_msg, have, sizeof(g_msg));
            str_cat(g_msg, "/", sizeof(g_msg));
            str_cat(g_msg, size, sizeof(g_msg));
            str_cat(g_msg, " B\",\"tags\":[\"", sizeof(g_msg));
            str_cat(g_msg, st, sizeof(g_msg));
            str_cat(g_msg, "\"]}", sizeof(g_msg));
            /* advance past this object */
            while (*p && *p != '}') p++;
            if (!*p) break;
        }
        str_cat(g_msg, "]}", sizeof(g_msg));
    }
    str_cat(g_msg, "]", sizeof(g_msg));             /* re-close the array */
}

static void push_devices(void) {
    int n = hal_mesh_devices(g_data, sizeof(g_data));
    if (n <= 0) return;
    g_data[n] = '\0';
    str_copy(g_msg, "{\"type\":\"ui.people.set\",\"field\":\"devices\",\"sections\":",
             sizeof(g_msg));
    str_cat(g_msg, g_data, sizeof(g_msg));
    append_mesh_sections();
    str_cat(g_msg, "}", sizeof(g_msg));
    send_msg(g_msg);
}

/* Refresh the list only when the host mesh registry actually changed. */
static void tick_refresh(int force) {
    int n = hal_mesh_status(g_status, sizeof(g_status));
    if (n <= 0) return;
    g_status[n] = '\0';
    char rev[16] = "";
    json_raw(g_status, "revision", rev, sizeof(rev));
    if (!force && str_eq(rev, g_last_rev)) return;
    str_copy(g_last_rev, rev, sizeof(g_last_rev));
    push_devices();
}

/* ── Module entry points ─────────────────────────────────────────────── */
int32_t module_init(void) {
    hal_log(6, "[bluetooth] mesh view up", 24);
    tick_refresh(1);
    return 0;
}

int32_t module_tick(void) {
    /* Every 5th tick (~10 s) force a re-push even when topology is unchanged,
     * so the "seen Xs ago" freshness chips keep counting up. */
    static int n = 0;
    tick_refresh(++n % 5 == 0);
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
        tick_refresh(1);
    } else if (str_eq(cmd, "setpref")) {
        char kv[64] = "";
        if (json_raw(buf, "setpref_kv", kv, sizeof(kv)) && kv[0]) {
            hal_mesh_set_pref(kv, str_len(kv));
            tick_refresh(1);
        }
    } else if (str_eq(cmd, "message") || str_eq(cmd, "devices_tap")) {
        /* Row buttons arrive as the bare action name ("message"); row taps as
         * "<field>_tap". Both carry the row id in "devices_id". */
        /* Envelope button (or row tap): jump into the Chat wapp's 1:1
         * conversation with this callsign — the host handles the navigation. */
        char id[32] = "";
        if (json_raw(buf, "devices_id", id, sizeof(id)) && id[0] && id[0] != '#') {
            char m[128];
            str_copy(m, "{\"type\":\"mesh.message\",\"callsign\":\"", sizeof(m));
            str_cat(m, id, sizeof(m));
            str_cat(m, "\"}", sizeof(m));
            send_msg(m);
        }
    }
    return 0;
}

int32_t module_tick_interval_ms(void) { return 2000; }

void module_destroy(void) {}
