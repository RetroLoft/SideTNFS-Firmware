/* One outstanding request per session. At most two identical retries.
 * All rounds of one cartridge operation share a four-second deadline.
 * Faults stop the current operation; the filesystem layer decides whether
 * a fresh session is safe on a later caller request. */
#include "lwip/udp.h"
#include "lwip/dns.h"
#include "network_api.h"
#define R13_SLOTS 9
#define R13_PACKET 540
static struct r13_net_slot {
    struct udp_pcb *pcb; ip_addr_t address; uint16_t port,sid;
    uint8_t seq,cmd; bool mounted,poisoned,waiting,dns_wait,dns_ready;
    /* May remain idle for hours: a signed 32-bit deadline is insufficient. */
    bool recovering; uint64_t recover_at;
    uint32_t started,reject_peer,reject_session,reject_sequence,reject_size;
    uint16_t received,retry_ms,last_reject_sid,last_reject_len;
    uint8_t retries,last_reject_seq,last_reject_cmd,last_reject_reason;
    uint32_t base_peer,base_session,base_sequence,base_size;
    uint8_t rx[R13_PACKET];
} r13_net[R13_SLOTS];
/* Cumulative diagnostics survive session retirement; never control recovery. */
static struct r35_resource {
    uint32_t rpc,tx_alloc,tx_free,pbuf_fail,send_fail,pcb_alloc,pcb_free,pcb_fail;
    uint32_t opens,closes,dir_opens,dir_closes,files,dirs,abandoned_files,abandoned_dirs;
    uint32_t resource_rc,faults;
} r35_resources[R13_SLOTS];
#ifndef R35_HEAP_REPORT
#define R35_HEAP_REPORT(where) ((void)(where))
#endif
static void r35_report(const char *where){
    R35_HEAP_REPORT(where);
    for(unsigned i=0;i<R13_SLOTS;i++){
        struct r35_resource *a=&r35_resources[i];
        if(!a->rpc && !a->pcb_fail)continue;
        LOG(TNFS,INFO,"F35 AUDIT where=%s slot=%u sid=%04x rpc=%lu faults=%lu pcb=%lu/%lu pcb_fail=%lu txbuf=%lu/%lu pbuf_fail=%lu send_fail=%lu resource_rc=%lu\r\n",
            where,i,r13_net[i].sid,(unsigned long)a->rpc,(unsigned long)a->faults,
            (unsigned long)a->pcb_alloc,(unsigned long)a->pcb_free,(unsigned long)a->pcb_fail,
            (unsigned long)a->tx_alloc,(unsigned long)a->tx_free,(unsigned long)a->pbuf_fail,
            (unsigned long)a->send_fail,(unsigned long)a->resource_rc);
        LOG(TNFS,INFO,"F35 HANDLES slot=%u open=%lu close=%lu live_files=%lu diropen=%lu dirclose=%lu live_dirs=%lu abandoned_files=%lu abandoned_dirs=%lu\r\n",
            i,(unsigned long)a->opens,(unsigned long)a->closes,(unsigned long)a->files,
            (unsigned long)a->dir_opens,(unsigned long)a->dir_closes,(unsigned long)a->dirs,
            (unsigned long)a->abandoned_files,(unsigned long)a->abandoned_dirs);
    }
}
static uint32_t r13_deadline;
static unsigned r13_current,r13_guest_op;
static uint32_t r13_file_offset;
static uint8_t r13_tx[R13_PACKET];
static uint16_t r13_le16(const uint8_t *p){return p[0]|((uint16_t)p[1]<<8);}
static uint32_t r13_le32(const uint8_t *p){return r13_le16(p)|((uint32_t)r13_le16(p+2)<<16);}
static void r13_putle16(uint8_t *p,unsigned n){p[0]=n;p[1]=n>>8;}
static void r13_putle32(uint8_t *p,uint32_t n){r13_putle16(p,n);r13_putle16(p+2,n>>16);}
static void r13_dns(const char *name,const ip_addr_t *address,void *arg){
    (void)name;struct r13_net_slot *s=arg;if(!s->dns_wait)return;
    if(address){ip_addr_copy(s->address,*address);s->dns_ready=true;}
    s->dns_wait=false;
}
static void r13_udp(void *arg,struct udp_pcb *pcb,struct pbuf *p,const ip_addr_t *address,u16_t port){
    struct r13_net_slot *s=arg;
    if(!p)return;
    if(pcb!=s->pcb){pbuf_free(p);return;} /* Retired endpoint cannot feed a new session. */
    uint8_t h[4]={0};if(p->tot_len>=4)pbuf_copy_partial(p,h,4,0);
    if(s->waiting && !s->received){
        if(port!=s->port || !ip_addr_cmp(address,&s->address)){s->reject_peer++;s->last_reject_reason=1;}
        else if(p->tot_len<4 || p->tot_len>R13_PACKET){s->reject_size++;s->last_reject_reason=2;}
        else {
            pbuf_copy_partial(p,h,4,0);
            if(h[2]!=s->seq || h[3]!=s->cmd){s->reject_sequence++;s->last_reject_reason=3;}
            else if(s->cmd!=0 && r13_le16(h)!=s->sid){s->reject_session++;s->last_reject_reason=4;}
            else {pbuf_copy_partial(p,s->rx,p->tot_len,0);s->received=p->tot_len;}
        }
        if(!s->received){s->last_reject_sid=r13_le16(h);s->last_reject_seq=h[2];s->last_reject_cmd=h[3];s->last_reject_len=p->tot_len;}
    }
    pbuf_free(p);
}
static bool r13_expired(void){return (int32_t)(time_us_32()-r13_deadline)>=0;}
/* Diagnostics run outside the lwIP callback. Deltas belong to this request. */
static void r13_rx_diag(void){
    struct r13_net_slot *s=&r13_net[r13_current];
    LOG(TNFS,DEBUG,"R14A RX slot=%u retries=%u min_ms=%u delta_peer=%lu delta_session=%lu delta_seqcmd=%lu delta_size=%lu last_reason=%u last_sid=%04x last_seq=%u last_cmd=%02x last_len=%u\r\n",
      r13_current,s->retries,s->retry_ms,(unsigned long)(s->reject_peer-s->base_peer),
      (unsigned long)(s->reject_session-s->base_session),(unsigned long)(s->reject_sequence-s->base_sequence),
      (unsigned long)(s->reject_size-s->base_size),s->last_reject_reason,s->last_reject_sid,s->last_reject_seq,s->last_reject_cmd,s->last_reject_len);
}
static int r13_fault(const char *reason,const char *where,unsigned line){
    struct r13_net_slot *s=&r13_net[r13_current];
    if(!s->poisoned)LOG(TNFS,ERROR,"R14A FAIL slot=%u op=%u offset=%lu cmd=%02x seq=%u sid=%04x reason=%s where=%s line=%u rx=%u age_us=%lu reject_peer=%lu reject_session=%lu reject_seq=%lu reject_size=%lu\r\n",
        r13_current,r13_guest_op,(unsigned long)r13_file_offset,s->cmd,s->seq,s->sid,reason,where,line,s->received,
        (unsigned long)(time_us_32()-s->started),(unsigned long)s->reject_peer,(unsigned long)s->reject_session,
        (unsigned long)s->reject_sequence,(unsigned long)s->reject_size);
    r35_resources[r13_current].faults++;
    r35_report("fault");
    r13d_radio_diag("tnfs-failure");
    r13_rx_diag();s->poisoned=true;return -65;
}
#define r13_bad() r13_fault("protocol",__func__,__LINE__)
/* r13_tx remains immutable for the whole transaction; polling never starts
 * another cartridge RPC. No new sequence, open, seek or remount on retry. */
