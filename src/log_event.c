#include "include/log_event.h"

#ifdef SIDETNFS_DEBUG

#include <stdio.h>

#define LOG_BUF_SIZE 64u  // must be a power of 2

typedef struct {
    uint32_t code;
    uint32_t a, b, c;
} LogEntry;

static LogEntry s_buf[LOG_BUF_SIZE];
static uint32_t s_head = 0;
static uint32_t s_tail = 0;

void log_event(uint32_t code, uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t idx    = s_head & (LOG_BUF_SIZE - 1u);
    s_buf[idx].code = code;
    s_buf[idx].a    = a;
    s_buf[idx].b    = b;
    s_buf[idx].c    = c;
    s_head++;
    // If buffer is full, advance tail so oldest entry is overwritten.
    if ((s_head - s_tail) > LOG_BUF_SIZE) s_tail = s_head - LOG_BUF_SIZE;
}

void log_flush(void)
{
    while (s_tail != s_head) {
        uint32_t idx    = s_tail & (LOG_BUF_SIZE - 1u);
        const LogEntry *e = &s_buf[idx];
        printf("[EVT %02lx] %08lx %08lx %08lx\n",
               (unsigned long)e->code,
               (unsigned long)e->a,
               (unsigned long)e->b,
               (unsigned long)e->c);
        s_tail++;
    }
}

#endif // SIDETNFS_DEBUG
