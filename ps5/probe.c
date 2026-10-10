/* Diagnostic only: no save mounts, writes, credentials or kernel patches.
 * Symbol probing only resolves names to see what this firmware exports; nothing
 * resolved is ever called. */
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
#include "common/log.h"
#ifndef PSCLOUD_HOST_TEST
#include <ps5/kernel.h>
extern int sceKernelLoadStartModule(const char *path,size_t args,const void *argp,uint32_t flags,void *option,int *result);
extern int sceKernelDlsym(int handle,const char *symbol,void **address);
#endif

/* Candidate functions for creating a save container on the receiver. Presence is
 * reported per firmware; signatures and behavior remain unknown until tested. */
static const char *const fs_symbols[]={"sceFsInitMountSaveDataOpt","sceFsMountSaveData","sceFsUmountSaveData",
    "sceFsInitCreatePfsSaveDataOpt","sceFsCreatePfsSaveDataImage","sceFsCreatePprPfsSaveDataImage",
    "sceFsInitCreatePprPfsSaveDataOpt","sceFsPprCreate","sceFsCreatePfsTrophyDataImage",NULL};
static const char *const savedata_symbols[]={"sceSaveDataInitialize3","sceSaveDataMount","sceSaveDataMount2",
    "sceSaveDataMount3","sceSaveDataMount5","sceSaveDataUmount","sceSaveDataDirNameSearch","sceSaveDataSetParam",
    "sceSaveDataGetParam","sceSaveDataDelete","sceSaveDataCreateTransactionResource",NULL};
static const char *const regmgr_symbols[]={"sceRegMgrGetBin","sceRegMgrGetInt",NULL};
static void probe_module(FILE *out,const char *label,const char *const *paths,const char *const *symbols) {
#ifdef PSCLOUD_HOST_TEST
    (void)paths;(void)symbols;
    fprintf(out,"module.%s=not-probed-on-host\n",label);
#else
    int handle=-1;const char *loaded=NULL;
    for(;*paths&&handle<0;paths++) {int started=0;handle=sceKernelLoadStartModule(*paths,0,NULL,0,NULL,&started);if(handle>=0)loaded=*paths;}
    if(handle<0) {fprintf(out,"module.%s=unavailable code=%x\n",label,(unsigned)handle);return;}
    fprintf(out,"module.%s=%s\n",label,loaded);
    for(;*symbols;symbols++) {void *address=NULL;int found=!sceKernelDlsym(handle,*symbols,&address)&&address;fprintf(out,"symbol.%s.%s=%s\n",label,*symbols,found?"present":"missing");}
#endif
}
static void probe_symbols(FILE *out) {
    const char *const fs[]={"/system/common/lib/libSceFsInternalForVsh.sprx","libSceFsInternalForVsh.sprx",NULL};
    const char *const savedata[]={"/system/common/lib/libSceSaveData.native.sprx","/system/common/lib/libSceSaveData.sprx","libSceSaveData.sprx",NULL};
    const char *const regmgr[]={"/system/common/lib/libSceRegMgr.sprx","libSceRegMgr.sprx",NULL};
    pscloud_log("INFO","Diagnostic: resolving system library symbol names (no calls)");
    probe_module(out,"fs",fs,fs_symbols);
    probe_module(out,"savedata",savedata,savedata_symbols);
    probe_module(out,"regmgr",regmgr,regmgr_symbols);
}

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
static int scan(FILE *out,const char *root) {
    DIR *users=opendir(root);
    if(!users) {fprintf(out,"user_scan=unavailable errno=%d\n",errno); pscloud_log("ERROR", "Cannot enumerate user save folders: errno=%d", errno); return -1;}
    struct dirent *u;
    unsigned count=0;
    while((u=readdir(users))) {
        if(!identifier(u->d_name)) continue;
        char home[1024]; struct stat st;
        if(snprintf(home,sizeof home,"%s/%s",root,u->d_name)>=(int)sizeof home) continue;
        if(lstat(home,&st)||!S_ISDIR(st.st_mode)) continue;
        count++;
        pscloud_log("INFO", "Scanning save folders for user %u", count);
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
    return 0;
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
    char logpath[1400];
#ifdef PSCLOUD_HOST_TEST
    snprintf(logpath, sizeof logpath, "%s.log", report);
#else
    snprintf(logpath, sizeof logpath, "/data/pscloud.log");
#endif
    if(pscloud_log_open(logpath)) pscloud_notify("Log unavailable; diagnostic will continue");
    pscloud_notify("Diagnostic started - FW %x.%02x", (fw>>24)&255, (fw>>16)&255);
    pscloud_log("INFO", "Diagnostic 0.4; directory enumeration and symbol presence only");
    int fd=open(report,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);
    if(fd<0) {pscloud_notify("Diagnostic failed: cannot write report (errno=%d)", errno); pscloud_log_close(); return 1;}
    FILE *out=fdopen(fd,"w");
    if(!out) {close(fd); pscloud_notify("Diagnostic failed: report stream unavailable"); pscloud_log_close(); return 1;}
    fprintf(out,"PSCloud diagnostic 0.4\nfirmware_hex=%08x\nfirmware=%x.%02x\n",
            fw,(fw>>24)&255,(fw>>16)&255);
    fprintf(out,"scope=directory names and system symbol presence only; no save contents or keys read, no probed function called\n");
    int failed = scan(out,root) != 0;
    probe_symbols(out);
    if(fflush(out)!=0) failed=1;
    if(fsync(fd)!=0) failed=1;
    if(fclose(out)!=0) failed=1;
    printf("PSCloud diagnostic %s: %s\n",failed?"failed":"written",report);
    pscloud_notify(failed ? "Diagnostic finished with errors - see /data/pscloud.log" : "Diagnostic complete - save folders scanned");
    pscloud_log("INFO", "Diagnostic report: %s", report);
    pscloud_log_close();
    return failed?1:0;
}
