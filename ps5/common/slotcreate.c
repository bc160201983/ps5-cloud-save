/* Creating a save slot the receiving profile does not have yet. Facts this relies on
 * (measured on 11.40 and 7.00, see ps5/SAVE_CREATION.md):
 * - libSceFsInternalForVsh creates and formats a save container; it opens like any save.
 * - sce_sys holds param.sfo and keystone. keystone and the 32-byte PARAMS value belong
 *   to the game edition and the console profile, so they come from a slot of the same
 *   game in the same profile. Per slot: SAVEDATA_DIRECTORY, texts, SAVEDATA_BLOCKS,
 *   SAVEDATA_REVISION (time UUID) and the save time at PARAMS+0x68.
 * - The save database lists every slot; blocks = size/64 KiB, size_kib = size/1 KiB. */
#ifndef __FreeBSD__
#define _POSIX_C_SOURCE 200809L
#endif
#include "slotcreate.h"
#include "managed.h"
#include "mount.h"
#include "restore.h"
#include "appmeta.h"
#include "log.h"
#include <sqlite3.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#ifndef PSCLOUD_HOST_TEST
#include <dlfcn.h>
#endif

#define SFO_MAX 65536
struct sys_files {unsigned char *sfo,*keystone;size_t sfo_size,keystone_size;struct stat root,dir,sfo_st,keystone_st;};

