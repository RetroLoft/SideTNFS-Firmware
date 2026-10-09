#!/usr/bin/env python3
"""Exercise the real endpoint; set SANITIZE=address,undefined for sanitizer runs."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
static uint32_t test_enter(void);
static void test_leave(uint32_t saved);
#define S20_CRITICAL_ENTER() test_enter()
#define S20_CRITICAL_LEAVE(saved) test_leave(saved)
#include "romemul/native/sideeth20.h"
static uint16_t memory[32768];
static uintptr_t shared;
static uint32_t sequence;
static unsigned critical_count;
static uint32_t test_enter(void){assert(s20.pending);return 0x20u;}
static void test_leave(uint32_t saved){
    assert(saved==0x20u && !s20.pending);
    assert(memory[S20_ACK/2]==s20_be16(s20.request+3));
    assert(memory[S20_ACK/2+1]==s20_be16(s20.request+5));critical_count++;
}
static void send(unsigned op,const uint8_t *p,unsigned n,int bad){
    uint8_t b[S20_MAX+9];b[0]=op;s20_put16(b+1,n);
    unsigned seq=++sequence;s20_put16(b+3,seq>>16);s20_put16(b+5,seq);
    if(n)memcpy(b+7,p,n);s20_put16(b+7+n,s20_crc(b,7+n)^(bad?1:0));
    assert(s20_feed(S20_START));for(unsigned i=0;i<n+9;i++)assert(s20_feed(S20_DATA+2*b[i]));
    assert(s20_feed(S20_STOP));
}
static unsigned status(void){return memory[(S20_ACK+4)/2];}
static void command(unsigned op,const uint8_t *p,unsigned n){
    send(op,p,n,0);assert(s20.pending);s20_service(shared);assert(!s20.pending);
    assert(memory[S20_ACK/2]==(sequence>>16));assert(memory[S20_ACK/2+1]==(sequence&65535));
}
static void read_frame(uint8_t *p,unsigned n){
    for(unsigned i=0;i<n;i++){
        uint16_t w=memory[(S20_RING+s20.cons*S20_STRIDE+i)/2];p[i]=(i&1)?w:w>>8;
    }
}
static void ip_checksum(uint8_t *f){s20_put16(f+24,0);s20_put16(f+24,s20_checksum(f+14,20));}
int main(void){
    shared=(uintptr_t)memory;for(unsigned i=0;i<32768;i++)memory[i]=0xa55a;
    s20_init(shared);assert(memory[S20_BASE/2-1]==0xa55a);assert(memory[(S20_ACK+6)/2]==0xa55a);
    assert(memory[S20_BASE/2]==0x5345 && memory[S20_BASE/2+1]==0x5448);
    send(1,NULL,0,1);assert(!s20.pending && s20.rejected==1);
    send(1,NULL,0,0);assert(s20.pending);unsigned first=sequence;
    send(2,(uint8_t[]){0,1},2,0);assert(!s20.enabled && s20.busy_drops==1);
    s20_service(shared);assert(memory[S20_ACK/2+1]==first && !s20.enabled);
    assert(s20_feed(S20_START));assert(!s20_feed(0xd000));assert(!s20.collecting);
    command(2,(uint8_t[]){0,2},2);assert(status()==6);
    command(2,(uint8_t[]){0,1},2);assert(status()==0 && s20.enabled);
    uint8_t arp[60]={0},out[590];memset(arp,255,6);memcpy(arp+6,(uint8_t[]){2,3,4,5,6,7},6);
    s20_put16(arp+12,0x0806);s20_put16(arp+14,1);s20_put16(arp+16,0x0800);
    arp[18]=6;arp[19]=4;s20_put16(arp+20,1);memcpy(arp+22,arp+6,6);
    memcpy(arp+28,(uint8_t[]){192,168,254,2},4);memcpy(arp+38,s20_ip,4);
    send(3,arp,60,0);assert(s20.prod==s20.cons); /* IRQ performs no Ethernet work. */
    s20_service(shared);assert(status()==0 && s20.length[s20.cons]==60);
    read_frame(out,60);assert(!memcmp(out,arp+6,6));assert(s20_be16(out+20)==2);
    assert(!memcmp(out+22,s20_mac,6) && !memcmp(out+28,s20_ip,4));
    command(3,arp,60);assert(status()==3 && s20.rx_dropped==1);
    command(4,(uint8_t[]){0,1},2);assert(status()==0 && s20.prod==s20.cons);
    command(3,arp,41);assert(status()==6);arp[41]=9;command(3,arp,60);assert(status()==7);
    uint8_t ping[60]={0};memcpy(ping,s20_mac,6);memcpy(ping+6,arp+6,6);s20_put16(ping+12,0x0800);
    uint8_t *ip=ping+14;ip[0]=0x45;ip[8]=64;ip[9]=1;s20_put16(ip+2,33);
    memcpy(ip+12,arp+28,4);memcpy(ip+16,s20_ip,4);ip[20]=8;ip[24]=17;ip[28]=21;ip[32]=42;
    s20_put16(ip+22,s20_checksum(ip+20,13));ip_checksum(ping);
    command(3,ping,60);assert(status()==0);read_frame(out,60);
    assert(out[34]==0 && !s20_checksum(out+14,20) && !s20_checksum(out+34,13));
    assert(!memcmp(out+26,s20_ip,4) && !memcmp(out+30,ip+12,4) && out[46]==42);
    command(4,(uint8_t[]){0,1},2);assert(status()==0);
    command(3,ping,46);assert(status()==6); /* total length exceeds frame */
    ip[20]^=1;command(3,ping,60);assert(status()==6);ip[20]^=1;
    ip[10]^=1;command(3,ping,60);assert(status()==6);ip[10]^=1;
    s20_put16(ip+6,0x2000);ip_checksum(ping);command(3,ping,60);assert(status()==6);
    s20_put16(ip+6,1);ip_checksum(ping);command(3,ping,60);assert(status()==6);
    s20_put16(ip+6,0);ip[16]=8;ip_checksum(ping);command(3,ping,60);assert(status()==7);
    command(2,(uint8_t[]){0,0},2);command(3,arp,60);assert(status()==1);
    command(2,(uint8_t[]){0,1},2);
    for(unsigned n=0;n<60;n++)command(3,ping,n); /* short input coverage */
    command(99,NULL,0);assert(status()==6);
    s20_feed(S20_START);for(unsigned i=0;i<S20_MAX+10;i++)s20_feed(S20_DATA);
    assert(!s20.collecting && !s20.pending);
    assert(memory[S20_BASE/2-1]==0xa55a && memory[(S20_ACK+6)/2]==0xa55a);
    assert(critical_count>20);
    puts("F20 endpoint: CRC, busy mailbox, IRQ isolation, ARP, ICMP, bounds and guards passed");
}
'''

with tempfile.TemporaryDirectory(prefix="sideeth20-") as temp:
    source = Path(temp) / "test.c"
    binary = Path(temp) / "test"
    source.write_text(SOURCE)
    sanitize = ["-fsanitize=" + os.environ["SANITIZE"]] if os.environ.get("SANITIZE") else []
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                    "-Werror", *sanitize, "-I", str(ROOT),
                    str(source), "-o", str(binary)], check=True, timeout=30)
    subprocess.run([str(binary)], check=True, timeout=15)
