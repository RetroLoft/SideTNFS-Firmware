/* Read-only R-Type asset stress test. Never executes game code. */
#include <stdint.h>
#include <stddef.h>
extern uint32_t rload_ticks(void);
extern int32_t r03_tos(const uint8_t *,uint32_t);
static uint8_t args[16];
static void w(uint8_t*p,uint16_t v){p[0]=v>>8;p[1]=v;}
static void l(uint8_t*p,uint32_t v){w(p,v>>16);w(p+2,v);}
static void ptr(uint8_t*p,const void*v){l(p,(uint32_t)(uintptr_t)v);}
static int32_t simple(unsigned op){w(args,op);return r03_tos(args,2);}
static int32_t openf(const char*p,unsigned op){w(args,op);ptr(args+2,p);w(args+6,0);return r03_tos(args,8);}
static int32_t closef(int h){w(args,0x3e);w(args+2,h);return r03_tos(args,4);}
static int32_t io(unsigned op,int h,void*p,uint32_t n){w(args,op);w(args+2,h);l(args+4,n);ptr(args+8,p);return r03_tos(args,12);}
static int32_t seekf(int h,unsigned mode){w(args,0x42);l(args+2,0);w(args+6,h);w(args+8,mode);return r03_tos(args,10);}
static void* alloc(uint32_t n){w(args,0x48);l(args+2,n);int32_t r=r03_tos(args,6);return r>0?(void*)(uintptr_t)r:0;}
static int32_t release(void*p){w(args,0x49);ptr(args+2,p);return r03_tos(args,6);}
static void print(const char*p){w(args,9);ptr(args+2,p);r03_tos(args,6);}
struct asset {const char*path;uint32_t size,crc;};
#include "assets.h"
static char root[112],logroot[112],path[160],logname[160],logleaf[]="RLOAD000.LOG",line[512];
static uint32_t table[256],cycles,session_start,cycle_start;
static unsigned used;
static void add(const char*p){while(*p && used<sizeof(line)-1)line[used++]=*p++;line[used]=0;}
static void num(uint32_t n){char s[11];unsigned k=0;do{s[k++]=(char)('0'+n%10);n/=10;}while(n);while(k){char c[2]={s[--k],0};add(c);}}
static void hex(uint32_t n){for(unsigned k=0;k<8;k++){char c[2]={"0123456789ABCDEF"[n>>28],0};add(c);n<<=4;}}
static void begin(const char*p){used=0;add(p);}
/* Unsigned subtraction handles a single 32-bit tick-counter wrap. */
static void duration(uint32_t start,uint32_t end){
 uint32_t ticks=end-start,ms=(ticks%200)*5;
 num(ticks/200);add(".");
 char fraction[4]={(char)('0'+ms/100),(char)('0'+ms/10%10),(char)('0'+ms%10),0};add(fraction);
}
static void timing(uint32_t now){add(" cycle_s=");duration(cycle_start,now);add(" total_s=");duration(session_start,now);}
static int logline(void){int h;int32_t r,c;add("\r\n");print(line);w(args,0x3d);ptr(args+2,logname);w(args+6,1);h=r03_tos(args,8);if(h<0)return 0;
 r=seekf(h,2);if(r>=0)r=io(0x40,h,line,used);c=closef(h);return r==(int32_t)used && c==0;}
