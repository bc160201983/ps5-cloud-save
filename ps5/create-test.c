/* Test only: can PSCloud make a brand-new PS5 save container and open it again?
 * Everything happens inside /data/pscloud/create-test. No game save folder and no
 * system database is changed; the save database and one existing container header
 * are only read. Results go to /data/pscloud/create-test/result.txt.
 * The system calls are the ones other PS5 save tools use (see SAVE_CREATION.md). */
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "common/appmeta.h"
#include "common/log.h"
#include "common/mount.h"
#include "common/snapshot.h"

#define FOLDER "/data/pscloud/create-test"
#define IMAGE FOLDER "/new-save.img"
#define MOUNT FOLDER "/mnt"
#define IMAGE_SIZE (32ULL*1024*1024)

typedef struct {int block_size;uint8_t flags[2];} create_options;
typedef struct {uint8_t reserved;const char *budget;} mount_options;
typedef struct {uint8_t reserved;} unmount_options;
typedef int (*init_create_fn)(create_options *);
typedef int (*create_fn)(create_options *,const char *,int,uint64_t,uint8_t *);
typedef int (*allocate_fn)(int,uint64_t,uint64_t,int);
extern int sceFsInitMountSaveDataOpt(mount_options *options);
extern int sceFsMountSaveData(mount_options *options,const char *image,const char *mount,uint8_t *key);
extern int sceFsInitUmountSaveDataOpt(unmount_options *options);
extern int sceFsUmountSaveData(unmount_options *options,const char *mount,int handle,int flags);

