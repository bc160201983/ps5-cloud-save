#define _POSIX_C_SOURCE 200809L
#include "savemeta.h"
#include "snapshot.h"
#include "log.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <dirent.h>
#include <string.h>
#include <stdlib.h>
static unsigned r16(const unsigned char *p) {return p[0]|((unsigned)p[1]<<8);}
static uint32_t r32(const unsigned char *p) {return r16(p)|((uint32_t)r16(p+2)<<16);}
static int present(int dir,const char *name) {
    int scan=openat(dir,".",O_RDONLY | O_DIRECTORY | O_NOFOLLOW);if(scan<0)return 0;
    DIR *d=fdopendir(scan);if(!d) {close(scan);return 0;}
    struct dirent *e;int found=0;while((e=readdir(d)))if(!strcmp(e->d_name,name)) {found=1;break;}
    closedir(d);return found;
}
static int read_meta(int payload,struct pscloud_save_meta *meta,char hash[65],unsigned required) {
    memset(meta,0,sizeof *meta);
    if(!present(payload,"sce_sys"))return -1;
    int dir=openat(payload,"sce_sys",O_RDONLY | O_DIRECTORY | O_NOFOLLOW);if(dir<0)return -1;
    if(!present(dir,"param.sfo")) {close(dir);pscloud_log("ERROR","Recovery metadata: param.sfo not present");return -1;}
    int fd=openat(dir,"param.sfo",O_RDONLY | O_NOFOLLOW | O_NONBLOCK);close(dir);struct stat st;
    if(fd<0)return -1;
    if(fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size<20 || st.st_size>65536 || pscloud_file_hash(fd,hash)) {close(fd);return -1;}
    size_t size=(size_t)st.st_size;unsigned char *data=malloc(size);if(!data) {close(fd);return -1;}
    size_t have=0;while(have<size) {ssize_t n=read(fd,data+have,size-have);if(n<=0)break;have+=(size_t)n;}close(fd);
    int bad=have!=size || r32(data)!=0x46535000;
    size_t keys=bad?0:r32(data+8),values=bad?0:r32(data+12),count=bad?0:r32(data+16);
    if(required==0)pscloud_log("INFO","Memory metadata structure: magic=%08x size=%zu keys=%zu values=%zu entries=%zu read=%zu",have>=4?r32(data):0,size,keys,values,count,have);
    if(count>128 || keys<20+16*count || keys>values || values>size)bad=1;
    unsigned seen=0;
    for(size_t i=0;!bad && i<count;i++) {
        const unsigned char *entry=data+20+16*i;size_t ko=r16(entry),offset=r32(entry+12),length=r32(entry+4),allocation=r32(entry+8);
        if(ko>=values-keys || offset>size-values || length>allocation || allocation>size-values-offset) {bad=1;break;}
        const char *key=(const char *)data+keys+ko;size_t max=values-keys-ko;
        if(!memchr(key,0,max)) {bad=1;break;}
        if(required==0)pscloud_log("INFO","Memory metadata entry %zu: format=%04x length=%zu allocation=%zu",i,r16(entry+2),length,allocation);
        const unsigned char *value=data+values+offset;
        unsigned bit=!strcmp(key,"TITLE_ID")?1U:!strcmp(key,"SAVEDATA_DIRECTORY")?2U:!strcmp(key,"ACCOUNT_ID")?4U:0;
        if(!bit)continue;
        if(seen&bit) {bad=1;break;}seen|=bit;
        if(bit==4) {
            if(length!=8 || (r16(entry+2)!=0x0004 && r16(entry+2)!=0x0404)) {bad=1;break;}
            memcpy(meta->account,value,length);meta->account_size=length;
        }else {
            char *out=bit==1?meta->title:meta->slot;size_t capacity=bit==1?sizeof meta->title:sizeof meta->slot;
            const unsigned char *end=length?memchr(value,0,length):NULL;
            if(r16(entry+2)!=0x0204 || !end || (size_t)(end-value)>=capacity) {bad=1;break;}
            memcpy(out,value,(size_t)(end-value));
        }
    }
    free(data);pscloud_log("INFO","Recovery SFO identity: title_field=%s directory_field=%s account_field=%s",*meta->title?"present":"missing",*meta->slot?"present":"missing",meta->account_size?"present":"missing");
    return bad || seen!=required?-1:0;
}
int pscloud_save_meta_read(int payload,struct pscloud_save_meta *meta,char hash[65]) {return read_meta(payload,meta,hash,7);}
int pscloud_save_meta_read_memory(int payload,struct pscloud_save_meta *meta,char hash[65]) {return read_meta(payload,meta,hash,0);}
int pscloud_save_meta_matches(const struct pscloud_save_meta *m,const char *title,const char *slot) {
    return !strcmp(m->title,title) && !strcmp(m->slot,slot) && m->account_size==8;
}