/* Use a private DTA; do not overwrite the caller's search buffer. */
static int32_t remount_probe(void){
 uint8_t dta[44];char pattern[160];unsigned i=0;int32_t result;
 w(args,0x2f);void*old=(void*)(uintptr_t)r03_tos(args,2);
 while(root[i]){pattern[i]=root[i];i++;}if(pattern[i-1]!='\\')pattern[i++]='\\';
 pattern[i++]='*';pattern[i++]='.';pattern[i++]='*';pattern[i]=0;
 w(args,0x1a);ptr(args+2,dta);r03_tos(args,6);
 w(args,0x4e);ptr(args+2,pattern);w(args+6,0x37);result=r03_tos(args,8);
 /* Drain this small test-directory search, with a bound for other folders. */
 if(result==0){int32_t next=0;unsigned count=0;while(next==0 && count++<4096)next=simple(0x4f);if(next!=0 && next!=-49)result=next;}
 w(args,0x1a);ptr(args+2,old);r03_tos(args,6);return result;
}
static void wait_recovery(void){uint32_t start=rload_ticks();while((uint32_t)(rload_ticks()-start)<2000){} }
static int fail(const char*stage,int32_t rc){
 /* Capture the original result and timestamps BEFORE recovery work. */
 begin("FAIL cycle=");num(cycles+1);add(" file=");add(path);add(" stage=");add(stage);add(" rc=$");hex((uint32_t)rc);timing(rload_ticks());
 print(line);print("\r\nWaiting 10 seconds before directory probe and failure log write...\r\n");
 wait_recovery();int32_t probe=remount_probe();
 add(" recovery_wait_s=10 probe_rc=$");hex((uint32_t)probe);
 add(" completed_cycles=");num(cycles);add(" failed_cycles=1 attempted_cycles=");num(cycles+1);
 if(!logline())print("FAILURE LOG NOT SAVED - retain this screen/UART. Test stopped.\r\n");
 else print("Failure saved. Test stopped; failed cycle was not retried.\r\n");return 0;
}
static uint32_t crc(const uint8_t*p,uint32_t n){uint32_t c=0xffffffffUL;while(n--)c=table[(c^*p++)&255]^(c>>8);return ~c;}
static int stopped(void){if(!simple(0x0b))return 0;return (simple(7)&255)==27;}
static int load(const struct asset*a){unsigned i=0,j=0;int h;uint8_t*buf;int32_t r,c;uint32_t sum;
 while(root[i]){path[i]=root[i];i++;}if(i && path[i-1]!='\\')path[i++]='\\';while(a->path[j])path[i++]=a->path[j++];path[i]=0;
 buf=alloc(a->size+1);if(!buf)return fail("Malloc",(int32_t)(intptr_t)buf);
 h=openf(path,0x3d);if(h<0){release(buf);return fail("Fopen",h);}
 r=seekf(h,2);if(r!=(int32_t)a->size){closef(h);release(buf);return fail("size",r);}
 r=seekf(h,0);if(r){closef(h);release(buf);return fail("rewind",r);}
 r=io(0x3f,h,buf,a->size);if(r!=(int32_t)a->size){closef(h);release(buf);return fail("Fread/short-read",r);}
 sum=crc(buf,a->size);r=io(0x3f,h,buf+a->size,1);c=closef(h);
 if(r){release(buf);return fail("EOF",r);}if(c){release(buf);return fail("Fclose",c);}
 c=release(buf);if(c)return fail("Mfree",c);
 if(sum!=a->crc)return fail("CRC32",(int32_t)sum);
 return 1;}
static int newlog(void){for(unsigned n=0;n<1000;n++){int h;unsigned i=0,j=0;logleaf[5]='0'+n/100;logleaf[6]='0'+n/10%10;logleaf[7]='0'+n%10;while(logroot[i]){logname[i]=logroot[i];i++;}if(logname[i-1]!='\\')logname[i++]='\\';while(logleaf[j])logname[i++]=logleaf[j++];logname[i]=0;
 h=openf(logname,0x3d);if(h>=0){if(closef(h))return 0;continue;}if(h!=-33)return 0;
 h=openf(logname,0x3c);if(h<0)return 0;return closef(h)==0;}return 0;}
void r03_test_main(void){uint8_t input[114];unsigned i;int stop=0;
 print("RLOAD v4 - R-Type full asset read/CRC loop\r\nNo game execution. ESC stops between files.\r\nChoose a separate log folder; P: or a local HDD is recommended.\r\nR-Type directory [M:\\GAMES\\RTYPE]: ");
 input[0]=110;input[1]=0;w(args,0x0a);ptr(args+2,input);r03_tos(args,6);print("\r\n");
 if(input[1]){for(i=0;i<input[1];i++)root[i]=input[i+2];root[i]=0;}else{const char*p="M:\\GAMES\\RTYPE";for(i=0;p[i];i++)root[i]=p[i];root[i]=0;}
 if(root[1]!=':' || root[2]!='\\'){print("Use an absolute Atari path, e.g. O:\\GAMES\\RTYPE\r\n");return;}
 print("Log directory [P:\\]: ");input[0]=110;input[1]=0;w(args,0x0a);ptr(args+2,input);r03_tos(args,6);print("\r\n");
 if(input[1]){for(i=0;i<input[1];i++)logroot[i]=input[i+2];logroot[i]=0;}else{logroot[0]='P';logroot[1]=':';logroot[2]='\\';logroot[3]=0;}
 if(logroot[1]!=':' || logroot[2]!='\\'){print("Use an existing absolute log directory, e.g. P:\\ or C:\\TESTS\r\n");return;}
 if(!newlog()){print("Cannot create a fresh RLOADnnn.LOG in the selected log folder. Test not started.\r\n");return;}
 for(i=0;i<256;i++){uint32_t c=i;for(unsigned j=0;j<8;j++)c=(c>>1)^((c&1)?0xedb88320UL:0);table[i]=c;}
 begin("START RLOAD v4 source=");add(root);add(" log=");add(logname);add(" files=6 bytes=436571 expected=installation-2026-10-02");if(!logline()){print("Log failure; stopped.\r\n");return;}
 session_start=rload_ticks();
 while(!stop){cycle_start=rload_ticks();for(i=0;i<sizeof(assets)/sizeof(assets[0]);i++){if(stopped()){stop=1;break;}if(!load(&assets[i]))return;}
 if(!stop){uint32_t end=rload_ticks();cycles++;begin("PASS cycle=");num(cycles);add(" files=6 bytes=436571 CRC32=OK");timing(end);if(!logline()){print("Log failure; stopped.\r\n");return;}}}
 begin("STOP completed_cycles=");num(cycles);add(" total_s=");duration(session_start,rload_ticks());if(!logline())print("Log failure on stop.\r\n");
}
