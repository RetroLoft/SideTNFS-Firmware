"""Exercise existing parser through R12S adapter with utility-style packets."""
from pathlib import Path
import os,subprocess,tempfile
r=Path(__file__).resolve().parent
h=(r.parent/'romemul/native/config_protocol.h').read_text().replace('#include "include/debug.h"','').replace('#include "include/constants.h"','')
s=(r.parent/'romemul/native/config_protocol.c').read_text().replace('#include "config_protocol.h"','').replace('inline ','')
test=r'''
#include "wire.h"
#include "config_wire.h"
#include "sideeth20.h"
#include "cart_wire.h"
static r02_wire recovery;
static unsigned seen,expected_command,expected_words;
static int token_test;
static uint16_t token_word;
static void received(const TransmissionProtocol *p){
 assert(p->command_id==expected_command && p->payload_size==expected_words*2);
 for(unsigned i=0;i<expected_words;i++)assert(((const uint16_t*)p->payload)[i]==(token_test?token_word:(uint16_t)(i*257)));
 seen++;
}
static void address(uint16_t a){
 if(s20_feed(a) || r02_feed(&recovery,a))return;
 parse_protocol(r12s_config_address(a,sidetnfs_tp_shifted),received);
}
static void word(uint16_t w){address(config_wire_byte(w>>8));address(config_wire_byte(w));}
int main(void){
 init_protocol_parser();
 for(unsigned command=0x40d;command<=0x418;command++){
  expected_command=command;expected_words=command==0x40f?256:2;
  address(0xa5a4);word(command);word(expected_words*2);
  for(unsigned i=0;i<expected_words;i++)word((uint16_t)(i*257));
  assert(nextTPstep==HEADER_DETECTION && sidetnfs_tp_shifted);
 }
 assert(seen==12 && !recovery.accepted);
 /* Reproduce raw ST token loss in the actual endpoint dispatch chain. */
 expected_command=0x40e;expected_words=2;token_test=1;
 const uint16_t collisions[]={R02_START,R02_END,R02_DATA,R02_REPLY,S20_START,S20_STOP,S20_DATA,S20_BASE};
 for(unsigned i=0;i<sizeof(collisions)/sizeof(collisions[0]);i++){
  memset(&recovery,0,sizeof(recovery));memset(&s20,0,sizeof(s20));
  nextTPstep=HEADER_DETECTION;
  unsigned before=seen;token_word=collisions[i];
  address(0xabcd);address(expected_command);address(4);
  address(token_word);address(token_word);
  assert(seen==before); /* swallowed token prevents complete request */
 }
 /* Exhaustively cover every 16-bit token half through the new encoding,
  * with both odd 68000 and even Falcon header observations. */
 for(unsigned mode=0;mode<2;mode++)for(unsigned w=0;w<65536;w++){
  memset(&recovery,0,sizeof(recovery));memset(&s20,0,sizeof(s20));
  nextTPstep=HEADER_DETECTION;token_word=(uint16_t)w;unsigned before=seen;
  address((uint16_t)(CONFIG_WIRE_HEADER & (mode?0xfffe:0xffff)));
  word(expected_command);word(4);word(token_word);word(token_word);
  assert(seen==before+1 && nextTPstep==HEADER_DETECTION);
  assert(!recovery.collecting && !recovery.pending && !s20.collecting && !s20.pending);
 }
 assert(!config_needs_030_cache(0x10010,0)); /* Mega STE */
 assert(!config_needs_030_cache(0,0));
 assert(config_needs_030_cache(0x30000,30));
 assert(!config_needs_030_cache(0x30000,40));
 assert(!config_needs_030_cache(0x30000,60));
 assert(!config_needs_030_cache(0x30000,(unsigned long)-1));
 terminate_protocol_parser();puts("F32 raw-token collisions reproduced; all 65536 encoded token halves pass ST and Falcon headers; CPU guards pass. R12S utility transport: all config command IDs, token/payload word ordering and every encoded byte pass through actual legacy parser; no recovery packet collision.");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)
 prefix='#include <assert.h>\n#include <stdint.h>\n#include <stdbool.h>\n#define __not_in_flash_func(n) n\n#define DPRINTF(...) ((void)0)\n#define DPRINTFRAW(...) ((void)0)\nstatic struct {uint32_t timerawh,timerawl;} timer={0,1},*timer_hw=&timer;\n'
 (p/'test.c').write_text(prefix+h+s+test)
 subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-misleading-indentation','-I',str(Path(os.environ.get('SIDETNFS_CONFIG_DIR',r.parent.parent/'SideTNFS-Config'))/'include'),'-I',str(r.parent/'romemul/native'),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
