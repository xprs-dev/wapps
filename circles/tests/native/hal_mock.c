#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>

/* ---- minimal sqlite3 decls (no dev header installed) ---- */
typedef struct sqlite3 sqlite3; typedef struct sqlite3_stmt sqlite3_stmt;
extern int sqlite3_open(const char*, sqlite3**);
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

/* ---- capture of hal_msg_send ---- */
#define MAXCAP 8000
static char* g_cap[MAXCAP]; static int g_capn=0;
void cap_clear(void){ for(int i=0;i<g_capn;i++) free(g_cap[i]); g_capn=0; }
int  cap_contains(const char* s){ for(int i=0;i<g_capn;i++) if(strstr(g_cap[i],s)) return 1; return 0; }
const char* cap_find(const char* s){ for(int i=0;i<g_capn;i++) if(strstr(g_cap[i],s)) return g_cap[i]; return 0; }

void hal_msg_send(const char* json, uint32_t len){
  if(g_capn<MAXCAP){ char* c=(char*)malloc(len+1); memcpy(c,json,len); c[len]=0; g_cap[g_capn++]=c; }
}

/* ---- injectable inbox for module_handle_event ---- */
static char g_inbox[16384]; static int g_inbox_set=0;
void inbox_set(const char* s){ strncpy(g_inbox,s,sizeof(g_inbox)-1); g_inbox[sizeof(g_inbox)-1]=0; g_inbox_set=1; }
uint32_t hal_msg_available(void){ return g_inbox_set? (uint32_t)strlen(g_inbox):0; }
uint32_t hal_msg_recv(char* buf, uint32_t cap){ if(!g_inbox_set) return 0; uint32_t n=strlen(g_inbox); if(n>cap)n=cap; memcpy(buf,g_inbox,n); g_inbox_set=0; return n; }

/* ---- misc HAL ---- */
void hal_log(int32_t lvl,const char* m,uint32_t n){ (void)lvl;(void)m;(void)n; }
uint64_t hal_time_ms(void){ return 0; }
uint64_t hal_time_epoch(void){ return 1700000000ULL; }
uint32_t hal_identity_pubkey(char* b,uint32_t cap){ const char* s="AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"; uint32_t n=strlen(s); if(n>cap)n=cap; memcpy(b,s,n); return n; }
uint32_t hal_identity_sign(const char* m,uint32_t ml,char* o,uint32_t cap){ (void)m;(void)ml; const char* s="SIGSIGSIGSIGSIGSIG"; uint32_t n=strlen(s); if(n>cap)n=cap; memcpy(o,s,n); return n; }
uint32_t hal_verify(const char* p,uint32_t pl,const char* m,uint32_t ml,const char* s,uint32_t sl){ (void)p;(void)pl;(void)m;(void)ml;(void)s;(void)sl; return 1; }
uint32_t hal_npub(const char* p,uint32_t pl,char* o,uint32_t cap){ (void)p;(void)pl; const char* s="npub1mockmock"; uint32_t n=strlen(s); if(n>cap)n=cap; memcpy(o,s,n); return n; }
uint32_t hal_encrypt(const char* p,uint32_t pl,const char* m,uint32_t ml,char* o,uint32_t cap){ (void)p;(void)pl; uint32_t n=ml<cap?ml:cap; memcpy(o,m,n); return n; }
uint32_t hal_decrypt(const char* p,uint32_t pl,const char* b,uint32_t bl,char* o,uint32_t cap){ (void)p;(void)pl; uint32_t n=bl<cap?bl:cap; memcpy(o,b,n); return n; }
uint32_t hal_crypto_keygen(char* o,uint32_t cap){ const char* s="{\"priv\":\"1111111111111111111111111111111111111111111111111111111111111111\",\"pub\":\"2222222222222222222222222222222222222222222222222222222222222222\"}"; uint32_t n=strlen(s); if(n>cap)n=cap; memcpy(o,s,n); return n; }
uint32_t hal_crypto_sign(const char* p,uint32_t pl,const char* m,uint32_t ml,char* o,uint32_t cap){ (void)p;(void)pl;(void)m;(void)ml; const char* s="deadbeef"; uint32_t n=strlen(s); if(n>cap)n=cap; memcpy(o,s,n); return n; }
int32_t hal_crypto_verify(const char* p,uint32_t pl,const char* s,uint32_t sl,const char* m,uint32_t ml){ (void)p;(void)pl;(void)s;(void)sl;(void)m;(void)ml; return 1; }
static unsigned char g_rndctr=1;
uint32_t hal_crypto_random(char* o,uint32_t n){ for(uint32_t i=0;i<n;i++) o[i]=(char)(g_rndctr+i); g_rndctr+=7; return n; }
uint32_t hal_crypto_aes_encrypt(const char* k,uint32_t kl,const char* in,uint32_t il,char* o,uint32_t cap){ (void)k;(void)kl; if(16+il>cap) return 0; for(int i=0;i<16;i++)o[i]=0; memcpy(o+16,in,il); return 16+il; }
uint32_t hal_crypto_aes_decrypt(const char* k,uint32_t kl,const char* in,uint32_t il,char* o,uint32_t cap){ (void)k;(void)kl; if(il<=16) return 0; uint32_t n=il-16; if(n>cap)n=cap; memcpy(o,in+16,n); return n; }
uint32_t hal_rns_identity(char* o,uint32_t cap){ const char* s="rnsid"; uint32_t n=strlen(s); if(n>cap)n=cap; memcpy(o,s,n); return n; }
int32_t  hal_rns_broadcast(const char* p,uint32_t l){ (void)p;(void)l; return 1; }
int32_t  hal_rns_send_to(const char* d,uint32_t dl,const char* p,uint32_t pl){ (void)d;(void)dl;(void)p;(void)pl; return 1; }
int32_t  hal_rns_pull(const char* d,uint32_t dl){ (void)d;(void)dl; return 1; }
uint32_t hal_rns_delivery_dest(char* o,uint32_t cap){ const char* s="delivdest"; uint32_t n=strlen(s); if(n>cap)n=cap; memcpy(o,s,n); return n; }
uint32_t hal_rns_prop_dest(char* o,uint32_t cap){ const char* s="propdest"; uint32_t n=strlen(s); if(n>cap)n=cap; memcpy(o,s,n); return n; }
int32_t  hal_rns_rv_announce(const char* s,uint32_t sl,const char* a,uint32_t al){ (void)s;(void)sl;(void)a;(void)al; return 1; }
uint32_t hal_rns_rv_resolve(const char* s,uint32_t sl,char* o,uint32_t cap){ (void)s;(void)sl;(void)o;(void)cap; return 0; }
int32_t  hal_rns_rv_send(const char* s,uint32_t sl,const char* p,uint32_t pl){ (void)s;(void)sl;(void)p;(void)pl; return 1; }
uint32_t hal_rns_available(void){ return 0; }
uint32_t hal_rns_recv(char* o,uint32_t cap){ (void)o;(void)cap; return 0; }
int32_t  hal_contacts_query(const char* q,uint32_t ql,char* o,uint32_t cap){ (void)q;(void)ql; const char* s="[]"; uint32_t n=strlen(s); if(n>cap)return -2; memcpy(o,s,n); return n; }

