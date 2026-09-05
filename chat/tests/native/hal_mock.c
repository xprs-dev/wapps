/*
 * A native HAL for the chat wapp's tests: real sqlite3 behind hal_sqlite_*,
 * an in-memory KV, a capture of everything hal_msg_send says, and an
 * injectable event queue for module_handle_event. Modelled on
 * circles/tests/native/hal_mock.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>

/* ---- minimal sqlite3 decls (no dev header needed) ---- */
typedef struct sqlite3 sqlite3; typedef struct sqlite3_stmt sqlite3_stmt;
extern int sqlite3_open(const char*, sqlite3**);
extern int sqlite3_close(sqlite3*);
extern int sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
extern int sqlite3_bind_text(sqlite3_stmt*, int, const char*, int, void(*)(void*));
extern int sqlite3_bind_int64(sqlite3_stmt*, int, long long);
extern int sqlite3_bind_null(sqlite3_stmt*, int);
extern int sqlite3_step(sqlite3_stmt*);
extern int sqlite3_finalize(sqlite3_stmt*);
extern int sqlite3_column_count(sqlite3_stmt*);
extern const char* sqlite3_column_name(sqlite3_stmt*, int);
extern int sqlite3_column_type(sqlite3_stmt*, int);
extern const unsigned char* sqlite3_column_text(sqlite3_stmt*, int);
extern long long sqlite3_column_int64(sqlite3_stmt*, int);
extern const char* sqlite3_errmsg(sqlite3*);
#define SROW 100
#define SDONE 101
#define SOK 0
#define S_INT 1
#define S_NULL 5
#define S_TRANSIENT ((void(*)(void*))-1)

const char *g_root = "/tmp/chat_native_test";

/* ---- capture of hal_msg_send ---- */
#define MAXCAP 8000
static char* g_cap[MAXCAP]; static int g_capn=0;
void cap_clear(void){ for(int i=0;i<g_capn;i++) free(g_cap[i]); g_capn=0; }
int  cap_count(const char* s){ int n=0; for(int i=0;i<g_capn;i++) if(strstr(g_cap[i],s)) n++; return n; }
int  cap_contains(const char* s){ return cap_count(s)>0; }
const char* cap_find(const char* s){ for(int i=0;i<g_capn;i++) if(strstr(g_cap[i],s)) return g_cap[i]; return 0; }
const char* cap_nth(int i){ return i<g_capn? g_cap[i]:0; }
int  cap_n(void){ return g_capn; }
void hal_msg_send(const char* json, uint32_t len){
  if(g_capn<MAXCAP){ char* c=(char*)malloc(len+1); memcpy(c,json,len); c[len]=0; g_cap[g_capn++]=c; }
}

/* ---- injectable host inbox ---- */
static char g_inbox[16384]; static int g_inbox_set=0;
void inbox_set(const char* s){ strncpy(g_inbox,s,sizeof(g_inbox)-1); g_inbox[sizeof(g_inbox)-1]=0; g_inbox_set=1; }
uint32_t hal_msg_available(void){ return g_inbox_set? (uint32_t)strlen(g_inbox):0; }
uint32_t hal_msg_recv(char* buf, uint32_t cap){ if(!g_inbox_set) return 0; uint32_t n=strlen(g_inbox); if(n>cap)n=cap; memcpy(buf,g_inbox,n); g_inbox_set=0; return n; }

/* ---- injectable core event queue ---- */
#define EVQ 64
static char* g_ev_topic[EVQ]; static char* g_ev_row[EVQ]; static int g_ev_r=0, g_ev_w=0;
void event_push(const char* topic, const char* row){
  if(g_ev_w-g_ev_r>=EVQ) return;
  g_ev_topic[g_ev_w%EVQ]=strdup(topic); g_ev_row[g_ev_w%EVQ]=strdup(row); g_ev_w++;
}
int32_t hal_event_subscribe(const char* t,uint32_t l){ (void)t;(void)l; return 0; }
int32_t hal_event_unsubscribe(const char* t,uint32_t l){ (void)t;(void)l; return 0; }
int32_t hal_event_publish(const char* t,uint32_t tl,const char* p,uint32_t pl){ (void)t;(void)tl;(void)p;(void)pl; return 0; }
uint32_t hal_event_available(void){ return (uint32_t)(g_ev_w-g_ev_r); }
uint32_t hal_event_recv(char* tb,uint32_t tc,char* rb,uint32_t rc){
  if(g_ev_w==g_ev_r) return 0;
  int i=g_ev_r%EVQ; g_ev_r++;
  snprintf(tb,tc,"%s",g_ev_topic[i]); uint32_t n=strlen(g_ev_row[i]); if(n>rc) n=rc; memcpy(rb,g_ev_row[i],n);
  free(g_ev_topic[i]); free(g_ev_row[i]); return n;
}

