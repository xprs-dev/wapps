#ifndef ROOM_H
#define ROOM_H

/*
 * room.h — Chat rooms as NIP-72 moderated communities (kind 34550) plus a
 * custom, npub-signed moderation op-log (kind 9078), reduced client-side.
 *
 * Everything about rooms, roles, moderation and reputation lives in the wapp
 * (the host stays generic). Rooms federate to ANY standard NOSTR relay: a room
 * is a NIP-72 community, room messages are ordinary kind-1 notes tagged to it,
 * and non-XPRS clients simply ignore our custom op kind.
 *
 * Authority is subtree-scoped. The room tree has one root, the main room, whose
 * admin is the GLOBAL admin and whose moderators are GLOBAL mods (authority over
 * everything). Every sub-room has its own sub-admin (its proposer) and sub-mods,
 * whose authority covers that room and its descendants only. An op on room R is
 * honoured only from the global admin, a global mod, or the admin/mod of R or of
 * any ANCESTOR of R.
 *
 * Reputation is global per pubkey, level 1..10, computed deterministically from
 * the trailing 6 months of participation plus net moderation points.
 *
 * Spec: aurora/docs/chat-rooms.md. This is a self-contained translation unit; it
 * talks to the host only through the generic HALs.
 */

/* Event kinds. 34550/1 are standard; 9078 is our custom op-log (regular event,
 * stored by any relay, ignored by clients that don't know it). */
#define KIND_ROOM_DEF      34550
#define KIND_ROOM_MSG      1
#define KIND_ROOM_OP       9078
#define KIND_ROOM_PROPOSAL 9079  /* someone asks to create a sub-room */
#define KIND_ROOM_APPROVAL 9080  /* a parent authority approves a proposal */

/* The root room. Its admin pubkey is the GLOBAL admin. This is the project key;
 * it is published (the 34550) by whoever runs the project. Replace before
 * release; during bring-up it can be set to the test device's own pubkey so the
 * tester is the global admin. */
#define MAIN_ROOM_ID "main"
extern char ROOM_MAIN_ADMIN[65]; /* hex x-only pubkey, or "" until configured */

/* Open the DB, ensure the schema, learn the self pubkey, seed the main-room row.
 * Safe to call again once the self pubkey becomes known. */
void room_init(void);

/* NIP-01 filter (JSON) subscribing to room defs (34550), ops (9078) and messages
 * (kind 1, #h) for every known room. Writes into [out]; returns its length. */
unsigned room_sub_filter(char *out, unsigned cap);

/* Feed one NOSTR event (the JSON hal_nostr_event_recv returns). Returns 1 if it
 * was a room event we consumed (def / op / room message), else 0. */
int room_ingest(const char *event_json);

/* Is [id] a known room id (so a conversation-send should post to it)? */
int room_is_room(const char *id);

/* If [event_json] is a kind-1 room message (has an #h naming a known room),
 * record it for reputation and write its roomId into [out], returning 1 so the
 * caller renders it through the normal conversation pipeline; else 0. */
int room_note_roomid(const char *event_json, char *out, unsigned cap);

/* May the current user post in [roomId] now? 0 if closed / self banned or
 * suspended (client-enforced soft gating). */
int room_self_can_post(const char *roomId);

/* Post [text] to room [roomId] as a NIP-72 community message. Returns 1 ok. */
int room_post(const char *roomId, const char *text);

/* Publish a NIP-25 reaction (kind 7) on room message [mid]: like, or retract
 * when [remove]. Returns 1 if published. */
int room_react(const char *roomId, const char *mid, int remove);

/* Publish a moderation op if self has authority over [roomId]. [op] is one of
 * kick / suspend / unsuspend / ban / close / award / deduct / promote / demote.
 * [until] is a unix-seconds deadline (suspend), [amount] the points delta
 * (award/deduct). Returns 1 if published, 0 if not authorised or invalid. */
int room_moderate(const char *roomId, const char *op, const char *target_pub,
                  long until, int amount, const char *reason);

/* Does [pub] (hex) have moderation authority over [roomId] (subtree walk)? */
int room_has_authority(const char *pub, const char *roomId);

/* Does the current user have authority over [roomId]? */
int room_self_authority(const char *roomId);

/* Ban [target_pub] from the whole wapp (global op). Global authority only. */
int room_ban_wapp(const char *target_pub);

/* Is [pub] our own pubkey (used to drop our own federated-back copy)? */
int room_is_self(const char *pub);

/* Create a sub-room named [name] under [parentId] (empty = top level): publish a
 * NIP-72 34550 with self as admin + parent link. Returns 1. */
int room_create(const char *parentId, const char *name);

/* Propose a sub-room [name] under [parentId]. If self already has authority over
 * the parent it is created immediately; otherwise a proposal (9079) is published
 * for a parent authority to approve. Returns 1. */
int room_propose(const char *parentId, const char *name);

/* Approve proposal [proposalId] if self has authority over its parent (publishes
 * a 9080). The proposer then publishes the sub-room, becoming its admin, with the
 * approver added as a moderator. Returns 1 if published. */
int room_approve(const char *proposalId);

/* Newest still-pending proposal this user can act on (authority over its parent).
 * Writes its id/name/parent; returns 1 if one exists, else 0. */
int room_newest_pending(char *id, unsigned idcap, char *name, unsigned namecap,
                        char *parent, unsigned pcap);

/* Global reputation level 1..10 for [pub] (hex). */
int room_rep_level(const char *pub);

/* Rooms whose name or id matches [q] (case-insensitive substring; empty [q]
 * matches all). Writes comma-separated JSON items {"id","name"} — no
 * brackets — into [out]. Returns how many matched. */
int room_search(const char *q, char *out, unsigned cap);

/* Does [roomId] hang under the main room? The rail draws that tree, so a room
 * joined from search that sits outside it needs listing separately. */
int room_on_main_tree(const char *roomId);

/* The room's display name (its id when unnamed). */
void room_name_of(const char *roomId, char *out, unsigned cap);

/* Distinct authors seen posting in [roomId]. NOT a membership count — rooms
 * have no roster (nobody publishes join/leave), so this is what we have
 * actually observed, and callers must label it "seen". */
int room_people_seen(const char *roomId);

/* Render the room list (indented tree) into the conversations widget. */
void room_render_tree(void);

/* Same, appending [extra_items] (pre-built JSON rail items — the caller's
 * broadcast channels) after the room tree. NULL/empty = rooms only. */
void room_render_tree_with(const char *extra_items);

/* Render the member roster for [roomId] into the Members people-list field. */
void room_render_members(const char *roomId);

#endif /* ROOM_H */