/* ---- sqlite-backed hal_sqlite_* ---- */
#define MAXH 128
static sqlite3* g_db[MAXH]; static int g_dbn=1; static char g_err[256];
static void mkparents(const char* full){ char tmp[700]; strncpy(tmp,full,sizeof(tmp)-1); tmp[sizeof(tmp)-1]=0; for(char* p=tmp+1;*p;p++){ if(*p=='/'){ *p=0; mkdir(tmp,0777); *p='/'; } } }
int32_t hal_sqlite_open(const char* path,uint32_t len){
  char rel[512]; if(len>=sizeof(rel))return -1; memcpy(rel,path,len); rel[len]=0;
  char full[700]; snprintf(full,sizeof(full),"/tmp/ct/%s",rel); mkparents(full);
  sqlite3* db; if(sqlite3_open(full,&db)!=SOK) return -1;
  if(g_dbn>=MAXH) return -1; int h=g_dbn++; g_db[h]=db; return h;
}
static void bind_params(sqlite3_stmt* st,const char* params,uint32_t plen){
  if(!params||plen==0) return; const char* p=params; while(*p&&*p!='[')p++; if(*p)p++; int idx=1;
  while(*p){ while(*p==' '||*p==',')p++; if(*p==']'||!*p) break;
    if(*p=='"'){ p++; static char val[8192]; int v=0; while(*p&&*p!='"'){ if(*p=='\\'&&p[1]){ p++; if(*p=='n')val[v++]='\n'; else val[v++]=*p; } else val[v++]=*p; p++; } val[v]=0; if(*p=='"')p++; sqlite3_bind_text(st,idx++,val,v,S_TRANSIENT); }
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
  if((uint32_t)o>cap) return -2; memcpy(out,buf,o); return o;
}
uint32_t hal_sqlite_error(int32_t h,char* o,uint32_t cap){ (void)h; uint32_t n=strlen(g_err); if(n>cap)n=cap; memcpy(o,g_err,n); return n; }
void hal_sqlite_close(int32_t h){ (void)h; }
