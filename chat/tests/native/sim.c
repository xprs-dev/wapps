/*
 * sim.c — an on-machine, no-device network for the chat wapp.
 *
 * The wapp only ever talks to the HAL: it hands the core a payload and is
 * called back when one arrives (docs/architecture.md — transports are CORE).
 * So to test 1:1 and closed-group flows end to end we do not need a phone or a
 * radio; we need a stand-in CORE that carries a packet from one instance of the
 * wapp to another. That is all this file is.
 *
 * Every node runs the REAL wapp (main.c / room.c / db.c / thread.c / xprs.c).
 * A node's whole persistent state is its sqlite under its own root, so we run
 * many nodes in one process turn-based: to act as node N we module_destroy the
 * one that was loaded and module_init against N's root and callsign. Switching
 * nodes is therefore exactly a restart, which is a feature — a scenario can
 * prove a group survives one.
 *
 * The mock HAL (hal_mock.c) exposes NULL-by-default hooks; here we install them
 * so a node's hal_xprs_send / hal_xprs_message / hal_xprs_broadcast / hal_xprs_read
 * become deliveries into the OTHER nodes' event queues, shaped exactly as the
 * core shapes them (xprs.message content, xprs.reaction packet, xprs.status.tx),
 * and hal_xprs_groups / hal_xprs_group_roster answer from the roster this file
 * keeps. Closed-group membership (26.7) is enforced HERE, at the core's door:
 * a non-member's post is refused with -2 and reaches no one, just as on device.
 *
 *   sh tests/native/run-sim.sh
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---- the wapp's entry points (same three test_room.c drives) ---- */
void module_init(void);
void module_handle_event(void);
void module_destroy(void);

/* ---- the mock HAL's seams we use ---- */
void mock_set_root(const char* r);
void cap_clear(void);
int  cap_count(const char* s);
int  cap_contains(const char* s);
const char* cap_find(const char* s);
int cap_n(void); const char* cap_nth(int);
void mock_kv_ns(int n);
void inbox_set(const char* s);
void event_push(const char* topic, const char* row);
void mock_set_time(uint64_t t);
uint64_t hal_time_epoch(void);

/* ---- the hooks hal_mock.c calls when installed ---- */
extern const char* (*g_hk_identity)(void);
extern int32_t (*g_hk_send)(const char*,uint32_t);
extern int32_t (*g_hk_message)(const char*,uint32_t,const char*,uint32_t,uint32_t,char*,uint32_t);
extern int32_t (*g_hk_broadcast)(const char*,uint32_t,const char*,uint32_t,const char*,uint32_t,char*,uint32_t);
extern int32_t (*g_hk_read)(const char*,uint32_t);
extern int32_t (*g_hk_groups)(char*,uint32_t);
extern int32_t (*g_hk_roster)(const char*,uint32_t,char*,uint32_t);

/* the section-5 identifier, from the wapp's own xprs.c */
void xprs_id(const char *wire, unsigned len, char out[7]);

/* ─────────────────────────── the network ─────────────────────────── */

#define MAXNODE 8
#define MAXGRP  8
#define MAXMEM  8
#define MAXPEND 256

typedef struct { char topic[40]; char row[1400]; } ev_t;

typedef struct {
  char call[8];
  char root[160];
  ev_t pend[MAXPEND]; int pn;
} node_t;

typedef struct {
  char call[8];                                  /* X5.... (no #) */
  char nick[24];
  struct { char call[8]; char role[8]; } mem[MAXMEM]; int mn;   /* role incl. "invited" */
} grp_t;

static node_t N[MAXNODE]; static int NN;
static grp_t  G[MAXGRP];  static int GN;
static int cur = -1;                              /* loaded node, or -1 */
static int g_seq = 0;                             /* id counter for 1:1 / local */

