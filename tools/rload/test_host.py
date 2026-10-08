"""Compile the actual file-checking routine against fault-injecting GEMDOS mocks."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parent
src=(root/'rload.c').read_text()
load=src[src.index('static int load('):src.index('static int newlog(')]
crc=src[src.index('static uint32_t crc('):src.index('static int stopped(')]
code=r'''
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
struct asset {const char*path;uint32_t size,crc;};
static char root[]="M:\\GAMES\\RTYPE",path[160];
static uint32_t table[256];
static unsigned mode,live,opened,reads;static const char*stage;
static uint8_t payload[4097];
static void *alloc(uint32_t n){if(mode==1)return 0;live++;return malloc(n);}
static int32_t release(void*p){assert(live);live--;free(p);return mode==9?-40:0;}
static int openf(const char*p,unsigned op){assert(op==0x3d);assert(!strcmp(p,"M:\\GAMES\\RTYPE\\FILES\\RRD"));if(mode==2)return -33;opened++;return 5;}
static int32_t closef(int h){assert(h==5 && opened);opened--;return mode==8?-36:0;}
static int32_t seekf(int h,unsigned m){assert(h==5);return m==2?(mode==3?4096:4097):(mode==4?-65:0);}
static int32_t io(unsigned op,int h,void*p,uint32_t n){assert(op==0x3f && h==5);reads++;if(n==1)return mode==7?1:0;assert(n==4097);memcpy(p,payload,n);if(mode==6)((uint8_t*)p)[4000]^=1;return mode==5?4096:4097;}
static int fail(const char*s,int32_t rc){(void)rc;stage=s;return 0;}
'''+crc+load+r'''
int main(void){for(unsigned i=0;i<256;i++){uint32_t c=i;for(unsigned j=0;j<8;j++)c=c>>1^((c&1)?0xedb88320UL:0);table[i]=c;}assert(crc((uint8_t*)"123456789",9)==0xcbf43926UL);
for(unsigned i=0;i<sizeof(payload);i++)payload[i]=(uint8_t)(i*31);
struct asset a={"FILES\\RRD",4097,crc(payload,4097)};
const char*expected[]={0,"Malloc","Fopen","size","rewind","Fread/short-read","CRC32","EOF","Fclose","Mfree"};
for(mode=0;mode<10;mode++){stage=0;reads=0;int ok=load(&a);assert(ok==(mode==0));assert(!live && !opened);if(mode)assert(!strcmp(stage,expected[mode]));else assert(reads==2);}
puts("PASS: CRC known vector, successful read, allocation/open/size/seek/short-read/corruption/EOF/close/free failures; no leaked buffers or handles.");}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(code)
 subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
