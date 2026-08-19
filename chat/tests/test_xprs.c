/*
 * The wire chat actually airs — unit tests.
 *
 * Chat used to invent its own three-field frame; it now speaks XPRS
 * (docs/XPRS.md), which every other station on the device already reads. Two
 * properties matter more than the rest:
 *
 *   1. Round-tripping. Everything chat routes on — a 1:1 callsign, a group,
 *      a position, a bare in-range line — has to come back out of the packet
 *      exactly as it went in, because the whole of ble_handle below the
 *      parser is written against those three fields.
 *   2. Refusing rather than mangling. A frame with no XPRS form (the ?MAIL /
 *      ?IGATE control frames) or one that would not fit must produce NO
 *      packet, so the caller falls back to the compact frame instead of
 *      airing a truncated one.
 *
 * Run via the App Creator "Run tests" action, or: make tests
 */
#include "wapp_test.h"
#include "../xprs.c"

/* 2026-08-08 14:26:40 UTC — the timestamp docs/XPRS.md uses throughout. */
#define T0 1786199200ULL

/* ── Time, section 4.8 ───────────────────────────────────────────── */

WAPP_TEST(stamp_is_the_documented_shape) {
  char s[24];
  xprs_stamp(s, sizeof(s), T0);
  WAPP_EXPECT_STR_EQ(s, "2026-08-08_14:26:40");
}

WAPP_TEST(stamp_round_trips) {
  char s[24];
  xprs_stamp(s, sizeof(s), T0);
  WAPP_EXPECT_TRUE(xprs_parse_stamp(s) == T0);
}

WAPP_TEST(stamp_round_trips_across_a_leap_day) {
  /* 2024-02-29 00:00:00 UTC. A civil-date bug shows up here first. */
  char s[24];
  xprs_stamp(s, sizeof(s), 1709164800ULL);
  WAPP_EXPECT_STR_EQ(s, "2024-02-29_00:00:00");
  WAPP_EXPECT_TRUE(xprs_parse_stamp(s) == 1709164800ULL);
}

WAPP_TEST(a_malformed_stamp_is_zero_not_a_guess) {
  WAPP_EXPECT_TRUE(xprs_parse_stamp("2026-08-08") == 0);
  WAPP_EXPECT_TRUE(xprs_parse_stamp("not a time at all") == 0);
  WAPP_EXPECT_TRUE(xprs_parse_stamp("2026-13-08_14:26:40") == 0);
}

/* ── Addresses, section 6.3 ──────────────────────────────────────── */

WAPP_TEST(stations_are_told_from_groups) {
  WAPP_EXPECT_TRUE(xprs_is_station("X1RD89"));
  WAPP_EXPECT_TRUE(xprs_is_station("X3RLY7"));
  WAPP_EXPECT_TRUE(xprs_is_station("X5A3F2"));
  WAPP_EXPECT_TRUE(xprs_is_station("CT1ABC-9"));
  WAPP_EXPECT_TRUE(xprs_is_station("CT1ABC"));
  WAPP_EXPECT_TRUE(!xprs_is_station("LISBOA"));
  WAPP_EXPECT_TRUE(!xprs_is_station("FEED"));
  WAPP_EXPECT_TRUE(!xprs_is_station("NOSTR"));
  WAPP_EXPECT_TRUE(!xprs_is_station(""));
}

/* ── Building ────────────────────────────────────────────────────── */

WAPP_TEST(a_direct_message_is_a_documented_packet) {
  char w[300];
  WAPP_EXPECT_TRUE(xprs_pack(w, sizeof(w), "X1QZ3N", "X1RD89",
                             "meet at the bridge at six", T0) > 0);
  WAPP_EXPECT_STR_EQ(w, "t:message f:X1QZ3N d:X1RD89 ts:2026-08-08_14:26:40 "
                        "m:meet at the bridge at six");
}

WAPP_TEST(a_group_loses_the_hash_chat_marks_it_with) {
  char w[300];
  xprs_pack(w, sizeof(w), "X1QZ3N", "#LISBOA", "net starts in ten minutes", T0);
  WAPP_EXPECT_STR_EQ(w, "t:message f:X1QZ3N d:LISBOA ts:2026-08-08_14:26:40 "
                        "m:net starts in ten minutes");
}

WAPP_TEST(no_addressee_is_a_broadcast_with_no_d) {
  char w[300];
  xprs_pack(w, sizeof(w), "X1QZ3N", "", "anyone near the north gate?", T0);
  WAPP_EXPECT_STR_EQ(w, "t:message f:X1QZ3N ts:2026-08-08_14:26:40 "
                        "m:anyone near the north gate?");
}

WAPP_TEST(a_position_is_an_observation_not_a_message) {
  char w[300];
  xprs_pack(w, sizeof(w), "X1QZ3N", "!", "38.7223,-9.1393,at the ferry", T0);
  WAPP_EXPECT_STR_EQ(w, "t:observation f:X1QZ3N ts:2026-08-08_14:26:40 "
                        "pos:38.7223,-9.1393 m:at the ferry");
}

WAPP_TEST(a_position_without_a_comment_carries_no_m) {
  char w[300];
  xprs_pack(w, sizeof(w), "X1QZ3N", "!", "38.7223,-9.1393", T0);
  WAPP_EXPECT_STR_EQ(w, "t:observation f:X1QZ3N ts:2026-08-08_14:26:40 "
                        "pos:38.7223,-9.1393");
}

