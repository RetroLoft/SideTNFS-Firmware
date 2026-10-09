#ifndef SIDETNFS_LOG_H
#define SIDETNFS_LOG_H
#include <stdbool.h>
#include <stdint.h>
typedef enum { SL_OFF,SL_ERROR,SL_WARN,SL_INFO,SL_DEBUG,SL_TRACE } sidetnfs_log_level;
typedef enum { SC_BOOT,SC_GEMDOS,SC_SD,SC_TNFS,SC_WIFI,SC_NTP,SC_SIDEETH,SC_LEGACY,SC_COUNT } sidetnfs_log_component;
#ifndef SIDETNFS_LOG_COMPILED_LEVEL
#define SIDETNFS_LOG_COMPILED_LEVEL 5
#endif
#ifndef SIDETNFS_LOG_DEFAULT_LEVEL
#define SIDETNFS_LOG_DEFAULT_LEVEL 3
#endif
#if SIDETNFS_LOG_COMPILED_LEVEL < 0 || SIDETNFS_LOG_COMPILED_LEVEL > 5 || SIDETNFS_LOG_DEFAULT_LEVEL < 0 || SIDETNFS_LOG_DEFAULT_LEVEL > SIDETNFS_LOG_COMPILED_LEVEL
#error Invalid SIDETNFS logging levels
#endif
/* Expected filesystem probes and EOF are diagnostic, not faults. */
static inline sidetnfs_log_level sidetnfs_log_result_level(int32_t r){
 if(r>=0)return SL_TRACE;
 if(r==-33 || r==-34 || r==-49 || r==-32)return SL_DEBUG;
 return SL_ERROR;
}
bool sidetnfs_log_enabled(sidetnfs_log_component component,sidetnfs_log_level level);
bool sidetnfs_log_set_level(sidetnfs_log_component component,sidetnfs_log_level level);
bool sidetnfs_log_set_all(sidetnfs_log_level level);
void sidetnfs_log_emit(sidetnfs_log_component component,sidetnfs_log_level level,const char *fmt,...) __attribute__((format(printf,3,4)));
void sidetnfs_log_raw(const char *fmt,...) __attribute__((format(printf,1,2)));
/* Gating is outside the call: suppressed arguments are not evaluated. */
#define LOG_AT(component,level,...) do { \
    sidetnfs_log_level sl_level_=(level); \
    if(sl_level_>SL_OFF && sl_level_<=SIDETNFS_LOG_COMPILED_LEVEL && sidetnfs_log_enabled(SC_##component,sl_level_)) \
        sidetnfs_log_emit(SC_##component,sl_level_,__VA_ARGS__); \
} while(0)
#define LOG(component,level,...) LOG_AT(component,SL_##level,__VA_ARGS__)
#endif
