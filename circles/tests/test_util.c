/*
 * Unit tests for the circles wapp's encoding helpers (hex / base64url / bech32 /
 * JSON). Pure logic — no HAL needed. The crypto, epoch and sync paths are
 * exercised live across two devices (see the plan's verification section).
 */
#include "wapp_test.h"
#include "../util.c"   /* pull in the helpers directly (single test TU) */

WAPP_TEST(hex_roundtrip) {
  unsigned char in[4] = { 0x00, 0x7f, 0xab, 0xff };
  char hex[16];
  hex_encode(in, 4, hex);
  WAPP_EXPECT_STR_EQ(hex, "007fabff");
  unsigned char out[4];
  WAPP_EXPECT_INT_EQ(hex_decode(hex, out, sizeof(out)), 4);
  WAPP_EXPECT_TRUE(out[0] == 0x00 && out[1] == 0x7f && out[2] == 0xab && out[3] == 0xff);
  WAPP_EXPECT_INT_EQ(hex_decode("0a0", out, sizeof(out)), -1); /* odd length */
}

WAPP_TEST(b64url_roundtrip) {
  unsigned char in[5] = { 'h', 'e', 'l', 'l', 'o' };
  char enc[16];
  int n = b64url_encode(in, 5, enc, sizeof(enc));
  WAPP_EXPECT_TRUE(n > 0);
  WAPP_EXPECT_STR_EQ(enc, "aGVsbG8");   /* base64url, no padding */
  unsigned char dec[8];
  WAPP_EXPECT_INT_EQ(b64url_decode(enc, dec, sizeof(dec)), 5);
  WAPP_EXPECT_TRUE(wapp__memeq(dec, in, 5));
}

WAPP_TEST(b64url_32bytes) {
  unsigned char in[32];
  for (int i = 0; i < 32; i++) in[i] = (unsigned char)(i * 7 + 1);
  char enc[64]; unsigned char dec[32];
  WAPP_EXPECT_TRUE(b64url_encode(in, 32, enc, sizeof(enc)) == 43);
  WAPP_EXPECT_INT_EQ(b64url_decode(enc, dec, sizeof(dec)), 32);
  WAPP_EXPECT_TRUE(wapp__memeq(dec, in, 32));
}

WAPP_TEST(npub_decode_vector) {
  /* Canonical NIP-19 example. */
  unsigned char pk[32];
  int rc = npub_decode(
      "npub1sg6plzptd64u62a878hep2kev88swjh3tw00gjsfl8f237lmu63q0uf63m", pk);
  WAPP_EXPECT_INT_EQ(rc, 0);
  char hex[80];
  hex_encode(pk, 32, hex);
  WAPP_EXPECT_STR_EQ(
      hex, "82341f882b6eabcd2ba7f1ef90aad961cf074af15b9ef44a09f9d2a8fbfbe6a2");
}

WAPP_TEST(npub_decode_rejects_bad) {
  unsigned char pk[32];
  WAPP_EXPECT_TRUE(npub_decode("npub1notvalidchecksum000000", pk) != 0);
  WAPP_EXPECT_TRUE(npub_decode("nsec1abcdef", pk) != 0);   /* wrong hrp */
  WAPP_EXPECT_TRUE(npub_decode("", pk) != 0);
}

WAPP_TEST(json_helpers) {
  const char *buf = "{\"command\":\"conversations_send\",\"conversations_convo\":\"abc\","
                    "\"conversations_input\":\"hi there\",\"e\":7,\"t\":-3}";
  char v[64];
  WAPP_EXPECT_TRUE(jstr(buf, "conversations_convo", v, sizeof(v)));
  WAPP_EXPECT_STR_EQ(v, "abc");
  WAPP_EXPECT_TRUE(jstr(buf, "conversations_input", v, sizeof(v)));
  WAPP_EXPECT_STR_EQ(v, "hi there");
  long e = 0;
  WAPP_EXPECT_TRUE(jint(buf, "e", &e));
  WAPP_EXPECT_INT_EQ(e, 7);
  WAPP_EXPECT_TRUE(jint(buf, "t", &e));
  WAPP_EXPECT_INT_EQ(e, -3);
  WAPP_EXPECT_FALSE(jstr(buf, "missing", v, sizeof(v)));
}
