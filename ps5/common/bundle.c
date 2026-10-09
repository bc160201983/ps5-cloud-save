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
#include <stdint.h>
#include <openssl/evp.h>
#include "mount.h"
#include "savemeta.h"
#include <stdlib.h>
#ifndef PSCLOUD_HOST_TEST
#include <ps5/kernel.h>
#endif

static const char *bundle_names[]={"sdimg_PlayerSaveSlot0Save","sdimg_PlayerSaveProfileSaveData","manifest.txt"};
static unsigned b16(const unsigned char *p) {return (unsigned)p[0]|((unsigned)p[1]<<8);}
static uint32_t b32(const unsigned char *p) {return b16(p)|((uint32_t)b16(p+2)<<16);}
static uint32_t bundle_crc(const unsigned char *p,size_t n) {
    uint32_t c=0xffffffffU;while(n--) {c^=*p++;for(unsigned i=0;i<8;i++)c=(c>>1)^(0xedb88320U&(0U-(c&1U)));}return c^0xffffffffU;
}
static int memory_hash(const unsigned char *p,size_t n,char hash[65]) {
    unsigned char digest[32];unsigned size=0;
    if(!EVP_Digest(p,n,digest,&size,EVP_sha256(),NULL) || size!=32)return -1;
    for(unsigned i=0;i<32;i++)snprintf(hash+2*i,3,"%02x",digest[i]);
    return 0;
}
int pscloud_bundle_parse(const unsigned char *a,size_t length,const struct pscloud_snapshot *s,const unsigned char *images[2],size_t sizes[2]) {
    if(!pscloud_snapshot_valid(s) || strcmp(s->title,"PPSA02433") || strcmp(s->slot,"WholeGame") || length<22 || length>PSCLOUD_RESTORE_MAX)return -1;
    const unsigned char *end=a+length-22;
    if(b32(end)!=0x06054b50 || b16(end+4) || b16(end+6) || b16(end+8)!=3 || b16(end+10)!=3 || b16(end+20))return -1;
    size_t central=b32(end+16),cs=b32(end+12),local=0,pos=central;
    if(central>length-22 || cs!=length-22-central)return -1;
    const unsigned char *parts[3];size_t lengths[3];
    for(unsigned i=0;i<3;i++) {
        size_t n=strlen(bundle_names[i]);
        if(pos>length-22 || length-22-pos<46+n || local>central || central-local<30+n)return -1;
        const unsigned char *c=a+pos,*l=a+local;size_t bytes=b32(l+22);
        if(b32(c)!=0x02014b50 || b32(l)!=0x04034b50 || (b16(l+6)!=0 && b16(l+6)!=0x800) || b16(c+8)!=b16(l+6) ||
           b16(c+10) || b16(l+8) || b16(c+28)!=n || b16(l+26)!=n || b16(c+30) || b16(c+32) || b16(c+34) ||
           b16(c+36) || b32(c+38) || b16(l+28) || b32(c+42)!=local || memcmp(c+46,bundle_names[i],n) ||
           memcmp(l+30,bundle_names[i],n) || b32(l+18)!=bytes || b32(c+20)!=bytes || b32(c+24)!=bytes || b32(c+16)!=b32(l+14) ||
           bytes>central-local-30-n || !bytes)return -1;
        parts[i]=l+30+n;lengths[i]=bytes;
        if(bundle_crc(parts[i],bytes)!=b32(l+14))return -1;
        local+=30+n+bytes;pos+=46+n;
    }
    if(local!=central || pos!=length-22)return -1;
    char hashes[2][65],manifest[512];
    for(unsigned i=0;i<2;i++) {
        if(lengths[i]<0x860 || parts[i][0]!=2 || memory_hash(parts[i],lengths[i],hashes[i]))return -1;
        images[i]=parts[i];sizes[i]=lengths[i];
    }
    int n=snprintf(manifest,sizeof manifest,"FORMAT=PSCLOUD_ENCRYPTED_GAME_V1\nFIRMWARE=11.40\nUSER_ID=%s\nTITLE=%s\n%s=%s\n%s=%s\n",s->user,s->title,bundle_names[0],hashes[0],bundle_names[1],hashes[1]);
    return n<0 || (size_t)n!=lengths[2] || memcmp(manifest,parts[2],lengths[2])?-1:0;
}

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
    struct pscloud_dedup_stats stats;
    int duplicate=pscloud_snapshot_exists_checked(spool,&s,&stats);
    pscloud_log("INFO","Whole-game dedup: scanned=%u invalid=%u matched=%u missing=%u hash_failed=%u different=%u phase=%u",stats.scanned,stats.invalid,stats.matched,stats.missing,stats.hash_failed,stats.different,stats.hash_phase);
    if(duplicate<0 || (!duplicate && stats.hash_failed))goto done;
    if(duplicate) {
        if(pscloud_snapshot_requeue(spool,stats.archive))goto done;
        pscloud_notify("Whole game unchanged - duplicate skipped; checking existing cloud copy");result=0;goto done;
    }
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
static int write_image_bytes(int dir,const char *name,const unsigned char *data,size_t size,const struct stat *owner) {
    int fd=openat(dir,name,O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,owner->st_mode&0777);if(fd<0)return -1;
    struct stat st;int bad=fstat(fd,&st) || !S_ISREG(st.st_mode);
    if(!bad && (fchmod(fd,owner->st_mode&0777) || fchown(fd,owner->st_uid,owner->st_gid)))bad=1;
    size_t have=0;while(!bad && have<size) {ssize_t n=write(fd,data+have,size-have);if(n<0 && errno==EINTR)continue;if(n<=0)bad=1;else have+=(size_t)n;}
    if(fsync(fd))bad=1;
    if(close(fd))bad=1;
    return bad?-1:0;
}
static int bundle_restore(const char *home,const char *root,const struct pscloud_snapshot *s,const unsigned char *archive,size_t length,int verify_only) {
    const unsigned char *images[2];size_t sizes[2];
    if(pscloud_bundle_parse(archive,length,s,images,sizes) || pscloud_verify_hash(archive,length,s->sha256) || pscloud_no_foreign_mount())return 1;
#ifndef PSCLOUD_HOST_TEST
    if((kernel_get_fw_version()&0xffff0000U)!=0x11400000U)return 1;
#endif
    int parent=-1,lock=-1,target=-1,stage=-1,original[2]={-1,-1},result=1,committed=0,journal=0,mount_marker=0,attempted=0,checked_unmount=0;
    struct stat before[2];char baseline[2][65],id[33],stage_name[64]={0},source[1400],stage_path[1600];
    int migrate=0;unsigned char *payloads[2]={NULL,NULL};size_t payload_sizes[2]={0};struct pscloud_save_meta source_meta[2];
    char replacement[2][96]={{0}},undo[2][96]={{0}},mount[1800]={0};struct pscloud_mount_state state={0};
    parent=pscloud_open_directory(root);if(parent<0)goto done;
    lock=openat(parent,".mount.lock",O_CREAT | O_RDWR | O_NOFOLLOW,0600);
    if(lock<0 || flock(lock,LOCK_EX | LOCK_NB) || pscloud_active_marker(parent)!=0)goto done;
    snprintf(source,sizeof source,"%s/%s/savedata_prospero/%s",home,s->user,s->title);target=pscloud_open_directory(source);if(target<0)goto done;
    for(unsigned i=0;i<2;i++) {
        original[i]=openat(target,bundle_names[i],O_RDONLY | O_NOFOLLOW | O_NONBLOCK);unsigned char key[0x60];
        if(original[i]<0 || fstat(original[i],&before[i]) || !S_ISREG(before[i].st_mode) || before[i].st_nlink!=1 ||
           before[i].st_size<0x860 || pread(original[i],key,sizeof key,0x800)!=(ssize_t)sizeof key)goto done;
        if(memcmp(key,images[i]+0x800,sizeof key) || before[i].st_size!=(off_t)sizes[i]) {
            migrate=1;pscloud_log("INFO","Recreated save detected: preparing decrypted-data recovery for %s",bundle_names[i]);
        }
        if(pscloud_file_hash(original[i],baseline[i]))goto done;
    }
    if(pscloud_random_id(id))goto done;
    snprintf(stage_name,sizeof stage_name,"whole-restore-%s",id);
    (void)mkdirat(parent,"rollback",0700);int rollback=openat(parent,"rollback",O_RDONLY | O_DIRECTORY | O_NOFOLLOW);if(rollback<0)goto done;
    int made=mkdirat(rollback,stage_name,0700)==0;
    stage=made?openat(rollback,stage_name,O_RDONLY | O_DIRECTORY | O_NOFOLLOW):-1;close(rollback);if(stage<0)goto done;
    snprintf(stage_path,sizeof stage_path,"%s/rollback/%s",root,stage_name);
    for(unsigned i=0;i<2;i++) {
        char name[64];snprintf(name,sizeof name,"before-%u.img",i);
        int out=openat(stage,name,O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,0600);
        int bad=out<0 || lseek(original[i],0,SEEK_SET)<0 || pscloud_copy_image(original[i],out);
        if(out>=0 && close(out))bad=1;
        if(bad)goto done;
        if(!migrate) {
            snprintf(replacement[i],sizeof replacement[i],".pscloud-new-%s-%u.part",id,i);
            if(write_image_bytes(target,replacement[i],images[i],sizes[i],&before[i]))goto done;
        }else {
            snprintf(name,sizeof name,"current-%u.img",i);
            out=openat(stage,name,O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,0600);
            bad=out<0 || lseek(original[i],0,SEEK_SET)<0 || pscloud_copy_image(original[i],out);
            if(out>=0 && close(out))bad=1;
            if(bad)goto done;
        }
        snprintf(name,sizeof name,"incoming-%u.img",i);
        if(write_image_bytes(stage,name,images[i],sizes[i],&before[i]))goto done;
    }
    if(fsync(stage) || fsync(target))goto done;
    /* Validate both incoming encrypted filesystems before either original changes. */
    for(unsigned i=0;i<2;i++) {
        char image[1800],mount_name[64];snprintf(image,sizeof image,"%s/incoming-%u.img",stage_path,i);
        snprintf(mount_name,sizeof mount_name,"mount-%u",i);if(mkdirat(stage,mount_name,0700))goto done;
        snprintf(mount,sizeof mount,"%s/%s",stage_path,mount_name);
        int marker=openat(parent,".mount-active",O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,0600);if(marker<0)goto done;
        mount_marker=1;attempted=0;checked_unmount=0;size_t n=strlen(mount);int bad=write(marker,mount,n)!=(ssize_t)n || fsync(marker);if(close(marker))bad=1;
        if(bad || fsync(parent) || pscloud_mount_begin(&state))goto done;
        attempted=1;if(pscloud_mount_copy(&state,image,mount))goto done;
        const char *payload=mount;
#ifdef PSCLOUD_HOST_TEST
        payload=getenv("PSCLOUD_TEST_PAYLOAD");
        char source_payload[1600];const char *source_root=getenv("PSCLOUD_TEST_SOURCE_ROOT");
        if(migrate && source_root) {snprintf(source_payload,sizeof source_payload,"%s/%u",source_root,i);payload=source_payload;}
#endif
        int dir=payload?pscloud_open_directory(payload):-1;int file=dir>=0?openat(dir,"ue4savegame.dpx.sav",O_RDONLY | O_NOFOLLOW | O_NONBLOCK):-1;struct stat st;
        bad=file<0 || fstat(file,&st) || !S_ISREG(st.st_mode) || st.st_size<=0 || st.st_size>PSCLOUD_RESTORE_MAX;
        if(!bad && migrate) {
            char metadata_hash[65];
            if(pscloud_save_meta_read(dir,&source_meta[i],metadata_hash) || !pscloud_save_meta_matches(&source_meta[i],s->title,bundle_names[i]+6)) {
                pscloud_log("ERROR","Source recovery metadata does not match the selected game/save identity");bad=1;result=3;
            }else {
                payload_sizes[i]=(size_t)st.st_size;payloads[i]=malloc(payload_sizes[i]);size_t have=0;
                if(!payloads[i])bad=1;
                while(!bad && have<payload_sizes[i]) {ssize_t n=read(file,payloads[i]+have,payload_sizes[i]-have);if(n<=0)bad=1;else have+=(size_t)n;}
                struct stat after;if(!bad && (fstat(file,&after) || after.st_size!=st.st_size || after.st_mtim.tv_sec!=st.st_mtim.tv_sec || after.st_mtim.tv_nsec!=st.st_mtim.tv_nsec))bad=1;
            }
        }
        if(file>=0)close(file);
        if(dir>=0)close(dir);
        if(bad || pscloud_mount_end(&state,mount))goto done;
        checked_unmount=1;if(pscloud_mount_leave(&state))goto done;
        if(unlinkat(parent,".mount-active",0) || fsync(parent))goto done;
        mount_marker=0;
    }
    if(migrate)for(unsigned i=0;i<2;i++) {
        char image[1800],mount_name[64];snprintf(image,sizeof image,"%s/current-%u.img",stage_path,i);
        snprintf(mount_name,sizeof mount_name,"current-mount-%u",i);if(mkdirat(stage,mount_name,0700))goto done;
        snprintf(mount,sizeof mount,"%s/%s",stage_path,mount_name);
        int marker=openat(parent,".mount-active",O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,0600);if(marker<0)goto done;
        mount_marker=1;attempted=0;checked_unmount=0;size_t n=strlen(mount);int bad=write(marker,mount,n)!=(ssize_t)n || fsync(marker);if(close(marker))bad=1;
        if(bad || fsync(parent) || pscloud_mount_begin(&state))goto done;
        attempted=1;if(pscloud_mount_copy(&state,image,mount))goto done;
        const char *payload=mount;
#ifdef PSCLOUD_HOST_TEST
        payload=getenv("PSCLOUD_TEST_PAYLOAD");char target_payload[1600];const char *target_root=getenv("PSCLOUD_TEST_TARGET_ROOT");
        if(target_root) {snprintf(target_payload,sizeof target_payload,"%s/%u",target_root,i);payload=target_payload;}
#endif
        int dir=payload?pscloud_open_directory(payload):-1;struct pscloud_save_meta meta;char old_meta[65],new_meta[65];
        bad=dir<0 || pscloud_save_meta_read(dir,&meta,old_meta) || !pscloud_save_meta_matches(&meta,s->title,bundle_names[i]+6) ||
            meta.account_size!=source_meta[i].account_size || memcmp(meta.account,source_meta[i].account,meta.account_size);
        if(bad) {if(dir>=0)close(dir);result=3;pscloud_log("ERROR","Recovery rejected: destination game/save/account metadata does not match backup");goto done;}
        int old=openat(dir,"ue4savegame.dpx.sav",O_RDONLY | O_NOFOLLOW | O_NONBLOCK);struct stat owner;
        bad=old<0 || fstat(old,&owner) || !S_ISREG(owner.st_mode);if(old>=0)close(old);
        if(!bad)bad=write_image_bytes(dir,".pscloud-recovery.part",payloads[i],payload_sizes[i],&owner);
        if(!bad)bad=renameat(dir,".pscloud-recovery.part",dir,"ue4savegame.dpx.sav") || fsync(dir);
        if(!bad)bad=pscloud_save_meta_read(dir,&meta,new_meta) || strcmp(old_meta,new_meta);
        close(dir);if(bad)goto done;
        if(pscloud_mount_end(&state,mount))goto done;
        checked_unmount=1;if(pscloud_mount_leave(&state))goto done;
        if(unlinkat(parent,".mount-active",0) || fsync(parent))goto done;
        mount_marker=0;
        int prepared=openat(stage,".",O_RDONLY | O_DIRECTORY | O_NOFOLLOW);char name[64];snprintf(name,sizeof name,"current-%u.img",i);
        int encrypted=prepared>=0?openat(prepared,name,O_RDONLY | O_NOFOLLOW | O_NONBLOCK):-1;if(prepared>=0)close(prepared);
        unsigned char before_key[0x60],after_key[0x60];
        bad=encrypted<0 || pread(original[i],before_key,sizeof before_key,0x800)!=(ssize_t)sizeof before_key || pread(encrypted,after_key,sizeof after_key,0x800)!=(ssize_t)sizeof after_key || memcmp(before_key,after_key,sizeof before_key);
        snprintf(replacement[i],sizeof replacement[i],".pscloud-new-%s-%u.part",id,i);
        int out=!bad?openat(target,replacement[i],O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,before[i].st_mode&0777):-1;
        if(!bad)bad=out<0 || lseek(encrypted,0,SEEK_SET)<0 || pscloud_copy_image(encrypted,out);
        if(out>=0) {if(fchmod(out,before[i].st_mode&0777) || fchown(out,before[i].st_uid,before[i].st_gid))bad=1;if(close(out))bad=1;}
        if(encrypted>=0)close(encrypted);
        if(bad)goto done;
    }
    for(unsigned i=0;i<2;i++) {
        char hash[65];struct stat now;
        int check=openat(target,bundle_names[i],O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
        int bad=check<0 || fstat(check,&now) || now.st_dev!=before[i].st_dev || now.st_ino!=before[i].st_ino || pscloud_file_hash(check,hash) || strcmp(hash,baseline[i]);
        if(check>=0)close(check);
        if(bad)goto done;
    }
    if(verify_only) {result=0;pscloud_log("INFO","Recovery staged check passed: originals untouched; recreated=%d",migrate);goto done;}
    int marker=openat(parent,".restore-active",O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,0600);if(marker<0)goto done;
    journal=1;char text[256];int n=snprintf(text,sizeof text,"USER_ID=%s\nTITLE=%s\nROLLBACK=%s\n",s->user,s->title,stage_name);
    int bad=write(marker,text,(size_t)n)!=n || fsync(marker);if(close(marker))bad=1;if(bad || fsync(parent))goto done;
    for(unsigned i=0;i<2;i++) {
#ifdef PSCLOUD_HOST_TEST
        if(i==1 && getenv("PSCLOUD_TEST_BUNDLE_COMMIT_FAIL"))goto rollback_pair;
#endif
        if(renameat(target,replacement[i],target,bundle_names[i]))goto rollback_pair;
        replacement[i][0]=0;committed++;
        if(fsync(target))goto rollback_pair;
    }
    result=0;goto finish_journal;
rollback_pair:
    /* An ordinary partial failure restores every already-replaced original.
     * An interrupted process retains a durable marker and both rollback images. */
    for(int i=0;i<committed;i++) {
        snprintf(undo[i],sizeof undo[i],".pscloud-undo-%s-%d.part",id,i);
        int out=openat(target,undo[i],O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,before[i].st_mode&0777);
        bad=out<0 || lseek(original[i],0,SEEK_SET)<0 || pscloud_copy_image(original[i],out);
        if(out>=0) {if(fchown(out,before[i].st_uid,before[i].st_gid) || fchmod(out,before[i].st_mode&0777))bad=1;if(close(out))bad=1;}
        if(bad || renameat(target,undo[i],target,bundle_names[i]) || fsync(target))goto done;
        undo[i][0]=0;
    }
finish_journal:
    if(unlinkat(parent,".restore-active",0) || fsync(parent)) {result=1;goto done;}journal=0;
done:
    if(state.mounted) {if(pscloud_mount_end(&state,mount))result=1;else checked_unmount=1;}
    if(pscloud_mount_leave(&state))result=1;
    if(mount_marker && (!attempted || checked_unmount)) {if(unlinkat(parent,".mount-active",0) || fsync(parent))result=1;}
    for(unsigned i=0;i<2;i++) {
        if(original[i]>=0)close(original[i]);
        free(payloads[i]);
        if(target>=0 && !journal) {if(*replacement[i])unlinkat(target,replacement[i],0);if(*undo[i])unlinkat(target,undo[i],0);}
    }
    if(stage>=0)close(stage);
    if(target>=0)close(target);
    if(lock>=0)close(lock);
    if(parent>=0)close(parent);
    pscloud_notify(result?"Whole-game restore failed - keep game closed; inspect rollback and activity":verify_only?"Recovery staged check passed - live saves untouched":"Whole-game restore complete - both rollback images retained");
    return result;
}
int pscloud_bundle_restore(const char *home,const char *root,const struct pscloud_snapshot *s,const unsigned char *archive,size_t length) {return bundle_restore(home,root,s,archive,length,0);}
int pscloud_bundle_restore_check(const char *home,const char *root,const struct pscloud_snapshot *s,const unsigned char *archive,size_t length) {return bundle_restore(home,root,s,archive,length,1);}
