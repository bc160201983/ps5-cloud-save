#define _POSIX_C_SOURCE 200809L
#include "zip.h"
#include "log.h"
#include <sys/stat.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#define MAX_FILES 4096
#define MAX_ARCHIVE (2ULL * 1024 * 1024 * 1024)
struct entry {char *name; uint32_t crc, size, offset;};
struct writer {FILE *out; struct entry *entries; unsigned count; unsigned long long bytes, progress_blocks;};
static int write16(FILE *f, uint16_t n) {
    unsigned char b[2] = {(unsigned char)n, (unsigned char)(n>>8)};
    return fwrite(b, 1, 2, f)==2 ? 0 : -1;
}
static int write32(FILE *f, uint32_t n) {
    return write16(f, (uint16_t)n) || write16(f, (uint16_t)(n>>16)) ? -1 : 0;
}
static uint32_t crc_update(uint32_t crc, const unsigned char *p, size_t n) {
    while(n--) {
        crc ^= *p++;
        for(unsigned i=0;i<8;i++) crc = (crc>>1) ^ (0xedb88320U & (0U-(crc&1U)));
    }
    return crc;
}
static int add_file(struct writer *w, int dir, const char *name, const char *relative) {
    if(w->count >= MAX_FILES) {errno=EFBIG; return -1;}
    int fd=openat(dir, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if(fd<0) return -1;
    struct stat before, after;
    if(fstat(fd,&before) || !S_ISREG(before.st_mode) || before.st_size<0 ||
       (unsigned long long)before.st_size>MAX_ARCHIVE) {close(fd); errno=EINVAL; return -1;}
    off_t offset=ftello(w->out);
    size_t len=strlen(relative);
    if(offset<0 || (unsigned long long)offset+(unsigned long long)before.st_size+len+30 > MAX_ARCHIVE) {
        close(fd); errno=EFBIG; return -1;
    }
    char *copy=strdup(relative);
    if(!copy) {close(fd); return -1;}
    uint32_t size=(uint32_t)before.st_size;
    int failed=write32(w->out,0x04034b50) || write16(w->out,20) || write16(w->out,0x800) ||
        write16(w->out,0) || write16(w->out,0) || write16(w->out,0x21) ||
        write32(w->out,0) || write32(w->out,size) || write32(w->out,size) ||
        write16(w->out,(uint16_t)len) || write16(w->out,0) || fwrite(relative,1,len,w->out)!=len;
    unsigned char buffer[65536]; uint32_t crc=0xffffffffU; unsigned long long copied=0;
    while(!failed && copied<size) {
        size_t want=size-copied<sizeof buffer ? (size_t)(size-copied) : sizeof buffer;
        ssize_t n=read(fd,buffer,want);
        if(n<0 && errno==EINTR) continue;
        if(n<=0 || fwrite(buffer,1,(size_t)n,w->out)!=(size_t)n) {failed=1; break;}
        crc=crc_update(crc,buffer,(size_t)n); copied+=(size_t)n;
        unsigned long long blocks=(w->bytes+copied)/(16ULL*1024*1024);
        if(blocks>w->progress_blocks) {
            w->progress_blocks=blocks;
            pscloud_notify("Export progress: %llu MiB copied",(w->bytes+copied)/(1024*1024));
        }
    }
    if(fstat(fd,&after) || before.st_size!=after.st_size ||
       before.st_mtim.tv_sec!=after.st_mtim.tv_sec || before.st_mtim.tv_nsec!=after.st_mtim.tv_nsec ||
       before.st_ctim.tv_sec!=after.st_ctim.tv_sec || before.st_ctim.tv_nsec!=after.st_ctim.tv_nsec) {
        errno=EBUSY; failed=1;
    }
    close(fd);
    crc ^= 0xffffffffU;
    if(!failed) {
        off_t end=ftello(w->out);
        if(end<0 || fseeko(w->out,offset+14,SEEK_SET) || write32(w->out,crc) || fseeko(w->out,end,SEEK_SET)) failed=1;
    }
    if(failed) {free(copy); return -1;}
    w->entries[w->count++] = (struct entry){copy,crc,size,(uint32_t)offset};
    w->bytes+=size;
    pscloud_log("INFO","Exported payload file %u: %s (%u bytes)",w->count,relative,size);
    if(w->count%25==0) pscloud_notify("Export progress: %u files copied",w->count);
    return 0;
}
static int walk(struct writer *w,int root,const char *prefix,unsigned depth) {
    if(depth>32) {errno=ELOOP; return -1;}
    struct stat before,after;
    if(fstat(root,&before)) return -1;
    int scan=openat(root,".",O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if(scan<0) return -1;
    DIR *d=fdopendir(scan);
    if(!d) {close(scan); return -1;}
    struct dirent *e; int failed=0;
    while(1) {
        errno=0; e=readdir(d);
        if(!e) {if(errno)failed=1; break;}
        for(const unsigned char *p=(const unsigned char *)e->d_name; *p; p++) {
            if(*p<32 || *p==127 || *p=='\\') {errno=EINVAL; failed=1; break;}
        }
        if(failed) break;
        if(!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")) continue;
        if(!*prefix && !strcmp(e->d_name,"sce_sys")) {
            pscloud_log("INFO","Excluded destination-specific sce_sys metadata"); continue;
        }
        char relative[1024];
        int n=snprintf(relative,sizeof relative,"%s%s%s",prefix,*prefix?"/":"",e->d_name);
        if(n<0 || n>=(int)sizeof relative) {errno=ENAMETOOLONG; failed=1; break;}
        struct stat st;
        if(fstatat(root,e->d_name,&st,AT_SYMLINK_NOFOLLOW)) {failed=1; break;}
        if(S_ISDIR(st.st_mode)) {
            int child=openat(root,e->d_name,O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
            if(child<0) {failed=1; break;}
            failed=walk(w,child,relative,depth+1)!=0;
            close(child);
        } else if(S_ISREG(st.st_mode)) {
            failed=add_file(w,root,e->d_name,relative)!=0;
        } else {
            pscloud_log("ERROR","Rejected non-regular source entry: %s",relative);
            errno=EINVAL; failed=1;
        }
        if(failed) break;
    }
    int saved=errno;
    closedir(d); errno=saved;
    if(!failed && (fstat(root,&after) || before.st_mtim.tv_sec!=after.st_mtim.tv_sec ||
       before.st_mtim.tv_nsec!=after.st_mtim.tv_nsec)) {errno=EBUSY; failed=1;}
    return failed ? -1 : 0;
}
int pscloud_zip_export(int source_fd,const char *destination,unsigned *files,unsigned long long *bytes) {
    *files=0; *bytes=0;
    int fd=open(destination,O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,0600);
    if(fd<0) return -1;
    struct writer w={0};
    w.out=fdopen(fd,"wb"); w.entries=calloc(MAX_FILES,sizeof *w.entries);
    if(!w.out || !w.entries) {if(w.out)fclose(w.out); else close(fd); free(w.entries); return -1;}
    int failed=walk(&w,source_fd,"",0)!=0;
    if(!failed && !w.count) {errno=ENOENT; failed=1;}
    off_t start=ftello(w.out);
    if(start<0) failed=1;
    for(unsigned i=0;!failed && i<w.count;i++) {
        struct entry *e=&w.entries[i]; size_t n=strlen(e->name);
        failed=write32(w.out,0x02014b50) || write16(w.out,20) || write16(w.out,20) ||
            write16(w.out,0x800) || write16(w.out,0) || write16(w.out,0) || write16(w.out,0x21) ||
            write32(w.out,e->crc) || write32(w.out,e->size) || write32(w.out,e->size) ||
            write16(w.out,(uint16_t)n) || write16(w.out,0) || write16(w.out,0) ||
            write16(w.out,0) || write16(w.out,0) || write32(w.out,0) || write32(w.out,e->offset) ||
            fwrite(e->name,1,n,w.out)!=n;
    }
    off_t end=ftello(w.out);
    if(end<0 || (unsigned long long)end>MAX_ARCHIVE) {errno=EFBIG; failed=1;}
    if(!failed) failed=write32(w.out,0x06054b50) || write16(w.out,0) || write16(w.out,0) ||
        write16(w.out,(uint16_t)w.count) || write16(w.out,(uint16_t)w.count) ||
        write32(w.out,(uint32_t)(end-start)) || write32(w.out,(uint32_t)start) || write16(w.out,0);
    int saved=errno;
    if(fflush(w.out) || fsync(fd)) {saved=errno; failed=1;}
    if(fclose(w.out)) {saved=errno; failed=1;}
    *files=w.count; *bytes=w.bytes;
    for(unsigned i=0;i<w.count;i++) free(w.entries[i].name);
    free(w.entries); errno=saved;
    return failed ? -1 : 0;
}