/* ---- log capture ---- */
static char* g_log[2000]; static int g_logn=0;
void hal_log(int32_t lvl,const char* m,uint32_t n){ (void)lvl; if(g_logn<2000){ char* c=malloc(n+1); memcpy(c,m,n); c[n]=0; g_log[g_logn++]=c; } }
int log_count(const char* s){ int k=0; for(int i=0;i<g_logn;i++) if(strstr(g_log[i],s)) k++; return k; }
void log_clear(void){ for(int i=0;i<g_logn;i++) free(g_log[i]); g_logn=0; }

/* ---- time / identity ---- */
static uint64_t g_now=1700000000ULL;
void mock_set_time(uint64_t t){ g_now=t; }
uint64_t hal_time_ms(void){ return g_now*1000; }
uint64_t hal_time_epoch(void){ return g_now; }
int32_t hal_time_utc_offset(void){ return 0; }
uint32_t hal_identity(char* b,uint32_t cap){ const char* s="X1TEST"; uint32_t n=strlen(s); if(n>cap)n=cap; memcpy(b,s,n); return n; }

/* ---- kv (in memory) ---- */
#define KVMAX 64
static char* g_kv_k[KVMAX]; static char* g_kv_v[KVMAX]; static uint32_t g_kv_l[KVMAX]; static int g_kvn=0;
static int kv_find(const char* k,uint32_t kl){ for(int i=0;i<g_kvn;i++) if(strlen(g_kv_k[i])==kl&&!memcmp(g_kv_k[i],k,kl)) return i; return -1; }
void mock_kv_set(const char* k,const char* v){ int i=kv_find(k,strlen(k)); if(i<0){ i=g_kvn++; g_kv_k[i]=strdup(k); g_kv_v[i]=0; } free(g_kv_v[i]); g_kv_v[i]=strdup(v); g_kv_l[i]=strlen(v); }
int mock_kv_exists(const char* k){ return kv_find(k,strlen(k))>=0; }
uint32_t hal_kv_get(const char* k,uint32_t kl,char* o,uint32_t cap){ int i=kv_find(k,kl); if(i<0) return 0; uint32_t n=g_kv_l[i]; if(n>cap)n=cap; memcpy(o,g_kv_v[i],n); return n; }
int32_t hal_kv_set(const char* k,uint32_t kl,const char* v,uint32_t vl){ char kk[64]; if(kl>=sizeof(kk)) return -1; memcpy(kk,k,kl); kk[kl]=0; int i=kv_find(kk,kl); if(i<0){ i=g_kvn++; g_kv_k[i]=strdup(kk); g_kv_v[i]=0; } free(g_kv_v[i]); g_kv_v[i]=malloc(vl+1); memcpy(g_kv_v[i],v,vl); g_kv_v[i][vl]=0; g_kv_l[i]=vl; return 0; }
int32_t hal_kv_delete(const char* k,uint32_t kl){ int i=kv_find(k,kl); if(i<0) return -1; free(g_kv_k[i]); free(g_kv_v[i]); for(int j=i;j<g_kvn-1;j++){ g_kv_k[j]=g_kv_k[j+1]; g_kv_v[j]=g_kv_v[j+1]; g_kv_l[j]=g_kv_l[j+1]; } g_kvn--; return 0; }

