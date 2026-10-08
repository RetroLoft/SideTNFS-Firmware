"""Check the real mailbox endpoint's local/routed packet boundary."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
source=r'''
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
static unsigned routes;static uint16_t route_status;
static uint16_t routed(const uint8_t *f,uint16_t n){assert(f[12]==8 && n==42);routes++;return route_status;}
#define S20_ROUTE_TX(f,n) routed(f,n)
#include "sideeth20.h"
static uint16_t mem[32768];
static void checksum(uint8_t *f){s20_put16(f+24,0);s20_put16(f+24,s20_checksum(f+14,20));}
int main(void){
 uintptr_t m=(uintptr_t)mem;uint8_t frame[60]={0};
 s20_init(m);s20_feed(0);s20_service(m);s20.enabled=true;
 frame[12]=8;frame[14]=0x45;frame[16]=0;frame[17]=28;frame[22]=64;frame[23]=1;
 memcpy(frame+26,"\xc0\xa8\xfe\x02",4);memcpy(frame+30,s20_ip,4);
 frame[34]=8;s20_put16(frame+36,s20_checksum(frame+34,8));checksum(frame);
 assert(!s20_tx(m,frame,42) && !routes && s20.prod!=s20.cons);
 s20.cons=s20.prod;memcpy(frame+30,"\x01\x01\x01\x01",4);checksum(frame);
 assert(!s20_tx(m,frame,42) && routes==1 && s20.prod==s20.cons);
 route_status=3;assert(s20_tx(m,frame,42)==3 && routes==2);
 frame[24]^=1;assert(s20_tx(m,frame,42)==6 && routes==2);checksum(frame);
 frame[20]=0x20;checksum(frame);assert(s20_tx(m,frame,42)==6 && routes==2);
 frame[20]=0;checksum(frame);assert(s20_tx(m,frame,33)==6 && routes==2);
 s20.enabled=false;assert(s20_tx(m,frame,42)==1 && routes==2);
 s20.wifi_up=true;s20.host_ip=0xc0a80102;s20_publish(m,7);
 assert(mem[(S20_BASE+12)/2]==0 && mem[(S20_BASE+14)/2]==4);
 assert(mem[(S20_BASE+32)/2]==0xc0a8 && mem[(S20_BASE+34)/2]==0x0102);
 puts("F21 endpoint: local echo stays local, routed TX bounded, malformed/fragmented/disabled TX excluded, Wi-Fi metadata verified.");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(source)
 subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=undefined','-I',str(root/'romemul/native'),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
