/*
 * Thread ids, reply markers and like votes — unit tests.
 *
 * These pin down the two conventions that a bug shipped broken: a reply's
 * "+<id> " marker is WIRE SYNTAX and must never reach a bubble as text, and a
 * heart is a VOTE that must never reach a bubble as a message. Both failures
 * were visible on a 1:1 LXMF conversation — a bubble reading
 * "+9eb53a4af55e5da04cdcc44842502041e8d5e2460f123358c31a17f8a31993dd OK",
 * and a heart that did nothing at all.
 *
 * Run via the App Creator "Run tests" action, or: make tests
 */
#include "wapp_test.h"
#include "../thread.c"

/* ── Message ids: derived, never assigned ────────────────────────── */

WAPP_TEST(mid_is_four_lowercase_hex) {
  char id[5];
  msg_id("X16JK8", "hello", id);
  WAPP_EXPECT_INT_EQ((int)strlen(id), 4);
  for (int i = 0; i < 4; i++) {
    char c = id[i];
    WAPP_EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
  }
}

WAPP_TEST(mid_is_stable_for_the_same_input) {
  char a[5], b[5];
  msg_id("X16JK8", "hello", a);
  msg_id("X16JK8", "hello", b);
  WAPP_EXPECT_STR_EQ(a, b);
}

/* The whole point: sender and receiver derive the SAME id, so a reply or a
 * heart can name its target with nothing extra on the wire. */
WAPP_TEST(mid_agrees_across_both_ends) {
  const char *dest = "85cdc0319f2a4d1e7b0c5a6d8e9f0123";
  char sender_side[5], receiver_side[5];
  msg_id(dest, "TABLET-WORKS-1", sender_side);     /* our echo, from our dest */
  msg_id(dest, "TABLET-WORKS-1", receiver_side);   /* their drain, from `from` */
  WAPP_EXPECT_STR_EQ(sender_side, receiver_side);
}

WAPP_TEST(mid_separates_sender_from_text) {
  char a[5], b[5];
  msg_id("AB", "CD", a);
  msg_id("A", "BCD", b);   /* would collide without the "|" separator */
  WAPP_EXPECT_TRUE(!wapp__streq(a, b));
}

WAPP_TEST(mid_differs_by_text) {
  char a[5], b[5];
  msg_id("X16JK8", "hello", a);
  msg_id("X16JK8", "hellO", b);
  WAPP_EXPECT_TRUE(!wapp__streq(a, b));
}

/* ── Reply markers ───────────────────────────────────────────────── */

WAPP_TEST(reply_marker_splits_parent_from_text) {
  char parent[5]; const char *disp;
  WAPP_EXPECT_TRUE(thread_parse("+9eb5 OK", parent, &disp));
  WAPP_EXPECT_STR_EQ(parent, "9eb5");
  WAPP_EXPECT_STR_EQ(disp, "OK");
}

WAPP_TEST(plain_text_is_not_a_marker) {
  char parent[5]; const char *disp;
  WAPP_EXPECT_TRUE(!thread_parse("OK", parent, &disp));
  WAPP_EXPECT_STR_EQ(parent, "");
  WAPP_EXPECT_STR_EQ(disp, "OK");
}

/* A message may legitimately start with "+" and digits. Only an exact 4-hex
 * (or legacy 64-hex) run followed by a space is syntax. */
WAPP_TEST(a_phone_number_is_not_a_marker) {
  char parent[5]; const char *disp;
  WAPP_EXPECT_TRUE(!thread_parse("+351912345678 call me", parent, &disp));
  WAPP_EXPECT_STR_EQ(disp, "+351912345678 call me");
  WAPP_EXPECT_TRUE(!thread_parse("+12345 later", parent, &disp));
  WAPP_EXPECT_STR_EQ(disp, "+12345 later");
}

WAPP_TEST(marker_needs_hex_and_a_space) {
  char parent[5]; const char *disp;
  WAPP_EXPECT_TRUE(!thread_parse("+9zb5 OK", parent, &disp));   /* z is not hex */
  WAPP_EXPECT_TRUE(!thread_parse("+9eb5OK", parent, &disp));    /* no space */
  WAPP_EXPECT_TRUE(!thread_parse("+9EB5 OK", parent, &disp));   /* upper case */
}

/* The exact bubble from the bug report: an older build named its parent by the
 * 64-hex LXMF envelope hash. Unresolvable (only the receiver ever knew it) but
 * still syntax — strip it, thread nothing, and never show it. */
WAPP_TEST(legacy_long_marker_is_stripped_not_shown) {
  char parent[5]; const char *disp;
  WAPP_EXPECT_TRUE(thread_parse(
      "+9eb53a4af55e5da04cdcc44842502041e8d5e2460f123358c31a17f8a31993dd OK",
      parent, &disp));
  WAPP_EXPECT_STR_EQ(parent, "");
  WAPP_EXPECT_STR_EQ(disp, "OK");
}

