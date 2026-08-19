/*
 * atm — XPRS Wapp: operate a participation coin's blockchain and faucet.
 *
 * Thin bridge: the real logic lives host-side in
 * lib/wapp/coin/atm_host_bridge.dart (using the participation-coin library).
 * This module turns GeoUI commands into atm.* messages; the host runs the
 * coin's ledger and pushes back the field updates (coin list, status, npub,
 * descriptor) the screens render.
 *
 * Commands (screens/home.ui.json):
 *   create     -> atm.create     {name, symbol, exp, reward}
 *   faucet     -> atm.faucet     {coinId: atm_coin, to: atm_to, amount: atm_amount}
 *   addatm     -> atm.addatm     {coinId: atm_coin, npub: atm_trust}
 *   descriptor -> atm.descriptor {coinId: atm_coin}
 *
 * Build: cd wapps/atm && make
 */

#include "../hal/xprs_wasm_hal.h"

static unsigned slen(const char *s) {
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}

static int seq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static void put_raw(char *buf, unsigned *len, unsigned cap, const char *s) {
    unsigned l = *len;
    for (unsigned i = 0; s[i] && l < cap - 1; i++) buf[l++] = s[i];
    *len = l;
}

static void put_escaped(char *buf, unsigned *len, unsigned cap, const char *s) {
    unsigned l = *len;
    for (unsigned i = 0; s[i] && l < cap - 2; i++) {
        char c = s[i];
        if (c == '"')        { buf[l++] = '\\'; buf[l++] = '"'; }
        else if (c == '\\')  { buf[l++] = '\\'; buf[l++] = '\\'; }
        else if (c == '\n')  { buf[l++] = '\\'; buf[l++] = 'n'; }
        else if ((unsigned char)c < 0x20) { /* drop */ }
        else                 { buf[l++] = c; }
    }
    *len = l;
}

static int find_str(const char *obj, const char *key, char *val,
                    unsigned val_len) {
    unsigned klen = slen(key);
    val[0] = '\0';
    for (const char *p = obj; *p; p++) {
        if (*p != '"') continue;
        int match = 1;
        for (unsigned i = 0; i < klen; i++) {
            if (p[1 + i] != key[i]) { match = 0; break; }
        }
        if (!match || p[1 + klen] != '"') continue;
        const char *q = p + 2 + klen;
        while (*q && *q != ':') q++;
        if (!*q) return 0;
        q++;
        while (*q == ' ' || *q == '\t') q++;
        if (*q == '"') {
            q++;
            unsigned vi = 0;
            while (*q && *q != '"' && vi < val_len - 1) val[vi++] = *q++;
            val[vi] = '\0';
        } else {
            unsigned vi = 0;
            while (*q && *q != ',' && *q != '}' && *q != ' ' && vi < val_len - 1)
                val[vi++] = *q++;
            val[vi] = '\0';
        }
        return 1;
    }
    return 0;
}

static char g_out[8192];

static void send_plain(const char *type) {
    unsigned len = 0;
    put_raw(g_out, &len, sizeof(g_out), "{\"type\":\"");
    put_raw(g_out, &len, sizeof(g_out), type);
    put_raw(g_out, &len, sizeof(g_out), "\"}");
    hal_msg_send(g_out, len);
}

/* Append a ,"key":"value" pair (escaped) into g_out at *len. */
static void put_field(unsigned *len, const char *key, const char *val) {
    put_raw(g_out, len, sizeof(g_out), ",\"");
    put_raw(g_out, len, sizeof(g_out), key);
    put_raw(g_out, len, sizeof(g_out), "\":\"");
    put_escaped(g_out, len, sizeof(g_out), val);
    put_raw(g_out, len, sizeof(g_out), "\"");
}

static void begin(unsigned *len, const char *type) {
    *len = 0;
    put_raw(g_out, len, sizeof(g_out), "{\"type\":\"");
    put_raw(g_out, len, sizeof(g_out), type);
    put_raw(g_out, len, sizeof(g_out), "\"");
}

void module_init(void) {
    hal_log(1, "[atm] init", 10);
    send_plain("atm.list");
    send_plain("atm.npub");
}

void module_tick(void) { }
void module_destroy(void) { }
uint32_t module_tick_interval_ms(void) { return 60000; }

void module_handle_event(void) {
    char buf[8192];
    if (hal_msg_available() == 0) return;
    uint32_t n = hal_msg_recv(buf, sizeof(buf) - 1);
    if (n == 0) return;
    buf[n] = '\0';

    char cmd[24];
    if (!find_str(buf, "command", cmd, sizeof(cmd))) { send_plain("atm.list"); return; }

    static char a[4096];
    char b[200], c[200];
    unsigned len;

    if (seq(cmd, "create")) {
        begin(&len, "atm.create");
        find_str(buf, "atm_singular", a, sizeof(a)); put_field(&len, "singular", a);
        find_str(buf, "atm_plural", a, sizeof(a));   put_field(&len, "plural", a);
        find_str(buf, "atm_code", a, sizeof(a));     put_field(&len, "code", a);
        find_str(buf, "atm_symbol", a, sizeof(a));   put_field(&len, "symbol", a);
        find_str(buf, "atm_desc", a, sizeof(a));     put_field(&len, "desc", a);
        find_str(buf, "atm_picture", a, sizeof(a));  put_field(&len, "picture", a);
        find_str(buf, "atm_exp", a, sizeof(a));      put_field(&len, "exp", a);
        put_raw(g_out, &len, sizeof(g_out), "}");
        hal_msg_send(g_out, len);
    } else if (seq(cmd, "faucet")) {
        begin(&len, "atm.faucet");
        find_str(buf, "atm_coin", a, sizeof(a));   put_field(&len, "coinId", a);
        find_str(buf, "atm_to", b, sizeof(b));     put_field(&len, "to", b);
        find_str(buf, "atm_amount", c, sizeof(c)); put_field(&len, "amount", c);
        put_raw(g_out, &len, sizeof(g_out), "}");
        hal_msg_send(g_out, len);
    } else if (seq(cmd, "addatm")) {
        begin(&len, "atm.addatm");
        find_str(buf, "atm_coin", a, sizeof(a));   put_field(&len, "coinId", a);
        find_str(buf, "atm_trust", b, sizeof(b));  put_field(&len, "npub", b);
        put_raw(g_out, &len, sizeof(g_out), "}");
        hal_msg_send(g_out, len);
    } else if (seq(cmd, "descriptor")) {
        begin(&len, "atm.descriptor");
        find_str(buf, "atm_coin", a, sizeof(a));   put_field(&len, "coinId", a);
        put_raw(g_out, &len, sizeof(g_out), "}");
        hal_msg_send(g_out, len);
    } else {
        send_plain("atm.list");
    }
}
