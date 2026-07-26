#ifndef LWIP_PBUF_H
#define LWIP_PBUF_H
#include <stdint.h>
#include <stddef.h>

typedef uint16_t u16_t;
typedef uint8_t u8_t;

#define PBUF_TRANSPORT 0
#define PBUF_RAM 1

struct pbuf
{
    void *payload;
    uint16_t tot_len;
    uint16_t len;
    struct pbuf *next;
};

struct pbuf *pbuf_alloc(int layer, uint16_t length, int type);
void pbuf_free(struct pbuf *p);
uint16_t pbuf_copy_partial(const struct pbuf *p, void *dataptr, uint16_t len, uint16_t offset);

#endif
