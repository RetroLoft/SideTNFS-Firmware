/* Main-loop adapter: SD service first; never enter networking mid-packet. */
#include "network_state.h"
#include "network_api.h"
static r11_coop r11_state;
static const sidetnfs_system_settings_t *r11_settings;
static char **r11_password;
static uintptr_t r11_shared;
static uint32_t r11_sd_counts[6],r11_next_report,r11_max_step_us;
static bool r11_report_started;
static int r11_begin(void *c){(void)c;return r11_radio_begin(r11_settings,r11_password);}
static void r11_poll(void *c){(void)c;r11_radio_poll();}
static int r11_link(void *c){(void)c;return r11_radio_link();}
static void r11_leave(void *c){(void)c;r11_radio_leave();}
static void r11_event(void *c,unsigned state,unsigned attempt,int reason){
    (void)c;
    r13d_radio_diag("state");
    if(state==R11_OFFLINE)LOG(WIFI,WARN,"R13D cooldown ms=60000 next_episode=automatic\r\n");
    /* This is Wi-Fi readiness only. Native TNFS mounts are a later gate. */
    *((volatile uint32_t *)(r11_shared+GEMDRVEMUL_NETWORK_STATUS))=state==R11_UP?0xffffffffu:0;
    LOG_AT(WIFI,reason?SL_WARN:SL_INFO,"R13D net ms=%lu state=%u attempt=%u reason=%d sd_mounted=%u\r\n",(unsigned long)(time_us_64()/1000),state,attempt,reason,(unsigned)r03_mounted);
}
static r11_ops r11_callbacks={NULL,r11_begin,r11_poll,r11_link,r11_leave,r11_event};
static void r11_service(uintptr_t shared,const sidetnfs_system_settings_t *settings,char **password){
    unsigned state=r11_state.state;
    bool sd=r02.pending && r02.request[0]>=16 && r02.request[0]<=26;
    r02_service(shared);
    if(sd)r11_sd_counts[state]++;
    r11_shared=shared;r11_settings=settings;r11_password=password;
    if(!r02.pending && !r02.collecting){
        uint32_t start=time_us_32();
        r11_tick(&r11_state,&r11_callbacks,(uint32_t)(time_us_64()/1000),r03_mounted,settings->ssid[0]!=0);
        uint32_t used=time_us_32()-start;if(used>r11_max_step_us)r11_max_step_us=used;
    }
    /* A request can arrive during an SDK action; service it on return. */
    state=r11_state.state;sd=r02.pending && r02.request[0]>=16 && r02.request[0]<=26;
    r02_service(shared);if(sd)r11_sd_counts[state]++;
    static uint64_t audit_due;
    uint64_t audit_now=time_us_64();
    if(audit_now>=audit_due && !r02.pending && !r02.collecting){
        audit_due=audit_now+60000000ULL;r35_report("periodic");
    }
    uint32_t now=(uint32_t)(time_us_64()/1000);
    if(!r11_report_started || r11_due(now,r11_next_report)){
        r11_report_started=true;r11_next_report=now+5000;
        r13d_radio_diag("service");
        LOG(GEMDOS,DEBUG,"R11 service ms=%lu state=%u sd_idle=%lu sd_join=%lu sd_up=%lu sd_backoff=%lu sd_offline=%lu sd_disabled=%lu max_net_us=%lu\r\n",(unsigned long)now,r11_state.state,(unsigned long)r11_sd_counts[0],(unsigned long)r11_sd_counts[1],(unsigned long)r11_sd_counts[2],(unsigned long)r11_sd_counts[3],(unsigned long)r11_sd_counts[4],(unsigned long)r11_sd_counts[5],(unsigned long)r11_max_step_us);
    }
}
