/* Diagnostic only: no save mounts, writes, credentials or kernel patches. */
#define _POSIX_C_SOURCE 200809L
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#ifndef PSCLOUD_HOST_TEST
#include <ps5/kernel.h>
#endif

static int identifier(const char *s) {
    if (!*s || strlen(s)>16) return 0;
    for (;*s;s++) if (!isxdigit((unsigned char)*s)) return 0;
    return 1;
}
static int title_id(const char *s) {
    if(strlen(s)!=9 || (strncmp(s,"PPSA",4) && strncmp(s,"CUSA",4))) return 0;
    for(int i=4;i<9;i++) if(!isdigit((unsigned char)s[i])) return 0;
    return 1;
}
static void scan(FILE *out,const char *root) {
    DIR *users=opendir(root);
    if(!users) {fprintf(out,"user_scan=unavailable errno=%d\n",errno); return;}
    struct dirent *u;
    unsigned count=0;
    while((u=readdir(users))) {
        if(!identifier(u->d_name)) continue;
        char home[1024]; struct stat st;
        if(snprintf(home,sizeof home,"%s/%s",root,u->d_name)>=(int)sizeof home) continue;
        if(lstat(home,&st)||!S_ISDIR(st.st_mode)) continue;
        count++;
        const char *types[]={"savedata_prospero","savedata"};
        for(unsigned i=0;i<2;i++) {
            char path[1200]; snprintf(path,sizeof path,"%s/%s",home,types[i]);
            if(lstat(path,&st)||!S_ISDIR(st.st_mode)) {
                fprintf(out,"user_%u.%s=unavailable\n",count,types[i]); continue;
            }
            DIR *d=opendir(path);
            if(!d) {fprintf(out,"user_%u.%s=unavailable errno=%d\n",count,types[i],errno); continue;}
            struct dirent *e; unsigned titles=0;
            while((e=readdir(d))) {
                if(!title_id(e->d_name)) continue;
                char full[1400]; snprintf(full,sizeof full,"%s/%s",path,e->d_name);
                if(lstat(full,&st)||!S_ISDIR(st.st_mode)) continue;
                fprintf(out,"user_%u.%s.title=%s\n",count,types[i],e->d_name);
                titles++;
            }
            closedir(d);
            fprintf(out,"user_%u.%s.count=%u\n",count,types[i],titles);
        }
    }
    closedir(users);
    fprintf(out,"users=%u\n",count);
}
int main(int argc,char **argv) {
#ifdef PSCLOUD_HOST_TEST
    if(argc!=3) return 2;
    const char *root=argv[1],*report=argv[2];
    uint32_t fw=0;
#else
    (void)argc; (void)argv;
    const char *root="/user/home",*report="/data/pscloud-probe.txt";
    uint32_t fw=kernel_get_fw_version();
#endif
    int fd=open(report,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);
    if(fd<0) {perror("diagnostic report"); return 1;}
    FILE *out=fdopen(fd,"w");
    if(!out) {close(fd); return 1;}
    fprintf(out,"PSCloud diagnostic 0.2\nfirmware_hex=%08x\nfirmware=%x.%02x\n",
            fw,(fw>>24)&255,(fw>>16)&255);
    fprintf(out,"scope=directory names only; no save contents or keys read\n");
    scan(out,root);
    int failed=fflush(out)!=0;
    if(fsync(fd)!=0) failed=1;
    if(fclose(out)!=0) failed=1;
    printf("PSCloud diagnostic %s: %s\n",failed?"failed":"written",report);
    return failed?1:0;
}
