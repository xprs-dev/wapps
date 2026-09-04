/*
 * db.h — the chat wapp's storage door, and the small string/JSON kit every
 * unit here shares.
 *
 * Storage is hal_sqlite_*: a database file under this wapp's private data
 * dir, created on open, WAL, encrypted under an encrypted profile. The same
 * door the circles wapp uses, wrapped the same way. There is one index
 * database (the rooms, the blocked set, the hidden set) and one database PER
 * CONVERSATION -- see db_init_room and room.c.
 */
#ifndef CHAT_DB_H
#define CHAT_DB_H

#include <stdint.h>

/* ── strings (no libc in wasm32-wasi -nostartfiles) ─────────────────── */
unsigned s_len(const char *s);
int  s_eq(const char *a, const char *b);
int  s_pre(const char *s, const char *pre);
void s_cpy(char *d, const char *s, unsigned m);
void s_cat(char *d, const char *s, unsigned m);
char s_up(char c);
void u_itoa(unsigned v, char *out);
void u_lltoa(unsigned long long v, char *out);

/* ── JSON, read ───────────────────────────────────────────────────── */
/* "key":"value" -> value, with \uXXXX (one byte), \n, \" and \\ decoded. */
int  jstr(const char *buf, const char *key, char *out, unsigned m);
/* "key":123 -> 123 (0 when absent). */
long long jint(const char *buf, const char *key);
/* "key":true / 1 -> 1, false / 0 -> 0, absent -> def. */
int  jbool_def(const char *buf, const char *key, int def);
int  jbool(const char *buf, const char *key);
/* One [key,value] pair of a delivered packet's "fields" array. */
int  jfield(const char *row, const char *key, char *out, unsigned m);
/* Copy the next {...} object at/after *cur into out (string-aware, so a `}`
 * inside somebody's message does not end the walk). Advances *cur. 1 = found. */
int  next_object(const char **cur, char *out, unsigned cap);

/* ── JSON, write ──────────────────────────────────────────────────── */
/* Append src to dst, JSON-escaped. */
void jesc(char *dst, unsigned m, const char *src);

/* A bind-parameter array for hal_sqlite: ["a",1,"b"]. */
typedef struct { char b[2400]; int first; } pj_t;
void        pj_init(pj_t *p);
void        pj_str(pj_t *p, const char *s);
void        pj_int(pj_t *p, long long v);
const char *pj_done(pj_t *p);

/* ── sqlite ───────────────────────────────────────────────────────── */
int  db_open(const char *relpath);            /* handle or -1 */
void db_close(int h);
int  db_exec(int h, const char *sql, const char *params);       /* 0 / -1 */
int  db_query(int h, const char *sql, const char *params,
              char *out, unsigned cap);       /* bytes, -1 error, -2 too small */
/* One integer out of a one-row, one-column query (e.g. SELECT count(*) AS n).
 * Returns def when the query fails or answers nothing. */
long long db_int(int h, const char *sql, const char *params, long long def);

/* Schemas, idempotent. */
void db_init_index(int h);
void db_init_room(int h);

#endif /* CHAT_DB_H */
