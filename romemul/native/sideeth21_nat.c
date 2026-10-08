/* F21 TCP/UDP/echo NAT for a single Atari. No DHCP/DNS proxy, no sockets,
 * no waits. WiFi output and Ethernet delivery happen only in poll().
 * IPv4 fragments and ICMP errors are deliberately unsupported. */
#include "sideeth21_nat.h"
#include <stddef.h>
#include <string.h>
#ifdef S21_HOST
#include "sideeth21_host.h"
#else
#include "lwip/opt.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/memp.h"
#include "lwip/sys.h"
#endif
#define S21_MTU 576u
#define S21_FRAME 590u
#define S21_MAPS 16u
#define S21_PER_PROTO 8u
#define S21_QUEUE 4u
#define S21_FIRST_PORT 40000u
#define S21_LAST_PORT 40959u
#define S21_TCP 6u
#define S21_UDP 17u
#define S21_ICMP 1u
/* ICMP IDs cannot be reserved through a PCB. Firmware RAW is disabled,
 * so it has no application ICMP echo clients competing for these IDs. */
#if LWIP_RAW
#error "F21 ICMP ID ownership requires firmware LWIP_RAW=0"
#endif
#if defined(PICO_CYW43_ARCH_POLL) && !PICO_CYW43_ARCH_POLL
#error "F21 NAT requires serialized polling lwIP"
#endif
#if MEMP_MEM_MALLOC
#error "F21 TCP reservation preflight requires fixed lwIP MEMP pools"
#endif
_Static_assert(S21_LAST_PORT<49152u,"NAT must stay outside lwIP ephemeral defaults");
struct mapping {
    bool used;
    uint8_t proto,mac[6],fin;
    bool closed;
    uint16_t local,remote,port;
    uint32_t destination,last;
    struct tcp_pcb *tcp;
    struct udp_pcb *udp;
};
struct packet {uint16_t len;uint8_t bytes[S21_FRAME];};
struct parsed {unsigned h,total,l4,proto;uint16_t source,dest;uint32_t src,dst;};
static struct mapping maps[S21_MAPS];
static struct packet txq[S21_QUEUE],rxq[S21_QUEUE];
static unsigned txhead,txtail,txcount,rxhead,rxtail,rxcount;
static struct netif *wifi;
static uint32_t wifi_ip;
static bool enabled;
static uint16_t next_port;
static struct s21_nat_stats stats;
static const uint8_t gateway_mac[6]={2,0x53,0x49,0x44,0x45,1};
static uint16_t be16(const uint8_t *p){return (uint16_t)((p[0]<<8)|p[1]);}
static uint32_t be32(const uint8_t *p){return ((uint32_t)be16(p)<<16)|be16(p+2);}
static void put16(uint8_t *p,uint16_t v){p[0]=(uint8_t)(v>>8);p[1]=(uint8_t)v;}
static void put32(uint8_t *p,uint32_t v){put16(p,(uint16_t)(v>>16));put16(p+2,(uint16_t)v);}
static uint32_t sum_bytes(uint32_t sum,const uint8_t *p,unsigned n){
    while(n>1){sum+=be16(p);p+=2;n-=2;}if(n)sum+=(uint16_t)*p<<8;return sum;
}
static uint16_t finish_sum(uint32_t sum){while(sum>>16)sum=(sum&65535u)+(sum>>16);return (uint16_t)~sum;}
static uint16_t checksum(const uint8_t *p,unsigned n){return finish_sum(sum_bytes(0,p,n));}
static uint16_t transport_sum(const uint8_t *p,const struct parsed *v){
    uint32_t sum=sum_bytes(0,p+12,8);sum+=v->proto+v->l4;
    return finish_sum(sum_bytes(sum,p+v->h,v->l4));
}
/* Work on byte arrays: no packed/unaligned accesses and no dependence on
 * whether incoming pbuf headers span multiple segments. */
