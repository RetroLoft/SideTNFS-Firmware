/* R14 stateless SD mutations throughout the configured mapping.
 * Canonical short paths cannot escape that mapping; root is protected. */
#include "sd_protocol.h"
static char r08_old[300];
static bool r08_poisoned;
static bool r08_path(const uint8_t *p,size_t len,bool root){
    if(!r03_path(p,len,false))return false;
    if(len==2)return false; /* never mutate mapped root */
    (void)root;
    unsigned part=0,ext=0;bool dot=false;
    for(size_t i=1;i<len;i++){
        unsigned c=p[i];
        if(c==0 || c=='\\'){
            if(!part || (dot && !ext))return false;
            part=ext=0;dot=false;continue;
        }
        if(c=='.'){if(dot || !part)return false;dot=true;continue;}
        if(!((c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_' || c=='-'))return false;
        if(dot){if(++ext>3)return false;}else if(++part>8)return false;
    }return true;
}
static size_t r08_fs(uint8_t op,const uint8_t *p,uint16_t len,uint8_t *out){
    if(op==21 && len==4 && r03_mounted && r02_be32(p)==r03_session){
        DWORD free_clusters;FATFS *fs=NULL;FRESULT fr=f_getfree(r03_root,&free_clusters,&fs);
        r02_put32(out,r03_error(fr));if(fr!=FR_OK)return 4;
        r02_put32(out+4,free_clusters);r02_put32(out+8,fs->n_fatent-2);
        r02_put32(out+12,512);r02_put32(out+16,fs->csize);return 20;
    }
    if(op<=21){size_t n=r03_fs(op,p,len,out);if(op==16 && (int32_t)r02_be32(out)==0)r08_poisoned=false;return n;}
    int32_t result=-32;FRESULT fr=FR_OK;
    if(len<4 || !r03_mounted || r02_be32(p)!=r03_session){result=-46;goto finish;}
    if(r08_poisoned){result=-65;goto finish;}
    if(op==27 || op==28){
        unsigned prefix=op==27?6:8;
        if(len<prefix+2 || !r08_path(p+prefix,len-prefix,false)){result=-34;goto finish;}
        if(op==27){unsigned attr=r02_be16(p+4);
            if(attr&~0x27u){result=-36;goto finish;}
            fr=f_chmod(r03_full,attr,AM_RDO|AM_HID|AM_SYS|AM_ARC);
        }else{
            FILINFO info={0};info.ftime=r02_be16(p+4);info.fdate=r02_be16(p+6);
            unsigned month=(info.fdate>>5)&15,day=info.fdate&31;
            if(!month || month>12 || !day || day>31 || (info.ftime>>11)>23 || ((info.ftime>>5)&63)>59 || (info.ftime&31)>29){result=-36;goto finish;}
            static const unsigned days[12]={31,28,31,30,31,30,31,31,30,31,30,31};
            unsigned year=1980+(info.fdate>>9),maxday=days[month-1]+(month==2 && year%4==0 && (year%100!=0 || year%400==0));
            if(day>maxday){result=-36;goto finish;}
            fr=f_utime(r03_full,&info);
        }
        result=r03_error(fr);
    }else if(op==22 || op==24){
        if(!r08_path(p+4,len-4,op==24)){result=-36;goto finish;}
        if(op==24){fr=f_mkdir(r03_full);result=fr==FR_EXIST?-36:r03_error(fr);}
        else{
            fr=f_open(&r03_file,r03_full,FA_WRITE|FA_CREATE_ALWAYS);result=r03_error(fr);
            if(fr==FR_OK){fr=f_close(&r03_file);result=fr==FR_OK?0:-65;}
        }
    }else if(op==23){
        if(len<12){result=-34;goto finish;}
        unsigned n=r02_be16(p+8),count=r02_be16(p+10);uint32_t offset=r02_be32(p+4);
        if(!count || 12u+n+count!=len || count>256 || offset>0x7fffffffu-count){result=-34;goto finish;}
        if(!r08_path(p+12,n,false)){result=-36;goto finish;}
        fr=f_open(&r03_file,r03_full,FA_WRITE);result=r03_error(fr);
        if(fr==FR_OK){
            UINT written=0;
            if(offset>f_size(&r03_file))result=-64;
            else{
                fr=f_lseek(&r03_file,offset);
                if(fr==FR_OK)fr=f_write(&r03_file,p+12+n,count,&written);
                result=fr==FR_OK?(int32_t)written:-65;
            }
            FRESULT closed=f_close(&r03_file); /* flush data and metadata before ACK */
            if(closed!=FR_OK){result=-65;fr=closed;}
        }
    }else if(op==25){
        if(len<6 || r02_be16(p+4)>1){result=-34;goto finish;}
        if(!r08_path(p+6,len-6,true)){result=-36;goto finish;}
        fr=f_stat(r03_full,&r03_info);result=r03_error(fr);
        if(fr==FR_OK){
            if(!!(r03_info.fattrib&AM_DIR)!=!!r02_be16(p+4)){result=-36;goto finish;}
            fr=f_unlink(r03_full);result=r03_error(fr);
        }
    }else if(op==26){
        if(len<6){result=-34;goto finish;}unsigned n=r02_be16(p+4);
        if(n>len-6u || !r08_path(p+6,n,false)){result=-36;goto finish;}
        strcpy(r08_old,r03_full);
        if(!r08_path(p+6+n,len-6-n,false)){result=-36;goto finish;}
        /* File rename only in this stage, so open descendant/current-dir
         * bookkeeping cannot be invalidated by renaming a directory. */
        fr=f_stat(r08_old,&r03_info);result=r03_error(fr);
        if(fr==FR_OK){
            if(r03_info.fattrib&AM_DIR){result=-36;goto finish;}
            fr=f_rename(r08_old,r03_full);result=fr==FR_EXIST?-36:r03_error(fr);
        }
    }
    /* Disk/close errors leave the outcome uncertain. No more mutations in
     * this mount session; never automatically retry create or write. */
    if(result==-65)r08_poisoned=true;
finish:
    r02_put32(out,(uint32_t)result);
    LOG_AT(SD,sidetnfs_log_result_level(result),"R08 op=%u result=%ld poisoned=%u\r\n",op,(long)result,(unsigned)r08_poisoned);
    return 4;
}
