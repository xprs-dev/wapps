/*
 * XPRS wapp unit-test framework — header
 *
 * Each test_*.c file in a wapp's tests/ folder includes this header,
 * declares cases with WAPP_TEST(name) { ... }, and uses the
 * WAPP_EXPECT_* macros to assert. Cases self-register at link time
 * via a custom "wapp_tests" section the runner walks.
 *
 * See wapps/wapp-interfaces.md §20 for the full spec.
 */

#ifndef WAPP_TEST_H
#define WAPP_TEST_H

#include <stdint.h>
#include <string.h>
#include "xprs_wasm_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Per-case result populated by WAPP_EXPECT_* on failure. */
typedef struct WappTestCtx {
    int  passed;        /* 1 = no failure yet, 0 = first failure recorded */
    char error[256];    /* "<file>:<line>: <message>" — empty when passed */
} WappTestCtx;

typedef void (*WappTestFn)(WappTestCtx *ctx);

/* One registered case. The runner walks an array of these placed in
 * the "wapp_tests" section by the WAPP_TEST macro. */
typedef struct WappTestCase {
    const char *name;   /* case name as written in WAPP_TEST(name) */
    const char *file;   /* __FILE__ at WAPP_TEST site */
    int         line;   /* __LINE__ at WAPP_TEST site */
    WappTestFn  fn;     /* the case body */
} WappTestCase;

/* Internal: record a failure with __FILE__/__LINE__ and a message. */
void wapp_test_fail(WappTestCtx *ctx, const char *file, int line,
                    const char *msg);

/* Internal helpers — stripped, no libc dependency. */
int  wapp__streq(const char *a, const char *b);
int  wapp__memeq(const void *a, const void *b, unsigned n);
unsigned wapp__itoa(long long v, char *out, unsigned cap);

/* ── Public macros ──────────────────────────────────────────────── */

/*
 * WAPP_TEST(name) { body }
 * Defines and registers a test case named `name`. The body runs as
 * a function; pass by reaching the end without a failed expectation.
 */
#define WAPP_TEST(case_name)                                              \
    static void wapp__case_##case_name(WappTestCtx *ctx);                 \
    __attribute__((used, retain, section("wapp_tests")))                  \
    static const WappTestCase wapp__entry_##case_name = {                 \
        .name = #case_name,                                               \
        .file = __FILE__,                                                 \
        .line = __LINE__,                                                 \
        .fn   = wapp__case_##case_name,                                   \
    };                                                                    \
    static void wapp__case_##case_name(WappTestCtx *ctx)

/* Force-fail with a literal message. */
#define WAPP_FAIL(literal_msg) do {                                       \
    wapp_test_fail(ctx, __FILE__, __LINE__, (literal_msg));               \
    return;                                                               \
} while (0)

#define WAPP_EXPECT_TRUE(expr) do {                                       \
    if (!(expr)) {                                                        \
        wapp_test_fail(ctx, __FILE__, __LINE__,                           \
            "expected true: " #expr);                                     \
        return;                                                           \
    }                                                                     \
} while (0)

#define WAPP_EXPECT_FALSE(expr) do {                                      \
    if ((expr)) {                                                         \
        wapp_test_fail(ctx, __FILE__, __LINE__,                           \
            "expected false: " #expr);                                    \
        return;                                                           \
    }                                                                     \
} while (0)

#define WAPP_EXPECT_INT_EQ(actual, expected) do {                         \
    long long _a_ = (long long)(actual);                                  \
    long long _e_ = (long long)(expected);                                \
    if (_a_ != _e_) {                                                     \
        char _buf_[96];                                                   \
        unsigned _o_ = 0;                                                 \
        const char *_p_ = "expected ";                                    \
        while (*_p_ && _o_ + 1 < sizeof(_buf_)) _buf_[_o_++] = *_p_++;    \
        _o_ += wapp__itoa(_e_, _buf_ + _o_, sizeof(_buf_) - _o_);         \
        _p_ = " got ";                                                    \
        while (*_p_ && _o_ + 1 < sizeof(_buf_)) _buf_[_o_++] = *_p_++;    \
        _o_ += wapp__itoa(_a_, _buf_ + _o_, sizeof(_buf_) - _o_);         \
        _buf_[_o_] = 0;                                                   \
        wapp_test_fail(ctx, __FILE__, __LINE__, _buf_);                   \
        return;                                                           \
    }                                                                     \
} while (0)

#define WAPP_EXPECT_STR_EQ(actual, expected) do {                         \
    const char *_a_ = (actual);                                           \
    const char *_e_ = (expected);                                         \
    if (!_a_ || !_e_ || !wapp__streq(_a_, _e_)) {                         \
        char _buf_[256];                                                  \
        unsigned _o_ = 0;                                                 \
        const char *_p_ = "expected '";                                   \
        while (*_p_ && _o_ + 1 < sizeof(_buf_)) _buf_[_o_++] = *_p_++;    \
        const char *_s_ = _e_ ? _e_ : "(null)";                           \
        while (*_s_ && _o_ + 1 < sizeof(_buf_)) _buf_[_o_++] = *_s_++;    \
        _p_ = "' got '";                                                  \
        while (*_p_ && _o_ + 1 < sizeof(_buf_)) _buf_[_o_++] = *_p_++;    \
        _s_ = _a_ ? _a_ : "(null)";                                       \
        while (*_s_ && _o_ + 1 < sizeof(_buf_)) _buf_[_o_++] = *_s_++;    \
        if (_o_ + 1 < sizeof(_buf_)) _buf_[_o_++] = '\'';                 \
        _buf_[_o_] = 0;                                                   \
        wapp_test_fail(ctx, __FILE__, __LINE__, _buf_);                   \
        return;                                                           \
    }                                                                     \
} while (0)

#define WAPP_EXPECT_MEM_EQ(actual, expected, n) do {                      \
    if (!wapp__memeq((actual), (expected), (unsigned)(n))) {              \
        wapp_test_fail(ctx, __FILE__, __LINE__,                           \
            "memory differs from expected");                              \
        return;                                                           \
    }                                                                     \
} while (0)

#ifdef __cplusplus
}
#endif

#endif /* WAPP_TEST_H */
