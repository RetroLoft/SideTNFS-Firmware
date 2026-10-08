/* F20 asynchronous SIDEETH mailbox. GPL-3.0, derived from local SideETH.
 * Timer paths never Supexec or wait for a Pico response. Shared SE20 cookie
 * gate covers native GEMDRIVE as well as this driver. */
#include <stdint.h>
#ifndef S20_HOST
#include <osbind.h>
#endif
#include "cart.h"
struct api {uint32_t magic,version;volatile uint16_t busy;uint16_t reserved;};
static struct api *api;
static uint32_t pending;
static uint8_t packet[599];
#ifndef S20_HOST
static uint16_t spl7(void){uint16_t s;__asm__ volatile("move.w %%sr,%0\n\tori.w #0x0700,%%sr":"=d"(s)::"cc","memory");return s;}
static void spl(uint16_t s){__asm__ volatile("move.w %0,%%sr"::"d"(s):"cc","memory");}
#else
static uint16_t spl7(void){return 0;}
static void spl(uint16_t s){(void)s;}
#endif
static int enter(void){
    uint16_t sr=spl7();int ok=0;
    if(!api){const uint32_t *jar=*(const uint32_t * volatile *)0x5a0;
        unsigned i;if(jar)for(i=0;i<128 && jar[2*i];i++)if(jar[2*i]==0x53453230UL){api=(struct api *)jar[2*i+1];break;}}
    if(api && api->magic==0x53453230UL && api->version==1 && !api->busy){api->busy=1;ok=1;}
    spl(sr);return ok;
}
static void leave(void){__asm__ volatile("":::"memory");api->busy=0;}
#ifndef S20_HOST
#define S20_WORD(off) (*(volatile uint16_t *)(ROM3_BASE+(off)))
#define S20_LONG(off) (*(volatile uint32_t *)(ROM3_BASE+(off)))
#endif
static uint16_t rw(uint32_t off){return S20_WORD(off);}
static uint32_t rl(uint32_t off){return S20_LONG(off);}
static int ready(void){return rw(SIDEETH_OFF_PROTO+2)==2 && rl(SIDEETH_OFF_MAGIC)==SIDEETH_MAGIC && (!pending || rl(0xf9b0)==pending);}
#ifndef S20_HOST
static void event(uint32_t off){volatile uint8_t x=*(volatile uint8_t *)(ROM3_BASE+off);unsigned n;(void)x;for(n=0;n<8;n++)__asm__ volatile("nop");}
#else
static void event(uint32_t off){s20_test_event(off);}
#endif
static uint16_t crc(const uint8_t *p,unsigned n){uint16_t c=0xffff;unsigned b;while(n--){c^=(uint16_t)*p++<<8;for(b=0;b<8;b++)c=(c&0x8000)?(c<<1)^0x1021:c<<1;}return c;}
static int send(unsigned op,const uint8_t *p,unsigned n){
    unsigned i;uint16_t c;uint32_t seq;
    if(n>590 || !ready())return -1;
    seq=rl(0xf9b0)+1;if(!seq)seq=1;
    packet[0]=op;packet[1]=n>>8;packet[2]=n;
    packet[3]=seq>>24;packet[4]=seq>>16;packet[5]=seq>>8;packet[6]=seq;
    for(i=0;i<n;i++)packet[7+i]=p[i];c=crc(packet,n+7);packet[n+7]=c>>8;packet[n+8]=c;
    pending=seq;event(0xe000);for(i=0;i<n+9;i++)event(0xe400+2UL*packet[i]);event(0xe002);return 0;
}
int16 cart_cmd_super(uint16_t cmd,const uint16_t *p,uint16_t n){
    int16 result=-1;if(!enter())return -1;
    if(cmd==SIDEETH_CMD_GET_INFO || cmd==SIDEETH_CMD_GET_MAC || cmd==SIDEETH_CMD_POLL)result=(rw(SIDEETH_OFF_PROTO+2)==2 && rl(SIDEETH_OFF_MAGIC)==SIDEETH_MAGIC)?0:-1;
    else if(cmd==SIDEETH_CMD_SET_STATE && n==2)result=send(2,(const uint8_t *)p,n);
    else if(cmd==SIDEETH_CMD_RX_ACK && n==2)result=send(4,(const uint8_t *)p,n);
    leave();return result;
}
static uint16_t cmd_arg,size_arg;static const uint16_t *ptr_arg;static int16 result_arg;
static long command_super(void){result_arg=cart_cmd_super(cmd_arg,ptr_arg,size_arg);return 0;}
int16 cart_cmd(uint16_t cmd,const uint16_t *p,uint16_t n){cmd_arg=cmd;ptr_arg=p;size_arg=n;Supexec(command_super);return result_arg;}
int16 cart_cmd_u16(uint16_t cmd,uint16_t value){uint8_t b[2];b[0]=value>>8;b[1]=value;return cart_cmd(cmd,(const uint16_t *)b,2);}
int16 cart_cmd_u16_super(uint16_t cmd,uint16_t value){uint8_t b[2];b[0]=value>>8;b[1]=value;return cart_cmd_super(cmd,(const uint16_t *)b,2);}
int16 cart_tx_ready(void){int16 ok;if(!enter())return 0;ok=ready();leave();return ok;}
int16 cart_tx_frame(const uint8_t *p,uint16_t n){int16 rc;if(!enter())return -1;rc=send(3,p,n);leave();return rc;}
static void read_bytes(uint32_t off,uint8_t *p,unsigned n){unsigned i;for(i=0;i<n;i+=2){uint16_t v=rw(off+i);p[i]=v>>8;if(i+1<n)p[i+1]=v;}}
int16 cart_receive_frame(uint8_t *p,uint16_t cap){
    unsigned idx,n;uint8_t ack[2]={0,1};int16 result=0;
    if(!enter())return 0;
    if(ready() && rw(SIDEETH_OFF_RX_PENDING)){
        idx=rw(SIDEETH_OFF_RX_CONS)%SIDEETH_RX_SLOTS;n=rw(SIDEETH_OFF_RX_SLOT_LEN+2*idx);
        if(n>=14 && n<=cap && n<=590){read_bytes(SIDEETH_OFF_RX_RING+idx*SIDEETH_SLOT_SIZE,p,n);result=n;}
        if(send(4,ack,2))result=0;
    }
    leave();return result;
}
uint16_t cart_read_u16_super(uint32_t off){uint16_t v=0;if(enter()){v=rw(off);leave();}return v;}
void cart_read_bytes_super(uint32_t off,uint8_t *p,uint16_t n){if(enter()){read_bytes(off,p,n);leave();}}
static uint32_t read_off,read_value;static uint8_t *read_ptr;static uint16_t read_len;
static long read_word_super(void){read_value=cart_read_u16_super(read_off);return 0;}
static long read_long_super(void){read_value=0;if(enter()){read_value=rl(read_off);leave();}return 0;}
static long read_block_super(void){cart_read_bytes_super(read_off,read_ptr,read_len);return 0;}
uint16_t cart_read_u16(uint32_t off){read_off=off;Supexec(read_word_super);return read_value;}
uint32_t cart_read_u32_swapped(uint32_t off){read_off=off;Supexec(read_long_super);return read_value;}
void cart_read_bytes(uint32_t off,uint8_t *p,uint16_t n){read_off=off;read_ptr=p;read_len=n;Supexec(read_block_super);}
