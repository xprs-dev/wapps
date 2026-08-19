/*
 * APRS-IS LIVE integration test — exercises the real aprs.c against the
 * real APRS-IS network (rotate.aprs2.net:14580).
 *
 * This is NOT part of the wasm unit suite (tests/test_*.c): the wasm
 * runner is synchronous and can't wait on async network I/O, so a live
 * test can't run inside the editor's Tests tab. Instead this is a
 * standalone native harness that backs the Aurora HAL socket calls
 * (hal_socket_*) with real POSIX sockets and links the SAME aprs.c the
 * wapp ships — so it genuinely tests our receive + send code on the wire.
 *
 * Build + run (see run.sh in this folder):
 *   cc -I<wapps/hal> tests/integration/it_aprs_is.c -o it_aprs && ./it_aprs
 *
 * Exit 0 = pass, non-zero = fail. Skips (exit 0 with SKIP) when the
 * server is unreachable so CI without internet stays green.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>

/* ── HAL socket backing: real POSIX sockets ──────────────────────── */
static int g_fd = -1;
static long g_sent_total = 0;

int32_t hal_socket_open(const char *host, uint32_t hl, int32_t port) {
  char h[256];
  if (hl >= sizeof(h)) hl = sizeof(h) - 1;
  memcpy(h, host, hl); h[hl] = 0;
  char p[8]; snprintf(p, sizeof(p), "%d", port);
  struct addrinfo hints, *res = 0;
  memset(&hints, 0, sizeof(hints));
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(h, p, &hints, &res) != 0 || !res) return -1;
  int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (fd < 0) { freeaddrinfo(res); return -1; }
  if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
    close(fd); freeaddrinfo(res); return -1;
  }
  freeaddrinfo(res);
  g_fd = fd;
  return 1;
}
int32_t hal_socket_status(int32_t h) { (void)h; return g_fd >= 0 ? 1 : 2; }
int32_t hal_socket_send(int32_t h, const char *b, uint32_t n) {
  (void)h;
  ssize_t w = send(g_fd, b, n, 0);
  if (w > 0) g_sent_total += w;
  return (int32_t)w;
}
uint32_t hal_socket_recv(int32_t h, char *b, uint32_t n) {
  (void)h;
  fd_set fds; FD_ZERO(&fds); FD_SET(g_fd, &fds);
  struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 200000; /* 200ms */
  if (select(g_fd + 1, &fds, 0, 0, &tv) <= 0) return 0;
  ssize_t r = recv(g_fd, b, n, 0);
  return r > 0 ? (uint32_t)r : 0;
}
void hal_socket_close(int32_t h) { (void)h; if (g_fd >= 0) close(g_fd); g_fd = -1; }

/* Other HAL symbols aprs.c references. */
void hal_log(int32_t l, const char *m, uint32_t n) { (void)l; (void)m; (void)n; }
uint64_t hal_time_ms(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
uint64_t hal_time_epoch(void) { return (uint64_t)time(0); }

/* The code under test. */
#include "../../chat.c"

/* ── the test ─────────────────────────────────────────────────────── */
int main(void) {
  const char *HOST = "rotate.aprs2.net";
  const int PORT = 14580;
  printf("== APRS-IS live integration (%s:%d) ==\n", HOST, PORT);

  int h = aprs_connect(HOST, PORT);
  if (h < 0 || !aprs_is_open(h)) {
    printf("SKIP: APRS-IS unreachable (no internet?) — not a failure.\n");
    return 0;
  }

  /* 1) Bidirectional comms: read the server banner ('# javAPRSSrvr ...'). */
  char banner[512] = "";
  for (int i = 0; i < 10 && banner[0] == 0; i++) {
    char tmp[512];
    uint32_t n = hal_socket_recv(h, tmp, sizeof(tmp) - 1);
    if (n > 0) { tmp[n] = 0; snprintf(banner, sizeof(banner), "%s", tmp); }
  }
  printf("banner: %.120s\n", banner[0] ? banner : "(none)");
  int banner_ok = banner[0] == '#';

  /* 2) Log in read-only (passcode -1) with a filter around Lisbon. */
  aprs_login(h, "N0CALL", -1, 38.7223, -9.1393, 250);

  /* 3) RECEIVE real packets and parse them with aprs.c. */
  int lines = 0, positions = 0, messages = 0;
  char first_pos[64] = "";
  time_t t0 = time(0);
  while (time(0) - t0 < 15) {
    char line[600];
    int n = aprs_poll_line(h, line, sizeof(line));
    if (n <= 0) continue;
    lines++;
    aprs_packet_t p;
    if (!aprs_parse(line, &p)) continue;
    if (p.type == APRS_POSITION && p.has_pos) {
      positions++;
      if (!first_pos[0])
        snprintf(first_pos, sizeof(first_pos), "%s @ %.4f,%.4f",
                 p.from, p.lat, p.lon);
    } else if (p.type == APRS_MESSAGE) {
      messages++;
    }
    if (positions >= 3) break;
  }
  printf("RX: lines=%d positions=%d messages=%d  first=%s\n",
         lines, positions, messages, first_pos[0] ? first_pos : "(none)");

  /* 4) SEND path: write a self-addressed message over the live socket.
   *    With a read-only (passcode -1) login the server DROPS it, so the
   *    global network is not polluted — but the real socket write must
   *    succeed, proving our transmit path works on the wire. */
  long before = g_sent_total;
  aprs_send_message(h, "N0CALL", "N0CALL", "aurora send-path self-test", 1);
  long wrote = g_sent_total - before;
  printf("TX: wrote %ld bytes to the live socket\n", wrote);

  /* 5) Session still alive after sending? Keep reading briefly. */
  int after_lines = 0;
  time_t t1 = time(0);
  while (time(0) - t1 < 4) {
    char line[600];
    if (aprs_poll_line(h, line, sizeof(line)) > 0) after_lines++;
    if (after_lines >= 1) break;
  }
  printf("post-send RX lines: %d\n", after_lines);

  aprs_disconnect(h);

  int ok_rx = banner_ok && positions >= 1;
  int ok_tx = wrote > 0 && after_lines >= 1;
  printf("\nRESULT: receive=%s  send=%s\n",
         ok_rx ? "PASS" : "FAIL", ok_tx ? "PASS" : "FAIL");
  return (ok_rx && ok_tx) ? 0 : 1;
}
