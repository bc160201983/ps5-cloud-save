#ifndef __FreeBSD__
#define _POSIX_C_SOURCE 200809L
#endif
#include "managed.h"
#include "restore.h"
#include "mount.h"
#include "log.h"
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

int pscloud_managed_restore(const char *home,const char *root,const struct pscloud_snapshot *s,const char *archive_name) {
    if(!pscloud_snapshot_valid(s) || strcmp(s->title,"PPSA02433") ||
       (strcmp(s->slot,"PlayerSaveSlot0Save") && strcmp(s->slot,"PlayerSaveProfileSaveData")) || pscloud_no_foreign_mount())return 1;
    if(strchr(archive_name,'/') || strlen(archive_name)>127)return 1;
    char downloads[1400];snprintf(downloads,sizeof downloads,"%s/downloads",root);
    int dir=pscloud_open_directory(downloads);unsigned char *archive=NULL;size_t length=0,payload_size=0;
    const unsigned char *payload=NULL;
    int invalid=dir<0 || pscloud_read_archive(dir,archive_name,&archive,&length) ||
        pscloud_verify_hash(archive,length,s->sha256) || pscloud_save_payload(archive,length,&payload,&payload_size);
    if(dir>=0)close(dir);
    if(invalid) {free(archive);return 1;}
    int parent=pscloud_open_directory(root),lock=-1,sourceparent=-1,original=-1,stagefd=-1,rollback=-1;
    int marker=0,attempted=0,unmounted=0,created=0,committed=0,result=1,target_part_created=0;
    struct pscloud_mount_state state={0};
    char stage[1600]={0},image[1700]={0},mount[1700]={0},id[33]={0},temporary[128]={0};
    struct stat before;char baseline[65];
    if(parent<0)goto cleanup;
    lock=openat(parent,".mount.lock",O_CREAT | O_RDWR | O_NOFOLLOW,0600);
    if(lock<0 || flock(lock,LOCK_EX | LOCK_NB) || pscloud_active_marker(parent)!=0)goto cleanup;
    (void)mkdirat(parent,"staging",0700);(void)mkdirat(parent,"rollback",0700);
    char sourcepath[1400],image_name[96];
    snprintf(sourcepath,sizeof sourcepath,"%s/%s/savedata_prospero/%s",home,s->user,s->title);
    snprintf(image_name,sizeof image_name,"sdimg_%s",s->slot);
    sourceparent=pscloud_open_directory(sourcepath);
    if(sourceparent<0)goto cleanup;
    original=openat(sourceparent,image_name,O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if(original<0 || fstat(original,&before) || !S_ISREG(before.st_mode) || before.st_nlink!=1 || pscloud_file_hash(original,baseline))goto cleanup;
    if(pscloud_random_id(id))goto cleanup;
    char stagebase[1500];snprintf(stagebase,sizeof stagebase,"%s/staging",root);
    int stages=pscloud_open_directory(stagebase);if(stages<0)goto cleanup;
    int made=mkdirat(stages,id,0700)==0;close(stages);if(!made)goto cleanup;
    created=1;snprintf(stage,sizeof stage,"%s/%s",stagebase,id);
    snprintf(image,sizeof image,"%s/image",stage);snprintf(mount,sizeof mount,"%s/mount",stage);
    stagefd=pscloud_open_directory(stage);
    if(stagefd<0)goto cleanup;
    int out=openat(stagefd,"image",O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,0600);
    int copied=out>=0 && pscloud_copy_image(original,out)==0;
    if(out>=0 && close(out))copied=0;
    if(!copied || mkdirat(stagefd,"mount",0700) || fsync(stagefd))goto cleanup;
    char rollbackpath[1500];snprintf(rollbackpath,sizeof rollbackpath,"%s/rollback",root);
    rollback=pscloud_open_directory(rollbackpath);if(rollback<0)goto cleanup;
    char rollbackname[192];snprintf(rollbackname,sizeof rollbackname,"pre-restore-%s-%s-%s-%s.img",s->user,s->title,s->slot,id);
    out=openat(rollback,rollbackname,O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,0600);
    copied=out>=0 && lseek(original,0,SEEK_SET)>=0 && pscloud_copy_image(original,out)==0;
    if(out>=0 && close(out))copied=0;
    if(!copied || fsync(rollback))goto cleanup;
    pscloud_log("INFO","Encrypted original rollback retained: %s/%s",rollbackpath,rollbackname);
    if(pscloud_mount_begin(&state))goto cleanup;
    out=openat(parent,".mount-active",O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,0600);
    if(out<0)goto cleanup;
    marker=1;size_t n=strlen(mount);
    int bad=write(out,mount,n)!=(ssize_t)n || fsync(out);
    if(close(out))bad=1;
    if(bad || fsync(parent))goto cleanup;
    attempted=1;if(pscloud_mount_copy(&state,image,mount))goto cleanup;
    const char *mounted=mount;
#ifdef PSCLOUD_HOST_TEST
    mounted=getenv("PSCLOUD_TEST_PAYLOAD");if(!mounted)goto cleanup;
#endif
    int payload_dir=pscloud_open_directory(mounted);struct stat old;
    if(payload_dir<0)goto cleanup;
    if(fstatat(payload_dir,"ue4savegame.dpx.sav",&old,AT_SYMLINK_NOFOLLOW) || !S_ISREG(old.st_mode)) {close(payload_dir);goto cleanup;}
    out=openat(payload_dir,".pscloud-replacement",O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,old.st_mode&0777);
    bad=out<0;size_t written=0;
    while(!bad && written<payload_size) {
        ssize_t w=write(out,payload+written,payload_size-written);
        if(w<0 && errno==EINTR)continue;
        if(w<=0) {bad=1;break;}written+=(size_t)w;
    }
    if(out>=0) {if(fsync(out))bad=1;if(close(out))bad=1;}
    if(!bad && (renameat(payload_dir,".pscloud-replacement",payload_dir,"ue4savegame.dpx.sav") || fsync(payload_dir)))bad=1;
    if(bad)unlinkat(payload_dir,".pscloud-replacement",0);
    close(payload_dir);if(bad)goto cleanup;
    if(pscloud_mount_end(&state,mount))goto cleanup;
    unmounted=1;
    if(pscloud_mount_leave(&state))goto cleanup;
    char current[65];struct stat present;
    if(pscloud_file_hash(original,current) || strcmp(current,baseline) ||
       fstatat(sourceparent,image_name,&present,AT_SYMLINK_NOFOLLOW) || present.st_ino!=before.st_ino || present.st_dev!=before.st_dev)goto cleanup;
    snprintf(temporary,sizeof temporary,".pscloud-restore-%s.part",id);
    out=openat(sourceparent,temporary,O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,before.st_mode&0777);
    target_part_created=out>=0;
    int encrypted=openat(stagefd,"image",O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    copied=out>=0 && encrypted>=0 && pscloud_copy_image(encrypted,out)==0;
    if(encrypted>=0)close(encrypted);
    if(out>=0 && close(out))copied=0;
    if(!copied || pscloud_file_hash(original,current) || strcmp(current,baseline))goto cleanup;
    if(renameat(sourceparent,temporary,sourceparent,image_name))goto cleanup;
    committed=1;
    if(fsync(sourceparent)) {
        pscloud_notify("Restore replaced image but durability failed - retain rollback and inspect");goto cleanup;
    }
    result=0;pscloud_notify("Console-managed restore complete - rollback retained");
cleanup:
    if(state.mounted) {if(pscloud_mount_end(&state,mount))result=1;else unmounted=1;}
    if(pscloud_mount_leave(&state))result=1;
    if(marker && (!attempted || unmounted)) {if(unlinkat(parent,".mount-active",0) || fsync(parent))result=1;}
    if(!committed && target_part_created && *temporary && sourceparent>=0)unlinkat(sourceparent,temporary,0);
    if(created && !state.mounted && result==0) {unlink(image);rmdir(mount);rmdir(stage);}
    if(rollback>=0)close(rollback);
    if(stagefd>=0)close(stagefd);
    if(original>=0)close(original);
    if(sourceparent>=0)close(sourceparent);
    if(lock>=0)close(lock);
    if(parent>=0)close(parent);
    free(archive);return result;
}