WAPP_TEST(control_frames_have_no_xprs_form_and_are_refused) {
  char w[300];
  WAPP_EXPECT_TRUE(xprs_pack(w, sizeof(w), "X1QZ3N", "?MAIL", "7 4821", T0) == 0);
  WAPP_EXPECT_TRUE(xprs_pack(w, sizeof(w), "X1QZ3N", "?IGATE", "", T0) == 0);
  WAPP_EXPECT_TRUE(xprs_pack(w, sizeof(w), "X1QZ3N", "?PING", "3", T0) == 0);
}

WAPP_TEST(a_frame_that_does_not_fit_is_no_frame_at_all) {
  char w[64];
  /* Truncating would corrupt the body silently; the caller must be able to
   * tell that it has to air the compact form instead. */
  WAPP_EXPECT_TRUE(xprs_pack(w, sizeof(w), "X1QZ3N", "X1RD89",
      "a message far longer than the buffer it is being written into, so that "
      "the packing has to refuse it outright", T0) == 0);
}

/* ── Reading ─────────────────────────────────────────────────────── */

WAPP_TEST(compact_frames_are_not_mistaken_for_xprs) {
  WAPP_EXPECT_TRUE(!xprs_looks_like("X1QZ3N\x1f" "X1RD89\x1f" "hello"));
  WAPP_EXPECT_TRUE(xprs_looks_like("t:message f:X1QZ3N"));
}

WAPP_TEST(everything_chat_routes_on_survives_a_round_trip) {
  const char *tos[]   = { "X1RD89", "#LISBOA", "", "!" };
  const char *texts[] = { "meet at six", "net in ten", "who is around?",
                          "38.7223,-9.1393,at the ferry" };
  for (int i = 0; i < 4; i++) {
    char w[300], f[16], to[24], tx[256];
    unsigned long long ts = 0;
    WAPP_EXPECT_TRUE(xprs_pack(w, sizeof(w), "X1QZ3N", tos[i], texts[i], T0) > 0);
    WAPP_EXPECT_TRUE(xprs_unpack(w, f, sizeof(f), to, sizeof(to),
                                 tx, sizeof(tx), &ts));
    WAPP_EXPECT_STR_EQ(f, "X1QZ3N");
    WAPP_EXPECT_STR_EQ(to, tos[i]);
    WAPP_EXPECT_STR_EQ(tx, texts[i]);
    WAPP_EXPECT_TRUE(ts == T0);
  }
}

WAPP_TEST(the_message_keeps_its_spaces_and_colons) {
  /* `m:` is greedy and last (section 4), which is what lets chat's own
   * conventions — "ENC1:...", a "+<id> " reply marker — ride inside it. */
  char f[16], to[24], tx[256];
  unsigned long long ts = 0;
  WAPP_EXPECT_TRUE(xprs_unpack(
      "t:message f:X1QZ3N d:X1RD89 ts:2026-08-08_14:26:40 "
      "m:+9eb5 ENC1:aGVsbG8= ~sig here",
      f, sizeof(f), to, sizeof(to), tx, sizeof(tx), &ts));
  WAPP_EXPECT_STR_EQ(tx, "+9eb5 ENC1:aGVsbG8= ~sig here");
}

WAPP_TEST(a_packet_with_no_sender_is_dropped) {
  char f[16], to[24], tx[256];
  WAPP_EXPECT_TRUE(!xprs_unpack("t:message ts:2026-08-08_14:26:40 m:hello",
                                f, sizeof(f), to, sizeof(to), tx, sizeof(tx), 0));
}

WAPP_TEST(types_chat_cannot_show_are_left_to_the_xprs_wapp) {
  char f[16], to[24], tx[256];
  /* A beacon, a poll, a service announcement: real XPRS, nothing for a chat
   * bubble. Dropping them here is what stops them appearing as text. */
  WAPP_EXPECT_TRUE(!xprs_unpack(
      "t:poll f:X1QZ3N ts:2026-08-08_14:26:40 opt:yes,no m:tonight?",
      f, sizeof(f), to, sizeof(to), tx, sizeof(tx), 0));
  WAPP_EXPECT_TRUE(!xprs_unpack(
      "t:observation f:X1RD89 link:ble peers:2 hears:X32DVA",
      f, sizeof(f), to, sizeof(to), tx, sizeof(tx), 0));
}

WAPP_TEST(a_group_comes_back_marked_with_a_hash) {
  char f[16], to[24], tx[256];
  WAPP_EXPECT_TRUE(xprs_unpack(
      "t:message f:X1QZ3N d:LISBOA ts:2026-08-08_14:26:40 m:net in ten",
      f, sizeof(f), to, sizeof(to), tx, sizeof(tx), 0));
  WAPP_EXPECT_STR_EQ(to, "#LISBOA");
}

WAPP_TEST(a_packet_with_no_clock_still_reads) {
  /* Section 10.7: a station without a clock sends no ts:. The message is
   * still a message; only its time is unknown. */
  char f[16], to[24], tx[256];
  unsigned long long ts = 1;
  WAPP_EXPECT_TRUE(xprs_unpack("t:message f:X1QZ3N d:X1RD89 m:no clock here",
                               f, sizeof(f), to, sizeof(to), tx, sizeof(tx), &ts));
  WAPP_EXPECT_STR_EQ(tx, "no clock here");
  WAPP_EXPECT_TRUE(ts == 0);
}
