#define _POSIX_C_SOURCE 200809L
#include "cloud.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

int pscloud_configure(const char *path, struct settings *s) {
    int fd=open(path,O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    struct stat st;
    if(fd<0) return -1;
    if(fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size>8192) {close(fd); return -1;}
    FILE *f=fdopen(fd,"r");
    if(!f) {close(fd); return -1;}
    const char *keys[]={"URL","USER","PASSWORD","CA_BUNDLE","MODE"};
    char *values[]={s->url,s->user,s->password,s->ca,s->mode};
    size_t sizes[]={sizeof s->url,sizeof s->user,sizeof s->password,sizeof s->ca,sizeof s->mode};
    unsigned seen=0; int failed=0; char line[4096];
    while(fgets(line,sizeof line,f)) {
        size_t n=strlen(line);
        if(n==sizeof line-1 && line[n-1]!='\n') {failed=1; break;}
        while(n && (line[n-1]=='\r' || line[n-1]=='\n')) line[--n]=0;
        if(!n || line[0]=='#') continue;
        char *eq=strchr(line,'=');
        if(!eq) {failed=1; break;}
        *eq++=0; unsigned i;
        for(i=0;i<5;i++) if(!strcmp(line,keys[i])) break;
        if(i==5 || (seen & (1U<<i)) || !*eq || strlen(eq)>=sizes[i]) {failed=1; break;}
        strcpy(values[i],eq); seen|=1U<<i;
    }
    if(ferror(f)) failed=1;
    fclose(f);
    return failed || seen!=31 || strncmp(s->url,"https://",8) ||
        strchr(s->url,'?') || strchr(s->url,'#') || s->ca[0]!='/' ||
        (strcmp(s->mode,"once") && strcmp(s->mode,"watch")) ? -1 : 0;
}
