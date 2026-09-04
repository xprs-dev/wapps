/*
 * The chat wapp, end to end on a real sqlite: what enters through the one
 * door, what the host is told, and what survives an engine restart.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include "room.h"
#include "db.h"

/* hal_mock.c */
void cap_clear(void); int cap_count(const char*); int cap_contains(const char*);
const char* cap_find(const char*); const char* cap_nth(int); int cap_n(void);
void inbox_set(const char*); void event_push(const char*, const char*);
int log_count(const char*); void log_clear(void);
void mock_set_time(uint64_t); void mock_kv_set(const char*, const char*); int mock_kv_exists(const char*);
const char* mock_last_wire(void); void mock_set_history(const char*); void mock_query_cap(uint32_t);
/* main.c */
void module_init(void); void module_handle_event(void); void module_destroy(void);

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } else g_pass++; } while (0)
#define TEST(name) static void name(void); static void run_##name(void) { printf("%s\n", #name); cap_clear(); log_clear(); name(); } static void name(void)

static void fresh(void) {
  module_destroy();
  system("rm -rf /tmp/chat_native_test");
  cap_clear(); log_clear();
  module_init();
  cap_clear(); log_clear();
}

/* A Local-room packet as WappDelivery.deliverPacket shapes it. */
static void local_packet(const char *from, const char *id, const char *text, const char *scope) {
  char row[1200];
  snprintf(row, sizeof(row),
    "{\"id\":\"%s\",\"type\":\"message\",\"from\":\"%s\",\"to\":\"\",\"fields\":[[\"t\",\"message\"],[\"f\",\"%s\"],[\"ts\",\"2026-09-04_10:00:00\"]%s,[\"m\",\"%s\"]],\"forUs\":false,\"sealed\":false,\"scope\":\"%s\",\"bearer\":\"ble\",\"sig\":\"verified\"}",
    id, from, from, scope[0] ? "" : "", text, scope[0] ? scope : "global");
  event_push("xprs.message", row);
  module_handle_event();
}

TEST(only_local_exists_by_default) {
  fresh();
  room_hydrate();
  const char *rail = cap_find("ui.rooms.set");
  CHECK(rail && strstr(rail, "\"id\":\"#LOCAL\""));
  CHECK(rail && !strstr(rail, "X5"));
  CHECK(cap_count("\"id\":\"") >= 1);
  CHECK(room_known("#LOCAL"));
  CHECK(!room_known("#NEWS"));
}

TEST(a_local_message_is_stored_and_shown_once) {
  fresh();
  local_packet("X1PEER", "aaa111", "hello room", "local");
  CHECK(cap_count("ui.convo.msg") == 1);
  CHECK(cap_contains("\"id\":\"#LOCAL\""));
  CHECK(cap_contains("\"text\":\"hello room\""));
  CHECK(cap_count("\"type\":\"notify\"") == 1);
  CHECK(cap_contains("\"count\":1"));
  cap_clear();
  /* The same packet off another bearer: one row, nothing said. */
  local_packet("X1PEER", "aaa111", "hello room", "local");
  CHECK(cap_count("ui.convo.msg") == 0);
  CHECK(cap_count("\"type\":\"notify\"") == 0);
}

TEST(unscoped_and_addressed_traffic_stays_out_of_local) {
  fresh();
  local_packet("X1PEER", "bbb222", "global words", "global");
  CHECK(cap_count("ui.convo.msg") == 0);
  CHECK(log_count("local dropped: not scope:local") == 1);
  /* Addressed to a station: correspondence, not the room. */
  event_push("xprs.message",
    "{\"id\":\"ccc333\",\"type\":\"message\",\"from\":\"X1PEER\",\"to\":\"X1TEST\",\"fields\":[[\"t\",\"message\"],[\"f\",\"X1PEER\"],[\"d\",\"X1TEST\"],[\"m\",\"private words\"]],\"scope\":\"local\",\"sealed\":false}");
  module_handle_event();
  CHECK(cap_count("ui.convo.msg") == 0);
}

