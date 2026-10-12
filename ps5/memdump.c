/* Diagnostic: copies the loaded program image of the running game into /data/pscloud/dump
 * so its constants (for example a save encryption seed) can be searched offline. It only
 * reads the game's memory through the system debug service; the game is not changed.
 * Optional /data/pscloud/memdump.conf: "TITLE=PPSAxxxxx" picks the game; default: the first
 * running PPSA title. Output: <title>.mem (readable chunks back to back) and <title>.map. */
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ps5/kernel.h>
#include <ps5/mdbg.h>

int sceKernelGetAppInfo(pid_t pid,void *info);
typedef struct {char reserved[45];char message[3075];} notification;
int sceKernelSendNotificationRequest(int,notification *,size_t,int);

#define FOLDER "/data/pscloud/dump"
#define CHUNK (64*1024)
#define MAX_TOTAL (1024LL*1024*1024)
#define MAX_GAP (256LL*1024*1024)

static void notify(const char *text) {notification n;memset(&n,0,sizeof n);snprintf(n.message,sizeof n.message,"PSCloud: %s",text);sceKernelSendNotificationRequest(0,&n,sizeof n,0);}
static int title_of(pid_t pid,char out[10]) {
    unsigned char info[256];memset(info,0,sizeof info);if(sceKernelGetAppInfo(pid,info))return -1;
    memcpy(out,info+0x10,9);out[9]=0;return strncmp(out,"PPSA",4)?-1:0;
}
static pid_t find_game(const char *wanted,char title[10]) {
    int mib[4]={CTL_KERN,KERN_PROC,KERN_PROC_PROC,0};size_t size=0;
    if(sysctl(mib,4,NULL,&size,NULL,0)||!size)return -1;
    size+=size/4;char *buffer=malloc(size);if(!buffer)return -1;
    if(sysctl(mib,4,buffer,&size,NULL,0)) {free(buffer);return -1;}
    pid_t found=-1;
    for(size_t offset=0;offset+sizeof(int)<=size;) {
        struct kinfo_proc *ki=(struct kinfo_proc *)(buffer+offset);
        if(ki->ki_structsize<=0||offset+(size_t)ki->ki_structsize>size)break;
        offset+=(size_t)ki->ki_structsize;
        char t[10];if(title_of(ki->ki_pid,t))continue;
        if(*wanted&&strcmp(t,wanted))continue;
        if(strstr(ki->ki_comm,"eboot")||found<0) {found=ki->ki_pid;strcpy(title,t);if(strstr(ki->ki_comm,"eboot"))break;}
    }
    free(buffer);return found;
}
int main(void) {
    char wanted[16]="",title[10]="",line[64];FILE *conf=fopen("/data/pscloud/memdump.conf","r");
    if(conf) {if(fgets(line,sizeof line,conf)&&!strncmp(line,"TITLE=",6)) {line[strcspn(line,"\r\n")]=0;snprintf(wanted,sizeof wanted,"%.9s",line+6);}fclose(conf);}
    pid_t pid=find_game(wanted,title);
    if(pid<0) {printf("PSCLOUD_MEMDUMP no running game%s%s\n",*wanted?" ":"",wanted);notify("Memory dump: start the game first");return 1;}
    uint32_t handle=0;if(kernel_dynlib_handle(pid,"eboot.bin",&handle))handle=0;
    intptr_t base=kernel_dynlib_mapbase_addr(pid,handle);
    printf("PSCLOUD_MEMDUMP game %s pid %d handle %u base 0x%llx\n",title,(int)pid,(unsigned)handle,(unsigned long long)base);
    if(!base) {notify("Memory dump: program address not found");return 1;}
    mkdir("/data/pscloud",0700);mkdir(FOLDER,0700);
    char path[128];snprintf(path,sizeof path,FOLDER "/%s.mem",title);int out=open(path,O_CREAT|O_TRUNC|O_WRONLY,0600);
    snprintf(path,sizeof path,FOLDER "/%s.map",title);FILE *map=fopen(path,"w");
    if(out<0||!map) {notify("Memory dump: cannot write to /data/pscloud/dump");return 1;}
    fprintf(map,"title %s pid %d base 0x%llx\n",title,(int)pid,(unsigned long long)base);
    unsigned char *chunk=malloc(CHUNK);long long total=0,gap=0,run_start=-1,file_offset=0;
    for(intptr_t addr=base;chunk&&total<MAX_TOTAL&&gap<MAX_GAP;addr+=CHUNK) {
        if(mdbg_copyout(pid,addr,chunk,CHUNK)==0) {
            if(run_start<0) {run_start=addr;fprintf(map,"region 0x%llx file %lld\n",(unsigned long long)addr,file_offset);}
            if(write(out,chunk,CHUNK)!=CHUNK)break;
            total+=CHUNK;file_offset+=CHUNK;gap=0;
        } else {
            if(run_start>=0) {fprintf(map,"end 0x%llx\n",(unsigned long long)addr);run_start=-1;}
            gap+=CHUNK;
        }
    }
    if(run_start>=0)fprintf(map,"end of dump\n");
    fprintf(map,"bytes %lld\n",total);fclose(map);close(out);free(chunk);
    printf("PSCLOUD_MEMDUMP wrote %lld bytes for %s\n",total,title);
    snprintf(line,sizeof line,"Memory dump of %s done (%lld MB)",title,total>>20);notify(line);
    return 0;
}
