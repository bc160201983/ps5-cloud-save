/* FW 11.40 manual test: archive an explicitly mounted, closed game's save.
 * Mount ownership stays with the user's Save Mounter session. No auto-mount,
 * original-save writes, restore or cloud upload occurs in this payload. */
#define _POSIX_C_SOURCE 200809L
#include "common/log.h"
#include "common/zip.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifndef PSCLOUD_HOST_TEST
#include <ps5/kernel.h>
#endif

static int open_directory(const char *path) {
    if(path[0]!='/') {errno=EINVAL; return -1;}
    char copy[1024];
    if(strlen(path)>=sizeof copy) {errno=ENAMETOOLONG; return -1;}
    strcpy(copy,path);
    int fd=open("/",O_RDONLY | O_DIRECTORY);
    if(fd<0) return -1;
    char *save=NULL, *part=strtok_r(copy,"/",&save);
    while(part) {
        if(!strcmp(part,".") || !strcmp(part,"..")) {close(fd); errno=EINVAL; return -1;}
        int next=openat(fd,part,O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        close(fd); if(next<0) return -1;
        fd=next; part=strtok_r(NULL,"/",&save);
    }
    return fd;
}
static int read_config(const char *path,char *source,size_t size,char title[10]) {
    int fd=open(path,O_RDONLY | O_NOFOLLOW);
    if(fd<0) return -1;
    FILE *f=fdopen(fd,"r"); if(!f) {close(fd); return -1;}
    char line[1200]; int source_seen=0,title_seen=0,closed_seen=0,confirmed=0,failed=0;
    while(fgets(line,sizeof line,f)) {
        size_t n=strlen(line);
        if(n==sizeof line-1 && line[n-1]!='\n') {failed=1; break;}
        while(n && (line[n-1]=='\n' || line[n-1]=='\r')) line[--n]=0;
        if(!n || line[0]=='#') continue;
        if(!strncmp(line,"SOURCE=",7) && !source_seen) {
            if(strlen(line+7)>=size) {failed=1; break;}
            strcpy(source,line+7); source_seen=1;
        } else if(!strncmp(line,"TITLE=",6) && !title_seen) {
            if(strlen(line+6)!=9 || strncmp(line+6,"PPSA",4)) {failed=1; break;}
            for(int i=10;i<15;i++) if(line[i]<'0' || line[i]>'9') failed=1;
            strcpy(title,line+6); title_seen=1;
        } else if(!strncmp(line,"CONFIRM_GAME_CLOSED=",20) && !closed_seen) {
            confirmed=!strcmp(line+20,"yes"); closed_seen=1;
        } else {failed=1; break;}
    }
    if(ferror(f)) failed=1;
    fclose(f);
    if(failed || !source_seen || !title_seen || !closed_seen || !confirmed) {errno=EINVAL; return -1;}
#ifndef PSCLOUD_HOST_TEST
    if(strncmp(source,"/mnt/pfs/",9) || !source[9]) {errno=EINVAL; return -1;}
#endif
    return 0;
}
static int random_name(char *name,size_t size,const char *title) {
    unsigned char bytes[16]; int fd=open("/dev/urandom",O_RDONLY);
    if(fd<0) return -1;
    size_t have=0;
    while(have<sizeof bytes) {
        ssize_t n=read(fd,bytes+have,sizeof bytes-have);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) {close(fd); errno=EIO; return -1;}
        have+=(size_t)n;
    }
    close(fd); char hex[33];
    for(unsigned i=0;i<16;i++) snprintf(hex+i*2,3,"%02x",bytes[i]);
    return snprintf(name,size,"ps5-11.40-%s-%s.zip",title,hex)>=(int)size ? -1 : 0;
}
int main(int argc,char **argv) {
    const char *config,*spool,*logpath;
#ifdef PSCLOUD_HOST_TEST
    if(argc!=4) return 2;
    config=argv[1]; spool=argv[2]; logpath=argv[3];
#else
    (void)argc; (void)argv;
    config="/data/pscloud-export.conf"; spool="/data/pscloud/spool"; logpath="/data/pscloud.log";
#endif
    if(pscloud_log_open(logpath)) pscloud_notify("Export log unavailable; stdout logging continues");
    pscloud_notify("Save export payload started");
#ifndef PSCLOUD_HOST_TEST
    uint32_t fw=kernel_get_fw_version();
    if((fw & 0xffff0000U)!=0x11400000U) {
        pscloud_notify("Export stopped: this test targets firmware 11.40");
        pscloud_log_close(); return 1;
    }
#endif
    char source[1024]={0},title[10]={0};
    pscloud_log("INFO","Reading export configuration");
    if(read_config(config,source,sizeof source,title)) {
        pscloud_notify("Export stopped: missing/invalid mounted-save config");
        pscloud_log("ERROR","Expected SOURCE, TITLE and CONFIRM_GAME_CLOSED=yes in %s",config);
        pscloud_log_close(); return 1;
    }
    pscloud_log("INFO","Game closure is user-confirmed; payload cannot independently detect it");
    int dir=open_directory(source);
    if(dir<0) {
        pscloud_notify("Export failed: mounted save unavailable (errno=%d)",errno);
        pscloud_log_close(); return 1;
    }
#ifndef PSCLOUD_HOST_TEST
    if(mkdir("/data/pscloud",0700) && errno!=EEXIST) {
        close(dir); pscloud_notify("Export failed: cannot create backup folder"); pscloud_log_close(); return 1;
    }
    int parent=open_directory("/data/pscloud");
    if(parent<0 || (mkdirat(parent,"spool",0700) && errno!=EEXIST)) {
        if(parent>=0)close(parent);
        close(dir); pscloud_notify("Export failed: backup folder unavailable"); pscloud_log_close(); return 1;
    }
    close(parent);
#endif
    int spoolfd=open_directory(spool);
    if(spoolfd<0) {close(dir); pscloud_notify("Export failed: spool unavailable"); pscloud_log_close(); return 1;}
    char name[128],part[1400],ready[1400];
    if(random_name(name,sizeof name,title)) {
        close(spoolfd); close(dir); pscloud_notify("Export failed: cannot generate backup ID"); pscloud_log_close(); return 1;
    }
    snprintf(part,sizeof part,"%s/%s.part",spool,name);
    snprintf(ready,sizeof ready,"%s/%s.ready",spool,name);
    pscloud_notify("Exporting %s - please keep the save mounted",title);
    unsigned files=0; unsigned long long bytes=0;
    int failed=pscloud_zip_export(dir,part,&files,&bytes)!=0;
    close(dir);
    if(failed) {
        int saved=errno;
        unlink(part);
        pscloud_notify("Export failed - partial backup removed");
        pscloud_log("ERROR","Archive creation failed: errno=%d (%s)",saved,strerror(saved));
    } else if(rename(part,ready) || fsync(spoolfd)) {
        pscloud_notify("Export failed while committing backup - see log");
        pscloud_log("ERROR","Queue commit failed: errno=%d",errno);
        failed=1;
    } else {
        pscloud_log("INFO","Backup ready: %s; files=%u bytes=%llu",ready,files,bytes);
        pscloud_notify("Export complete: %u files - backup saved locally",files);
        pscloud_log("INFO","Cloud upload is not enabled in this payload. Save Mounter still owns the mount.");
    }
    close(spoolfd); pscloud_log_close(); return failed?1:0;
}