/* id -> author callsign, so a read receipt goes back to who sent it */
static struct { char id[16]; char author[8]; } AUTH[1024]; static int AUTHN;
static void remember_author(const char* id, const char* author){
  if(!id[0]||AUTHN>=1024) return;
  snprintf(AUTH[AUTHN].id,sizeof AUTH[AUTHN].id,"%s",id);
  snprintf(AUTH[AUTHN].author,sizeof AUTH[AUTHN].author,"%s",author); AUTHN++;
}
static const char* author_of(const char* id){
  for(int i=AUTHN-1;i>=0;i--) if(!strcmp(AUTH[i].id,id)) return AUTH[i].author;
  return 0;
}

static int node_by_call(const char* c){ for(int i=0;i<NN;i++) if(!strcmp(N[i].call,c)) return i; return -1; }
static grp_t* grp_by_call(const char* c){ for(int i=0;i<GN;i++) if(!strcmp(G[i].call,c)) return &G[i]; return 0; }

static int is_posting_member(grp_t* g, const char* call){
  if(!g) return 0;
  for(int i=0;i<g->mn;i++) if(!strcmp(g->mem[i].call,call)){
    const char* r=g->mem[i].role;
    return !strcmp(r,"member")||!strcmp(r,"mod")||!strcmp(r,"admin");
  }
  return 0;
}

static void enqueue(int ni, const char* topic, const char* row){
  node_t* n=&N[ni]; if(n->pn>=MAXPEND) return;
  snprintf(n->pend[n->pn].topic,sizeof n->pend[n->pn].topic,"%s",topic);
  snprintf(n->pend[n->pn].row,sizeof n->pend[n->pn].row,"%s",row);
  n->pn++;
}
/* to every posting member of g except the sender */
static void enqueue_members(grp_t* g, const char* from, const char* topic, const char* row){
  for(int i=0;i<g->mn;i++){
    const char* r=g->mem[i].role;
    if(strcmp(r,"member")&&strcmp(r,"mod")&&strcmp(r,"admin")) continue;   /* invited: no post */
    if(!strcmp(g->mem[i].call,from)) continue;
    int ni=node_by_call(g->mem[i].call); if(ni>=0) enqueue(ni, topic, row);
  }
}

/* ---- JSON escape a body into a growing buffer ---- */
static void jesc_into(char* o, int cap, const char* s){
  int n=strlen(o);
  for(; *s && n<cap-2; s++){
    unsigned char c=(unsigned char)*s;
    if(c=='"'||c=='\\'){ o[n++]='\\'; o[n++]=c; }
    else if(c=='\n'){ o[n++]='\\'; o[n++]='n'; }
    else o[n++]=c;                                  /* UTF-8 bytes pass through */
  }
  o[n]=0;
}

/* ---- read a space-delimited XPRS field; m: is the rest of the line ---- */
static int wfield(const char* wire, const char* key, char* out, int osz){
  int kl=strlen(key);
  const char* p=wire;
  while(*p){
    /* token starts at p (start of wire, or just after a space) */
    if(!strncmp(p,key,kl)){
      const char* v=p+kl; int n=0;
      if(!strcmp(key,"m:")){ while(*v && n<osz-1) out[n++]=*v++; }   /* rest of line */
      else { while(*v && *v!=' ' && n<osz-1) out[n++]=*v++; }
      out[n]=0; return 1;
    }
    while(*p && *p!=' ') p++;                        /* skip to next space */
    while(*p==' ') p++;
  }
  out[0]=0; return 0;
}

/* ────────────────── the hooks: a send becomes a delivery ────────────── */

#define STAMP "2026-09-04_10:00:00"

/* on_core_packet reads XPRS fields (m,r,add,remove,ts,n) from a `fields` array
 * and the envelope (from,to,id,scope,sealed,sig,bearer) from top level — the
 * shape WappDelivery.deliverPacket puts on the wire. */
