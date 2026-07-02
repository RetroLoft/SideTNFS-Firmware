#ifndef LOG_EVENT_H_
#define LOG_EVENT_H_

#include <stdint.h>

// ── Event codes ───────────────────────────────────────────────────────────────
#define EVT_DISPATCH        0x01u   // command dispatched; a=cmd
#define EVT_FRAMING         0x02u   // framing error; a=cmd
#define EVT_DGETPATH        0x20u   // dgetpath returning path; a=path[0..3] packed BE
#define EVT_DSETPATH_RX     0x21u   // dsetpath received; a=path[0..3]
#define EVT_DSETPATH_OK     0x22u   // dsetpath stored; a=path[0..3]
#define EVT_FSFIRST         0x30u   // fsfirst; a=fspec[0..3], b=attrib
#define EVT_FSNEXT          0x31u   // fsnext; a=ndta, b=dir_index
#define EVT_FSNEXT_NDTA0    0x32u   // fsnext bad: ndta was 0
#define EVT_FOPEN           0x40u   // fopen; a=name[0..3], b=mode
#define EVT_FCREATE         0x41u   // fcreate; a=name[0..3]
#define EVT_FOPEN_WRDENY    0x42u   // fopen write blocked (legacy); a=name[0..3], b=mode
#define EVT_FWRITE_DENY     0x43u   // fwrite blocked (legacy); a=fd
#define EVT_FWRITE          0x44u   // fwrite chunk; a=fd, b=count, c=n_written
#define EVT_FWRITE_ACK      0x45u   // fwrite ack; a=fd
#define EVT_FDELETE         0x46u   // fdelete; a=name[0..3]
#define EVT_DCREATE_DO      0x47u   // dcreate; a=name[0..3]
#define EVT_DDELETE_DO      0x48u   // ddelete; a=name[0..3]
#define EVT_FRENAME         0x49u   // frename; a=old[0..3], b=new[0..3]
#define EVT_PEXEC           0x50u   // pexec dispatched

// Pack up to 4 bytes of a C string into a uint32_t (big-endian, zero-padded).
static inline uint32_t log_pack4(const char *s)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        v <<= 8;
        if (s[i]) v |= (uint8_t)s[i];
    }
    return v;
}

#ifdef SIDETNFS_DEBUG

void log_event(uint32_t code, uint32_t a, uint32_t b, uint32_t c);
void log_flush(void);

#else // !SIDETNFS_DEBUG

static inline void log_event(uint32_t code, uint32_t a, uint32_t b, uint32_t c)
{ (void)code; (void)a; (void)b; (void)c; }
static inline void log_flush(void) {}

#endif // SIDETNFS_DEBUG

#endif // LOG_EVENT_H_
