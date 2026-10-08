#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
static char captured[2048];static unsigned used;
static int capture_vprintf(const char *fmt,va_list ap){int n=vsnprintf(captured+used,sizeof(captured)-used,fmt,ap);assert(n>=0 && (unsigned)n<sizeof(captured)-used);used+=(unsigned)n;return n;}
static int capture_printf(const char *fmt,...){va_list ap;va_start(ap,fmt);int n=capture_vprintf(fmt,ap);va_end(ap);return n;}
#define SIDETNFS_LOG_CLOCK_MS() 1234u
#define SIDETNFS_LOG_PRINTF capture_printf
#define SIDETNFS_LOG_VPRINTF capture_vprintf
#include "sidetnfs_log.c"
#define _DEBUG 1
#include "include/debug.h"
int main(void){
#if SIDETNFS_LOG_COMPILED_LEVEL == 0
 unsigned effects=0;
 LOG(TNFS,ERROR,"compiled off %u",++effects);DPRINTF("off %u",++effects);DPRINTFRAW("off %u",++effects);
 assert(!effects && !used && !sidetnfs_log_set_all(SL_INFO));
 assert(sidetnfs_log_set_all(SL_OFF));
 puts("Logger: OFF compile ceiling suppresses all calls and arguments.");
 return 0;
#else
 assert(sidetnfs_log_result_level(508)==SL_TRACE);
 assert(sidetnfs_log_result_level(-33)==SL_DEBUG && sidetnfs_log_result_level(-49)==SL_DEBUG);
 assert(sidetnfs_log_result_level(-65)==SL_ERROR && sidetnfs_log_result_level(-36)==SL_ERROR);
 unsigned effects=0;
 LOG(TNFS,TRACE,"hidden %u\n",++effects);assert(effects==0 && !used);
 LOG(SD,INFO,"ready\n");assert(!strcmp(captured,"SIDETNFS 1234 [SD] INFO: ready\n"));
 assert(sidetnfs_log_set_level(SC_SD,SL_OFF));LOG(SD,ERROR,"off %u\n",++effects);assert(!effects);
 assert(!sidetnfs_log_set_level(SC_COUNT,SL_INFO));assert(!sidetnfs_log_set_level(SC_TNFS,(sidetnfs_log_level)99));
 assert(!sidetnfs_log_enabled((sidetnfs_log_component)-1,SL_INFO));assert(!sidetnfs_log_enabled(SC_TNFS,SL_OFF));
 assert(!sidetnfs_log_set_all((sidetnfs_log_level)99));
 used=0;captured[0]=0;DPRINTF("hidden %u\n",++effects);assert(!effects && !used);
#if SIDETNFS_LOG_COMPILED_LEVEL >= 5
 assert(sidetnfs_log_set_level(SC_TNFS,SL_TRACE));LOG(TNFS,TRACE,"packet %u\n",++effects);assert(effects==1);
 assert(strstr(captured,"[TNFS] TRACE: packet 1"));LOG(SD,ERROR,"still off %u\n",++effects);assert(effects==1);
 assert(sidetnfs_log_set_level(SC_LEGACY,SL_DEBUG));DPRINTF("legacy %u",++effects);DPRINTFRAW(" tail\n");
 assert(effects==2 && strstr(captured,"[LEGACY] DEBUG:") && strstr(captured,"legacy 2 tail\n"));
#else
 assert(!sidetnfs_log_set_level(SC_TNFS,SL_TRACE));assert(!sidetnfs_log_set_level(SC_LEGACY,SL_DEBUG));
 LOG(TNFS,TRACE,"compiled out %u\n",++effects);DPRINTFRAW("hidden %u",++effects);assert(!effects);
#endif
 assert(sidetnfs_log_set_all(SL_OFF));unsigned before=effects;
 LOG(TNFS,ERROR,"off %u\n",++effects);DPRINTF("off %u",++effects);DPRINTFRAW("off %u",++effects);assert(effects==before);
 puts("Logger: component isolation, OFF, severity filtering, argument suppression, invalid settings, format, compile ceiling and DPRINTF compatibility pass.");
#endif
}
