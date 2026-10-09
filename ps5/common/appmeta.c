#define _POSIX_C_SOURCE 200809L
#include "appmeta.h"
#include "restore.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int pscloud_title_valid(const char *s) {
    if(strlen(s)!=9 || strncmp(s,"PPSA",4))return 0;
    for(unsigned i=4;i<9;i++)if(s[i]<'0' || s[i]>'9')return 0;
    return 1;
}
static int string_value(const char *p,char *out,size_t max) {
    if(*p!='"')return -1;
    p++;size_t n=0;
    while(*p && *p!='"') {
        unsigned char ch=(unsigned char)*p++;
        if(ch=='\\') {
            ch=(unsigned char)*p++;if(!ch)return -1;
            if(ch=='u') {
                unsigned value=0;
                for(unsigned i=0;i<4;i++) {char digit=*p++;if(!digit)return -1;const char *at=strchr("0123456789abcdef",digit);if(!at)return -1;value=value*16+(unsigned)(at-"0123456789abcdef");}
                if(value>=0xd800 && value<=0xdfff)return -1;
                unsigned char bytes[3];size_t count=0;
                if(value<128)bytes[count++]=(unsigned char)value;
                else if(value<2048) {bytes[count++]=(unsigned char)(0xc0|(value>>6));bytes[count++]=(unsigned char)(0x80|(value&63));}
                else {bytes[count++]=(unsigned char)(0xe0|(value>>12));bytes[count++]=(unsigned char)(0x80|((value>>6)&63));bytes[count++]=(unsigned char)(0x80|(value&63));}
                if(!value || n+count>=max)return -1;
                memcpy(out+n,bytes,count);n+=count;continue;
            }
            if(ch=='n' || ch=='r' || ch=='t' || ch=='b' || ch=='f')ch=' ';
            else if(ch!='"' && ch!='\\' && ch!='/')return -1;
        }
        if(ch<32 || n+1>=max)return -1;
        out[n++]=(char)ch;
    }
    if(*p!='"' || !n)return -1;
    out[n]=0;return 0;
}
int pscloud_app_name(const char *root,const char *title,char *name,size_t max) {
    if(!max || !pscloud_title_valid(title))return -1;
    snprintf(name,max,"%s",!strcmp(title,"PPSA02433")?"Crash Bandicoot 4":title);
    char path[1400];if(snprintf(path,sizeof path,"%s/%s",root,title)>=(int)sizeof path)return -1;
    int dir=pscloud_open_directory(path);if(dir<0)return 0;
    int fd=openat(dir,"param.json",O_RDONLY | O_NOFOLLOW | O_NONBLOCK);close(dir);
    if(fd<0)return 0;
    struct stat st;if(fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size<=0 || st.st_size>65536) {close(fd);return 0;}
    char *json=malloc((size_t)st.st_size+1);if(!json) {close(fd);return -1;}
    size_t have=0;
    while(have<(size_t)st.st_size) {ssize_t n=read(fd,json+have,(size_t)st.st_size-have);if(n<=0)break;have+=(size_t)n;}
    close(fd);json[have]=0;
    const char *language=json;
    while((language=strstr(language,"\"en-US\""))) {const char *after=language+7;while(*after==' ' || *after=='\n' || *after=='\r' || *after=='\t')after++;if(*after==':')break;language+=7;}
    const char *key=strstr(language?language:json,"\"titleName\"");
    if(key) {const char *p=strchr(key,':');if(p) {p++;while(*p==' ' || *p=='\n' || *p=='\r' || *p=='\t')p++;char decoded[256]={0};if(!string_value(p,decoded,sizeof decoded))snprintf(name,max,"%s",decoded);}}
    free(json);return 0;
}