static FILE *result;
static void say(const char *format,...) {
    char line[2048];va_list ap;va_start(ap,format);vsnprintf(line,sizeof line,format,ap);va_end(ap);
    printf("PSCLOUD_CREATE %s\n",line);fflush(stdout);
    if(result) {fprintf(result,"%s\n",line);fflush(result);}
}
static void hex(const char *label,const char *path,off_t offset,size_t length) {
    unsigned char bytes[0x60];char text[0x60*2+1];int fd=open(path,O_RDONLY|O_NOFOLLOW);
    if(length>sizeof bytes)length=sizeof bytes;
    ssize_t n=fd<0?-1:pread(fd,bytes,length,offset);if(fd>=0)close(fd);
    if(n<0) {say("%s %s @0x%llx unreadable (errno %d)",label,path,(unsigned long long)offset,errno);return;}
    for(ssize_t i=0;i<n;i++)snprintf(text+2*i,3,"%02x",bytes[i]);text[2*n]=0;
    say("%s %s @0x%llx: %s",label,path,(unsigned long long)offset,text);
}
static void list(const char *path) {
    DIR *d=opendir(path);if(!d) {say("list %s failed (errno %d)",path,errno);return;}
    struct dirent *e;int n=0;
    while((e=readdir(d))&&n<40) {if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;say("  entry %s",e->d_name);n++;}
    closedir(d);say("list %s: %d entries",path,n);
}
/* What the console's key service says about the new container's key area. */
static void key_check(void) {
    uint8_t *request=calloc(1,4096);if(!request)return;
    int fd=open(IMAGE,O_RDONLY|O_NOFOLLOW);ssize_t n=fd<0?-1:pread(fd,request,0x60,0x800);if(fd>=0)close(fd);
    if(n!=0x60) {say("key check: header unreadable");free(request);return;}
    int device=open("/dev/pfsmgr",O_RDWR);
    if(device<0) {say("key check: /dev/pfsmgr unavailable (errno %d)",errno);free(request);return;}
    int r=ioctl(device,0xc0845302UL,request);close(device);
    int zero=1;for(int i=0;i<32;i++)if(request[0x60+i])zero=0;
    say("key check: unseal result %d, key is %s",r,r<0?"unknown":zero?"all zero (same as used to create)":"not zero");
    memset(request,0,4096);free(request);
}
static int write_marker(void) {
    char path[256];snprintf(path,sizeof path,"%s/pscloud-test.txt",MOUNT);
    int fd=open(path,O_CREAT|O_TRUNC|O_WRONLY,0644);if(fd<0) {say("write test file failed (errno %d)",errno);return -1;}
    const char text[]="PSCloud create test\n";ssize_t n=write(fd,text,sizeof text-1);fsync(fd);close(fd);
    say("write test file: %s",n==(ssize_t)(sizeof text-1)?"ok":"short write");return n==(ssize_t)(sizeof text-1)?0:-1;
}
static int read_marker(void) {
    char path[256],text[64]={0};snprintf(path,sizeof path,"%s/pscloud-test.txt",MOUNT);
    int fd=open(path,O_RDONLY);if(fd<0) {say("read test file failed (errno %d)",errno);return -1;}
    ssize_t n=read(fd,text,sizeof text-1);close(fd);
    int ok=n>0&&!strcmp(text,"PSCloud create test\n");say("read test file: %s",ok?"ok, same text":"different text");return ok?0:-1;
}
static int unmount_now(void) {
    unmount_options u;memset(&u,0,sizeof u);sceFsInitUmountSaveDataOpt(&u);
    int r=sceFsUmountSaveData(&u,MOUNT,0,0);say("unmount result 0x%x",(unsigned)r);return r<0?-1:0;
}
/* Read-only: the save database layout and a few example rows, plus one real container header. */
static void describe_system(void) {
    DIR *d=opendir("/system_data/savedata_prospero");if(!d) {say("save database folder unreadable (errno %d)",errno);return;}
    struct dirent *e;int done=0;
    while((e=readdir(d))&&!done) {
        if(e->d_name[0]=='.')continue;
        char db[512],journal[560];snprintf(db,sizeof db,"/system_data/savedata_prospero/%s/db/user/savedata.db",e->d_name);
        struct stat st;if(stat(db,&st))continue;
        say("profile %s",e->d_name);
        if(result)pscloud_save_db_describe(db,NULL,3,result);
        pscloud_save_db_describe(db,NULL,3,stdout);
        const char *ext[]={"-journal","-wal","-shm"};
        for(int i=0;i<3;i++) {snprintf(journal,sizeof journal,"%s%s",db,ext[i]);say("  %s %s",ext[i],stat(journal,&st)?"absent":"present");}
        char home[512];snprintf(home,sizeof home,"/user/home/%s/savedata_prospero",e->d_name);
        DIR *t=opendir(home);struct dirent *g;
        while(t&&(g=readdir(t))&&!done) {
            if(g->d_name[0]=='.')continue;
            char game[800];snprintf(game,sizeof game,"%s/%.15s",home,g->d_name);DIR *s=opendir(game);struct dirent *f;
            while(s&&(f=readdir(s))) {
                if(strncmp(f->d_name,"sdimg_",6))continue;
                char image[1100];snprintf(image,sizeof image,"%s/%.200s",game,f->d_name);
                if(!stat(image,&st))say("existing container %s size %lld",image,(long long)st.st_size);
                hex("existing header",image,0,0x20);hex("existing key area",image,0x800,0x60);done=1;break;
            }
            if(s)closedir(s);
        }
        if(t)closedir(t);
        done=1;
    }
    closedir(d);
}

/* Read-only: copy two slots of one game here, open the copies, and print their
 * sce_sys/param.sfo entries so the per-slot differences are visible. */