static bool parse_ip(const uint8_t *p,unsigned available,struct parsed *v){
    if(available<20 || (p[0]>>4)!=4)return false;
    v->h=(p[0]&15u)*4u;v->total=be16(p+2);
    if(v->h<20 || v->h>available || v->total<v->h || v->total>available ||
       (be16(p+6)&0xbfffu) || checksum(p,v->h))return false;
    v->proto=p[9];v->l4=v->total-v->h;v->src=be32(p+12);v->dst=be32(p+16);
    const uint8_t *l4=p+v->h;
    if(v->proto==S21_TCP){
        if(v->l4<20)return false;
        unsigned h=(l4[12]>>4)*4u;if(h<20 || h>v->l4)return false;
        for(unsigned i=20;i<h;){
            unsigned kind=l4[i];if(kind==0)break;if(kind==1){i++;continue;}
            if(i+1>=h || l4[i+1]<2 || l4[i+1]>h-i)return false;
            if(kind==2 && l4[i+1]!=4)return false;i+=l4[i+1];
        }
        if(transport_sum(p,v))return false;
    }else if(v->proto==S21_UDP){
        if(v->l4<8 || be16(l4+4)!=v->l4)return false;
        if(be16(l4+6) && transport_sum(p,v))return false;
    }else if(v->proto==S21_ICMP){
        if(v->l4<8 || l4[1] || (l4[0]!=0 && l4[0]!=8) || checksum(l4,v->l4))return false;
    }else return false;
    v->source=be16(l4+(v->proto==S21_ICMP?4:0));
    v->dest=v->proto==S21_ICMP?0:be16(l4+2);return true;
}
static void clamp_mss(uint8_t *p,const struct parsed *v){
    if(v->proto!=S21_TCP)return;
    uint8_t *tcp=p+v->h;if(!(tcp[13]&2))return;
    unsigned h=(tcp[12]>>4)*4u;
    for(unsigned i=20;i<h;){unsigned k=tcp[i];if(!k)break;if(k==1){i++;continue;}
        if(k==2 && be16(tcp+i+2)>536)put16(tcp+i+2,536);i+=tcp[i+1];}
}
static void rechecksum(uint8_t *p,const struct parsed *v){
    put16(p+10,0);put16(p+10,checksum(p,v->h));
    uint8_t *l4=p+v->h;unsigned offset=v->proto==S21_TCP?16:v->proto==S21_UDP?6:2;
    put16(l4+offset,0);
    uint16_t sum=v->proto==S21_ICMP?checksum(l4,v->l4):transport_sum(p,v);
    put16(l4+offset,(!sum && v->proto==S21_UDP)?65535:sum);
}
static void release(struct mapping *m){
    if(m->tcp)tcp_abort(m->tcp);if(m->udp)udp_remove(m->udp);
    memset(m,0,sizeof(*m));
}
static void flush(void){
    for(unsigned i=0;i<S21_MAPS;i++)release(&maps[i]);
    txhead=txtail=txcount=rxhead=rxtail=rxcount=0;
}
void s21_nat_init(void){flush();memset(&stats,0,sizeof(stats));wifi=NULL;wifi_ip=0;enabled=false;next_port=S21_FIRST_PORT;}
static bool link_up(struct netif *n){return n && netif_is_up(n) && netif_is_link_up(n) && !ip4_addr_isany_val(*netif_ip4_addr(n));}
void s21_nat_set_link(struct netif *n,bool on){
    uint32_t ip=link_up(n)?lwip_ntohl(ip4_addr_get_u32(netif_ip4_addr(n))):0;
    bool usable=on && ip!=0;
    if(wifi!=n || wifi_ip!=ip || enabled!=usable)flush();
    wifi=n;wifi_ip=ip;enabled=usable;
}
static void gc(void){
    uint32_t now=sys_now();
    for(unsigned i=0;i<S21_MAPS;i++)if(maps[i].used){
        uint32_t limit=maps[i].proto==S21_TCP?(maps[i].closed?10000u:300000u):maps[i].proto==S21_UDP?60000u:10000u;
        if((uint32_t)(now-maps[i].last)>limit){release(&maps[i]);stats.expired++;}
    }
}
static struct mapping *outbound(const struct parsed *v,const uint8_t mac[6]){
    for(unsigned i=0;i<S21_MAPS;i++){struct mapping *m=&maps[i];
        if(m->used && m->proto==v->proto && m->local==v->source && m->destination==v->dst &&
           m->remote==v->dest && !memcmp(m->mac,mac,6))return m;
    }return NULL;
}
static bool owned(uint8_t proto,uint16_t port){
    for(unsigned i=0;i<S21_MAPS;i++)if(maps[i].used && maps[i].proto==proto && maps[i].port==port)return true;
    return false;
}
static struct mapping *allocate(const struct parsed *v,const uint8_t mac[6]){
    gc();unsigned count=0;struct mapping *m=NULL,*closed=NULL;
    for(unsigned i=0;i<S21_MAPS;i++){
        if(maps[i].used && maps[i].proto==v->proto)count++;
        if(!maps[i].used && !m)m=&maps[i];
        if(maps[i].used && maps[i].closed && maps[i].proto==v->proto &&
           (!closed || (uint32_t)(sys_now()-maps[i].last)>(uint32_t)(sys_now()-closed->last)))closed=&maps[i];
    }
    /* Preserve FIN/ACK retransmissions for a short grace, but never let
     * completed HTTP/1.0 connections exhaust CAB's next connection. */
    if(count>=S21_PER_PROTO || !m){
        if(!closed)return NULL;
        release(closed);m=closed;
    }
    if(v->proto==S21_TCP){
        /* tcp_new() kills unrelated TCP connections when its pool is full.
         * Probe the fixed pool without invoking that recovery path. The
         * polling architecture cannot run another lwIP allocation between
         * returning this cell and tcp_new() immediately taking it again. */
        void *probe=memp_malloc(MEMP_TCP_PCB);
        if(!probe)return NULL;
        memp_free(MEMP_TCP_PCB,probe);
        m->tcp=tcp_new();if(!m->tcp)return NULL;
    }
    if(v->proto==S21_UDP){m->udp=udp_new();if(!m->udp)return NULL;}
    bool bound=false;
    for(unsigned tries=0;tries<=S21_LAST_PORT-S21_FIRST_PORT;tries++){
        uint16_t port=next_port++;if(next_port>S21_LAST_PORT)next_port=S21_FIRST_PORT;
        if(owned((uint8_t)v->proto,port))continue;
        err_t err=ERR_OK;
        if(m->tcp)err=tcp_bind(m->tcp,IP_ADDR_ANY,port);
        if(m->udp)err=udp_bind(m->udp,IP_ADDR_ANY,port);
        if(err==ERR_OK){m->port=port;bound=true;break;}
        if(err!=ERR_USE)break;
    }
    if(!bound){release(m);return NULL;}
    m->used=true;m->proto=(uint8_t)v->proto;m->local=v->source;m->remote=v->dest;
    m->destination=v->dst;memcpy(m->mac,mac,6);m->last=sys_now();return m;
}
static void tcp_activity(struct mapping *m,const uint8_t *ip,const struct parsed *v,bool incoming){
    if(v->proto!=S21_TCP)return;
    uint8_t flags=ip[v->h+13];
    if(flags&1)m->fin|=incoming?2:1;
    if((flags&4) || m->fin==3)m->closed=true;
}
uint16_t s21_nat_tx(const uint8_t *frame,uint16_t len){
    if(!enabled || !link_up(wifi)){stats.tx_dropped++;return 7;}
    if(!frame || len<34 || len>S21_FRAME || be16(frame+12)!=0x0800){stats.invalid++;return 6;}
    struct parsed v;const uint8_t *ip=frame+14;
    if(!parse_ip(ip,len-14,&v) || v.src!=0xc0a8fe02u || (frame[6]&1) || ip[8]<=1){stats.invalid++;return 6;}
    if(v.dst==wifi_ip || (v.dst&0xffffff00u)==0xc0a8fe00u || (v.dst>>24)==0 ||
       (v.dst>>24)==127 || (v.dst>>28)>=14){stats.tx_dropped++;return 6;}
    if(v.proto==S21_ICMP && ip[v.h]!=8){stats.invalid++;return 6;}
    if(txcount==S21_QUEUE){stats.tx_dropped++;return 5;}
    struct mapping *m=outbound(&v,frame+6);
    bool new_syn=v.proto==S21_TCP && (ip[v.h+13]&0x12u)==2;
    if(m && m->closed && new_syn){release(m);m=NULL;}
    /* Late FIN/ACK traffic for a reclaimed connection must not create
     * another reservation or a new public tuple. */
    if(!m && v.proto==S21_TCP && !new_syn){stats.tx_dropped++;return 7;}
    if(!m)m=allocate(&v,frame+6);
    if(!m){stats.tx_dropped++;return 5;}
    struct packet *q=&txq[txhead];q->len=(uint16_t)v.total;memcpy(q->bytes,ip,v.total);
    put32(q->bytes+12,wifi_ip);q->bytes[8]--;put16(q->bytes+v.h+(v.proto==S21_ICMP?4:0),m->port);
    clamp_mss(q->bytes,&v);rechecksum(q->bytes,&v);m->last=sys_now();tcp_activity(m,ip,&v,false);
    txhead=(txhead+1)%S21_QUEUE;txcount++;stats.tx_packets++;return 0;
}
/* A minimal safe extraction determines whether this is a reserved flow.
 * Never touch p->payload directly: hook executes before lwIP validation. */