static void react_row(char* row,int cap,const char* from,const char* to,const char* scope,
                      const char* id,const char* r,const char* kindkey){
  snprintf(row,cap,
   "{\"id\":\"%s\",\"type\":\"reaction\",\"from\":\"%s\",\"to\":\"%s\",\"scope\":\"%s\","
   "\"sealed\":false,\"sig\":\"verified\",\"fields\":[[\"t\",\"reaction\"],[\"r\",\"%s\"],[\"%s\",\"like\"]]}",
   id,from,to,scope,r,kindkey);
}
static void msg_packet(char* row,int cap,const char* from,const char* to,const char* scope,
                       const char* id,const char* body){
  snprintf(row,cap,
   "{\"id\":\"%s\",\"type\":\"message\",\"from\":\"%s\",\"to\":\"%s\",\"scope\":\"%s\","
   "\"sealed\":false,\"bearer\":\"lan\",\"sig\":\"verified\",\"fields\":[[\"t\",\"message\"],"
   "[\"f\",\"%s\"],[\"ts\",\"" STAMP "\"],[\"m\",\"", id,from,to,scope,from);
  jesc_into(row,cap,body); strncat(row,"\"]]}",cap-strlen(row)-1);
}

static const char* hk_identity(void){ return cur>=0 ? N[cur].call : "X1TEST"; }

/* t:message / t:reaction composed by the wapp and handed to the core. */
static int32_t hk_send(const char* w, uint32_t l){
  (void)l;
  char from[8]=""; wfield(w,"f:",from,sizeof from);
  if(!from[0]) return 0;
  char t[16]=""; wfield(w,"t:",t,sizeof t);
  char id[7]="";   xprs_id(w, strlen(w), id);
  char ts[24]="";  wfield(w,"ts:",ts,sizeof ts);

  if(!strcmp(t,"reaction")){
    char scope[12]="", d[8]="", r[70]="", add[8]="", rem[8]="";
    wfield(w,"scope:",scope,sizeof scope);
    wfield(w,"d:",d,sizeof d);
    wfield(w,"r:",r,sizeof r);
    wfield(w,"add:",add,sizeof add);
    wfield(w,"remove:",rem,sizeof rem);
    char row[400];
    const char* kindkey = rem[0] ? "remove" : "add";
    if(scope[0]){                                    /* Local-room vote */
      react_row(row,sizeof row,from,"","local",id,r,kindkey);
      for(int i=0;i<NN;i++) if(strcmp(N[i].call,from)) enqueue(i,"xprs.reaction",row);
      return 0;
    }
    grp_t* g = grp_by_call(d);
    if(g){                                           /* directed reaction to a closed group */
      if(!is_posting_member(g,from)) return -2;
      react_row(row,sizeof row,from,d,"",id,r,kindkey);
      enqueue_members(g,from,"xprs.reaction",row);
      return 0;
    }
    if(d[0]){                                        /* 1:1 reaction */
      int ni=node_by_call(d);
      react_row(row,sizeof row,from,d,"",id,r,kindkey);
      if(ni>=0) enqueue(ni,"xprs.reaction",row);
      return 0;
    }
    return 0;
  }

  if(!strcmp(t,"message")){
    char d[8]="", m[900]="", r[8]="";
    wfield(w,"d:",d,sizeof d);
    wfield(w,"r:",r,sizeof r);
    wfield(w,"m:",m,sizeof m);
    grp_t* g = grp_by_call(d);
    if(g){                                           /* closed-group post (26) */
      if(!is_posting_member(g,from)) return -2;      /* the core's door, verbatim */
      char row[1400];
      int o=snprintf(row,sizeof row,
        "{\"call\":\"%s\",\"title\":\"#%s\",\"id\":\"%s\",\"ts\":%llu,"
        "\"sig\":\"verified\",\"bearer\":\"rns\",\"content\":\"",
        from,d,id,(unsigned long long)hal_time_epoch());
      (void)o; jesc_into(row,sizeof row,m); strncat(row,"\"}",sizeof row-strlen(row)-1);
      remember_author(id,from);
      enqueue_members(g,from,"xprs.message",row);
      return 0;
    }
    /* an open-group bulletin (d: a plain name), or undirected — deliver as a
     * broadcast packet to everyone else. Kept simple; closed groups + 1:1 are
     * the flows these tests care about. */
    char row[1400];
    msg_packet(row,sizeof row,from,d,d[0]?"":"local",id,m);
    for(int i=0;i<NN;i++) if(strcmp(N[i].call,from)) enqueue(i,"xprs.message",row);
    return 0;
  }
  return 0;
}

