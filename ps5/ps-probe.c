/* Read-only diagnostic: prints the process list and each process's app info the way
 * the dashboard's running-game check reads it. Changes nothing on the console. */
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int sceKernelGetAppInfo(pid_t pid,void *info);

int main(void) {
    int mib[4]={CTL_KERN,KERN_PROC,KERN_PROC_PROC,0};size_t size=0;
    int r=sysctl(mib,4,NULL,&size,NULL,0);
    printf("PSCLOUD_PS sysctl=%d size=%zu kinfo_proc=%zu self=%d\n",r,size,sizeof(struct kinfo_proc),(int)getpid());
    if(r||!size)return 1;
    size+=size/4;char *buffer=malloc(size);if(!buffer)return 1;
    r=sysctl(mib,4,buffer,&size,NULL,0);printf("PSCLOUD_PS sysctl2=%d size=%zu\n",r,size);
    if(r) {free(buffer);return 1;}
    unsigned count=0;
    for(size_t offset=0;offset+sizeof(int)<=size;) {
        struct kinfo_proc *ki=(struct kinfo_proc *)(buffer+offset);
        if(ki->ki_structsize<=0||offset+(size_t)ki->ki_structsize>size) {printf("PSCLOUD_PS stop structsize=%d\n",ki->ki_structsize);break;}
        offset+=(size_t)ki->ki_structsize;count++;
        unsigned char info[256];memset(info,0,sizeof info);
        int a=sceKernelGetAppInfo(ki->ki_pid,info);
        printf("PSCLOUD_PS pid=%d comm=%.19s appinfo=%d raw=",ki->ki_pid,ki->ki_comm,a);
        for(int i=0;i<48;i++)printf("%02x",info[i]);
        printf(" text=");
        for(int i=0;i<48;i++)putchar(info[i]>=32&&info[i]<127?info[i]:'.');
        putchar('\n');
    }
    printf("PSCLOUD_PS processes=%u\n",count);fflush(stdout);free(buffer);return 0;
}