static int copy_file(const char *from,const char *to) {
    int in=open(from,O_RDONLY|O_NOFOLLOW),out=in<0?-1:open(to,O_CREAT|O_TRUNC|O_WRONLY|O_NOFOLLOW,0600);
    char *buffer=malloc(1<<20);int bad=in<0||out<0||!buffer;
    while(!bad) {ssize_t n=read(in,buffer,1<<20);if(n<0)bad=1;if(n<=0)break;if(write(out,buffer,(size_t)n)!=n)bad=1;}
    if(!bad&&fsync(out))bad=1;
    free(buffer);if(in>=0)close(in);if(out>=0)close(out);if(bad)unlink(to);return bad?-1:0;
}
static unsigned le16(const unsigned char *p) {return p[0]|((unsigned)p[1]<<8);}
static unsigned le32(const unsigned char *p) {return le16(p)|(le16(p+2)<<16);}
static void dump_sfo(const char *label,const char *path) {
    int fd=open(path,O_RDONLY|O_NOFOLLOW);struct stat st;
    if(fd<0||fstat(fd,&st)||st.st_size<20||st.st_size>65536) {say("%s param.sfo unreadable",label);if(fd>=0)close(fd);return;}
    size_t size=(size_t)st.st_size;unsigned char *data=malloc(size);ssize_t n=data?read(fd,data,size):-1;close(fd);
    if(n!=(ssize_t)size||le32(data)!=0x46535000) {say("%s param.sfo not an SFO (size %zu)",label,size);free(data);return;}
    size_t keys=le32(data+8),values=le32(data+12),count=le32(data+16);
    say("%s param.sfo size %zu entries %zu",label,size,count);
    for(size_t i=0;i<count&&i<64&&20+16*i+16<=size;i++) {
        const unsigned char *e=data+20+16*i;size_t ko=keys+le16(e),format=le16(e+2),length=le32(e+4),max=le32(e+8),offset=values+le32(e+12);
        if(ko>=size||offset>size||length>size-offset)break;
        char key[64];snprintf(key,sizeof key,"%.63s",(const char *)data+ko);
        if(format==0x0204) {char text[1100];size_t c=length<sizeof text-1?length:sizeof text-1;memcpy(text,data+offset,c);text[c]=0;for(size_t k=0;k<c;k++)if(text[k]=='\n')text[k]='|';say("%s   %s fmt=%04zx len=%zu max=%zu text=%s",label,key,format,length,max,text);}
        else {
            say("%s   %s fmt=%04zx len=%zu max=%zu",label,key,format,length,max);
            for(size_t at=0;at<length&&at<1024;at+=64) {char line[140];size_t m=length-at<64?length-at:64;for(size_t k=0;k<m;k++)snprintf(line+2*k,3,"%02x",data[offset+at+k]);line[2*m]=0;say("%s     +%03zx %s",label,at,line);}
        }
    }
    free(data);
}
__attribute__((unused)) static void sibling_check(struct pscloud_mount_state *state) {
    DIR *users=opendir("/user/home");struct dirent *u;int done=0;
    while(users&&(u=readdir(users))&&!done) {
        if(u->d_name[0]=='.')continue;
        char base[300];snprintf(base,sizeof base,"/user/home/%.32s/savedata_prospero",u->d_name);DIR *games=opendir(base);struct dirent *g;
        while(games&&(g=readdir(games))&&!done) {
            if(!pscloud_title_valid(g->d_name))continue;
            char folder[400],slots[2][300];int found=0;snprintf(folder,sizeof folder,"%s/%s",base,g->d_name);
            DIR *f=opendir(folder);struct dirent *e;
            while(f&&(e=readdir(f))&&found<2) {
                struct stat st;char path[700];snprintf(path,sizeof path,"%s/%.200s",folder,e->d_name);
                if(strncmp(e->d_name,"sdimg_",6)||!strncmp(e->d_name,"sdimg_sce_",10)||stat(path,&st)||!S_ISREG(st.st_mode)||st.st_size>256LL*1024*1024)continue;
                snprintf(slots[found++],sizeof slots[0],"%.299s",path);
            }
            if(f)closedir(f);
            if(found<2)continue;
            done=1;say("compare two slots of %s for profile %s",g->d_name,u->d_name);
            char db[300];snprintf(db,sizeof db,"/system_data/savedata_prospero/%.32s/db/user/savedata.db",u->d_name);
            if(result)pscloud_save_db_describe(db,g->d_name,8,result);
            for(int i=0;i<2;i++) {
                char copy[200],label[16],sfo[300];snprintf(copy,sizeof copy,FOLDER "/slot-%d.img",i);snprintf(label,sizeof label,"slot%d",i);
                say("%s is %s",label,slots[i]);hex(label,slots[i],0x800,0x60);
                if(copy_file(slots[i],copy)) {say("%s copy failed (errno %d)",label,errno);continue;}
                if(pscloud_mount_copy(state,copy,MOUNT)) {say("%s copy would not open",label);unlink(copy);continue;}
                char sys[300];snprintf(sys,sizeof sys,"%s/sce_sys",MOUNT);list(sys);
                snprintf(sfo,sizeof sfo,"%s/sce_sys/param.sfo",MOUNT);dump_sfo(label,sfo);
                if(pscloud_mount_end(state,MOUNT))say("%s copy did not close; restart the console before running again",label);
                else unlink(copy);
            }
        }
        if(games)closedir(games);
    }
    if(users)closedir(users);
    if(!done)say("no game with two save slots under 256 MB found");
}