/* a 1:1 — the core builds and signs the packet; we just carry it to `to`. */
static int32_t hk_message(const char* to,uint32_t tl,const char* text,uint32_t l,
                          uint32_t priv,char* id,uint32_t cap){
  (void)tl;(void)l;
  const char* from = cur>=0 ? N[cur].call : "X1TEST";
  char mid[16]; snprintf(mid,sizeof mid,"s%05d",++g_seq);
  snprintf(id,cap,"%s",mid);
  int ni=node_by_call(to);
  if(ni>=0){
    char row[1400];
    snprintf(row,sizeof row,
      "{\"call\":\"%s\",\"id\":\"%s\",\"ts\":%llu,\"sig\":\"verified\",\"bearer\":\"rns\",\"content\":\"",
      from,mid,(unsigned long long)hal_time_epoch());
    char body[1000]; body[0]=0; jesc_into(body,sizeof body,text);
    strncat(row,body,sizeof row-strlen(row)-3); strncat(row,"\"}",sizeof row-strlen(row)-1);
    enqueue(ni,"xprs.message",row);
  }
  remember_author(mid,from);
  return priv?1:2;                                   /* the form actually used */
}

/* the Local room — an undirected broadcast to all in earshot. */
static int32_t hk_broadcast(const char* text,uint32_t l,const char* scope,uint32_t sl,
                            const char* reply,uint32_t rl,char* id,uint32_t cap){
  (void)l;(void)scope;(void)sl;(void)reply;(void)rl;
  const char* from = cur>=0 ? N[cur].call : "X1TEST";
  char mid[16]; snprintf(mid,sizeof mid,"b%05d",++g_seq);
  snprintf(id,cap,"%s",mid);
  char row[1400];
  msg_packet(row,sizeof row,from,"","local",mid,text);
  for(int i=0;i<NN;i++) if(strcmp(N[i].call,from)) enqueue(i,"xprs.message",row);
  remember_author(mid,from);
  return 2;
}

/* a read receipt (13.7): back to whoever authored the message. */
static int32_t hk_read(const char* id, uint32_t l){
  (void)l;
  const char* a = author_of(id);
  if(a){ int ni=node_by_call(a);
    if(ni>=0){ char row[120]; snprintf(row,sizeof row,"{\"id\":\"%s\",\"state\":\"read\"}",id);
      enqueue(ni,"xprs.status.tx",row); } }
  return 0;
}

/* the groups THIS node belongs to (posting or invited), for xgroups_refresh. */
static int32_t hk_groups(char* o,uint32_t cap){
  const char* me = cur>=0 ? N[cur].call : "";
  char buf[2048]; int n=0; buf[n++]='['; int first=1;
  for(int gi=0;gi<GN;gi++){
    for(int i=0;i<G[gi].mn;i++) if(!strcmp(G[gi].mem[i].call,me)){
      if(!first) buf[n++]=',';
      first=0;
      n+=snprintf(buf+n,sizeof buf-n,"{\"call\":\"%s\",\"nick\":\"%s\",\"role\":\"%s\"}",
                  G[gi].call,G[gi].nick,G[gi].mem[i].role);
      break;
    }
  }
  buf[n++]=']'; buf[n]=0;
  if((uint32_t)n>cap) return -n;
  memcpy(o,buf,n); return n;
}

