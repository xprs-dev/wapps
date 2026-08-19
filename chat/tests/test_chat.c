/*
 * APRS wapp — unit tests.
 *
 * Unity build: includes ../aprs.c so the library functions (parse,
 * build, passcode) are visible. Covers the two things the user asked
 * for — receiving messages (aprs_parse) and sending messages
 * (aprs_build_message / aprs_build_beacon) — plus the computed
 * passcode and a build→parse round-trip.
 *
 * Run via the App Creator "Run tests" action, or: make tests
 */
#include "wapp_test.h"
#include "../chat.c"

/* decimal degrees -> integer (deg*10000) for exact comparison */
static int dd4(double v) { return (int)(v * 10000.0 + (v >= 0 ? 0.5 : -0.5)); }

/* ── Receive: parse incoming packets ─────────────────────────────── */

WAPP_TEST(rx_position_uncompressed) {
  aprs_packet_t p;
  WAPP_EXPECT_TRUE(aprs_parse("CT1ABC>APRS,TCPIP*:!3843.34N/00908.36W>hello", &p));
  WAPP_EXPECT_STR_EQ(p.from, "CT1ABC");
  WAPP_EXPECT_INT_EQ(p.type, APRS_POSITION);
  WAPP_EXPECT_TRUE(p.has_pos);
  WAPP_EXPECT_INT_EQ(dd4(p.lat), 387223);
  WAPP_EXPECT_INT_EQ(dd4(p.lon), -91393);
}

WAPP_TEST(rx_position_timestamped) {
  aprs_packet_t p;
  /* '@' carries DTI + DDHHMMz (8 chars) before the position block */
  WAPP_EXPECT_TRUE(aprs_parse("N0XYZ-9>APRS:@092345z3843.34N/00908.36W>x", &p));
  WAPP_EXPECT_INT_EQ(p.type, APRS_POSITION);
  WAPP_EXPECT_TRUE(p.has_pos);
  WAPP_EXPECT_INT_EQ(dd4(p.lat), 387223);
  WAPP_EXPECT_INT_EQ(dd4(p.lon), -91393);
}

WAPP_TEST(rx_south_east_hemispheres) {
  aprs_packet_t p;
  WAPP_EXPECT_TRUE(aprs_parse("VK2AAA>APRS:!3500.00S/15100.00E#", &p));
  WAPP_EXPECT_TRUE(p.has_pos);
  WAPP_EXPECT_INT_EQ(dd4(p.lat), -350000);
  WAPP_EXPECT_INT_EQ(dd4(p.lon), 1510000);
}

WAPP_TEST(rx_message_with_id) {
  aprs_packet_t p;
  WAPP_EXPECT_TRUE(aprs_parse("N0CALL-1>APRS::CT1ABC   :Hello there{042", &p));
  WAPP_EXPECT_STR_EQ(p.from, "N0CALL-1");
  WAPP_EXPECT_INT_EQ(p.type, APRS_MESSAGE);
  WAPP_EXPECT_STR_EQ(p.addressee, "CT1ABC");
  WAPP_EXPECT_STR_EQ(p.text, "Hello there");
  WAPP_EXPECT_STR_EQ(p.msgid, "042");
}

WAPP_TEST(rx_message_without_id) {
  aprs_packet_t p;
  WAPP_EXPECT_TRUE(aprs_parse("AA1AA>APRS::BB2BB    :ping", &p));
  WAPP_EXPECT_INT_EQ(p.type, APRS_MESSAGE);
  WAPP_EXPECT_STR_EQ(p.addressee, "BB2BB");
  WAPP_EXPECT_STR_EQ(p.text, "ping");
  WAPP_EXPECT_STR_EQ(p.msgid, "");
}

WAPP_TEST(rx_status_has_no_position) {
  aprs_packet_t p;
  WAPP_EXPECT_TRUE(aprs_parse("CT1ABC>APRS:>just a status", &p));
  WAPP_EXPECT_INT_EQ(p.type, APRS_STATUS);
  WAPP_EXPECT_FALSE(p.has_pos);
}

WAPP_TEST(rx_rejects_garbage) {
  aprs_packet_t p;
  WAPP_EXPECT_FALSE(aprs_parse("not a packet", &p)); /* no ':' */
  WAPP_EXPECT_FALSE(aprs_parse(":onlyinfo", &p));     /* no '>' in header */
}

/* ── Send: build outgoing TNC2 lines ─────────────────────────────── */

WAPP_TEST(tx_message_direct) {
  char line[256];
  aprs_build_message(line, sizeof(line), "X16JK8", "N0CALL", "Hi", 7);
  WAPP_EXPECT_STR_EQ(line, "X16JK8>APRS,TCPIP*::N0CALL   :Hi{7");
}

