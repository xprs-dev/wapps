/*
 * Native mock HAL for the mesh wapp test. Provides just the HAL the wapp
 * uses: log, kv, msg (with an injectable inbox + an outbox capture), and the
 * three read-only Reticulum calls returning canned JSON. Compiled together with
 * main.c by run.sh — no wasm, no host.
 */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ── outbox capture (hal_msg_send) ─────────────────────────────────────── */
#define CAP_MAX 64
static char *g_cap[CAP_MAX];
static int   g_capn = 0;
void cap_clear(void) { for (int i = 0; i < g_capn; i++) free(g_cap[i]); g_capn = 0; }
int  cap_count(void) { return g_capn; }
const char *cap_at(int i) { return (i >= 0 && i < g_capn) ? g_cap[i] : ""; }
int  cap_contains(const char *s) { for (int i = 0; i < g_capn; i++) if (strstr(g_cap[i], s)) return 1; return 0; }

void hal_msg_send(const char *json, uint32_t len) {
    if (g_capn >= CAP_MAX) return;
    char *c = (char *)malloc(len + 1);
    memcpy(c, json, len); c[len] = '\0';
    g_cap[g_capn++] = c;
}

/* ── injectable inbox (hal_msg_recv) ───────────────────────────────────── */
static char g_inbox[2048];
static int  g_inbox_set = 0;
void inbox_set(const char *s) {
    strncpy(g_inbox, s, sizeof(g_inbox) - 1);
    g_inbox[sizeof(g_inbox) - 1] = '\0';
    g_inbox_set = 1;
}
uint32_t hal_msg_available(void) { return g_inbox_set ? (uint32_t)strlen(g_inbox) : 0; }
uint32_t hal_msg_recv(char *buf, uint32_t cap) {
    if (!g_inbox_set) return 0;
    uint32_t n = strlen(g_inbox); if (n > cap) n = cap;
    memcpy(buf, g_inbox, n); g_inbox_set = 0; return n;
}

void hal_log(int32_t lvl, const char *m, uint32_t n) { (void)lvl; (void)m; (void)n; }

/* ── kv store ──────────────────────────────────────────────────────────── */
#define KV_MAX 16
static char g_kvk[KV_MAX][32];
static char g_kvv[KV_MAX][128];
static int  g_kvn = 0;
uint32_t hal_kv_get(const char *k, uint32_t kl, char *out, uint32_t cap) {
    (void)kl;
    for (int i = 0; i < g_kvn; i++) {
        if (strcmp(g_kvk[i], k) == 0) {
            uint32_t n = strlen(g_kvv[i]); if (n > cap) n = cap;
            memcpy(out, g_kvv[i], n); out[n] = '\0'; return n;
        }
    }
    return 0;
}
void hal_kv_set(const char *k, uint32_t kl, const char *v, uint32_t vl) {
    (void)kl;
    for (int i = 0; i < g_kvn; i++) {
        if (strcmp(g_kvk[i], k) == 0) {
            uint32_t n = vl < 127 ? vl : 127; memcpy(g_kvv[i], v, n); g_kvv[i][n] = '\0'; return;
        }
    }
    if (g_kvn >= KV_MAX) return;
    strncpy(g_kvk[g_kvn], k, 31);
    uint32_t n = vl < 127 ? vl : 127; memcpy(g_kvv[g_kvn], v, n); g_kvv[g_kvn][n] = '\0';
    g_kvn++;
}

/* ── canned Reticulum data (the mesh wapp reads it verbatim) ─────────────────────────────────────────────── */
static char g_last_filter[256] = "";
const char *last_filter(void) { return g_last_filter; }

int32_t hal_rns_status(char *out, uint32_t cap) {
    const char *s = "{\"up\":true,\"mode\":\"tcpclient\",\"paths\":12,"
                    "\"passive\":false,\"observed\":3}";
    uint32_t n = strlen(s); if (n > cap) return -(int32_t)n;
    memcpy(out, s, n); return (int32_t)n;
}

int32_t hal_rns_hubs(char *out, uint32_t cap) {
    const char *s = "[{\"endpoint\":\"rns.beleth.net:4242\",\"connected\":true},"
                    "{\"endpoint\":\"offline.example:4242\",\"connected\":false}]";
    uint32_t n = strlen(s); if (n > cap) return -(int32_t)n;
    memcpy(out, s, n); return (int32_t)n;
}

int32_t hal_rns_nodes(const char *filter, uint32_t filter_len, char *out, uint32_t cap) {
    uint32_t fn = filter_len < sizeof(g_last_filter) - 1 ? filter_len : sizeof(g_last_filter) - 1;
    memcpy(g_last_filter, filter, fn); g_last_filter[fn] = '\0';
    const char *s =
      "{\"nodes\":["
        "{\"id\":\"self\",\"label\":\"me\",\"kind\":\"self\",\"services\":[],\"xprs\":true,\"hops\":0,\"via\":\"\",\"relayer\":\"\",\"meta\":{}},"
        "{\"id\":\"hubA\",\"label\":\"hubA\",\"kind\":\"hub\",\"services\":[],\"xprs\":false,\"hops\":1,\"via\":\"tcp\",\"relayer\":\"\",\"meta\":{\"children\":1}},"
        "{\"id\":\"leaf1\",\"label\":\"AB1CD\",\"kind\":\"leaf\",\"services\":[\"chat\",\"files\"],\"xprs\":true,\"hops\":2,\"via\":\"tcp\",\"relayer\":\"hubA\",\"meta\":{\"callsign\":\"AB1CD\"}}"
      "],\"edges\":["
        "{\"from\":\"self\",\"to\":\"hubA\",\"kind\":\"uplink\"},"
        "{\"from\":\"hubA\",\"to\":\"leaf1\",\"kind\":\"relay\"}"
      "],\"sample\":true,\"observed\":3}";
    uint32_t n = strlen(s); if (n > cap) return -(int32_t)n;
    memcpy(out, s, n); return (int32_t)n;
}