WAPP_TEST(marker_round_trips_through_the_wire) {
  char wire[64]; char parent[5]; const char *disp;
  thread_wire(wire, sizeof(wire), "9eb5", "see you tomorrow");
  WAPP_EXPECT_STR_EQ(wire, "+9eb5 see you tomorrow");
  WAPP_EXPECT_TRUE(thread_parse(wire, parent, &disp));
  WAPP_EXPECT_STR_EQ(parent, "9eb5");
  WAPP_EXPECT_STR_EQ(disp, "see you tomorrow");
}

WAPP_TEST(no_parent_means_no_marker_on_the_wire) {
  char wire[64];
  thread_wire(wire, sizeof(wire), "", "plain message");
  WAPP_EXPECT_STR_EQ(wire, "plain message");
}

/* ── Like votes ──────────────────────────────────────────────────── */

WAPP_TEST(short_like_vote_is_recognised) {
  char tgt[5]; int unlike = 9;
  WAPP_EXPECT_TRUE(like_parse("b9fb:like", tgt, &unlike));
  WAPP_EXPECT_STR_EQ(tgt, "b9fb");
  WAPP_EXPECT_INT_EQ(unlike, 0);
  WAPP_EXPECT_TRUE(like_parse("b9fb:unlike", tgt, &unlike));
  WAPP_EXPECT_INT_EQ(unlike, 1);
}

WAPP_TEST(long_like_vote_is_recognised) {
  const char *ev =
      "82ccbaec1f0d4a5b6c7d8e9f00112233445566778899aabbccddeeff00112233";
  char mid[70]; int unlike = 9;
  char in[80]; strcpy(in, ev); strcat(in, ":like");
  WAPP_EXPECT_TRUE(roomlike_parse(in, mid, &unlike));
  WAPP_EXPECT_STR_EQ(mid, ev);
  WAPP_EXPECT_INT_EQ(unlike, 0);
}

WAPP_TEST(a_message_is_never_a_vote) {
  char mid[70]; int unlike;
  WAPP_EXPECT_TRUE(!anylike_parse("OK", mid, &unlike));
  WAPP_EXPECT_TRUE(!anylike_parse("hello there", mid, &unlike));
  WAPP_EXPECT_TRUE(!anylike_parse("+9eb5 OK", mid, &unlike));
  WAPP_EXPECT_TRUE(!anylike_parse("b9fb:lik", mid, &unlike));    /* truncated */
  WAPP_EXPECT_TRUE(!anylike_parse("b9fb:like!", mid, &unlike));  /* trailing */
  WAPP_EXPECT_TRUE(!anylike_parse("zzzz:like", mid, &unlike));   /* not hex */
  WAPP_EXPECT_TRUE(!anylike_parse("b9f:like", mid, &unlike));    /* too short */
}

/* Both send and receive paths route votes through anylike_parse, so it has to
 * cover the 4-hex group/LXMF form AND the 64-hex room form. */
WAPP_TEST(anylike_covers_both_id_lengths) {
  char mid[70]; int unlike = 9;
  WAPP_EXPECT_TRUE(anylike_parse("b9fb:unlike", mid, &unlike));
  WAPP_EXPECT_STR_EQ(mid, "b9fb");
  WAPP_EXPECT_INT_EQ(unlike, 1);
  WAPP_EXPECT_TRUE(anylike_parse(
      "82ccbaec1f0d4a5b6c7d8e9f00112233445566778899aabbccddeeff00112233:like",
      mid, &unlike));
  WAPP_EXPECT_INT_EQ((int)strlen(mid), 64);
  WAPP_EXPECT_INT_EQ(unlike, 0);
}

/* ── Votes that name their target by content ─────────────────────── */

WAPP_TEST(votemark_splits_id_from_content_key) {
  char mid[70]; int unlike = 9; const char *ck;
  WAPP_EXPECT_TRUE(votemark_parse("+like:9eb5 1a2b3c4d", mid, &unlike, &ck));
  WAPP_EXPECT_STR_EQ(mid, "9eb5");
  WAPP_EXPECT_STR_EQ(ck, "1a2b3c4d");
  WAPP_EXPECT_INT_EQ(unlike, 0);
  WAPP_EXPECT_TRUE(votemark_parse("+unlike:9eb5 1a2b3c4d", mid, &unlike, &ck));
  WAPP_EXPECT_INT_EQ(unlike, 1);
}

/* A message with no text (an image reference) has no content key: the id is
 * all there is, and the vote still has to parse. */
WAPP_TEST(votemark_without_a_key_still_parses) {
  char mid[70]; int unlike; const char *ck;
  WAPP_EXPECT_TRUE(votemark_parse("+like:9eb5", mid, &unlike, &ck));
  WAPP_EXPECT_STR_EQ(mid, "9eb5");
  WAPP_EXPECT_STR_EQ(ck, "");
}

/* The id may be any length — a 64-hex NOSTR event id from a room, or the
 * envelope hash an older build used. */
