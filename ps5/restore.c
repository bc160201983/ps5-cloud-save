#define _POSIX_C_SOURCE 200809L
#include "common/restore.h"
#include "common/zip.h"
#include "common/log.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifndef PSCLOUD_HOST_TEST
#include <ps5/kernel.h>
#endif
static int identity(char hex[33]) {
    unsigned char bytes[16];int fd=open("/dev/urandom",O_RDONLY);
    if(fd<0)return -1;
    size_t have=0;
    while(have<sizeof bytes) {
        ssize_t n=read(fd,bytes+have,sizeof bytes-have);
        if(n<0 && errno==EINTR)continue;
        if(n<=0) {close(fd);return -1;}have+=(size_t)n;
    }
    close(fd);for(unsigned i=0;i<16;i++)snprintf(hex+2*i,3,"%02x",bytes[i]);return 0;
}
int main(int argc,char **argv) {
#ifdef PSCLOUD_HOST_TEST
    if(argc!=5)return 2;
    const char *config=argv[1],*downloads=argv[2],*rollback=argv[3],*log=argv[4];
#else
    (void)argc;(void)argv;
    const char *config="/data/pscloud-restore.conf",*downloads="/data/pscloud/downloads",
        *rollback="/data/pscloud/rollback",*log="/data/pscloud.log";
#endif
    pscloud_log_open(log);pscloud_notify("Controlled restore started");
#ifndef PSCLOUD_HOST_TEST
    if((kernel_get_fw_version()&0xffff0000U)!=0x11400000U) {
        pscloud_notify("Restore stopped: firmware 11.40 required");pscloud_log_close();return 2;
    }
#endif
    struct restore_settings s={0};
    if(pscloud_restore_config(config,&s,1)) {
        pscloud_notify("Restore stopped: invalid config or missing confirmations");pscloud_log_close();return 2;
    }
    int source=pscloud_open_directory(downloads);unsigned char *archive=NULL;size_t length=0;
    const unsigned char *data=NULL;size_t size=0;
    int failed=source<0 || pscloud_read_archive(source,s.backup,&archive,&length) ||
        pscloud_verify_hash(archive,length,s.sha256) || pscloud_save_payload(archive,length,&data,&size);
    if(source>=0)close(source);
    if(failed) {free(archive);pscloud_notify("Restore stopped: archive validation failed");pscloud_log_close();return 1;}
    int target=pscloud_open_directory(s.target);struct stat original,current;
    const char *save="ue4savegame.dpx.sav";
    if(target<0 || fstatat(target,save,&original,AT_SYMLINK_NOFOLLOW) ||
       !S_ISREG(original.st_mode) || original.st_nlink!=1) {
        if(target>=0)close(target);
        free(archive);pscloud_notify("Restore stopped: existing destination save unavailable");pscloud_log_close();return 1;
    }
#ifndef PSCLOUD_HOST_TEST
    int parent=pscloud_open_directory("/data/pscloud");
    if(parent<0 || (mkdirat(parent,"rollback",0700) && errno!=EEXIST)) {
        if(parent>=0)close(parent);
        close(target);free(archive);pscloud_notify("Restore stopped: rollback folder unavailable");pscloud_log_close();return 1;
    }
    close(parent);
#endif
    int backupdir=pscloud_open_directory(rollback);char id[33],backup[1400],part[96];
    if(backupdir<0 || identity(id) || snprintf(backup,sizeof backup,"%s/pre-restore-%s-%s.zip",rollback,s.title,id)>=(int)sizeof backup) {
        if(backupdir>=0)close(backupdir);
        close(target);free(archive);pscloud_notify("Restore stopped: rollback unavailable");pscloud_log_close();return 1;
    }
    unsigned files=0;unsigned long long bytes=0;
    failed=pscloud_zip_export(target,backup,&files,&bytes)!=0;
    if(!failed && fsync(backupdir))failed=1;
    if(failed) {
        unlink(backup);close(backupdir);close(target);free(archive);
        pscloud_notify("Restore stopped: destination backup failed");pscloud_log_close();return 1;
    }
    close(backupdir);pscloud_log("INFO","Destination rollback saved: %s",backup);
    snprintf(part,sizeof part,".pscloud-restore-%s.part",id);
    int fd=openat(target,part,O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,original.st_mode&0777);
    int created=fd>=0;
    failed=fd<0;size_t written=0;
    while(!failed && written<size) {
        ssize_t n=write(fd,data+written,size-written);
        if(n<0 && errno==EINTR)continue;
        if(n<=0) {failed=1;break;}written+=(size_t)n;
    }
    if(fd>=0) {if(fsync(fd))failed=1;if(close(fd))failed=1;}
    if(!failed && (fstatat(target,save,&current,AT_SYMLINK_NOFOLLOW) || current.st_ino!=original.st_ino ||
       current.st_dev!=original.st_dev || current.st_size!=original.st_size ||
       current.st_mtim.tv_sec!=original.st_mtim.tv_sec || current.st_mtim.tv_nsec!=original.st_mtim.tv_nsec))failed=1;
    int replaced=0;
    if(!failed) {if(renameat(target,part,target,save))failed=1;else replaced=1;}
    if(replaced && fsync(target))failed=1;
    if(failed && !replaced && created)unlinkat(target,part,0);
    close(target);free(archive);
    if(replaced && failed)pscloud_notify("Save replaced but durability check failed - keep rollback and inspect log");
    else pscloud_notify(failed?"Restore stopped before replacement - rollback retained":
        "Save restored - unmount in Save Mounter before launching game");
    pscloud_log_close();return failed?1:0;
}