/* ---- xprs doors ---- */
static int g_bcast_n=0; static char g_last_wire[1024]; static char g_history[65536]="[]";
const char* mock_last_wire(void){ return g_last_wire; }
void mock_set_history(const char* json){ snprintf(g_history,sizeof(g_history),"%s",json); }
int32_t hal_xprs_send(const char* w,uint32_t l){ if(l>=sizeof(g_last_wire)) return -1; memcpy(g_last_wire,w,l); g_last_wire[l]=0; return 0; }
int32_t hal_xprs_message(const char* to,uint32_t tl,const char* t,uint32_t l,uint32_t priv,char* id,uint32_t cap){ (void)to;(void)tl;(void)t;(void)l; snprintf(id,cap,"m%05d",++g_bcast_n); return priv?1:2; }
int32_t hal_xprs_broadcast(const char* t,uint32_t l,const char* s,uint32_t sl,const char* r,uint32_t rl,char* id,uint32_t cap){ (void)t;(void)l;(void)s;(void)sl;(void)r;(void)rl; snprintf(id,cap,"b%05d",++g_bcast_n); return 2; }
int32_t hal_xprs_read(const char* id,uint32_t l){ (void)id;(void)l; return 0; }
int32_t hal_xprs_history(const char* q,uint32_t ql,char* o,uint32_t cap){ (void)q;(void)ql; uint32_t n=strlen(g_history); if(n>cap) return -(int32_t)n; memcpy(o,g_history,n); return n; }
int32_t hal_xprs_groups(char* o,uint32_t cap){ const char* s="[]"; uint32_t n=strlen(s); if(n>cap) return -2; memcpy(o,s,n); return n; }
int32_t hal_xprs_stations(char* o,uint32_t cap){ const char* s="[{\"title\":\"Heard over the air (2)\",\"items\":[{\"id\":\"X1NEAR\",\"title\":\"X1NEAR\",\"subtitle\":\"BLE - -40 dBm - 3 packets\",\"tags\":[\"seen 20s ago\",\"BLE\",\"peers 2\"]},{\"id\":\"X1TEST\",\"title\":\"X1TEST\",\"subtitle\":\"BLE\",\"tags\":[\"seen 5s ago\",\"BLE\"]}]},{\"title\":\"Heard this hour (1)\",\"items\":[{\"id\":\"X1EARLIER\",\"title\":\"X1EARLIER\",\"subtitle\":\"LAN\",\"tags\":[\"seen 40m ago\",\"LAN\"]}]},{\"title\":\"On Reticulum (1)\",\"items\":[{\"id\":\"X1FAR\",\"title\":\"X1FAR\",\"subtitle\":\"RNS\",\"tags\":[\"seen 3m ago\",\"RNS\"]}]}]"; uint32_t n=strlen(s); if(n>cap) return -(int32_t)n; memcpy(o,s,n); return n; }
int32_t hal_mesh_devices(char* o,uint32_t cap){ const char* s="[{\"title\":\"Nearby\",\"items\":[{\"id\":\"X1NEAR\",\"title\":\"X1NEAR\",\"subtitle\":\"phone\"}]},{\"title\":\"Multi-hop\",\"items\":[{\"id\":\"X1FAR\",\"title\":\"X1FAR\"}]}]"; uint32_t n=strlen(s); if(n>cap) return -(int32_t)n; memcpy(o,s,n); return n; }
int32_t hal_people_directory(const char* q,uint32_t ql,char* o,uint32_t cap){ (void)q;(void)ql; const char* s="[{\"kind\":\"xprs\",\"callsign\":\"X1PEER\",\"nick\":\"Peer\",\"live\":true}]"; uint32_t n=strlen(s); if(n>cap) return -2; memcpy(o,s,n); return n; }