WAPP_TEST(tx_message_uppercases_and_pads) {
  char line[256];
  aprs_build_message(line, sizeof(line), "X16JK8", "ct1abc-9", "yo", 1);
  WAPP_EXPECT_STR_EQ(line, "X16JK8>APRS,TCPIP*::CT1ABC-9 :yo{1");
}

/* ── Bulletins / group messaging (APRS spec ch.14) ───────────────── */

WAPP_TEST(tx_bulletin_group_full5) {
  char line[256];
  aprs_build_bulletin(line, sizeof(line), "X16JK8", "EMCOM", '0', "Net at 8pm");
  /* BLN + id + 5-char group = exactly 9, no padding */
  WAPP_EXPECT_STR_EQ(line, "X16JK8>APRS,TCPIP*::BLN0EMCOM:Net at 8pm");
}

WAPP_TEST(tx_bulletin_group_padded) {
  char line[256];
  aprs_build_bulletin(line, sizeof(line), "X16JK8", "wx", '3', "rain");
  /* lowercase group uppercased; "BLN3WX" padded to 9 */
  WAPP_EXPECT_STR_EQ(line, "X16JK8>APRS,TCPIP*::BLN3WX   :rain");
}

WAPP_TEST(tx_bulletin_general_no_group) {
  char line[256];
  aprs_build_bulletin(line, sizeof(line), "X16JK8", "", '0', "hello all");
  WAPP_EXPECT_STR_EQ(line, "X16JK8>APRS,TCPIP*::BLN0     :hello all");
}

WAPP_TEST(tx_bulletin_lines_use_sequential_ids) {
  char l0[256], l1[256];
  aprs_build_bulletin(l0, sizeof(l0), "X16JK8", "WX", '0', "line one");
  aprs_build_bulletin(l1, sizeof(l1), "X16JK8", "WX", '1', "line two");
  WAPP_EXPECT_STR_EQ(l0, "X16JK8>APRS,TCPIP*::BLN0WX   :line one");
  WAPP_EXPECT_STR_EQ(l1, "X16JK8>APRS,TCPIP*::BLN1WX   :line two");
}

WAPP_TEST(rx_bulletin_group) {
  aprs_packet_t p;
  WAPP_EXPECT_TRUE(aprs_parse("N0CALL>APRS,TCPIP*::BLN0EMCOM:Net tonight", &p));
  WAPP_EXPECT_INT_EQ(p.type, APRS_MESSAGE);
  WAPP_EXPECT_TRUE(p.is_bulletin);
  WAPP_EXPECT_STR_EQ(p.group, "EMCOM");
  WAPP_EXPECT_INT_EQ(p.bulletin_id, '0');
  WAPP_EXPECT_STR_EQ(p.text, "Net tonight");
}

WAPP_TEST(rx_bulletin_general) {
  aprs_packet_t p;
  WAPP_EXPECT_TRUE(aprs_parse("AA1AA>APRS::BLN5     :hello", &p));
  WAPP_EXPECT_TRUE(p.is_bulletin);
  WAPP_EXPECT_STR_EQ(p.group, "");
  WAPP_EXPECT_INT_EQ(p.bulletin_id, '5');
  WAPP_EXPECT_STR_EQ(p.text, "hello");
}

WAPP_TEST(rx_direct_message_not_bulletin) {
  aprs_packet_t p;
  WAPP_EXPECT_TRUE(aprs_parse("N0CALL-1>APRS::X16JK8   :ping{4", &p));
  WAPP_EXPECT_FALSE(p.is_bulletin);
  WAPP_EXPECT_STR_EQ(p.addressee, "X16JK8");
  WAPP_EXPECT_STR_EQ(p.text, "ping");
}

WAPP_TEST(tx_beacon_with_path) {
  char line[256];
  aprs_build_beacon(line, sizeof(line), "X16JK8", 38.7223, -9.1393, "/>",
                    "WIDE1-1", "test");
  WAPP_EXPECT_STR_EQ(line, "X16JK8>APRS,WIDE1-1:!3843.34N/00908.36W>test");
}

WAPP_TEST(tx_beacon_no_path_no_comment) {
  char line[256];
  aprs_build_beacon(line, sizeof(line), "X16JK8", 38.7223, -9.1393, "/>", "", "");
  WAPP_EXPECT_STR_EQ(line, "X16JK8>APRS:!3843.34N/00908.36W>");
}

/* ── Passcode (computed, used to authenticate TX) ────────────────── */

