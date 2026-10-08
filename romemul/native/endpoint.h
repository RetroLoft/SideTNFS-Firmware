/* Native GEMDRIVE command endpoint. The SD service owns the mount;
 * SD and TNFS reads/writes are dispatched through this endpoint. */
#include "wire.h"
#include <stdarg.h>

static r02_wire r02;
#include "hardware/sync.h"
#define S20_CRITICAL_ENTER() save_and_disable_interrupts()
#define S20_CRITICAL_LEAVE(state) restore_interrupts(state)
#include "sideeth21_nat.h"
#define S20_ROUTE_TX(frame,len) s21_nat_tx((frame),(uint16_t)(len))
#include "sideeth20.h"
static uintptr_t s21_shared;
int s21_nat_deliver(const uint8_t *frame,uint16_t len){
    if(!s21_shared || !s20.enabled || s20.prod!=s20.cons || len>S20_MAX)return 1;
    if(s20_enqueue(s21_shared,frame,len))return 1;
    s20_publish(s21_shared,s20.last_status);
    return 0;
}
_Static_assert(GEMDRVEMUL_FLOPPY_SESSION_MEDIA_CHANGED+2<=S20_BASE,"SIDEETH overlaps upstream");
_Static_assert(R02_REPLY+R02_REPLY_SIZE<=S20_BASE,"SIDEETH overlaps native response");
#include "progress.h"
static uint8_t r02_response[R02_REPLY_SIZE];
#include "sd_fs.h"
#include "settings_drive.h"
#include "config_wire.h"
#include <malloc.h>
/* Allocator-owned arena, not total free RAM or largest allocatable block. */
static void r35_heap_report(const char *where){
    struct mallinfo m=mallinfo();
    LOG(TNFS,INFO,"F35 HEAP where=%s arena=%lu used=%lu free_in_arena=%lu free_chunks=%lu\r\n",
        where,(unsigned long)m.arena,(unsigned long)m.uordblks,
        (unsigned long)m.fordblks,(unsigned long)m.ordblks);
}
#define R35_HEAP_REPORT(where) r35_heap_report(where)
#include "tnfs_transport.h"
#include "tnfs_fs.h"
#include "ntp.h"
_Static_assert(GEMDRVEMUL_FLOPPY_SESSION_MEDIA_CHANGED + 2 <= R02_START, "Upstream shared memory overlaps native wire window");

static void r02_text(size_t *used, const char *fmt, ...) {
    if(*used>=511) return;
    va_list ap;
    va_start(ap,fmt);
    int n=vsnprintf((char *)r02_response+12+*used,512-*used,fmt,ap);
    va_end(ap);
    if(n>0) *used+=(size_t)n < 512-*used ? (size_t)n : 511-*used;
}

static uint16_t r02_listing(size_t *used, const char *path) {
    DIR directory;
    FILINFO info;
    r02a_mark(2, 0, 0);
    FRESULT fr=f_opendir(&directory,path);
    r02a_mark(3, fr, 0);
    r02_text(used,"DIR %s result=%u\r\n",path,(unsigned)fr);
    if(fr!=FR_OK) return 4;
    unsigned count=0;
    while(*used<440 && count<12) {
        r02a_mark(4, 0, count);
        fr=f_readdir(&directory,&info);
        r02a_mark(5, fr, count);
        if(fr!=FR_OK || !info.fname[0]) break;
        r02_text(used,"%c %.32s %lu\r\n",(info.fattrib&AM_DIR)?'D':'F',info.fname,(unsigned long)info.fsize);
        count++;
    }
    bool limited=count==12 || *used>=440;
    r02a_mark(6, fr, count);
    FRESULT close_fr=f_closedir(&directory);
    r02a_mark(7, close_fr, count);
    r02_text(used,"read=%u close=%u%s\r\n",(unsigned)fr,(unsigned)close_fr,limited?" LIMITED":" END");
    return fr==FR_OK && close_fr==FR_OK ? 0 : 4;
}

