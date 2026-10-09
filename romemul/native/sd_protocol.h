/* R03 read-only SD service. One caller/main loop, no IRQ filesystem work.
 * Requests 16 mount; 17 stat; 18 first; 19 next; 20 read; 21 geometry.
 * Every non-mount request starts with the mount session (BE32).
 * Every response starts with a signed GEMDOS result (BE32).
 * Read uses path+offset, opening/closing each chunk: no remote file handles. */
#define R03_SEARCHES 32
#define R03_PATH 128
typedef struct { DIR dir; bool open; uint16_t attrs; char pattern[12]; } r03_search;
static r03_search r03_searches[R03_SEARCHES];
static char r03_root[160], r03_full[300];
static FILINFO r03_info;
static FIL r03_file;
static DIR r03_mount_check;
static uint32_t r03_session;
static bool r03_mounted;

static int32_t r03_error(FRESULT fr) {
    switch(fr) {
    case FR_OK:return 0;
    case FR_NO_FILE:return -33;
    case FR_NO_PATH:return -34;
    case FR_DENIED:case FR_WRITE_PROTECTED:return -36;
    case FR_NOT_READY:case FR_NOT_ENABLED:case FR_INVALID_DRIVE:return -46;
    case FR_INVALID_NAME:return -34;
    default:return -65;
    }
}
/* Require a canonical guest path. No volume letters, relative paths,
 * parent components or control bytes can escape the configured root. */