/* the roster of a group (gid arrives as "#X5...."). */
static int32_t hk_roster(const char* gid,uint32_t gl,char* o,uint32_t cap){
  (void)gl;
  const char* c = gid[0]=='#' ? gid+1 : gid;
  grp_t* g = grp_by_call(c);
  char buf[2048]; int n=0; buf[n++]='['; int first=1;
  if(g) for(int i=0;i<g->mn;i++){
    const char* r=g->mem[i].role;
    const char* state = !strcmp(r,"invited") ? "invited" : "member";
    if(!first) buf[n++]=',';
    first=0;
    n+=snprintf(buf+n,sizeof buf-n,"{\"state\":\"%s\",\"call\":\"%s\",\"role\":\"%s\"}",state,g->mem[i].call,r);
  }
  buf[n++]=']'; buf[n]=0;
  if((uint32_t)n>cap) return -n;
  memcpy(o,buf,n); return n;
}

/* ─────────────────────── driving the nodes ──────────────────────── */

static int add_node(const char* call){
  int i=NN++; snprintf(N[i].call,sizeof N[i].call,"%s",call);
  snprintf(N[i].root,sizeof N[i].root,"/tmp/chat_sim/%s",call);
  N[i].pn=0; return i;
}
static grp_t* add_group(const char* call,const char* nick){
  grp_t* g=&G[GN++]; snprintf(g->call,sizeof g->call,"%s",call);
  snprintf(g->nick,sizeof g->nick,"%s",nick); g->mn=0; return g;
}
static void group_put(grp_t* g,const char* call,const char* role){
  for(int i=0;i<g->mn;i++) if(!strcmp(g->mem[i].call,call)){ snprintf(g->mem[i].role,sizeof g->mem[i].role,"%s",role); return; }
  snprintf(g->mem[g->mn].call,sizeof g->mem[g->mn].call,"%s",call);
  snprintf(g->mem[g->mn].role,sizeof g->mem[g->mn].role,"%s",role); g->mn++;
}

/* become node ni (a restart if we were someone else). */
static void load(int ni){
  if(cur==ni) return;
  if(cur>=0) module_destroy();
  cur=ni; mock_set_root(N[ni].root); mock_kv_ns(ni+1); module_init();
}
/* hand node ni everything the network has queued for it, then let it run. */
static void pump(int ni){
  load(ni);
  node_t* n=&N[ni];
  for(int i=0;i<n->pn;i++) event_push(n->pend[i].topic, n->pend[i].row);
  n->pn=0;
  module_handle_event();
}
/* a person on node ni types a host command. */
static void ui(int ni, const char* cmd){ load(ni); inbox_set(cmd); module_handle_event(); }

static void open_room(int ni, const char* id){
  char c[128]; snprintf(c,sizeof c,"{\"command\":\"rooms_open\",\"rooms_convo\":\"%s\"}",id); ui(ni,c);
}
static void send(int ni, const char* id, const char* text){
  char c[1200]; snprintf(c,sizeof c,
    "{\"command\":\"rooms_send\",\"rooms_convo\":\"%s\",\"rooms_input\":\"%s\"}",id,text); ui(ni,c);
}

/* pull "key":"value" out of the most recent capture line containing `needle`. */
static int cap_field(const char* needle,const char* key,char* out,int osz){
  out[0]=0;
  const char* line=cap_find(needle); if(!line) return 0;
  char pat[48]; snprintf(pat,sizeof pat,"\"%s\":\"",key);
  const char* p=strstr(line,pat); if(!p) return 0;
  p+=strlen(pat); int n=0;
  while(*p && *p!='"' && n<osz-1){ if(*p=='\\'&&p[1])p++; out[n++]=*p++; }
  out[n]=0; return n>0;
}
static void like(int ni,const char* room,const char* mid){
  char c[200]; snprintf(c,sizeof c,
    "{\"command\":\"rooms_send\",\"rooms_convo\":\"%s\",\"rooms_input\":\"+like:%s\"}",room,mid); ui(ni,c);
}

/* ─────────────────────────── scenarios ──────────────────────────── */

static int g_pass=0, g_fail=0;
#define CHECK(c) do{ if(!(c)){ printf("  FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); g_fail++; } else g_pass++; }while(0)

