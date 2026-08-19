/*
 * aprs.h — reusable APRS client library for Aurora wapps.
 *
 * A small, self-contained C port of the APRS-IS logic from the XPRS
 * reference implementation, built entirely on the Aurora HAL socket
 * primitives (hal_socket_*). Any wapp can include this to talk to an
 * APRS-IS server: open a connection, log in (with a computed passcode),
 * stream/parse packets, and transmit messages or position beacons.
 *
 * Promotable to wapps/sdk/ to share across wapps.
 */
#ifndef APRS_H
#define APRS_H

#include <stdint.h>

/* APRS-IS defaults (from the reference). */
#define APRS_DEFAULT_HOST "rotate.aprs2.net"
#define APRS_DEFAULT_PORT 14580
#define APRS_MAX_MSG_LEN  67
#define APRS_MAX_COMMENT  107

/* Packet classification (matches the reference AprsPacketType order). */
enum {
  APRS_OTHER = 0,
  APRS_POSITION = 1,
  APRS_MESSAGE = 2,
  APRS_STATUS = 3,
  APRS_WEATHER = 4,
  APRS_TELEMETRY = 5
};

typedef struct {
  char from[16];      /* source callsign */
  int  type;          /* APRS_* */
  int  has_pos;       /* 1 if lat/lon valid */
  double lat, lon;
  char addressee[16]; /* message recipient (type==APRS_MESSAGE) */
  char text[160];     /* message body */
  char msgid[16];     /* message sequence id, "" if none */
  char comment[80];   /* position comment */
  int  is_bulletin;   /* 1 if addressee is a bulletin (BLN<id>[group]) */
  char group[8];      /* bulletin group name (1-5 chars), "" for general */
  char bulletin_id;   /* bulletin line id char '0'-'9' (or 'A'-'Z'), 0 if n/a */
} aprs_packet_t;

/* Compute the APRS-IS passcode for a callsign (port of aprsPasscode:
 * seed 0x73e2, XOR successive chars, mask 0x7FFF; uppercase, stop at '-'). */
int aprs_passcode(const char *callsign);

/* Open a TCP connection to an APRS-IS server. Returns a socket handle
 * (>=0) or -1. Poll aprs_is_open() before logging in. */
int aprs_connect(const char *host, int port);
int aprs_is_open(int handle);     /* 1 when the socket is connected */
void aprs_disconnect(int handle);

/* Build (without sending) the APRS-IS login line — exposed for tests. */
void aprs_build_login(char *out, unsigned max, const char *callsign,
                      int passcode, double lat, double lon, int radius_km);

/* Send the APRS-IS login line:
 *   user <call> pass <code> vers Aurora 1.0 filter r/<lat>/<lon>/<km>
 * Pass passcode = aprs_passcode(call) for TX, or -1 for receive-only. */
void aprs_login(int handle, const char *callsign, int passcode,
                double lat, double lon, int radius_km);

/* Build the server-side filter "r/<lat>/<lon>/<km>[ <extra>]". [extra] may be
 * "" or e.g. "g/CALL/BUDDY1/BUDDY2" to ALSO receive messages addressed to a
 * buddy list (used for store-and-forward of BLE-heard stations' mail). */
void aprs_build_filter(char *out, unsigned max, double lat, double lon,
                       int radius_km, const char *extra);

/* Log in with an extra filter term appended (e.g. a g/ buddy list). Pass
 * extra="" for the plain range filter (equivalent to aprs_login). */
void aprs_login_ex(int handle, const char *callsign, int passcode,
                   double lat, double lon, int radius_km, const char *extra);

/* Drain socket bytes and, if a full CRLF-delimited line is buffered,
 * copy it (without the CRLF) into line[max] and return its length.
 * Returns 0 when no complete line is ready. Skips APRS-IS comment
 * lines beginning with '#'. */
int aprs_poll_line(int handle, char *line, int max);

/* Parse one TNC2 line into out. Always fills from/type; sets has_pos
 * and the message/comment fields when present. Returns 1 on success. */
