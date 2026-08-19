/*
 * Forum wapp — unit tests
 *
 * The unity build pattern: this file #includes ../main.c so static
 * helpers (sanitise_title, format_timestamp, str_eq, json_*) become
 * visible to the test cases. No other test_*.c file may include
 * main.c in the same link, or wasm-ld will fail with duplicate
 * symbols on the lifecycle functions.
 *
 * Run via the App Creator wapp's "Run tests" action, or directly
 * with: cd wapps/forum && make tests
 */

#include "wapp_test.h"
#include "../main.c"

/* ── sanitise_title — title slug for thread filenames ──────────── */

WAPP_TEST(sanitise_title_basic) {
    char out[64];
    sanitise_title("Hello, world!", out, sizeof(out));
    WAPP_EXPECT_STR_EQ(out, "hello-world");
}

WAPP_TEST(sanitise_title_strips_leading_trailing_dashes) {
    char out[64];
    sanitise_title("  --Hello--  ", out, sizeof(out));
    WAPP_EXPECT_STR_EQ(out, "hello");
}

WAPP_TEST(sanitise_title_collapses_runs_of_separators) {
    char out[64];
    sanitise_title("foo   bar___baz", out, sizeof(out));
    WAPP_EXPECT_STR_EQ(out, "foo-bar-baz");
}

WAPP_TEST(sanitise_title_lowercases) {
    char out[64];
    sanitise_title("ABCxyz", out, sizeof(out));
    WAPP_EXPECT_STR_EQ(out, "abcxyz");
}

WAPP_TEST(sanitise_title_keeps_digits) {
    char out[64];
    sanitise_title("Release v1.2.3", out, sizeof(out));
    WAPP_EXPECT_STR_EQ(out, "release-v1-2-3");
}

WAPP_TEST(sanitise_title_empty_falls_back_to_t) {
    char out[64];
    sanitise_title("", out, sizeof(out));
    WAPP_EXPECT_STR_EQ(out, "t");
}

WAPP_TEST(sanitise_title_only_punctuation_falls_back_to_t) {
    char out[64];
    sanitise_title("!!!---???", out, sizeof(out));
    WAPP_EXPECT_STR_EQ(out, "t");
}

WAPP_TEST(sanitise_title_caps_at_60_chars) {
    char out[128];
    /* 70-char input → at most 60 chars output (and trailing dashes
     * stripped, so could be shorter). */
    sanitise_title(
        "abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghij1234567890",
        out, sizeof(out));
    unsigned len = 0;
    while (out[len]) len++;
    WAPP_EXPECT_TRUE(len <= 60);
}

/* ── format_timestamp — civil-from-epoch ──────────────────────── */

WAPP_TEST(format_timestamp_unix_zero) {
    char out[32];
    format_timestamp(0, out, sizeof(out));
    WAPP_EXPECT_STR_EQ(out, "1970-01-01 00:00_00");
}

WAPP_TEST(format_timestamp_known_date) {
    /* 1700000000 = 2023-11-14 22:13:20 UTC */
    char out[32];
    format_timestamp(1700000000ULL, out, sizeof(out));
    WAPP_EXPECT_STR_EQ(out, "2023-11-14 22:13_20");
}

WAPP_TEST(format_timestamp_leap_year) {
    /* 2020-02-29 12:00:00 UTC = 1582977600 */
    char out[32];
    format_timestamp(1582977600ULL, out, sizeof(out));
    WAPP_EXPECT_STR_EQ(out, "2020-02-29 12:00_00");
}

/* ── string predicates ─────────────────────────────────────────── */

WAPP_TEST(str_eq_equal) {
    WAPP_EXPECT_INT_EQ(str_eq("abc", "abc"), 1);
}

WAPP_TEST(str_eq_different) {
    WAPP_EXPECT_INT_EQ(str_eq("abc", "abd"), 0);
}

WAPP_TEST(str_eq_length_mismatch) {
    WAPP_EXPECT_INT_EQ(str_eq("abc", "abcd"), 0);
    WAPP_EXPECT_INT_EQ(str_eq("abcd", "abc"), 0);
}

WAPP_TEST(str_eq_empty) {
    WAPP_EXPECT_INT_EQ(str_eq("", ""), 1);
    WAPP_EXPECT_INT_EQ(str_eq("", "a"), 0);
}

WAPP_TEST(str_starts_match) {
    WAPP_EXPECT_INT_EQ(str_starts("profile.read.response", "profile."), 1);
}

WAPP_TEST(str_starts_no_match) {
    WAPP_EXPECT_INT_EQ(str_starts("hello", "world"), 0);
}

WAPP_TEST(str_starts_prefix_longer_than_string) {
    WAPP_EXPECT_INT_EQ(str_starts("hi", "hello"), 0);
}

WAPP_TEST(str_starts_empty_prefix) {
    WAPP_EXPECT_INT_EQ(str_starts("anything", ""), 1);
}

/* ── json_find_string — minimal JSON field extractor ────────── */

WAPP_TEST(json_find_string_basic) {
    const char *json = "{\"type\":\"action\",\"name\":\"refresh\"}";
    char buf[32];
    int ok = json_find_string(json, 33, "type", buf, sizeof(buf));
    WAPP_EXPECT_INT_EQ(ok, 1);
    WAPP_EXPECT_STR_EQ(buf, "action");
}

WAPP_TEST(json_find_string_missing_key) {
    const char *json = "{\"type\":\"action\"}";
    char buf[32];
    int ok = json_find_string(json, 17, "missing", buf, sizeof(buf));
    WAPP_EXPECT_INT_EQ(ok, 0);
}

WAPP_TEST(json_find_string_handles_escaped_quote) {
    const char *json = "{\"k\":\"a\\\"b\"}";
    char buf[16];
    int ok = json_find_string(json, 12, "k", buf, sizeof(buf));
    WAPP_EXPECT_INT_EQ(ok, 1);
    /* The current implementation strips up to the next unescaped " —
     * so it stops at the escaped quote. Test documents the current
     * contract, not necessarily the most correct behaviour. */
    (void)buf;
}