/* ---- sqlite-backed hal_sqlite_* ---- */
#define MAXH 128
static sqlite3* g_db[MAXH]; static int g_dbn=1; static char g_err[256];
static uint32_t g_query_cap_limit=0;   /* tests can force -2 */
void mock_query_cap(uint32_t c){ g_query_cap_limit=c; }
static void mkparents(const char* full){ char tmp[700]; strncpy(tmp,full,sizeof(tmp)-1); tmp[sizeof(tmp)-1]=0; for(char* p=tmp+1;*p;p++){ if(*p=='/'){ *p=0; mkdir(tmp,0777); *p='/'; } } }
int32_t hal_sqlite_open(const char* path,uint32_t len){
  char rel[512]; if(len>=sizeof(rel))return -1; memcpy(rel,path,len); rel[len]=0;
  char full[700]; snprintf(full,sizeof(full),"%s/%s",g_root,rel); mkparents(full);
  sqlite3* db; if(sqlite3_open(full,&db)!=SOK) return -1;
  if(g_dbn>=MAXH) return -1; int h=g_dbn++; g_db[h]=db; return h;
}
static void bind_params(sqlite3_stmt* st,const char* params,uint32_t plen){
  if(!params||plen==0) return; const char* p=params; while(*p&&*p!='[')p++; if(*p)p++; int idx=1;
  while(*p){ while(*p==' '||*p==',')p++; if(*p==']'||!*p) break;
    if(*p=='"'){ p++; static char val[8192]; int v=0; while(*p&&*p!='"'){ if(*p=='\\'&&p[1]){ p++; if(*p=='n')val[v++]='\n'; else if(*p=='t')val[v++]='\t'; else if(*p=='u'){ val[v++]=(char)strtol((char[]){p[3],p[4],0},0,16); p+=4; } else val[v++]=*p; } else val[v++]=*p; p++; } val[v]=0; if(*p=='"')p++; sqlite3_bind_text(st,idx++,val,v,S_TRANSIENT); }
    else if(*p=='n'){ while(*p&&*p!=','&&*p!=']')p++; sqlite3_bind_null(st,idx++); }
    else { char num[64]; int n=0; while(*p&&(*p=='-'||(*p>='0'&&*p<='9'))){ if(n<63)num[n++]=*p; p++; } num[n]=0; sqlite3_bind_int64(st,idx++,atoll(num)); }
  }
}
int32_t hal_sqlite_exec(int32_t h,const char* sql,uint32_t sl,const char* params,uint32_t pl){
  if(h<=0||h>=MAXH||!g_db[h]) return -1; static char z[16384]; if(sl>=sizeof(z))return -1; memcpy(z,sql,sl); z[sl]=0;
  sqlite3_stmt* st; if(sqlite3_prepare_v2(g_db[h],z,-1,&st,0)!=SOK){ snprintf(g_err,sizeof(g_err),"%s",sqlite3_errmsg(g_db[h])); return -1; }
  bind_params(st,params,pl); int rc; while((rc=sqlite3_step(st))==SROW){} sqlite3_finalize(st);
  if(rc!=SDONE){ snprintf(g_err,sizeof(g_err),"%s",sqlite3_errmsg(g_db[h])); return -1; } return 0;
}
int32_t hal_sqlite_query(int32_t h,const char* sql,uint32_t sl,const char* params,uint32_t pl,char* out,uint32_t cap){
  if(h<=0||h>=MAXH||!g_db[h]) return -1; static char z[16384]; if(sl>=sizeof(z))return -1; memcpy(z,sql,sl); z[sl]=0;
  sqlite3_stmt* st; if(sqlite3_prepare_v2(g_db[h],z,-1,&st,0)!=SOK){ snprintf(g_err,sizeof(g_err),"%s",sqlite3_errmsg(g_db[h])); return -1; }
  bind_params(st,params,pl);
  static char buf[1<<20]; int o=0; buf[o++]='['; int first=1; int rc;
  while((rc=sqlite3_step(st))==SROW){ if(!first)buf[o++]=','; first=0; buf[o++]='{'; int nc=sqlite3_column_count(st);
    for(int c=0;c<nc;c++){ if(c)buf[o++]=','; o+=sprintf(buf+o,"\"%s\":",sqlite3_column_name(st,c)); int ty=sqlite3_column_type(st,c);
      if(ty==S_NULL){ o+=sprintf(buf+o,"null"); }
      else if(ty==S_INT){ o+=sprintf(buf+o,"%lld",sqlite3_column_int64(st,c)); }
      else { const unsigned char* t=sqlite3_column_text(st,c); buf[o++]='"'; for(const unsigned char* q=t;q&&*q;q++){ if(*q=='"'||*q=='\\'){buf[o++]='\\';buf[o++]=*q;} else if(*q=='\n'){buf[o++]='\\';buf[o++]='n';} else buf[o++]=*q; } buf[o++]='"'; }
    } buf[o++]='}';
  }
  sqlite3_finalize(st); buf[o++]=']'; buf[o]=0;
  if(g_query_cap_limit && (uint32_t)o>g_query_cap_limit) return -2;
  if((uint32_t)o>cap) return -2; memcpy(out,buf,o); return o;
}
uint32_t hal_sqlite_error(int32_t h,char* o,uint32_t cap){ (void)h; uint32_t n=strlen(g_err); if(n>cap)n=cap; memcpy(o,g_err,n); return n; }
void hal_sqlite_close(int32_t h){ if(h>0&&h<MAXH&&g_db[h]){ sqlite3_close(g_db[h]); g_db[h]=0; } }