static bool r10_ready_logged;
static void r02_service(uintptr_t shared) {
    r02a_init(shared);
    volatile uint16_t *ready=(volatile uint16_t *)(shared+R02_READY);
    ready[0]=0x5232; ready[1]=0x4121; /* Atari sees R02! */
    if(!r10_ready_logged){
        r10_ready_logged=true;
        s20_init(shared);s21_shared=shared;s21_nat_init();
        LOG(SIDEETH,INFO,"F21 SIDEETH gateway ready; TCP/UDP/ICMP routing enabled\r\n");
        LOG(BOOT,INFO,"R12S boot ms=%lu stage=ready-published words=%04x:%04x SD=%u\r\n",(unsigned long)(time_us_32()/1000),ready[0],ready[1],(unsigned)sidetnfs_sd_global_status());
    }
    r02a_heartbeat++;
    if(!r02.pending) return;
    __asm__ volatile("" ::: "memory");
    uint8_t op=r02.request[0];
    uint16_t len=r02_be16(r02.request+1), status=0;
    r02a_begin(r02_be32(r02.request+3), op);
    size_t used=0;
    memset(r02_response,0,sizeof(r02_response));
    r02_put32(r02_response,r02_be32(r02.request+3));
    r02_put16(r02_response+4,op);
    if(op==14 && len==0){used=r14_clock(r02_response+12);
    }else if(op==15 && len==0){
        memset(r02_response+12,0,4);used=13;
        for(unsigned i=0;i<R13_SLOTS;i++){
            sidetnfs_slot_tnfs_context_t c;
            bool valid=i<g_drive_count && g_runtime_drives[i].valid &&
                sidetnfs_probe_get_slot_context(i,&c) && c.backend_type==2 && c.transport==0;
            r02_response[16+i]=valid?g_runtime_drives[i].drive_number:255;
        }
    }else if(op>=48 && op<48+16*R13_SLOTS){
        unsigned slot=(op-48)/16;
        sidetnfs_slot_tnfs_context_t c;
        if(!sidetnfs_probe_get_slot_context(slot,&c) || c.backend_type!=2 || c.transport!=0){r02_put32(r02_response+12,(uint32_t)-46);used=4;}
        else used=r13_fs(slot,16+(op-48)%16,r02.request+7,len,r02_response+12);
    }else if(op>=16 && op<=28) {
        used=r08_fs(op,r02.request+7,len,r02_response+12);
    } else if(op>=32 && op<=42) {
        used=r12s_fs(op,r02.request+7,len,r02_response+12);
    } else if(op==1) { /* Exact echo, including zeros and odd source addresses. */
        memcpy(r02_response+12,r02.request+7,len); used=len;
    } else if((op==2 || op==3) && len==0) {
        r02_text(&used,"R02 SD global=%u raw=%u\r\n",(unsigned)sidetnfs_sd_global_status(),(unsigned)sidetnfs_sd_global_fresult());
        if(sidetnfs_sd_global_status()!=SIDETNFS_SD_STATUS_READY) status=2;
        else if(op==2) status=r02_listing(&used,"0:/");
        else {
            bool found=false;
            for(int i=0;i<SIDETNFS_SD_SERVICE_MAX_SLOTS;i++) {
                sidetnfs_sd_drive_status_t s;
                if(!sidetnfs_sd_get_drive_status(i,&s)) continue;
                r02_text(&used,"slot=%d letter=%c state=%u path=%.96s\r\n",i,s.driveletter,(unsigned)s.status,s.sd_path);
                if(s.status==SIDETNFS_SD_STATUS_READY) { status=r02_listing(&used,s.sd_path); found=true; break; }
            }
            if(!found) { status=3; r02_text(&used,"No ready configured SD slot.\r\n"); }
        }
    } else { status=1; } /* Unknown command or wrong exact payload size. */
    r02_put16(r02_response+6,status);
    r02_put16(r02_response+8,used);
    r02_put16(r02_response+524,r02_crc(r02_response,524));
    /* ROM is served as 16-bit words. Convert each BE byte pair into one
     * native Pico word; an Atari word read then sees the original pair.
     * Publish sequence last. Guest validates the copied response CRC. */
    r02a_mark(8, status, r02a_entries);
    volatile uint16_t *out=(volatile uint16_t *)(shared+R02_REPLY);
    for(unsigned i=2;i<R02_REPLY_SIZE/2;i++) out[i]=r02_be16(r02_response+2*i);
    __asm__ volatile("" ::: "memory");
    out[0]=r02_be16(r02_response); out[1]=r02_be16(r02_response+2);
    __asm__ volatile("" ::: "memory");
    r02a_published=r02_be32(r02.request+3);
    r02.pending=false;
    r02a_mark(9, status, r02a_entries);
}

