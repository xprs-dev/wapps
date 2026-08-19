/*
 * APRS-IS LIVE test — runs inside the editor's Tests tab via "Run live".
 *
 * The blocking hal_socket_*_sync HAL lets this synchronous wasm test do
 * real network I/O within module_run_tests. It connects to the real
 * server, decodes real packets through the shipped aprs.c, and writes a
 * frame on the wire (send path) — and emits the actual evidence
 * (banner, raw received frames, the frame sent) as `tests.log` messages
 * so the Tests-tab log window shows it really worked.
 *
 * Offline-tolerant (returns as a pass-skip when unreachable).
 * Declarations only — aprs.c's impl is pulled in by test_aprs.c.
 */
#include "wapp_test.h"
#include "../chat.h"

/* Emit a line into the Tests-tab log window. */
static void tlog(const char *s) {
  char m[700];
  const char *pre = "{\"type\":\"tests.log\",\"text\":\"";
  unsigned o = 0;
  for (const char *p = pre; *p; p++) m[o++] = *p;
  for (const char *p = s; *p && o < sizeof(m) - 8; p++) {
    char c = *p;
    if (c == '"' || c == '\\') { m[o++] = '\\'; m[o++] = c; }
    else if (c == '\n' || c == '\r') { m[o++] = ' '; }
    else m[o++] = c;
  }
  m[o++] = '"'; m[o++] = '}';
  hal_msg_send(m, o);
}

WAPP_TEST(live_aprs_is_roundtrip) {
  const char *host = "rotate.aprs2.net";
  tlog("connecting to rotate.aprs2.net:14580 ...");
  int h = hal_socket_open_sync(host, (uint32_t)strlen(host), 14580);
  if (h < 0) { tlog("offline - skipping live test"); return; }

  static char acc[8192];
  int acclen = 0;
  char rd[2048];

  /* 1) Read the server banner ('# ...') — proof of bidirectional comms. */
  char banner[180] = "";
  uint64_t t0 = hal_time_ms();
  while (hal_time_ms() - t0 < 4000 && !banner[0]) {
    int n = hal_socket_read_sync(h, rd, sizeof(rd) - 1);
    if (n > 0 && rd[0] == '#') {
      int i = 0;
      while (rd[i] && rd[i] != '\r' && rd[i] != '\n' && i < 178) {
        banner[i] = rd[i]; i++;
      }
      banner[i] = 0;
    }
  }
  if (!banner[0]) { tlog("no banner - skipping"); hal_socket_close_sync(h); return; }
  { char b[220]; b[0] = 0; strncat(b, "banner: ", sizeof(b) - 1);
    strncat(b, banner, sizeof(b) - 1 - strlen(b)); tlog(b); }

  /* 2) Log in read-only (passcode -1), filter around Lisbon. */
  char login[256];
  aprs_build_login(login, sizeof(login), "N0CALL", -1, 38.7223, -9.1393, 300);
  int lw = hal_socket_write_sync(h, login, (uint32_t)strlen(login));
  WAPP_EXPECT_TRUE(lw > 0);
  tlog("logged in (read-only); receiving real packets...");

  /* 3) Receive real packets and parse them with aprs.c. Log the first
   *    few raw frames as proof of real data on the wire. */
  int positions = 0, logged = 0;
  t0 = hal_time_ms();
  while (hal_time_ms() - t0 < 9000 && positions < 3) {
    int n = hal_socket_read_sync(h, acc + acclen, (uint32_t)(sizeof(acc) - 1 - acclen));
    if (n > 0) acclen += n;
    int start = 0;
    for (int i = 0; i < acclen; i++) {
      if (acc[i] != '\n') continue;
      acc[i] = 0;
      if (i > start && acc[i - 1] == '\r') acc[i - 1] = 0;
      const char *line = acc + start;
      if (line[0] && line[0] != '#') {
        aprs_packet_t p;
        if (aprs_parse(line, &p) && p.type == APRS_POSITION && p.has_pos) {
          positions++;
          if (logged < 3) {
            char b[600]; b[0] = 0;
            strncat(b, "RX  ", sizeof(b) - 1);
            strncat(b, line, sizeof(b) - 1 - strlen(b));
            tlog(b); logged++;
          }
        }
      }
      start = i + 1;
    }
    if (start > 0) {
      for (int i = start; i < acclen; i++) acc[i - start] = acc[i];
      acclen -= start;
    }
    if (acclen >= (int)sizeof(acc) - 1) acclen = 0;
  }
  { char b[64]; b[0] = 0; strncat(b, "received positions: ", sizeof(b) - 1);
    char nb[12]; int v = positions, j = 0, k = 0; char t[12];
    if (v == 0) t[j++] = '0'; while (v > 0) { t[j++] = (char)('0' + v % 10); v /= 10; }
    while (j > 0) nb[k++] = t[--j]; nb[k] = 0;
    strncat(b, nb, sizeof(b) - 1 - strlen(b)); tlog(b); }
  WAPP_EXPECT_TRUE(positions >= 1); /* real station(s) received & parsed */

  /* 4) Send path: write a self-addressed message on the live socket. */
  char frame[300];
  aprs_build_message(frame, sizeof(frame), "N0CALL", "N0CALL",
                     "aurora live self-test", 1);
  int fl = (int)strlen(frame);
  frame[fl] = '\r'; frame[fl + 1] = '\n'; frame[fl + 2] = 0;
  int w = hal_socket_write_sync(h, frame, (uint32_t)(fl + 2));
  WAPP_EXPECT_TRUE(w > 0);
  { char b[360]; b[0] = 0; strncat(b, "TX  ", sizeof(b) - 1);
    frame[fl] = 0; /* drop CRLF for display */
    strncat(b, frame, sizeof(b) - 1 - strlen(b));
    strncat(b, "   (write accepted)", sizeof(b) - 1 - strlen(b)); tlog(b); }

  hal_socket_close_sync(h);
}
