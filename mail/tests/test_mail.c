/*
 * mail — unit tests.
 *
 * What is actually dangerous in this wapp is not the UI, it is the two places
 * where a mistake FAILS SILENTLY:
 *
 *   1. Key encodings. Three encodings of the same 32 bytes are in play (hex for
 *      NOSTR, base64url for the relay lane, npub for humans). Hand a lane the
 *      wrong one and nothing crashes — the message is simply never delivered,
 *      or is encrypted to a key nobody holds. So every conversion is pinned,
 *      both directions, and round-tripped.
 *
 *   2. The dedup envelope. Get it wrong and the user either sees every message
 *      twice (the two lanes not folding) or loses messages (over-eager dedup).
 *
 * Unity build: includes ../main.c to reach the static helpers.
 */
#include "wapp_test.h"
#include "../main.c"

/* ── Key encodings ───────────────────────────────────────────────────── */

/* A real x-only pubkey and its two other spellings. K_HEX is the NIP-19 test
 * vector; K_B64U is its base64url, taken from an independent encoder — NOT from
 * this wapp's own output, or the test would only prove the code agrees with
 * itself. (The first draft of this constant had a typo, and that is precisely
 * what caught it.) */
#define K_HEX  "3bf0c63fcb93463407af97a5e5ee64fa883d107ef9e558472c4eb9aaaefa459d"
#define K_B64U "O_DGP8uTRjQHr5el5e5k-og9EH755VhHLE65qq76RZ0"

WAPP_TEST(hex_to_b64url_matches_the_relay_lane_spelling) {
    char out[64];
    hex_to_b64url(K_HEX, out, sizeof(out));
    WAPP_EXPECT_STR_EQ(out, K_B64U);
    /* 43 unpadded chars — hal_relay_dm_send decodes exactly 32 bytes or drops
     * the message on the floor. */
    WAPP_EXPECT_INT_EQ((int)str_len(out), 43);
}

WAPP_TEST(b64url_to_hex_is_the_exact_inverse) {
    char out[80];
    WAPP_EXPECT_TRUE(b64url_to_hex(K_B64U, out, sizeof(out)));
    WAPP_EXPECT_STR_EQ(out, K_HEX);
}

WAPP_TEST(b64url_round_trips) {
    char b64[64], back[80];
    hex_to_b64url(K_HEX, b64, sizeof(b64));
    WAPP_EXPECT_TRUE(b64url_to_hex(b64, back, sizeof(back)));
    WAPP_EXPECT_STR_EQ(back, K_HEX);
}

WAPP_TEST(b64url_rejects_wrong_length) {
    char out[80];
    WAPP_EXPECT_TRUE(!b64url_to_hex("tooshort", out, sizeof(out)));
    WAPP_EXPECT_TRUE(!b64url_to_hex("", out, sizeof(out)));
}

WAPP_TEST(npub_decodes_to_hex) {
    /* The canonical NIP-19 test vector. */
    char out[80];
    WAPP_EXPECT_TRUE(npub_to_hex(
        "npub180cvv07tjdrrgpa0j7j7tmnyl2yr6yr7l8j4s3evf6u64th6gkwsyjh6w6",
        out, sizeof(out)));
    WAPP_EXPECT_STR_EQ(
        out, "3bf0c63fcb93463407af97a5e5ee64fa883d107ef9e558472c4eb9aaaefa459d");
}

WAPP_TEST(npub_rejects_a_non_npub) {
    char out[80];
    WAPP_EXPECT_TRUE(!npub_to_hex("nsec1abc", out, sizeof(out)));
    WAPP_EXPECT_TRUE(!npub_to_hex("hello", out, sizeof(out)));
}

/* key_to_hex is what the New-message box runs on whatever the user typed. All
 * three spellings must land on the same conversation — otherwise the same person
 * shows up as two threads depending on how you addressed them. */
WAPP_TEST(key_to_hex_accepts_all_three_spellings) {
    char a[80], b[80], c[80];
    WAPP_EXPECT_TRUE(key_to_hex(K_HEX, a, sizeof(a)));
    WAPP_EXPECT_TRUE(key_to_hex(K_B64U, b, sizeof(b)));
    WAPP_EXPECT_TRUE(key_to_hex(
        "npub180cvv07tjdrrgpa0j7j7tmnyl2yr6yr7l8j4s3evf6u64th6gkwsyjh6w6",
        c, sizeof(c)));
    WAPP_EXPECT_STR_EQ(a, K_HEX);
    WAPP_EXPECT_STR_EQ(b, K_HEX);
    WAPP_EXPECT_STR_EQ(c, K_HEX);
}