int aprs_parse(const char *line, aprs_packet_t *out);

/* Build (without sending) the TNC2 lines — exposed for testing.
 * No trailing CRLF; aprs_send_* append it before transmitting.
 * seq >= 0 appends the "{<seq>" message number; seq < 0 omits it (the
 * recipient must not ack the line — e.g. a re-originated third-party copy). */
void aprs_build_message(char *out, unsigned max, const char *from,
                        const char *to, const char *text, int seq);
/* Like aprs_build_message but with an explicit path (the part after ">APRS,").
 * Pass "TCPIP*" for IS-originated traffic, or "qAR,<igatecall>" when gating a
 * message heard on another medium (e.g. BLE) into APRS-IS. Empty/NULL = no via. */
void aprs_build_message_via(char *out, unsigned max, const char *from,
                            const char *to, const char *text, int seq,
                            const char *via);
void aprs_build_beacon(char *out, unsigned max, const char *from,
                       double lat, double lon, const char *sym,
                       const char *path, const char *comment);

/* Transmit a message: <from>>APRS::<DEST padded 9>:<text>{<seq> */
void aprs_send_message(int handle, const char *from, const char *to,
                       const char *text, int seq);

/* Transmit a pre-built TNC2 line verbatim (CRLF appended). Used to relay a
 * frame received on another transport onto APRS-IS. */
void aprs_send_raw(int handle, const char *line);

/* Long-message support (APRSdroid-style): a message longer than the APRS
 * body limit is split into chunks of at most max_len chars at word
 * boundaries (hard-breaking only a single word that itself exceeds the
 * limit), each transmitted as an independent message with its own
 * incrementing sequence number. The receiver merges consecutive parts.
 *
 * aprs_part_count  — number of parts text splits into at max_len.
 * aprs_split_text  — write the chunk at part index idx (0-based, <=max_len
 *                    chars + NUL) into out; returns 1 if that part exists,
 *                    0 once idx is past the end.
 * aprs_send_message_multi — split text and transmit every part; *seq is the
 *                    starting sequence number and is advanced past the last
 *                    part used. Returns the number of parts sent. */
int  aprs_part_count(const char *text, int max_len);
int  aprs_split_text(const char *text, int max_len, int idx,
                     char *out, unsigned out_sz);
int  aprs_send_message_multi(int handle, const char *from, const char *to,
                             const char *text, int max_len, int *seq);

/* APRS bulletins / group messaging (APRS spec ch.14).
 * A bulletin's addressee is "BLN" + a line id char + an optional group name
 * (1-5 chars), padded to 9: e.g. "BLN0EMCOM", "BLN0     " (general).
 * Multi-line bulletins reuse the line id (0,1,2,...) to order the lines, and
 * carry NO message number (bulletins are never acked).
 *
 * aprs_build_bulletin       — build one bulletin line (no CRLF).
 * aprs_send_bulletin_multi  — split text into <=max_len chunks and send each
 *                             as line 0,1,2,... (capped at 10 lines, BLN0-9).
 *                             Returns the number of lines sent. */
void aprs_build_bulletin(char *out, unsigned max, const char *from,
                         const char *group, char line_id, const char *text);
/* Like aprs_build_bulletin but with an explicit path (see aprs_build_message_via). */
void aprs_build_bulletin_via(char *out, unsigned max, const char *from,
                             const char *group, char line_id, const char *text,
                             const char *via);
int  aprs_send_bulletin_multi(int handle, const char *from, const char *group,
                              const char *text, int max_len);

/* Transmit a position beacon:
 *   <from>>APRS,<path>:!<DDMM.mmN><symtable><DDDMM.mmW><symcode><comment>
 * path may be "" (omitted). sym must be 2 chars (table+code). */
void aprs_send_beacon(int handle, const char *from, double lat, double lon,
                      const char *sym, const char *path, const char *comment);

#endif /* APRS_H */
