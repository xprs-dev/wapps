/*
 * room.h — every conversation this wapp holds, and the ONE door a message
 * comes in through.
 *
 * A room is a conversation: the Local room ("#LOCAL", always there), an open
 * group ("#NAME"), a closed group ("#X5ABCD", XPRS section 26) or a station
 * ("X16JK8"). Each room is its own sqlite database under rooms/, listed in
 * index.sqlite3. Nothing else remembers a message: not a ring in RAM, not a
 * key in KV, not the host. The host's conversation widget is a VIEW that this
 * unit paints from the database -- ui.convo.* out, never in.
 *
 * room_admit() is the door. Live traffic, our own sends, the archive refill
 * and system notes all go through it, so blocking, hiding and "have I seen
 * this" are decided in exactly one place. "Seen" is the message id being the
 * table's primary key.
 */
#ifndef CHAT_ROOM_H
#define CHAT_ROOM_H

typedef struct {
  const char *room;     /* conversation id */
  const char *title;    /* used only when the room does not exist yet */
  const char *mid;      /* the message's id -- REQUIRED, it is the dedup */
  const char *dir;      /* "in" | "out" */
  const char *sender;   /* callsign; "" for a system note */
  const char *body;     /* <= 900 bytes */
  const char *parent;   /* id this replies to, or "" */
  const char *via;      /* bearer as the host spells it, or "" */
  const char *auth;     /* "verified" or "" */
  const char *rid;      /* 1:1 receipt id, or "" */
  const char *status;   /* "sent" | "delivered" | "read" | "" */
  unsigned long long ts;/* sender epoch; 0 = now */
  int enc;              /* body travelled sealed */
  int sys;              /* a system note, not words */
  int replay;           /* archive refill: store only -- no unread, no notify */
} room_msg_t;

/* Open the index, create the schema, make sure "#LOCAL" exists. [self] is
 * this station's callsign (a bubble from it renders as ours). */
void room_init(const char *self);
void room_destroy(void);

/* A row for [id] in the index, created if missing. 1 created, 0 existed,
 * -1 refused (an id this wapp cannot render). No UI traffic. */
int room_ensure(const char *id, const char *title);
int room_known(const char *id);
/* An id this wapp can draw: "#..." or a station callsign. */
int room_renderable(const char *id);

/* THE DOOR. 1 stored and shown, 0 filtered or already held, -1 no database. */
int room_admit(const room_msg_t *m);

/* The user opened [id]: repaint it from the database (clear + newest tail),
 * mark it read. */
void room_open(const char *id);
/* The user left the open conversation (nav_back). */
void room_left(void);
const char *room_open_id(void);
/* User action: make [id] exist and focus it on screen. */
void room_start(const char *id, const char *title);

/* Paint the whole view: the rail, one row per room, the blocked set, the
 * unread total. Called once at start by every engine. */
void room_hydrate(void);
/* Redraw the rail alone (the set or its order changed). */
void room_rail(void);
/* The Local room's switch in Settings. Off = off the rail, not counted. */
void room_set_local_enabled(int on);

void room_react(const char *id, const char *mid, const char *who, int remove, int mine);
/* Remember which room a 1:1 we sent belongs to, so a later tick finds it. */
void room_tx_note(const char *rid, const char *room);
void room_status(const char *rid, const char *state);
void room_close(const char *id);
int  room_set_private(const char *id, int on);
int  room_is_private(const char *id);
void room_hide(const char *id, const char *mid);
void room_set_title(const char *id, const char *title);
unsigned long long room_max_ts(const char *id);

int  is_blocked(const char *call);
int  block_add(const char *call);      /* 1 blocked now, 0 no change */
int  block_remove(const char *call);
void blocked_publish(void);

/* Shared with main.c */
void fmt_time_at(char *b, unsigned long long epoch);
void log1(const char *line);

#endif /* CHAT_ROOM_H */
