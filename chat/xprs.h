/*
 * xprs — the chat wapp's wire format on the licence-free transports.
 *
 * Everything chat airs over Bluetooth and Reticulum used to be a compact
 * three-field frame ("<from>\x1f<to>\x1f<text>") invented here. XPRS
 * (docs/XPRS.md in the aurora repo) is the format the whole device now speaks,
 * so the same content goes out as `key:value` fields that any XPRS station —
 * including one that has never heard of this wapp — can read.
 *
 * This unit is only the ENVELOPE. What chat puts inside `m:` (ENC1 ciphertext,
 * a ~signature, a reply marker) is untouched, so nothing above these two
 * functions has to change.
 *
 * Chat's internal routing convention is unchanged too, so the mapping is:
 *
 *   to = "X1RD89"   1:1        t:message f:.. d:X1RD89 ts:.. m:text
 *   to = "#LISBOA"  group      t:message f:.. d:LISBOA ts:.. m:text
 *   to = ""         in range   t:message f:.. ts:.. m:text
 *   to = "!"        position   t:observation f:.. ts:.. pos:LAT,LON m:comment
 *
 * `?MAIL`, `?IGATE`, `?PING` and friends are wapp-to-wapp control frames with
 * no XPRS meaning yet; xprs_pack refuses them and the caller airs the compact
 * form, which both ends still read.
 */
#ifndef XPRS_H
#define XPRS_H

/* Build an XPRS packet for chat's (from, to, text). Returns the length
 * written, or 0 when this frame has no XPRS form (control frames) or would not
 * fit [max]. */
unsigned xprs_pack(char *out, unsigned max, const char *from, const char *to,
                   const char *text, unsigned long long now);

/* Is [wire] an XPRS packet? Cheap prefix test — every packet starts `t:`. */
int xprs_looks_like(const char *wire);

/* Read an XPRS packet back into chat's (from, to, text). Returns 0 when the
 * packet is not one chat can show (unknown type, no sender). [ts_out] receives
 * the SENDER's timestamp as an epoch, or 0 when the packet carried none —
 * something the compact frame never had. */
int xprs_unpack(const char *wire, char *from, unsigned fmax, char *to,
                unsigned tmax, char *text, unsigned xmax,
                unsigned long long *ts_out);

/* Epoch <-> "YYYY-MM-DD_HH:MM:SS" UTC, XPRS section 4.8. Exposed for tests. */
void xprs_stamp(char *out, unsigned max, unsigned long long epoch);
unsigned long long xprs_parse_stamp(const char *s);

/* Does an XPRS address name a station rather than a group (section 6.3)? */
int xprs_is_station(const char *addr);

#endif
