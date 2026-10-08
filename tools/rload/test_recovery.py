from pathlib import Path
import subprocess,tempfile
s=Path(__file__).with_name('rload.c').read_text()
helpers=s[s.index('static void add('):s.index('static int logline(')]
recovery=s[s.index('static void wait_recovery('):s.index('static uint32_t crc(')]
code=r'''
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
static char line[512],path[]="M:\\GAMES\\RTYPE\\FILES\\RRD";static unsigned used;
static uint32_t cycles=16,session_start=0,cycle_start=4000,tick=5000,probe_tick;
static int probe_count,write_count,write_ok=1;static int32_t probe_rc;
static uint32_t rload_ticks(void){return tick++;}
static void print(const char*p){(void)p;}
static int32_t remount_probe(void){probe_count++;probe_tick=tick;assert(write_count==0);return probe_rc;}
static int logline(void){write_count++;assert(probe_count==1);return write_ok;}
'''+helpers+recovery+r'''
int main(void){assert(fail("Fread/short-read",-2)==0);assert(probe_tick>=7001);assert(write_count==1);assert(cycles==16);assert(strstr(line,"FAIL cycle=17"));assert(strstr(line,"rc=$FFFFFFFE cycle_s=5.000 total_s=25.000"));assert(strstr(line,"probe_rc=$00000000"));assert(strstr(line,"failed_cycles=1 attempted_cycles=17"));
probe_count=write_count=0;probe_rc=-2;write_ok=0;tick=5000;assert(fail("Fread/short-read",-2)==0);assert(write_count==1);assert(strstr(line,"probe_rc=$FFFFFFFE"));assert(cycles==16);
tick=0xfffffff0U;wait_recovery();assert(tick==1985);puts("PASS: full 10-second wait, wraparound, probe before write, original failure/time retained, counters unchanged, failed remount/log bounded.");}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(code);subprocess.run(['cc','-std=c99',str(p/'test.c'),'-o',str(p/'test')],check=True);subprocess.run([str(p/'test')],check=True)
