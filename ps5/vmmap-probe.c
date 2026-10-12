/* Diagnostic: lists memory regions of the system menu (SceShellUI) and of this probe with
 * sysctl KERN_PROC_VMMAP, and reads one page through the system debug service, to check
 * what live mode can use on this firmware. Read only. */
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ps5/kernel.h>
#include <ps5/mdbg.h>

#ifndef KERN_PROC_VMMAP
#define KERN_PROC_VMMAP 32
#endif

static pid_t find(const char *name) {
    int mib[4]={CTL_KERN,KERN_PROC,KERN_PROC_PROC,0};size_t size=0;
    if(sysctl(mib,4,NULL,&size,NULL,0)||!size)return -1;
    size+=size/4;char *buffer=malloc(size);if(!buffer)return -1;
    if(sysctl(mib,4,buffer,&size,NULL,0)) {free(buffer);return -1;}
    pid_t found=-1;
    for(size_t offset=0;offset+sizeof(int)<=size;) {
        struct kinfo_proc *ki=(struct kinfo_proc *)(buffer+offset);
        if(ki->ki_structsize<=0||offset+(size_t)ki->ki_structsize>size)break;
        offset+=(size_t)ki->ki_structsize;
        if(strstr(ki->ki_comm,name)) {found=ki->ki_pid;break;}
    }
    free(buffer);return found;
}
static void regions(pid_t pid,const char *label) {
    int mib[4]={CTL_KERN,KERN_PROC,KERN_PROC_VMMAP,(int)pid};size_t size=0;
    int r=sysctl(mib,4,NULL,&size,NULL,0);
    printf("PSCLOUD_VMMAP %s pid %d size query %d bytes %zu kinfo_vmentry %zu\n",label,(int)pid,r,size,sizeof(struct kinfo_vmentry));
    if(r||!size)return;
    size=size*4/3;char *buffer=malloc(size);if(!buffer)return;
    r=sysctl(mib,4,buffer,&size,NULL,0);printf("PSCLOUD_VMMAP %s read %d bytes %zu\n",label,r,size);
    unsigned count=0;unsigned long long rw=0,readable=0;int shown=0;
    for(size_t offset=0;!r&&offset+sizeof(int)<=size;) {
        struct kinfo_vmentry *e=(struct kinfo_vmentry *)(buffer+offset);
        if(e->kve_structsize<=0||offset+(size_t)e->kve_structsize>size) {printf("PSCLOUD_VMMAP stop structsize %d\n",e->kve_structsize);break;}
        offset+=(size_t)e->kve_structsize;count++;
        unsigned long long len=(unsigned long long)(e->kve_end-e->kve_start);
        if(e->kve_protection&KVME_PROT_READ)readable+=len;
        if((e->kve_protection&KVME_PROT_READ)&&(e->kve_protection&KVME_PROT_WRITE))rw+=len;
        if(shown<40) {shown++;printf("PSCLOUD_VMMAP %s 0x%llx-0x%llx prot %d type %d path %.60s\n",label,(unsigned long long)e->kve_start,(unsigned long long)e->kve_end,e->kve_protection,e->kve_type,e->kve_path);}
        if(count==1&&pid!=getpid()) {unsigned char page[4096];int m=mdbg_copyout(pid,(intptr_t)e->kve_start,page,sizeof page);printf("PSCLOUD_VMMAP %s debug read of first region: %d\n",label,m);}
    }
    printf("PSCLOUD_VMMAP %s regions %u readable %llu MB read-write %llu MB\n",label,count,readable>>20,rw>>20);
    free(buffer);
}
int main(void) {
    regions(getpid(),"self");
    pid_t shell=find("SceShellUI");
    if(shell>0)regions(shell,"SceShellUI");else printf("PSCLOUD_VMMAP SceShellUI not found\n");
    pid_t game=find("eboot");
    if(game>0)regions(game,"game");else printf("PSCLOUD_VMMAP no game running\n");
    fflush(stdout);return 0;
}
