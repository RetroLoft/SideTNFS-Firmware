#!/usr/bin/env python3
"""Compile actual NAT module against bounded lwIP mocks; optional SANITIZE=undefined."""
import os
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
MOCK = r'''
#ifndef SIDEETH21_HOST_H
#define SIDEETH21_HOST_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#define LWIP_RAW 0
#define PICO_CYW43_ARCH_POLL 1
#define MEMP_MEM_MALLOC 0
#define MEMP_TCP_PCB 0
typedef int err_t;
#define ERR_OK 0
#define ERR_USE -8
#define ERR_IF -12
#define ERR_MEM -1
#define IP_ADDR_ANY NULL
#define PBUF_LINK 1
#define PBUF_RAM 0
typedef struct {uint32_t addr;} ip4_addr_t;
struct pbuf {void *payload;uint16_t len,tot_len;struct pbuf *next;};
struct tcp_pcb {uint16_t port;};
struct udp_pcb {uint16_t port;};
struct netif {int up,link;ip4_addr_t ip;err_t (*output)(struct netif *,struct pbuf *,const ip4_addr_t *);};
static uint32_t clock_ms;
static unsigned frees,allocations,reservations;
static uint8_t tcp_ports[65536],udp_ports[65536];
static bool fail_alloc;
static bool fail_tcp_pool;
static unsigned tcp_new_calls,probe_allocations,probe_frees;
static uint32_t lwip_htonl(uint32_t v){return ((v&255)<<24)|((v&65280)<<8)|((v>>8)&65280)|(v>>24);}
#define lwip_ntohl lwip_htonl
#define netif_is_up(n) ((n)->up)
#define netif_is_link_up(n) ((n)->link)
#define netif_ip4_addr(n) (&(n)->ip)
#define ip4_addr_isany_val(a) (!(a).addr)
#define ip4_addr_get_u32(a) ((a)->addr)
#define ip4_addr_set_u32(a,v) ((a)->addr=(v))
static uint32_t sys_now(void){return clock_ms;}
static void *memp_malloc(int pool){assert(pool==MEMP_TCP_PCB);if(fail_tcp_pool || fail_alloc)return NULL;probe_allocations++;return calloc(1,sizeof(struct tcp_pcb));}
static void memp_free(int pool,void *p){assert(pool==MEMP_TCP_PCB && p);probe_frees++;free(p);}
static struct tcp_pcb *tcp_new(void){tcp_new_calls++;return fail_alloc?NULL:calloc(1,sizeof(struct tcp_pcb));}
static struct udp_pcb *udp_new(void){return fail_alloc?NULL:calloc(1,sizeof(struct udp_pcb));}
static err_t tcp_bind(struct tcp_pcb *p,const void *ip,uint16_t port){(void)ip;if(tcp_ports[port])return ERR_USE;p->port=port;tcp_ports[port]=1;reservations++;return ERR_OK;}
static err_t udp_bind(struct udp_pcb *p,const void *ip,uint16_t port){(void)ip;if(udp_ports[port])return ERR_USE;p->port=port;udp_ports[port]=1;reservations++;return ERR_OK;}
static void tcp_abort(struct tcp_pcb *p){if(p->port){tcp_ports[p->port]=0;reservations--;}free(p);}
static void udp_remove(struct udp_pcb *p){if(p->port){udp_ports[p->port]=0;reservations--;}free(p);}
static struct pbuf *pbuf_alloc(int layer,uint16_t n,int type){
    (void)layer;(void)type;if(fail_alloc)return NULL;
    struct pbuf *p=calloc(1,sizeof(*p));assert(p);p->payload=calloc(1,n);assert(p->payload);
    p->len=p->tot_len=n;allocations++;return p;
}
static void pbuf_free(struct pbuf *p){while(p){struct pbuf *next=p->next;free(p->payload);free(p);frees++;p=next;}}
static uint16_t pbuf_copy_partial(const struct pbuf *p,void *out,uint16_t n,uint16_t offset){
    unsigned done=0;while(p && offset>=p->len){offset-=p->len;p=p->next;}
    while(p && done<n){unsigned size=p->len-offset;if(size>n-done)size=n-done;
        memcpy((uint8_t *)out+done,(uint8_t *)p->payload+offset,size);done+=size;p=p->next;offset=0;}
    return (uint16_t)done;
}
static err_t pbuf_take(struct pbuf *p,const void *data,uint16_t n){assert(n<=p->len);memcpy(p->payload,data,n);return ERR_OK;}
#endif
'''
TEST = r'''
#include <assert.h>
#include <stdio.h>
#define S21_HOST 1
#include "romemul/native/sideeth21_nat.c"
static uint8_t sent[576],received[590];
static unsigned sent_len,received_len,outputs,deliveries;
static bool blocked;
static err_t output(struct netif *n,struct pbuf *p,const ip4_addr_t *dst){
    (void)n;sent_len=p->tot_len;assert(pbuf_copy_partial(p,sent,sent_len,0)==sent_len);
    assert(lwip_ntohl(dst->addr)==be32(sent+16));outputs++;return ERR_OK;
}
int s21_nat_deliver(const uint8_t *f,uint16_t n){if(blocked)return 1;memcpy(received,f,n);received_len=n;deliveries++;return 0;}
static struct netif sta={1,1,{0},output};
static const uint8_t mac[6]={2,0x53,0x49,0x44,0x45,1};
static void reset(void){s21_nat_init();sta.up=sta.link=1;sta.ip.addr=lwip_htonl(0xc0a80128);s21_nat_set_link(&sta,true);sent_len=received_len=outputs=deliveries=0;blocked=fail_alloc=false;clock_ms=0;}
/* Independent checksum writer/validator used to build input, not NAT helpers. */
static unsigned rawsum(const uint8_t *p,unsigned n){unsigned s=0;for(unsigned i=0;i<n;i++)s+=(unsigned)p[i]<<((i&1)?0:8);while(s>65535)s=(s&65535)+(s>>16);return s;}
static void checks(uint8_t *ip){
    unsigned h=(ip[0]&15)*4,n=be16(ip+2)-h,proto=ip[9],offset=proto==6?16:proto==17?6:2;
    uint8_t *p=ip+h;put16(p+offset,0);
    unsigned sum=rawsum(p,n);if(proto!=1)sum+=rawsum(ip+12,8)+proto+n;
    while(sum>65535)sum=(sum&65535)+(sum>>16);unsigned c=(~sum)&65535;if(!c && proto==17)c=65535;put16(p+offset,c);
    put16(ip+10,0);put16(ip+10,(~rawsum(ip,h))&65535);
}
static unsigned make(uint8_t *f,unsigned proto,unsigned source,unsigned remote){
    memset(f,0,590);memcpy(f,mac,6);memcpy(f+6,mac,6);put16(f+12,0x0800);
    uint8_t *ip=f+14;ip[0]=0x45;ip[8]=64;ip[9]=proto;put32(ip+12,0xc0a8fe02);put32(ip+16,0x08080808);
    unsigned n=proto==6?24:proto==17?13:11;put16(ip+2,20+n);
    uint8_t *l4=ip+20;
    if(proto==1){l4[0]=8;put16(l4+4,source);l4[8]=1;l4[9]=2;l4[10]=3;}
    else {put16(l4,source);put16(l4+2,remote);
        if(proto==6){l4[12]=0x60;l4[13]=2;l4[20]=2;l4[21]=4;put16(l4+22,1460);}
        else {put16(l4+4,n);memcpy(l4+8,"DNS!?",5);}}
    checks(ip);return 14+20+n;
}
static struct pbuf *buffer(const uint8_t *ip,unsigned n,unsigned split){
    struct pbuf *p=pbuf_alloc(PBUF_LINK,(uint16_t)(split?split:n),PBUF_RAM);assert(p);pbuf_take(p,ip,p->len);
    if(split){p->next=pbuf_alloc(PBUF_LINK,n-split,PBUF_RAM);assert(p->next);pbuf_take(p->next,ip+split,n-split);p->tot_len=n;}return p;
}
static unsigned response(uint8_t *ip){
    memcpy(ip,sent,sent_len);put32(ip+16,0xc0a80128);put32(ip+12,0x08080808);
    unsigned h=(ip[0]&15)*4;if(ip[9]==1)ip[h]=0;
    else {uint16_t a=be16(ip+h),b=be16(ip+h+2);put16(ip+h,b);put16(ip+h+2,a);if(ip[9]==6)ip[h+13]=0x12;}
    checks(ip);return sent_len;
}
static void verify_packet(const uint8_t *ip){
    unsigned h=(ip[0]&15)*4,n=be16(ip+2)-h;assert(rawsum(ip,h)==65535);
    unsigned sum=rawsum(ip+h,n);if(ip[9]!=1)sum+=rawsum(ip+12,8)+ip[9]+n;
    while(sum>65535)sum=(sum&65535)+(sum>>16);assert(sum==65535);
}
int main(void){
    uint8_t f[590],r[600];unsigned n,k;struct pbuf *p;reset();
    /* TCP: reserved explicit local ports are skipped, MSS/checksums repaired. */
    tcp_ports[40000]=1;n=make(f,6,1234,80);assert(!s21_nat_tx(f,n));assert(outputs==0);s21_nat_poll();
    assert(outputs==1 && be32(sent+12)==0xc0a80128 && be16(sent+20)==40001 && sent[8]==63);
    assert(be16(sent+42)==536);verify_packet(sent);assert(tcp_ports[40001]);tcp_ports[40000]=0;
    k=response(r);p=buffer(r,k,7);assert(s21_nat_ip4_input(p,&sta)==1);assert(deliveries==0);s21_nat_poll();
    assert(deliveries==1 && received_len==k+14 && !memcmp(received,mac,6));assert(be32(received+30)==0xc0a8fe02);
    assert(be16(received+36)==1234);verify_packet(received+14);
    /* Strict remote: reserved port from the wrong remote is eaten, not delivered. */
    put32(r+12,0x08080404);checks(r);unsigned old=deliveries;p=buffer(r,k,0);assert(s21_nat_ip4_input(p,&sta)==1);s21_nat_poll();assert(deliveries==old);
    /* Unreserved local TNFS/NTP/DNS packets and other interface remain lwIP-owned. */
    put16(r+22,49152);checks(r);p=buffer(r,k,0);old=frees;assert(!s21_nat_ip4_input(p,&sta) && frees==old);pbuf_free(p);
    struct netif other=sta;p=buffer(r,k,0);assert(!s21_nat_ip4_input(p,&other));pbuf_free(p);
    reset();assert(!reservations);n=make(f,17,2345,53);assert(!s21_nat_tx(f,n));s21_nat_poll();verify_packet(sent);k=response(r);
    p=buffer(r,k,23);assert(s21_nat_ip4_input(p,&sta));blocked=true;s21_nat_poll();assert(!deliveries && s21_nat_get_stats()->rx_pending==1);
    blocked=false;s21_nat_poll();assert(deliveries==1 && be16(received+36)==2345);verify_packet(received+14);
    /* UDP zero-checksum input accepted, generated packets carry valid checksum. */
    put16(f+14+20+6,0);assert(!s21_nat_tx(f,n));s21_nat_poll();verify_packet(sent);
    reset();n=make(f,1,77,0);assert(!s21_nat_tx(f,n));s21_nat_poll();assert(be16(sent+24)>=40000 && be16(sent+24)<=40959);k=response(r);
    p=buffer(r,k,1);assert(s21_nat_ip4_input(p,&sta));s21_nat_poll();assert(received[34]==0 && be16(received+38)==77);verify_packet(received+14);
    /* Malformed/truncated headers, IP checksum, L4 checksum, fragments and oversized UDP. */
    reset();n=make(f,6,1111,80);for(unsigned i=0;i<n;i++)assert(s21_nat_tx(f,i)==6);
    f[14+10]^=1;assert(s21_nat_tx(f,n)==6);f[14+10]^=1;
    f[14+20+16]^=1;assert(s21_nat_tx(f,n)==6);f[14+20+16]^=1;
    f[14+20+12]=0x10;checks(f+14);assert(s21_nat_tx(f,n)==6);
    n=make(f,6,1111,80);f[14+20+21]=255;checks(f+14);assert(s21_nat_tx(f,n)==6);
    n=make(f,17,1111,53);put16(f+20,0x2000);checks(f+14);assert(s21_nat_tx(f,n)==6);
    put16(f+20,1);checks(f+14);assert(s21_nat_tx(f,n)==6);
    n=make(f,17,1111,53);put16(f+14+20+4,7);checks(f+14);assert(s21_nat_tx(f,n)==6);
    n=make(f,17,1111,53);put32(f+14+12,0xc0a8fe03);checks(f+14);assert(s21_nat_tx(f,n)==6);
    n=make(f,17,1111,53);assert(!s21_nat_tx(f,n));s21_nat_poll();k=response(r);
    put16(r+2,577);p=buffer(r,576,19);assert(s21_nat_ip4_input(p,&sta));assert(!s21_nat_get_stats()->rx_pending);
    k=response(r);put16(r+6,0x2000);checks(r);p=buffer(r,k,21);assert(s21_nat_ip4_input(p,&sta));assert(!s21_nat_get_stats()->rx_pending);
    k=response(r);r[26]^=1;p=buffer(r,k,0);assert(s21_nat_ip4_input(p,&sta));assert(!s21_nat_get_stats()->rx_pending);
    /* Queue bounds, no forced eviction, resource failure and reconnection cleanup. */
    reset();n=make(f,17,9000,53);for(unsigned i=0;i<4;i++)assert(!s21_nat_tx(f,n));assert(s21_nat_tx(f,n)==5);
    for(unsigned i=0;i<4;i++)s21_nat_poll();assert(outputs==4);k=response(r);blocked=true;
    for(unsigned i=0;i<5;i++){p=buffer(r,k,0);assert(s21_nat_ip4_input(p,&sta));}assert(s21_nat_get_stats()->rx_pending==4);
    sta.ip.addr=lwip_htonl(0xc0a80129);s21_nat_set_link(&sta,true);assert(!reservations && !s21_nat_get_stats()->rx_pending && !s21_nat_get_stats()->mappings);
    reset();for(unsigned i=0;i<8;i++){n=make(f,17,1000+i,53);assert(!s21_nat_tx(f,n));s21_nat_poll();}assert(reservations==8);
    n=make(f,17,2000,53);assert(s21_nat_tx(f,n)==5);clock_ms=60001;s21_nat_poll();assert(!reservations);
    fail_alloc=true;assert(s21_nat_tx(f,n)==5);fail_alloc=false;
    assert(!s21_nat_tx(f,n));s21_nat_set_link(NULL,false);assert(!reservations && s21_nat_tx(f,n)==7);
    /* CAB HTTP/1.0: more than eight closed TCP connections must keep working.
     * FIN grace retains final ACK tuple until another connection needs room. */
    reset();for(unsigned i=0;i<24;i++){
        n=make(f,6,2000+i,80);assert(!s21_nat_tx(f,n));s21_nat_poll();
        k=response(r);p=buffer(r,k,0);assert(s21_nat_ip4_input(p,&sta));s21_nat_poll();
        f[14+20+13]=0x11;checks(f+14);assert(!s21_nat_tx(f,n));s21_nat_poll();
        k=response(r);r[20+13]=0x11;checks(r);p=buffer(r,k,0);assert(s21_nat_ip4_input(p,&sta));s21_nat_poll();
        f[14+20+13]=0x10;checks(f+14);assert(!s21_nat_tx(f,n));s21_nat_poll();
        assert(reservations<=8);clock_ms++;
    }
    n=make(f,6,2000,80);f[14+20+13]=0x10;checks(f+14);assert(s21_nat_tx(f,n)==7);
    clock_ms+=10001;s21_nat_poll();assert(!reservations);
    n=make(f,6,3456,80);assert(!s21_nat_tx(f,n));s21_nat_poll();k=response(r);
    r[20+13]=4;checks(r);p=buffer(r,k,0);assert(s21_nat_ip4_input(p,&sta));s21_nat_poll();
    clock_ms+=10001;s21_nat_poll();assert(!reservations);
    /* Full native TCP pool must reject NAT without invoking tcp_new's
     * connection-eviction fallback. Existing bindings stay untouched. */
    unsigned new_calls=tcp_new_calls;tcp_ports[45000]=1;fail_tcp_pool=true;
    n=make(f,6,6789,80);assert(s21_nat_tx(f,n)==5);
    assert(tcp_new_calls==new_calls && tcp_ports[45000] && !reservations);
    fail_tcp_pool=false;tcp_ports[45000]=0;
    reset();s21_nat_set_link(NULL,false);assert(allocations==frees);
    assert(probe_allocations==probe_frees && probe_allocations==tcp_new_calls);
    printf("F21 NAT passed: TCP/UDP/ICMP, reservations, chained pbufs, strict matching, checksums, malformed input, queues, expiry and reconnect; static queues=%zu maps=%zu bytes\n",sizeof(txq)+sizeof(rxq),sizeof(maps));
}
'''
with tempfile.TemporaryDirectory(prefix='sideeth21-') as temp:
    directory=Path(temp)
    (directory/'sideeth21_host.h').write_text(MOCK)
    source=directory/'test.c';source.write_text(TEST)
    binary=directory/'test'
    sanitize=['-fsanitize='+os.environ['SANITIZE']] if os.environ.get('SANITIZE') else []
    subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror',*sanitize,
                    '-I',str(directory),'-I',str(ROOT),str(source),'-o',str(binary)],check=True,timeout=30)
    subprocess.run([str(binary)],check=True,timeout=20)
