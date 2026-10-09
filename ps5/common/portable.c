#define _POSIX_C_SOURCE 200809L
#include "portable.h"
#include "zip.h"
#include "snapshot.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <openssl/evp.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
static const char *names[]={"progress.dat","profile.dat","manifest.txt"};
static unsigned u16(const unsigned char *p) {return p[0]|((unsigned)p[1]<<8);}
static uint32_t u32(const unsigned char *p) {return u16(p)|((uint32_t)u16(p+2)<<16);}
static uint32_t crc(const unsigned char *p,size_t n) {uint32_t c=0xffffffffU;while(n--) {c^=*p++;for(unsigned i=0;i<8;i++)c=(c>>1)^(0xedb88320U&(0U-(c&1U)));}return c^0xffffffffU;}
static int hash(const unsigned char *p,size_t n,char out[65]) {unsigned char d[32];unsigned size=0;if(!EVP_Digest(p,n,d,&size,EVP_sha256(),NULL)||size!=32)return -1;for(unsigned i=0;i<32;i++)snprintf(out+2*i,3,"%02x",d[i]);return 0;}
static int manifest(char *out,size_t cap,unsigned fw,long long created,const char *a,const char *b) {
    if((fw!=0x07000000U&&fw!=0x11400000U)||created<0)return -1;
    int n=snprintf(out,cap,"FORMAT=PSCLOUD_PORTABLE_CRASH_V1\nTITLE=PPSA02433\nSOURCE_FIRMWARE=%08x\nCREATED_UNIX=%lld\nPROGRESS_SHA256=%s\nPROFILE_SHA256=%s\n",fw,created,a,b);
    return n<0||(size_t)n>=cap?-1:n;
}
int pscloud_portable_parse(const unsigned char *a,size_t length,struct pscloud_portable *p) {
    memset(p,0,sizeof *p);if(length<22||length>PSCLOUD_PORTABLE_MAX)return -1;
    const unsigned char *end=a+length-22;
    if(u32(end)!=0x06054b50||u16(end+4)||u16(end+6)||u16(end+8)!=3||u16(end+10)!=3||u16(end+20))return -1;
    size_t central=u32(end+16),pos=central,local=0;
    if(central>length-22||u32(end+12)!=length-22-central)return -1;
    const unsigned char *parts[3];size_t sizes[3];
    for(unsigned i=0;i<3;i++) {
        size_t n=strlen(names[i]);
        if(pos>length-22||length-22-pos<46+n||local>central||central-local<30+n)return -1;
        const unsigned char *c=a+pos,*l=a+local;size_t bytes=u32(l+22);
        if(u32(c)!=0x02014b50||u32(l)!=0x04034b50||(u16(l+6)!=0&&u16(l+6)!=0x800)||u16(c+8)!=u16(l+6)||
           u16(c+10)||u16(l+8)||u16(c+28)!=n||u16(l+26)!=n||u16(c+30)||u16(c+32)||u16(c+34)||u16(c+36)||u32(c+38)||
           u16(l+28)||u32(c+42)!=local||memcmp(c+46,names[i],n)||memcmp(l+30,names[i],n)||u32(l+18)!=bytes||u32(c+20)!=bytes||
           u32(c+24)!=bytes||u32(c+16)!=u32(l+14)||bytes>central-local-30-n||!bytes||bytes>(i==2?511U:32U*1024*1024))return -1;
        parts[i]=l+30+n;sizes[i]=bytes;if(crc(parts[i],bytes)!=u32(l+14))return -1;
        local+=30+n+bytes;pos+=46+n;
    }
    if(local!=central||pos!=length-22)return -1;
    char text[512],expected[512],digest[2][65];memcpy(text,parts[2],sizes[2]);text[sizes[2]]=0;
    const char *prefix="FORMAT=PSCLOUD_PORTABLE_CRASH_V1\nTITLE=PPSA02433\nSOURCE_FIRMWARE=";
    if(memchr(parts[2],0,sizes[2])||strncmp(text,prefix,strlen(prefix)))return -1;
    char *after=NULL;errno=0;unsigned long fw=strtoul(text+strlen(prefix),&after,16);
    if(errno||fw>UINT_MAX||strncmp(after,"\nCREATED_UNIX=",14))return -1;
    char *created=after+14;errno=0;p->created=strtoll(created,&after,10);p->firmware=(unsigned)fw;
    if(errno||after==created||*after!='\n'||p->created<0)return -1;
    for(unsigned i=0;i<2;i++)if(hash(parts[i],sizes[i],digest[i]))return -1;
    int n=manifest(expected,sizeof expected,p->firmware,p->created,digest[0],digest[1]);
    if(n<0||(size_t)n!=sizes[2]||memcmp(expected,parts[2],sizes[2]))return -1;
    for(unsigned i=0;i<2;i++) {p->payload[i]=parts[i];p->size[i]=sizes[i];}return 0;
}
int pscloud_portable_export(int stage,const char *destination,unsigned fw,long long created) {
    char digest[2][65],text[512];
    for(unsigned i=0;i<2;i++) {int fd=openat(stage,names[i],O_RDONLY|O_NOFOLLOW|O_NONBLOCK);struct stat st;
        int bad=fd<0||fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_nlink!=1||st.st_size<=0||st.st_size>32*1024*1024||pscloud_file_hash(fd,digest[i]);if(fd>=0)close(fd);if(bad)return -1;}
    int n=manifest(text,sizeof text,fw,created,digest[0],digest[1]);if(n<0)return -1;
    int fd=openat(stage,names[2],O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);if(fd<0)return -1;
    int bad=write(fd,text,(size_t)n)!=n||fsync(fd);if(close(fd))bad=1;if(bad)return -1;
    unsigned files=0;unsigned long long bytes=0;return pscloud_zip_export_named(stage,destination,names,3,&files,&bytes)||files!=3?-1:0;
}
