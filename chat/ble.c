#include "ble.h"
#include "xprs_wasm_hal.h"

static unsigned b_len(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }

void ble_start(void) { hal_ble_scan_start(); }

void ble_stop(void) {
  hal_ble_scan_stop();
  hal_ble_advertise_stop();
}

int ble_poll(char *buf, unsigned max) {
  if (max == 0) return 0;
  unsigned n = hal_ble_scan_read(buf, max - 1);
  buf[n < max ? n : max - 1] = 0;
  return (int)n;
}

void ble_send(const char *frame) {
  if (frame && frame[0]) hal_ble_advertise(frame, b_len(frame));
}
