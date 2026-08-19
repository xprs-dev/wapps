/*
 * XPRS wapp unit-test framework — runner
 *
 * Linked into tests.wasm alongside the production sources. Exports
 * module_run_tests, which the engine calls after instantiating the
 * test module. The runner walks the wapp_tests linker section,
 * executes each case, and emits one tests.case message per case
 * plus a final tests.complete summary via hal_msg_send.
 *
 * See wapps/wapp-interfaces.md §20 for the wire protocol.
 */

#include "wapp_test.h"

/* Cases auto-register into the "wapp_tests" data section via the
 * WAPP_TEST() macro. wasm-ld synthesises start/stop bounds for any
 * section whose name is a valid C identifier. The weak attribute
 * keeps tests.wasm linkable even when no tests are present (the
 * runner just reports zero cases). */
extern const WappTestCase __start_wapp_tests __attribute__((weak));
extern const WappTestCase __stop_wapp_tests  __attribute__((weak));

/* ── Tiny helpers (no libc, keep tests.wasm small) ──────────────── */

int wapp__streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

int wapp__memeq(const void *a, const void *b, unsigned n) {
    const unsigned char *p = (const unsigned char *)a;
    const unsigned char *q = (const unsigned char *)b;
    for (unsigned i = 0; i < n; i++) if (p[i] != q[i]) return 0;
    return 1;
}