WAPP_TEST(key_to_hex_uppercases_are_normalised) {
    char out[80];
    WAPP_EXPECT_TRUE(key_to_hex(
        "3BF0C63FCB93463407AF97A5E5EE64FA883D107EF9E558472C4EB9AAAEFA459D",
        out, sizeof(out)));
    WAPP_EXPECT_STR_EQ(out, K_HEX);
}

WAPP_TEST(key_to_hex_rejects_a_callsign) {
    /* Not a key — the caller must fall back to a relay lookup, not send a DM to
     * a garbage pubkey. */
    char out[80];
    WAPP_EXPECT_TRUE(!key_to_hex("N0CALL", out, sizeof(out)));
    WAPP_EXPECT_TRUE(!key_to_hex("", out, sizeof(out)));
}

/* ── The dedup envelope ──────────────────────────────────────────────── */

WAPP_TEST(envelope_round_trips) {
    char env[128], id[20];
    env_wrap("a1b2c3d4", "hello there", env, sizeof(env));
    const char *text = env_split(env, id, sizeof(id));
    WAPP_EXPECT_STR_EQ(id, "a1b2c3d4");
    WAPP_EXPECT_STR_EQ(text, "hello there");
}

WAPP_TEST(envelope_preserves_the_text_exactly) {
    /* The text is the user's message. Nothing may be eaten — including the
     * characters the envelope itself uses as delimiters appearing in the body. */
    char env[256], id[20];
    env_wrap("00000001", "a:b, \"quoted\" \x02 and more", env, sizeof(env));
    const char *text = env_split(env, id, sizeof(id));
    WAPP_EXPECT_STR_EQ(id, "00000001");
    WAPP_EXPECT_STR_EQ(text, "a:b, \"quoted\" \x02 and more");
}

WAPP_TEST(message_without_an_envelope_is_passed_through) {
    /* A sender running older code sends bare text. It must still be delivered
     * (dedup then falls back to the event id), not mangled or dropped. */
    char id[20];
    const char *text = env_split("plain old message", id, sizeof(id));
    WAPP_EXPECT_STR_EQ(id, "");
    WAPP_EXPECT_STR_EQ(text, "plain old message");
}

WAPP_TEST(a_truncated_envelope_is_treated_as_plain_text) {
    /* SOH but no STX: not our envelope. Deliver it rather than silently eat the
     * message. */
    char id[20];
    char broken[32];
    broken[0] = ENV_SOH;
    str_copy(broken + 1, "deadbeef no stx", sizeof(broken) - 1);
    const char *text = env_split(broken, id, sizeof(id));
    WAPP_EXPECT_STR_EQ(id, "");
    WAPP_EXPECT_TRUE(text == broken);
}

/* ── JSON unescaping ─────────────────────────────────────────────────────
 * The bug this locks, seen live on-device: the relay lane hands us the message
 * as HOST-ENCODED JSON, so the envelope's control bytes arrive as the literal
 * six characters "". json_raw returns the raw value, escapes and all, so
 * the envelope went unrecognised, the two lanes never folded, and ONE message
 * showed up as FOUR (+4 received, 0 folded). */

WAPP_TEST(json_unescape_restores_the_envelope_control_bytes) {
    char s[64];
    str_copy(s, "\\u0001a1b2c3d4\\u0002hello", sizeof(s));
    json_unescape(s);
    WAPP_EXPECT_INT_EQ((unsigned char)s[0], ENV_SOH);
    WAPP_EXPECT_INT_EQ((unsigned char)s[9], ENV_STX);

    /* ...and only then can the envelope be split, which is the whole point. */
    char id[20];
    const char *text = env_split(s, id, sizeof(id));
    WAPP_EXPECT_STR_EQ(id, "a1b2c3d4");
    WAPP_EXPECT_STR_EQ(text, "hello");
}

WAPP_TEST(json_unescape_handles_ordinary_escapes) {
    char s[80];
    str_copy(s, "line1\\nline2 \\\"quoted\\\" back\\\\slash", sizeof(s));
    json_unescape(s);
    WAPP_EXPECT_STR_EQ(s, "line1\nline2 \"quoted\" back\\slash");
}

WAPP_TEST(json_unescape_decodes_utf8) {
    char s[32];
    str_copy(s, "caf\\u00e9", sizeof(s));  /* é = C3 A9 */
    json_unescape(s);
    WAPP_EXPECT_STR_EQ(s, "caf\xc3\xa9");
}

WAPP_TEST(json_unescape_leaves_plain_text_alone) {
    char s[32];
    str_copy(s, "nothing to do here", sizeof(s));
    json_unescape(s);
    WAPP_EXPECT_STR_EQ(s, "nothing to do here");
}

/* ── The seen-ring ───────────────────────────────────────────────────── */