TEST(own_broadcast_and_its_echo_are_one_bubble) {
  fresh();
  inbox_set("{\"command\":\"rooms_send\",\"rooms_convo\":\"#LOCAL\",\"rooms_input\":\"from me\"}");
  module_handle_event();
  CHECK(cap_count("ui.convo.msg") == 1);
  CHECK(cap_contains("\"dir\":\"out\""));
  CHECK(cap_count("\"type\":\"notify\"") == 0);
  const char *m = cap_find("ui.convo.msg");
  char mid[16] = ""; const char *p = strstr(m, "\"mid\":\""); if (p) sscanf(p + 7, "%15[^\"]", mid);
  cap_clear();
  /* The core hands our own packet back off the air under our callsign. */
  local_packet("X1TEST", mid, "from me", "local");
  CHECK(cap_count("ui.convo.msg") == 0);
}

TEST(open_repaints_from_the_database_newest_fifty) {
  fresh();
  for (int i = 0; i < 60; i++) {
    char id[16], t[32]; snprintf(id, sizeof(id), "id%04d", i); snprintf(t, sizeof(t), "msg %d", i);
    local_packet("X1PEER", id, t, "local");
  }
  CHECK(cap_contains("\"count\":60"));
  cap_clear();
  inbox_set("{\"command\":\"rooms_open\",\"rooms_convo\":\"#LOCAL\"}");
  module_handle_event();
  CHECK(cap_count("ui.convo.clear") == 1);
  CHECK(cap_count("ui.convo.msg") == 50);
  /* Ascending: the first bubble is msg 10, the last is msg 59. */
  int first = -1, last = -1;
  for (int i = 0; i < cap_n(); i++) {
    const char *c = cap_nth(i);
    if (!strstr(c, "ui.convo.msg")) continue;
    const char *p = strstr(c, "\"text\":\"msg ");
    int v = p ? atoi(p + 12) : -1;
    if (first < 0) first = v;
    last = v;
  }
  CHECK(first == 10 && last == 59);
  /* The clear comes before the upsert, the upsert before the first bubble. */
  int i_clear = -1, i_up = -1, i_msg = -1;
  for (int i = 0; i < cap_n(); i++) {
    const char *c = cap_nth(i);
    if (i_clear < 0 && strstr(c, "ui.convo.clear")) i_clear = i;
    if (i_up < 0 && strstr(c, "ui.convo.upsert")) i_up = i;
    if (i_msg < 0 && strstr(c, "ui.convo.msg")) i_msg = i;
  }
  CHECK(i_clear < i_up && i_up < i_msg);
  CHECK(cap_contains("\"count\":0"));
}

TEST(a_message_for_the_open_room_does_not_count) {
  fresh();
  inbox_set("{\"command\":\"rooms_open\",\"rooms_convo\":\"#LOCAL\"}");
  module_handle_event();
  cap_clear();
  local_packet("X1PEER", "ddd444", "while open", "local");
  CHECK(cap_contains("\"unread\":0"));
  CHECK(cap_contains("\"count\":0"));
  /* Leave; the next one counts. */
  inbox_set("{\"command\":\"nav_back\"}");
  module_handle_event();
  cap_clear();
  local_packet("X1PEER", "eee555", "after leaving", "local");
  CHECK(cap_contains("\"unread\":1"));
}