WAPP_TEST(votemark_takes_a_long_id) {
  const char *ev =
      "82ccbaec1f0d4a5b6c7d8e9f00112233445566778899aabbccddeeff00112233";
  char in[96]; strcpy(in, "+like:"); strcat(in, ev); strcat(in, " 1a2b3c4d");
  char mid[70]; int unlike; const char *ck;
  WAPP_EXPECT_TRUE(votemark_parse(in, mid, &unlike, &ck));
  WAPP_EXPECT_STR_EQ(mid, ev);
  WAPP_EXPECT_STR_EQ(ck, "1a2b3c4d");
}

WAPP_TEST(a_reply_is_not_a_vote) {
  char mid[70]; int unlike; const char *ck;
  WAPP_EXPECT_TRUE(!votemark_parse("+9eb5 OK", mid, &unlike, &ck));
  WAPP_EXPECT_TRUE(!votemark_parse("+like", mid, &unlike, &ck));
  WAPP_EXPECT_TRUE(!votemark_parse("+like:", mid, &unlike, &ck));
  WAPP_EXPECT_TRUE(!votemark_parse("I like: this", mid, &unlike, &ck));
}

WAPP_TEST(votemark_round_trips) {
  char wire[96]; char mid[70]; int unlike; const char *ck;
  votemark_wire(wire, sizeof(wire), "9eb5", 0, "1a2b3c4d");
  WAPP_EXPECT_STR_EQ(wire, "+like:9eb5 1a2b3c4d");
  WAPP_EXPECT_TRUE(votemark_parse(wire, mid, &unlike, &ck));
  WAPP_EXPECT_STR_EQ(mid, "9eb5");
  WAPP_EXPECT_STR_EQ(ck, "1a2b3c4d");

  votemark_wire(wire, sizeof(wire), "9eb5", 1, "");
  WAPP_EXPECT_STR_EQ(wire, "+unlike:9eb5");
}

/* Whatever the message, the vote is the same handful of bytes — these ride
 * Bluetooth, where a few hundred characters of quoted text would not be free. */
WAPP_TEST(a_vote_is_small_whatever_it_votes_on) {
  char wire[96];
  votemark_wire(wire, sizeof(wire), "9eb5", 0, "1a2b3c4d");
  WAPP_EXPECT_TRUE(strlen(wire) < 24);
}

/* ── The two paths, end to end ───────────────────────────────────── */

/*
 * What do_rooms_send does for an LXMF conversation, and what lxmf_drain does
 * with the result. The vote never becomes a message; the reply arrives as text
 * with a parent, on an id the sender can also compute.
 */
WAPP_TEST(lxmf_round_trip_reply_then_like) {
  const char *a_dest = "85cdc0319f2a4d1e7b0c5a6d8e9f0123";   /* who sent it */

  /* A sends "smth". Both ends derive the same id for it. */
  char a_mid[5];
  msg_id(a_dest, "smth", a_mid);

  /* B replies to it: the composer hands the send path "+<mid> OK". */
  char wire[64];
  thread_wire(wire, sizeof(wire), a_mid, "OK");

  /* Send path: not a vote, so it is a message — and the echo shows the text
   * WITHOUT the marker (the bug: it showed the marker). */
  char vote_mid[70]; int unlike;
  WAPP_EXPECT_TRUE(!anylike_parse(wire, vote_mid, &unlike));
  char parent[5]; const char *disp;
  WAPP_EXPECT_TRUE(thread_parse(wire, parent, &disp));
  WAPP_EXPECT_STR_EQ(disp, "OK");
  WAPP_EXPECT_STR_EQ(parent, a_mid);

  /* Receive path at A: same split, same parent, so the reply threads under
   * the message A sent. */
  char rx_parent[5]; const char *rx_disp;
  WAPP_EXPECT_TRUE(thread_parse(wire, rx_parent, &rx_disp));
  WAPP_EXPECT_STR_EQ(rx_disp, "OK");
  WAPP_EXPECT_STR_EQ(rx_parent, a_mid);

  /* B hearts A's message: the composer hands the send path
   * "+like:<mid> <content key>". That is a vote on both ends — never a
   * bubble — and it survives the trip with the key intact, which is what lets
   * A find the message even when A holds it under no id at all. */
  char heart[96];
  votemark_wire(heart, sizeof(heart), a_mid, 0, "1a2b3c4d");
  char tgt[70]; int ul = 9; const char *ck;
  WAPP_EXPECT_TRUE(votemark_parse(heart, tgt, &ul, &ck));
  WAPP_EXPECT_STR_EQ(tgt, a_mid);
  WAPP_EXPECT_STR_EQ(ck, "1a2b3c4d");
  WAPP_EXPECT_INT_EQ(ul, 0);

  /* And the older bare form still parses, so a peer that has not updated is
   * still understood. */
  char legacy[16]; strcpy(legacy, a_mid); strcat(legacy, ":like");
  WAPP_EXPECT_TRUE(anylike_parse(legacy, tgt, &ul));
  WAPP_EXPECT_STR_EQ(tgt, a_mid);
}
