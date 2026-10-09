#ifndef __FreeBSD__
#define _POSIX_C_SOURCE 200809L
#endif
#include "bundle.h"
#include "managed.h"
#include "restore.h"
#include "snapshot.h"
#include "zip.h"
#include "log.h"
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>

int pscloud_game_backup(const char *home,const char *root,const char *user,const char *title) {
    struct pscloud_snapshot s={0};
    if(strlen(user)>=sizeof s.user || strlen(title)>=sizeof s.title)return -1;
    strcpy(s.user,user);strcpy(s.title,title);strcpy(s.slot,"WholeGame");
    memset(s.sha256,'0',64);
    if(!pscloud_snapshot_valid(&s) || strcmp(title,"PPSA02433"))return -1;
    const char *names[]={"sdimg_PlayerSaveSlot0Save","sdimg_PlayerSaveProfileSaveData"};
    int result=-1,parent=-1,lock=-1,source=-1,stage=-1,spool=-1;
    char id[33],stage_name[64]={0},source_path[1400],part[1600]={0};
    char name[128],identity[144],ready[144],hashes[2][65];
    int sources[2]={-1,-1};struct stat before[2],directory_before,directory_after;unsigned copied=0;
    parent=pscloud_open_directory(root);if(parent<0)goto done;
    lock=openat(parent,".mount.lock",O_CREAT | O_RDWR | O_NOFOLLOW,0600);
    if(lock<0 || flock(lock,LOCK_EX | LOCK_NB) || pscloud_active_marker(parent)!=0)goto done;
    snprintf(source_path,sizeof source_path,"%s/%s/savedata_prospero/%s",home,user,title);
    source=pscloud_open_directory(source_path);if(source<0)goto done;
    if(fstat(source,&directory_before))goto done;
    int scan=openat(source,".",O_RDONLY | O_DIRECTORY | O_NOFOLLOW);if(scan<0)goto done;
    DIR *directory=fdopendir(scan);if(!directory) {close(scan);goto done;}
    struct dirent *entry;int unsupported=0;
    while(1) {
        errno=0;entry=readdir(directory);
        if(!entry) {if(errno)unsupported=1;break;}
        if(!strncmp(entry->d_name,"sdimg_",6) && strncmp(entry->d_name,"sdimg_sce_bu_",13) &&
           strcmp(entry->d_name,names[0]) && strcmp(entry->d_name,names[1]))unsupported=1;
    }
    closedir(directory);
    if(unsupported) {pscloud_log("ERROR","Additional save slots found; refusing an incomplete whole-game backup");goto done;}
    /* Open and hash both originals before copying either. Reject missing slots,
     * symlinks, changing images, or a path replaced during the operation. */
    for(unsigned i=0;i<2;i++) {
        sources[i]=openat(source,names[i],O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
        if(sources[i]<0 || fstat(sources[i],&before[i]) || pscloud_file_hash(sources[i],hashes[i]))goto done;
    }
    if(pscloud_random_id(id))goto done;
    snprintf(stage_name,sizeof stage_name,"bundle-%s",id);
    if(mkdirat(parent,stage_name,0700)) {stage_name[0]=0;goto done;}
    stage=openat(parent,stage_name,O_RDONLY | O_DIRECTORY | O_NOFOLLOW);if(stage<0)goto done;
    for(unsigned i=0;i<2;i++) {
        int out=openat(stage,names[i],O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,0600);
        if(out<0)goto done;
        copied=i+1;int bad=pscloud_copy_image(sources[i],out);if(close(out))bad=1;
        if(bad)goto done;
        int check=openat(stage,names[i],O_RDONLY | O_NOFOLLOW | O_NONBLOCK);char hash[65];
        bad=check<0 || pscloud_file_hash(check,hash) || strcmp(hash,hashes[i]);
        if(check>=0)close(check);
        if(bad)goto done;
    }
    int manifest=openat(stage,"manifest.txt",O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,0600);
    if(manifest<0)goto done;
    FILE *out=fdopen(manifest,"w");if(!out) {close(manifest);goto done;}
    int bad=fprintf(out,"FORMAT=PSCLOUD_ENCRYPTED_GAME_V1\nFIRMWARE=11.40\nUSER_ID=%s\nTITLE=%s\n%s=%s\n%s=%s\n",user,title,names[0],hashes[0],names[1],hashes[1])<0;
    if(fflush(out) || fsync(manifest))bad=1;
    if(fclose(out))bad=1;
    if(bad || fsync(stage))goto done;
    (void)mkdirat(parent,"spool",0700);
    spool=openat(parent,"spool",O_RDONLY | O_DIRECTORY | O_NOFOLLOW);if(spool<0)goto done;
    snprintf(name,sizeof name,"ps5-11.40-%s-%s.zip",title,id);
    snprintf(part,sizeof part,"%s/spool/%s.part",root,name);
    unsigned files=0;unsigned long long bytes=0;
    const char *entries[]={names[0],names[1],"manifest.txt"};
    if(pscloud_zip_export_named(stage,part,entries,3,&files,&bytes) || files!=3)goto done;
    for(unsigned i=0;i<2;i++) {
        int check=openat(source,names[i],O_RDONLY | O_NOFOLLOW | O_NONBLOCK);struct stat now;char hash[65];
        bad=check<0 || fstat(check,&now) || now.st_dev!=before[i].st_dev || now.st_ino!=before[i].st_ino ||
            pscloud_file_hash(check,hash) || strcmp(hash,hashes[i]);
        if(check>=0)close(check);
        if(bad)goto done;
    }
    if(fstat(source,&directory_after) || directory_before.st_mtim.tv_sec!=directory_after.st_mtim.tv_sec ||
       directory_before.st_mtim.tv_nsec!=directory_after.st_mtim.tv_nsec || directory_before.st_ctim.tv_sec!=directory_after.st_ctim.tv_sec ||
       directory_before.st_ctim.tv_nsec!=directory_after.st_ctim.tv_nsec)goto done;
    int archive=open(part,O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    bad=archive<0 || pscloud_file_hash(archive,s.sha256);if(archive>=0)close(archive);
    if(bad)goto done;
    int duplicate=pscloud_snapshot_exists(spool,&s);if(duplicate<0)goto done;
    if(duplicate) {pscloud_notify("Whole game unchanged - duplicate skipped");result=0;goto done;}
    s.created=(long long)time(NULL);if(s.created<0)s.created=0;
    snprintf(identity,sizeof identity,"%s.identity",name);snprintf(ready,sizeof ready,"%s.ready",name);
    int meta=openat(spool,identity,O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,0600);if(meta<0)goto done;
    out=fdopen(meta,"w");if(!out) {close(meta);goto done;}
    bad=fprintf(out,"USER_ID=%s\nTITLE=%s\nSAVE_NAME=WholeGame\nSHA256=%s\nCREATED_UNIX=%lld\n",user,title,s.sha256,s.created)<0;
    if(fflush(out) || fsync(meta))bad=1;
    if(fclose(out))bad=1;
    if(bad || renameat(spool,strrchr(part,'/')+1,spool,ready) || fsync(spool))goto done;
    part[0]=0;result=0;pscloud_notify("Whole-game backup ready: progress and profile together; originals untouched");
done:
    if(*part)unlink(part);
    if(stage>=0) {
        for(unsigned i=0;i<copied;i++)unlinkat(stage,names[i],0);
        unlinkat(stage,"manifest.txt",0);close(stage);
    }
    if(parent>=0 && *stage_name)unlinkat(parent,stage_name,AT_REMOVEDIR);
    for(unsigned i=0;i<2;i++)if(sources[i]>=0)close(sources[i]);
    if(source>=0)close(source);
    if(spool>=0)close(spool);
    if(lock>=0)close(lock);
    if(parent>=0)close(parent);
    if(result)pscloud_log("ERROR","Whole-game backup failed; originals not modified");
    return result;
}