TEST(replay_from_the_archive_is_silent) {
  fresh();
  mock_set_history(
    "[{\"ts\":1700000100,\"bearer\":\"ble\",\"from\":\"X1PEER\",\"to\":\"\",\"type\":\"message\",\"id\":\"h00002\",\"own\":false,\"sig\":\"verified\",\"wire\":\"t:message f:X1PEER ts:2026-09-04_10:01:00 scope:local m:second\"},"
    "{\"ts\":1700000000,\"bearer\":\"ble\",\"from\":\"X1PEER\",\"to\":\"\",\"type\":\"message\",\"id\":\"h00001\",\"own\":false,\"sig\":\"verified\",\"wire\":\"t:message f:X1PEER ts:2026-09-04_10:00:00 scope:local m:first\"},"
    "{\"ts\":1699999900,\"bearer\":\"ble\",\"from\":\"X1PEER\",\"to\":\"\",\"type\":\"message\",\"id\":\"h00000\",\"own\":false,\"sig\":\"verified\",\"wire\":\"t:message f:X1PEER ts:2026-09-04_09:59:00 m:unscoped\"}]");
  /* A restart reads it in. */
  module_destroy(); cap_clear(); log_clear();
  module_init();
  CHECK(cap_count("ui.convo.msg") == 2);
  CHECK(cap_count("\"type\":\"notify\"") == 0);
  CHECK(!cap_contains("\"count\":1"));
  CHECK(log_count("local backfill: read=3 kept=2") == 1);
  /* Oldest first on screen. */
  { int ia = -1, ib = -1;
    for (int i = 0; i < cap_n(); i++) {
      if (ia < 0 && strstr(cap_nth(i), "\"text\":\"first\"")) ia = i;
      if (ib < 0 && strstr(cap_nth(i), "\"text\":\"second\"")) ib = i;
    }
    CHECK(ia >= 0 && ib >= 0 && ia < ib); }
  /* A second read finds nothing new. */
  cap_clear(); log_clear();
  mock_set_time(1700000200);
  inbox_set("{\"command\":\"rooms_open\",\"rooms_convo\":\"#LOCAL\"}");
  module_handle_event();
  CHECK(log_count("kept=0") == 1);
  CHECK(cap_count("ui.convo.msg") == 2);
}

TEST(a_blocked_sender_never_enters) {
  fresh();
  inbox_set("{\"command\":\"rooms_block\",\"rooms_blockcall\":\"x1spam\"}");
  module_handle_event();
  CHECK(cap_contains("\"ui.convo.blocked\",\"from\":[\"X1SPAM\"]"));
  cap_clear();
  local_packet("X1SPAM", "fff666", "buy now", "local");
  CHECK(cap_count("ui.convo.msg") == 0);
  CHECK(cap_count("\"type\":\"notify\"") == 0);
  CHECK(is_blocked("X1SPAM"));
}

TEST(hide_forgets_a_message_for_good) {
  fresh();
  local_packet("X1PEER", "ggg777", "ugly", "local");
  cap_clear();
  inbox_set("{\"command\":\"rooms_hide\",\"rooms_convo\":\"#LOCAL\",\"rooms_hidekey\":\"ggg777\"}");
  module_handle_event();
  CHECK(cap_contains("ui.convo.remove"));
  cap_clear();
  local_packet("X1PEER", "ggg777", "ugly", "local");
  CHECK(cap_count("ui.convo.msg") == 0);
}

TEST(a_one_to_one_arrives_by_callsign_and_creates_its_room) {
  fresh();
  event_push("xprs.message",
    "{\"id\":\"hhh888\",\"type\":\"message\",\"from\":\"9fe08ecd\",\"call\":\"X1PEER\",\"sig\":\"verified\",\"to\":\"\",\"content\":\"hi there\",\"title\":\"X1PEER\",\"ts\":1700000000,\"forUs\":true,\"bearer\":\"rns\",\"sealed\":true}");
  module_handle_event();
  CHECK(room_known("X1PEER"));
  CHECK(cap_contains("\"id\":\"X1PEER\""));
  CHECK(cap_contains("\"enc\":true"));
  CHECK(cap_count("ui.rooms.set") >= 1);
  const char *rail = cap_find("ui.rooms.set");
  CHECK(rail && strstr(rail, "X1PEER"));
  /* No callsign: not an XPRS station, nothing shown. */
  cap_clear();
  event_push("xprs.message",
    "{\"id\":\"\",\"type\":\"message\",\"from\":\"deadbeef\",\"call\":\"\",\"to\":\"\",\"content\":\"nomad words\",\"title\":\"\",\"bearer\":\"rns\"}");
  module_handle_event();
  CHECK(cap_count("ui.convo.msg") == 0);
}

