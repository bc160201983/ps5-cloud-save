/* Console-owned staged mount -> archive -> checked unmount -> queue publication.
 * No write-back to /user/home and no Windows Save Mounter dependency. */
#ifndef __FreeBSD__
#define _POSIX_C_SOURCE 200809L
#endif
#include "common/mount.h"
#include "common/restore.h"
#include "common/zip.h"
#include "common/log.h"
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifndef PSCLOUD_HOST_TEST
#include <ps5/kernel.h>
#endif
struct selection {char user[17],title[10],slot[64];};
static int select_save(const char *path,struct selection *s) {
    int fd=open(path,O_RDONLY | O_NOFOLLOW | O_NONBLOCK);struct stat st;
    if(fd<0)return -1;
    if(fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size>2048) {close(fd);return -1;}
    FILE *f=fdopen(fd,"r");if(!f) {close(fd);return -1;}
    char confirmed[8]={0},line[256];unsigned seen=0;int failed=0;
    const char *keys[]={"USER_ID","TITLE","SAVE_NAME","CONFIRM_GAME_CLOSED"};
    char *values[]={s->user,s->title,s->slot,confirmed};
    size_t sizes[]={sizeof s->user,sizeof s->title,sizeof s->slot,sizeof confirmed};
    while(fgets(line,sizeof line,f)) {
        size_t n=strlen(line);
        if(n==sizeof line-1 && line[n-1]!='\n') {failed=1;break;}
        while(n && (line[n-1]=='\n' || line[n-1]=='\r'))line[--n]=0;
        if(!n || line[0]=='#')continue;
        char *eq=strchr(line,'=');if(!eq) {failed=1;break;}*eq++=0;
        unsigned i;for(i=0;i<4;i++)if(!strcmp(line,keys[i]))break;
        if(i==4 || (seen&(1U<<i)) || !*eq || strlen(eq)>=sizes[i]) {failed=1;break;}
        strcpy(values[i],eq);seen|=1U<<i;
    }
    if(ferror(f))failed=1;
    fclose(f);
    if(failed || seen!=15 || strcmp(confirmed,"yes") || strcmp(s->title,"PPSA02433") ||
       (strcmp(s->slot,"PlayerSaveSlot0Save") && strcmp(s->slot,"PlayerSaveProfileSaveData")))return -1;
    for(const char *p=s->user;*p;p++)if(!strchr("0123456789abcdef",*p))return -1;
    return 0;
}
static int random_id(char id[33]) {
    unsigned char b[16];int fd=open("/dev/urandom",O_RDONLY);if(fd<0)return -1;
    size_t have=0;
    while(have<sizeof b) {
        ssize_t n=read(fd,b+have,sizeof b-have);
        if(n<0 && errno==EINTR)continue;
        if(n<=0) {close(fd);return -1;}have+=(size_t)n;
    }
    close(fd);for(unsigned i=0;i<16;i++)snprintf(id+i*2,3,"%02x",b[i]);return 0;
}
static int copy_image(int source,int destination) {
    struct stat before,after;
    if(fstat(source,&before) || !S_ISREG(before.st_mode) || before.st_size<0x860 ||
       before.st_size>2LL*1024*1024*1024)return -1;
    unsigned char type;
    if(pread(source,&type,1,0)!=1 || type!=2)return -1;
    unsigned char data[65536];off_t copied=0;
    while(copied<before.st_size) {
        ssize_t n=read(source,data,sizeof data);
        if(n<0 && errno==EINTR)continue;
        if(n<=0)return -1;
        size_t have=0;
        while(have<(size_t)n) {
            ssize_t w=write(destination,data+have,(size_t)n-have);
            if(w<0 && errno==EINTR)continue;
            if(w<=0)return -1;
            have+=(size_t)w;
        }
        copied+=n;
    }
    if(fstat(source,&after) || after.st_size!=before.st_size ||
       after.st_mtim.tv_sec!=before.st_mtim.tv_sec || after.st_mtim.tv_nsec!=before.st_mtim.tv_nsec ||
       after.st_ctim.tv_sec!=before.st_ctim.tv_sec || after.st_ctim.tv_nsec!=before.st_ctim.tv_nsec)return -1;
    return fsync(destination);
}
static int no_foreign_mount(void) {
#ifdef PSCLOUD_HOST_TEST
    return getenv("PSCLOUD_TEST_FOREIGN_MOUNT")?-1:0;
#else
    DIR *d=opendir("/mnt/pfs");if(!d)return -1;
    struct dirent *e;int busy=0;
    while((e=readdir(d)))if(strcmp(e->d_name,".") && strcmp(e->d_name,".."))busy=1;
    closedir(d);return busy?-1:0;
#endif
}
static int active_marker(int parent) {
    int scan=openat(parent,".",O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if(scan<0)return -1;
    DIR *d=fdopendir(scan);if(!d) {close(scan);return -1;}
    struct dirent *entry;int found=0;
    while(1) {
        errno=0;entry=readdir(d);
        if(!entry) {if(errno)found=-1;break;}
        if(!strcmp(entry->d_name,".mount-active")) {found=1;break;}
    }
    closedir(d);return found;
}
int main(int argc,char **argv) {
#ifdef PSCLOUD_HOST_TEST
    if(argc!=5)return 2;
    const char *config=argv[1],*home=argv[2],*root=argv[3],*log=argv[4];
#else
    (void)argc;(void)argv;
    const char *config="/data/pscloud-backup.conf",*home="/user/home",
        *root="/data/pscloud",*log="/data/pscloud.log";
#endif
    pscloud_log_open(log);pscloud_notify("Console-managed backup started");
    struct selection s={0};int result=1;
#ifndef PSCLOUD_HOST_TEST
    if((kernel_get_fw_version()&0xffff0000U)!=0x11400000U)goto config_error;
#endif
    if(select_save(config,&s))goto config_error;
    if(no_foreign_mount()) {pscloud_notify("Backup stopped: unmount other saves first");goto finish;}
    char sourcepath[1400];snprintf(sourcepath,sizeof sourcepath,"%s/%s/savedata_prospero/%s",home,s.user,s.title);
    int sourceparent=pscloud_open_directory(sourcepath);
    if(sourceparent<0)goto finish;
    char image_name[80];snprintf(image_name,sizeof image_name,"sdimg_%s",s.slot);
    int original=openat(sourceparent,image_name,O_RDONLY | O_NOFOLLOW | O_NONBLOCK);close(sourceparent);
    if(original<0)goto finish;
    int parent=pscloud_open_directory(root);
    if(parent<0) {close(original);goto finish;}
    int lock=openat(parent,".mount.lock",O_CREAT | O_RDWR | O_NOFOLLOW,0600);
    if(lock<0 || flock(lock,LOCK_EX | LOCK_NB)) {
        if(lock>=0)close(lock);
        close(parent);close(original);goto finish;
    }
    if(active_marker(parent)!=0) {
        pscloud_notify("Backup stopped: prior staged mount requires inspection");
        close(lock);close(parent);close(original);goto finish;
    }
    int initialized=(mkdirat(parent,"staging",0700)==0 || errno==EEXIST) &&
        (mkdirat(parent,"spool",0700)==0 || errno==EEXIST);
    char stagebase[1400],spoolpath[1400];
    snprintf(stagebase,sizeof stagebase,"%s/staging",root);snprintf(spoolpath,sizeof spoolpath,"%s/spool",root);
    int stages=initialized?pscloud_open_directory(stagebase):-1;
    int spool=initialized?pscloud_open_directory(spoolpath):-1;
    char id[33],stage[1500],image[1600],mount[1600],name[128],part[1800],ready[1800];
    int created=0;struct pscloud_mount_state state={0};int part_created=0;
    int marker_created=0,mount_attempted=0,unmounted=0;
    if(stages<0 || spool<0 || random_id(id))goto cleanup;
    if(mkdirat(stages,id,0700))goto cleanup;
    created=1;
    snprintf(stage,sizeof stage,"%s/%s",stagebase,id);
    snprintf(image,sizeof image,"%s/image",stage);snprintf(mount,sizeof mount,"%s/mount",stage);
    snprintf(name,sizeof name,"ps5-11.40-%s-%s.zip",s.title,id);
    snprintf(part,sizeof part,"%s/%s.part",spoolpath,name);snprintf(ready,sizeof ready,"%s/%s.ready",spoolpath,name);
    int stagefd=pscloud_open_directory(stage);
    if(stagefd<0)goto cleanup;
    int destination=openat(stagefd,"image",O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,0600);
    int copied=destination>=0 && copy_image(original,destination)==0;
    if(destination>=0 && close(destination))copied=0;
    if(!copied || mkdirat(stagefd,"mount",0700) || fsync(stagefd)) {close(stagefd);goto cleanup;}
    close(stagefd);close(original);original=-1;
    pscloud_log("INFO","Selection: user=%s title=%s save=%s; staged copy=%s",s.user,s.title,s.slot,image);
    if(pscloud_mount_begin(&state))goto cleanup;
    int marker=openat(parent,".mount-active",O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,0600);
    if(marker<0)goto cleanup;
    marker_created=1;
    size_t mountlen=strlen(mount);
    int marker_bad=write(marker,mount,mountlen)!=(ssize_t)mountlen || fsync(marker);
    if(close(marker))marker_bad=1;
    if(marker_bad || fsync(parent))goto cleanup;
    mount_attempted=1;
    if(pscloud_mount_copy(&state,image,mount))goto cleanup;
    const char *payload=mount;
#ifdef PSCLOUD_HOST_TEST
    payload=getenv("PSCLOUD_TEST_PAYLOAD");if(!payload)goto cleanup;
#endif
    int mounted=pscloud_open_directory(payload);
    if(mounted<0)goto cleanup;
    unsigned files=0;unsigned long long bytes=0;
    part_created=1;
    int exported=pscloud_zip_export(mounted,part,&files,&bytes)==0;close(mounted);
    if(!exported)goto cleanup;
    if(pscloud_mount_end(&state,mount))goto cleanup;
    unmounted=1;
    if(pscloud_mount_leave(&state))goto cleanup;
    /* Bind the queue snapshot to its exact user/title/slot in a local sidecar. */
    char identity[160];snprintf(identity,sizeof identity,"%s.identity",name);
    int meta=openat(spool,identity,O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,0600);
    if(meta<0)goto cleanup;
    FILE *out=fdopen(meta,"w");if(!out) {close(meta);goto cleanup;}
    int bad=fprintf(out,"USER_ID=%s\nTITLE=%s\nSAVE_NAME=%s\n",s.user,s.title,s.slot)<0;
    if(fflush(out) || fsync(meta))bad=1;
    if(fclose(out))bad=1;
    if(bad || rename(part,ready) || fsync(spool))goto cleanup;
    part_created=0;result=0;pscloud_notify("Console backup ready: %u files - original untouched",files);
cleanup:
    if(state.mounted) {
        if(pscloud_mount_end(&state,mount)) {
            result=1;pscloud_log("ERROR","Unmount failed; staged image retained for inspection");
        } else unmounted=1;
    }
    if(pscloud_mount_leave(&state))result=1;
    if(marker_created && (!mount_attempted || unmounted)) {
        if(unlinkat(parent,".mount-active",0) || fsync(parent))result=1;
    }
    if(part_created)unlink(part);
    /* Keep failed images/mounts for diagnosis; never delete a mounted image. */
    if(created && !state.mounted && result==0) {
        unlink(image);rmdir(mount);rmdir(stage);
    }
    if(stages>=0)close(stages);
    if(spool>=0)close(spool);
    close(lock);close(parent);if(original>=0)close(original);
    goto finish;
config_error:
    result=2;pscloud_notify("Backup stopped: firmware/config/game-closed confirmation invalid");
finish:
    if(result==1)pscloud_notify("Console backup failed - original save was not replaced");
    pscloud_log_close();return result;
}