int s21_nat_ip4_input(struct pbuf *p,struct netif *inp){
    if(!enabled || inp!=wifi || !p)return 0;
    uint8_t ip[S21_MTU];unsigned copy=p->tot_len<S21_MTU?p->tot_len:S21_MTU;
    if(copy<20 || pbuf_copy_partial(p,ip,(uint16_t)copy,0)!=copy || (ip[0]>>4)!=4)return 0;
    unsigned h=(ip[0]&15u)*4u;if(h<20 || h>copy || be32(ip+16)!=wifi_ip)return 0;
    unsigned proto=ip[9];uint16_t port;
    /* Nonfirst fragments have no usable ports. Let normal lwIP own them;
     * first fragments of NAT flows are consumed below, so cannot assemble. */
    if(be16(ip+6)&0x1fffu)return 0;
    if(proto==S21_TCP || proto==S21_UDP){if(copy<h+4)return 0;port=be16(ip+h+2);}
    else if(proto==S21_ICMP){if(copy<h+8 || ip[h]!=0)return 0;port=be16(ip+h+4);}
    else return 0;
    if(!owned((uint8_t)proto,port))return 0;
    struct parsed v;struct mapping *match=NULL;
    if(parse_ip(ip,copy,&v) && v.total<=S21_MTU && ip[8]>1){
        for(unsigned i=0;i<S21_MAPS;i++){struct mapping *m=&maps[i];
            if(m->used && m->proto==proto && m->port==port && m->destination==v.src &&
               (proto==S21_ICMP || m->remote==v.source)){match=m;break;}
        }
    }
    if(!match || rxcount==S21_QUEUE){pbuf_free(p);stats.rx_dropped++;return 1;}
    struct packet *q=&rxq[rxhead];q->len=(uint16_t)(14+v.total);
    memcpy(q->bytes,match->mac,6);memcpy(q->bytes+6,gateway_mac,6);put16(q->bytes+12,0x0800);
    uint8_t *out=q->bytes+14;memcpy(out,ip,v.total);put32(out+16,0xc0a8fe02u);out[8]--;
    put16(out+v.h+(proto==S21_ICMP?4:2),match->local);clamp_mss(out,&v);rechecksum(out,&v);
    match->last=sys_now();tcp_activity(match,ip,&v,true);
    rxhead=(rxhead+1)%S21_QUEUE;rxcount++;stats.rx_packets++;pbuf_free(p);return 1;
}
void s21_nat_poll(void){
    if(!enabled)return;
    if(!link_up(wifi) || lwip_ntohl(ip4_addr_get_u32(netif_ip4_addr(wifi)))!=wifi_ip){s21_nat_set_link(NULL,false);return;}
    /* One output per poll bounds lwIP work and gives native TNFS priority. */
    if(txcount){struct packet *q=&txq[txtail];
        struct pbuf *p=pbuf_alloc(PBUF_LINK,q->len,PBUF_RAM);
        if(p){
            err_t err=pbuf_take(p,q->bytes,q->len);
            if(err==ERR_OK){ip4_addr_t dst;ip4_addr_set_u32(&dst,lwip_htonl(be32(q->bytes+16)));
                err=wifi->output?wifi->output(wifi,p,&dst):ERR_IF;}
            pbuf_free(p);if(err!=ERR_OK)stats.tx_dropped++;
        }else stats.tx_dropped++;
        txtail=(txtail+1)%S21_QUEUE;txcount--;
    }
    if(rxcount){struct packet *q=&rxq[rxtail];
        if(!s21_nat_deliver(q->bytes,q->len)){rxtail=(rxtail+1)%S21_QUEUE;rxcount--;}
    }
    gc();
}
const struct s21_nat_stats *s21_nat_get_stats(void){
    stats.mappings=0;for(unsigned i=0;i<S21_MAPS;i++)stats.mappings+=maps[i].used?1:0;
    stats.tx_pending=(uint16_t)txcount;stats.rx_pending=(uint16_t)rxcount;return &stats;
}
