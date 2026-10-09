#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
typedef unsigned short u16_t;typedef int err_t;
#define ERR_OK 0
#define ERR_MEM -1
#define ERR_INPROGRESS -2
#define PBUF_TRANSPORT 0
#define PBUF_RAM 0
#define LOG(component,level,...) ((void)0)
#define LOG_AT(component,level,...) ((void)0)
typedef struct {int a;} ip_addr_t;
#define ip_addr_copy(a,b) ((a)=(b))
#define ip_addr_cmp(x,y) ((x)->a==(y)->a)
typedef struct {int16_t year;int8_t month,day,dotw,hour,min,sec;} datetime_t;
typedef struct {unsigned rtc_enabled;char ntp_server[128],utc_offset[8];} sidetnfs_system_settings_t;
struct udp_pcb {int x;};struct pbuf {unsigned tot_len;uint8_t b[48];};
#define GEMDRVEMUL_RTC_STATUS 0x10
static uint32_t shared_words[16];
#define STATUS shared_words[4]
static bool rtc_accept=true;
static uint32_t now;static unsigned sends,sets;static datetime_t clock_value;
static uint32_t time_us_32(void){return now;}
static uint64_t time_us_64(void){return now;}
static void rtc_init(void){}
static bool rtc_set_datetime(const datetime_t *d){clock_value=*d;sets++;return rtc_accept;}
static bool rtc_get_datetime(datetime_t *d){*d=clock_value;return true;}
static void cyw43_arch_lwip_begin(void){}
static void cyw43_arch_lwip_end(void){}
static int dns_gethostbyname(const char *h,ip_addr_t *ip,void(*cb)(const char*,const ip_addr_t*,void*),void *ctx){(void)h;(void)cb;(void)ctx;ip->a=1;return 0;}
static struct udp_pcb *udp_new(void){static struct udp_pcb p;return &p;}
static void udp_recv(struct udp_pcb *p,void(*cb)(void*,struct udp_pcb*,struct pbuf*,const ip_addr_t*,u16_t),void *arg){(void)p;(void)cb;(void)arg;}
static struct pbuf *pbuf_alloc(int a,unsigned n,int b){(void)a;(void)b;static struct pbuf p;p.tot_len=n;return &p;}
static void pbuf_take(struct pbuf *p,const void *b,unsigned n){memcpy(p->b,b,n);}
static void pbuf_copy_partial(struct pbuf *p,void *b,unsigned n,unsigned o){memcpy(b,p->b+o,n);}
static void pbuf_free(struct pbuf *p){(void)p;}
static int udp_sendto(struct udp_pcb *p,struct pbuf *b,const ip_addr_t *ip,unsigned port){(void)p;(void)ip;assert(port==123 && b->tot_len==48);sends++;return 0;}
static void r02_put16(uint8_t *p,unsigned n){p[0]=n>>8;p[1]=n;}
static void r02_put32(uint8_t *p,uint32_t n){r02_put16(p,n>>16);r02_put16(p+2,n);}
#include "ntp.h"
static void net32(uint8_t *p,uint32_t n){r02_put32(p,n);}
int main(void){
 sidetnfs_system_settings_t c={1,"clock.example","+1"};uint8_t out[8];
 assert(r14_clock(out)==4 && !sets);r14_ntp_tick(&c,false,(uintptr_t)shared_words);assert(!sends);
 r14_ntp_tick(&c,true,(uintptr_t)shared_words);assert(r14_ntp.state==2);r14_ntp_tick(&c,true,(uintptr_t)shared_words);assert(sends==1 && r14_ntp.state==3);
 struct pbuf p={48,{0}};p.b[0]=0x24;p.b[1]=2;memcpy(p.b+24,r14_ntp.nonce,8);net32(p.b+40,3998764800u); /*2026*/
 ip_addr_t wrong={2};r14_ntp_recv(NULL,NULL,&p,&wrong,123);assert(r14_ntp.state==3);
 p.b[24]^=1;r14_ntp_recv(NULL,NULL,&p,&r14_ntp.ip,123);assert(r14_ntp.state==3);p.b[24]^=1;
 p.b[0]|=0xc0;r14_ntp_recv(NULL,NULL,&p,&r14_ntp.ip,123);assert(r14_ntp.state==3);p.b[0]=0x24;
 p.b[1]=0;r14_ntp_recv(NULL,NULL,&p,&r14_ntp.ip,123);assert(r14_ntp.state==3);p.b[1]=2;
 r14_ntp_recv(NULL,NULL,&p,&r14_ntp.ip,123);assert(r14_ntp.state==4 && !sets && STATUS==0);
 r14_ntp_tick(&c,true,(uintptr_t)shared_words);assert(STATUS==UINT32_MAX && sets==1 && r14_ntp.valid && r14_clock(out)==8 && clock_value.hour==1);
 r14_ntp_tick(&c,true,(uintptr_t)shared_words);assert(sets==1 && sends==1);
 uint64_t epoch;net32(p.b+40,100000000u);assert(r14_ntp_reply(p.b,48,r14_ntp.nonce,&epoch) && epoch>2085978496ULL);
 STATUS=0;memset(&r14_ntp,0,sizeof(r14_ntp));now=0xffff0000u;
 r14_ntp_tick(&c,true,(uintptr_t)shared_words);r14_ntp_tick(&c,true,(uintptr_t)shared_words);now+=5000001;r14_ntp_tick(&c,true,(uintptr_t)shared_words);assert(r14_ntp.state==6 && !r14_ntp.valid);
 for(unsigned i=0;i<2;i++){now+=60000001;r14_ntp_tick(&c,true,(uintptr_t)shared_words);r14_ntp_tick(&c,true,(uintptr_t)shared_words);now+=5000001;r14_ntp_tick(&c,true,(uintptr_t)shared_words);}
 assert(r14_ntp.attempts==3);unsigned before=sends;now+=60000001;r14_ntp_tick(&c,true,(uintptr_t)shared_words);assert(sends==before && !r14_ntp.valid);
 assert(STATUS==0); /* timeouts never publish READY */
 memset(&r14_ntp,0,sizeof(r14_ntp));r14_ntp.state=4;r14_ntp.epoch=1790000000;
 rtc_accept=false;r14_ntp_tick(&c,true,(uintptr_t)shared_words);assert(!r14_ntp.valid && STATUS==0);
 memset(&r14_ntp,0,sizeof(r14_ntp));c.rtc_enabled=0;unsigned sent=sends;
 r14_ntp_tick(&c,true,(uintptr_t)shared_words);assert(STATUS==0 && sends==sent);
 puts("F29 utility sync status: successful RTC update publishes READY; disabled, pending, timeout and RTC failure do not.");
 puts("R14 NTP: peer/token/mode/stratum validation, deferred RTC update, timezone, era, timeout rollover and bounded attempts pass.");
}