TEST(a_sent_one_to_one_carries_its_tick_and_the_tick_finds_it_after_restart) {
  fresh();
  inbox_set("{\"command\":\"rooms_send\",\"rooms_convo\":\"X1PEER\",\"rooms_input\":\"yo\"}");
  module_handle_event();
  const char *m = cap_find("ui.convo.msg");
  char rid[16] = ""; { const char *p = m ? strstr(m, "\"rid\":\"") : 0; if (p) sscanf(p + 7, "%15[^\"]", rid); }
  CHECK(m && strstr(m, "\"status\":\"sent\"") && rid[0] == 'm');
  module_destroy(); module_init(); cap_clear();
  { char ev[128]; snprintf(ev, sizeof(ev), "{\"id\":\"%s\",\"state\":\"delivered\"}", rid);
    event_push("xprs.status.tx", ev); }
  module_handle_event();
  { char want[96]; snprintf(want, sizeof(want), "\"ui.convo.status\",\"rid\":\"%s\",\"status\":\"delivered\"", rid);
    CHECK(cap_contains(want)); }
  cap_clear();
  inbox_set("{\"command\":\"rooms_open\",\"rooms_convo\":\"X1PEER\"}");
  module_handle_event();
  CHECK(cap_contains("\"status\":\"delivered\""));
}

TEST(rooms_survive_a_restart_and_kv_is_cleaned_once) {
  fresh();
  mock_kv_set("groups", "#NEWS;"); mock_kv_set("gseen", "x;"); mock_kv_set("chan", "1");
  local_packet("X1PEER", "iii999", "kept", "local");
  module_destroy(); cap_clear();
  module_init();
  CHECK(mock_kv_exists("chan"));
  /* fresh() already ran the clean-up on this profile: the keys set after it
   * are untouched, which is the point of doing it once. */
  CHECK(mock_kv_exists("groups"));
  CHECK(room_known("#LOCAL"));
  inbox_set("{\"command\":\"rooms_open\",\"rooms_convo\":\"#LOCAL\"}");
  module_handle_event();
  CHECK(cap_contains("\"text\":\"kept\""));
  /* And on a brand-new profile the legacy keys go. */
  module_destroy(); system("rm -rf /tmp/chat_native_test");
  mock_kv_set("recent", "a=1;");
  module_init();
  CHECK(!mock_kv_exists("recent"));
  CHECK(mock_kv_exists("chan"));
}

