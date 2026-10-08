/* Actual network adapter and filesystem RPC tested against a wire-level
 * in-memory UDP server. No real network or source downloads. */
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
typedef int err_t;typedef unsigned short u16_t;
typedef struct {unsigned addr;} ip_addr_t;
#define ip_addr_copy(a,b) ((a)=(b))
#define ip_addr_cmp(a,b) ((a)->addr==(b)->addr)
#define ERR_OK 0
#define ERR_INPROGRESS -5
#define ERR_MEM -1
#define PBUF_TRANSPORT 0
#define PBUF_RAM 0
static char last_log[768],failure_log[768];
static void test_log(const char *f,...){va_list a;va_start(a,f);vsnprintf(last_log,sizeof(last_log),f,a);va_end(a);if(strstr(last_log,"R13A FAIL") || strstr(last_log,"R13C FAIL") || strstr(last_log,"R14A FAIL"))strcpy(failure_log,last_log);}
#define LOG(component,level,...) test_log(__VA_ARGS__)
#define LOG_AT(component,level,...) test_log(__VA_ARGS__)
typedef struct {int unused;} sidetnfs_system_settings_t;
typedef struct {char host[64],mount_path[32];uint16_t port;} sidetnfs_slot_tnfs_context_t;
struct pbuf;
struct udp_pcb {unsigned slot;void *arg;void (*cb)(void*,struct udp_pcb*,struct pbuf*,const ip_addr_t*,u16_t);};
struct pbuf {unsigned tot_len;uint8_t bytes[540];};
static bool fail_pbuf,fail_pcb;
static struct udp_pcb pcbs[32];static unsigned pcb_count,locks;
static uint64_t tick;static int online=1;
static uint32_t time_us_32(void){return (uint32_t)tick;}
static uint64_t time_us_64(void){return tick;}
static void tight_loop_contents(void){tick+=1000;}
static void cyw43_arch_lwip_begin(void){assert(!locks);locks++;}
static void cyw43_arch_lwip_end(void){assert(locks==1);locks--;}
static int r11_radio_link(void){return online;}
static struct pbuf delayed;
static struct udp_pcb *delayed_pcb;static ip_addr_t delayed_addr;static uint32_t delayed_due;
static void r11_radio_poll(void){
 if(delayed_pcb && (int32_t)(tick-delayed_due)>=0){struct udp_pcb *p=delayed_pcb;delayed_pcb=NULL;p->cb(p->arg,p,&delayed,&delayed_addr,16384);}
}
static bool sidetnfs_probe_get_slot_context(unsigned slot,sidetnfs_slot_tnfs_context_t *c){
 memset(c,0,sizeof(*c));c->host[0]='0'+slot;c->port=16384;strcpy(c->mount_path,"/");return true;
}
static unsigned dns_mode;
static void (*dns_cb)(const char*,const ip_addr_t*,void*);static void *dns_arg;
static err_t dns_gethostbyname(const char *host,ip_addr_t *address,void (*cb)(const char*,const ip_addr_t*,void*),void *arg){
 if(dns_mode){dns_cb=cb;dns_arg=arg;if(dns_mode==3)cb(host,NULL,arg);return dns_mode==1?ERR_MEM:ERR_INPROGRESS;}
 address->addr=100+(unsigned)(host[0]-'0');
#ifdef R13C
 if(host[0]=='1')address->addr=100; /* Two mappings to the same peer. */
#endif
 return ERR_OK;
}
static struct udp_pcb *udp_new(void){if(fail_pcb)return NULL;assert(pcb_count<32);pcbs[pcb_count].slot=pcb_count;return &pcbs[pcb_count++];}
static void udp_recv(struct udp_pcb *p,void (*cb)(void*,struct udp_pcb*,struct pbuf*,const ip_addr_t*,u16_t),void *arg){p->cb=cb;p->arg=arg;}
static void udp_remove(struct udp_pcb *p){if(delayed_pcb==p)delayed_pcb=NULL;p->cb=NULL;}
static struct pbuf packet;
static struct pbuf *pbuf_alloc(int a,unsigned n,int b){(void)a;(void)b;if(fail_pbuf)return NULL;packet.tot_len=n;return &packet;}
static void pbuf_take(struct pbuf *p,const void *b,unsigned n){memcpy(p->bytes,b,n);}
static void pbuf_copy_partial(struct pbuf *p,void *b,unsigned n,unsigned off){assert(off+n<=p->tot_len);memcpy(b,p->bytes+off,n);}
static void pbuf_free(struct pbuf *p){(void)p;}
static err_t udp_sendto(struct udp_pcb*,struct pbuf*,const ip_addr_t*,unsigned);
#ifdef R13D
static unsigned radio_diags;
void r13d_radio_diag(const char *where){assert(!strcmp(where,"tnfs-failure"));radio_diags++;}
#include "tnfs_transport.h"
#elif defined(R13C)
#include "r13c_net.h"
#elif !defined(R13A)
#include "r13_net.h"
#else
#include "r13a_net.h"
#endif
static uint16_t r02_be16(const uint8_t *p){return ((unsigned)p[0]<<8)|p[1];}
static uint32_t r02_be32(const uint8_t *p){return ((uint32_t)r02_be16(p)<<16)|r02_be16(p+2);}
static void r02_put16(uint8_t *p,unsigned n){p[0]=n>>8;p[1]=n;}
static void r02_put32(uint8_t *p,uint32_t n){r02_put16(p,n>>16);r02_put16(p+2,n);}
#include "pattern.inc"
typedef struct {char name[14];uint8_t attr;uint16_t time,date;uint32_t size;} SidetnfsAtariDirEntry;
static bool sidetnfs_normalize_dir_entry(const char *name,unsigned flags,uint32_t size,uint32_t mtime,SidetnfsAtariDirEntry *e){
 (void)mtime;if(strlen(name)>12 || !strcmp(name,".") || !strcmp(name,".."))return false;
 memset(e,0,sizeof(*e));strcpy(e->name,name);e->attr=(flags&1)?16:0;e->size=size;return true;
}
#ifdef R13E
#include "tnfs_fs.h"
#elif defined(R13B)
#include "r13b_fs.h"
#elif !defined(R13A)
#include "r13_fs.h"
#else
#include "r13a_fs.h"
#endif
static struct {uint8_t data[80000];unsigned size,pos,round;bool exists,opened;} server[9];
#ifdef R13C
static struct {bool valid;struct pbuf request,response;uint64_t tick;} cache[9];
static unsigned drop_once,drop_twice,drop_before,cache_hits,send_error,mount_drop,drop_new,server_min_ms=1000;
#endif
static unsigned directory_target;
static unsigned forced_status;
static bool test_dir_exists;
static unsigned sends,writes,closes,drop_cmd,short_cmd,wrong_peer_cmd,read_cap,delay_cmd,inject_stray;
static err_t udp_sendto(struct udp_pcb *pcb,struct pbuf *p,const ip_addr_t *a,unsigned port){
 assert(locks && port==16384);unsigned slot=(unsigned)((struct r13_net_slot *)pcb->arg-r13_net);assert(slot<9);sends++;
 const uint8_t *q=p->bytes;unsigned n=p->tot_len,cmd=q[3];assert(n>=4);
 #ifdef R13C
 if(send_error==cmd && cmd){return ERR_MEM;}
 if(drop_before==cmd && cmd){drop_before=0;return ERR_OK;}
 if(mount_drop && !cmd)return ERR_OK;
#endif
 struct pbuf response={.tot_len=5};memcpy(response.bytes,q,4);uint8_t *b=response.bytes;
 if(cmd!=0)assert(r13_le16(q)==0x100+slot);
 #ifdef R13C
 if(cmd && cache[slot].valid && cache[slot].request.bytes[2]==q[2]){
   assert(n==cache[slot].request.tot_len && !memcmp(q,cache[slot].request.bytes,n));
   assert(tick-cache[slot].tick>=server_min_ms*1000u);
   cache[slot].tick=tick;cache_hits++;response=cache[slot].response;goto deliver;
 }
#endif
 if(cmd==0){cache[slot].valid=false;assert(n==10 && q[4]==1 && q[5]==2 && !memcmp(q+6,"/\0\0\0",4));r13_putle16(b,0x100+slot);response.tot_len=9;
#ifdef R13C
 r13_putle16(b+7,server_min_ms);
#endif
 }
 else if(forced_status){b[4]=forced_status;}
 else if(cmd==0x29){assert(n>=10 && q[n-1]==0 && q[8]=='/');unsigned flags=r13_le16(q+4);
   if(flags&0x100)server[slot].exists=true;
   if(!server[slot].exists)b[4]=2;
   else {if(flags&0x200)server[slot].size=0;server[slot].pos=0;server[slot].opened=true;b[5]=7;response.tot_len=6;}}
 else if(cmd==0x25){assert(n==10 && q[4]==7 && q[5]==0 && server[slot].opened);server[slot].pos=r13_le32(q+6);response.tot_len=9;r13_putle32(b+5,server[slot].pos);}
 else if(cmd==0x21){assert(n==7 && q[4]==7 && server[slot].opened);unsigned count=r13_le16(q+5),pos=server[slot].pos;
   if(pos>=server[slot].size)b[4]=0x21;
   else {if(read_cap && count>read_cap)count=read_cap;if(count>server[slot].size-pos)count=server[slot].size-pos;r13_putle16(b+5,count);memcpy(b+7,server[slot].data+pos,count);response.tot_len=7+count;server[slot].pos+=count;}}
 else if(cmd==0x22){assert(server[slot].opened && q[4]==7);unsigned count=r13_le16(q+5);assert(n==7+count && server[slot].pos+count<=80000);
   memcpy(server[slot].data+server[slot].pos,q+7,count);server[slot].pos+=count;if(server[slot].pos>server[slot].size)server[slot].size=server[slot].pos;
   writes++;r13_putle16(b+5,count);response.tot_len=7;}
 else if(cmd==0x23){assert(n==5 && q[4]==7);server[slot].opened=false;closes++;}
 else if(cmd==0x24 && test_dir_exists && !strcmp((char*)q+4,"/R13TEST")){response.tot_len=27;r13_putle16(b+5,0040755);}
 else if(cmd==0x24){assert(q[n-1]==0);if(!server[slot].exists || !strcmp((char*)q+4,"/NEW.TXT"))b[4]=2;
   else {response.tot_len=27;r13_putle16(b+5,0100644);r13_putle32(b+11,server[slot].size);}}
 else if(cmd==0x27){assert(n==16 && !strcmp((char*)q+6,"/FILE.TXT") && r13_le16(q+4)==0444);}
 else if(cmd==0x26){assert(n>5 && q[n-1]==0);server[slot].exists=false;}
 else if(cmd==0x28){assert(!strcmp((char*)q+4,"/FILE.TXT"));assert(!strcmp((char*)q+4+strlen((char*)q+4)+1,"/NEW.TXT"));}
 else if(cmd==0x13 || cmd==0x14){assert(!strcmp((char*)q+4,"/R13TEST"));if(cmd==0x13 && test_dir_exists)b[4]=0x0b;else test_dir_exists=cmd==0x13;}
 else if(cmd==0x17){assert(n>=12 && q[8]=='*' && !q[9] && q[n-1]==0);b[5]=3;response.tot_len=6;server[slot].round=0;}
 else if(cmd==0x18){assert(n==6 && q[4]==3 && q[5]==1);if(server[slot].round++>directory_target)b[4]=0x21;
   else {response.tot_len=31;b[5]=1;r13_putle32(b+10,server[slot].size);memcpy(b+22,server[slot].round<=directory_target?"SKIP.TXT":"FILE.TXT",9);}}
 else if(cmd==0x12){assert(n==5 && q[4]==3);}
 else assert(0);
#ifdef R13C
 if(cmd){cache[slot].valid=true;cache[slot].request=*p;cache[slot].response=response;cache[slot].tick=tick;}
 if(cmd && cmd==drop_new)return ERR_OK;
 deliver:
 if(drop_twice==cmd && cmd){drop_twice=0;drop_once=cmd;return ERR_OK;}
 if(drop_once==cmd && cmd){drop_once=0;return ERR_OK;}
#endif
 if(drop_cmd && cmd==drop_cmd)return ERR_OK;
 if(short_cmd && cmd==short_cmd)response.tot_len=4;
 if(wrong_peer_cmd && cmd==wrong_peer_cmd){ip_addr_t wrong={999};pcb->cb(pcb->arg,pcb,&response,&wrong,port);return ERR_OK;}
 if(inject_stray){response.bytes[2]++;pcb->cb(pcb->arg,pcb,&response,a,port);response.bytes[2]--;}
 if(delay_cmd && cmd==delay_cmd){delayed=response;delayed_pcb=pcb;delayed_addr=*a;delayed_due=tick+100000;return ERR_OK;}
 pcb->cb(pcb->arg,pcb,&response,a,port);
 if(inject_stray)pcb->cb(pcb->arg,pcb,&response,a,port); /* duplicate */
 return ERR_OK;
}
static uint8_t req[256],out[512];static unsigned len;
static int call(unsigned slot,unsigned op){memset(out,0xcc,sizeof(out));unsigned n=r13_fs(slot,op,req,len,out);assert(n>=4 && n<=512);return (int32_t)r02_be32(out);}
static void pathreq(unsigned slot,const char *path){r02_put32(req,r13_tokens[slot]);strcpy((char*)req+4,path);len=5+strlen(path);}
static void writereq(unsigned slot,unsigned pos,unsigned count){r02_put32(req,r13_tokens[slot]);r02_put32(req+4,pos);r02_put16(req+8,10);r02_put16(req+10,count);memcpy(req+12,"\\FILE.TXT",10);for(unsigned i=0;i<count;i++)req[22+i]=(uint8_t)(pos+i);len=22+count;}
int main(void){
 len=0;assert(call(0,16)==0);pathreq(0,"\\FILE.TXT");
 unsigned count_before=sends,pcb_before=pcb_count;
 for(unsigned mode=1;mode<=3;mode++){
  dns_mode=mode;uint64_t start=tick;
  assert(call(0,22)==-2 && !r13_net[0].poisoned && !r13_net[0].mounted && !r13_net[0].dns_wait);
  assert(sends==count_before && pcb_count==pcb_before && tick-start<=4001000u);
  ip_addr_t late={999};dns_cb("0",&late,dns_arg);assert(!r13_net[0].dns_ready);
 }
 dns_mode=0;
 puts("R14A pre-mount DNS errors/timeouts send nothing, allocate no socket and remain retryable; late idle callback ignored.");

 for(unsigned slot=0;slot<2;slot++){len=0;assert(call(slot,16)==0);pathreq(slot,"\\FILE.TXT");assert(call(slot,22)==0 && server[slot].exists && !server[slot].opened);}
 r02_put32(req,r13_tokens[0]);r02_put16(req+4,1);memcpy(req+6,"\\FILE.TXT",10);len=16;
 unsigned metadata_sends=sends;drop_cmd=0x27;assert(call(0,27)==-32);r02_put16(req+4,2);assert(call(0,27)==-32 && !r13_net[0].poisoned);
 assert(sends==metadata_sends);drop_cmd=0;
 assert(call(0,28)==-32 && !r13_net[0].poisoned);
 for(unsigned i=0;i<71000;i+=200){unsigned count=71000-i<200?71000-i:200;writereq(0,i,count);assert(call(0,23)==(int)count && !server[0].opened);}
 for(unsigned i=0;i<71000;i+=508){unsigned count=71000-i<508?71000-i:508;r02_put32(req,r13_tokens[0]);r02_put32(req+4,i);r02_put16(req+8,count);memcpy(req+10,"\\FILE.TXT",10);len=20;assert(call(0,20)==(int)count);for(unsigned j=0;j<count;j++)assert(out[4+j]==(uint8_t)(i+j));}

#ifdef R13A
 read_cap=97;delay_cmd=0x21;inject_stray=1;
 r02_put32(req,r13_tokens[0]);r02_put32(req+4,0);r02_put16(req+8,508);memcpy(req+10,"\\FILE.TXT",10);len=20;
 assert(call(0,20)==508);for(unsigned j=0;j<508;j++)assert(out[4+j]==(uint8_t)j);
 assert(r13_net[0].reject_sequence>0);
 read_cap=delay_cmd=inject_stray=0;
 puts("R13A short-read, delayed-reply and stray-sequence tests pass.");
#endif
 pathreq(0,"\\FILE.TXT");assert(call(0,17)==0 && r02_be32(out+12)==71000);
 r02_put32(req,r13_tokens[0]);r02_put16(req+4,0);r02_put16(req+6,16);memcpy(req+8,"\\*.*",5);len=13;assert(call(0,18)==0 && !strcmp((char*)out+34,"FILE.TXT"));len=6;assert(call(0,19)==-49 && !r13_search[0][0].open);
#ifdef R13E
 for(unsigned target=255;target<=511;target+=128){
   directory_target=target;r02_put32(req,r13_tokens[0]);r02_put16(req+4,0);r02_put16(req+6,0);memcpy(req+8,"\\FILE.TXT",10);len=18;
   assert(call(0,18)==0 && !r13_search[0][0].open && !strcmp((char*)out+34,"FILE.TXT"));
   len=6;assert(call(0,19)==-49);
 }
 directory_target=600;r02_put32(req,r13_tokens[0]);r02_put16(req+4,0);r02_put16(req+6,0);memcpy(req+8,"\\FILE.TXT",10);len=18;
 assert(call(0,18)==-36 && !r13_search[0][0].open && !r13_net[0].poisoned);
 directory_target=0;
 pathreq(0,"\\FILE.TXT");assert(call(0,17)==0);
 puts("R13E 256/384/512-entry scans, exact-handle release and nonfatal scan limit pass.");
#endif
 pathreq(0,"\\R13TEST");assert(call(0,24)==0);
#ifdef R13B
 /* Repeat a run with the existing directory, then stat and read the same slot. */
 assert(call(0,24)==-36 && !r13_net[0].poisoned);
 assert(call(0,17)==0 && (r02_be16(out+4)&16));
 /* Exercise actual wire replies, not host errno constants. */
 const int expected[]={0,-36,-33,-36,-36,-64,-37,-36,-39,-36,-36,-36,-34,-36,-64,-35,-35,-36,-36,-64,-36,-36,-32,-36,-36,-36,-36,-65,-65,-36,-36,-36,-65,-49};
 for(unsigned code=1;code<sizeof(expected)/sizeof(expected[0]);code++){
   forced_status=code;assert(call(0,24)==expected[code]);
 }
 forced_status=0;
 pathreq(0,"\\FILE.TXT");assert(call(0,17)==0 && !r13_net[0].poisoned);
 writereq(0,0,20);assert(call(0,23)==20);
 r02_put32(req,r13_tokens[0]);r02_put32(req+4,0);r02_put16(req+8,20);memcpy(req+10,"\\FILE.TXT",10);len=20;
 assert(call(0,20)==20);for(unsigned j=0;j<20;j++)assert(out[4+j]==j);
 puts("R13B existing-directory/stat/read/write and all defined wire status translations pass.");
#endif
 r02_put32(req,r13_tokens[0]);r02_put16(req+4,1);memcpy(req+6,"\\R13TEST",9);len=15;assert(call(0,25)==0);
 r02_put16(req+4,10);memcpy(req+6,"\\FILE.TXT",10);memcpy(req+16,"\\NEW.TXT",9);len=25;assert(call(0,26)==0);
#ifdef R13C
 assert(ip_addr_cmp(&r13_net[0].address,&r13_net[1].address) && r13_net[0].sid!=r13_net[1].sid);
 writereq(1,0,200);assert(call(1,23)==200);
 for(unsigned turn=0;turn<8;turn++){
   unsigned slot=turn&1;r02_put32(req,r13_tokens[slot]);r02_put32(req+4,turn*7);r02_put16(req+8,31);memcpy(req+10,"\\FILE.TXT",10);len=20;
   drop_once=0x21;assert(call(slot,20)==31);
   for(unsigned k=0;k<31;k++)assert(out[4+k]==(uint8_t)(turn*7+k));
 }
 puts("R13C same-peer distinct sessions with interleaved recovered reads pass.");
 unsigned commands[]={0x29,0x25,0x21,0x23};
 for(unsigned j=0;j<4;j++){
   drop_once=commands[j];unsigned hits=cache_hits;
   r02_put32(req,r13_tokens[0]);r02_put32(req+4,251);r02_put16(req+8,251);memcpy(req+10,"\\FILE.TXT",10);len=20;
   assert(call(0,20)==251 && cache_hits==hits+1 && !server[0].opened);
   for(unsigned k=0;k<251;k++)assert(out[4+k]==(uint8_t)(251+k));
 }
 for(unsigned losses=1;losses<=2;losses++){
   if(losses==1)drop_once=0x22;else drop_twice=0x22;
   unsigned w=writes,hits=cache_hits;writereq(0,200,20);
   assert(call(0,23)==20 && writes==w+1 && cache_hits==hits+losses && server[0].pos==220);
 }
 drop_before=0x22;unsigned w=writes;writereq(0,240,20);assert(call(0,23)==20 && writes==w+1 && !r13_net[0].poisoned);
 /* Timer wrap and an advertised longer minimum still obey the total budget. */
 tick=0xfff80000u;r13_net[0].retry_ms=server_min_ms=1500;drop_once=0x24;
 pathreq(0,"\\FILE.TXT");assert(call(0,17)==0 && !r13_net[0].poisoned);
 r13_net[0].retry_ms=server_min_ms=1000;
 puts("R13C dropped OPEN/SEEK/READ/CLOSE replies, lost WRITE ACKs, dropped request, immutable retries and timer wrap pass.");
#endif
 pathreq(0,"\\..\\BAD");unsigned before=sends;assert(call(0,22)==-34 && sends==before);
 writereq(0,0,20);drop_cmd=0x22;before=writes;assert(call(0,23)==-65 && writes==before+1 && r13_net[0].poisoned);

#ifdef R13A
 assert(strstr(failure_log,"cmd=22") && strstr(failure_log,"reason=reply-timeout"));
#endif
 before=sends;assert(call(0,23)==-65 && sends==before); /* Never resend uncertain write. */
 drop_cmd=0;writereq(1,0,20);assert(call(1,23)==20 && !r13_net[1].poisoned); /* isolated */
 short_cmd=0x23;writereq(1,20,20);assert(call(1,23)==-65 && r13_net[1].poisoned); /* close acknowledgement required */
 len=0;assert(call(2,16)==0);short_cmd=0;online=0;pathreq(2,"\\FILE.TXT");before=sends;assert(call(2,22)==-2 && !r13_net[2].poisoned && sends==before);
 online=1;wrong_peer_cmd=0x29;assert(call(2,22)==-65 && r13_net[2].poisoned);

#ifdef R13A
 /* Document the existing wire ambiguity, not a claimed fix: an old
  * same-session reply can match again after 256 sequence increments. */
 struct r13_net_slot *ns=&r13_net[1];uint8_t ancient_seq=ns->seq;
 for(unsigned j=0;j<256;j++)ns->seq++;
 assert(ns->seq==ancient_seq);ns->cmd=0x21;ns->waiting=true;ns->received=0;
 struct pbuf ancient={.tot_len=8};r13_putle16(ancient.bytes,ns->sid);
 ancient.bytes[2]=ancient_seq;ancient.bytes[3]=0x21;ancient.bytes[5]=1;ancient.bytes[7]=0x5a;
 r13_udp(ns,ns->pcb,&ancient,&ns->address,ns->port);assert(ns->received==8);
 ns->waiting=false;
 puts("Known limitation reproduced: same-session reply identity repeats after 256 sequence values; not claimed fixed.");
#endif
#ifdef R13C
 wrong_peer_cmd=0;
 for(unsigned slot=3;slot<6;slot++){len=0;assert(call(slot,16)==0);pathreq(slot,"\\FILE.TXT");assert(call(slot,22)==0);}
 unsigned count=sends;send_error=0x24;pathreq(3,"\\FILE.TXT");assert(call(3,17)==-2 && sends==count+1);send_error=0;
 count=sends;r13_net[4].retry_ms=5000;drop_cmd=0x24;pathreq(4,"\\FILE.TXT");uint64_t start=tick;
 assert(call(4,17)==-2 && sends==count+1 && tick-start<=4001000u);drop_cmd=0;
 count=sends;len=0;assert(call(6,16)==0);mount_drop=1;pathreq(6,"\\FILE.TXT");assert(call(6,22)==-2 && sends==count+1);mount_drop=0;
 pathreq(5,"\\FILE.TXT");assert(call(5,17)==0); /* Other slot survives. */
 writereq(5,0,200);assert(call(5,23)==200);
 r02_put32(req,r13_tokens[5]);r02_put32(req+4,0);r02_put16(req+8,200);memcpy(req+10,"\\FILE.TXT",10);len=20;
 read_cap=31;drop_new=0x21;start=tick;
 assert(call(5,20)==-2 && tick-start<=4001000u && r13_net[5].recovering);
 read_cap=drop_new=0;
 puts("R13C repeated partial-read recovery cannot reset the whole-operation deadline.");
 puts("R13C permanent loss, send error, long server minimum, MOUNT no-retry and session isolation pass.");
#endif
#ifdef R13D
 assert(radio_diags>0);
 /* A recovered radio never resets an uncertain TNFS slot or resends its write. */
 before=sends;online=0;writereq(0,0,20);assert(call(0,23)==-65 && sends==before);
 online=1;assert(call(0,23)==-65 && sends==before);
 puts("R13D radio recovery leaves interrupted TNFS writes stopped.");
 /* Even a later read must never clear a write-uncertain slot. */
 pathreq(0,"\\FILE.TXT");assert(call(0,17)==-65 && sends==before);
 /* Mount failure did not issue the requested mutation; later mount is safe. */
 unsigned old_writes=writes;pathreq(6,"\\FILE.TXT");
 tick+=1000000;assert(call(6,22)==0 && writes==old_writes);
 /* Read-only session loss: no automatic replay, bounded cooldown, fresh PCB. */
 struct udp_pcb *retired=r13_net[5].pcb;
 pathreq(5,"\\FILE.TXT");tick+=1000000;assert(call(5,17)==0);
 assert(r13_net[5].pcb!=retired && !r13_net[5].poisoned);
 for(unsigned cycle=0;cycle<7;cycle++){
   retired=r13_net[5].pcb;drop_cmd=0x24;pathreq(5,"\\FILE.TXT");
   assert(call(5,17)==-2 && r13_net[5].recovering && !r13_net[5].pcb);
   before=sends;drop_cmd=0;assert(call(5,17)==-2 && sends==before);
   const uint64_t waits[]={1000000ULL,2148483649ULL,4295967296ULL,
       7356930000ULL,86400000000ULL,300ULL*86400000000ULL,1000000ULL};
   if(cycle==0){tick+=999999;assert(call(5,17)==-2 && sends==before);tick++;}
   else tick+=waits[cycle];
   assert(call(5,17)==0 && r13_net[5].pcb!=retired && !r13_net[5].recovering);
   /* The old search must fail, not read a new session's handle or claim EOF. */
   r02_put32(req,r13_tokens[5]);r02_put16(req+4,0);len=6;before=sends;
   assert(call(5,19)==-2 && sends==before);
   struct r13_net_slot *s=&r13_net[5];struct pbuf late={.tot_len=5};
   r13_putle16(late.bytes,s->sid);late.bytes[2]=s->seq;late.bytes[3]=s->cmd;
   s->received=0;s->waiting=true;r13_udp(s,retired,&late,&s->address,s->port);
   assert(!s->received);s->waiting=false;
 }
 puts("F19 recovery: fresh mount after failure, read faults recover on later access, cooldown, retired replies/cursors rejected, uncertain writes stay blocked across reads.");

#endif
 /* F35 diagnostics: account real successful RPCs and preserve abandoned handles. */
 for(unsigned i=0;i<R13_SLOTS;i++){
   struct r35_resource *a=&r35_resources[i];
   assert(a->tx_alloc==a->tx_free);
   assert(a->pcb_alloc-a->pcb_free==(r13_net[i].pcb?1u:0u));
   assert(a->opens==a->closes+a->files+a->abandoned_files);
   assert(a->dir_opens==a->dir_closes+a->dirs+a->abandoned_dirs);
 }
 assert(r35_resources[5].abandoned_files>0);
 r13_current=5;struct r35_resource *audit=&r35_resources[5];
 unsigned allocations=audit->tx_alloc,failures=audit->pbuf_fail;
 fail_pbuf=true;assert(r13_send(&r13_net[5],4)==ERR_MEM);fail_pbuf=false;
 assert(audit->pbuf_fail==failures+1 && audit->tx_alloc==allocations && audit->tx_alloc==audit->tx_free);
 fail_pcb=true;assert(r13_begin(8)==-65);fail_pcb=false;
 assert(r35_resources[8].pcb_fail==1 && r35_resources[8].pcb_alloc==0);
 r35_report("test");
 puts("F35 resource audit: balanced TX buffers/PCBs, acknowledged handle accounting, retirement persistence and allocation failure injection pass.");
 puts("R13 wire/FS: 71KB offset writes/reads, stat, listings/EOF/close, mkdir/rmdir/rename, path rejection, unresolved-write fail-closed, close failure, slot isolation, offline readiness and peer filtering pass.");
}