static bool r03_path(const uint8_t *p, size_t len, bool wildcard) {
    if(!len || len>R03_PATH || p[0]!='\\' || p[len-1]!=0) return false;
    size_t root=strlen(r03_root), used=root, component=1;
    if(root+len>=sizeof(r03_full)) return false;
    memcpy(r03_full,r03_root,root);
    for(size_t i=1;i<len;i++) {
        unsigned c=p[i];
        if(c==0 && i!=len-1) return false;
        if(c==0 || c=='\\') {
            size_t n=i-component;
            if((n==1 && p[component]=='.') || (n==2 && p[component]=='.' && p[component+1]=='.')) return false;
            if(c=='\\' && n==0) return false;
            if(i>1 && p[i-1]=='\\') return false;
            component=i+1;
        } else if(c<32 || c>=127 || c==':' || c=='/' || ((!wildcard) && (c=='*' || c=='?'))) return false;
        if(i==1 && c!=0 && (used==0 || r03_full[used-1]!='/')) r03_full[used++]='/';
        r03_full[used++]=c=='\\'?'/':(char)c;
    }
    if(len==2) r03_full[root]=0;
    return true;
}
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
static const char *r03_shortname(void) {
    char test[12];
    if(r03_pattern(r03_info.fname,test,false))return r03_info.fname;
    if(r03_pattern(r03_info.altname,test,false))return r03_info.altname;
    return NULL;
}
static int32_t r03_next(unsigned slot,uint8_t *dta) {
    r03_search *s=&r03_searches[slot];
    if(!s->open)return -49;
    for(;;) {
        r02a_mark(4,0,slot);
        FRESULT fr=f_readdir(&s->dir,&r03_info);
        r02a_mark(5,fr,slot);
        if(fr!=FR_OK || !r03_info.fname[0]) {
            f_closedir(&s->dir);s->open=false;
            return fr==FR_OK?-49:r03_error(fr);
        }
        if(!strcmp(r03_info.fname,".") || !strcmp(r03_info.fname,".."))continue;
        const char *name=r03_shortname();
        if(!name || (r03_info.fattrib&0x16&~s->attrs) || !r03_match(s->pattern,name))continue;
        memset(dta,0,44);dta[21]=r03_info.fattrib&0x37;
        r02_put16(dta+22,r03_info.ftime);r02_put16(dta+24,r03_info.fdate);
        r02_put32(dta+26,r03_info.fsize);
        for(unsigned i=0;name[i] && i<12;i++)dta[30+i]=(uint8_t)r03_up((unsigned char)name[i]);
        return 0;
    }
}
static size_t r03_fs(uint8_t op,const uint8_t *p,uint16_t len,uint8_t *out) {
    int32_t result=-32;size_t used=4;
    if(op==16) {
        if(len!=0)goto finish;
        r03_mounted=false;
        for(unsigned i=0;i<R03_SEARCHES;i++){if(r03_searches[i].open)f_closedir(&r03_searches[i].dir);r03_searches[i].open=false;}
        /* Mount only a ready configured P: SD mapping. No guessed drive. */
        sidetnfs_sd_drive_status_t sd;bool found=false;
        for(int i=0;i<SIDETNFS_SD_SERVICE_MAX_SLOTS;i++) {
            if(sidetnfs_sd_get_drive_status(i,&sd) && sd.driveletter=='P' && sd.status==SIDETNFS_SD_STATUS_READY) {found=true;break;}
        }
        if(!found || sidetnfs_sd_global_status()!=SIDETNFS_SD_STATUS_READY) {result=-46;goto finish;}
        size_t n=strlen(sd.sd_path);
        if(n==0 || n>sizeof(r03_root)-5) {result=-34;goto finish;}
        /* Service configuration already names an SD directory; make its
         * volume explicit so native/virtual current paths cannot redirect it. */
        if(n>=2 && sd.sd_path[1]==':') {
            if(sd.sd_path[0]!='0') {result=-46;goto finish;}
            strcpy(r03_root,sd.sd_path);
        } else {strcpy(r03_root,"0:/");strcat(r03_root,sd.sd_path[0]=='/'?sd.sd_path+1:sd.sd_path);}
        for(char *s=r03_root;*s;s++)if(*s=='\\')*s='/';
        n=strlen(r03_root);while(n>3 && r03_root[n-1]=='/')r03_root[--n]=0;
        FRESULT fr=f_opendir(&r03_mount_check,r03_root);
        if(fr!=FR_OK) {result=r03_error(fr);goto finish;}
        f_closedir(&r03_mount_check);
        for(unsigned i=0;i<R03_SEARCHES;i++) {if(r03_searches[i].open)f_closedir(&r03_searches[i].dir);r03_searches[i].open=false;}
        r03_session=time_us_32()^0x52303331u;if(!r03_session)r03_session=1;
        r03_mounted=true;r02_put32(out+4,r03_session);used=8;result=0;goto finish;
    }
    if(len<4 || !r03_mounted || r02_be32(p)!=r03_session) {result=-46;goto finish;}
    if(op==17) {
        if(!r03_path(p+4,len-4,false)){result=-34;goto finish;}
        FRESULT fr;
        if(len==6){memset(&r03_info,0,sizeof(r03_info));r03_info.fattrib=AM_DIR;fr=FR_OK;}
        else fr=f_stat(r03_full,&r03_info);
        result=r03_error(fr);
        if(fr==FR_OK) {
            r02_put16(out+4,r03_info.fattrib);r02_put16(out+6,r03_info.ftime);
            r02_put16(out+8,r03_info.fdate);r02_put16(out+10,0);r02_put32(out+12,r03_info.fsize);used=16;
        }
    } else if(op==18) {
        if(len<10 || r02_be16(p+4)>=R03_SEARCHES){result=-34;goto finish;}
        unsigned slot=r02_be16(p+4);r03_search *s=&r03_searches[slot];
        if(s->open){f_closedir(&s->dir);s->open=false;}
        if(!r03_path(p+8,len-8,true)){result=-34;goto finish;}
        char *pattern=strrchr(r03_full,'/');
        if(!pattern || !r03_pattern(pattern+1,s->pattern,true)){result=-33;goto finish;}
        *pattern=0;s->attrs=r02_be16(p+6);
        /* Volume-label searches return no entries in this recovery stage. */
        if(s->attrs&8){result=-33;goto finish;}
        r02a_mark(2,0,slot);FRESULT fr=f_opendir(&s->dir,r03_full);
        r02a_mark(3,fr,slot);result=r03_error(fr);
        if(fr==FR_OK){s->open=true;result=r03_next(slot,out+4);if(result==-49)result=-33;if(result==0)used=48;}
    } else if(op==19) {
        if(len!=6 || r02_be16(p+4)>=R03_SEARCHES)goto finish;
        result=r03_next(r02_be16(p+4),out+4);if(result==0)used=48;
    } else if(op==20) {
        if(len<12 || r02_be16(p+8)>508 || !r03_path(p+10,len-10,false)){result=-34;goto finish;}
        FRESULT fr=f_open(&r03_file,r03_full,FA_READ);result=r03_error(fr);
        if(fr==FR_OK) {
            uint32_t offset=r02_be32(p+4);UINT count=0;
            if(offset>=f_size(&r03_file))result=0;
            else {
                fr=f_lseek(&r03_file,offset);
                if(fr==FR_OK)fr=f_read(&r03_file,out+4,r02_be16(p+8),&count);
                result=fr==FR_OK?(int32_t)count:r03_error(fr);
            }
            FRESULT close=f_close(&r03_file);if(result>=0 && close!=FR_OK)result=r03_error(close);
            if(result>=0)used=4+(size_t)result;
        }
    } else if(op==21) {
        if(len!=4)goto finish;
        /* Card geometry, with zero writable free space for this view. */
        FATFS *fs;DWORD free_clusters;FRESULT fr=f_getfree("0:",&free_clusters,&fs);
        if(fr!=FR_OK){result=r03_error(fr);goto finish;}
        r02_put32(out+4,0);r02_put32(out+8,fs->n_fatent-2);r02_put32(out+12,512);r02_put32(out+16,fs->csize);
        used=20;result=0;
    }
finish:
    r02_put32(out,(uint32_t)result);
    LOG_AT(SD,sidetnfs_log_result_level(result),"R03 op=%u result=%ld bytes=%u\r\n",op,(long)result,(unsigned)used);
    return used;
}
