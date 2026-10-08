/* R02a progress: main-loop milestones, independently captured by DMA IRQ.
 * No filesystem calls, UART or protocol-pending dependency in snapshot. */
#define R02A_CAPTURE 0xd004u
#define R02A_SNAPSHOT 0xef40u
#define R02A_SNAPSHOT_SIZE 64u
static volatile uint32_t r02a_sequence, r02a_entries, r02a_published, r02a_heartbeat;
static volatile uint16_t r02a_opcode, r02a_phase, r02a_result;
static uint16_t r02a_generation;
static bool r02a_initialized;

static void r02a_init(uintptr_t shared) {
    if(r02a_initialized) return;
    volatile uint16_t *out=(volatile uint16_t *)(shared+R02A_SNAPSHOT);
    for(unsigned i=0;i<R02A_SNAPSHOT_SIZE/2;i++) out[i]=0;
    r02a_initialized=true;
}

static void r02a_mark(uint16_t phase, uint16_t result, uint32_t entries) {
    uint32_t irq=save_and_disable_interrupts();
    r02a_result=result; r02a_entries=entries; r02a_phase=phase;
    restore_interrupts(irq);
}
static void r02a_begin(uint32_t sequence, uint16_t op) {
    uint32_t irq=save_and_disable_interrupts();
    r02a_sequence=sequence; r02a_opcode=op;
    r02a_result=0; r02a_entries=0; r02a_phase=1;
    restore_interrupts(irq);
}
static bool r02a_irq(uint16_t address, uintptr_t shared) {
    if(address==R02A_CAPTURE) {
        uint8_t b[R02A_SNAPSHOT_SIZE]={0};
        uint16_t generation=++r02a_generation;
        r02_put32(b,0x52324144); /* R2AD */
        r02_put16(b+4,generation); r02_put16(b+6,1);
        r02_put32(b+8,r02.accepted); r02_put32(b+12,r02.rejected);
        r02_put32(b+16,r02.busy_drops);
        r02_put32(b+20,r02.accepted ? r02_be32(r02.request+3) : 0);
        r02_put16(b+24,r02.accepted ? r02.request[0] : 0);
        r02_put16(b+26,r02a_phase); r02_put32(b+28,r02a_sequence);
        r02_put16(b+32,r02a_opcode); r02_put16(b+34,r02a_result);
        r02_put32(b+36,r02a_entries); r02_put32(b+40,r02a_published);
        r02_put16(b+44,r02.pending); r02_put16(b+46,r02.collecting);
        r02_put16(b+48,r02.used); r02_put32(b+52,r02a_heartbeat);
        r02_put32(b+56,time_us_32());
        r02_put16(b+60,r02_crc(b,60));
        volatile uint16_t *out=(volatile uint16_t *)(shared+R02A_SNAPSHOT);
        for(unsigned i=0;i<R02A_SNAPSHOT_SIZE/2;i++) if(i!=2) out[i]=r02_be16(b+2*i);
        __asm__ volatile("" ::: "memory");
        out[2]=generation; /* atomic commit word, only written last */
        return true;
    }
    if(address>=R02A_SNAPSHOT && address<R02A_SNAPSHOT+R02A_SNAPSHOT_SIZE) return true;
    if(s20_feed(address))return true;
    return r02_feed(&r02,address);
}