/* Read-only: for one small slot of every game (and every Ghost of Tsushima slot),
 * print the start of PARAMS and a hash of sce_sys/keystone, to see what that
 * 32-byte value depends on. Works on copies like sibling_check. */
static void params_head(const char *label,const char *sfo) {
    int fd=open(sfo,O_RDONLY|O_NOFOLLOW);struct stat st;
    if(fd<0||fstat(fd,&st)||st.st_size<20||st.st_size>65536) {say("%s param.sfo unreadable",label);if(fd>=0)close(fd);return;}
    size_t size=(size_t)st.st_size;unsigned char *data=malloc(size);ssize_t n=data?read(fd,data,size):-1;close(fd);
    int blank=1;for(ssize_t i=0;i<n;i++)if(data[i]) {blank=0;break;}
    if(n!=(ssize_t)size||blank||le32(data)!=0x46535000) {say("%s param.sfo %s (size %zu)",label,blank?"all zero":"not an SFO",size);free(data);return;}
    size_t keys=le32(data+8),values=le32(data+12),count=le32(data+16);
    for(size_t i=0;i<count&&20+16*i+16<=size;i++) {
        const unsigned char *e=data+20+16*i;size_t ko=keys+le16(e),offset=values+le32(e+12),length=le32(e+4);
        if(ko>=size||offset>size||length>size-offset||strcmp((const char *)data+ko,"PARAMS")||length<0x70)continue;
        char line[0x70*2+1];for(size_t k=0;k<0x70;k++)snprintf(line+2*k,3,"%02x",data[offset+k]);say("%s PARAMS %s",label,line);
    }
    free(data);
}
static void keystone_hash(const char *label,const char *path) {
    int fd=open(path,O_RDONLY|O_NOFOLLOW);char hash[65];
    if(fd<0) {say("%s keystone missing",label);return;}
    struct stat st;fstat(fd,&st);int bad=pscloud_file_hash(fd,hash);close(fd);say("%s keystone %lld bytes sha256 %s",label,(long long)st.st_size,bad?"?":hash);
}
static void survey(struct pscloud_mount_state *state) {
    DIR *users=opendir("/user/home");struct dirent *u;int games_done=0;
    while(users&&(u=readdir(users))) {
        if(u->d_name[0]=='.')continue;
        char base[300];snprintf(base,sizeof base,"/user/home/%.32s/savedata_prospero",u->d_name);DIR *games=opendir(base);struct dirent *g;
        while(games&&(g=readdir(games))) {
            if(!pscloud_title_valid(g->d_name))continue;
            int ghost=!strcmp(g->d_name,"PPSA02225");if(!ghost&&games_done>=14)continue;
            char folder[400];snprintf(folder,sizeof folder,"%s/%s",base,g->d_name);DIR *f=opendir(folder);struct dirent *e;int taken=0;
            while(f&&(e=readdir(f))&&(ghost||!taken)) {
                struct stat st;char path[700];snprintf(path,sizeof path,"%s/%.200s",folder,e->d_name);
                if(strncmp(e->d_name,"sdimg_",6)||!strncmp(e->d_name,"sdimg_sce_bu",12)||stat(path,&st)||!S_ISREG(st.st_mode)||st.st_size>(ghost?512LL:64LL)*1024*1024)continue;
                if(!ghost&&!strcmp(e->d_name,"sdimg_sce_sdmemory"))continue;
                char label[120];snprintf(label,sizeof label,"%.12s %s %.40s",u->d_name,g->d_name,e->d_name+6);
                if(copy_file(path,FOLDER "/survey.img")) {say("%s copy failed",label);continue;}
                if(pscloud_mount_copy(state,FOLDER "/survey.img",MOUNT)) {say("%s would not open",label);unlink(FOLDER "/survey.img");continue;}
                char sfo[300],key[300];snprintf(sfo,sizeof sfo,"%s/sce_sys/param.sfo",MOUNT);snprintf(key,sizeof key,"%s/sce_sys/keystone",MOUNT);
                params_head(label,sfo);keystone_hash(label,key);
                if(ghost)dump_sfo(label,sfo);
                if(pscloud_mount_end(state,MOUNT)) {say("%s did not close; stop",label);if(f)closedir(f);if(games)closedir(games);if(users)closedir(users);return;}
                unlink(FOLDER "/survey.img");taken=1;
            }
            if(f)closedir(f);
            if(taken&&!ghost)games_done++;
        }
        if(games)closedir(games);
    }
    if(users)closedir(users);
}

