/*
 * util.h — tiny string / JSON / hex / base64url / bech32 helpers for the
 * circles wapp. Kept dependency-free (only libc-free primitives) so the same
 * code builds for app.wasm and the test runner.
 */
#ifndef CIRCLES_UTIL_H
#define CIRCLES_UTIL_H

/* ── strings ──────────────────────────────────────────────────────────── */
unsigned s_len(const char *s);
int      s_eq(const char *a, const char *b);
void     s_cpy(char *d, const char *s, unsigned m);
void     s_cat(char *d, const char *s, unsigned m);

/* ── JSON (flat objects, host wire format) ────────────────────────────── */
/* Append `src` to `dst` (capacity m), JSON-escaping " \ and newlines. */
void jesc(char *dst, unsigned m, const char *src);
/* Extract a string field "key":"value" into out; returns 1 if found. */
int  jstr(const char *buf, const char *key, char *out, unsigned m);
/* Extract a numeric field "key":<number> into *out; returns 1 if found. */
int  jint(const char *buf, const char *key, long *out);
/* Read a boolean field "key":true/false/1/0. Returns 1 for true, else 0. */
int  jbool(const char *buf, const char *key);
/* Extract the next {...} object from *cur into out (quote/escape aware), and
 * advance *cur past it. Returns 1 if an object was found, 0 otherwise. Lets a
 * caller walk a JSON array of flat row objects. */
int  next_object(const char **cur, char *out, unsigned cap);

/* ── hex ──────────────────────────────────────────────────────────────── */
/* Encode n bytes to 2n lowercase hex chars + NUL (out must hold 2n+1). */
void hex_encode(const unsigned char *in, unsigned n, char *out);
/* Decode a hex string into out (max maxout bytes). Returns byte count, or -1. */
int  hex_decode(const char *in, unsigned char *out, unsigned maxout);

/* ── base64url (no padding) ───────────────────────────────────────────── */
/* Encode n bytes; writes NUL-terminated string to out. Returns chars, or -1. */
int  b64url_encode(const unsigned char *in, unsigned n, char *out, unsigned maxout);
/* Decode; returns byte count written to out, or -1. */
int  b64url_decode(const char *in, unsigned char *out, unsigned maxout);

/* ── bech32 (NIP-19) ──────────────────────────────────────────────────── */
/* Decode an npub… string to its 32-byte x-only public key. Returns 0 on
 * success, -1 on any malformed input / bad checksum / wrong hrp. */
int  npub_decode(const char *npub, unsigned char out32[32]);

#endif /* CIRCLES_UTIL_H */
