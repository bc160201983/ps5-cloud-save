#define _POSIX_C_SOURCE 200809L
#include "restore.h"
#include <openssl/evp.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

int pscloud_open_directory(const char *path) {
    if(path[0]!='/' || strlen(path)>=1024) return -1;
    char copy[1024]; strcpy(copy,path);
    int fd=open("/",O_RDONLY | O_DIRECTORY);
    if(fd<0) return -1;
    char *save=NULL,*part=strtok_r(copy,"/",&save);
    while(part) {
        if(!strcmp(part,".") || !strcmp(part,"..")) {close(fd); return -1;}
        int next=openat(fd,part,O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        close(fd); if(next<0)return -1;
        fd=next; part=strtok_r(NULL,"/",&save);
    }
    return fd;
}
int pscloud_restore_config(const char *path,struct restore_settings *s,int restoring) {
    int fd=open(path,O_RDONLY | O_NOFOLLOW | O_NONBLOCK); struct stat st;
    if(fd<0)return -1;
    if(fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size>4096) {close(fd); return -1;}
    FILE *f=fdopen(fd,"r"); if(!f) {close(fd);return -1;}
    const char *keys[]={"BACKUP","SHA256","TITLE","TARGET","CONFIRM_GAME_CLOSED","CONFIRM_RESTORE"};
    char closed[8]={0},confirm[8]={0};
    char *values[]={s->backup,s->sha256,s->title,s->target,closed,confirm};
    size_t sizes[]={sizeof s->backup,sizeof s->sha256,sizeof s->title,sizeof s->target,sizeof closed,sizeof confirm};
    char line[1200]; unsigned seen=0; int bad=0;
    while(fgets(line,sizeof line,f)) {
        size_t n=strlen(line);
        if(n==sizeof line-1 && line[n-1]!='\n') {bad=1;break;}
        while(n && (line[n-1]=='\r' || line[n-1]=='\n'))line[--n]=0;
        if(!n || line[0]=='#')continue;
        char *eq=strchr(line,'='); if(!eq) {bad=1;break;} *eq++=0;
        unsigned i; for(i=0;i<6;i++)if(!strcmp(line,keys[i]))break;
        if(i==6 || (!restoring && i>=3) || (seen&(1U<<i)) || !*eq || strlen(eq)>=sizes[i]) {bad=1;break;}
        strcpy(values[i],eq);seen|=1U<<i;
    }
    if(ferror(f))bad=1;
    fclose(f);
    if(bad || seen!=(restoring?63U:7U) || strcmp(s->title,"PPSA02433") || strlen(s->sha256)!=64)return -1;
    for(unsigned i=0;i<64;i++)if(!strchr("0123456789abcdef",s->sha256[i]))return -1;
    const char *prefix="ps5-11.40-PPSA02433-"; size_t n=strlen(prefix);
    if(strlen(s->backup)!=n+32+4 || strncmp(s->backup,prefix,n) || strcmp(s->backup+n+32,".zip"))return -1;
    for(unsigned i=0;i<32;i++)if(!strchr("0123456789abcdef",s->backup[n+i]))return -1;
    if(restoring && (strcmp(closed,"yes") || strcmp(confirm,"yes") || s->target[0]!='/'))return -1;
#ifndef PSCLOUD_HOST_TEST
    if(restoring && (strncmp(s->target,"/mnt/pfs/",9) || !s->target[9]))return -1;
#endif
    return 0;
}
int pscloud_verify_hash(const unsigned char *data,size_t size,const char *expected) {
    unsigned char digest[EVP_MAX_MD_SIZE]; unsigned length=0; char hex[65];
    if(!EVP_Digest(data,size,digest,&length,EVP_sha256(),NULL) || length!=32)return -1;
    for(unsigned i=0;i<32;i++)snprintf(hex+i*2,3,"%02x",digest[i]);
    return strcmp(hex,expected)?-1:0;
}
static uint16_t u16(const unsigned char *p) {return (uint16_t)(p[0]|p[1]<<8);}
static uint32_t u32(const unsigned char *p) {return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
int pscloud_save_payload(const unsigned char *a,size_t length,const unsigned char **data,size_t *size) {
    const char *name="ue4savegame.dpx.sav"; size_t n=strlen(name);
    if(length<30+n+46+n+22 || length>PSCLOUD_RESTORE_MAX)return -1;
    const unsigned char *end=a+length-22;
    if(u32(end)!=0x06054b50 || u16(end+4) || u16(end+6) || u16(end+8)!=1 || u16(end+10)!=1 || u16(end+20))return -1;
    size_t central=u32(end+16),cs=u32(end+12);
    if(cs!=46+n || central!=length-22-cs || central<30+n)return -1;
    const unsigned char *c=a+central;
    if(u32(c)!=0x02014b50 || u16(c+8)!=u16(a+6) || u16(c+10) || u16(c+28)!=n ||
       u16(c+30) || u16(c+32) || u16(c+34) || u32(c+42) || memcmp(c+46,name,n))return -1;
    if(u32(a)!=0x04034b50 || (u16(a+6)!=0 && u16(a+6)!=0x800) || u16(a+8) ||
       u16(a+26)!=n || u16(a+28) || memcmp(a+30,name,n))return -1;
    size_t payload=u32(a+22);
    if(!payload || payload!=central-30-n || u32(a+18)!=payload || u32(c+20)!=payload || u32(c+24)!=payload || u32(a+14)!=u32(c+16))return -1;
    uint32_t crc=0xffffffffU;
    for(size_t j=0;j<payload;j++) {
        crc^=a[30+n+j];
        for(unsigned k=0;k<8;k++)crc=(crc>>1)^(0xedb88320U&(0U-(crc&1U)));
    }
    if((crc^0xffffffffU)!=u32(a+14))return -1;
    *data=a+30+n; *size=payload;return 0;
}
int pscloud_read_archive(int directory,const char *name,unsigned char **data,size_t *size) {
    int fd=openat(directory,name,O_RDONLY | O_NOFOLLOW | O_NONBLOCK); struct stat st;
    if(fd<0)return -1;
    if(fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size<=0 || st.st_size>PSCLOUD_RESTORE_MAX) {close(fd);return -1;}
    *size=(size_t)st.st_size; *data=malloc(*size);
    if(!*data) {close(fd);return -1;}
    size_t have=0;
    while(have<*size) {
        ssize_t n=read(fd,*data+have,*size-have);
        if(n<0 && errno==EINTR)continue;
        if(n<=0) {free(*data);*data=NULL;close(fd);return -1;}
        have+=(size_t)n;
    }
    close(fd); return 0;
}
