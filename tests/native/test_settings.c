/* Exercise the actual flash service with the supplied embedded content. */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "wire.h"
#define LOG(component,level,...) ((void)0)
#define LOG_AT(component,level,...) ((void)0)
static uint32_t time_us_32(void){return 123456;}
static unsigned r03_up(unsigned c) { return c>='a' && c<='z'?c-32:c; }
/* TOS wildcard matching uses padded 8.3 fields; '?' also matches padding. */
static bool r03_pattern(const char *s,char out[12],bool wild) {
    memset(out,' ',11);out[11]=0;
    if(wild && s[0]=='*' && !s[1]){memset(out,'?',11);return true;}
    unsigned pos=0,limit=8;
    if(!*s) return false;
    while(*s) {
        unsigned c=(unsigned char)*s++;
        if(c=='.') { if(limit==11) return false; pos=8;limit=11;continue; }
        if(c=='*' && wild) { while(pos<limit) out[pos++]='?'; while(*s && *s!='.') s++;continue; }
        if(pos>=limit || c<33 || c>=127 || c=='/' || c=='\\' || c==':' || c=='"' ||
           c=='<' || c=='>' || c=='|' || ((!wild) && (c=='*' || c=='?'))) return false;
        out[pos++]=(char)r03_up(c);
    }
    return out[0]!=' ';
}
static bool r03_match(const char *pattern,const char *name) {
    char padded[12];if(!r03_pattern(name,padded,false))return false;
    for(unsigned i=0;i<11;i++)if(pattern[i]!='?' && pattern[i]!=padded[i])return false;
    return true;
}
#include "settings_drive.h"
#include "config_wire.h"
static uint8_t req[256],out[512];
static int32_t call(unsigned op,unsigned n){memset(out,0xcc,sizeof(out));r12s_fs(op,req,n,out);return (int32_t)r02_be32(out);}
int main(void){
 assert(call(32,0)==0);uint32_t sid=r02_be32(out+4);assert(sid);r02_put32(req,sid);
 strcpy((char*)req+4,"\\");assert(call(33,6)==0 && r02_be16(out+4)==16);
 strcpy((char*)req+4,"\\SIDETNFS.PRG");assert(call(33,18)==0 && r02_be16(out+4)==0 && r02_be16(out+6)==0 && r02_be16(out+8)==0x21 && r02_be32(out+12)==sidetnfs_config_prg_length);
 r02_put16(req+4,0);r02_put16(req+6,0);strcpy((char*)req+8,"\\*.*");assert(call(34,13)==0 && !strcmp((char*)out+34,"SIDETNFS.PRG"));
 assert(out[25]==0 && r02_be16(out+26)==0 && r02_be16(out+28)==0x21);
 assert(call(35,6)==0 && !strcmp((char*)out+34,"README.TXT") && out[25]==0 && r02_be16(out+28)==0x21);assert(call(35,6)==-49);
 /* Compare every chunk including the 64 KB boundary and short final read. */
 strcpy((char*)req+10,"\\SIDETNFS.PRG");
 for(uint32_t off=0;off<sidetnfs_config_prg_length;off+=508){
  r02_put32(req+4,off);r02_put16(req+8,508);int32_t got=call(36,24);
  unsigned want=sidetnfs_config_prg_length-off;if(want>508)want=508;
  assert(got==(int32_t)want && !memcmp(out+4,sidetnfs_config_prg+off,want));
 }
 r02_put32(req+4,0xffffffffu);assert(call(36,24)==0);r02_put16(req+8,509);assert(call(36,24)==-32);
 strcpy((char*)req+4,"\\..\\PRIVATE");assert(call(33,17)==-34);
 for(unsigned op=38;op<=44;op++)assert(call(op,4)==-36);r02_put32(req,sid+2);assert(call(37,4)==-46);
 for(unsigned i=0;i<256;i++)assert(r12s_config_address(0x7e00+2*i,true)==2*i);
 assert(r12s_config_address(0xa5a4,true)==0xa5a4 && r12s_config_address(0x7e00,false)==0x7e00 && r12s_config_address(0xe200,true)==0xe200);
 puts("F28 settings metadata: ordinary files, valid DOS date, all mutations rejected. R12S flash service: root/stat/list/read, full PRG byte comparison, EOF, bounds, session, read-only enforcement and config window mapping pass.");
}
