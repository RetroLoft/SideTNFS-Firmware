/* Cooperative Wi-Fi lifecycle. One transition/action per tick, no wait loops.
 * Caller services SD first and continues doing so in every network state.
 * Time values are wrapping milliseconds; all intervals are < INT32_MAX. */
#include <stdint.h>
#include <stdbool.h>
enum {R11_IDLE,R11_JOINING,R11_UP,R11_BACKOFF,R11_OFFLINE,R11_DISABLED};
typedef struct {uint8_t state,attempts;uint32_t deadline,next_status;} r11_coop;
typedef struct {
 void *context;
 int (*begin)(void *); /* enqueue asynchronous join; 0 success */
 void (*poll)(void *);
 int (*link)(void *); /* 1 IPv4 ready, 0 pending/down, -1 terminal failure */
 void (*leave)(void *);
 void (*event)(void *,unsigned,unsigned,int);
} r11_ops;
static bool r11_due(uint32_t now,uint32_t deadline){return (int32_t)(now-deadline)>=0;}
static void r11_change(r11_coop *s,r11_ops *o,unsigned state,int reason){s->state=(uint8_t)state;o->event(o->context,state,s->attempts,reason);}
static void r11_failed(r11_coop *s,r11_ops *o,uint32_t now,int reason){
 o->leave(o->context);
 s->deadline=now+(s->attempts<3?5000u:60000u);
 r11_change(s,o,s->attempts<3?R11_BACKOFF:R11_OFFLINE,reason);
}
static void r11_tick(r11_coop *s,r11_ops *o,uint32_t now,bool sd_mounted,bool configured){
 if(s->state==R11_DISABLED)return;
 if(s->state==R11_OFFLINE){
  o->poll(o->context);
  if(!sd_mounted || !configured || !r11_due(now,s->deadline))return;
  /* Fresh bounded episode after cooldown; never reset file/session state. */
  s->attempts=0;s->deadline=now;r11_change(s,o,R11_BACKOFF,-4);return;
 }
 if(s->state==R11_IDLE){
  if(!configured){r11_change(s,o,R11_DISABLED,0);return;}
  if(!sd_mounted)return; /* publish and mount SD before starting Wi-Fi */
 }
 if(s->state==R11_IDLE || s->state==R11_BACKOFF){
  if(s->state==R11_BACKOFF && !r11_due(now,s->deadline)){o->poll(o->context);return;}
  s->attempts++;int result=o->begin(o->context);
  if(result){r11_failed(s,o,now,result);return;}
  s->deadline=now+20000;s->next_status=now;
  r11_change(s,o,R11_JOINING,0);return;
 }
 o->poll(o->context);
 if(s->state==R11_JOINING && r11_due(now,s->deadline)){r11_failed(s,o,now,-2);return;}
 if(!r11_due(now,s->next_status))return;
 s->next_status=now+100;
 int link=o->link(o->context);
 if(s->state==R11_JOINING){
  if(link==1){r11_change(s,o,R11_UP,0);return;}
  if(link<0 || r11_due(now,s->deadline))r11_failed(s,o,now,link<0?-1:-2);
 }else if(s->state==R11_UP && link!=1){
  /* Retry loss of a working link after backoff; each episode is bounded. */
  s->attempts=0;r11_failed(s,o,now,-3);
 }
}
