#include "include/sidetnfs_log.h"
#include <stdio.h>
#include <stdarg.h>
#ifndef SIDETNFS_LOG_CLOCK_MS
#include "pico/time.h"
#define SIDETNFS_LOG_CLOCK_MS() ((uint32_t)(time_us_64()/1000))
#endif
#ifndef SIDETNFS_LOG_PRINTF
#define SIDETNFS_LOG_PRINTF printf
#define SIDETNFS_LOG_VPRINTF vprintf
#endif
static uint8_t thresholds[SC_COUNT]={
 SIDETNFS_LOG_DEFAULT_LEVEL,SIDETNFS_LOG_DEFAULT_LEVEL,SIDETNFS_LOG_DEFAULT_LEVEL,SIDETNFS_LOG_DEFAULT_LEVEL,
 SIDETNFS_LOG_DEFAULT_LEVEL,SIDETNFS_LOG_DEFAULT_LEVEL,SIDETNFS_LOG_DEFAULT_LEVEL,SIDETNFS_LOG_DEFAULT_LEVEL
};
bool sidetnfs_log_enabled(sidetnfs_log_component c,sidetnfs_log_level l){
 return (unsigned)c<SC_COUNT && l>SL_OFF && l<=SIDETNFS_LOG_COMPILED_LEVEL && (unsigned)l<=thresholds[c];
}
bool sidetnfs_log_set_level(sidetnfs_log_component c,sidetnfs_log_level l){
 if((unsigned)c>=SC_COUNT || (unsigned)l>SIDETNFS_LOG_COMPILED_LEVEL)return false;
 thresholds[c]=(uint8_t)l;return true;
}
bool sidetnfs_log_set_all(sidetnfs_log_level l){
 if((unsigned)l>SIDETNFS_LOG_COMPILED_LEVEL)return false;
 for(unsigned c=0;c<SC_COUNT;c++)thresholds[c]=(uint8_t)l;
 return true;
}
void sidetnfs_log_emit(sidetnfs_log_component c,sidetnfs_log_level l,const char *fmt,...){
 static const char *const components[]={"BOOT","GEMDOS","SD","TNFS","WIFI","NTP","SIDEETH","LEGACY"};
 static const char *const levels[]={"OFF","ERROR","WARN","INFO","DEBUG","TRACE"};
 if(!sidetnfs_log_enabled(c,l))return;
 SIDETNFS_LOG_PRINTF("SIDETNFS %lu [%s] %s: ",(unsigned long)SIDETNFS_LOG_CLOCK_MS(),components[c],levels[l]);
 va_list ap;va_start(ap,fmt);SIDETNFS_LOG_VPRINTF(fmt,ap);va_end(ap);
}
void sidetnfs_log_raw(const char *fmt,...){
 if(!sidetnfs_log_enabled(SC_LEGACY,SL_DEBUG))return;
 va_list ap;va_start(ap,fmt);SIDETNFS_LOG_VPRINTF(fmt,ap);va_end(ap);
}