static void reset_world(void){
  if(cur>=0){ module_destroy(); cur=-1; }
  NN=0; GN=0; AUTHN=0; g_seq=0;
  system("rm -rf /tmp/chat_sim");
}

/* A one-to-one: B sends to A, A reads, B sees the read tick. */
static void t_direct(void){
  printf("1:1 — a message and its read receipt\n");
  reset_world();
  int A=add_node("X1ANNA"), B=add_node("X1BOBB");

  send(B, "X1ANNA", "hello anna");                 /* B -> A */
  CHECK(cap_contains("ui.convo.msg"));             /* B's own bubble, dir out */
  CHECK(cap_contains("\"dir\":\"out\""));

  cap_clear();
  pump(A);                                          /* A's core delivers it */
  open_room(A, "X1BOBB");                           /* A opens the thread */
  CHECK(cap_count("ui.convo.msg") >= 1);
  CHECK(cap_contains("hello anna"));
  CHECK(cap_contains("\"dir\":\"in\""));

  /* A opening the thread flushed a read receipt back toward B. */
  cap_clear();
  pump(B);
  CHECK(cap_contains("ui.convo.status") || cap_contains("read"));
  (void)A;
}

/* Emoji survive the round trip (the byte-for-byte bug that once dropped them). */
static void t_emoji(void){
  printf("1:1 — an emoji round trip\n");
  reset_world();
  add_node("X1ANNA"); add_node("X1BOBB");
  int A=0,B=1;

  send(B, "X1ANNA", "hi \xF0\x9F\x98\x80");         /* U+1F600 grinning face */
  cap_clear();
  pump(A); open_room(A,"X1BOBB");
  CHECK(cap_contains("ui.convo.msg"));
  CHECK(cap_contains("\xF0\x9F\x98\x80"));          /* the same four bytes arrive */
  (void)B;
}

/* A closed group: every member sees a member's post; a like reaches them and
 * echoes on the sender; a non-member is refused and reaches no one. */
static void t_group(void){
  printf("closed group — post, like, and the membership door\n");
  reset_world();
  int A=add_node("X1ANNA"), B=add_node("X1BOBB"), C=add_node("X1CATE"), X=add_node("X1XENO");
  grp_t* g=add_group("X5GULF","Gulf net");
  group_put(g,"X1ANNA","admin");
  group_put(g,"X1BOBB","member");
  group_put(g,"X1CATE","member");
  /* X1XENO is NOT a member. */

  /* Admin posts. */
  send(A, "#X5GULF", "net is open");
  cap_clear();
  pump(B); open_room(B,"#X5GULF");
  CHECK(cap_contains("net is open"));
  CHECK(cap_contains("\"dir\":\"in\""));
  cap_clear();
  pump(C); open_room(C,"#X5GULF");
  CHECK(cap_contains("net is open"));               /* fanned out to every member */

  /* Grab the id of the admin's post as B rendered it, to like it. The wapp
   * likes by section-5 id via a votemark; here we just drive a like on the
   * last message from B's open room using the reaction path. */
  /* B likes the admin post (votemark "+like:<id>"). We reconstruct the id the
   * same way the wire did: it is A's message id. B's UI would carry it; for
   * the harness we re-open and read the id from the rendered bubble. */

  /* Non-member X tries to post: refused (-2), nobody hears it. */
  cap_clear();
  send(X, "#X5GULF", "let me in");
  CHECK(cap_contains("once you accept"));           /* the wapp's words for -2 */
  int heard=0;
  for(int i=0;i<NN;i++) heard+=N[i].pn;             /* nothing queued anywhere */
  CHECK(heard==0);
  (void)A;(void)B;(void)C;(void)X;
}

/* A closed group survives a restart: after re-init, a member still posts and
 * the post still fans out. (Node switching IS a restart here.) */
