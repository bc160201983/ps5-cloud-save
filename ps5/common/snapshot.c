#define _POSIX_C_SOURCE 200809L
#include "snapshot.h"
#include <openssl/evp.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
static int component(const char *s,unsigned max) {
    size_t n=strlen(s);if(!n || n>max)return 0;
    for(;*s;s++)if(!strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-",*s))return 0;
    return 1;
}
int pscloud_snapshot_valid(const struct pscloud_snapshot *s) {
    if(!component(s->user,16) || strlen(s->title)!=9 || strncmp(s->title,"PPSA",4) || !component(s->slot,63) || strlen(s->sha256)!=64)return 0;
    for(const char *p=s->user;*p;p++)if(!strchr("0123456789abcdef",*p))return 0;
    for(unsigned i=4;i<9;i++)if(s->title[i]<'0' || s->title[i]>'9')return 0;
    for(const char *p=s->sha256;*p;p++)if(!strchr("0123456789abcdef",*p))return 0;
    return 1;
}
int pscloud_snapshot_read(int dir,const char *name,struct pscloud_snapshot *s) {
    int fd=openat(dir,name,O_RDONLY | O_NOFOLLOW | O_NONBLOCK);struct stat st;
    if(fd<0)return -1;
    if(fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size>1024) {close(fd);return -1;}
    FILE *f=fdopen(fd,"r");if(!f) {close(fd);return -1;}
    const char *keys[]={"USER_ID","TITLE","SAVE_NAME","SHA256"};
    char *values[]={s->user,s->title,s->slot,s->sha256};
    size_t sizes[]={sizeof s->user,sizeof s->title,sizeof s->slot,sizeof s->sha256};
    char line[256];unsigned seen=0;int bad=0;
    while(fgets(line,sizeof line,f)) {
        size_t n=strlen(line);if(n==sizeof line-1 && line[n-1]!='\n') {bad=1;break;}
        while(n && (line[n-1]=='\r' || line[n-1]=='\n'))line[--n]=0;
        char *eq=strchr(line,'=');if(!eq) {bad=1;break;}*eq++=0;
        unsigned i;for(i=0;i<4;i++)if(!strcmp(line,keys[i]))break;
        if(i==4 || (seen&(1U<<i)) || !*eq || strlen(eq)>=sizes[i]) {bad=1;break;}
        strcpy(values[i],eq);seen|=1U<<i;
    }
    if(ferror(f))bad=1;
    fclose(f);return bad || seen!=15 || !pscloud_snapshot_valid(s)?-1:0;
}
int pscloud_file_hash(int fd,char hex[65]) {
    struct stat before,after;
    if(fstat(fd,&before) || !S_ISREG(before.st_mode))return -1;
    EVP_MD_CTX *ctx=EVP_MD_CTX_new();if(!ctx)return -1;
    int good=EVP_DigestInit_ex(ctx,EVP_sha256(),NULL);unsigned char b[65536],digest[EVP_MAX_MD_SIZE];
    off_t offset=0;unsigned n=0;
    while(good && offset<before.st_size) {
        ssize_t got=pread(fd,b,sizeof b,offset);
        if(got<0 && errno==EINTR)continue;
        if(got<=0) {good=0;break;}
        good=EVP_DigestUpdate(ctx,b,(size_t)got);offset+=got;
    }
    if(fstat(fd,&after) || before.st_size!=after.st_size || before.st_mtim.tv_sec!=after.st_mtim.tv_sec || before.st_mtim.tv_nsec!=after.st_mtim.tv_nsec)good=0;
    if(good)good=EVP_DigestFinal_ex(ctx,digest,&n);
    EVP_MD_CTX_free(ctx);if(!good || n!=32)return -1;
    for(unsigned i=0;i<32;i++)snprintf(hex+2*i,3,"%02x",digest[i]);
    return 0;
}
int pscloud_snapshot_exists(int dir,const struct pscloud_snapshot *wanted) {
    int scan=openat(dir,".",O_RDONLY | O_DIRECTORY | O_NOFOLLOW);if(scan<0)return -1;
    DIR *d=fdopendir(scan);if(!d) {close(scan);return -1;}
    struct dirent *e;int same=0;
    while((e=readdir(d))) {
        size_t n=strlen(e->d_name);
        if(n<10 || n>200 || strcmp(e->d_name+n-9,".identity"))continue;
        struct pscloud_snapshot s={0};
        if(pscloud_snapshot_read(dir,e->d_name,&s) || strcmp(s.user,wanted->user) || strcmp(s.title,wanted->title) ||
           strcmp(s.slot,wanted->slot) || strcmp(s.sha256,wanted->sha256))continue;
        char file[256];int fd=-1;
        for(unsigned i=0;i<2 && fd<0;i++) {
            snprintf(file,sizeof file,"%.*s.%s",(int)n-9,e->d_name,i?"sent":"ready");
            fd=openat(dir,file,O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
        }
        char hash[65];if(fd>=0) {if(!pscloud_file_hash(fd,hash) && !strcmp(hash,wanted->sha256))same=1;close(fd);}
        if(same)break;
    }
    closedir(d);return same;
}
int pscloud_snapshot_folder(const struct pscloud_snapshot *s,char *folder,unsigned size) {
    if(!pscloud_snapshot_valid(s))return -1;
    const char *game=!strcmp(s->title,"PPSA02433")?"Crash%20Bandicoot%204%20-%20PPSA02433":s->title;
    int n=snprintf(folder,size,"%s/User-%s/%s",game,s->user,s->slot);
    return n<0 || (unsigned)n>=size?-1:0;
}
