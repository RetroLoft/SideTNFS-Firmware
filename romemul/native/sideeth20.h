/* F20/F21 Ethernet endpoint. Main loop owns all packet processing.
 * IRQ collector only copies complete CRC-checked requests to a single mailbox. */
#ifndef SIDETNFS_SIDEETH20_H
#define SIDETNFS_SIDEETH20_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#ifndef S20_CRITICAL_ENTER
#define S20_CRITICAL_ENTER() 0u
#define S20_CRITICAL_LEAVE(saved) ((void)(saved))
#endif
#ifndef S20_ROUTE_TX
#define S20_ROUTE_TX(frame,len) 7u
#endif
#define S20_BASE 0xf240u
#define S20_RING 0xf2a0u
#define S20_SCRATCH 0xf750u
#define S20_END 0xf9a8u
#define S20_ACK 0xf9b0u
#define S20_MAX 590u
#define S20_STRIDE 600u
#define S20_START 0xe000u
#define S20_STOP 0xe002u
#define S20_DATA 0xe400u
_Static_assert(S20_BASE+64+32==S20_RING,"SIDEETH metadata layout");
_Static_assert(S20_RING+2*S20_STRIDE==S20_SCRATCH,"SIDEETH ring layout");
_Static_assert(S20_SCRATCH+S20_STRIDE==S20_END,"SIDEETH scratch layout");
_Static_assert(S20_END<=S20_ACK && S20_ACK+6<=0x10000u,"SIDEETH window bounds");
typedef struct {
    uint8_t receiving[S20_MAX+9],request[S20_MAX+9];
    uint16_t used,prod,cons,length[2],rx_dropped,tx_dropped;
    bool collecting,enabled,wifi_up;
    uint16_t last_status;
    uint32_t host_ip;
    volatile bool pending;
    uint32_t accepted,rejected,busy_drops;
} s20_state;
static s20_state s20;
static const uint8_t s20_mac[6]={2,0x53,0x49,0x44,0x45,1};
static const uint8_t s20_ip[4]={192,168,254,1};
static uint16_t s20_be16(const uint8_t *p){return (uint16_t)((p[0]<<8)|p[1]);}
static uint32_t s20_be32(const uint8_t *p){return ((uint32_t)s20_be16(p)<<16)|s20_be16(p+2);}
static void s20_put16(uint8_t *p,uint16_t n){p[0]=(uint8_t)(n>>8);p[1]=(uint8_t)n;}
static uint16_t s20_crc(const uint8_t *p,size_t n){
    uint16_t c=0xffff;while(n--){c^=(uint16_t)*p++<<8;for(unsigned i=0;i<8;i++)c=(uint16_t)((c&0x8000)?(c<<1)^0x1021:c<<1);}return c;
}
static uint16_t s20_checksum(const uint8_t *p,size_t n){
    uint32_t sum=0;while(n>1){sum+=s20_be16(p);p+=2;n-=2;}if(n)sum+=(uint16_t)*p<<8;
    while(sum>>16)sum=(sum&65535)+(sum>>16);return (uint16_t)~sum;
}
/* Each shared CPU word is read as one big-endian word by the Atari. */
static void s20_word(uintptr_t m,unsigned o,uint16_t v){*(volatile uint16_t *)(m+o)=v;}
static void s20_long(uintptr_t m,unsigned o,uint32_t v){s20_word(m,o,(uint16_t)(v>>16));s20_word(m,o+2,(uint16_t)v);}
static void s20_bytes(uintptr_t m,unsigned o,const uint8_t *p,unsigned n){
    for(unsigned i=0;i<n;i+=2)s20_word(m,o+i,(uint16_t)((p[i]<<8)|((i+1<n)?p[i+1]:0)));
}
static void s20_publish(uintptr_t m,uint16_t status){
    s20.last_status=status;
    s20_long(m,S20_BASE,0x53455448u);s20_long(m,S20_BASE+4,2);
    s20_long(m,S20_BASE+8,status);s20_long(m,S20_BASE+12,(s20.enabled?3u:0u)|(s20.wifi_up?4u:0u));
    s20_bytes(m,S20_BASE+16,s20_mac,6);s20_word(m,S20_BASE+24,576);
    s20_word(m,S20_BASE+26,S20_MAX);s20_word(m,S20_BASE+28,2);s20_word(m,S20_BASE+30,0);
    s20_long(m,S20_BASE+32,s20.host_ip);s20_long(m,S20_BASE+36,0xc0a8fe02u);
    s20_long(m,S20_BASE+40,0xffffff00u);s20_long(m,S20_BASE+44,0xc0a8fe01u);
    s20_word(m,S20_BASE+50,0);
    s20_word(m,S20_BASE+52,s20.rx_dropped);s20_word(m,S20_BASE+54,s20.tx_dropped);
    s20_word(m,S20_BASE+66,s20.cons);
    s20_word(m,S20_BASE+68,s20.length[0]);s20_word(m,S20_BASE+70,s20.length[1]);
    __asm__ volatile("" ::: "memory");s20_word(m,S20_BASE+64,s20.prod);
    s20_word(m,S20_BASE+48,s20.prod!=s20.cons);
}
static void s20_init(uintptr_t m){
    memset(&s20,0,sizeof(s20));
    for(unsigned o=S20_BASE;o<S20_ACK+6;o+=2)s20_word(m,o,0);
    s20_publish(m,0);
}
static bool s20_feed(uint16_t a){
    if(a==S20_START){
        if(s20.collecting)s20.rejected++;
        if(s20.pending)s20.busy_drops++;
        s20.used=0;s20.collecting=!s20.pending;return true;
    }
    if(a==S20_STOP){
        if(s20.collecting){
            unsigned n=s20.used;
            if(n>=9 && s20_be16(s20.receiving+1)<=S20_MAX &&
               n==9u+s20_be16(s20.receiving+1) && s20_crc(s20.receiving,n-2)==s20_be16(s20.receiving+n-2)){
                memcpy(s20.request,s20.receiving,n);s20.accepted++;
                __asm__ volatile("" ::: "memory");s20.pending=true;
            }else s20.rejected++;
        }s20.collecting=false;return true;
    }
    if(a>=S20_DATA && a<S20_DATA+512 && !(a&1)){
        if(s20.collecting){
            if(s20.used<sizeof(s20.receiving))s20.receiving[s20.used++]=(uint8_t)((a-S20_DATA)>>1);
            else {s20.collecting=false;s20.rejected++;}
        }return true;
    }
    if(s20.collecting){s20.collecting=false;s20.rejected++;}
    return a>=S20_BASE && a<S20_ACK+6;
}
static uint16_t s20_enqueue(uintptr_t m,const uint8_t *f,unsigned n){
    if(s20.prod!=s20.cons){s20.rx_dropped++;return 3;}
    unsigned slot=s20.prod;s20_bytes(m,S20_RING+slot*S20_STRIDE,f,n);
    s20.length[slot]=(uint16_t)n;__asm__ volatile("" ::: "memory");s20.prod^=1;return 0;
}
static uint16_t s20_tx(uintptr_t m,const uint8_t *f,unsigned n){
    if(!s20.enabled)return 1;
    if(n<14 || n>S20_MAX)return 6;
    s20_bytes(m,S20_SCRATCH,f,n);
    uint8_t reply[S20_MAX];unsigned size=0;
    if(s20_be16(f+12)==0x0806){
        if(n<42 || s20_be16(f+14)!=1 || s20_be16(f+16)!=0x0800 ||
           f[18]!=6 || f[19]!=4 || s20_be16(f+20)!=1)return 6;
        if(memcmp(f+38,s20_ip,4))return 7;
        memset(reply,0,60);memcpy(reply,f+22,6);memcpy(reply+6,s20_mac,6);
        s20_put16(reply+12,0x0806);memcpy(reply+14,f+14,6);s20_put16(reply+20,2);
        memcpy(reply+22,s20_mac,6);memcpy(reply+28,s20_ip,4);
        memcpy(reply+32,f+22,10);size=60;
    }else if(s20_be16(f+12)==0x0800){
        if(n<34)return 6;
        const uint8_t *ip=f+14;unsigned h=(ip[0]&15u)*4u,total=s20_be16(ip+2);
        if((ip[0]>>4)!=4 || h<20 || h>n-14 || total<h || total>n-14 ||
           (s20_be16(ip+6)&0xbfffu) || s20_checksum(ip,h))return 6;
        if(memcmp(ip+16,s20_ip,4))return S20_ROUTE_TX(f,n);
        if(ip[9]!=1)return 7;
        if(total<h+8 || ip[h]!=8 || ip[h+1]!=0 || s20_checksum(ip+h,total-h))return 6;
        size=14+total;memcpy(reply,f,size);memcpy(reply,f+6,6);memcpy(reply+6,s20_mac,6);
        uint8_t *out=reply+14;memcpy(out+16,ip+12,4);memcpy(out+12,s20_ip,4);out[8]=64;
        out[h]=0;s20_put16(out+h+2,0);s20_put16(out+h+2,s20_checksum(out+h,total-h));
        s20_put16(out+10,0);s20_put16(out+10,s20_checksum(out,h));
        if(size<60){memset(reply+size,0,60-size);size=60;}
    }else return 7;
    return s20_enqueue(m,reply,size);
}
static void s20_service(uintptr_t m){
    if(!s20.pending)return;
    __asm__ volatile("" ::: "memory");
    uint8_t op=s20.request[0];unsigned n=s20_be16(s20.request+1);const uint8_t *p=s20.request+7;
    uint16_t status=6;
    if(op==1 && n==0)status=0;
    else if(op==2 && n==2 && s20_be16(p)<=1){
        s20.enabled=s20_be16(p)!=0;status=0;
        if(!s20.enabled){s20.prod=s20.cons=0;s20.length[0]=s20.length[1]=0;}
    }else if(op==3){status=s20_tx(m,p,n);if(status)s20.tx_dropped++;}
    else if(op==4 && n==2){
        if(!s20.enabled)status=1;
        else if(s20_be16(p)!=1)status=6;
        else if(s20.prod==s20.cons)status=2;
        else {s20.length[s20.cons]=0;s20.cons^=1;status=0;}
    }
    s20_publish(m,status);s20_word(m,S20_ACK+4,status);
    /* A guest may issue its next START as soon as ACK is visible. The
     * IRQ must see pending cleared when it handles that START. */
    uint32_t irq=S20_CRITICAL_ENTER();
    __asm__ volatile("" ::: "memory");s20_long(m,S20_ACK,s20_be32(s20.request+3));
    __asm__ volatile("" ::: "memory");s20.pending=false;
    S20_CRITICAL_LEAVE(irq);
}
#endif
