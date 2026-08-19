#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* wapp entry points */
void module_init(void);
void module_handle_event(void);
int  circle_create(const char* name);
int  circle_resolve_short(const char* code, char* outId, unsigned cap);
void circle_on_datagram(const char* from, const char* json);

/* harness hooks (hal_mock.c) */
void cap_clear(void);
int  cap_contains(const char* s);
const char* cap_find(const char* s);
void inbox_set(const char* s);

static int g_fail=0;
static void ok(const char* what,int cond){ printf("%s %s\n", cond?"PASS":"FAIL", what); if(!cond)g_fail=1; }

static int xget(const char* json,const char* key,char* out,int cap){
  char pat[64]; snprintf(pat,sizeof(pat),"\"%s\":\"",key);
  const char* p=strstr(json,pat); if(!p)return 0; p+=strlen(pat); int i=0;
  while(*p&&*p!='"'&&i<cap-1) out[i++]=*p++; out[i]=0; return 1;
}
static void run(const char* cmd){ inbox_set(cmd); module_handle_event(); }

int main(void){
  system("rm -rf /tmp/ct/circles");
  module_init();

  /* create a circle, learn its id */
  cap_clear();
  circle_create("Embaixada");
  const char* up=cap_find("ui.convo.upsert");
  char cid[80]=""; if(up) xget(up,"id",cid,sizeof(cid));
  ok("circle_create emits convo upsert with id", up && cid[0]);

  /* short reference code: Share shows "circle/<first3>-<last3>", and we can
   * resolve it (and the full key) back to the circle. */
  cap_clear();
  char cs[256]; snprintf(cs,sizeof(cs),"{\"command\":\"share_circle\",\"conversations_convo\":\"%s\"}",cid);
  run(cs);
  const char* shf=cap_find("\"field\":\"share_short\"");
  char shortc[40]=""; if(shf) xget(shf,"value",shortc,sizeof(shortc));
  char want[40]; { int n=(int)strlen(cid);
    snprintf(want,sizeof(want),"circle/%c%c%c-%c%c%c",cid[0],cid[1],cid[2],cid[n-3],cid[n-2],cid[n-1]); }
  ok("Share shows the short code circle/first3-last3", shf && strcmp(shortc,want)==0);
  char r[80];
  ok("resolve short code -> the circle", circle_resolve_short(shortc,r,sizeof(r)) && strcmp(r,cid)==0);
  char full[128]; snprintf(full,sizeof(full),"circle:%s",cid);
  ok("resolve full key -> the circle", circle_resolve_short(full,r,sizeof(r)) && strcmp(r,cid)==0);
  ok("resolve a non-matching short code -> not found", circle_resolve_short("circle/000-000",r,sizeof(r))==0);
  const char* lf=cap_find("\"field\":\"share_link\"");
  ok("Share shows a xprs.dev deep link with the full id",
     lf && strstr(lf,"https://xprs.dev/circle/") && strstr(lf,cid));
  /* a deep-link URL is parsed back to the circle id (self is the owner here) */
  char url[200]; snprintf(url,sizeof(url),"https://xprs.dev/circle/%s",cid);
  cap_clear(); circle_apply_join(url);
  ok("deep-link URL parses to the circle id", cap_contains("already in this circle"));

  /* opening a circle pushes the in-circle folder rail by default */
  cap_clear();
  char c1[256]; snprintf(c1,sizeof(c1),"{\"command\":\"conversations_open\",\"conversations_convo\":\"%s\"}",cid);
  run(c1);
  ok("opening a circle pushes conv_rail by default", cap_contains("\"field\":\"conv_rail\""));

  /* gear -> New folder (from the circle: parent is the root) */
  char c2[256]; snprintf(c2,sizeof(c2),"{\"command\":\"folder_new\",\"conversations_convo\":\"%s\"}",cid);
  run(c2);
  /* save it with an emoji icon */
  cap_clear();
  run("{\"command\":\"folder_save\",\"folder_name\":\"News\",\"folder_type\":\"chat\",\"folder_icon\":\"\xF0\x9F\x93\xB0\",\"folder_desc\":\"press\"}");
  const char* cr=cap_find("\"field\":\"conv_rail\"");
  ok("after save the circle rail lists 'News'", cr && strstr(cr,"\"name\":\"News\""));
  ok("rail carries the emoji icon raw (not a Material name)", cr && strstr(cr,"\xF0\x9F\x93\xB0"));
  ok("rail is navigation-only (no Add/Settings/Access/Up controls)",
     cr && !strstr(cr,"__add") && !strstr(cr,"__edit") && !strstr(cr,"__access") && !strstr(cr,"__up"));
  ok("saving a root folder returns to the circle (screen.close)", cap_contains("ui.screen.close"));

  /* resolve the News id from the rail */
  char fid[40]="";
  if(cr){ const char* n=strstr(cr,"\"name\":\"News\""); const char* best=0; const char* p=cr;
    while((p=strstr(p,"\"id\":\""))){ if(p<n) best=p; else break; p+=6; }
    if(best){ best+=6; int i=0; while(best[i]&&best[i]!='"'&&i<39){fid[i]=best[i];i++;} fid[i]=0; } }
  ok("resolved News id", fid[0]!=0);

  /* tap it in the rail -> opens the folder view titled with the folder name */
  cap_clear();
  char c3[256]; snprintf(c3,sizeof(c3),"{\"command\":\"conv_rail_tap\",\"conv_rail_id\":\"%s\"}",fid);
  run(c3);
  const char* so=cap_find("\"type\":\"ui.screen.open\"");
  ok("folder view opens", so && strstr(so,"Folder view"));
  ok("folder view title is the folder name, not 'Folder view'", so && strstr(so,"\"title\":\"News\""));
  ok("folder chat area activated", cap_contains("\"field\":\"folderchat_active\",\"value\":true"));

  /* post into the folder */
  cap_clear();
  run("{\"command\":\"folderchat_send\",\"folderchat_input\":\"hi folder\"}");
  ok("folder message renders", cap_contains("ui.chat.append") && cap_contains("\"field\":\"folderchat\"") && cap_contains("hi folder"));

  /* 8) join flow: an applicant scans the key and applies; owner sees the request
   * and approves; the applicant becomes a member. */
  const char* applicant="BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB";
  char jr[400]; snprintf(jr,sizeof(jr),
    "{\"k\":\"jr\",\"c\":\"%.16s\",\"cid\":\"%s\",\"frm\":\"%s\",\"nm\":\"Bob\",\"s\":\"sig\"}",
    cid,cid,applicant);
  circle_on_datagram("peer", jr);
  cap_clear();
  char op[256]; snprintf(op,sizeof(op),"{\"command\":\"manage_people\",\"conversations_convo\":\"%s\"}",cid);
  run(op);
  const char* pl=cap_find("\"field\":\"members\"");
  ok("People panel shows the pending join request", pl && strstr(pl,"wants to join") && strstr(pl,"req_approve"));
  cap_clear();
  char ap[160]; snprintf(ap,sizeof(ap),"{\"command\":\"req_approve\",\"members_id\":\"%s\"}",applicant);
  run(ap);
  const char* pl2=cap_find("\"field\":\"members\"");
  ok("after approval the applicant is a member and the request is gone",
     pl2 && strstr(pl2,applicant) && !strstr(pl2,"wants to join"));
  ok("review panel has the status tabs + history",
     pl2 && strstr(pl2,"\"title\":\"Candidates\"") && strstr(pl2,"\"title\":\"Active\"") && strstr(pl2,"\"title\":\"History\""));

  /* 9) suspend the member → moves to the Suspended tab; history records it */
  cap_clear();
  char su[160]; snprintf(su,sizeof(su),"{\"command\":\"mstat:suspended\",\"members_id\":\"%s\"}",applicant);
  run(su);
  const char* pl3=cap_find("\"field\":\"members\"");
  ok("suspend creates a Suspended tab with the member", pl3 && strstr(pl3,"\"title\":\"Suspended\"") && strstr(pl3,applicant));
  ok("history logs applied/approved/suspended",
     pl3 && strstr(pl3,"\"title\":\"History\"") && strstr(pl3,"applied") && strstr(pl3,"approved") && strstr(pl3,"suspended"));

  printf(g_fail?"\n=== TESTS FAILED ===\n":"\n=== ALL TESTS PASSED ===\n");
  return g_fail;
}