static err_t r13_send(struct r13_net_slot *s,unsigned size){
    cyw43_arch_lwip_begin();
    struct pbuf *p=pbuf_alloc(PBUF_TRANSPORT,size,PBUF_RAM);err_t e=ERR_MEM;
    struct r35_resource *a=&r35_resources[r13_current];
    if(p){a->tx_alloc++;pbuf_take(p,r13_tx,size);e=udp_sendto(s->pcb,p,&s->address,s->port);pbuf_free(p);a->tx_free++;if(e!=ERR_OK)a->send_fail++;}
    else a->pbuf_fail++;
    cyw43_arch_lwip_end();return e;
}
/* Returns raw wire rc; negative values are local failures. */
static int r13_rpc(uint8_t cmd,const uint8_t *body,unsigned count){
    struct r13_net_slot *s=&r13_net[r13_current];
    if(s->poisoned)return -65;
    if(count>R13_PACKET-4)return r13_bad();
    if(r13_expired())return r13_fault("operation-budget",__func__,__LINE__);
    r35_resources[r13_current].rpc++;
    r13_putle16(r13_tx,s->sid);r13_tx[2]=++s->seq;r13_tx[3]=cmd;
    memcpy(r13_tx+4,body,count);s->started=time_us_32();s->cmd=cmd;s->received=0;s->waiting=true;s->retries=0;
    s->base_peer=s->reject_peer;s->base_session=s->reject_session;s->base_sequence=s->reject_sequence;s->base_size=s->reject_size;
    s->last_reject_reason=0;s->last_reject_sid=0;s->last_reject_seq=0;s->last_reject_cmd=0;s->last_reject_len=0;
    uint32_t interval=(uint32_t)(s->retry_ms<1000?1000:s->retry_ms)*1000u;
    uint32_t next=s->started+interval;
    LOG(TNFS,TRACE,"F19 TX slot=%u op=%u offset=%lu sid=%04x seq=%u cmd=%02x bytes=%u budget_us=%lu\r\n",
        r13_current,r13_guest_op,(unsigned long)r13_file_offset,s->sid,s->seq,cmd,count+4,
        (unsigned long)(r13_deadline-time_us_32()));
    err_t e=r13_send(s,count+4);
    while(e==ERR_OK && !s->received && !r13_expired()){
        r11_radio_poll();
        /* MOUNT has no established session/cache: never replay it. */
        if(!s->received && !r13_expired() && cmd!=0 && s->mounted && s->retries<2 && (int32_t)(time_us_32()-next)>=0){
            s->retries++;
            LOG(TNFS,WARN,"R14A RETRY slot=%u op=%u offset=%lu cmd=%02x seq=%u sid=%04x retry=%u age_us=%lu\r\n",
              r13_current,r13_guest_op,(unsigned long)r13_file_offset,cmd,s->seq,s->sid,s->retries,(unsigned long)(time_us_32()-s->started));
            /* Logging must not push a resend beyond the operation deadline. */
            if(r13_expired())break;
            e=r13_send(s,count+4);next=time_us_32()+interval;
        }
        tight_loop_contents();
    }
    s->waiting=false;
    if(e!=ERR_OK)return r13_fault("send",__func__,__LINE__);
    if(!s->received)return r13_fault("reply-timeout",__func__,__LINE__);
    if(s->received<5)return r13_fault("short-reply",__func__,__LINE__);
    if(s->retries){LOG(TNFS,INFO,"R14A RECOVER slot=%u cmd=%02x seq=%u retries=%u rc=%u\r\n",r13_current,cmd,s->seq,s->retries,s->rx[4]);r13_rx_diag();}
    if(s->rx[4] && s->rx[4]!=0x21)LOG(TNFS,DEBUG,"R14A RC slot=%u op=%u cmd=%02x seq=%u rc=%u rx=%u\r\n",r13_current,r13_guest_op,cmd,s->seq,s->rx[4],s->received);
    struct r35_resource *a=&r35_resources[r13_current];
    if(s->rx[4]==0x08 || s->rx[4]==0x0f || s->rx[4]==0x10)a->resource_rc++;
    if(!s->rx[4]){
        if(cmd==0x29 && s->received==6){a->opens++;a->files++;}
        if(cmd==0x23){a->closes++;if(a->files)a->files--;}
        if(cmd==0x17 && s->received>=6){a->dir_opens++;a->dirs++;}
        if(cmd==0x12){a->dir_closes++;if(a->dirs)a->dirs--;}
    }
    return s->rx[4];
}
static int r13_begin(unsigned slot){
    r13_current=slot;r13_deadline=time_us_32()+4000000u;
    struct r13_net_slot *s=&r13_net[slot];
    if(s->poisoned)return -65;
    if(s->recovering){
        if(time_us_64()<s->recover_at)return -2;
        s->recovering=false;
        LOG(TNFS,INFO,"F19 TNFS remount slot=%u op=%u\r\n",slot,r13_guest_op);
    }
    if(r11_radio_link()!=1)return -2; /* Allow later first access after Wi-Fi joins. */
    if(s->mounted)return 0;
    sidetnfs_slot_tnfs_context_t config;
    if(!sidetnfs_probe_get_slot_context(slot,&config))return -46;
    uint32_t dns_started=time_us_32();
    s->port=config.port;s->dns_wait=true;s->dns_ready=false;
    cyw43_arch_lwip_begin();
    err_t e=dns_gethostbyname(config.host,&s->address,r13_dns,s);
    if(e==ERR_OK){s->dns_ready=true;s->dns_wait=false;}
    cyw43_arch_lwip_end();
    while(e==ERR_INPROGRESS && s->dns_wait && !r13_expired()){r11_radio_poll();tight_loop_contents();}
    s->dns_wait=false;
    if(!s->dns_ready){
        /* No MOUNT or file operation was sent. A DNS failure is temporary,
         * not ambiguous server state: leave this mapping available to retry. */
        LOG(TNFS,WARN,"R14A DNS unavailable slot=%u error=%d elapsed_us=%lu retry=next-access\r\n",
            slot,(int)e,(unsigned long)(time_us_32()-dns_started));
        return -2;
    }
    cyw43_arch_lwip_begin();s->pcb=udp_new();
    if(s->pcb){r35_resources[slot].pcb_alloc++;udp_recv(s->pcb,r13_udp,s);}
    else r35_resources[slot].pcb_fail++;
    cyw43_arch_lwip_end();if(!s->pcb)return r13_bad();
    uint8_t b[80]={1,2};const char *root=config.mount_path[0]?config.mount_path:"/";
    unsigned n=strlen(root)+1;if(n+4>sizeof(b))return r13_bad();
    memcpy(b+2,root,n);b[2+n]=b[3+n]=0;
    int rc=r13_rpc(0,b,n+4);
    if(rc==0){
        if(s->received!=9)return r13_bad();
        s->sid=r13_le16(s->rx);s->retry_ms=r13_le16(s->rx+7);s->mounted=true;
        LOG(TNFS,DEBUG,"R14A SESSION slot=%u sid=%04x server_min_ms=%u retry_ms=%u\r\n",slot,s->sid,s->retry_ms,s->retry_ms<1000?1000:s->retry_ms);
    }
    else {s->poisoned=true;if(rc>0)rc=-65;}
    LOG_AT(TNFS,rc?SL_ERROR:SL_INFO,"R13 mount slot=%u host=%s result=%d\r\n",slot,config.host,rc);
    return rc;
}
