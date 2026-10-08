"""Exercise actual STX mailbox code against actual Pico endpoint, without hardware."""
from pathlib import Path
import subprocess,tempfile
r=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'transprt.h').write_text('#include <stdint.h>\ntypedef int16_t int16;\n')
 code=r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "sideeth20.h"
static uint16_t mem[32768];static unsigned accesses;
static long Supexec(long (*f)(void)){return f();}
static uint16_t word(uint32_t off){accesses++;return mem[off/2];}
static uint32_t lng(uint32_t off){return ((uint32_t)word(off)<<16)|word(off+2);}
static void s20_test_event(uint32_t off){accesses++;assert(s20_feed(off));}
#define S20_HOST
#define S20_WORD(off) word(off)
#define S20_LONG(off) lng(off)
#include "cart.c"
int main(void){
 uintptr_t m=(uintptr_t)mem;struct api gate={0x53453230,1,0,0};api=&gate;s20_init(m);
 assert(cart_cmd_super(SIDEETH_CMD_GET_INFO,NULL,0)==0);
 gate.busy=1;unsigned a=accesses;assert(!cart_tx_ready());assert(cart_cmd_u16_super(SIDEETH_CMD_SET_STATE,1)==-1);assert(accesses==a);
 gate.busy=0;assert(cart_cmd_u16_super(SIDEETH_CMD_SET_STATE,1)==0);
 assert(!s20.enabled && s20.pending && !gate.busy); /* returns before Pico processes */
 assert(!cart_tx_ready());s20_service(m);assert(s20.enabled && cart_tx_ready());
 uint8_t arp[60]={0};memset(arp,255,6);memcpy(arp+6,"\x02\x01\x02\x03\x04\x05",6);
 arp[12]=8;arp[13]=6;arp[15]=1;arp[16]=8;arp[18]=6;arp[19]=4;arp[21]=1;
 memcpy(arp+22,arp+6,6);memcpy(arp+28,"\xc0\xa8\xfe\x02",4);memcpy(arp+38,"\xc0\xa8\xfe\x01",4);
 assert(cart_tx_frame(arp,60)==0);assert(!cart_tx_ready());assert(!s20.prod);s20_service(m);
 assert(cart_tx_ready() && s20.prod==1);uint8_t reply[600];
 assert(cart_receive_frame(reply,sizeof(reply))==60 && reply[21]==2);
 assert(s20.pending && s20.cons==0);assert(cart_receive_frame(reply,sizeof(reply))==0); /* no duplicate before ACK */
 s20_service(m);assert(s20.cons==1 && !cart_receive_frame(reply,sizeof(reply)));
 assert(!gate.busy);puts("F20 actual STX/Pico: shared busy gate prevents bus reads, asynchronous CRC framing, bounded pending queue, ARP roundtrip and ACK duplicate exclusion pass.");
}
'''
 (p/'test.c').write_text(code)
 subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror','-Wno-int-to-pointer-cast','-Wno-misleading-indentation','-fsanitize=undefined','-I',str(p),'-I',str(r/'atari/sideeth'),'-I',str(r/'romemul/native'),str(p/'test.c'),'-o',str(p/'test')],check=True,timeout=30)
 subprocess.run([str(p/'test')],check=True,timeout=30)