unsigned wapp__itoa(long long v, char *out, unsigned cap) {
    if (cap == 0) return 0;
    unsigned o = 0;
    if (v < 0) {
        if (o + 1 < cap) out[o++] = '-';
        v = -v;
    }
    char tmp[24];
    int t = 0;
    if (v == 0) tmp[t++] = '0';
    while (v > 0 && t < (int)sizeof(tmp)) {
        tmp[t++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (t > 0 && o + 1 < cap) out[o++] = tmp[--t];
    return o;
}

static unsigned u64_to_dec(uint64_t v, char *out, unsigned cap) {
    if (cap == 0) return 0;
    char tmp[24];
    int t = 0;
    if (v == 0) tmp[t++] = '0';
    while (v > 0 && t < (int)sizeof(tmp)) {
        tmp[t++] = (char)('0' + (v % 10));
        v /= 10;
    }
    unsigned o = 0;
    while (t > 0 && o + 1 < cap) out[o++] = tmp[--t];
    return o;
}

static unsigned strcpy_into(const char *s, char *out, unsigned o,
                             unsigned cap) {
    while (s && *s && o + 1 < cap) out[o++] = *s++;
    return o;
}

/* JSON-escape a C string into out[off..]; returns updated offset. */
static unsigned json_str(const char *s, char *out, unsigned o,
                          unsigned cap) {
    if (o + 1 < cap) out[o++] = '"';
    if (!s) {
        if (o + 1 < cap) out[o++] = '"';
        return o;
    }
    while (*s && o + 6 < cap) {
        unsigned char c = (unsigned char)*s++;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c == '\r') { out[o++] = '\\'; out[o++] = 'r'; }
        else if (c == '\t') { out[o++] = '\\'; out[o++] = 't'; }
        else if (c < 0x20) {
            const char hex[] = "0123456789abcdef";
            out[o++] = '\\'; out[o++] = 'u';
            out[o++] = '0';  out[o++] = '0';
            out[o++] = hex[(c >> 4) & 0xF];
            out[o++] = hex[c & 0xF];
        } else {
            out[o++] = (char)c;
        }
    }
    if (o + 1 < cap) out[o++] = '"';
    return o;
}

/* ── Failure capture ────────────────────────────────────────────── */

void wapp_test_fail(WappTestCtx *ctx, const char *file, int line,
                    const char *msg) {
    if (!ctx || !ctx->passed) return; /* first failure wins */
    ctx->passed = 0;

    /* Trim file path to its basename for cleaner output. */
    const char *base = file;
    if (file) {
        for (const char *p = file; *p; p++) {
            if (*p == '/' || *p == '\\') base = p + 1;
        }
    }

    unsigned o = 0;
    o = strcpy_into(base, ctx->error, o, sizeof(ctx->error));
    if (o + 1 < sizeof(ctx->error)) ctx->error[o++] = ':';
    o += wapp__itoa(line, ctx->error + o, sizeof(ctx->error) - o);
    if (o + 2 < sizeof(ctx->error)) {
        ctx->error[o++] = ':'; ctx->error[o++] = ' ';
    }
    o = strcpy_into(msg, ctx->error, o, sizeof(ctx->error));
    if (o < sizeof(ctx->error)) ctx->error[o] = '\0';
    else ctx->error[sizeof(ctx->error) - 1] = '\0';
}

/* ── Suite name from __FILE__ ───────────────────────────────────── */

static unsigned derive_suite(const char *file, char *out, unsigned cap) {
    if (cap == 0) return 0;
    const char *base = file ? file : "tests";
    if (file) {
        for (const char *p = file; *p; p++) {
            if (*p == '/' || *p == '\\') base = p + 1;
        }
    }
    unsigned o = 0;
    while (o + 1 < cap && base[o] && base[o] != '.') {
        out[o] = base[o];
        o++;
    }
    out[o] = '\0';
    return o;
}

/* ── req_id passthrough ──────────────────────────────────────────
 * The engine stamps the originating tests.run req_id into the test
 * module's KV under "__tests_req_id" before calling
 * module_run_tests, so the runner can echo it back to the requester.
 */

static int32_t read_req_id(void) {
    char buf[16];
    uint32_t n = hal_kv_get("__tests_req_id", 14, buf, sizeof(buf) - 1);
    if (n == 0) return 0;
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    buf[n] = '\0';
    long long v = 0;
    int neg = 0;
    const char *p = buf;
    if (*p == '-') { neg = 1; p++; }
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    if (neg) v = -v;
    return (int32_t)v;
}

/* 1 if `name` appears as a comma-separated token in `csv`. Used to
 * skip cases the editor disabled (csv comes from KV "test.disabled"). */
static int name_in_csv(const char *name, const char *csv) {
    unsigned nl = 0; while (name[nl]) nl++;
    const char *p = csv;
    while (*p) {
        const char *start = p;
        while (*p && *p != ',') p++;
        unsigned len = (unsigned)(p - start);
        if (len == nl) {
            int eq = 1;
            for (unsigned i = 0; i < len; i++)
                if (start[i] != name[i]) { eq = 0; break; }
            if (eq) return 1;
        }
        if (*p == ',') p++;
    }
    return 0;
}

/* ── Main entry — exported as module_run_tests ──────────────────── */

__attribute__((export_name("module_run_tests")))
void module_run_tests(void) {
    char buf[768];
    char suite[80];

    int32_t req_id = read_req_id();
    int passed = 0;
    int failed = 0;
    int skipped = 0;

    /* Cases the editor disabled — CSV of names in KV "test.disabled". */
    static char disabled[512];
    uint32_t dn = hal_kv_get("test.disabled", 13, disabled, sizeof(disabled) - 1);
    if (dn >= sizeof(disabled)) dn = sizeof(disabled) - 1;
    disabled[dn] = '\0';

    uint64_t t0 = hal_time_ms();

    const WappTestCase *p   = &__start_wapp_tests;
    const WappTestCase *end = &__stop_wapp_tests;

    /* Walk by struct stride; the section is an array of WappTestCase. */
    for (; p < end; p++) {
        derive_suite(p->file, suite, sizeof(suite));

        int is_skip = disabled[0] &&
                      name_in_csv(p->name ? p->name : "?", disabled);

        WappTestCtx ctx = { .passed = 1 };
        ctx.error[0] = '\0';

        uint64_t case_t0 = hal_time_ms();
        if (!is_skip && p->fn) p->fn(&ctx);
        uint64_t case_dur = hal_time_ms() - case_t0;

        unsigned o = 0;
        o = strcpy_into("{\"type\":\"tests.case\",\"req_id\":",
                         buf, o, sizeof(buf));
        o += wapp__itoa((long long)req_id, buf + o, sizeof(buf) - o);
        o = strcpy_into(",\"suite\":", buf, o, sizeof(buf));
        o = json_str(suite, buf, o, sizeof(buf));
        o = strcpy_into(",\"name\":", buf, o, sizeof(buf));
        o = json_str(p->name ? p->name : "?", buf, o, sizeof(buf));
        o = strcpy_into(",\"passed\":", buf, o, sizeof(buf));
        o = strcpy_into((is_skip || ctx.passed) ? "true" : "false",
                         buf, o, sizeof(buf));
        o = strcpy_into(",\"skipped\":", buf, o, sizeof(buf));
        o = strcpy_into(is_skip ? "true" : "false", buf, o, sizeof(buf));
        o = strcpy_into(",\"duration_ms\":", buf, o, sizeof(buf));
        o += u64_to_dec(case_dur, buf + o, sizeof(buf) - o);
        o = strcpy_into(",\"error\":", buf, o, sizeof(buf));
        if (ctx.passed) {
            o = strcpy_into("null", buf, o, sizeof(buf));
        } else {
            o = json_str(ctx.error, buf, o, sizeof(buf));
        }
        if (o + 1 < sizeof(buf)) buf[o++] = '}';

        hal_msg_send(buf, o);

        if (is_skip)        skipped++;
        else if (ctx.passed) passed++;
        else                 failed++;
    }

    uint64_t total_dur = hal_time_ms() - t0;

    unsigned o = 0;
    o = strcpy_into("{\"type\":\"tests.complete\",\"req_id\":",
                     buf, o, sizeof(buf));
    o += wapp__itoa((long long)req_id, buf + o, sizeof(buf) - o);
    o = strcpy_into(",\"status\":0,\"passed\":", buf, o, sizeof(buf));
    o += wapp__itoa(passed, buf + o, sizeof(buf) - o);
    o = strcpy_into(",\"failed\":", buf, o, sizeof(buf));
    o += wapp__itoa(failed, buf + o, sizeof(buf) - o);
    o = strcpy_into(",\"skipped\":", buf, o, sizeof(buf));
    o += wapp__itoa(skipped, buf + o, sizeof(buf) - o);
    o = strcpy_into(",\"duration_ms\":", buf, o, sizeof(buf));
    o += u64_to_dec(total_dur, buf + o, sizeof(buf) - o);
    o = strcpy_into(",\"error\":null}", buf, o, sizeof(buf));

    hal_msg_send(buf, o);
}

/* tests.wasm exports only module_run_tests. The engine instantiates
 * the test module, calls module_run_tests, drains the outbox, and
 * disposes — there is no init/tick/handle_event lifecycle on a
 * test build. If a wapp `#include "../main.c"` from a test file,
 * main.c's lifecycle functions get linked but are dead-stripped
 * since nothing exports them in this build. */
