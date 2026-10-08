/* Same stateless file RPC contract as P:. Sessions/searches are per mapping.
 * Short uppercase names only, matching the existing TNFS name policy. */
static uint32_t r13_tokens[R13_SLOTS];
static struct {bool open,eof,invalid;uint8_t handle;uint16_t attrs;char pattern[12];} r13_search[R13_SLOTS][32];
static char r13_pathname[128],r13_other[128];
static uint8_t r13_body[520];
static int r13_error(int rc){
    if(rc<=0)return rc;
    /* TNFS wire status values, NOT host POSIX errno numbers.
     * Ordinary server refusals must not become the guest's fatal -65. */
    switch(rc){
    case 0x02:return -33; /* missing file */
    case 0x0c:return -34; /* path component is not a directory */
    case 0x06:return -37; /* bad file handle */
    case 0x08:return -39; /* out of memory */
    case 0x0f:case 0x10:return -35; /* too many open files */
    case 0x05:case 0x0e:case 0x13:return -64; /* invalid request/range */
    case 0x16:return -32; /* unsupported operation */
    case 0x21:return -49; /* directory EOF; file EOF handled by reader */
    case 0x01:case 0x03:case 0x04:case 0x07:case 0x09:case 0x0a:
    case 0x0b:case 0x0d:case 0x11:case 0x12:case 0x14:case 0x15:
    case 0x17:case 0x18:case 0x19:case 0x1a:case 0x1d:case 0x1e:
    case 0x1f:return -36; /* access/resource/I/O refusal, not lost transport */
    default:return -65; /* stale/bad session state, protocol or unknown code */
    }
}
static bool r13_path(const uint8_t *p,unsigned n,char *out,bool wildcard){
    if(n<2 || n>128 || p[0]!='\\' || p[n-1])return false;
    unsigned start=1;
    for(unsigned i=0;i<n;i++){
        unsigned c=p[i];out[i]=c=='\\'?'/':c;
        if(i==0)continue;
        if(!c || c=='\\'){
            if(!c && i!=n-1)return false;
            if(i==1 && !c)return true;
            if(i==start || (i-start==1 && p[start]=='.') || (i-start==2 && p[start]=='.' && p[start+1]=='.'))return false;
            char part[128],padded[12];memcpy(part,p+start,i-start);part[i-start]=0;
            if(!r03_pattern(part,padded,wildcard && !c))return false;
            start=i+1;
        }else if(c<33 || c>=127 || c==':' || c=='/')return false;
    }return true;
}
static int r13_simple(uint8_t cmd,const char *path){return r13_error(r13_rpc(cmd,(const uint8_t *)path,strlen(path)+1));}
static int r13_stat(const char *path,uint8_t *out){
    int rc=r13_simple(0x24,path);if(rc)return rc;
    struct r13_net_slot *s=&r13_net[r13_current];if(s->received!=27)return r13_bad();
    unsigned mode=r13_le16(s->rx+5);SidetnfsAtariDirEntry entry;
    /* Reuse existing timestamp conversion and attribute policy. */
    if(!sidetnfs_normalize_dir_entry("STAT",(mode&0xf000)==0x4000?1:0,r13_le32(s->rx+11),r13_le32(s->rx+19),&entry))return r13_bad();
    r02_put16(out+4,entry.attr|((mode&0222)?0:1));r02_put16(out+6,entry.time);r02_put16(out+8,entry.date);
    r02_put16(out+10,0);r02_put32(out+12,entry.size);return 0;
}
static int r13_close(uint8_t h){int rc=r13_error(r13_rpc(0x23,&h,1));return rc?r13_bad():0;}
static int r13_open(const char *path,unsigned flags,uint8_t *handle){
    r13_putle16(r13_body,flags);r13_putle16(r13_body+2,0644);
    unsigned n=strlen(path)+1;memcpy(r13_body+4,path,n);
    int rc=r13_error(r13_rpc(0x29,r13_body,n+4));if(rc)return rc;
    struct r13_net_slot *s=&r13_net[r13_current];if(s->received!=6)return r13_bad();
    *handle=s->rx[5];return 0;
}
static int r13_next(unsigned idx,uint8_t *dta){
    unsigned slot=r13_current;
    if(r13_search[slot][idx].invalid)return -2; /* Restart with Fsfirst after session loss. */
    if(!r13_search[slot][idx].open)return -49;
    for(unsigned round=0;round<512;round++){
        int rc;
        if(r13_search[slot][idx].eof)goto end;
        r13_body[0]=r13_search[slot][idx].handle;r13_body[1]=1;
        rc=r13_rpc(0x18,r13_body,2);
        if(rc<0)return rc;
        if(rc && rc!=0x21)return r13_error(rc);
        struct r13_net_slot *s=&r13_net[slot];
        if(rc==0x21)r13_search[slot][idx].eof=true;
        if(s->received==5 && rc==0x21)goto end;
        if(s->received<9 || s->rx[5]>1)return r13_bad();
        if(!s->rx[5]){r13_search[slot][idx].eof=true;goto end;}
        if(s->received<23 || !memchr(s->rx+22,0,s->received-22))return r13_bad();
        SidetnfsAtariDirEntry e;
        if(!sidetnfs_normalize_dir_entry((char *)s->rx+22,s->rx[9],r13_le32(s->rx+10),r13_le32(s->rx+14),&e))continue;
        if((e.attr&0x16&~r13_search[slot][idx].attrs) || !r03_match(r13_search[slot][idx].pattern,e.name))continue;
        memset(dta,0,44);dta[21]=e.attr;r02_put16(dta+22,e.time);r02_put16(dta+24,e.date);
        r02_put32(dta+26,e.size);memcpy(dta+30,e.name,strlen(e.name)+1);return 0;
end:
        r13_body[0]=r13_search[slot][idx].handle;
        rc=r13_error(r13_rpc(0x12,r13_body,1));r13_search[slot][idx].open=false;
        return rc?rc:-49;
    }
    /* A bounded scan limit is not a broken transport/session. */
    r13_body[0]=r13_search[slot][idx].handle;
    int closed=r13_error(r13_rpc(0x12,r13_body,1));r13_search[slot][idx].open=false;
    return closed?closed:-36;
}
static size_t r13_fs(unsigned slot,uint8_t op,const uint8_t *p,unsigned len,uint8_t *out){
    int rc=-32;unsigned used=4;uint8_t handle=0;bool opened=false;
    bool already_poisoned=r13_net[slot].poisoned;
    r13_guest_op=op;r13_file_offset=(op==20 || op==23) && len>=8?r02_be32(p+4):0;
    if(op==16){
        if(len)goto done;
        if(!r13_tokens[slot]){r13_tokens[slot]=time_us_32()^(0x52313300u+slot);if(!r13_tokens[slot])r13_tokens[slot]=1;}
        r02_put32(out+4,r13_tokens[slot]);rc=0;used=8;goto done;
    }
    if(len<4 || !r13_tokens[slot] || r02_be32(p)!=r13_tokens[slot]){rc=-46;goto done;}
    /* No CHMOD on unverified daemons: some register an empty handler.
     * Decline metadata setters locally even while network/DNS is unavailable. */
    if(op==27 || op==28){rc=-32;goto done;}
    rc=r13_begin(slot);if(rc)goto done;rc=-32;
    if(op==17 || op==22 || op==24){
        if(!r13_path(p+4,len-4,r13_pathname,false)){rc=-34;goto done;}
        if(op==17){rc=r13_stat(r13_pathname,out);if(!rc)used=16;}
        else if(!strcmp(r13_pathname,"/"))rc=-36;
        else if(op==24)rc=r13_simple(0x13,r13_pathname);
        else {rc=r13_open(r13_pathname,0x303,&handle);opened=!rc;}
    }else if(op==20 || op==23){
        bool write=op==23;unsigned n,count,offset;
        if(len<(write?12u:10u)){rc=-34;goto done;}
        offset=r02_be32(p+4);n=write?r02_be16(p+8):len-10;count=r02_be16(p+(write?10:8));
        if(count>508 || offset>0x7fffffffu-count || (write && 12+n+count!=len) ||
           !r13_path(p+(write?12:10),n,r13_pathname,false)){rc=-34;goto done;}
        rc=r13_open(r13_pathname,write?2:1,&handle);if(rc)goto done;opened=true;
        r13_body[0]=handle;r13_body[1]=0;r13_putle32(r13_body+2,offset);
        rc=r13_error(r13_rpc(0x25,r13_body,6));if(rc)goto done;
        if(r13_net[slot].received!=9 || r13_le32(r13_net[slot].rx+5)!=offset){rc=r13_bad();goto done;}
        unsigned total=0;
        do {
            unsigned wanted=count-total;
            r13_body[0]=handle;r13_putle16(r13_body+1,wanted);
            if(write)memcpy(r13_body+3,p+12+n,wanted);
            int wire=r13_rpc(write?0x22:0x21,r13_body,write?3+wanted:3);
            struct r13_net_slot *s=&r13_net[slot];
            if(!write && wire==0x21 && s->received==5){rc=(int)total;used=4+total;break;}
            rc=(!write && wire==0x21)?0:r13_error(wire);if(rc)goto done;
            if(s->received<7){rc=r13_bad();goto done;}
            unsigned actual=r13_le16(s->rx+5);
            if(actual>wanted || s->received!=(write?7:7+actual)){rc=r13_bad();goto done;}
            if(!write)memcpy(out+4+total,s->rx+7,actual);
            total+=actual;rc=(int)total;used=write?4:4+total;
            /* A short positive READ can be a server packet-size cap.
             * Continue at the acknowledged cursor until filled or real EOF.
             * A short WRITE remains a partial write; never repeat it. */
            if(write || wire==0x21 || !actual)break;
        }while(total<count);
    }else if(op==25){
        if(len<8 || r02_be16(p+4)>1 || !r13_path(p+6,len-6,r13_pathname,false)){rc=-34;goto done;}
        rc=!strcmp(r13_pathname,"/")?-36:r13_simple(r02_be16(p+4)?0x14:0x26,r13_pathname);
    }else if(op==26){
        unsigned n=len>=6?r02_be16(p+4):0;
        if(len<6 || n>len-6 || !r13_path(p+6,n,r13_pathname,false) || !r13_path(p+6+n,len-6-n,r13_other,false)){rc=-34;goto done;}
        if(!strcmp(r13_pathname,"/") || !strcmp(r13_other,"/")){rc=-36;goto done;}
        /* Preserve existing destination; no implicit replacement. */
        uint8_t stat[16];rc=r13_stat(r13_pathname,stat);if(rc)goto done;
        if(r02_be16(stat+4)&16){rc=-36;goto done;}
        rc=r13_stat(r13_other,stat);if(rc!=-33){if(!rc)rc=-36;goto done;}
        n=strlen(r13_pathname)+1;unsigned m=strlen(r13_other)+1;
        memcpy(r13_body,r13_pathname,n);memcpy(r13_body+n,r13_other,m);rc=r13_error(r13_rpc(0x28,r13_body,n+m));
    }else if(op==18 || op==19){
        if(len<6 || r02_be16(p+4)>=32){rc=-34;goto done;}unsigned idx=r02_be16(p+4);bool exact=false;
        if(op==18){
            if(len<10 || !r13_path(p+8,len-8,r13_pathname,true)){rc=-34;goto done;}
            if(r13_search[slot][idx].open){r13_body[0]=r13_search[slot][idx].handle;rc=r13_error(r13_rpc(0x12,r13_body,1));r13_search[slot][idx].open=false;if(rc)goto done;}
            char *slash=strrchr(r13_pathname,'/');
            if(!slash || !r03_pattern(slash+1,r13_search[slot][idx].pattern,true)){rc=-33;goto done;}
            exact=strchr(slash+1,'*')==NULL && strchr(slash+1,'?')==NULL;
            *slash=0;r13_search[slot][idx].attrs=r02_be16(p+6);
            if(r13_search[slot][idx].attrs&8){rc=-33;goto done;}
            memset(r13_body,0,4);r13_body[4]='*';r13_body[5]=0;
            const char *dir=*r13_pathname?r13_pathname:"/";unsigned n=strlen(dir)+1;memcpy(r13_body+6,dir,n);
            rc=r13_error(r13_rpc(0x17,r13_body,n+6));if(rc)goto done;
            if(r13_net[slot].received<6){rc=r13_bad();goto done;}
            r13_search[slot][idx].handle=r13_net[slot].rx[5];r13_search[slot][idx].open=true;r13_search[slot][idx].eof=false;r13_search[slot][idx].invalid=false;
        }
        rc=r13_next(idx,out+4);
        if(op==18 && (exact || rc<0) && r13_search[slot][idx].open && !r13_net[slot].poisoned){
            r13_body[0]=r13_search[slot][idx].handle;
            int closed=r13_error(r13_rpc(0x12,r13_body,1));r13_search[slot][idx].open=false;
            if(closed)rc=r13_bad();
        }
        if(op==18 && rc==-49)rc=-33;if(!rc)used=48;
    }else if(op==21){
        /* Same documented synthetic geometry used by legacy TNFS backend.
         * Capacity is unknown; actual writes report server errors. */
        r02_put32(out+4,32768);r02_put32(out+8,65536);r02_put32(out+12,512);r02_put32(out+16,32);used=20;rc=0;
    }
done:
    if(opened && !r13_net[slot].poisoned){int closed=r13_close(handle);if(closed){rc=closed;used=4;}}
    if(!already_poisoned && r13_net[slot].poisoned && (!r13_net[slot].mounted ||
       op==17 || op==18 || op==19 || op==20 || op==21)){
        /* No mutation in this operation (or MOUNT never completed). Do not
         * replay it. Retire all server handles and allow a later request to
         * mount with a new UDP endpoint. Guest -2 does not latch fatal state. */
        struct r13_net_slot *s=&r13_net[slot];
        LOG(TNFS,WARN,"F19 TNFS retire slot=%u op=%u sid=%04x retry=next-access cooldown_ms=1000\r\n",slot,op,s->sid);
        cyw43_arch_lwip_begin();if(s->pcb){udp_remove(s->pcb);r35_resources[slot].pcb_free++;}cyw43_arch_lwip_end();
        struct r35_resource *a=&r35_resources[slot];
        a->abandoned_files+=a->files;a->abandoned_dirs+=a->dirs;a->files=a->dirs=0;
        memset(s,0,sizeof(*s));s->recovering=true;s->recover_at=time_us_64()+1000000u;
        memset(r13_search[slot],0,sizeof(r13_search[slot]));
        for(unsigned i=0;i<32;i++)r13_search[slot][i].invalid=true;
        rc=-2;used=4;
    }
    if(rc<0)used=4;
    r02_put32(out,(uint32_t)rc);
    LOG_AT(TNFS,sidetnfs_log_result_level(rc),"R13 slot=%u op=%u result=%ld poisoned=%u\r\n",slot,op,(long)rc,r13_net[slot].poisoned);
    return used;
}
