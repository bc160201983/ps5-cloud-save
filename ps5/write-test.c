/* Test only: does the PS5 accept a save slot that PSCloud adds itself?
 * Run 1 adds two slots to PPSA10595 for the profile that has it:
 *   pscloudtest1: a copy of slot customizesavedata41's container (same key area),
 *   pscloudtest2: a brand-new container with customizesavedata41's files.
 * Both get their own sce_sys/param.sfo identity, the slot icon, and one row in
 * savedata.db. The database is copied to /data/pscloud/write-test first.
 * Run 2 (when /data/pscloud/write-test/created.txt exists) removes both slots and
 * their rows again. Existing slots are only read. */
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "sqlite3.h"
#include "common/log.h"
#include "common/mount.h"

#define FOLDER "/data/pscloud/write-test"
#define MOUNT FOLDER "/mnt"
#define FILES FOLDER "/files"
#define TITLE "PPSA10595"
#define SOURCE "customizesavedata41"

typedef struct {int block_size;uint8_t flags[2];} create_options;
typedef int (*init_create_fn)(create_options *);
typedef int (*create_fn)(create_options *,const char *,int,uint64_t,uint8_t *);
typedef int (*allocate_fn)(int,uint64_t,uint64_t,int);

static FILE *result;
static void say(const char *format,...) {
    char line[2048];va_list ap;va_start(ap,format);vsnprintf(line,sizeof line,format,ap);va_end(ap);
    printf("PSCLOUD_WRITE %s\n",line);fflush(stdout);
    if(result) {fprintf(result,"%s\n",line);fflush(result);}
}
static int copy_file(const char *from,const char *to,const struct stat *owner) {
    int in=open(from,O_RDONLY|O_NOFOLLOW),out=in<0?-1:open(to,O_CREAT|O_EXCL|O_WRONLY|O_NOFOLLOW,0600);
    char *buffer=malloc(1<<20);int bad=in<0||out<0||!buffer;
    while(!bad) {ssize_t n=read(in,buffer,1<<20);if(n<0)bad=1;if(n<=0)break;if(write(out,buffer,(size_t)n)!=n)bad=1;}
    if(!bad&&owner&&(fchown(out,owner->st_uid,owner->st_gid)||fchmod(out,owner->st_mode&07777)))bad=1;
    if(!bad&&fsync(out))bad=1;
    free(buffer);if(in>=0)close(in);if(out>=0)close(out);if(bad&&out>=0)unlink(to);return bad?-1:0;
}
/* Copy a folder tree, keeping each file's owner and mode. */
static int copy_tree(const char *from,const char *to,int depth) {
    if(depth>16)return -1;
    DIR *d=opendir(from);if(!d)return -1;struct dirent *e;int bad=0;
    while(!bad&&(e=readdir(d))) {
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        char a[1024],b[1024];struct stat st;snprintf(a,sizeof a,"%s/%s",from,e->d_name);snprintf(b,sizeof b,"%s/%s",to,e->d_name);
        if(lstat(a,&st)) {bad=1;break;}
        if(S_ISDIR(st.st_mode)) {
            if(mkdir(b,0700)&&errno!=EEXIST) {bad=1;break;}
            bad=copy_tree(a,b,depth+1)||chown(b,st.st_uid,st.st_gid)||chmod(b,st.st_mode&07777);
        } else if(S_ISREG(st.st_mode)) {unlink(b);bad=copy_file(a,b,&st);}
        else bad=1;
    }
    closedir(d);return bad?-1:0;
}
static void remove_tree(const char *path,int depth) {
    DIR *d=depth>16?NULL:opendir(path);struct dirent *e;
    while(d&&(e=readdir(d))) {
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        char p[1024];struct stat st;snprintf(p,sizeof p,"%s/%s",path,e->d_name);
        if(!lstat(p,&st)&&S_ISDIR(st.st_mode))remove_tree(p,depth+1);else unlink(p);
    }
    if(d)closedir(d);rmdir(path);
}
static unsigned le16(const unsigned char *p) {return p[0]|((unsigned)p[1]<<8);}
static unsigned le32(const unsigned char *p) {return le16(p)|(le16(p+2)<<16);}
static void put32(unsigned char *p,uint32_t v) {p[0]=(unsigned char)v;p[1]=(unsigned char)(v>>8);p[2]=(unsigned char)(v>>16);p[3]=(unsigned char)(v>>24);}
/* Value of SFO entry key: pointer, length and allocated size. */
static unsigned char *sfo_entry(unsigned char *data,size_t size,const char *key,size_t *length,size_t *max) {
    if(size<20||le32(data)!=0x46535000)return NULL;
    size_t keys=le32(data+8),values=le32(data+12),count=le32(data+16);
    for(size_t i=0;i<count&&20+16*i+16<=size;i++) {
        unsigned char *e=data+20+16*i;size_t ko=keys+le16(e),offset=values+le32(e+12);
        *length=le32(e+4);*max=le32(e+8);
        if(ko>=size||offset>size||*max>size-offset)return NULL;
        if(!strncmp((const char *)data+ko,key,size-ko)) {return data+offset;}
    }
    return NULL;
}
static int sfo_text(unsigned char *data,size_t size,const char *key,const char *text) {
    size_t length,max;unsigned char *v=sfo_entry(data,size,key,&length,&max);size_t n=strlen(text)+1;
    if(!v||n>max)return -1;
    memset(v,0,max);memcpy(v,text,n);
    /* entry length lives in the index; find the index entry whose value is v */
    size_t values=le32(data+12),count=le32(data+16);
    for(size_t i=0;i<count;i++) {unsigned char *e=data+20+16*i;if(data+values+le32(e+12)==v) {put32(e+4,(uint32_t)n);return 0;}}
    return -1;
}
/* New identity for the slot's sce_sys/param.sfo: directory, title, revision, save time. */
static int patch_sfo(const char *root,const char *slot,const char *title,time_t now) {
    char path[512];snprintf(path,sizeof path,"%s/sce_sys/param.sfo",root);
    int fd=open(path,O_RDWR|O_NOFOLLOW);struct stat st;if(fd<0||fstat(fd,&st)||st.st_size<20||st.st_size>65536) {if(fd>=0)close(fd);say("param.sfo missing in %s",root);return -1;}
    size_t size=(size_t)st.st_size;unsigned char *data=malloc(size);int bad=!data||pread(fd,data,size,0)!=(ssize_t)size;
    if(!bad&&(sfo_text(data,size,"SAVEDATA_DIRECTORY",slot)||sfo_text(data,size,"MAINTITLE",title))) {bad=1;say("param.sfo text fields could not be set");}
    size_t length,max;unsigned char *revision=bad?NULL:sfo_entry(data,size,"SAVEDATA_REVISION",&length,&max);
    if(!bad&&revision&&length==16) {
        /* Time-based UUID stored little-endian; keep the clock and node bytes. */
        unsigned long long t=(unsigned long long)now*10000000ULL+0x01B21DD213814000ULL;
        put32(revision,(uint32_t)t);revision[4]=(unsigned char)(t>>32);revision[5]=(unsigned char)(t>>40);
        unsigned hi=(unsigned)((t>>48)&0x0fff)|0x1000;revision[6]=(unsigned char)hi;revision[7]=(unsigned char)(hi>>8);
    } else if(!bad) {bad=1;say("SAVEDATA_REVISION not found");}
    unsigned char *params=bad?NULL:sfo_entry(data,size,"PARAMS",&length,&max);
    if(!bad&&params&&length>=0x6c)put32(params+0x68,(uint32_t)now);else if(!bad) {bad=1;say("PARAMS not found");}
    if(!bad&&(pwrite(fd,data,size,0)!=(ssize_t)size||fsync(fd)))bad=1;
    free(data);close(fd);say("param.sfo for %s: %s",slot,bad?"failed":"set");return bad?-1:0;
}
static int mount_image(struct pscloud_mount_state *state,const char *image) {mkdir(MOUNT,0700);return pscloud_mount_copy(state,image,MOUNT);}
static int unmount_image(struct pscloud_mount_state *state) {int bad=pscloud_mount_end(state,MOUNT);if(bad)say("a container did not close; restart the console");return bad;}
/* Put a staged container in the game's folder as sdimg_<slot>, like its sibling. */
static int place(const char *staged,const char *folder,const char *slot,const struct stat *sibling) {
    char temporary[600],final[600];snprintf(temporary,sizeof temporary,"%s/.pscloud-%s",folder,slot);snprintf(final,sizeof final,"%s/sdimg_%s",folder,slot);
    struct stat st;if(!lstat(final,&st)) {say("%s already exists; not replaced",final);return -1;}
    unlink(temporary);
    if(copy_file(staged,temporary,sibling)) {say("copy into %s failed (errno %d)",folder,errno);return -1;}
    if(rename(temporary,final)) {say("rename failed (errno %d)",errno);unlink(temporary);return -1;}
    say("placed %s",final);return 0;
}
static int sql(sqlite3 *db,const char *text) {
    char *error=NULL;int r=sqlite3_exec(db,text,NULL,NULL,&error);
    if(r!=SQLITE_OK)say("sql failed (%d): %s",r,error?error:"?");
    sqlite3_free(error);return r==SQLITE_OK?0:-1;
}
static int add_row(sqlite3 *db,const char *slot,const char *title,time_t now) {
    char when[40],*statement;struct tm tm;gmtime_r(&now,&tm);strftime(when,sizeof when,"%Y-%m-%dT%H:%M:%S.00Z",&tm);
    statement=sqlite3_mprintf("INSERT INTO savedata (title_id,dir_name,main_title,sub_title,detail,tmp_dir_name,is_broken,user_param,blocks,free_blocks,size_kib,mtime,fake_broken,account_id,user_id,faked_owner,cloud_icon_url,cloud_revision,game_title_id,system_blocks,convert_performance,account_id_in_usb_path) "
        "SELECT title_id,%Q,%Q,sub_title,detail,tmp_dir_name,is_broken,user_param,blocks,free_blocks,size_kib,%Q,fake_broken,account_id,user_id,faked_owner,cloud_icon_url,cloud_revision,game_title_id,system_blocks,convert_performance,account_id_in_usb_path "
        "FROM savedata WHERE title_id=%Q AND dir_name=%Q",slot,title,when,TITLE,SOURCE);
    int bad=!statement||sql(db,statement);sqlite3_free(statement);
    if(!bad&&sqlite3_changes(db)!=1) {say("expected one row for %s, got %d",slot,sqlite3_changes(db));bad=1;}
    say("database row for %s: %s",slot,bad?"failed":"added");return bad?-1:0;
}
static int open_db(const char *path,sqlite3 **db) {
    if(sqlite3_open_v2(path,db,SQLITE_OPEN_READWRITE,NULL)!=SQLITE_OK) {say("database would not open: %s",*db?sqlite3_errmsg(*db):"?");sqlite3_close(*db);*db=NULL;return -1;}
    sqlite3_busy_timeout(*db,5000);return 0;
}