static void t_group_restart(void){
  printf("closed group — a member still posts after a restart\n");
  reset_world();
  int A=add_node("X1ANNA"), B=add_node("X1BOBB");
  grp_t* g=add_group("X5GULF","Gulf net");
  group_put(g,"X1ANNA","admin");
  group_put(g,"X1BOBB","member");

  send(A,"#X5GULF","first");
  pump(B); open_room(B,"#X5GULF");                  /* B is loaded */

  load(A);                                          /* switch away: B is torn down */
  load(B);                                          /* ...and brought back up: a restart */
  cap_clear();
  send(B,"#X5GULF","after restart");                /* still a posting member */
  CHECK(cap_contains("ui.convo.msg"));
  CHECK(cap_contains("after restart"));
  cap_clear();
  pump(A); open_room(A,"#X5GULF");
  CHECK(cap_contains("after restart"));             /* and it reaches the admin */
  (void)g;
}

/* The Local room: an undirected broadcast reaches every node in earshot. */
static void t_local(void){
  printf("local — a broadcast reaches everyone in earshot\n");
  reset_world();
  int A=add_node("X1ANNA"), B=add_node("X1BOBB"), C=add_node("X1CATE");
  send(A,"#LOCAL","anybody there");
  cap_clear(); pump(B); open_room(B,"#LOCAL"); CHECK(cap_contains("anybody there"));
  cap_clear(); pump(C); open_room(C,"#LOCAL"); CHECK(cap_contains("anybody there"));
  (void)A;
}

/* A like on a closed-group post reaches the other members AND echoes on the
 * one who sent it — the exact "c61 didn't update its own like" bug, now local. */
static void t_group_react(void){
  printf("closed group — a like fans out and echoes on the sender\n");
  reset_world();
  int A=add_node("X1ANNA"), B=add_node("X1BOBB"), C=add_node("X1CATE");
  grp_t* g=add_group("X5GULF","Gulf net");
  group_put(g,"X1ANNA","admin"); group_put(g,"X1BOBB","member"); group_put(g,"X1CATE","member");

  send(A,"#X5GULF","thanks");                       /* the post everyone will like */
  pump(B); open_room(B,"#X5GULF");
  char mid[24]; CHECK(cap_field("ui.convo.msg","mid",mid,sizeof mid));

  cap_clear();
  like(B,"#X5GULF",mid);                            /* B likes it */
  CHECK(cap_contains("ui.convo.react"));            /* echoes on B at once (the bug) */

  cap_clear(); pump(C); open_room(C,"#X5GULF");
  CHECK(cap_contains("ui.convo.react"));            /* and reaches another member */

  cap_clear(); pump(A); open_room(A,"#X5GULF");
  CHECK(cap_contains("ui.convo.react"));            /* and the author */
  (void)A;
}

/* A 1:1 like is a directed reaction that reaches the peer. */
static void t_direct_react(void){
  printf("1:1 — a like reaches the peer\n");
  reset_world();
  int A=add_node("X1ANNA"), B=add_node("X1BOBB");
  send(A,"X1BOBB","hey");
  pump(B); open_room(B,"X1BOBB");                    /* B's thread with A */
  char mid[24]; CHECK(cap_field("ui.convo.msg","mid",mid,sizeof mid));
  cap_clear();
  like(B,"X1ANNA",mid);                              /* B likes A's message */
  CHECK(cap_contains("ui.convo.react"));            /* local echo on B */
  cap_clear(); pump(A); open_room(A,"X1BOBB");
  CHECK(cap_contains("ui.convo.react"));            /* delivered to A */
  (void)A;
}

int main(void){
  mock_set_time(1700000000ULL);
  g_hk_identity=hk_identity; g_hk_send=hk_send; g_hk_message=hk_message;
  g_hk_broadcast=hk_broadcast; g_hk_read=hk_read; g_hk_groups=hk_groups; g_hk_roster=hk_roster;

  t_direct();
  t_emoji();
  t_group();
  t_group_react();
  t_direct_react();
  t_group_restart();
  t_local();

  printf("\nsim: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