#include "network_service.h"

/* R13 uses demand-driven native TNFS sessions, not the R12 fixture probe. */

static void r12s_service(uintptr_t shared,const sidetnfs_system_settings_t *settings,char **password){
    if(sidetnfs_tp_step!=HEADER_DETECTION || active_command_id!=0xffff){
        r02_service(shared);
        return;
    }
    r11_service(shared,settings,password);
    if(!r02.pending && !r02.collecting){
        s20.wifi_up=r11_state.state==R11_UP;
        s20.host_ip=(s20.wifi_up && netif_default)?lwip_ntohl(ip4_addr_get_u32(netif_ip4_addr(netif_default))):0;
        s21_nat_set_link(s20.wifi_up?netif_default:NULL,s20.enabled);
        if(s20.pending){unsigned op=s20.request[0];s20_service(shared);
            /* Packet-by-packet UART would throttle browser transfers. */
            static uint32_t next_report;
            uint32_t now=time_us_32();
            if(op==2 || (int32_t)(now-next_report)>=0){
                next_report=now+1000000;
                LOG_AT(SIDEETH,op==2?SL_INFO:SL_DEBUG,"F21 SIDEETH op=%u enabled=%u wifi=%u rx_drop=%u tx_drop=%u accepted=%lu rejected=%lu busy=%lu\r\n",op,s20.enabled,s20.wifi_up,s20.rx_dropped,s20.tx_dropped,(unsigned long)s20.accepted,(unsigned long)s20.rejected,(unsigned long)s20.busy_drops);
            }}
    }
    if(!r02.pending && !r02.collecting){
        s21_nat_set_link(s20.wifi_up?netif_default:NULL,s20.enabled);
        s21_nat_poll();
        static uint32_t next_nat_report,last_nat_total,last_nat_drops;
        uint32_t now=time_us_32();
        const struct s21_nat_stats *ns=s21_nat_get_stats();
        uint32_t total=ns->tx_packets+ns->rx_packets+ns->tx_dropped+ns->rx_dropped+ns->invalid+ns->expired;
        if(total!=last_nat_total && (int32_t)(now-next_nat_report)>=0){
            next_nat_report=now+5000000;last_nat_total=total;
            uint32_t drops=ns->tx_dropped+ns->rx_dropped+ns->invalid;
            LOG_AT(SIDEETH,drops!=last_nat_drops?SL_WARN:SL_DEBUG,"F21 NAT maps=%u tx=%lu rx=%lu tx_drop=%lu rx_drop=%lu invalid=%lu expired=%lu pending=%u/%u\r\n",ns->mappings,(unsigned long)ns->tx_packets,(unsigned long)ns->rx_packets,(unsigned long)ns->tx_dropped,(unsigned long)ns->rx_dropped,(unsigned long)ns->invalid,(unsigned long)ns->expired,ns->tx_pending,ns->rx_pending);
            last_nat_drops=drops;
        }
    }
    if(!r02.pending && !r02.collecting)r14_ntp_tick(settings,r11_state.state==R11_UP,shared);
}