static void undo(const char *user,const char *folder,const char *db_path) {
    say("Removing the test slots again.");
    sqlite3 *db=NULL;
    if(!open_db(db_path,&db)) {
        if(!sql(db,"BEGIN IMMEDIATE")&&!sql(db,"DELETE FROM savedata WHERE title_id='" TITLE "' AND dir_name IN ('pscloudtest1','pscloudtest2')")) {
            int n=sqlite3_changes(db);if(!sql(db,"COMMIT"))say("database rows removed: %d",n);
        } else sql(db,"ROLLBACK");
        sqlite3_close(db);
    }
    const char *slots[2]={"pscloudtest1","pscloudtest2"};
    for(int i=0;i<2;i++) {
        char path[700];snprintf(path,sizeof path,"%s/sdimg_%s",folder,slots[i]);say("remove %s: %s",path,unlink(path)?"not there":"done");
        snprintf(path,sizeof path,"/user/home/%s/savedata_prospero_meta/user/" TITLE "/%s_icon0.png",user,slots[i]);say("remove icon: %s",unlink(path)?"not there":"done");
    }
    unlink(FOLDER "/created.txt");
}

int main(void) {
    mkdir("/data/pscloud",0700);mkdir(FOLDER,0700);
    int undoing=!access(FOLDER "/created.txt",F_OK);
    result=fopen(undoing?FOLDER "/undo.txt":FOLDER "/result.txt","w");
    say("PSCloud write test (%s).",undoing?"remove":"add");
    /* Find the profile that has the source slot. */
    char user[40]="",folder[400]="",source[600]="";struct stat sibling;
    DIR *users=opendir("/user/home");struct dirent *u;
    while(users&&(u=readdir(users))&&!*user) {
        if(u->d_name[0]=='.')continue;
        snprintf(folder,sizeof folder,"/user/home/%.32s/savedata_prospero/" TITLE,u->d_name);snprintf(source,sizeof source,"%s/sdimg_" SOURCE,folder);
        if(!lstat(source,&sibling)&&S_ISREG(sibling.st_mode))snprintf(user,sizeof user,"%.32s",u->d_name);
    }
    if(users)closedir(users);
    if(!*user) {say("RESULT: no profile has %s slot %s; stop.",TITLE,SOURCE);goto out;}
    char db_path[300];snprintf(db_path,sizeof db_path,"/system_data/savedata_prospero/%s/db/user/savedata.db",user);
    say("profile %s, source %s (%lld bytes, owner %u:%u mode %o)",user,source,(long long)sibling.st_size,(unsigned)sibling.st_uid,(unsigned)sibling.st_gid,(unsigned)sibling.st_mode&07777);
    if(undoing) {undo(user,folder,db_path);goto out;}

    char journal[320];snprintf(journal,sizeof journal,"%s-journal",db_path);
    if(!access(journal,F_OK)) {say("RESULT: the save database is busy (journal present); stop.");goto out;}
    unlink(FOLDER "/savedata.db.before");
    if(copy_file(db_path,FOLDER "/savedata.db.before",NULL)) {say("RESULT: could not back up the save database; stop.");goto out;}
    say("save database backed up to %s/savedata.db.before",FOLDER);

    void *library=dlopen("libSceFsInternalForVsh.sprx",RTLD_LAZY);
    init_create_fn init_create=library?(init_create_fn)dlsym(library,"sceFsInitCreatePfsSaveDataOpt"):NULL;
    create_fn create_ppr=library?(create_fn)dlsym(library,"sceFsCreatePprPfsSaveDataImage"):NULL;
    allocate_fn allocate=library?(allocate_fn)dlsym(library,"sceFsUfsAllocateSaveData"):NULL;

    struct pscloud_mount_state state;memset(&state,0,sizeof state);
    if(pscloud_mount_begin(&state)) {say("RESULT: could not get save-mount rights; stop.");goto out;}
    time_t now=time(NULL);int made[2]={0,0},have_root=0;struct stat source_root;memset(&source_root,0,sizeof source_root);
    remove_tree(FILES,0);mkdir(FILES,0700);

    /* Slot 1: copy of the source container with a new identity. */
    const char *one=FOLDER "/pscloudtest1.img",*two=FOLDER "/pscloudtest2.img";unlink(one);unlink(two);
    if(copy_file(source,one,NULL)) say("slot 1: copying the source container failed");
    else if(mount_image(&state,one)) say("slot 1: the copy would not open");
    else {
        int bad=copy_tree(MOUNT,FILES,0);if(bad)say("slot 1: reading the source files failed");
        if(!lstat(MOUNT,&source_root)) {have_root=1;say("container root owner %u:%u mode %o",(unsigned)source_root.st_uid,(unsigned)source_root.st_gid,(unsigned)source_root.st_mode&07777);}
        if(!bad)bad=patch_sfo(MOUNT,"pscloudtest1","PSCloud test 1",now);
        if(unmount_image(&state))goto leave;
        if(!bad&&!place(one,folder,"pscloudtest1",&sibling))made[0]=1;
    }
    /* Slot 2: brand-new container (same size as the source) holding the source's files. */
    if(!init_create||!create_ppr) say("slot 2: create call missing");
    else {
        int fd=open(two,O_CREAT|O_TRUNC|O_RDWR,0600);int r=fd<0?-1:allocate?allocate(fd,(uint64_t)sibling.st_size,0,0):-1;
        if(fd>=0&&r<0&&ftruncate(fd,sibling.st_size))r=-2;else if(fd>=0)r=0;
        if(fd>=0)close(fd);
        create_options options;memset(&options,0,sizeof options);init_create(&options);options.flags[1]=0x02;uint8_t key[32];memset(key,0,sizeof key);
        r=r?r:create_ppr(&options,two,0,(uint64_t)sibling.st_size,key);say("slot 2: create result 0x%x",(unsigned)r);
        if(r>=0&&!mount_image(&state,two)) {
            int bad=copy_tree(FILES,MOUNT,0);if(bad)say("slot 2: writing files failed");
            if(!bad&&have_root&&(chown(MOUNT,source_root.st_uid,source_root.st_gid)||chmod(MOUNT,source_root.st_mode&07777)))say("slot 2: root owner not set (errno %d)",errno);
            if(!bad)bad=patch_sfo(MOUNT,"pscloudtest2","PSCloud test 2",now);
            if(unmount_image(&state))goto leave;
            if(!bad&&!place(two,folder,"pscloudtest2",&sibling))made[1]=1;
        } else if(r>=0) say("slot 2: the new container would not open");
    }
    unlink(one);unlink(two);remove_tree(FILES,0);

    if(made[0]||made[1]) {
        FILE *marker=fopen(FOLDER "/created.txt","w");if(marker) {fprintf(marker,"%s\n",user);fclose(marker);}
        char icon_from[600],icon_to[600];
        for(int i=0;i<2;i++)if(made[i]) {
            snprintf(icon_from,sizeof icon_from,"/user/home/%s/savedata_prospero_meta/user/" TITLE "/" SOURCE "_icon0.png",user);
            snprintf(icon_to,sizeof icon_to,"/user/home/%s/savedata_prospero_meta/user/" TITLE "/pscloudtest%d_icon0.png",user,i+1);
            struct stat icon;if(!lstat(icon_from,&icon)) {unlink(icon_to);say("icon %d: %s",i+1,copy_file(icon_from,icon_to,&icon)?"failed":"copied");}
            else say("icon %d: source has no icon file",i+1);
        }
        sqlite3 *db=NULL;
        if(!open_db(db_path,&db)) {
            int bad=sql(db,"BEGIN IMMEDIATE");
            if(!bad&&made[0])bad=add_row(db,"pscloudtest1","PSCloud test 1",now);
            if(!bad&&made[1])bad=add_row(db,"pscloudtest2","PSCloud test 2",now);
            if(!bad)bad=sql(db,"COMMIT");else sql(db,"ROLLBACK");
            sqlite3_close(db);
            say("RESULT: slot 1 %s, slot 2 %s, database %s. Open %s on the PS5 and check its save list; then run this ELF again to remove them.",made[0]?"added":"not added",made[1]?"added":"not added",bad?"NOT updated":"updated",TITLE);
        }
    } else say("RESULT: no slot was added.");
leave:
    if(pscloud_mount_leave(&state))say("could not drop save-mount rights; restart the console");
out:
    say("done");
    pscloud_notify("Write test finished. See %s",FOLDER);
    if(result)fclose(result);
    return 0;
}