WAPP_TEST(passcode_known_value) {
  WAPP_EXPECT_INT_EQ(aprs_passcode("N0CALL"), 13023);
}
WAPP_TEST(passcode_ignores_ssid) {
  WAPP_EXPECT_INT_EQ(aprs_passcode("N0CALL-9"), aprs_passcode("N0CALL"));
}
WAPP_TEST(passcode_is_case_insensitive) {
  WAPP_EXPECT_INT_EQ(aprs_passcode("n0call"), aprs_passcode("N0CALL"));
}

/* ── Round-trip: build → parse recovers the same data ────────────── */

WAPP_TEST(roundtrip_beacon) {
  char line[256]; aprs_packet_t p;
  aprs_build_beacon(line, sizeof(line), "X16JK8", 38.7223, -9.1393, "/>", "", "hi");
  WAPP_EXPECT_TRUE(aprs_parse(line, &p));
  WAPP_EXPECT_STR_EQ(p.from, "X16JK8");
  WAPP_EXPECT_TRUE(p.has_pos);
  WAPP_EXPECT_INT_EQ(dd4(p.lat), 387223);
  WAPP_EXPECT_INT_EQ(dd4(p.lon), -91393);
}

WAPP_TEST(roundtrip_message) {
  char line[256]; aprs_packet_t p;
  aprs_build_message(line, sizeof(line), "X16JK8", "N0CALL", "hi there", 5);
  WAPP_EXPECT_TRUE(aprs_parse(line, &p));
  WAPP_EXPECT_INT_EQ(p.type, APRS_MESSAGE);
  WAPP_EXPECT_STR_EQ(p.addressee, "N0CALL");
  WAPP_EXPECT_STR_EQ(p.text, "hi there");
  WAPP_EXPECT_STR_EQ(p.msgid, "5");
}

/* ── Long-message splitting (APRSdroid-style multi-part) ──────────── */

WAPP_TEST(split_empty_is_zero_parts) {
  char c[80];
  WAPP_EXPECT_INT_EQ(aprs_part_count("", 67), 0);
  WAPP_EXPECT_FALSE(aprs_split_text("", 67, 0, c, sizeof(c)));
}

WAPP_TEST(split_short_single_part) {
  char c[80];
  WAPP_EXPECT_INT_EQ(aprs_part_count("hello", 67), 1);
  WAPP_EXPECT_TRUE(aprs_split_text("hello", 67, 0, c, sizeof(c)));
  WAPP_EXPECT_STR_EQ(c, "hello");
  WAPP_EXPECT_FALSE(aprs_split_text("hello", 67, 1, c, sizeof(c)));
}

WAPP_TEST(split_at_word_boundaries) {
  const char *t = "the quick brown fox jumps";
  char c[80];
  WAPP_EXPECT_INT_EQ(aprs_part_count(t, 10), 3);
  WAPP_EXPECT_TRUE(aprs_split_text(t, 10, 0, c, sizeof(c)));
  WAPP_EXPECT_STR_EQ(c, "the quick");
  WAPP_EXPECT_TRUE(aprs_split_text(t, 10, 1, c, sizeof(c)));
  WAPP_EXPECT_STR_EQ(c, "brown fox");
  WAPP_EXPECT_TRUE(aprs_split_text(t, 10, 2, c, sizeof(c)));
  WAPP_EXPECT_STR_EQ(c, "jumps");
  WAPP_EXPECT_FALSE(aprs_split_text(t, 10, 3, c, sizeof(c)));
}

WAPP_TEST(split_hard_breaks_overlong_word) {
  const char *t = "abcdefghijklmnop"; /* 16 chars, no spaces */
  char c[80];
  WAPP_EXPECT_INT_EQ(aprs_part_count(t, 5), 4);
  WAPP_EXPECT_TRUE(aprs_split_text(t, 5, 0, c, sizeof(c)));
  WAPP_EXPECT_STR_EQ(c, "abcde");
  WAPP_EXPECT_TRUE(aprs_split_text(t, 5, 3, c, sizeof(c)));
  WAPP_EXPECT_STR_EQ(c, "p");
}

WAPP_TEST(split_real_limit_keeps_parts_under_67) {
  /* 78 chars > 67 -> at least 2 parts, each within the APRS body limit */
  const char *t =
      "This is a longer status update that exceeds the single APRS body "
      "limit by a bit";
  int n = aprs_part_count(t, APRS_MAX_MSG_LEN);
  WAPP_EXPECT_TRUE(n >= 2);
  char c[80];
  for (int i = 0; i < n; i++) {
    WAPP_EXPECT_TRUE(aprs_split_text(t, APRS_MAX_MSG_LEN, i, c, sizeof(c)));
    int len = 0; while (c[len]) len++;
    WAPP_EXPECT_TRUE(len <= APRS_MAX_MSG_LEN);
    WAPP_EXPECT_TRUE(len > 0);
  }
}
