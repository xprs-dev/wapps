/*
 * mesh — visualize and manage the mesh: Reticulum and XPRS.
 *
 * Formerly the "reticulum" wapp. The graph the host renders now carries both
 * halves of the street: Reticulum nodes/hubs (from the observed announce
 * registry) and XPRS stations heard over the air (kind "xprs", merged
 * host-side into the same {nodes,edges} snapshot).
 *
 * A thin driver around three read-only host HAL calls:
 *   hal_rns_status  → node status JSON
 *   hal_rns_nodes   → the observed network as {nodes,edges} (filtered)
 *   hal_rns_hubs    → configured bootstrap hubs [{endpoint,connected}]
 *
 * The graph itself is rendered by the host's native `$type:"graph"` widget
 * (a 3D view built on the graph3d engine). This module does NO graph math:
 * each tick it fetches the (filtered) snapshot and forwards it verbatim via
 * `ui.graph.set`, and forwards the widget's interactions (filter changes)
 * back to the host. Bootstrap-hub management and the passive toggle are
 * emitted as host-action messages (rns.hub.* / rns.passive.set) from the
 * Hubs/Settings screens.
 *
 * Build: cd wapps/mesh && make
 */

#include "../hal/xprs_wasm_hal.h"

/* ── String helpers ──────────────────────────────────────────────────── */
static unsigned str_len(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static int str_eq(const char *a, const char *b) { while (*a && *b && *a == *b) { a++; b++; } return *a == *b; }
static void str_copy(char *d, const char *s, unsigned m) { unsigned i = 0; while (i < m - 1 && s[i]) { d[i] = s[i]; i++; } d[i] = '\0'; }
static void str_cat(char *d, const char *s, unsigned m) { unsigned l = str_len(d); unsigned i = 0; while (l + i < m - 1 && s[i]) { d[l + i] = s[i]; i++; } d[l + i] = '\0'; }

/* Does NUL-terminated [s] begin with [pfx]? */
static int starts_with(const char *s, const char *pfx) {
    while (*pfx) { if (*s != *pfx) return 0; s++; pfx++; }
    return 1;
}

/* Append a JSON-escaped copy of [s] into [d] (bounded by [m]). */
static void json_cat_escaped(char *d, const char *s, unsigned m) {
    unsigned l = str_len(d);
    for (unsigned i = 0; s[i] && l < m - 2; i++) {
        char c = s[i];
        if (c == '"' || c == '\\') { if (l < m - 3) { d[l++] = '\\'; d[l++] = c; } }
        else if (c == '\n') { if (l < m - 3) { d[l++] = '\\'; d[l++] = 'n'; } }
        else if ((unsigned char)c < 0x20) { /* drop other control chars */ }
        else { d[l++] = c; }
    }
    d[l] = '\0';
}

/* ── Filter state (page-driven, persisted) ───────────────────────────── */
static int  g_xprs_only = 0;       /* show only xprs-software nodes  */
static char g_service[32]  = "";      /* show only nodes with this service */
static char g_search[64]   = "";      /* substring match on label/id/svc   */
static int  g_ready = 0;              /* the page has loaded once          */

static void load_state(void) {
    char buf[8];
    if (hal_kv_get("geo", 3, buf, sizeof(buf) - 1) > 0) g_xprs_only = (buf[0] == '1');
    hal_kv_get("svc", 3, g_service, sizeof(g_service) - 1);
    hal_kv_get("q", 1, g_search, sizeof(g_search) - 1);
}

static void save_state(void) {
    char b[2]; b[0] = g_xprs_only ? '1' : '0'; b[1] = '\0';
    hal_kv_set("geo", 3, b, 1);
    hal_kv_set("svc", 3, g_service, str_len(g_service));
    hal_kv_set("q", 1, g_search, str_len(g_search));
}

/* ── Buffers ─────────────────────────────────────────────────────────── *
 * The graph JSON can be large on a busy hub. 80KB holds many hundreds of
 * nodes; the xprs-only default keeps it far smaller. The host returns the
 * NEGATED required size if it doesn't fit — we then log and skip that frame. */
static char g_data[81920];            /* hal_rns_* output                   */
static char g_msg[82432];             /* outbound ui.webview.send wrapper    */

/* Forward an arbitrary host message (already-built JSON) to the host. */
static void send_msg(const char *json) { hal_msg_send(json, str_len(json)); }

/* ── Graph ───────────────────────────────────────────────────────────── *
 * Fetch the (filtered) {nodes,edges} snapshot and forward it verbatim to the
 * host's native `$type:"graph"` widget as {"type":"ui.graph.set","payload":…}.
 * No graph math here — the host lays it out (off the main thread) and paints. */
static void build_filter(char *out, unsigned m) {
    str_copy(out, "{\"xprsOnly\":", m);
    str_cat(out, g_xprs_only ? "true" : "false", m);
    str_cat(out, ",\"service\":\"", m);
    json_cat_escaped(out, g_service, m);
    str_cat(out, "\",\"search\":\"", m);
    json_cat_escaped(out, g_search, m);
    str_cat(out, "\"}", m);
}

static void push_graph(void) {
    char filter[160];
    build_filter(filter, sizeof(filter));
    int n = hal_rns_nodes(filter, str_len(filter), g_data, sizeof(g_data));
    if (n < 0) {
        hal_log(3, "[mesh] graph too big for buffer — narrow the filter", 53);
        return;
    }
    if (n == 0) return;
    g_data[n] = '\0';
    str_copy(g_msg, "{\"type\":\"ui.graph.set\",\"payload\":", sizeof(g_msg));
    str_cat(g_msg, g_data, sizeof(g_msg));
    str_cat(g_msg, "}", sizeof(g_msg));
    send_msg(g_msg);
}

/* ── Hubs (host-native list) ─────────────────────────────────────────── *
 * hal_rns_hubs returns [{"endpoint":"h:p","connected":bool},...]. Forward it
 * verbatim to the host's native bootstrap-manager panel — no parsing or markers
 * here (UTF-8 from the wapp gets mangled in transit; the host renders the dots). */
static void push_hubs(void) {
    int n = hal_rns_hubs(g_data, sizeof(g_data));
    if (n <= 0) return;
    g_data[n] = '\0';
    str_copy(g_msg, "{\"type\":\"ui.graph.hubs\",\"payload\":", sizeof(g_msg));
    str_cat(g_msg, g_data, sizeof(g_msg));
    str_cat(g_msg, "}", sizeof(g_msg));
    send_msg(g_msg);
}

/* Push everything the UI needs for a fresh frame. */
static void push_all(void) {
    push_graph();
    push_hubs();
}

/* ── Incoming-event helpers ──────────────────────────────────────────── */
/* Extract a string value for "key":"..." from [buf] into [out]. Returns 1 if
 * found. */
static int json_str(const char *buf, const char *key, char *out, unsigned m) {
    char pat[48];
    str_copy(pat, "\"", sizeof(pat));
    str_cat(pat, key, sizeof(pat));
    str_cat(pat, "\":\"", sizeof(pat));
    const char *p = buf;
    unsigned pl = str_len(pat);
    while (*p) {
        if (starts_with(p, pat)) {
            p += pl;
            unsigned i = 0;
            while (*p && *p != '"' && i < m - 1) {
                if (*p == '\\' && *(p + 1)) p++;
                out[i++] = *p++;
            }
            out[i] = '\0';
            return 1;
        }
        p++;
    }
    out[0] = '\0';
    return 0;
}

/* Read a boolean "key":true/false. Returns the value, or [dflt] if absent. */
static int json_bool(const char *buf, const char *key, int dflt) {
    char pat[48];
    str_copy(pat, "\"", sizeof(pat));
    str_cat(pat, key, sizeof(pat));
    str_cat(pat, "\":", sizeof(pat));
    const char *p = buf;
    unsigned pl = str_len(pat);
    while (*p) {
        if (starts_with(p, pat)) {
            p += pl;
            return (*p == 't') ? 1 : 0;
        }
        p++;
    }
    return dflt;
}

/* Emit a bootstrap-hub host action: {"type":"rns.hub.<verb>","endpoint":ep}. */
static void hub_action(const char *verb, const char *ep) {
    if (ep[0] == '\0') return;
    char m[160];
    str_copy(m, "{\"type\":\"rns.hub.", sizeof(m));
    str_cat(m, verb, sizeof(m));
    str_cat(m, "\",\"endpoint\":\"", sizeof(m));
    json_cat_escaped(m, ep, sizeof(m));
    str_cat(m, "\"}", sizeof(m));
    send_msg(m);
}

/* Emit a 1:1 LXMF send host action from a graph "node_message" command. The
 * page hands us the target's public key (meta.pubkey) plus the composed title +
 * body; the host derives the LXMF delivery dest from the key and sends. Built
 * into the large g_msg buffer since the body can be a few KB. */
static void lxmf_send_action(const char *full) {
    static char pubkey[160];
    static char title[256];
    static char content[3600];
    json_str(full, "pubkey", pubkey, sizeof(pubkey));
    json_str(full, "title", title, sizeof(title));
    json_str(full, "content", content, sizeof(content));
    if (pubkey[0] == '\0' || content[0] == '\0') return;
    str_copy(g_msg, "{\"type\":\"rns.lxmf.send\",\"pubkey\":\"", sizeof(g_msg));
    json_cat_escaped(g_msg, pubkey, sizeof(g_msg));
    str_cat(g_msg, "\",\"title\":\"", sizeof(g_msg));
    json_cat_escaped(g_msg, title, sizeof(g_msg));
    str_cat(g_msg, "\",\"content\":\"", sizeof(g_msg));
    json_cat_escaped(g_msg, content, sizeof(g_msg));
    str_cat(g_msg, "\"}", sizeof(g_msg));
    send_msg(g_msg);
}

/* ── Command dispatch ────────────────────────────────────────────────── */
static void handle_command(const char *cmd, const char *full) {
    /* Page interactions (posted via window.Host.postMessage). */
    if (str_eq(cmd, "ready")) {
        g_ready = 1;
        push_all();
        return;
    }
    if (str_eq(cmd, "graph_filter")) {
        g_xprs_only = json_bool(full, "xprsOnly", g_xprs_only);
        json_str(full, "service", g_service, sizeof(g_service));
        json_str(full, "search", g_search, sizeof(g_search));
        save_state();
        push_graph();
        return;
    }
    /* Bootstrap-hub management (GeoUI actions carry the field bundle in
     * "fields", e.g. {"hub_endpoint":"host:port"}). */
    if (str_eq(cmd, "hub_add") || str_eq(cmd, "hub_connect") ||
        str_eq(cmd, "hub_disconnect") || str_eq(cmd, "hub_remove")) {
        char ep[96];
        json_str(full, "hub_endpoint", ep, sizeof(ep));
        const char *verb = str_eq(cmd, "hub_add") ? "add"
                         : str_eq(cmd, "hub_connect") ? "connect"
                         : str_eq(cmd, "hub_disconnect") ? "disconnect"
                         : "remove";
        hub_action(verb, ep);
        /* Reflect the change quickly. */
        push_hubs();
        return;
    }
    /* 1:1 message to an observed node, picked in the graph detail panel. */
    if (str_eq(cmd, "node_message")) {
        lxmf_send_action(full);
        return;
    }
    /* Passive (relay-shedding) toggle from Settings. */
    if (str_eq(cmd, "apply_settings")) {
        int passive = json_bool(full, "passive", 0);
        char m[64];
        str_copy(m, "{\"type\":\"rns.passive.set\",\"value\":", sizeof(m));
        str_cat(m, passive ? "true" : "false", sizeof(m));
        str_cat(m, "}", sizeof(m));
        send_msg(m);
        return;
    }
    if (str_eq(cmd, "refresh")) {
        push_all();
        return;
    }
}

/* ── Module entry points ─────────────────────────────────────────────── */
void module_init(void) {
    hal_log(1, "[mesh] init", 11);
    load_state();
}

void module_tick(void) {
    /* Stream a fresh frame. Before the page signals "ready" we still refresh
     * the host-native Hubs list + status so those tabs are live immediately;
     * the graph push is cheap and harmless (buffered host-side until ready). */
    push_all();
}

void module_handle_event(void) {
    /* Big enough to hold a composed 1:1 message (node_message) plus its JSON
     * envelope — caps a DM body at ~3.5 KB. Other events are tiny. */
    static char buf[4096];
    if (hal_msg_available() == 0) return;
    uint32_t n = hal_msg_recv(buf, sizeof(buf) - 1);
    if (n == 0) return;
    buf[n] = '\0';

    char cmd[64];
    if (json_str(buf, "command", cmd, sizeof(cmd))) {
        handle_command(cmd, buf);
    }
}

void module_destroy(void) {
    save_state();
    hal_log(1, "[mesh] destroy", 14);
}

uint32_t module_tick_interval_ms(void) { return 2000; }
