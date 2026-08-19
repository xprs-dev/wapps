/*
 * wallet — XPRS Wapp: hold and transfer participation coins from different
 * providers.
 *
 * This module is a thin bridge: the real coin logic lives host-side in
 * lib/wapp/coin/coin_host_bridge.dart (using the participation-coin library).
 * Here we just translate GeoUI commands into coin.* messages and ask the host
 * to refresh the list. The host pushes back the field updates (coins list,
 * status line, the send token) that the screens render.
 *
 * Commands (from screens/home.ui.json action buttons):
 *   add      -> coin.add     {descriptor: add_descriptor}
 *   receive  -> coin.receive {token: recv_token}
 *   send     -> coin.send    {coinId: send_coin, to: send_to, amount: send_amount}
 *
 * Build: cd wapps/wallet && make   (or build-archive.sh wallet)
 */

#include "../hal/xprs_wasm_hal.h"

/* ── tiny string helpers (no libc) ───────────────────────────────────── */

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

/* Append [s] as escaped JSON string content (between the surrounding quotes). */
static void put_escaped(char *buf, unsigned *len, unsigned cap, const char *s) {
    unsigned l = *len;
    for (unsigned i = 0; s[i] && l < cap - 2; i++) {
        char c = s[i];
        if (c == '"')        { buf[l++] = '\\'; buf[l++] = '"'; }
        else if (c == '\\')  { buf[l++] = '\\'; buf[l++] = '\\'; }
        else if (c == '\n')  { buf[l++] = '\\'; buf[l++] = 'n'; }
        else if ((unsigned char)c < 0x20) { /* drop control chars */ }
        else                 { buf[l++] = c; }
    }
    *len = l;
}

/* Find "key":"value" (or "key":<number>) anywhere in [obj]; copy value out. */
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

/* ── outbound coin.* messages ────────────────────────────────────────── */

static char g_out[8192];
static char g_val[6144];

static void send_list(void) {
    const char *m = "{\"type\":\"coin.list\"}";
    hal_msg_send(m, slen(m));
}

/* {"type":"<type>","<key>":"<value>"} */
static void emit_kv(const char *type, const char *key, const char *value) {
    unsigned len = 0;
    put_raw(g_out, &len, sizeof(g_out), "{\"type\":\"");
    put_raw(g_out, &len, sizeof(g_out), type);
    put_raw(g_out, &len, sizeof(g_out), "\",\"");
    put_raw(g_out, &len, sizeof(g_out), key);
    put_raw(g_out, &len, sizeof(g_out), "\":\"");
    put_escaped(g_out, &len, sizeof(g_out), value);
    put_raw(g_out, &len, sizeof(g_out), "\"}");
    hal_msg_send(g_out, len);
}

static void emit_send(const char *coin, const char *to, const char *amount) {
    unsigned len = 0;
    put_raw(g_out, &len, sizeof(g_out), "{\"type\":\"coin.send\",\"coinId\":\"");
    put_escaped(g_out, &len, sizeof(g_out), coin);
    put_raw(g_out, &len, sizeof(g_out), "\",\"to\":\"");
    put_escaped(g_out, &len, sizeof(g_out), to);
    put_raw(g_out, &len, sizeof(g_out), "\",\"amount\":\"");
    put_escaped(g_out, &len, sizeof(g_out), amount);
    put_raw(g_out, &len, sizeof(g_out), "\"}");
    hal_msg_send(g_out, len);
}

/* ── module entry points ─────────────────────────────────────────────── */

void module_init(void) {
    hal_log(1, "[wallet] init", 13);
    send_list();
}

void module_tick(void) { /* event-driven; nothing to do */ }

void module_destroy(void) { /* nothing to clean up */ }

/* Event-driven: the wallet does no periodic work. Use a long idle interval
 * (the host arms a Timer with this value, and 0 would busy-loop). */
uint32_t module_tick_interval_ms(void) { return 60000; }

void module_handle_event(void) {
    char buf[8192];
    if (hal_msg_available() == 0) return;
    uint32_t n = hal_msg_recv(buf, sizeof(buf) - 1);
    if (n == 0) return;
    buf[n] = '\0';

    char cmd[32];
    if (!find_str(buf, "command", cmd, sizeof(cmd))) { send_list(); return; }

    if (seq(cmd, "add")) {
        if (find_str(buf, "add_descriptor", g_val, sizeof(g_val)))
            emit_kv("coin.add", "descriptor", g_val);
        else
            send_list();
    } else if (seq(cmd, "receive")) {
        if (find_str(buf, "recv_token", g_val, sizeof(g_val)))
            emit_kv("coin.receive", "token", g_val);
        else
            send_list();
    } else if (seq(cmd, "send")) {
        char coin[160], to[160], amount[24];
        find_str(buf, "send_coin", coin, sizeof(coin));
        find_str(buf, "send_to", to, sizeof(to));
        find_str(buf, "send_amount", amount, sizeof(amount));
        emit_send(coin, to, amount);
    } else if (seq(cmd, "coins_tap")) {
        /* A coin row was tapped: fetch its details and open the Coin screen. */
        if (find_str(buf, "coins_id", g_val, sizeof(g_val))) {
            emit_kv("coin.detail", "coinId", g_val);
            emit_kv("ui.screen.open", "name", "Coin");
        }
    } else {
        send_list();
    }
}