int main(void) {
    mkdir("/data/pscloud",0700);mkdir(FOLDER,0700);
    result=fopen(FOLDER "/result.txt","w");
    say("PSCloud create test. Only %s is written.",FOLDER);
    describe_system();

    void *library=dlopen("libSceFsInternalForVsh.sprx",RTLD_LAZY);
    init_create_fn init_create=library?(init_create_fn)dlsym(library,"sceFsInitCreatePfsSaveDataOpt"):NULL;
    create_fn create_ppr=library?(create_fn)dlsym(library,"sceFsCreatePprPfsSaveDataImage"):NULL;
    allocate_fn allocate=library?(allocate_fn)dlsym(library,"sceFsUfsAllocateSaveData"):NULL;
    say("library %s, init %s, create %s, allocate %s",library?"loaded":"missing",init_create?"found":"missing",create_ppr?"found":"missing",allocate?"found":"missing");
    if(!init_create||!create_ppr) {say("RESULT: this firmware does not offer the create call; stop.");goto out;}

    struct pscloud_mount_state state;memset(&state,0,sizeof state);
    if(pscloud_mount_begin(&state)) {say("RESULT: could not get save-mount rights; stop.");goto out;}
    unlink(IMAGE);mkdir(MOUNT,0700);
    survey(&state);
    if(state.mounted) {say("RESULT: a copy is still open; stop.");goto leave;}
    int fd=open(IMAGE,O_CREAT|O_TRUNC|O_RDWR,0600);
    if(fd<0) {say("RESULT: cannot create %s (errno %d)",IMAGE,errno);goto leave;}
    int r=allocate?allocate(fd,IMAGE_SIZE,0,0):-1;say("allocate result 0x%x",(unsigned)r);
    if(r<0&&ftruncate(fd,(off_t)IMAGE_SIZE)) {close(fd);say("RESULT: cannot size the image");goto leave;}
    close(fd);

    create_options options;memset(&options,0,sizeof options);init_create(&options);options.flags[1]=0x02;
    uint8_t key[32];memset(key,0,sizeof key);
    r=create_ppr(&options,IMAGE,0,IMAGE_SIZE,key);say("create result 0x%x (block size %d, flags %02x %02x)",(unsigned)r,options.block_size,options.flags[0],options.flags[1]);
    if(r<0) {say("RESULT: the console refused to create a save container.");goto leave;}
    hex("new header",IMAGE,0,0x20);hex("new key area",IMAGE,0x800,0x60);

    /* A: open it with the key it was made with, write a file. */
    mount_options m;memset(&m,0,sizeof m);sceFsInitMountSaveDataOpt(&m);m.budget="system";memset(key,0,sizeof key);
    r=sceFsMountSaveData(&m,IMAGE,MOUNT,key);say("open with creation key: 0x%x",(unsigned)r);
    int a=r>=0;
    if(a) {list(MOUNT);write_marker();if(unmount_now())a=0;}

    /* B: open it the way PSCloud opens every save (key from the container's key area). */
    key_check();
    int b=!pscloud_mount_copy(&state,IMAGE,MOUNT);say("open the PSCloud way: %s",b?"ok":"failed");
    if(b) {list(MOUNT);if(read_marker())b=0;if(pscloud_mount_end(&state,MOUNT))b=0;}

    say("RESULT: create ok; open with creation key %s; reopen the PSCloud way %s.",a?"ok":"failed",b?"ok":"failed");
    if(!state.mounted) {unlink(IMAGE);rmdir(MOUNT);say("test image removed");}
    else say("test image still open; restart the console before running again");
leave:
    if(pscloud_mount_leave(&state))say("could not drop save-mount rights; restart the console");
out:
    say("done");
    pscloud_notify("Create test finished. Send %s/result.txt",FOLDER);
    if(result)fclose(result);
    return 0;
}
