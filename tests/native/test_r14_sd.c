#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include "wire.h"
#define SIDETNFS_SD_SERVICE_MAX_SLOTS 1
#define SIDETNFS_SD_STATUS_READY 0
#define AM_DIR 16
#define FA_READ 1
#define FA_WRITE 2
#define FA_CREATE_ALWAYS 8
#define LOG(component,level,...) ((void)0)
#define LOG_AT(component,level,...) ((void)0)
typedef enum {FR_OK,FR_DISK_ERR,FR_INT_ERR,FR_NOT_READY,FR_NO_FILE,FR_NO_PATH,FR_INVALID_NAME,FR_DENIED,FR_EXIST,FR_INVALID_OBJECT,FR_WRITE_PROTECTED,FR_INVALID_DRIVE,FR_NOT_ENABLED} FRESULT;
typedef unsigned UINT;typedef uint32_t DWORD;
typedef struct {bool live;} DIR;
typedef struct {FILE *fp;} FIL;
typedef struct {char fname[64],altname[13];uint8_t fattrib;uint16_t ftime,fdate;uint32_t fsize;} FILINFO;
typedef struct {uint32_t n_fatent,csize;} FATFS;
typedef struct {char driveletter;unsigned status;char sd_path[128];} sidetnfs_sd_drive_status_t;
static char temp[]="/tmp/r08-fs-XXXXXX",hostpath[512];static unsigned mutations,live,fail_close,short_write;
static const char *host(const char *p){assert(!strncmp(p,"0:/hd",5));snprintf(hostpath,sizeof(hostpath),"%s%s",temp,p+5);return hostpath;}
static FRESULT err(void){return errno==ENOENT?FR_NO_FILE:errno==EEXIST?FR_EXIST:FR_DENIED;}
static unsigned sidetnfs_sd_global_status(void){return 0;}
static bool sidetnfs_sd_get_drive_status(int i,sidetnfs_sd_drive_status_t *s){if(i)return false;*s=(sidetnfs_sd_drive_status_t){.driveletter='P',.status=0,.sd_path="hd"};return true;}
static uint32_t time_us_32(void){return 123456;}
static void r02a_mark(unsigned a,unsigned b,unsigned c){(void)a;(void)b;(void)c;}
static FRESULT f_opendir(DIR *d,const char *p){struct stat s;if(stat(host(p),&s)||!S_ISDIR(s.st_mode))return FR_NO_PATH;d->live=true;return FR_OK;}
static FRESULT f_closedir(DIR *d){assert(d->live);d->live=false;return FR_OK;}
static FRESULT f_readdir(DIR *d,FILINFO *f){assert(d->live);memset(f,0,sizeof(*f));return FR_OK;}
static FRESULT f_stat(const char *p,FILINFO *f){struct stat s;if(stat(host(p),&s))return err();memset(f,0,sizeof(*f));f->fsize=s.st_size;f->fattrib=S_ISDIR(s.st_mode)?AM_DIR:0;return FR_OK;}
static FRESULT f_open(FIL *f,const char *p,unsigned mode){if(mode&FA_WRITE)mutations++;f->fp=fopen(host(p),mode&FA_CREATE_ALWAYS?"w+b":mode&FA_WRITE?"r+b":"rb");if(!f->fp)return err();live++;return FR_OK;}
static unsigned f_size(FIL *f){struct stat s;assert(!fstat(fileno(f->fp),&s));return s.st_size;}
static FRESULT f_lseek(FIL *f,unsigned n){return fseek(f->fp,n,SEEK_SET)?FR_DISK_ERR:FR_OK;}
static FRESULT f_read(FIL *f,void *out,unsigned n,UINT *got){*got=fread(out,1,n,f->fp);return ferror(f->fp)?FR_DISK_ERR:FR_OK;}
static FRESULT f_write(FIL *f,const void *in,unsigned n,UINT *got){if(short_write && n>short_write)n=short_write;*got=fwrite(in,1,n,f->fp);return ferror(f->fp)?FR_DISK_ERR:FR_OK;}
static FRESULT f_close(FIL *f){assert(f->fp);int e=fclose(f->fp);f->fp=NULL;live--;return e||fail_close?FR_DISK_ERR:FR_OK;}
static FRESULT f_getfree(const char *p,DWORD *n,FATFS **fs){static FATFS data={1026,8};(void)p;*n=100;*fs=&data;return FR_OK;}
static FRESULT f_mkdir(const char *p){mutations++;return mkdir(host(p),0700)?err():FR_OK;}
static FRESULT f_unlink(const char *p){mutations++;return remove(host(p))?err():FR_OK;}
static FRESULT f_rename(const char *a,const char *b){char old[512];strcpy(old,host(a));const char *newpath=host(b);if(!access(newpath,F_OK))return FR_EXIST;mutations++;return rename(old,newpath)?err():FR_OK;}
#define AM_RDO 1
#define AM_HID 2
#define AM_SYS 4
#define AM_ARC 32
static unsigned attr_set,time_set;
static FRESULT f_chmod(const char *p,unsigned a,unsigned mask){assert(mask==39);struct stat st;if(stat(host(p),&st))return err();attr_set=a;return FR_OK;}
static FRESULT f_utime(const char *p,const FILINFO *i){struct stat st;if(stat(host(p),&st))return err();assert(i->fdate==0x5d33 && i->ftime==0x6200);time_set++;return FR_OK;}
#include "sd_fs.h"
static uint8_t req[256],out[512];static uint32_t cookie;
static int32_t call(unsigned op,unsigned n){memset(out,0xcc,sizeof(out));r08_fs(op,req,n,out);return (int32_t)r02_be32(out);}
static unsigned path_request(unsigned off,const char *s){r02_put32(req,cookie);strcpy((char *)req+off,s);return off+strlen(s)+1;}
static int32_t write_at(const char *p,unsigned offset,unsigned n){unsigned plen=strlen(p)+1;r02_put32(req,cookie);r02_put32(req+4,offset);r02_put16(req+8,plen);r02_put16(req+10,n);strcpy((char *)req+12,p);for(unsigned i=0;i<n;i++)req[12+plen+i]=(uint8_t)((offset+i)*37u);return call(23,12+plen+n);}
int main(void){
 assert(mkdtemp(temp));assert(!call(16,0));cookie=r02_be32(out+4);
 assert(!call(22,path_request(4,"\\OUT.TXT")));
 unsigned md=path_request(6,"\\OUT.TXT");r02_put16(req+4,1);assert(!call(27,md) && attr_set==1);
 md=path_request(8,"\\OUT.TXT");r02_put16(req+4,0x6200);r02_put16(req+6,0x5d33);assert(!call(28,md) && time_set==1);
 r02_put16(req+4,0xffff);assert(call(28,md)==-36 && time_set==1);
 md=path_request(6,"\\OUT.TXT");r02_put16(req+4,0);assert(!call(25,md));
 r02_put32(req,cookie);assert(!call(21,4) && r02_be32(out+4)==100);
 const char *bad[]={"\\","\\R08TEST.\\A","\\R08TEST \\A","\\R08TEST\\..\\A","\\R08TEST\\A. ","\\R08TEST\\A.","\\R08TEST\\*","\\R08TEST\\A/B","\\R08TEST\\A:Z","\\R08TEST\\ABCDEFGHI"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++){unsigned before=mutations;assert(call(22,path_request(4,bad[i]))==-36);assert(mutations==before);}
 assert(!call(24,path_request(4,"\\R08TEST")));assert(!call(24,path_request(4,"\\R08TEST\\RUN")));
 const char *p="\\R08TEST\\RUN\\BIG.DAT";assert(!call(22,path_request(4,p)));assert(!live);
 for(unsigned offset=0;offset<70000;offset+=100)assert(write_at(p,offset,100)==100);
 for(unsigned offset=0;offset<70000;offset+=500){unsigned n=path_request(10,p);r02_put32(req+4,offset);r02_put16(req+8,500);assert(call(20,n)==500);for(unsigned i=0;i<500;i++)assert(out[4+i]==(uint8_t)((offset+i)*37u));}
 assert(write_at(p,70001,1)==-64);assert(!live);
 short_write=17;assert(write_at(p,70000,100)==17);short_write=0;
 assert(!call(17,path_request(4,p)) && r02_be32(out+12)==70017);
 unsigned n=path_request(6,p)-6;const char *q="\\R08TEST\\RUN\\NEW.DAT";strcpy((char *)req+6+n,q);r02_put16(req+4,n);assert(!call(26,6+n+strlen(q)+1));
 assert(call(17,path_request(4,p))==-33);
 n=path_request(6,q);r02_put16(req+4,1);assert(call(25,n)==-36); /* Ddelete cannot delete file */
 n=path_request(6,q);r02_put16(req+4,0);assert(!call(25,n));
 assert(!call(22,path_request(4,p)));fail_close=1;assert(write_at(p,0,100)==-65 && !live);fail_close=0;
 unsigned before=mutations;assert(write_at(p,0,100)==-65 && mutations==before);
 assert(!call(16,0));cookie=r02_be32(out+4);
 r02_put32(req,0);assert(call(22,path_request(4,p))==0); /* path helper restores session */
 n=path_request(4,p);r02_put32(req,0);before=mutations;assert(call(22,n)==-46 && mutations==before);
 n=path_request(6,p);r02_put16(req+4,0);assert(!call(25,n));
 n=path_request(6,"\\R08TEST\\RUN");r02_put16(req+4,1);assert(!call(25,n));
 n=path_request(6,"\\R08TEST");r02_put16(req+4,1);assert(!call(25,n));assert(!rmdir(temp));
 puts("R08 service: scoped paths, 70 KB IO, short writes, no holes, rename/delete, closed acknowledgements, failed-close poison and session rejection passed.");
}
