#define _POSIX_C_SOURCE 200809L
#include "common/sharing.h"
#include "common/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "common/restore.h"
/* Testing/advanced entrypoint: export or check only, never live replacement. */
int main(int argc,char **argv) {
    char home[1024]="/user/home",root[1024]="/data/pscloud",meta[1024]="/user/appmeta";
    char mode[16]={0},user[17]={0},title[10]={0},package[128]={0},closed[8]={0};
#ifdef PSCLOUD_HOST_TEST
    if(argc!=8)return 2;
    snprintf(home,sizeof home,"%s",argv[1]);snprintf(root,sizeof root,"%s",argv[2]);snprintf(meta,sizeof meta,"%s",argv[3]);
    snprintf(mode,sizeof mode,"%s",argv[4]);snprintf(user,sizeof user,"%s",argv[5]);snprintf(title,sizeof title,"%s",argv[6]);snprintf(package,sizeof package,"%s",argv[7]);strcpy(closed,"yes");
#else
    (void)argc;(void)argv;FILE *f=fopen("/data/pscloud-share-game.conf","r");if(!f)return 2;
    char line[512];unsigned seen=0;int invalid=0;
    const char *keys[]={"MODE","USER_ID","TITLE","PACKAGE","CONFIRM_GAME_CLOSED"};
    char *values[]={mode,user,title,package,closed};size_t caps[]={sizeof mode,sizeof user,sizeof title,sizeof package,sizeof closed};
    while(fgets(line,sizeof line,f)) {
        size_t n=strlen(line);if(n==sizeof line-1&&line[n-1]!='\n') {invalid=1;break;}while(n&&(line[n-1]=='\n'||line[n-1]=='\r'))line[--n]=0;
        if(!n||line[0]=='#')continue;
        char *eq=strchr(line,'=');if(!eq) {invalid=1;break;}*eq++=0;unsigned i;for(i=0;i<5;i++)if(!strcmp(line,keys[i]))break;
        if(i==5||seen&(1U<<i)||!*eq||strlen(eq)>=caps[i]) {invalid=1;break;}strcpy(values[i],eq);seen|=1U<<i;
    }
    if(ferror(f))invalid=1;
    fclose(f);if(invalid||(seen&23U)!=23U)return 2;
#endif
    if(strcmp(closed,"yes")||(strcmp(mode,"export")&&strcmp(mode,"check")))return 2;
    unsigned char *data=NULL;size_t size=0;int checking=!strcmp(mode,"check");
    if(checking) {
        if(!*package||strchr(package,'/')||strchr(package,'\\')||package[0]=='.')return 2;
        char path[1400];snprintf(path,sizeof path,"%s/share",root);int dir=pscloud_open_directory(path);
        int bad=dir<0||pscloud_read_archive(dir,package,&data,&size);if(dir>=0)close(dir);if(bad)return 2;
    }
    char log[1400],published[128],hash[65];snprintf(log,sizeof log,"%s/share-game.log",root);pscloud_log_open(log);
    int result=pscloud_share_game(home,root,meta,user,title,checking,data,size,published,hash);free(data);
    if(!result&&!checking)pscloud_log("EVENT","PORTABLE_PACKAGE=%s/share/%s; SHA256=%s",root,published,hash);
    pscloud_log_close();return result?1:0;
}
