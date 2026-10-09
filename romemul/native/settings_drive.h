/* Recovery settings disk: immutable flash files, no FatFS/TNFS dependency.
 * Opcodes 32..37 mirror the read-only SD wire operations 16..21. */
#include "include/sidetnfs_config_drive.h"
/* The medium is immutable; exported files have ordinary attributes so a
 * desktop copy does not request unsupported read-only Fcreate at the target.
 * Use the DOS epoch (1980-01-01), not an invalid all-zero date. */
#define R28_SETTINGS_DATE 0x0021u
static uint32_t r12s_session;
static struct {char pattern[12];unsigned cursor;bool active;} r12s_search[32];
static const char *r12s_name(unsigned i){
    static const char *names[]={"SIDETNFS.PRG","README.TXT"};return names[i];
}
static const uint8_t *r12s_data(unsigned i){return i==0?sidetnfs_config_prg:sidetnfs_config_readme;}
static uint32_t r12s_size(unsigned i){return i==0?sidetnfs_config_prg_length:sidetnfs_config_readme_length;}
static bool r12s_path(const uint8_t *p,unsigned n,bool wild,char pattern[12]){
    if(n<2 || n>14 || p[0]!='\\' || p[n-1] || memchr(p,0,n-1) || memchr(p+1,'\\',n-2))return false;
    if(n==2){memset(pattern,0,12);return true;}
    return r03_pattern((const char*)p+1,pattern,wild);
}
static int r12s_find(const char *pattern){
    for(unsigned i=0;i<2;i++)if(r03_match(pattern,r12s_name(i)))return (int)i;
    return -1;
}
static int32_t r12s_next(unsigned slot,uint8_t *out){
    if(slot>=32 || !r12s_search[slot].active)return -49;
    while(r12s_search[slot].cursor<2){
        unsigned i=r12s_search[slot].cursor++;
        if(!r03_match(r12s_search[slot].pattern,r12s_name(i)))continue;
        memset(out,0,44);r02_put16(out+24,R28_SETTINGS_DATE);r02_put32(out+26,r12s_size(i));
        strcpy((char*)out+30,r12s_name(i));return 0;
    }
    r12s_search[slot].active=false;return -49;
}
static size_t r12s_fs(uint8_t op,const uint8_t *p,uint16_t len,uint8_t *out){
    int32_t result=-32;size_t used=4;char pattern[12];
    if(op==32){
        if(len)goto finish;
        r12s_session=(time_us_32()^0x53313253u)|1u;
        memset(r12s_search,0,sizeof(r12s_search));r02_put32(out+4,r12s_session);used=8;result=0;
        LOG(GEMDOS,INFO,"R12S settings mounted S: SIDETNFS.PRG bytes=%lu\r\n",(unsigned long)sidetnfs_config_prg_length);
        goto finish;
    }
    if(len<4 || !r12s_session || r02_be32(p)!=r12s_session){result=-46;goto finish;}
    if(op==33){
        if(!r12s_path(p+4,len-4,false,pattern)){result=-34;goto finish;}
        int i=pattern[0]?r12s_find(pattern):-2;
        if(i==-1){result=-33;goto finish;}
        memset(out+4,0,12);r02_put16(out+4,i==-2?16:0);r02_put16(out+8,R28_SETTINGS_DATE);
        if(i>=0)r02_put32(out+12,r12s_size((unsigned)i));used=16;result=0;
    }else if(op==34){
        if(len<10 || r02_be16(p+4)>=32)goto finish;
        unsigned slot=r02_be16(p+4);r12s_search[slot].active=false;
        if(!r12s_path(p+8,len-8,true,pattern)){result=-34;goto finish;}
        if(!pattern[0] || r02_be16(p+6)==8){result=-33;goto finish;}
        memcpy(r12s_search[slot].pattern,pattern,12);r12s_search[slot].cursor=0;r12s_search[slot].active=true;
        result=r12s_next(slot,out+4);if(result==-49)result=-33;if(!result)used=48;
    }else if(op==35){
        if(len!=6)goto finish;
        result=r12s_next(r02_be16(p+4),out+4);if(!result)used=48;
    }else if(op==36){
        if(len<12 || r02_be16(p+8)>508)goto finish;
        if(!r12s_path(p+10,len-10,false,pattern)){result=-34;goto finish;}
        int i=pattern[0]?r12s_find(pattern):-1;if(i<0){result=-33;goto finish;}
        uint32_t offset=r02_be32(p+4),size=r12s_size((unsigned)i);unsigned count=r02_be16(p+8);
        if(offset>=size)count=0;else if(count>size-offset)count=size-offset;
        if(count)memcpy(out+4,r12s_data((unsigned)i)+offset,count);
        used=4+count;result=(int32_t)count;
    }else if(op==37){
        if(len!=4)goto finish;
        r02_put32(out+4,0);r02_put32(out+8,(sidetnfs_config_prg_length+sidetnfs_config_readme_length+511)/512);
        r02_put32(out+12,512);r02_put32(out+16,1);used=20;result=0;
    }else result=-36;
finish:
    r02_put32(out,(uint32_t)result);return used;
}