TEST(actions_reach_their_handlers) {
  fresh();
  inbox_set("{\"type\":\"action\",\"action\":\"rooms_search\"}");
  module_handle_event();
  CHECK(cap_contains("\"ui.screen.open\",\"name\":\"Search\""));
  cap_clear();
  inbox_set("{\"command\":\"searchall_search\",\"searchall_query\":\"#news\"}");
  module_handle_event();
  CHECK(cap_contains("\"id\":\"go:#NEWS\""));
  /* The picker asks the core who it has heard on the air, and nothing
   * else: in earshot first, heard this hour after, our own callsign never,
   * and nothing from the mesh table or the Reticulum directory. */
  cap_clear();
  inbox_set("{\"type\":\"action\",\"action\":\"rooms_newchat\"}");
  module_handle_event();
  { const char *m = cap_find("ui.people.set");
    const char *reach = m ? strstr(m, "Within reach") : 0;
    const char *hour = m ? strstr(m, "Heard this hour") : 0;
    const char *near = m ? strstr(m, "go:X1NEAR") : 0;
    const char *earlier = m ? strstr(m, "go:X1EARLIER") : 0;
    CHECK(reach && near && reach < near);
    CHECK(hour && earlier && hour < earlier && near < hour);
    CHECK(m && strstr(m, "seen 20s ago") && strstr(m, "seen 40m ago"));
    CHECK(m && !strstr(m, "go:X1TEST"));
    CHECK(m && !strstr(m, "X1PEER") && !strstr(m, "X1FAR")); }
  /* A search narrows both sections by callsign. */
  cap_clear();
  inbox_set("{\"command\":\"finduser_search\",\"finduser_query\":\"earl\"}");
  module_handle_event();
  { const char *m = cap_find("ui.people.set");
    CHECK(m && strstr(m, "go:X1EARLIER") && !strstr(m, "go:X1NEAR")); }
  cap_clear();
  inbox_set("{\"command\":\"searchall_tap\",\"searchall_id\":\"go:#NEWS\"}");
  module_handle_event();
  CHECK(room_known("#NEWS"));
  CHECK(cap_contains("\"select\":true"));
  /* Now a bulletin for it lands; one for a group we are not in does not. */
  cap_clear();
  event_push("xprs.message",
    "{\"id\":\"jjj000\",\"type\":\"message\",\"from\":\"X1PEER\",\"to\":\"NEWS\",\"fields\":[[\"t\",\"message\"],[\"f\",\"X1PEER\"],[\"d\",\"NEWS\"],[\"m\",\"+9eb5 news words\"]],\"scope\":\"global\",\"bearer\":\"lan\"}");
  module_handle_event();
  CHECK(cap_contains("\"id\":\"#NEWS\"") && cap_contains("\"text\":\"news words\"") && cap_contains("\"parent\":\"9eb5\""));
  cap_clear();
  event_push("xprs.message",
    "{\"id\":\"kkk111\",\"type\":\"message\",\"from\":\"X1PEER\",\"to\":\"OTHER\",\"fields\":[[\"t\",\"message\"],[\"f\",\"X1PEER\"],[\"d\",\"OTHER\"],[\"m\",\"nope\"]],\"scope\":\"global\"}");
  module_handle_event();
  CHECK(cap_count("ui.convo.msg") == 0);
  CHECK(!room_known("#OTHER"));
}

TEST(a_reply_buffer_too_small_halves_the_tail) {
  fresh();
  for (int i = 0; i < 40; i++) {
    char id[16], t[900]; snprintf(id, sizeof(id), "big%04d", i);
    memset(t, 'x', 800); t[800] = 0;
    local_packet("X1PEER", id, t, "local");
  }
  cap_clear();
  mock_query_cap(30000);
  inbox_set("{\"command\":\"rooms_open\",\"rooms_convo\":\"#LOCAL\"}");
  module_handle_event();
  mock_query_cap(0);
  CHECK(cap_count("ui.convo.msg") == 25);
}

TEST(a_room_file_name_is_safe_and_stable) {
  fresh();
  CHECK(room_ensure("#LOCAL", 0) == 0);
  CHECK(room_ensure("X1ABCD", "X1ABCD") == 1);
  CHECK(room_ensure("#X5ABCD", "grp") == 1);
  CHECK(room_ensure("bad/id", "x") == -1);
  CHECK(room_ensure("#a b", "x") == -1);
  CHECK(!room_renderable("nomad"));
  CHECK(access("/tmp/chat_native_test/rooms/_hLOCAL.sqlite3", 0) == 0);
}

int main(void) {
  run_only_local_exists_by_default();
  run_a_local_message_is_stored_and_shown_once();
  run_unscoped_and_addressed_traffic_stays_out_of_local();
  run_own_broadcast_and_its_echo_are_one_bubble();
  run_open_repaints_from_the_database_newest_fifty();
  run_a_message_for_the_open_room_does_not_count();
  run_replay_from_the_archive_is_silent();
  run_a_blocked_sender_never_enters();
  run_hide_forgets_a_message_for_good();
  run_a_one_to_one_arrives_by_callsign_and_creates_its_room();
  run_a_sent_one_to_one_carries_its_tick_and_the_tick_finds_it_after_restart();
  run_rooms_survive_a_restart_and_kv_is_cleaned_once();
  run_actions_reach_their_handlers();
  run_a_reply_buffer_too_small_halves_the_tail();
  run_a_room_file_name_is_safe_and_stable();
  printf("%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
