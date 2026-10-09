/* Generic sharing operates on copies of existing local containers. No foreign
 * metadata or keys are imported. Unknown game-level ownership bindings remain
 * possible: passing container checks is not a promise of in-game compatibility. */
#define _POSIX_C_SOURCE 200809L
#include "sharing.h"
#include "restore.h"
#include "snapshot.h"
#include "managed.h"
#include "mount.h"
#include "savemeta.h"
#include "appmeta.h"
#include "zip.h"
#include "log.h"
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <openssl/evp.h>
#ifndef PSCLOUD_HOST_TEST
#include <ps5/kernel.h>
#endif
#define SLOTS 128
#define FILES 4096
struct entry {char name[1024];const unsigned char *data;size_t size;};
struct package {unsigned count,fw;char version[32];char slots[SLOTS][64];size_t capacity[SLOTS];struct entry entries[SLOTS+1];};
static unsigned u16(const unsigned char *p) {return p[0]|((unsigned)p[1]<<8);}
static uint32_t u32(const unsigned char *p) {return u16(p)|((uint32_t)u16(p+2)<<16);}
static uint32_t crc(const unsigned char *p,size_t n) {uint32_t c=0xffffffffU;while(n--) {c^=*p++;for(unsigned i=0;i<8;i++)c=(c>>1)^(0xedb88320U&(0U-(c&1U)));}return c^0xffffffffU;}
static int digest(const unsigned char *p,size_t n,char hex[65]) {unsigned char d[32];unsigned length=0;if(!EVP_Digest(p,n,d,&length,EVP_sha256(),NULL)||length!=32)return -1;for(unsigned i=0;i<32;i++)snprintf(hex+i*2,3,"%02x",d[i]);return 0;}
static int word(const char *p,size_t max) {if(!*p||strlen(p)>max)return 0;for(;*p;p++)if(!strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-",*p))return 0;return 1;}
static int path_valid(const char *name) {
    if(!*name||strlen(name)>1000||name[0]=='/'||strchr(name,'\\'))return 0;
    char copy[1024];strcpy(copy,name);char *save=NULL;unsigned depth=0;
    for(char *p=strtok_r(copy,"/",&save);p;p=strtok_r(NULL,"/",&save)) {
        if(++depth>32||!strcmp(p,".")||!strcmp(p,"..")||!strcmp(p,"sce_sys")||!strncmp(p,".pscloud",8))return 0;
        for(const unsigned char *q=(const unsigned char *)p;*q;q++)if(*q<32||*q==127||*q==':')return 0;
    }
    return !strstr(name,"//")&&name[strlen(name)-1]!='/';
}
/* Stored ZIP only, exact local/central agreement, no aliases or special files. */
static int zip_index(const unsigned char *a,size_t size,struct entry *out,unsigned limit,unsigned *count) {
    if(size<22||size>PSCLOUD_SHARE_LIMIT)return -1;
    const unsigned char *end=a+size-22;
    if(u32(end)!=0x06054b50||u16(end+4)||u16(end+6)||u16(end+20)||u16(end+8)!=u16(end+10))return -1;
    unsigned n=u16(end+8);size_t central=u32(end+16),pos=central,local=0;
    if(!n||n>limit||central>size-22||u32(end+12)!=size-22-central)return -1;
    for(unsigned i=0;i<n;i++) {
        if(pos>size-22||size-22-pos<46||local>central||central-local<30)return -1;
        const unsigned char *c=a+pos,*l=a+local;size_t len=u16(c+28),bytes=u32(c+24);
        if(!len||len>=sizeof out[i].name||46+len>size-22-pos||30+len>central-local||bytes>central-local-30-len)return -1;
        if(u32(c)!=0x02014b50||u32(l)!=0x04034b50||u16(c+8)!=u16(l+6)||(u16(c+8)!=0&&u16(c+8)!=0x800)||u16(c+10)||u16(l+8)||u16(c+30)||u16(c+32)||u16(c+34)||u32(c+38)||u32(c+42)!=local||u16(l+26)!=len||u16(l+28)||u32(c+20)!=bytes||u32(l+18)!=bytes||u32(l+22)!=bytes||u32(c+16)!=u32(l+14)||memcmp(c+46,l+30,len))return -1;
        memcpy(out[i].name,c+46,len);out[i].name[len]=0;
        if(strlen(out[i].name)!=len||!path_valid(out[i].name))return -1;
        for(unsigned j=0;j<i;j++) {
            size_t other=strlen(out[j].name);
            if(!strcmp(out[j].name,out[i].name)||(other<len&&!strncmp(out[j].name,out[i].name,other)&&out[i].name[other]=='/')||(len<other&&!strncmp(out[j].name,out[i].name,len)&&out[j].name[len]=='/'))return -1;
        }
        out[i].data=l+30+len;out[i].size=bytes;
        if(crc(out[i].data,bytes)!=u32(c+16))return -1;
        pos+=46+len;local+=30+len+bytes;
    }
    if(pos!=size-22||local!=central)return -1;
    *count=n;return 0;
}
static int package_parse(const unsigned char *a,size_t size,const char *title,struct package *p) {
    unsigned n=0;if(zip_index(a,size,p->entries,SLOTS+1,&n)||n<2||strcmp(p->entries[0].name,"manifest.txt")||p->entries[0].size>30000)return -1;
    char text[30001],expected[30001];memcpy(text,p->entries[0].data,p->entries[0].size);text[p->entries[0].size]=0;
    if(strlen(text)!=p->entries[0].size)return -1;
    char parsed_title[10];unsigned count=0;int used=0;
    if(sscanf(text,"FORMAT=PSCLOUD_PORTABLE_GAME_V1\nTITLE=%9s\nSOURCE_FIRMWARE=%8x\nGAME_VERSION=%31s\nSLOTS=%u\n%n",parsed_title,&p->fw,p->version,&count,&used)!=4||!used||strcmp(parsed_title,title)||!count||count>SLOTS||count+1!=n||(p->fw!=0x07000000U&&p->fw!=0x11400000U))return -1;
    int length=snprintf(expected,sizeof expected,"FORMAT=PSCLOUD_PORTABLE_GAME_V1\nTITLE=%s\nSOURCE_FIRMWARE=%08x\nGAME_VERSION=%s\nSLOTS=%u\n",title,p->fw,p->version,count);
    if(length!=used||memcmp(expected,text,(size_t)used))return -1;
    const char *line=text+used;
    for(unsigned i=0;i<count;i++) {
        unsigned index=0;unsigned long long capacity=0;char hash[65],actual[65],name[80];used=0;
        if(sscanf(line,"SLOT=%u:%63[^:]:%llu:%64s\n%n",&index,p->slots[i],&capacity,hash,&used)!=4||!used||index!=i||!word(p->slots[i],63)||capacity<0x860||capacity>PSCLOUD_SHARE_LIMIT||strlen(hash)!=64)return -1;
        for(unsigned j=0;j<i;j++)if(!strcmp(p->slots[i],p->slots[j]))return -1;
        snprintf(name,sizeof name,"slot-%u.zip",i);if(strcmp(p->entries[i+1].name,name)||digest(p->entries[i+1].data,p->entries[i+1].size,actual)||strcmp(hash,actual))return -1;
        length=snprintf(expected,sizeof expected,"SLOT=%u:%s:%llu:%s\n",i,p->slots[i],capacity,hash);
        if(length!=used||memcmp(line,expected,(size_t)used))return -1;
        line+=used;p->capacity[i]=(size_t)capacity;
        struct entry *files=calloc(FILES,sizeof *files);unsigned number=0;
        int bad=!files||zip_index(p->entries[i+1].data,p->entries[i+1].size,files,FILES,&number);free(files);if(bad)return -1;
    }
    if(*line)return -1;
    p->count=count;return 0;
}
int pscloud_share_validate(const unsigned char *a,size_t size,const char *title) {
    struct package *p=calloc(1,sizeof *p);if(!p)return -1;
    int bad=!pscloud_title_valid(title)||package_parse(a,size,title,p);free(p);return bad?-1:0;
}
static int copy_fd(int input,int output) {
    if(lseek(input,0,SEEK_SET)<0)return -1;
    unsigned char buffer[65536];
    for(;;) {ssize_t n=read(input,buffer,sizeof buffer);if(n<0&&errno==EINTR)continue;if(n<0)return -1;if(!n)break;size_t at=0;
        while(at<(size_t)n) {ssize_t w=write(output,buffer+at,(size_t)n-at);if(w<0&&errno==EINTR)continue;if(w<=0)return -1;at+=(size_t)w;}}
    return fsync(output);
}
static int write_bytes(int dir,const char *name,const unsigned char *data,size_t size) {
    int fd=openat(dir,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);if(fd<0)return -1;size_t at=0;int bad=0;
    while(at<size) {ssize_t n=write(fd,data+at,size-at);if(n<0&&errno==EINTR)continue;if(n<=0) {bad=1;break;}at+=(size_t)n;}
    if(fsync(fd))bad=1;
    if(close(fd))bad=1;
    return bad?-1:0;
}
/* Only called for a mounted, disposable copied image, never a live directory. */
static int clear_payload(int dir,unsigned depth) {
    if(depth>32)return -1;
    int scan=openat(dir,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW);DIR *d=scan<0?NULL:fdopendir(scan);if(!d) {if(scan>=0)close(scan);return -1;}
    struct dirent *e;int bad=0;
    while((e=readdir(d))) {
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")||(!depth&&!strcmp(e->d_name,"sce_sys")))continue;
        struct stat st;if(fstatat(dir,e->d_name,&st,AT_SYMLINK_NOFOLLOW)) {bad=1;break;}
        if(S_ISDIR(st.st_mode)) {int child=openat(dir,e->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);bad=child<0||clear_payload(child,depth+1);if(child>=0)close(child);if(!bad)bad=unlinkat(dir,e->d_name,AT_REMOVEDIR);}
        else if(S_ISREG(st.st_mode)&&st.st_nlink==1)bad=unlinkat(dir,e->d_name,0);
        else bad=1;
        if(bad)break;
    }
    closedir(d);return bad?-1:fsync(dir);
}
static int extract_payload(int dir,const struct entry *archive) {
    struct entry *files=calloc(FILES,sizeof *files);unsigned count=0;struct stat owner;
    int bad=!files||fstat(dir,&owner)||zip_index(archive->data,archive->size,files,FILES,&count);
    if(!bad)bad=clear_payload(dir,0);
    for(unsigned i=0;!bad&&i<count;i++) {
        char name[1024];strcpy(name,files[i].name);char *last=strrchr(name,'/');int target=dup(dir);
        if(target<0) {bad=1;break;}
        if(last) {*last++=0;char *save=NULL;for(char *part=strtok_r(name,"/",&save);part;part=strtok_r(NULL,"/",&save)) {
            if(mkdirat(target,part,0700)&&errno!=EEXIST) {bad=1;break;}
            int next=openat(target,part,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);if(next<0) {bad=1;break;}
            if(fchown(next,owner.st_uid,owner.st_gid)||fsync(target))bad=1;
            close(target);target=next;if(bad)break;
        }} else last=name;
        if(!bad)bad=write_bytes(target,last,files[i].data,files[i].size);
        int fd=bad?-1:openat(target,last,O_RDONLY|O_NOFOLLOW);char hash[65],expected[65];
        if(fd<0)bad=1;
        if(fd>=0) {if(fchown(fd,owner.st_uid,owner.st_gid)||fsync(fd)||pscloud_file_hash(fd,hash)||digest(files[i].data,files[i].size,expected)||strcmp(hash,expected))bad=1;close(fd);}
        if(fsync(target))bad=1;
        close(target);
    }
    free(files);return bad?-1:fsync(dir);
}
static int order(const void *a,const void *b) {return strcmp(a,b);}
static int inventory(int source,struct package *p) {
    int scan=openat(source,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW);DIR *d=scan<0?NULL:fdopendir(scan);if(!d) {if(scan>=0)close(scan);return -1;}
    struct dirent *e;int bad=0;
    while((e=readdir(d))) {if(strncmp(e->d_name,"sdimg_",6)||!strncmp(e->d_name,"sdimg_sce_bu_",13))continue;
        if(!word(e->d_name+6,63)||p->count==SLOTS) {bad=1;break;}strcpy(p->slots[p->count++],e->d_name+6);}
    closedir(d);if(bad||!p->count)return -1;qsort(p->slots,p->count,sizeof p->slots[0],order);return 0;
}
/* A journal is durable before the first rename. Retained stage before-N.img
 * files are immutable recovery copies. Interrupted transactions block retries. */
static int commit(int parent,int source,int stage,struct package *p,struct stat *before,char baseline[][65],const char *id,const char *stage_path,const char *user,const char *title,const char *sha) {
    char journal[30000];int length=snprintf(journal,sizeof journal,"FORMAT=PSCLOUD_GENERIC_RESTORE_V1\nUSER_ID=%s\nTITLE=%s\nPACKAGE_SHA256=%s\nSTAGE=%s\nSLOTS=%u\n",user,title,sha,stage_path,p->count);
    for(unsigned i=0;i<p->count;i++) {
        char image[48],part[96];snprintf(image,sizeof image,"image-%u.img",i);snprintf(part,sizeof part,".pscloud-%s-%u.new",id,i);
        int input=openat(stage,image,O_RDONLY|O_NOFOLLOW),out=openat(source,part,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
        int bad=input<0||out<0||copy_fd(input,out);if(input>=0)close(input);
        if(out>=0) {if(fchown(out,before[i].st_uid,before[i].st_gid)||fchmod(out,before[i].st_mode&0777)||fsync(out))bad=1;close(out);}
        if(bad)return -1;
        int n=snprintf(journal+length,sizeof journal-(size_t)length,"SLOT=%u:%s:%s\n",i,p->slots[i],baseline[i]);if(n<0||(size_t)n>=sizeof journal-(size_t)length)return -1;length+=n;
    }
    if(fsync(source))return -1;
    for(unsigned i=0;i<p->count;i++) {
        char name[80],hash[65];snprintf(name,sizeof name,"sdimg_%s",p->slots[i]);int fd=openat(source,name,O_RDONLY|O_NOFOLLOW);struct stat st;
        int bad=fd<0||fstat(fd,&st)||st.st_ino!=before[i].st_ino||st.st_dev!=before[i].st_dev||pscloud_file_hash(fd,hash)||strcmp(hash,baseline[i]);if(fd>=0)close(fd);if(bad)return -1;
        snprintf(name,sizeof name,"before-%u.img",i);fd=openat(stage,name,O_RDONLY|O_NOFOLLOW);bad=fd<0||pscloud_file_hash(fd,hash)||strcmp(hash,baseline[i]);if(fd>=0)close(fd);if(bad)return -1;
    }
    if(write_bytes(parent,".restore-active",(const unsigned char *)journal,(size_t)length)||fsync(parent))return -1;
    unsigned changed=0;int failed=0;
    for(unsigned i=0;i<p->count;i++) {
        char live[80],part[96],image[48],hash[65],expected[65];snprintf(live,sizeof live,"sdimg_%s",p->slots[i]);snprintf(part,sizeof part,".pscloud-%s-%u.new",id,i);snprintf(image,sizeof image,"image-%u.img",i);
#ifdef PSCLOUD_HOST_TEST
        if(i==1&&getenv("PSCLOUD_TEST_GENERIC_COMMIT_FAIL")) {failed=1;break;}
#endif
        if(renameat(source,part,source,live)) {failed=1;break;}changed++;
        int a=openat(source,live,O_RDONLY|O_NOFOLLOW),b=openat(stage,image,O_RDONLY|O_NOFOLLOW);
        failed=fsync(source)||a<0||b<0||pscloud_file_hash(a,hash)||pscloud_file_hash(b,expected)||strcmp(hash,expected);if(a>=0)close(a);if(b>=0)close(b);if(failed)break;
    }
    if(failed)for(unsigned i=0;i<changed;i++) {
        char live[80],part[96],image[48],hash[65];snprintf(live,sizeof live,"sdimg_%s",p->slots[i]);snprintf(part,sizeof part,".pscloud-%s-%u.recover",id,i);snprintf(image,sizeof image,"before-%u.img",i);
        int input=openat(stage,image,O_RDONLY|O_NOFOLLOW),out=openat(source,part,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
        int bad=input<0||out<0||copy_fd(input,out);if(input>=0)close(input);
        if(out>=0) {if(fchown(out,before[i].st_uid,before[i].st_gid)||fchmod(out,before[i].st_mode&0777)||fsync(out))bad=1;close(out);}
        if(!bad)bad=renameat(source,part,source,live)||fsync(source);
        int verify=bad?-1:openat(source,live,O_RDONLY|O_NOFOLLOW);bad=bad||verify<0||pscloud_file_hash(verify,hash)||strcmp(hash,baseline[i]);if(verify>=0)close(verify);if(bad)return -1;
    }
    if(unlinkat(parent,".restore-active",0)||fsync(parent))return -1;
    return failed?-1:0;
}
int pscloud_share_game(const char *home,const char *root,const char *appmeta,const char *user,const char *title,int mode,const unsigned char *archive,size_t size,char published[128],char checksum[65]) {
    published[0]=0;checksum[0]=0;
    struct pscloud_snapshot identity={0};if(strlen(user)>=sizeof identity.user||strlen(title)>=sizeof identity.title)return -1;
    strcpy(identity.user,user);strcpy(identity.title,title);strcpy(identity.slot,"WholeGame");memset(identity.sha256,'0',64);
    if(!pscloud_snapshot_valid(&identity)||mode<0||mode>2)return -1;
    unsigned fw=0x11400000U;
#ifndef PSCLOUD_HOST_TEST
    fw=kernel_get_fw_version()&0xffff0000U;
#endif
    if(fw!=0x11400000U&&fw!=0x07000000U)return -1;
    struct package *p=calloc(1,sizeof *p);if(!p)return -1;
    int result=-1,parent=-1,lock=-1,source=-1,stage=-1,marker=0,attempted=0,unmounted=0;
    char path[1800],stage_path[1600]={0},mount[1800]={0},id[33],stage_name[80],version[32];
    struct pscloud_mount_state state={0};struct pscloud_save_meta first={0};
    struct stat before[SLOTS];char baseline[SLOTS][65];int original[SLOTS];for(unsigned i=0;i<SLOTS;i++)original[i]=-1;
    if(pscloud_app_version(appmeta,title,version,sizeof version)) {pscloud_log("ERROR","Sharing requires readable installed game version");goto done;}
    if(mode) {if(!archive||package_parse(archive,size,title,p)||strcmp(p->version,version)||digest(archive,size,checksum)) {pscloud_log("ERROR","Sharing package corrupt, wrong game or installed version mismatch");goto done;}}
    else {p->fw=fw;strcpy(p->version,version);}
    parent=pscloud_open_directory(root);if(parent<0)goto done;
    lock=openat(parent,".mount.lock",O_CREAT|O_RDWR|O_NOFOLLOW,0600);if(lock<0||flock(lock,LOCK_EX|LOCK_NB)||pscloud_active_marker(parent)||pscloud_no_foreign_mount())goto done;
    snprintf(path,sizeof path,"%s/%s/savedata_prospero/%s",home,user,title);source=pscloud_open_directory(path);if(source<0||(!mode&&inventory(source,p)))goto done;
    size_t total=0;
    for(unsigned i=0;i<p->count;i++) {
        char name[80];snprintf(name,sizeof name,"sdimg_%s",p->slots[i]);original[i]=openat(source,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);
        if(original[i]<0||fstat(original[i],&before[i])||!S_ISREG(before[i].st_mode)||before[i].st_nlink!=1||before[i].st_size<0x860||before[i].st_size>PSCLOUD_SHARE_LIMIT||pscloud_file_hash(original[i],baseline[i]))goto done;
        if(mode&&(size_t)before[i].st_size<p->capacity[i]) {pscloud_log("ERROR","Recipient slot %s is missing or smaller than source container",p->slots[i]);goto done;}
        if(!mode)p->capacity[i]=(size_t)before[i].st_size;
        total+=(size_t)before[i].st_size;if(total>PSCLOUD_SHARE_LIMIT)goto done;
    }
    if(pscloud_random_id(id))goto done;
    snprintf(stage_name,sizeof stage_name,"portable-stage-%s",id);if(mkdirat(parent,stage_name,0700))goto done;
    stage=openat(parent,stage_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);if(stage<0)goto done;snprintf(stage_path,sizeof stage_path,"%s/%s",root,stage_name);
    for(unsigned i=0;i<p->count;i++) {
        char image[1800],name[64];snprintf(name,sizeof name,"before-%u.img",i);
        int out=openat(stage,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);int bad=out<0||lseek(original[i],0,SEEK_SET)<0||pscloud_copy_image(original[i],out);if(out>=0)close(out);if(bad)goto done;
        snprintf(name,sizeof name,"image-%u.img",i);out=openat(stage,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);bad=out<0||lseek(original[i],0,SEEK_SET)<0||pscloud_copy_image(original[i],out);if(out>=0)close(out);if(bad)goto done;
        snprintf(image,sizeof image,"%s/%s",stage_path,name);snprintf(name,sizeof name,"mount-%u",i);if(mkdirat(stage,name,0700))goto done;snprintf(mount,sizeof mount,"%s/%s",stage_path,name);
        if(write_bytes(parent,".mount-active",(const unsigned char *)mount,strlen(mount)))goto done;
        marker=1;attempted=0;unmounted=0;if(fsync(parent)||pscloud_mount_begin(&state))goto done;attempted=1;if(pscloud_mount_copy(&state,image,mount))goto done;
        const char *payload=mount;
#ifdef PSCLOUD_HOST_TEST
        char fixture[1800];const char *fixtures=getenv("PSCLOUD_TEST_GENERIC_PAYLOADS");if(fixtures) {snprintf(fixture,sizeof fixture,"%s/%s",fixtures,p->slots[i]);payload=fixture;}
#endif
        int dir=pscloud_open_directory(payload);struct pscloud_save_meta meta;char metadata[65],after[65];
        bad=dir<0||pscloud_save_meta_read(dir,&meta,metadata)||!pscloud_save_meta_matches(&meta,title,p->slots[i]);
        if(!bad&&i)bad=meta.account_size!=first.account_size||memcmp(meta.account,first.account,meta.account_size);
        if(!bad&&!i)first=meta;
        if(!bad&&mode)bad=extract_payload(dir,&p->entries[i+1])||pscloud_save_meta_read(dir,&meta,after)||strcmp(metadata,after);
        if(!bad&&!mode) {unsigned files=0;unsigned long long bytes=0;snprintf(path,sizeof path,"%s/slot-%u.zip",stage_path,i);bad=pscloud_zip_export(dir,path,&files,&bytes);}
        if(dir>=0)close(dir);
        if(bad)goto done;
        if(pscloud_mount_end(&state,mount))goto done;
        unmounted=1;if(pscloud_mount_leave(&state))goto done;
        if(unlinkat(parent,".mount-active",0)||fsync(parent))goto done;
        marker=0;
        int copy=open(image,O_RDONLY|O_NOFOLLOW);unsigned char a[0x60],b[0x60];bad=copy<0||pread(original[i],a,sizeof a,0x800)!=(ssize_t)sizeof a||pread(copy,b,sizeof b,0x800)!=(ssize_t)sizeof b||memcmp(a,b,sizeof a);if(copy>=0)close(copy);if(bad)goto done;
    }
    for(unsigned i=0;i<p->count;i++) {
        char hash[65],name[80];struct stat current;snprintf(name,sizeof name,"sdimg_%s",p->slots[i]);
        if(fstatat(source,name,&current,AT_SYMLINK_NOFOLLOW)||current.st_ino!=before[i].st_ino||current.st_dev!=before[i].st_dev||pscloud_file_hash(original[i],hash)||strcmp(hash,baseline[i]))goto done;
    }
    if(!mode) {
        char manifest[30000];int length=snprintf(manifest,sizeof manifest,"FORMAT=PSCLOUD_PORTABLE_GAME_V1\nTITLE=%s\nSOURCE_FIRMWARE=%08x\nGAME_VERSION=%s\nSLOTS=%u\n",title,fw,version,p->count);
        char names[SLOTS+1][64];const char *ordered[SLOTS+1];strcpy(names[0],"manifest.txt");ordered[0]=names[0];
        for(unsigned i=0;i<p->count;i++) {char hash[65];snprintf(names[i+1],sizeof names[i+1],"slot-%u.zip",i);ordered[i+1]=names[i+1];int fd=openat(stage,names[i+1],O_RDONLY|O_NOFOLLOW);int bad=fd<0||pscloud_file_hash(fd,hash);if(fd>=0)close(fd);if(bad)goto done;
            int n=snprintf(manifest+length,sizeof manifest-(size_t)length,"SLOT=%u:%s:%zu:%s\n",i,p->slots[i],p->capacity[i],hash);if(n<0||(size_t)n>=sizeof manifest-(size_t)length)goto done;length+=n;}
        if(write_bytes(stage,"manifest.txt",(const unsigned char *)manifest,(size_t)length))goto done;
        if(mkdirat(parent,"share",0700)&&errno!=EEXIST)goto done;
        snprintf(published,128,"portable-%s-%s.zip",title,id);snprintf(path,sizeof path,"%s/share/%s",root,published);unsigned files=0;unsigned long long bytes=0;
        if(pscloud_zip_export_named(stage,path,ordered,p->count+1,&files,&bytes))goto done;
        int fd=open(path,O_RDONLY|O_NOFOLLOW);struct stat st;int bad=fd<0||fstat(fd,&st)||st.st_size>PSCLOUD_SHARE_LIMIT||pscloud_file_hash(fd,checksum);if(fd>=0)close(fd);if(bad) {published[0]=0;goto done;}
        unsigned char *verified=NULL;size_t verified_size=0;char directory[1400];snprintf(directory,sizeof directory,"%s/share",root);
        int share=pscloud_open_directory(directory);bad=share<0||pscloud_read_archive(share,published,&verified,&verified_size)||pscloud_share_validate(verified,verified_size,title);
        if(share>=0)close(share);free(verified);if(bad) {published[0]=0;goto done;}
    }
    if(mode==2&&commit(parent,source,stage,p,before,baseline,id,stage_path,user,title,checksum))goto done;
    pscloud_log("EVENT",mode==2?"GENERIC_RESTORE_COMMITTED=yes; rollback=%s":"LIVE_SAVES_UNCHANGED=yes; generic sharing passed; stage=%s",stage_path);result=0;
done:
    if(state.mounted) {if(pscloud_mount_end(&state,mount))result=-1;else unmounted=1;}
    if(pscloud_mount_leave(&state))result=-1;
    if(marker&&(!attempted||unmounted)&&!state.mounted&&!state.credentials_saved&&parent>=0) {if(unlinkat(parent,".mount-active",0)||fsync(parent))result=-1;}
    for(unsigned i=0;i<SLOTS;i++)if(original[i]>=0)close(original[i]);
    if(stage>=0)close(stage);
    if(source>=0)close(source);
    if(lock>=0)close(lock);
    if(parent>=0)close(parent);
    free(p);return result;
}
