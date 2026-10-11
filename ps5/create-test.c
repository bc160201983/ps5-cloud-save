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
        if(result)pscloud_save_db_describe(db,3,result);
        pscloud_save_db_describe(db,3,stdout);
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
