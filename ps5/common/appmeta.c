#define _POSIX_C_SOURCE 200809L
#include "appmeta.h"
#include "restore.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
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
static char *read_json(const char *path) {
    int fd=open(path,O_RDONLY | O_NOFOLLOW | O_NONBLOCK);if(fd<0)return NULL;
    struct stat st;if(fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size<=0 || st.st_size>65536) {close(fd);return NULL;}
    char *json=malloc((size_t)st.st_size+1);if(!json) {close(fd);return NULL;}
    size_t have=0;
    while(have<(size_t)st.st_size) {ssize_t n=read(fd,json+have,(size_t)st.st_size-have);if(n<=0)break;have+=(size_t)n;}
    close(fd);json[have]=0;return json;
}
/* First string value of "key" anywhere in the document. */
static int json_string(const char *json,const char *key,char *out,size_t max) {
    char quoted[64];snprintf(quoted,sizeof quoted,"\"%s\"",key);
    const char *at=strstr(json,quoted);if(!at)return -1;
    const char *p=at+strlen(quoted);while(*p==' '||*p=='\n'||*p=='\r'||*p=='\t')p++;
    if(*p!=':')return -1;
    p++;while(*p==' '||*p=='\n'||*p=='\r'||*p=='\t')p++;
    return string_value(p,out,max);
}
/* Finds the metadata folder for a title: <root>/<title>, else a folder (in <root> or
 * <root>/external, possibly named by content ID) whose param.json has this titleId, else an
 * installed game that keeps its saves under this ID (savedata.titleIdForSharing; e.g. Ghost
 * of Tsushima PPSA03210 saves as PPSA02225). */
int pscloud_app_meta_dir(const char *root,const char *title,char *out,size_t max) {
    if(!max||!pscloud_title_valid(title))return -1;
    char path[1400];struct stat st;
    if(snprintf(path,sizeof path,"%s/%s/param.json",root,title)<(int)sizeof path && !stat(path,&st) && S_ISREG(st.st_mode)) {
        return snprintf(out,max,"%s/%s",root,title)<(int)max?0:-1;
    }
    char sharing[1400]={0};
    for(int b=0;b<2;b++) {
        char base[1200];if(snprintf(base,sizeof base,b?"%s/external":"%s",root)>=(int)sizeof base)continue;
        DIR *d=opendir(base);if(!d)continue;
        struct dirent *e;
        while((e=readdir(d))) {
            if(e->d_name[0]=='.'||strlen(e->d_name)>96||strchr(e->d_name,'/'))continue;
            if(snprintf(path,sizeof path,"%s/%s/param.json",base,e->d_name)>=(int)sizeof path)continue;
            char *json=read_json(path);if(!json)continue;
            char id[32]={0},shared[32]={0};
            int exact=!json_string(json,"titleId",id,sizeof id)&&!strcmp(id,title);
            int shares=!exact&&!*sharing&&!json_string(json,"titleIdForSharing",shared,sizeof shared)&&!strcmp(shared,title);
            free(json);
            if(exact) {closedir(d);return snprintf(out,max,"%s/%s",base,e->d_name)<(int)max?0:-1;}
            if(shares)snprintf(sharing,sizeof sharing,"%s/%s",base,e->d_name);
        }
        closedir(d);
    }
    if(!*sharing)return -1;
    return snprintf(out,max,"%s",sharing)<(int)max?0:-1;
}
int pscloud_app_name(const char *root,const char *title,char *name,size_t max) {
    if(!max || !pscloud_title_valid(title))return -1;
    snprintf(name,max,"%s",!strcmp(title,"PPSA02433")?"Crash Bandicoot 4":title);
    char path[1400];if(pscloud_app_meta_dir(root,title,path,sizeof path))return 0;
    char file[1500];snprintf(file,sizeof file,"%s/param.json",path);
    char *json=read_json(file);if(!json)return 0;
    const char *language=json;
    while((language=strstr(language,"\"en-US\""))) {const char *after=language+7;while(*after==' ' || *after=='\n' || *after=='\r' || *after=='\t')after++;if(*after==':')break;language+=7;}
    const char *key=strstr(language?language:json,"\"titleName\"");
    if(key) {const char *p=strchr(key,':');if(p) {p++;while(*p==' ' || *p=='\n' || *p=='\r' || *p=='\t')p++;char decoded[256]={0};if(!string_value(p,decoded,sizeof decoded))snprintf(name,max,"%s",decoded);}}
    free(json);return 0;
}
int pscloud_app_version(const char *root,const char *title,char *version,size_t max) {
    if(!max||!pscloud_title_valid(title))return -1;
    char path[1400];if(pscloud_app_meta_dir(root,title,path,sizeof path))return -1;
    int dir=pscloud_open_directory(path);if(dir<0)return -1;
    unsigned char *data=NULL;size_t size=0;
    int bad=pscloud_read_archive(dir,"param.json",&data,&size);close(dir);
    if(bad||size>65536) {free(data);return -1;}
    char *json=malloc(size+1);if(!json) {free(data);return -1;}
    memcpy(json,data,size);json[size]=0;free(data);bad=1;
    const char *key=strstr(json,"\"contentVersion\"");
    if(key&&!strstr(key+16,"\"contentVersion\"")) {
        const char *p=key+16;while(*p==' '||*p=='\n'||*p=='\r'||*p=='\t')p++;
        if(*p==':') {p++;while(*p==' '||*p=='\n'||*p=='\r'||*p=='\t')p++;
            if(!string_value(p,version,max)) {bad=0;for(const char *q=version;*q;q++)if((*q<'0'||*q>'9')&&*q!='.')bad=1;}}
    }
    free(json);return bad?-1:0;
}
