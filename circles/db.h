/*
 * db.h — thin wrappers over the hal_sqlite_* HAL plus the circles schema.
 * The index database lists the circles we belong to (and our master key for the
 * ones we own); each circle additionally has its own database file holding its
 * members, epoch keys and message events.
 */
#ifndef CIRCLES_DB_H
#define CIRCLES_DB_H

/* Open/create a database under the wapp data dir. Returns a handle or -1. */
int  db_open(const char *relpath);
void db_close(int h);

/* Run a statement / query. `params` is an optional JSON array (or NULL) bound to
 * '?' placeholders. db_query writes a JSON array of row objects into out and
 * returns bytes written, -1 on error, or -2 if the buffer was too small. */
int  db_exec(int h, const char *sql, const char *params);
int  db_query(int h, const char *sql, const char *params, char *out, unsigned cap);

/* Create the schema (idempotent). */
void db_init_index(int h);
void db_init_circle(int h);

#endif /* CIRCLES_DB_H */
