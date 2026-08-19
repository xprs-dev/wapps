#include <stdint.h>
#include "xprs_wasm_hal.h"
#include "db.h"
#include "util.h"

int db_open(const char *relpath) {
  return hal_sqlite_open(relpath, s_len(relpath));
}

void db_close(int h) {
  if (h >= 0) hal_sqlite_close(h);
}

int db_exec(int h, const char *sql, const char *params) {
  return hal_sqlite_exec(h, sql, s_len(sql),
                         params ? params : "", params ? s_len(params) : 0);
}

int db_query(int h, const char *sql, const char *params, char *out, unsigned cap) {
  return hal_sqlite_query(h, sql, s_len(sql),
                          params ? params : "", params ? s_len(params) : 0,
                          out, cap);
}

void db_init_index(int h) {
  db_exec(h,
          "CREATE TABLE IF NOT EXISTS circles("
          "id TEXT PRIMARY KEY,"   /* circle master pubkey hex (the circleId) */
          "name TEXT,"
          "master_priv TEXT,"      /* set only for circles we own */
          "created INTEGER)",
          0);
}

void db_init_circle(int h) {
  db_exec(h, "CREATE TABLE IF NOT EXISTS meta(k TEXT PRIMARY KEY, v TEXT)", 0);
  db_exec(h,
          "CREATE TABLE IF NOT EXISTS members("
          "pub TEXT PRIMARY KEY,"      /* member x-only pubkey, base64url */
          "added_epoch INTEGER,"
          "revoked_epoch INTEGER,"
          "role TEXT,"
          "status TEXT)",             /* active | inactive | suspended | banned */
          0);
  /* Migrate older circles (harmless errors if the columns already exist). */
  db_exec(h, "ALTER TABLE members ADD COLUMN role TEXT", 0);
  db_exec(h, "ALTER TABLE members ADD COLUMN status TEXT", 0);
  /* Address book: a member's RNS delivery + propagation dests (hex), learned
   * from the keyset / presence, so we can sync events ADDRESSED to them
   * (reliable + store-and-forward) instead of best-effort broadcast. */
  db_exec(h, "ALTER TABLE members ADD COLUMN deliv TEXT", 0);
  db_exec(h, "ALTER TABLE members ADD COLUMN prop TEXT", 0);
  /* Member-management audit log (who did what, when). */
  db_exec(h,
          "CREATE TABLE IF NOT EXISTS audit("
          "id INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER, action TEXT, who TEXT)",
          0);
  db_exec(h, "CREATE TABLE IF NOT EXISTS roles("
             "name TEXT PRIMARY KEY, description TEXT, created INTEGER)", 0);
  /* Migrate role tables created before the description column existed. */
  db_exec(h, "ALTER TABLE roles ADD COLUMN description TEXT", 0);
  db_exec(h, "CREATE TABLE IF NOT EXISTS epochs(epoch INTEGER PRIMARY KEY, key TEXT)", 0);
  db_exec(h,
          "CREATE TABLE IF NOT EXISTS events("
          "id TEXT PRIMARY KEY,"   /* sha-like content id (hex of sig prefix) */
          "epoch INTEGER,"
          "author TEXT,"           /* author x-only pubkey base64url */
          "ts INTEGER,"
          "ct TEXT,"               /* ciphertext, base64 (IV||AES-256-CBC) */
          "sig TEXT,"              /* author signature (base85) over canonical */
          "body TEXT)",            /* decrypted plaintext, NULL until decryptable */
          0);
  /* ── Virtual folders: a permissioned, nestable tree inside the circle. ──
   * Each folder is its own access-controlled space (a chat now; blog/gallery/
   * forum/sub-folders later). Entry is crypto-enforced: a folder's content is
   * encrypted with a per-folder key handed only to members whose role (or
   * individual allow) permits entry, rotated when permissions change. */
  db_exec(h,
          "CREATE TABLE IF NOT EXISTS folders("
          "id TEXT PRIMARY KEY,"   /* short random hex id */
          "parent TEXT,"           /* parent folder id, '' = circle root */
          "name TEXT,"
          "type TEXT,"             /* chat | folder | blog | gallery | forum */
          "description TEXT,"
          "icon TEXT,"             /* Material icon name */
          "roles TEXT,"            /* CSV of role names allowed to enter */
          "allow TEXT,"            /* CSV of member pubkeys always allowed */
          "deny TEXT,"             /* CSV of member pubkeys always denied */
          "epoch INTEGER,"         /* current folder-key epoch */
          "created INTEGER)",
          0);
  /* Migrate folder tables created before description/icon existed. */
  db_exec(h, "ALTER TABLE folders ADD COLUMN description TEXT", 0);
  db_exec(h, "ALTER TABLE folders ADD COLUMN icon TEXT", 0);
  db_exec(h,
          "CREATE TABLE IF NOT EXISTS folder_keys("
          "folder TEXT, epoch INTEGER, key TEXT,"
          "PRIMARY KEY(folder,epoch))",
          0);
  db_exec(h,
          "CREATE TABLE IF NOT EXISTS folder_events("
          "id TEXT PRIMARY KEY, folder TEXT, epoch INTEGER, author TEXT,"
          "ts INTEGER, ct TEXT, sig TEXT, body TEXT)",
          0);
  /* Pending join applications (owner side): someone who scanned the circle key
   * and asked to enter. The owner approves → they become a member. */
  db_exec(h,
          "CREATE TABLE IF NOT EXISTS requests("
          "pub TEXT PRIMARY KEY, nick TEXT, ts INTEGER)",
          0);
  /* The applicant's RNS dests (from the join request) so approval can deliver
   * the keyset/key ADDRESSED to them (reliable) without re-discovery. */
  db_exec(h, "ALTER TABLE requests ADD COLUMN deliv TEXT", 0);
  db_exec(h, "ALTER TABLE requests ADD COLUMN prop TEXT", 0);
}