static int name_ok(const char *p,size_t max) {
    if(!*p||strlen(p)>max)return 0;
    for(;*p;p++)if(!strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-",*p))return 0;
    return 1;
}
static unsigned le16(const unsigned char *p) {return p[0]|((unsigned)p[1]<<8);}
static uint32_t le32(const unsigned char *p) {return le16(p)|((uint32_t)le16(p+2)<<16);}
static void put32(unsigned char *p,uint32_t v) {p[0]=(unsigned char)v;p[1]=(unsigned char)(v>>8);p[2]=(unsigned char)(v>>16);p[3]=(unsigned char)(v>>24);}
/* SFO entry by key: value pointer, index entry, length and allocation. */
static unsigned char *sfo_find(unsigned char *data,size_t size,const char *key,unsigned char **index,size_t *length,size_t *max) {
    if(size<20||le32(data)!=0x46535000)return NULL;
    size_t keys=le32(data+8),values=le32(data+12),count=le32(data+16);
    if(keys>size||values>size||count>128||20+16*count>keys)return NULL;
    for(size_t i=0;i<count;i++) {
        unsigned char *e=data+20+16*i;size_t ko=keys+le16(e),offset=values+le32(e+12);
        if(ko>=size||offset>size)return NULL;
        size_t l=le32(e+4),m=le32(e+8);if(m>size-offset||l>m)return NULL;
        if(!memchr(data+ko,0,size-ko))return NULL;
        if(!strcmp((const char *)data+ko,key)) {*index=e;*length=l;*max=m;return data+offset;}
    }
    return NULL;
}
static int sfo_set_text(unsigned char *data,size_t size,const char *key,const char *text) {
    unsigned char *index;size_t length,max,n=strlen(text)+1;unsigned char *v=sfo_find(data,size,key,&index,&length,&max);
    if(!v||le16(index+2)!=0x0204||n>max)return -1;
    memset(v,0,max);memcpy(v,text,n);put32(index+4,(uint32_t)n);return 0;
}
static void sfo_set_u64(unsigned char *data,size_t size,const char *key,unsigned long long value) {
    unsigned char *index;size_t length,max;unsigned char *v=sfo_find(data,size,key,&index,&length,&max);
    if(!v||length!=8)return;
    for(int i=0;i<8;i++)v[i]=(unsigned char)(value>>(8*i));
}
/* This slot's identity: name, subtitle, size, a new time-based revision and the save time. */
static int sfo_patch(unsigned char *data,size_t size,const char *slot,const char *subtitle,size_t capacity,time_t now) {
    if(sfo_set_text(data,size,"SAVEDATA_DIRECTORY",slot))return -1;
    (void)sfo_set_text(data,size,"SUBTITLE",subtitle);(void)sfo_set_text(data,size,"DETAIL","");
    sfo_set_u64(data,size,"SAVEDATA_BLOCKS",(unsigned long long)(capacity/65536));
    unsigned char *index;size_t length,max;
    unsigned char *revision=sfo_find(data,size,"SAVEDATA_REVISION",&index,&length,&max);
    if(revision&&length==16) { /* little-endian time fields; clock and node stay the console's */
        unsigned long long t=(unsigned long long)now*10000000ULL+0x01B21DD213814000ULL;
        put32(revision,(uint32_t)t);revision[4]=(unsigned char)(t>>32);revision[5]=(unsigned char)(t>>40);
        unsigned hi=(unsigned)((t>>48)&0x0fff)|0x1000;revision[6]=(unsigned char)hi;revision[7]=(unsigned char)(hi>>8);
    }
    unsigned char *params=sfo_find(data,size,"PARAMS",&index,&length,&max);
    if(params&&length>=0x6c)put32(params+0x68,(uint32_t)now);
    return 0;
}
static int read_file(const char *path,unsigned char **out,size_t *size,struct stat *st,size_t limit) {
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);if(fd<0)return -1;
    if(fstat(fd,st)||!S_ISREG(st->st_mode)||st->st_size<1||(size_t)st->st_size>limit) {close(fd);return -1;}
    *size=(size_t)st->st_size;*out=malloc(*size);size_t have=0;
    while(*out&&have<*size) {ssize_t n=read(fd,*out+have,*size-have);if(n<0&&errno==EINTR)continue;if(n<=0)break;have+=(size_t)n;}
    close(fd);if(!*out||have!=*size) {free(*out);*out=NULL;return -1;}
    return 0;
}
static int write_file(const char *path,const unsigned char *data,size_t size,const struct stat *owner) {
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);if(fd<0)return -1;size_t at=0;int bad=0;
    while(at<size) {ssize_t n=write(fd,data+at,size-at);if(n<0&&errno==EINTR)continue;if(n<=0) {bad=1;break;}at+=(size_t)n;}
    if(!bad&&owner&&(fchown(fd,owner->st_uid,owner->st_gid)||fchmod(fd,owner->st_mode&07777)))bad=1;
    if(fsync(fd))bad=1;
    if(close(fd))bad=1;
    if(bad)unlink(path);
    return bad?-1:0;
}
static int copy_path(const char *from,const char *to,const struct stat *owner) {
    int in=open(from,O_RDONLY|O_NOFOLLOW),out=in<0?-1:open(to,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
    int bad=in<0||out<0||pscloud_copy_image(in,out);
    if(!bad&&owner&&(fchown(out,owner->st_uid,owner->st_gid)||fchmod(out,owner->st_mode&07777)))bad=1;
    if(!bad&&fsync(out))bad=1;
    if(in>=0)close(in);
    if(out>=0)close(out);
    if(bad&&out>=0)unlink(to);
    return bad?-1:0;
}
static void free_sys(struct sys_files *s) {free(s->sfo);free(s->keystone);memset(s,0,sizeof *s);}
static int load_sys(const char *payload,struct sys_files *s) {
    char path[1200];memset(s,0,sizeof *s);
    if(lstat(payload,&s->root))return -1;
    snprintf(path,sizeof path,"%s/sce_sys",payload);if(lstat(path,&s->dir)||!S_ISDIR(s->dir.st_mode))return -1;
    snprintf(path,sizeof path,"%s/sce_sys/param.sfo",payload);if(read_file(path,&s->sfo,&s->sfo_size,&s->sfo_st,SFO_MAX))return -1;
    snprintf(path,sizeof path,"%s/sce_sys/keystone",payload);
    if(read_file(path,&s->keystone,&s->keystone_size,&s->keystone_st,4096)) {s->keystone=NULL;s->keystone_size=0;}
    return 0;
}
static int store_sys(const char *payload,const struct sys_files *s) {
    char path[1200];snprintf(path,sizeof path,"%s/sce_sys",payload);
    if(mkdir(path,0700)&&errno!=EEXIST)return -1;
    if(chown(path,s->dir.st_uid,s->dir.st_gid)||chmod(path,s->dir.st_mode&07777))return -1;
    snprintf(path,sizeof path,"%s/sce_sys/param.sfo",payload);unlink(path);if(write_file(path,s->sfo,s->sfo_size,&s->sfo_st))return -1;
    if(s->keystone) {snprintf(path,sizeof path,"%s/sce_sys/keystone",payload);unlink(path);if(write_file(path,s->keystone,s->keystone_size,&s->keystone_st))return -1;}
#ifndef PSCLOUD_HOST_TEST
    if(chown(payload,s->root.st_uid,s->root.st_gid)||chmod(payload,s->root.st_mode&07777))return -1;
#endif
    return 0;
}
static void db_path(const char *system_root,const char *user,char *out,size_t max) {snprintf(out,max,"%s/system_data/savedata_prospero/%s/db/user/savedata.db",system_root,user);}
/* A template: another slot of this game in this profile that the save database lists. */
static int pick_template(const char *folder,const char *db,const char *title,char *slot,size_t max) {
    sqlite3 *h=NULL;if(sqlite3_open_v2(db,&h,SQLITE_OPEN_READONLY,NULL)!=SQLITE_OK) {sqlite3_close(h);return -1;}
    sqlite3_busy_timeout(h,5000);sqlite3_stmt *q=NULL;int found=-1;
    if(sqlite3_prepare_v2(h,"SELECT 1 FROM savedata WHERE title_id=?1 AND dir_name=?2",-1,&q,NULL)==SQLITE_OK) {
        DIR *d=opendir(folder);struct dirent *e;
        /* Prefer a regular slot; the system-memory slot is the fallback. */
        for(int pass=0;pass<2&&found;pass++) {
            if(d)rewinddir(d);
            while(d&&(e=readdir(d))&&found) {
                if(strncmp(e->d_name,"sdimg_",6)||!strncmp(e->d_name,"sdimg_sce_bu",12)||!name_ok(e->d_name+6,63))continue;
                if(!pass&&!strcmp(e->d_name,"sdimg_sce_sdmemory"))continue;
                sqlite3_reset(q);sqlite3_bind_text(q,1,title,-1,SQLITE_STATIC);sqlite3_bind_text(q,2,e->d_name+6,-1,SQLITE_TRANSIENT);
                if(sqlite3_step(q)==SQLITE_ROW) {snprintf(slot,max,"%s",e->d_name+6);found=0;}
            }
        }
        if(d)closedir(d);
    }
    sqlite3_finalize(q);sqlite3_close(h);return found;
}
static int db_add(const char *db,const char *title,const char *template_slot,const char *slot,const char *subtitle,size_t capacity,time_t now,const char **why) {
    sqlite3 *h=NULL;if(sqlite3_open_v2(db,&h,SQLITE_OPEN_READWRITE,NULL)!=SQLITE_OK) {sqlite3_close(h);*why="The save list database could not be opened";return -1;}
    sqlite3_busy_timeout(h,5000);char when[40];struct tm tm;gmtime_r(&now,&tm);strftime(when,sizeof when,"%Y-%m-%dT%H:%M:%S.00Z",&tm);
    const char *sql="INSERT INTO savedata (title_id,dir_name,main_title,sub_title,detail,tmp_dir_name,is_broken,user_param,blocks,free_blocks,size_kib,mtime,fake_broken,account_id,user_id,faked_owner,cloud_icon_url,cloud_revision,game_title_id,system_blocks,convert_performance,account_id_in_usb_path) "
        "SELECT title_id,?3,main_title,?4,'',tmp_dir_name,0,user_param,?5,0,?6,?7,0,account_id,user_id,faked_owner,cloud_icon_url,0,game_title_id,system_blocks,convert_performance,account_id_in_usb_path "
        "FROM savedata WHERE title_id=?1 AND dir_name=?2 LIMIT 1";
    sqlite3_stmt *q=NULL;int bad=sqlite3_exec(h,"BEGIN IMMEDIATE",NULL,NULL,NULL)!=SQLITE_OK;
    if(!bad)bad=sqlite3_prepare_v2(h,sql,-1,&q,NULL)!=SQLITE_OK;
    if(!bad) {
        sqlite3_bind_text(q,1,title,-1,SQLITE_STATIC);sqlite3_bind_text(q,2,template_slot,-1,SQLITE_STATIC);sqlite3_bind_text(q,3,slot,-1,SQLITE_STATIC);
        sqlite3_bind_text(q,4,subtitle,-1,SQLITE_STATIC);sqlite3_bind_int64(q,5,(sqlite3_int64)(capacity/65536));sqlite3_bind_int64(q,6,(sqlite3_int64)(capacity/1024));sqlite3_bind_text(q,7,when,-1,SQLITE_STATIC);
        bad=sqlite3_step(q)!=SQLITE_DONE||sqlite3_changes(h)!=1;
    }
    sqlite3_finalize(q);
    if(!bad)bad=sqlite3_exec(h,"COMMIT",NULL,NULL,NULL)!=SQLITE_OK;
    if(bad) {pscloud_log("ERROR","Save list database: %s",sqlite3_errmsg(h));sqlite3_exec(h,"ROLLBACK",NULL,NULL,NULL);*why="The new slot could not be added to the save list database";}
    sqlite3_close(h);return bad?-1:0;
}
static int db_remove(const char *db,const char *title,const char *slot) {
    sqlite3 *h=NULL;if(sqlite3_open_v2(db,&h,SQLITE_OPEN_READWRITE,NULL)!=SQLITE_OK) {sqlite3_close(h);return -1;}
    sqlite3_busy_timeout(h,5000);sqlite3_stmt *q=NULL;int bad=sqlite3_prepare_v2(h,"DELETE FROM savedata WHERE title_id=?1 AND dir_name=?2",-1,&q,NULL)!=SQLITE_OK;
    if(!bad) {sqlite3_bind_text(q,1,title,-1,SQLITE_STATIC);sqlite3_bind_text(q,2,slot,-1,SQLITE_STATIC);bad=sqlite3_step(q)!=SQLITE_DONE;}
    sqlite3_finalize(q);sqlite3_close(h);return bad?-1:0;
}
static void icon_path(const char *home,const char *user,const char *title,const char *slot,char *out,size_t max) {snprintf(out,max,"%s/%s/savedata_prospero_meta/user/%s/%s_icon0.png",home,user,title,slot);}

#ifndef PSCLOUD_HOST_TEST
typedef struct {int block_size;unsigned char flags[2];} create_options;
typedef int (*init_create_fn)(create_options *);
typedef int (*create_fn)(create_options *,const char *,int,uint64_t,unsigned char *);
typedef int (*allocate_fn)(int,uint64_t,uint64_t,int);
#endif
/* New, formatted, empty container at path (capacity bytes). */
static int new_container(const char *path,size_t capacity,const char *template_image) {
#ifdef PSCLOUD_HOST_TEST
    /* Host stand-in: an image of the right size with the template's key area. */
    unsigned char key[0x60];int in=open(template_image,O_RDONLY|O_NOFOLLOW);int bad=in<0||pread(in,key,sizeof key,0x800)!=(ssize_t)sizeof key;if(in>=0)close(in);
    int fd=bad?-1:open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
    bad=fd<0||ftruncate(fd,(off_t)capacity)||pwrite(fd,key,sizeof key,0x800)!=(ssize_t)sizeof key||fsync(fd);if(fd>=0)close(fd);
    return bad?-1:0;
#else
    (void)template_image;
    static init_create_fn init_create;static create_fn create;static allocate_fn allocate;static int loaded;
    if(!loaded) {void *l=dlopen("libSceFsInternalForVsh.sprx",RTLD_LAZY);loaded=1;
        if(l) {init_create=(init_create_fn)dlsym(l,"sceFsInitCreatePfsSaveDataOpt");create=(create_fn)dlsym(l,"sceFsCreatePprPfsSaveDataImage");allocate=(allocate_fn)dlsym(l,"sceFsUfsAllocateSaveData");}}
    if(!init_create||!create) {pscloud_log("ERROR","This firmware does not offer the save-create call");return -1;}
    int fd=open(path,O_CREAT|O_EXCL|O_RDWR|O_NOFOLLOW,0600);if(fd<0)return -1;
    int r=allocate?allocate(fd,(uint64_t)capacity,0,0):-1;
    if(r<0&&ftruncate(fd,(off_t)capacity)) {close(fd);unlink(path);return -1;}
    close(fd);
    create_options options;memset(&options,0,sizeof options);init_create(&options);options.flags[1]=0x02;
    unsigned char key[32];memset(key,0,sizeof key);
    r=create(&options,path,0,(uint64_t)capacity,key);pscloud_log("INFO","Save container create result 0x%x",(unsigned)r);
    if(r<0) {unlink(path);return -1;}
    return 0;
#endif
}
/* Mounted folder for an image; on the host the test fixture folder of that slot. */
static const char *payload_of(const char *mount,const char *slot,char *buffer,size_t max) {
#ifdef PSCLOUD_HOST_TEST
    const char *fixtures=getenv("PSCLOUD_TEST_GENERIC_PAYLOADS");
    if(fixtures) {snprintf(buffer,max,"%s/%s",fixtures,slot);(void)mkdir(buffer,0700);return buffer;}
#else
    (void)slot;(void)buffer;(void)max;
#endif
    return mount;
}
/* .mount-active marks a mount in progress, like sharing does, so a crash blocks retries. */
static int marker_set(int parent,const char *mount) {
    (void)unlinkat(parent,".mount-active",0);
    int fd=openat(parent,".mount-active",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);if(fd<0)return -1;
    int bad=write(fd,mount,strlen(mount))!=(ssize_t)strlen(mount)||fsync(fd);close(fd);return bad||fsync(parent)?-1:0;
}
static int marker_clear(int parent) {return unlinkat(parent,".mount-active",0)||fsync(parent)?-1:0;}
/* Mount, run step, unmount. The step sees the mounted folder (host: the fixture folder). */
typedef int (*mounted_step)(const char *payload,void *ctx);
static int with_mount(int parent,const char *image,const char *mount,const char *slot,mounted_step step,void *ctx) {
    struct pscloud_mount_state state;memset(&state,0,sizeof state);char fixture[1200];
    if(marker_set(parent,mount))return -1;
    int bad=pscloud_mount_begin(&state)||pscloud_mount_copy(&state,image,mount);
    if(!bad)bad=step(payload_of(mount,slot,fixture,sizeof fixture),ctx);
    int stuck=state.mounted&&pscloud_mount_end(&state,mount);
    if(pscloud_mount_leave(&state))stuck=1;
    if(!stuck)marker_clear(parent);else pscloud_log("ERROR","A save container did not close; restart the console before other save operations");
    return bad||stuck?-1:0;
}
static int read_step(const char *payload,void *ctx) {return load_sys(payload,ctx);}
struct write_ctx {struct sys_files *sys;};
static int write_step(const char *payload,void *ctx) {return store_sys(payload,((struct write_ctx *)ctx)->sys);}

int pscloud_create_slot(const char *home,const char *system_root,const char *root,const char *user,const char *title,const char *slot,size_t capacity,const char *subtitle,const char **why) {
    static const char *none="";*why=none;
    if(!name_ok(user,16)||!pscloud_title_valid(title)||!name_ok(slot,31)||!strncmp(slot,"sce_",4)) {*why="Invalid profile, game or slot name";return -1;}
    capacity=(capacity+65535)/65536*65536;if(capacity<0x100000)capacity=0x100000;
    if(capacity>1024UL*1024*1024) {*why="The save slot is too large";return -1;}
    char folder[1200],live[1300],db[1200],template_slot[64],template_image[1300];
    snprintf(folder,sizeof folder,"%s/%s/savedata_prospero/%s",home,user,title);snprintf(live,sizeof live,"%s/sdimg_%s",folder,slot);db_path(system_root,user,db,sizeof db);
    struct stat st,template_st;
    if(!lstat(live,&st)) {*why="This slot already exists";return -1;}
    if(pick_template(folder,db,title,template_slot,sizeof template_slot)) {*why="This profile has no save for this game to take the game's identity from; start the game once until it saves";return -1;}
    snprintf(template_image,sizeof template_image,"%s/sdimg_%s",folder,template_slot);
    if(lstat(template_image,&template_st)||!S_ISREG(template_st.st_mode)) {*why="The other slot of this game could not be read";return -1;}
    char journal[1300];snprintf(journal,sizeof journal,"%s-journal",db);
    if(!lstat(journal,&st)) {*why="The save list database is busy; try again in a moment";return -1;}
    int parent=pscloud_open_directory(root);if(parent<0) {*why="PSCloud folder unavailable";return -1;}
    char id[33],stage[1300],image[1400],copy[1400],mount[1400];int result=-1,placed=0;struct sys_files sys;memset(&sys,0,sizeof sys);
    if(pscloud_random_id(id)) {close(parent);return -1;}
    snprintf(stage,sizeof stage,"%s/create-stage-%s",root,id);snprintf(image,sizeof image,"%s/new.img",stage);snprintf(copy,sizeof copy,"%s/template.img",stage);snprintf(mount,sizeof mount,"%s/mnt",stage);
    if(mkdir(stage,0700)||mkdir(mount,0700)) {*why="Could not prepare a temporary folder";goto done;}
    /* 1. The game's identity from the other slot, read from a copy. */
    if(copy_path(template_image,copy,NULL)||with_mount(parent,copy,mount,template_slot,read_step,&sys)) {*why="The other slot of this game could not be opened";goto done;}
    unlink(copy);
    time_t now=time(NULL);
    if(sfo_patch(sys.sfo,sys.sfo_size,slot,subtitle?subtitle:"",capacity,now)) {*why="The slot name does not fit this game's save metadata";goto done;}
    /* 2. A new container with that identity. */
    if(new_container(image,capacity,template_image)) {*why="This console refused to create a save container";goto done;}
    struct write_ctx w={&sys};
    if(with_mount(parent,image,mount,slot,write_step,&w)) {*why="The new save container could not be prepared";goto done;}
    /* 3. Into the game's folder, owned like its other slots, then listed in the database. */
    char temporary[1400];snprintf(temporary,sizeof temporary,"%s/.pscloud-new-%s",folder,slot);unlink(temporary);
    if(copy_path(image,temporary,&template_st)||rename(temporary,live)) {unlink(temporary);*why="The new slot could not be placed in the game's save folder";goto done;}
    placed=1;
    char backup[1300];snprintf(backup,sizeof backup,"%s/rollback",root);(void)mkdir(backup,0700);
    snprintf(backup,sizeof backup,"%s/rollback/savedata-%s.db.before-create",root,user);unlink(backup);
    if(copy_path(db,backup,NULL))pscloud_log("WARN","Save list database copy for safety could not be written");
    if(db_add(db,title,template_slot,slot,subtitle?subtitle:"",capacity,now,why))goto done;
    char icon_from[1300],icon_to[1300];icon_path(home,user,title,template_slot,icon_from,sizeof icon_from);icon_path(home,user,title,slot,icon_to,sizeof icon_to);
    struct stat icon;if(!lstat(icon_from,&icon)&&S_ISREG(icon.st_mode)) {unlink(icon_to);if(copy_path(icon_from,icon_to,&icon))pscloud_log("WARN","Slot icon not copied");}
    pscloud_log("EVENT","Created save slot %s for %s in profile %s (identity from slot %s, %zu bytes)",slot,title,user,template_slot,capacity);
    result=0;
done:
    if(result&&placed)unlink(live);
    free_sys(&sys);unlink(image);unlink(copy);rmdir(mount);rmdir(stage);close(parent);
    if(result)pscloud_log("ERROR","Creating save slot %s for %s failed: %s",slot,title,*why);
    return result;
}
int pscloud_remove_created_slot(const char *home,const char *system_root,const char *user,const char *title,const char *slot) {
    if(!name_ok(user,16)||!pscloud_title_valid(title)||!name_ok(slot,31))return -1;
    char path[1300],db[1200];db_path(system_root,user,db,sizeof db);
    int bad=db_remove(db,title,slot);
    snprintf(path,sizeof path,"%s/%s/savedata_prospero/%s/sdimg_%s",home,user,title,slot);if(unlink(path)&&errno!=ENOENT)bad=1;
    icon_path(home,user,title,slot,path,sizeof path);(void)unlink(path);
    pscloud_log(bad?"ERROR":"EVENT","Removed created save slot %s for %s%s",slot,title,bad?" (incomplete)":"");
    return bad?-1:0;
}
