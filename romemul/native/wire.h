/* R02 diagnostic transport. GPL-3.0. No filesystem or SDK dependencies. */
#ifndef SREC_R02A_WIRE_H
#define SREC_R02A_WIRE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define R02_MAX 256u
#define R02_START 0xd000u
#define R02_END 0xd002u
#define R02_DATA 0xe200u
#define R02_READY 0xef00u
#define R02_REPLY 0xf000u
#define R02_REPLY_SIZE 528u

/* Packet: opcode:u8, length:BE16, sequence:BE32, payload, CRC16:BE16.
 * Every byte is one even cartridge read DATA+(byte<<1). An explicit END
 * is required. Unrelated reads, excess bytes and incomplete frames reject;
 * no padding, format guessing or fallback. CRC is CCITT-FALSE. */
typedef struct {
    uint8_t receiving[R02_MAX+9], request[R02_MAX+9];
    uint16_t used, request_size;
    bool collecting;
    volatile bool pending;
    uint32_t accepted, rejected, busy_drops;
} r02_wire;

static inline uint16_t r02_be16(const uint8_t *p) { return ((uint16_t)p[0]<<8)|p[1]; }
static inline uint32_t r02_be32(const uint8_t *p) { return ((uint32_t)r02_be16(p)<<16)|r02_be16(p+2); }
static inline void r02_put16(uint8_t *p, uint16_t n) { p[0]=n>>8; p[1]=n; }
static inline void r02_put32(uint8_t *p, uint32_t n) { r02_put16(p,n>>16); r02_put16(p+2,n); }
static inline uint16_t r02_crc(const uint8_t *p, size_t n) {
    uint16_t crc=0xffff;
    while(n--) {
        crc^=(uint16_t)*p++<<8;
        for(unsigned i=0;i<8;i++) crc=(crc&0x8000)?(crc<<1)^0x1021:crc<<1;
    }
    return crc;
}

static inline bool r02_feed(r02_wire *w, uint16_t address) {
    if(address==R02_START) {
        if(w->pending) w->busy_drops++;
        if(w->collecting) w->rejected++;
        w->used=0;
        w->collecting=!w->pending;
        return true;
    }
    if(address==R02_END) {
        if(w->collecting) {
            unsigned n=w->used;
            if(n>=9 && r02_be16(w->receiving+1)<=R02_MAX &&
               n==9u+r02_be16(w->receiving+1) &&
               r02_crc(w->receiving,n-2)==r02_be16(w->receiving+n-2)) {
                memcpy(w->request,w->receiving,n);
                w->request_size=n;
                w->accepted++;
                /* The IRQ is the only producer; the main loop only clears
                 * pending after publishing the response. */
                __asm__ volatile("" ::: "memory");
                w->pending=true;
            } else w->rejected++;
        }
        w->collecting=false;
        return true;
    }
    if(address>=R02_DATA && address<R02_DATA+512 && !(address&1)) {
        if(w->collecting) {
            if(w->used<sizeof(w->receiving)) w->receiving[w->used++]=(address-R02_DATA)>>1;
            else { w->collecting=false; w->rejected++; }
        }
        return true;
    }
    if(w->collecting) { w->collecting=false; w->rejected++; }
    return (address>=R02_REPLY && address<R02_REPLY+R02_REPLY_SIZE) ||
           (address>=R02_READY && address<R02_READY+4);
}
#endif