WAPP_TEST(seen_ring_folds_a_duplicate) {
    g_seen_n = 0; g_seen_w = 0;
    WAPP_EXPECT_TRUE(!seen_has("aabbccdd"));
    seen_add("aabbccdd");
    WAPP_EXPECT_TRUE(seen_has("aabbccdd"));
    /* The second lane's copy of the SAME message: folded, not shown twice. */
    WAPP_EXPECT_TRUE(seen_has("aabbccdd"));
    WAPP_EXPECT_TRUE(!seen_has("11223344"));
}

WAPP_TEST(seen_ring_does_not_grow_without_bound) {
    g_seen_n = 0; g_seen_w = 0;
    char k[20];
    for (int i = 0; i < SEEN_MAX + 40; i++) {
        u64_str((unsigned long long)(1000 + i), k);
        seen_add(k);
    }
    WAPP_EXPECT_INT_EQ(g_seen_n, SEEN_MAX);
    /* The most recent id must still be remembered — that is the one a late
     * store-and-forward copy would collide with. */
    u64_str((unsigned long long)(1000 + SEEN_MAX + 39), k);
    WAPP_EXPECT_TRUE(seen_has(k));
}

WAPP_TEST(seen_ring_ignores_an_empty_key) {
    g_seen_n = 0; g_seen_w = 0;
    seen_add("");
    WAPP_EXPECT_INT_EQ(g_seen_n, 0);
    WAPP_EXPECT_TRUE(!seen_has(""));
}

/* ── The block list ──────────────────────────────────────────────────────
 * A spammer knows your key, so dropping their messages is the only defence.
 * What must not break: the list is keyed on the 64-char pubkey (a display name
 * is a nickname the spammer picks), it survives a restart, and unblocking
 * actually undoes it. */

#define SPAMMER "fa31ff61cfa7000000000000000000000000000000000000000000000000dead"

WAPP_TEST(block_add_then_is_blocked) {
    g_block_n = 0;
    WAPP_EXPECT_TRUE(!is_blocked(SPAMMER));
    WAPP_EXPECT_TRUE(block_add(SPAMMER));
    WAPP_EXPECT_TRUE(is_blocked(SPAMMER));
    /* Everybody else still gets through — a block is one key, not a mood. */
    WAPP_EXPECT_TRUE(!is_blocked(K_HEX));
}

WAPP_TEST(block_is_not_added_twice) {
    g_block_n = 0;
    WAPP_EXPECT_TRUE(block_add(SPAMMER));
    WAPP_EXPECT_TRUE(!block_add(SPAMMER));
    WAPP_EXPECT_INT_EQ(g_block_n, 1);
}

WAPP_TEST(block_rejects_a_non_key) {
    /* The host menu can hand us a display name; blocking it as if it were a key
     * would put junk in the list and block nobody. */
    g_block_n = 0;
    WAPP_EXPECT_TRUE(!block_add("fa31ff61cfa7"));   /* truncated title */
    WAPP_EXPECT_TRUE(!block_add("N0CALL"));
    WAPP_EXPECT_TRUE(!block_add(""));
    WAPP_EXPECT_INT_EQ(g_block_n, 0);
}

WAPP_TEST(block_refuses_to_block_yourself) {
    g_block_n = 0;
    str_copy(g_self, K_HEX, sizeof(g_self));
    WAPP_EXPECT_TRUE(!block_add(K_HEX));
    WAPP_EXPECT_INT_EQ(g_block_n, 0);
    g_self[0] = '\0';
}

WAPP_TEST(unblock_removes_exactly_one) {
    g_block_n = 0;
    block_add(SPAMMER);
    block_add(K_HEX);
    WAPP_EXPECT_TRUE(block_remove(SPAMMER));
    WAPP_EXPECT_TRUE(!is_blocked(SPAMMER));
    WAPP_EXPECT_TRUE(is_blocked(K_HEX));      /* the other one is untouched */
    WAPP_EXPECT_INT_EQ(g_block_n, 1);
    WAPP_EXPECT_TRUE(!block_remove(SPAMMER)); /* already gone */
}

WAPP_TEST(block_list_survives_a_restart) {
    /* Persisted in KV: a block that forgets itself on reboot is not a block. */
    g_block_n = 0;
    block_add(SPAMMER);
    block_add(K_HEX);
    g_block_n = 0;                 /* as if the wapp had just started */
    block_load();
    WAPP_EXPECT_INT_EQ(g_block_n, 2);
    WAPP_EXPECT_TRUE(is_blocked(SPAMMER));
    WAPP_EXPECT_TRUE(is_blocked(K_HEX));
    /* Leave the store clean for the next run. */
    g_block_n = 0;
    block_save();
}
