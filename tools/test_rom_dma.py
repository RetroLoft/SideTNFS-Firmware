"""Compile the actual copy macro against a deterministic DMA adapter."""
from pathlib import Path
import os,tempfile,subprocess
r=Path(__file__).resolve().parent
source=(r.parent/'romemul/include/memfunc.h').read_text()
macro=source[source.index('#define COPY_FIRMWARE_TO_RAM_DMA'):source.rindex('#endif')]
macro=macro.replace('extern uint16_t __rom_in_ram_start__;','/* test linker storage is dst */')
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
typedef unsigned uint;typedef int dma_channel_config;
static uint16_t src[32768],dst[32770];
static struct {uint32_t stat,stream_fifo,stream_addr,stream_ctr;} xip={1,0,0,0},*xip_ctrl_hw=&xip;
#define XIP_STAT_FIFO_EMPTY 1
#define XIP_AUX_BASE 0
#define DREQ_XIP_STREAM 0
#define __rom_in_ram_start__ dst[0]
static uint dma_claim_unused_channel(bool b){assert(b);return 0;}
static dma_channel_config dma_channel_get_default_config(uint c){(void)c;return 0;}
static void channel_config_set_read_increment(dma_channel_config *c,bool b){(void)c;assert(!b);}
static void channel_config_set_write_increment(dma_channel_config *c,bool b){(void)c;assert(b);}
static void channel_config_set_dreq(dma_channel_config *c,int n){(void)c;(void)n;}
static void dma_channel_configure(uint c,dma_channel_config *cfg,void *to,const void *from,unsigned n,bool go){(void)c;(void)cfg;(void)from;assert(go);memcpy(to,src,n*4);}
static bool dma_channel_is_busy(uint c){(void)c;return false;}
static void tight_loop_contents(void){}
'''+macro+r'''
static void check(unsigned words){
 memset(dst,0xa5,sizeof(dst));COPY_FIRMWARE_TO_RAM_DMA(src,words);
 assert(!memcmp(dst,src,words*2));assert(dst[words]==0xa5a5);
}
int main(int argc,char **argv){
 for(unsigned i=0;i<32768;i++)src[i]=(uint16_t)(i^0x1357);
 check(2);check(3);check(4);check(5);check(32767);check(32768);
 assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);unsigned n=(unsigned)fread(src,1,sizeof(src),f);fclose(f);
 assert(n>0 && n<=sizeof(src) && n%2==0);check(n/2);assert(dst[n/2-1]==src[n/2-1]);
 puts("Actual DMA macro: even/odd word counts, maximum ROM size, destination guard and complete native ROM pass.");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(code)
 subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror','-Wno-pointer-to-int-cast',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test'),str(Path(os.environ.get('SIDETNFS_GUEST_DIR',r.parent.parent/'SideTNFS-Gemdrive-integration'))/'dist/native/native.bin')],check=True)
