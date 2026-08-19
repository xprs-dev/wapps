#ifndef CHAT_THREAD_H
#define CHAT_THREAD_H
/*
 * Thread ids, reply markers and like votes — the wire conventions that let a
 * reply and a heart travel over transports with no room for extra fields
 * (APRS-IS, BLE, LXMF). Pure functions with no HAL dependency, kept out of
 * main.c so tests/test_thread.c can cover them directly.
 *
 * A message's id is derived, not assigned: both sides compute the same 4 hex
 * characters from "<from>|<text>", so a reply or a vote can name its target
 * without either side having to store or exchange an id.
 */

/* First 4 hex chars of sha1("<from>|<text>") — a message's thread id.
 * [from] must be something both sides agree on: the sender's callsign on a
 * group, the sender's LXMF delivery dest on a direct conversation. */
void msg_id(const char *from, const char *text, char out[5]);

/* Reply marker on the wire: "+<4hex> <text>". On match copies the parent id
 * into [parent] and points *disp at the text after the marker; otherwise
 * parent="" and *disp = wire. Returns 1 if a marker was found.
 * A 64-hex marker (an older build naming its parent by LXMF envelope hash or
 * NOSTR event id, which only one side ever knows) is stripped with an empty
 * parent — unresolvable, but still not text the user typed. */
int thread_parse(const char *wire, char parent[5], const char **disp);

/* Compose the wire form of a reply: "+<parent> <text>", or plain [text] when
 * [parent] is empty. Returns [out]. */
char *thread_wire(char *out, unsigned osz, const char *parent, const char *text);

/* Like vote on the wire: "<4hex>:like" / "<4hex>:unlike" — a vote on the
 * message with that thread id. Deliberately human-readable (no special leading
 * byte) so any APRS client can like a topic by typing it. On match copies the
 * target id into [tgt], sets *unlike, and returns 1. */
int like_parse(const char *wire, char tgt[5], int *unlike);

/* Like vote naming a LONG id (8..64 hex): room bubbles are NOSTR event ids.
 * The heart arrives through the ordinary send path as text, so every send path
 * that can carry one has to recognise it before it becomes a message. */
int roomlike_parse(const char *text, char mid[70], int *unlike);

/* Either form of like vote. Send paths use this to decide "vote, not message";
 * receive paths use it so a vote never renders as a bubble. */
int anylike_parse(const char *text, char mid[70], int *unlike);

/* A vote that also names WHAT it voted on: "+like:<id> <ck>" /
 * "+unlike:<id> <ck>", where <ck> is a short key derived from the target's
 * content.
 *
 * An id alone is not enough between two devices that did not agree on one.
 * Every message sent before ids were derived (and every message from a peer
 * that numbers them differently) is unreachable by id — the vote arrives
 * naming something the other side never held, and a like on yesterday's
 * message does nothing at all. Content is the one thing both ends always have.
 *
 * The key is opaque to this layer: the host computes and matches it, the wapp
 * only carries it. It is a fixed handful of characters whatever the message,
 * which matters because these votes also ride Bluetooth.
 *
 * On match copies the id into [mid], sets *unlike, and points *ck at the key
 * (empty when the vote carries none). Returns 1. */
int votemark_parse(const char *wire, char mid[70], int *unlike,
                   const char **ck);

/* Compose that wire form. Returns [out]. */
char *votemark_wire(char *out, unsigned osz, const char *mid, int unlike,
                    const char *ck);

#endif /* CHAT_THREAD_H */
