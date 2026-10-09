/* Portable Crash sharing pilot. Export/check staged copies only; never commit
 * containers to live save paths. Recipient profile metadata and keys stay local. */
#ifndef __FreeBSD__
#define _POSIX_C_SOURCE 200809L
#endif
#include "common/portable.h"
#include "common/restore.h"
#include "common/snapshot.h"
#include "common/mount.h"
#include "common/savemeta.h"
#include "common/managed.h"
#include "common/log.h"
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <openssl/evp.h>
#ifndef PSCLOUD_HOST_TEST
#include <ps5/kernel.h>
#endif
static const char *slots[]={"PlayerSaveSlot0Save","PlayerSaveProfileSaveData"};
static const char *payload_names[]={"progress.dat","profile.dat"};
struct share_config {char mode[16],user[17],package[128],closed[8];};
static int config_read(const char *path,struct share_config *s) {
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);struct stat st;
    if(fd<0)return -1;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<=0||st.st_size>1024) {close(fd);return -1;}
    FILE *f=fdopen(fd,"r");if(!f) {close(fd);return -1;}char line[256];unsigned seen=0;int bad=0;
    const char *keys[]={"MODE","USER_ID","PACKAGE","CONFIRM_GAME_CLOSED"};char *out[]={s->mode,s->user,s->package,s->closed};size_t caps[]={sizeof s->mode,sizeof s->user,sizeof s->package,sizeof s->closed};
    while(fgets(line,sizeof line,f)) {size_t n=strlen(line);if(n==sizeof line-1&&line[n-1]!='\n') {bad=1;break;}while(n&&(line[n-1]=='\n'||line[n-1]=='\r'))line[--n]=0;if(!n||line[0]=='#')continue;
        char *eq=strchr(line,'=');if(!eq) {bad=1;break;}*eq++=0;unsigned i;for(i=0;i<4;i++)if(!strcmp(line,keys[i]))break;
        if(i==4||seen&(1U<<i)||!*eq||strlen(eq)>=caps[i]) {bad=1;break;}strcpy(out[i],eq);seen|=1U<<i;
    }
    if(ferror(f))bad=1;
    fclose(f);if(bad||(seen&11U)!=11U||strcmp(s->closed,"yes")||(strcmp(s->mode,"export")&&strcmp(s->mode,"check")))return -1;
    if(!*s->user)return -1;
    for(const char *p=s->user;*p;p++)if(!strchr("0123456789abcdef",*p))return -1;
    if(!strcmp(s->mode,"check")) {size_t n=strlen(s->package);if(!(seen&4U)||n<5||strcmp(s->package+n-4,".zip"))return -1;
        for(const char *p=s->package;*p;p++)if(!strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.",*p))return -1;
        if(s->package[0]=='.')return -1;}
    return 0;
}
static int bytes_write(int dir,const char *name,const unsigned char *data,size_t size) {
    int fd=openat(dir,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);if(fd<0)return -1;size_t done=0;
    while(done<size) {ssize_t n=write(fd,data+done,size-done);if(n<=0) {close(fd);return -1;}done+=(size_t)n;}
    int bad=fsync(fd);if(close(fd))bad=1;return bad?-1:0;
}
static int payload_copy(int source,int destination) {
    struct stat before,after;if(fstat(source,&before)||before.st_size<=0||before.st_size>32*1024*1024||lseek(source,0,SEEK_SET)<0)return -1;
    unsigned char data[65536];off_t copied=0;
    while(copied<before.st_size) {ssize_t n=read(source,data,sizeof data);if(n<0&&errno==EINTR)continue;if(n<=0)return -1;
        size_t done=0;while(done<(size_t)n) {ssize_t w=write(destination,data+done,(size_t)n-done);if(w<0&&errno==EINTR)continue;if(w<=0)return -1;done+=(size_t)w;}copied+=n;}
    if(fstat(source,&after)||after.st_size!=before.st_size||after.st_mtim.tv_sec!=before.st_mtim.tv_sec||after.st_mtim.tv_nsec!=before.st_mtim.tv_nsec||
        after.st_ctim.tv_sec!=before.st_ctim.tv_sec||after.st_ctim.tv_nsec!=before.st_ctim.tv_nsec)return -1;
    return fsync(destination);
}
static int payload_verify(int dir,const unsigned char *data,size_t size) {
    unsigned char digest[32];unsigned length=0;char expected[65],actual[65];
    if(!EVP_Digest(data,size,digest,&length,EVP_sha256(),NULL)||length!=32)return -1;
    for(unsigned i=0;i<32;i++)snprintf(expected+2*i,3,"%02x",digest[i]);
    int fd=openat(dir,"ue4savegame.dpx.sav",O_RDONLY|O_NOFOLLOW|O_NONBLOCK);struct stat st;
    int bad=fd<0||fstat(fd,&st)||st.st_size!=(off_t)size||pscloud_file_hash(fd,actual)||strcmp(actual,expected);if(fd>=0)close(fd);return bad?-1:0;
}
int main(int argc,char **argv) {
#ifdef PSCLOUD_HOST_TEST
    if(argc!=4)return 2;
    const char *config=argv[1],*home=argv[2],*root=argv[3];unsigned fw=0x11400000U;
#else
    (void)argc;(void)argv;const char *config="/data/pscloud-share.conf",*home="/user/home",*root="/data/pscloud";unsigned fw=kernel_get_fw_version()&0xffff0000U;
#endif
    struct share_config s={0};if(config_read(config,&s))return 2;
    int check=!strcmp(s.mode,"check");if((!check&&fw!=0x11400000U)||(check&&fw!=0x11400000U&&fw!=0x07000000U))return 2;
    (void)mkdir(root,0700);char logpath[1400];snprintf(logpath,sizeof logpath,"%s/share.log",root);pscloud_log_open(logpath);
    pscloud_notify("Crash portable sharing %s - staged copies only; live saves will not be replaced",s.mode);
    int result=1,parent=-1,lock=-1,source=-1,stage=-1,shared=-1,original[2]={-1,-1},marker=0;
    struct pscloud_mount_state state={0};struct stat before[2];char baseline[2][65],id[33],stage_name[64],stage_path[1600],mount[1800]={0},source_path[1400],package_path[1600];
    unsigned char *package=NULL;size_t package_size=0;struct pscloud_portable portable={0};struct pscloud_save_meta first_meta={0};
    parent=pscloud_open_directory(root);if(parent<0)goto done;
    lock=openat(parent,".mount.lock",O_CREAT|O_RDWR|O_NOFOLLOW,0600);if(lock<0||flock(lock,LOCK_EX|LOCK_NB)||pscloud_active_marker(parent)||pscloud_no_foreign_mount())goto done;
    (void)mkdirat(parent,"share",0700);shared=openat(parent,"share",O_RDONLY|O_DIRECTORY|O_NOFOLLOW);if(shared<0)goto done;
    if(check&&(pscloud_read_archive(shared,s.package,&package,&package_size)||pscloud_portable_parse(package,package_size,&portable))) {pscloud_log("ERROR","Portable package rejected before mounting");goto done;}
    snprintf(source_path,sizeof source_path,"%s/%s/savedata_prospero/PPSA02433",home,s.user);source=pscloud_open_directory(source_path);if(source<0)goto done;
    for(unsigned i=0;i<2;i++) {char name[96];snprintf(name,sizeof name,"sdimg_%s",slots[i]);original[i]=openat(source,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);
        if(original[i]<0||fstat(original[i],&before[i])||!S_ISREG(before[i].st_mode)||before[i].st_nlink!=1||before[i].st_size<0x860||before[i].st_size>128*1024*1024||pscloud_file_hash(original[i],baseline[i]))goto done;}
    if(pscloud_random_id(id))goto done;
    snprintf(stage_name,sizeof stage_name,"share-stage-%s",id);
    if(mkdirat(parent,stage_name,0700))goto done;
    stage=openat(parent,stage_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);if(stage<0)goto done;
    snprintf(stage_path,sizeof stage_path,"%s/%s",root,stage_name);
    for(unsigned i=0;i<2;i++) {
        char name[64],image[1800];snprintf(name,sizeof name,"image-%u.img",i);int out=openat(stage,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
        int bad=out<0||lseek(original[i],0,SEEK_SET)<0||pscloud_copy_image(original[i],out);if(out>=0&&close(out))bad=1;if(bad)goto done;
        snprintf(image,sizeof image,"%s/%s",stage_path,name);snprintf(name,sizeof name,"mount-%u",i);if(mkdirat(stage,name,0700))goto done;snprintf(mount,sizeof mount,"%s/%s",stage_path,name);
        int journal=openat(parent,".mount-active",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);if(journal<0)goto done;
        marker=1;size_t length=strlen(mount);bad=write(journal,mount,length)!=(ssize_t)length||fsync(journal);if(close(journal))bad=1;if(bad||fsync(parent)||pscloud_mount_begin(&state))goto done;
        if(pscloud_mount_copy(&state,image,mount))goto done;
        const char *payload=mount;
#ifdef PSCLOUD_HOST_TEST
        char fixture[1600];const char *fixtures=getenv("PSCLOUD_TEST_SHARE_PAYLOADS");if(fixtures) {snprintf(fixture,sizeof fixture,"%s/%u",fixtures,i);payload=fixture;}
#endif
        int dir=pscloud_open_directory(payload);struct pscloud_save_meta meta;char metadata_hash[65],after_hash[65];
        bad=dir<0||pscloud_save_meta_read(dir,&meta,metadata_hash)||!pscloud_save_meta_matches(&meta,"PPSA02433",slots[i]);
        if(!bad&&i&&!check)bad=meta.account_size!=first_meta.account_size||memcmp(meta.account,first_meta.account,meta.account_size);
        if(!bad&&!i)first_meta=meta;
        int file=bad?-1:openat(dir,"ue4savegame.dpx.sav",O_RDONLY|O_NOFOLLOW|O_NONBLOCK);struct stat owner;
        if(file<0||fstat(file,&owner)||!S_ISREG(owner.st_mode)||owner.st_nlink!=1||owner.st_size<=0||owner.st_size>32*1024*1024)bad=1;
        if(!bad&&!check) {out=openat(stage,payload_names[i],O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);bad=out<0||payload_copy(file,out);if(out>=0&&close(out))bad=1;}
        if(file>=0)close(file);
        if(!bad&&check) {bad=bytes_write(dir,".pscloud-share.part",portable.payload[i],portable.size[i]);
            int part=bad?-1:openat(dir,".pscloud-share.part",O_RDWR|O_NOFOLLOW);if(part<0)bad=1;
            if(part>=0) {if(fchown(part,owner.st_uid,owner.st_gid)||fchmod(part,owner.st_mode&0777)||fsync(part))bad=1;close(part);}
            if(!bad)bad=renameat(dir,".pscloud-share.part",dir,"ue4savegame.dpx.sav")||fsync(dir);
            if(!bad)bad=payload_verify(dir,portable.payload[i],portable.size[i])||pscloud_save_meta_read(dir,&meta,after_hash)||strcmp(metadata_hash,after_hash);}
        pscloud_log("INFO","Portable stage %u: closing payload directory before unmount",i);if(dir>=0)close(dir);if(bad)goto done;
        if(pscloud_mount_end(&state,mount)||pscloud_mount_leave(&state))goto done;
        if(unlinkat(parent,".mount-active",0)||fsync(parent))goto done;
        marker=0;
        int copy=open(image,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);unsigned char a[0x60],b[0x60];
        bad=copy<0||pread(original[i],a,sizeof a,0x800)!=(ssize_t)sizeof a||pread(copy,b,sizeof b,0x800)!=(ssize_t)sizeof b||memcmp(a,b,sizeof a);if(copy>=0)close(copy);if(bad)goto done;
    }
    for(unsigned i=0;i<2;i++) {char hash[65];if(pscloud_file_hash(original[i],hash)||strcmp(hash,baseline[i]))goto done;}
    if(!check) {snprintf(package_path,sizeof package_path,"%s/share/crash-portable-%s.zip",root,id);
        if(pscloud_portable_export(stage,package_path,fw,(long long)time(NULL)))goto done;
        pscloud_log("EVENT","PORTABLE_PACKAGE=%s",package_path);}
    pscloud_log("EVENT","LIVE_SAVES_UNCHANGED=yes; staged sharing %s passed; stage=%s",s.mode,stage_path);result=0;
done:
    if(state.mounted) {if(pscloud_mount_end(&state,mount))result=1;}
    if(pscloud_mount_leave(&state))result=1;
    if(marker&&!state.mounted&&!state.credentials_saved&&parent>=0) {if(unlinkat(parent,".mount-active",0)||fsync(parent))result=1;}
    for(unsigned i=0;i<2;i++)if(original[i]>=0)close(original[i]);
    free(package);if(shared>=0)close(shared);if(stage>=0)close(stage);if(source>=0)close(source);if(lock>=0)close(lock);if(parent>=0)close(parent);
    pscloud_notify(result?"Portable sharing pilot failed - originals not committed; inspect share.log and staging marker":"Portable sharing pilot passed - live saves unchanged; recipient restore is not enabled yet");pscloud_log_close();return result;
}
