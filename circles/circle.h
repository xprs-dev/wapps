/*
 * circle.h — the Circles model: identities, membership keysets, epoch keys,
 * encrypted message events, and the peer-to-peer sync protocol. All circle
 * logic lives here (and in main.c's UI glue); the host only provides the generic
 * sqlite / crypto / rns HAL primitives.
 *
 * A Circle is a secp256k1 keypair (circleId = its x-only public key, hex). The
 * owner holds the master private key and signs the membership keyset. Members
 * are identified by their device x-only pubkey (base64url, as hal_identity_pubkey
 * emits). Messages are AES-256 encrypted under a shared per-Circle key that
 * rotates by epoch; each member keeps the epoch keys it is entitled to and can
 * request missing ones after being offline.
 */
#ifndef CIRCLES_CIRCLE_H
#define CIRCLES_CIRCLE_H

/* Open the index DB, load known circles, cache this device's identity, and
 * upsert a conversation row per circle. Call once at module_init. */
void circle_init(void);

/* Create a new circle named [name]; returns 1 on success. Generates the master
 * key, seeds epoch 1, adds ourselves as the first member, and shows the row. */
int circle_create(const char *name);

/* Add a member (by npub) to [circleId] — owner only. Bumps the epoch, rewraps
 * the new key to every member, and republishes the keyset. Returns 1 on success. */
int circle_add_member(const char *circleId, const char *npub);

/* Remove [circleId] from this device (delete if owned / leave if joined): drops
 * the index row, cache entry and host conversation. Returns 1 on success. */
int circle_delete(const char *circleId);

/* Encrypt + sign + store + broadcast a chat message to [circleId]. Returns 1. */
int circle_send(const char *circleId, const char *text);

/* Re-render all stored (decrypted) messages of [circleId] into its chat pane. */
void circle_render(const char *circleId);

/* Handle one inbound datagram (the inner JSON we/peers authored) from [from]
 * (the sender's RNS identity hex). */
void circle_on_datagram(const char *from, const char *json);

/* Periodic housekeeping (re-request missing epoch keys, light catch-up). */
void circle_tick(void);

/* ── Management panels (Edit / Share / People) ───────────────────────── */

/* Populate + open the full-screen Edit panel for [circleId] (owner). */
void circle_open_edit(const char *circleId);
/* Persist edited name/description for [circleId] and republish. */
void circle_save_edit(const char *circleId, const char *name, const char *desc);
/* Persist a newly-picked picture token for [circleId] and republish. */
void circle_set_picture(const char *circleId, const char *token);

/* ── Roles (proper add / edit-with-description / remove / default) ─────── */
void circle_open_roles(const char *circleId);
void circle_role_open_edit(const char *circleId, const char *roleName);
void circle_role_save(const char *circleId, const char *oldName,
                      const char *newName, const char *desc, int makeDefault);
void circle_role_remove(const char *circleId, const char *roleName);

/* Populate + open the full-screen Share panel (QR + copyable full key + short
 * reference code). */
void circle_open_share(const char *circleId);
/* Resolve a circle reference (full "circle:<hex>"/64-hex, or short
 * "circle/abc-xyz") to a full circleId among known circles. Returns 1 on a
 * unique match (writes outId), 0 if unknown or ambiguous. */
int circle_resolve_short(const char *code, char *outId, unsigned cap);

/* ── Joining ──────────────────────────────────────────────────────────── */
/* Apply to join a circle from a scanned/pasted reference (full key or QR
 * payload). Sends a signed join request to the circle's owner. */
int  circle_apply_join(const char *ref);
/* Owner: approve / reject a pending join applicant (by their pubkey). */
void circle_approve_request(const char *circleId, const char *pub);
void circle_reject_request(const char *circleId, const char *pub);

/* Populate + open the full-screen People panel for [circleId]. */
void circle_open_people(const char *circleId);
/* Comma-separated list of the circle's roles (for a role-picker prompt). */
void circle_roles_csv(const char *circleId, char *out, unsigned cap);
/* Set a member's role, or remove them from the circle, then republish. */
void circle_member_set_role(const char *circleId, const char *memberPub,
                            const char *role);
void circle_member_remove(const char *circleId, const char *memberPub);
/* Set a member's status: active | inactive | suspended | banned. */
void circle_member_set_status(const char *circleId, const char *memberPub,
                              const char *status);

/* ── Virtual folders (permissioned, nestable spaces) ──────────────────── */
/* Push the in-circle sub-folder rail (shown by default beside the circle chat
 * when a circle is opened). */
void circle_open_room_rail(const char *circleId);
/* Open the folder view (left rail of permitted sub-folders + the folder chat)
 * at the circle root. */
void circle_open_folders(const char *circleId);
/* Open a specific folder (rail = its sub-folders, active area = its chat). */
void circle_folder_view(const char *circleId, const char *folderId);
/* Folder editor (create under the current folder, or edit one). */
void circle_folder_open_edit(const char *circleId, const char *folderId);
void circle_folder_edit_current(const char *circleId);
/* Save the editor (name + description + icon + type) for the active target. */
void circle_folder_save(const char *circleId, const char *name,
                        const char *desc, const char *icon, const char *type);
void circle_folder_delete_current(const char *circleId);
/* Per-folder access editor (roles + individual allow/deny). */
void circle_folder_open_access(const char *circleId, const char *folderId);
void circle_folder_access_current(const char *circleId);
void circle_folder_role_toggle(const char *circleId, const char *role);
void circle_folder_member_cycle(const char *circleId, const char *memberPub);
/* Post to the currently-open folder chat. */
int  circle_folder_send(const char *circleId, const char *text);

#endif /* CIRCLES_CIRCLE_H */
