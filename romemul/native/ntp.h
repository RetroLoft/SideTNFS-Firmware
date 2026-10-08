/* Nonblocking, one-shot NTP sync per boot. Only idle main-loop code sends.
 * RTC is changed only after peer, mode, stratum and originate validation. */
#include <time.h>
#include "hardware/rtc.h"
static struct {
    struct udp_pcb *pcb;ip_addr_t ip;
    uint8_t state,attempts,nonce[8];bool valid;
    uint32_t deadline;int offset;
    uint64_t epoch;char host[128];
} r14_ntp;
static uint32_t r14_net32(const uint8_t *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static bool r14_ntp_reply(const uint8_t *b,unsigned n,const uint8_t *nonce,uint64_t *epoch){
    if(n<48 || (b[0]>>6)==3 || ((b[0]>>3)&7)<3 || ((b[0]>>3)&7)>4 || (b[0]&7)!=4 || !b[1] || b[1]>15 || memcmp(b+24,nonce,8))return false;
    uint64_t sec=r14_net32(b+40);
    if(!sec && !r14_net32(b+44))return false;
    /* NTP era rollover in2036, with supported dates1980..2107. */
    if(sec<2208988800ULL)sec+=0x100000000ULL;
    *epoch=sec-2208988800ULL;return *epoch>=315532800ULL && *epoch<4354819200ULL;
}
static void r14_ntp_dns(const char *name,const ip_addr_t *ip,void *ctx){
    (void)name;(void)ctx;if(r14_ntp.state!=1)return;
    if(ip){ip_addr_copy(r14_ntp.ip,*ip);r14_ntp.state=2;}else r14_ntp.state=5;
}
static void r14_ntp_recv(void *ctx,struct udp_pcb *pcb,struct pbuf *p,const ip_addr_t *ip,u16_t port){
    (void)ctx;(void)pcb;if(!p)return;
    if(r14_ntp.state==3 && port==123 && ip_addr_cmp(ip,&r14_ntp.ip) && p->tot_len>=48){
        uint8_t b[48];pbuf_copy_partial(p,b,48,0);
        if(r14_ntp_reply(b,48,r14_ntp.nonce,&r14_ntp.epoch))r14_ntp.state=4;
    }pbuf_free(p);
}
static void r14_ntp_tick(const sidetnfs_system_settings_t *cfg,bool up,uintptr_t shared){
    if(!cfg->rtc_enabled || !cfg->ntp_server[0] || r14_ntp.valid)return;
    uint32_t now=time_us_32();
    if(r14_ntp.state==4){
        int64_t local=(int64_t)r14_ntp.epoch+r14_ntp.offset;
        if(local<315532800LL || local>=4354819200LL){r14_ntp.state=5;return;}
        time_t t=(time_t)local;if((int64_t)t!=local){r14_ntp.state=5;return;}
        struct tm *tm=gmtime(&t);
        if(!tm){r14_ntp.state=5;return;}
        datetime_t dt={.year=tm->tm_year+1900,.month=tm->tm_mon+1,.day=tm->tm_mday,
            .dotw=tm->tm_wday,.hour=tm->tm_hour,.min=tm->tm_min,.sec=tm->tm_sec};
        rtc_init();r14_ntp.valid=rtc_set_datetime(&dt);r14_ntp.state=5;
        /* SIDETNFS.PRG reads this legacy ROM3 status directly. Publish only
         * after the validated response has successfully set the RTC. Both
         * 0 and READY are endian-invariant; boot initializes the field to 0. */
        *((volatile uint32_t *)(shared+GEMDRVEMUL_RTC_STATUS))=r14_ntp.valid?UINT32_MAX:0;
        LOG_AT(NTP,r14_ntp.valid?SL_INFO:SL_WARN,"R14 NTP sync=%u date=%04d-%02d-%02d time=%02d:%02d:%02d offset_hours=%d\r\n",r14_ntp.valid,dt.year,dt.month,dt.day,dt.hour,dt.min,dt.sec,r14_ntp.offset/3600);
        return;
    }
    if((r14_ntp.state==1 || r14_ntp.state==3) && (int32_t)(now-r14_ntp.deadline)>=0)r14_ntp.state=5;
    if(r14_ntp.state==5){
        LOG(NTP,WARN,"R14 NTP attempt=%u complete valid=%u\r\n",r14_ntp.attempts,r14_ntp.valid);
        r14_ntp.state=6;r14_ntp.deadline=now+60000000u;return;
    }
    if(!up)return;
    if(r14_ntp.state==6){if(r14_ntp.attempts>=3 || (int32_t)(now-r14_ntp.deadline)<0)return;r14_ntp.state=0;}
    if(!r14_ntp.state){
        r14_ntp.attempts++;r14_ntp.offset=atoi(cfg->utc_offset)*3600;
        snprintf(r14_ntp.host,sizeof(r14_ntp.host),"%s",cfg->ntp_server);
        r14_ntp.state=1;r14_ntp.deadline=now+5000000u;
        cyw43_arch_lwip_begin();
        err_t e=dns_gethostbyname(r14_ntp.host,&r14_ntp.ip,r14_ntp_dns,NULL);
        cyw43_arch_lwip_end();
        if(e==ERR_OK)r14_ntp.state=2;else if(e!=ERR_INPROGRESS)r14_ntp.state=5;
        LOG(NTP,INFO,"R14 NTP start attempt=%u host=%s\r\n",r14_ntp.attempts,r14_ntp.host);return;
    }
    if(r14_ntp.state==2){
        uint8_t b[48]={0};b[0]=0x23;
        uint64_t nonce=time_us_64()^((uint64_t)r14_ntp.attempts<<56);
        for(unsigned i=0;i<8;i++)b[40+i]=r14_ntp.nonce[i]=(uint8_t)(nonce>>(i*8));
        cyw43_arch_lwip_begin();
        if(!r14_ntp.pcb){r14_ntp.pcb=udp_new();if(r14_ntp.pcb)udp_recv(r14_ntp.pcb,r14_ntp_recv,NULL);}
        struct pbuf *p=r14_ntp.pcb?pbuf_alloc(PBUF_TRANSPORT,48,PBUF_RAM):NULL;
        err_t e=ERR_MEM;r14_ntp.state=3;r14_ntp.deadline=now+5000000u;
        if(p){pbuf_take(p,b,48);e=udp_sendto(r14_ntp.pcb,p,&r14_ntp.ip,123);pbuf_free(p);}
        cyw43_arch_lwip_end();if(e!=ERR_OK)r14_ntp.state=5;
    }
}
static size_t r14_clock(uint8_t *out){
    datetime_t t;if(!r14_ntp.valid || !rtc_get_datetime(&t)){r02_put32(out,(uint32_t)-2);return 4;}
    r02_put32(out,0);r02_put16(out+4,((t.year-1980)<<9)|(t.month<<5)|t.day);
    r02_put16(out+6,(t.hour<<11)|(t.min<<5)|(t.sec/2));return 8;
}
