/*
 * circles — private, encrypted group chat over Reticulum.
 *
 * This file is just the lifecycle + UI glue: it drains the per-wapp RNS datagram
 * channel, routes the conversations-widget commands, and shows prompts. All the
 * Circle logic (keys, epochs, encryption, sync) lives in circle.c.
 */
#include <stdint.h>
#include "xprs_wasm_hal.h"
#include "circle.h"
#include "util.h"

static char g_pending[72];   /* circle awaiting an "add member" npub */
static char g_panel[72];     /* circle whose Edit/Share/People panel is open */
static char g_member[80];    /* member awaiting a role/remove choice */
static char g_role[48];      /* role being edited ("" = creating a new one) */
static int  g_ticks = 0;

static void notify(const char *level, const char *body) {
  char m[300] = "{\"type\":\"notify\",\"level\":\"";
  s_cat(m, level, sizeof(m));
  s_cat(m, "\",\"title\":\"Circles\",\"body\":\"", sizeof(m));
  jesc(m, sizeof(m), body);
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
static char upc(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

/* Resolve a typed identifier (npub / callsign / nickname) to an npub using the
 * reusable contacts HAL. Returns 1 and writes the npub on success. A direct
 * npub passes through; a callsign/nickname is looked up among known contacts
 * (a callsign is only known once its npub is — exactly the picker's promise). */
static int resolve_identifier(const char *inp, char *out, unsigned cap) {
  if (inp[0] == 'n' && inp[1] == 'p' && inp[2] == 'u' && inp[3] == 'b' && inp[4] == '1') {
    s_cpy(out, inp, cap); return 1;
  }
  char up[24]; unsigned i = 0;
  for (; inp[i] && i < sizeof(up) - 1; i++) up[i] = upc(inp[i]);
  up[i] = 0;
  char cj[16384];
  int n = hal_contacts_query(inp, s_len(inp), cj, sizeof(cj));
  if (n <= 0) return 0;
  const char *cur = cj; char obj[512]; char firstnp[100] = "";
  while (next_object(&cur, obj, sizeof(obj))) {
    char np[100], cs[24];
    if (!jstr(obj, "npub", np, sizeof(np))) continue;
    if (!firstnp[0]) s_cpy(firstnp, np, sizeof(firstnp));
    jstr(obj, "callsign", cs, sizeof(cs));
    if (cs[0] && s_eq(cs, up)) { s_cpy(out, np, cap); return 1; }  /* exact callsign */
  }
  if (firstnp[0]) { s_cpy(out, firstnp, cap); return 1; }
  return 0;
}

/* ── prompts ──────────────────────────────────────────────────────────── */
static void prompt_new_circle(void) {
  const char *m = "{\"type\":\"ui.prompt\",\"id\":\"newcircle\",\"title\":\"New circle\","
    "\"body\":\"Name your circle. You can add people to it afterwards.\","
    "\"input\":{\"hint\":\"Circle name\",\"max\":40},\"confirm\":\"Create\"}";
  hal_msg_send(m, s_len(m));
}
static void prompt_join(void) {
  const char *m = "{\"type\":\"ui.prompt\",\"id\":\"joincode\",\"title\":\"Find a circle\","
    "\"body\":\"Enter the circle's short id (e.g. ab1-9xz). We'll find it across the "
    "network and ask the owner to let you in. A full circle key or scanned QR works too.\","
    "\"input\":{\"hint\":\"ab1-9xz\",\"max\":120},\"confirm\":\"Find\"}";
  hal_msg_send(m, s_len(m));
}
static void prompt_add_member(const char *circleId) {
  s_cpy(g_pending, circleId, sizeof(g_pending));
  char cj[16384];
  int n = hal_contacts_query("", 0, cj, sizeof(cj));
  char m[20000] = "{\"type\":\"ui.prompt\",\"id\":\"addmember\",\"title\":\"Add member\",";
  s_cat(m, "\"body\":\"Pick someone you know, or type a callsign, nickname or npub.\",", sizeof(m));
  if (n > 0 && cj[0] == '[' && cj[1] != ']') {
    s_cat(m, "\"chips\":[", sizeof(m));
    const char *cur = cj; char obj[512]; int first = 1, cnt = 0;
    while (next_object(&cur, obj, sizeof(obj)) && cnt < 30) {
      char np[100], cs[24], nick[48], label[80];
      if (!jstr(obj, "npub", np, sizeof(np))) continue;
      jstr(obj, "callsign", cs, sizeof(cs));
      jstr(obj, "nick", nick, sizeof(nick));
      label[0] = 0;
      if (cs[0]) s_cat(label, cs, sizeof(label));
      if (nick[0]) { if (label[0]) s_cat(label, " - ", sizeof(label)); s_cat(label, nick, sizeof(label)); }
      if (!label[0]) s_cpy(label, np, sizeof(label));
      if (!first) s_cat(m, ",", sizeof(m));
      first = 0; cnt++;
      s_cat(m, "{\"label\":\"", sizeof(m)); jesc(m, sizeof(m), label);
      s_cat(m, "\",\"value\":\"", sizeof(m)); jesc(m, sizeof(m), np);
      s_cat(m, "\"}", sizeof(m));
    }
    s_cat(m, "],\"chipMode\":\"select\",", sizeof(m));
  }
  s_cat(m, "\"input\":{\"hint\":\"Callsign, nickname or npub1…\",\"max\":80},"
          "\"confirm\":\"Add\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* Role / remove picker for a member (chips: each role, plus Remove). */
static void prompt_member_role(const char *circleId, const char *memberPub) {
  s_cpy(g_panel, circleId, sizeof(g_panel));
  s_cpy(g_member, memberPub, sizeof(g_member));
  char roles[512]; circle_roles_csv(circleId, roles, sizeof(roles));
  char m[2048] = "{\"type\":\"ui.prompt\",\"id\":\"memberrole\",\"title\":\"Member\",";
  s_cat(m, "\"body\":\"Choose a role, or remove this person.\",\"chips\":[", sizeof(m));
  const char *p = roles; int first = 1;
  while (*p) {
    char nm[48]; unsigned ni = 0;
    while (*p && *p != ',' && ni < sizeof(nm) - 1) nm[ni++] = *p++;
    nm[ni] = 0; if (*p == ',') p++;
    if (!nm[0]) continue;
    if (!first) s_cat(m, ",", sizeof(m));
    first = 0;
    s_cat(m, "{\"label\":\"", sizeof(m)); jesc(m, sizeof(m), nm);
    s_cat(m, "\",\"value\":\"role:", sizeof(m)); jesc(m, sizeof(m), nm);
    s_cat(m, "\"}", sizeof(m));
  }
  if (!first) s_cat(m, ",", sizeof(m));
  s_cat(m, "{\"label\":\"Remove from circle\",\"value\":\"remove\"}],", sizeof(m));
  s_cat(m, "\"chipMode\":\"select\",\"confirm\":\"Apply\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

static void on_prompt(const char *buf) {
  char pid[24] = "", val[120] = "", inp[120] = "";
  jstr(buf, "prompt_id", pid, sizeof(pid));
  jstr(buf, "prompt_value", val, sizeof(val));
  jstr(buf, "prompt_input", inp, sizeof(inp));
  if (s_eq(pid, "newcircle")) {
    if (inp[0]) circle_create(inp);
  } else if (s_eq(pid, "joincode")) {
    if (inp[0]) circle_apply_join(inp);
  } else if (s_eq(pid, "addmember")) {
    char npub[120] = "";
    if (val[0]) s_cpy(npub, val, sizeof(npub));           /* picked a contact */
    else if (inp[0] && !resolve_identifier(inp, npub, sizeof(npub))) {
      notify("error", "No contact matches — paste their npub instead");
    }
    if (npub[0] && g_pending[0]) circle_add_member(g_pending, npub);
    g_pending[0] = 0;
  } else if (s_eq(pid, "memberrole")) {
    if (g_panel[0] && g_member[0]) {
      if (s_eq(val, "remove")) {
        circle_member_remove(g_panel, g_member);
      } else if (val[0] == 'r' && val[1] == 'o' && val[2] == 'l' &&
                 val[3] == 'e' && val[4] == ':') {
        circle_member_set_role(g_panel, g_member, val + 5);
      }
    }
    g_member[0] = 0;
  }
}

/* ── inbound RNS datagrams ────────────────────────────────────────────── */
static void drain_rns(void) {
  /* Datagrams (esp. keysets carrying folder structure) can be large; keep the
   * scratch off the small wasm stack. */
  static char env[40000];
  static char payb64[32000];
  static unsigned char dg[24000];
  for (int guard = 0; guard < 32; guard++) {
    if (hal_rns_available() == 0) return;
    uint32_t n = hal_rns_recv(env, sizeof(env) - 1);
    if (n == 0) return;
    env[n] = 0;
    char from[80] = "";
    jstr(env, "from", from, sizeof(from));
    if (!jstr(env, "payload", payb64, sizeof(payb64))) continue;
    int dn = b64url_decode(payb64, dg, sizeof(dg) - 1);
    if (dn <= 0) continue;
    dg[dn] = 0;
    circle_on_datagram(from, (const char *)dg);
  }
}

/* ── lifecycle ────────────────────────────────────────────────────────── */
__attribute__((export_name("module_init")))
int module_init(void) {
  hal_log(1, "circles loaded", 14);
  circle_init();
  return 0;
}

__attribute__((export_name("module_tick")))
void module_tick(void) {
  drain_rns();
  if (++g_ticks >= 15) { g_ticks = 0; circle_tick(); }
}

__attribute__((export_name("module_handle_event")))
void module_handle_event(void) {
  char buf[4096];
  if (hal_msg_available() == 0) return;
  uint32_t n = hal_msg_recv(buf, sizeof(buf) - 1);
  if (n == 0) return;
  buf[n] = 0;

  char cmd[40];
  if (!jstr(buf, "command", cmd, sizeof(cmd))) return;

  if (s_eq(cmd, "new_circle")) {
    prompt_new_circle();
  } else if (s_eq(cmd, "join_circle")) {
    prompt_join();
  } else if (s_eq(cmd, "apply_url")) {
    /* Opened from a https://xprs.dev/circle/… deep link. */
    char code[160] = ""; jstr(buf, "code", code, sizeof(code));
    if (code[0]) circle_apply_join(code);
  } else if (s_eq(cmd, "req_approve")) {
    char mid[80] = ""; jstr(buf, "members_id", mid, sizeof(mid));
    if (g_panel[0] && mid[0]) circle_approve_request(g_panel, mid);
  } else if (s_eq(cmd, "req_reject")) {
    char mid[80] = ""; jstr(buf, "members_id", mid, sizeof(mid));
    if (g_panel[0] && mid[0]) circle_reject_request(g_panel, mid);
  } else if (s_eq(cmd, "edit_circle")) {
    char id[72] = ""; jstr(buf, "conversations_convo", id, sizeof(id));
    if (id[0]) { s_cpy(g_panel, id, sizeof(g_panel)); circle_open_edit(id); }
  } else if (s_eq(cmd, "share_circle")) {
    char id[72] = ""; jstr(buf, "conversations_convo", id, sizeof(id));
    if (id[0]) { s_cpy(g_panel, id, sizeof(g_panel)); circle_open_share(id); }
  } else if (s_eq(cmd, "manage_people")) {
    char id[72] = ""; jstr(buf, "conversations_convo", id, sizeof(id));
    if (id[0]) { s_cpy(g_panel, id, sizeof(g_panel)); circle_open_people(id); }
  } else if (s_eq(cmd, "delete_circle")) {
    char id[72] = ""; jstr(buf, "conversations_convo", id, sizeof(id));
    if (id[0]) circle_delete(id);
  } else if (s_eq(cmd, "edit_save")) {
    char name[64] = "", desc[300] = "";
    jstr(buf, "edit_name", name, sizeof(name));
    jstr(buf, "edit_desc", desc, sizeof(desc));
    if (g_panel[0]) circle_save_edit(g_panel, name, desc);
  } else if (s_eq(cmd, "edit_pic__pickimage")) {
    char tok[128] = ""; jstr(buf, "edit_pic", tok, sizeof(tok));
    if (g_panel[0] && tok[0]) circle_set_picture(g_panel, tok);
  } else if (s_eq(cmd, "manage_roles")) {
    if (g_panel[0]) circle_open_roles(g_panel);
  } else if (s_eq(cmd, "role_add")) {
    g_role[0] = 0;
    if (g_panel[0]) circle_role_open_edit(g_panel, "");
  } else if (s_eq(cmd, "role_edit")) {
    char rn[48] = ""; jstr(buf, "roles_id", rn, sizeof(rn));
    if (g_panel[0] && rn[0] && !s_eq(rn, "__add")) {
      s_cpy(g_role, rn, sizeof(g_role)); circle_role_open_edit(g_panel, rn);
    }
  } else if (s_eq(cmd, "role_del")) {
    char rn[48] = ""; jstr(buf, "roles_id", rn, sizeof(rn));
    if (g_panel[0] && rn[0] && !s_eq(rn, "__add")) circle_role_remove(g_panel, rn);
  } else if (s_eq(cmd, "role_save")) {
    char nm[48] = "", de[220] = ""; int def = jbool(buf, "role_default");
    jstr(buf, "role_name", nm, sizeof(nm));
    jstr(buf, "role_desc", de, sizeof(de));
    if (g_panel[0]) circle_role_save(g_panel, g_role, nm, de, def);
    g_role[0] = 0;
  } else if (s_eq(cmd, "role_delete")) {
    if (g_panel[0] && g_role[0]) circle_role_remove(g_panel, g_role);
    g_role[0] = 0;
  } else if (s_eq(cmd, "add_member")) {
    /* From the People panel's Add row, or a circle room action. */
    char id[72] = ""; jstr(buf, "conversations_convo", id, sizeof(id));
    const char *c = id[0] ? id : g_panel;
    if (c[0]) prompt_add_member(c);
  } else if (s_eq(cmd, "mrole")) {
    char mid[80] = ""; jstr(buf, "members_id", mid, sizeof(mid));
    if (g_panel[0] && mid[0]) prompt_member_role(g_panel, mid);
  } else if (s_eq(cmd, "mremove")) {
    char mid[80] = ""; jstr(buf, "members_id", mid, sizeof(mid));
    if (g_panel[0] && mid[0]) circle_member_remove(g_panel, mid);
  } else if (cmd[0] == 'm' && cmd[1] == 's' && cmd[2] == 't' && cmd[3] == 'a' &&
             cmd[4] == 't' && cmd[5] == ':') {
    /* mstat:<active|inactive|suspended|banned> from the member menu */
    char mid[80] = ""; jstr(buf, "members_id", mid, sizeof(mid));
    if (g_panel[0] && mid[0]) circle_member_set_status(g_panel, mid, cmd + 6);
  } else if (s_eq(cmd, "folderrail_tap")) {
    char id[40] = ""; jstr(buf, "folderrail_id", id, sizeof(id));
    if (g_panel[0] && id[0]) circle_folder_view(g_panel, id); /* into a sub-folder */
  } else if (s_eq(cmd, "folder_new")) {
    /* gear menu: create a folder under the current location. From the circle's
     * gear (carries conversations_convo) the parent is the circle root; from the
     * folder-view gear it's the open folder. */
    char id[72] = ""; jstr(buf, "conversations_convo", id, sizeof(id));
    if (id[0]) { s_cpy(g_panel, id, sizeof(g_panel)); circle_open_room_rail(g_panel); }
    if (g_panel[0]) circle_folder_open_edit(g_panel, "");
  } else if (s_eq(cmd, "folder_settings")) {
    if (g_panel[0]) circle_folder_edit_current(g_panel);
  } else if (s_eq(cmd, "folder_access_cur")) {
    if (g_panel[0]) circle_folder_access_current(g_panel);
  } else if (s_eq(cmd, "folder_save")) {
    char nm[64] = "", de[220] = "", ic[260] = "", ty[16] = "";
    jstr(buf, "folder_name", nm, sizeof(nm));
    jstr(buf, "folder_desc", de, sizeof(de));
    jstr(buf, "folder_icon", ic, sizeof(ic));
    jstr(buf, "folder_type", ty, sizeof(ty));
    if (g_panel[0]) circle_folder_save(g_panel, nm, de, ic, ty);
  } else if (s_eq(cmd, "folder_delete")) {
    if (g_panel[0]) circle_folder_delete_current(g_panel);
  } else if (s_eq(cmd, "frole")) {
    char rid[48] = ""; jstr(buf, "faccess_id", rid, sizeof(rid));
    if (g_panel[0] && rid[0]) circle_folder_role_toggle(g_panel, rid);
  } else if (s_eq(cmd, "fmem")) {
    char mid[80] = ""; jstr(buf, "faccess_id", mid, sizeof(mid));
    if (g_panel[0] && mid[0]) circle_folder_member_cycle(g_panel, mid);
  } else if (s_eq(cmd, "folderchat_send")) {
    char text[300] = ""; jstr(buf, "folderchat_input", text, sizeof(text));
    if (g_panel[0] && text[0]) circle_folder_send(g_panel, text);
  } else if (s_eq(cmd, "conversations_open")) {
    /* A circle was opened — render its stored (decrypted) message history and
     * show its sub-folder rail beside the chat. Without the render the chat pane
     * shows "No messages yet" until a new message arrives live, even though the
     * history is already in the local db. */
    char id[72] = ""; jstr(buf, "conversations_convo", id, sizeof(id));
    if (id[0]) { s_cpy(g_panel, id, sizeof(g_panel)); circle_render(id); circle_open_room_rail(g_panel); }
  } else if (s_eq(cmd, "conv_rail_tap")) {
    char id[40] = ""; jstr(buf, "conv_rail_id", id, sizeof(id));
    if (g_panel[0] && id[0]) circle_folder_view(g_panel, id);
  } else if (s_eq(cmd, "conversations_send")) {
    char id[72] = "", text[300] = "";
    jstr(buf, "conversations_convo", id, sizeof(id));
    jstr(buf, "conversations_input", text, sizeof(text));
    if (id[0] && text[0]) circle_send(id, text);
  } else if (s_eq(cmd, "prompt")) {
    on_prompt(buf);
  }
}

__attribute__((export_name("module_destroy")))
void module_destroy(void) {
  hal_log(1, "circles unloaded", 16);
}

__attribute__((export_name("module_tick_interval_ms")))
uint32_t module_tick_interval_ms(void) {
  return 1000;
}
