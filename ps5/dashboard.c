#ifndef __FreeBSD__
#define _POSIX_C_SOURCE 200809L
#endif
#include "common/cloud.h"
#include "common/snapshot.h"
#include "common/restore.h"
#include "common/managed.h"
#include "common/log.h"
#include "common/appmeta.h"
#include "common/bundle.h"
#include "common/google.h"
#include "common/transfer.h"
#include "common/sharing.h"
#include <pthread.h>
#include <stdatomic.h>
#include "ui.h"
#include <curl/curl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <openssl/evp.h>
#include <sys/statvfs.h>
#ifndef PSCLOUD_HOST_TEST
#include <ps5/kernel.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>
/* Measured on 11.40: the title ID is at byte 0x10 of the app info (e.g. eboot.bin ->
 * "PPSA03671"), not where the SDK ps sample's struct places it. Use a roomy raw buffer. */
int sceKernelGetAppInfo(pid_t pid,void *info);
/* The foreground game, as ProsperoMgr reads it. */
int sceSystemServiceGetAppIdOfRunningBigApp(void);
int sceSystemServiceGetAppTitleId(int app_id,char *title_id);
#endif
/* Whether a process of this game is running: 1 yes, 0 no, -1 process list unavailable.
 * Games may mount saves only while writing, so an unmounted save does not mean closed. */
static int title_running(const char *title) {
#ifdef PSCLOUD_HOST_TEST
    /* Tests name a file listing running titles, or containing "unavailable". */
    const char *path=getenv("PSCLOUD_TEST_RUNNING_TITLES");char list[256]={0};
    FILE *f=path?fopen(path,"r"):NULL;if(!f)return 0;
    size_t n=fread(list,1,sizeof list-1,f);fclose(f);list[n]=0;
    if(strstr(list,"unavailable"))return -1;
    return strstr(list,title)!=NULL;
#else
    int app=sceSystemServiceGetAppIdOfRunningBigApp();
    if(app>0) {char running[16]={0};if(!sceSystemServiceGetAppTitleId(app,running)&&!strncmp(running,title,9))return 1;}
    /* Also scan processes: covers a game that is open but not the foreground app. */
    int mib[4]={CTL_KERN,KERN_PROC,KERN_PROC_PROC,0};size_t size=0;
    if(sysctl(mib,4,NULL,&size,NULL,0)||!size)return -1;
    size+=size/4;char *buffer=malloc(size);if(!buffer)return -1;
    if(sysctl(mib,4,buffer,&size,NULL,0)) {free(buffer);return -1;}
    int found=0;unsigned seen=0;
    for(size_t offset=0;offset+sizeof(int)<=size&&!found;) {
        struct kinfo_proc *ki=(struct kinfo_proc *)(buffer+offset);
        if(ki->ki_structsize<=0||offset+(size_t)ki->ki_structsize>size)break;
        offset+=(size_t)ki->ki_structsize;seen++;
        char info[256];memset(info,0,sizeof info);
        if(sceKernelGetAppInfo(ki->ki_pid,info))continue;
        if(!memcmp(info+0x10,title,9)&&!info[0x19])found=1;
    }
    free(buffer);return found?1:seen?0:-1;
#endif
}
/* The game being played, for the home page: 1 found, 0 none, -1 unknown. */
static int running_game(char title[16]) {
    title[0]=0;
#ifdef PSCLOUD_HOST_TEST
    const char *path=getenv("PSCLOUD_TEST_RUNNING_TITLES");char list[256]={0};
    FILE *f=path?fopen(path,"r"):NULL;if(!f)return 0;
    size_t n=fread(list,1,sizeof list-1,f);fclose(f);list[n]=0;
    if(strstr(list,"unavailable"))return -1;
    char *game=strstr(list,"PPSA");if(!game)return 0;
    memcpy(title,game,9);title[9]=0;return pscloud_title_valid(title)?1:0;
#else
    int app=sceSystemServiceGetAppIdOfRunningBigApp();
    if(app>0&&!sceSystemServiceGetAppTitleId(app,title)&&pscloud_title_valid(title))return 1;
    title[0]=0;
    int mib[4]={CTL_KERN,KERN_PROC,KERN_PROC_PROC,0};size_t size=0;
    if(sysctl(mib,4,NULL,&size,NULL,0)||!size)return -1;
    size+=size/4;char *buffer=malloc(size);if(!buffer)return -1;
    if(sysctl(mib,4,buffer,&size,NULL,0)) {free(buffer);return -1;}
    int found=0;
    for(size_t offset=0;offset+sizeof(int)<=size&&!found;) {
        struct kinfo_proc *ki=(struct kinfo_proc *)(buffer+offset);
        if(ki->ki_structsize<=0||offset+(size_t)ki->ki_structsize>size)break;
        offset+=(size_t)ki->ki_structsize;
        if(strcmp(ki->ki_comm,"eboot.bin"))continue;
        char info[256];memset(info,0,sizeof info);
        if(sceKernelGetAppInfo(ki->ki_pid,info))continue;
        memcpy(title,info+0x10,9);title[9]=0;found=pscloud_title_valid(title);if(!found)title[0]=0;
    }
    free(buffer);return found;
#endif
}
extern int pscloud_backup_main(int,char **);
extern const char *pscloud_backup_archive(void);
extern int pscloud_worker_main(int,char **);
extern int pscloud_download_main(int,char **);
static char root[1024],home[1024],cloudpath[1200],logpath[1200],token[33],appmeta[1200];
static _Atomic int cloud_status;
static _Atomic int auto_upload=1,activity_refresh=1;
static _Atomic int local_keep_latest;
static _Atomic int compact_backups;
static _Atomic int game_close_backup;
static _Atomic int log_waiters;
/* Firmware is read once at start. Every firmware the SDK runs on is accepted; encrypted
 * (legacy) backup/restore stays limited to 11.40, so other firmware always uses compact. */
static unsigned firmware_id=0x11400000U;
static int legacy_firmware(void) {return firmware_id==0x11400000U;}
static int firmware_tested(void) {return firmware_id==0x11400000U||firmware_id==0x07000000U;}
static int compact_effective(void) {return compact_backups||!legacy_firmware();}
static void prune_local_history(void);
/* Per-game choices, in <root>/games.conf as "PPSA03671 CLOSE=1 ALL=1". Missing = on. */
struct game_setting {char title[10];int close_backup,backup_all,auto_sync,auto_apply;};
static struct game_setting game_settings[512];static unsigned game_settings_count;
static pthread_mutex_t settings_mutex=PTHREAD_MUTEX_INITIALIZER;
static void game_settings_load(void);
static int game_setting(const char *title,int which);
static void batch_json(int sock);
static time_t newest_container(int titledir);
static void sync_tick(void);
static void registry_publish(void);
static void console_id_load(void);
static void sync_after_restore(const char *user,const char *title,const char *file);
static void sync_slots_json(int sock,const char *query);
static long long sync_base(const char *user,const char *title);
static void sync_received(const char *user,const char *title,char sha[65]);
static int backup_one(const char *user,const char *title,char published[144],int *unchanged);
static int batch_start(const char *user);
static int schedule_save(void);
static void schedule_load(void);
static int schedule_hour;
static struct pscloud_google google,google_pending;
static char googlepath[1200];
static pthread_mutex_t operation_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t clients_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t clients_done=PTHREAD_COND_INITIALIZER;
static unsigned clients;
static _Atomic int background_active,background_result,background_cancelled;
static pthread_mutex_t cache_mutex=PTHREAD_MUTEX_INITIALIZER;
static char games_cache[262144],queue_cache[65536];
static char background_name[256];
static void cache_store(char *cache,size_t cap,const char *json) {
    pthread_mutex_lock(&cache_mutex);snprintf(cache,cap,"%s",json);pthread_mutex_unlock(&cache_mutex);
}
static volatile sig_atomic_t stopped;
static void stop_server(int sig) {(void)sig;stopped=1;}
struct response {char *data;size_t size,limit;};
static size_t collect(char *p,size_t a,size_t b,void *ctx) {
    if(!ctx) {if(b&&a>(size_t)-1/b)return 0;return a*b;}
    struct response *r=ctx;if(b && a>r->limit/b)return 0;size_t n=a*b;
    if(n>r->limit-r->size)return 0;
    memcpy(r->data+r->size,p,n);r->size+=n;r->data[r->size]=0;return n;
}
static _Thread_local int webdav_quiet; /* background sync checks do not fill Activity */
static long webdav(const struct settings *s,const char *url,const char *method,struct response *r) {
    CURL *c=curl_easy_init();if(!c)return 0;
    curl_easy_setopt(c,CURLOPT_URL,url);curl_easy_setopt(c,CURLOPT_PROTOCOLS_STR,"https");
    curl_easy_setopt(c,CURLOPT_USERAGENT,"PSCloud/" PSCLOUD_VERSION);
    curl_easy_setopt(c,CURLOPT_USERNAME,s->user);curl_easy_setopt(c,CURLOPT_PASSWORD,s->password);
    curl_easy_setopt(c,CURLOPT_CAINFO,s->ca);curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,10L);
    /* Metadata must not inherit the five-minute archive transfer timeout. */
    curl_easy_setopt(c,CURLOPT_TIMEOUT,!strcmp(method,"GET") && strstr(url,".zip") && !strstr(url,".identity")?300L:25L);curl_easy_setopt(c,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(c,CURLOPT_CUSTOMREQUEST,method);curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,collect);
    curl_easy_setopt(c,CURLOPT_WRITEDATA,r);struct curl_slist *headers=NULL;
    if(!strcmp(method,"PROPFIND")) {headers=curl_slist_append(headers,"Depth: 1");curl_easy_setopt(c,CURLOPT_HTTPHEADER,headers);}
    CURLcode result=curl_easy_perform(c);long status=0;curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);
    if(!webdav_quiet)pscloud_log("INFO","WebDAV %s: transport=%d HTTP=%ld",method,(int)result,status);
    curl_slist_free_all(headers);curl_easy_cleanup(c);return result==CURLE_OK?status:0;
}
static long webdav_put(const struct settings *s,const char *url,const char *data,size_t size) {
    CURL *c=curl_easy_init();if(!c)return 0;char sink[256];struct response r={sink,0,sizeof sink-1};
    curl_easy_setopt(c,CURLOPT_URL,url);curl_easy_setopt(c,CURLOPT_PROTOCOLS_STR,"https");
    curl_easy_setopt(c,CURLOPT_USERAGENT,"PSCloud/" PSCLOUD_VERSION);
    curl_easy_setopt(c,CURLOPT_USERNAME,s->user);curl_easy_setopt(c,CURLOPT_PASSWORD,s->password);
    curl_easy_setopt(c,CURLOPT_CAINFO,s->ca);curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,10L);curl_easy_setopt(c,CURLOPT_TIMEOUT,25L);curl_easy_setopt(c,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(c,CURLOPT_CUSTOMREQUEST,"PUT");curl_easy_setopt(c,CURLOPT_POSTFIELDS,data);curl_easy_setopt(c,CURLOPT_POSTFIELDSIZE,(long)size);
    curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,collect);curl_easy_setopt(c,CURLOPT_WRITEDATA,&r);
    CURLcode result=curl_easy_perform(c);long status=0;curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);curl_easy_cleanup(c);
    return result==CURLE_OK?status:0;
}
static int safe_word(const char *s,size_t max) {
    if(!*s || strlen(s)>max)return 0;
    for(;*s;s++)if(!strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-",*s))return 0;
    return 1;
}
static int valid_archive(const char *s) {
    int portable=!strncmp(s,"portable-",9);const char *prefix=portable?"portable-":"ps5-11.40-";size_t start=portable?9:10,n=start+10;
    if(strlen(s)!=n+36 || strncmp(s,prefix,strlen(prefix)) || s[n-1]!='-' || strcmp(s+n+32,".zip"))return 0;
    char title[10];memcpy(title,s+start,9);title[9]=0;if(!pscloud_title_valid(title))return 0;
    for(unsigned i=0;i<32;i++)if(!strchr("0123456789abcdef",s[n+i]))return 0;
    return 1;
}
static int decode(const char *src,size_t length,char *out,size_t max) {
    size_t n=0;
    for(size_t i=0;i<length;i++) {
        unsigned char ch=(unsigned char)src[i];
        if(ch=='+')ch=' ';
        else if(ch=='%') {
            if(i+2>=length)return -1;
            char hex[3]={src[i+1],src[i+2],0};char *end=NULL;long value=strtol(hex,&end,16);
            if(*end || !strchr("0123456789abcdefABCDEF",hex[0]) || !strchr("0123456789abcdefABCDEF",hex[1]))return -1;
            ch=(unsigned char)value;i+=2;
        }
        if(ch<32 || ch==127 || n+1>=max)return -1;
        out[n++]=(char)ch;
    }
    out[n]=0;return 0;
}
static int parameter(const char *form,const char *key,char *out,size_t max) {
    size_t k=strlen(key);int found=0;*out=0;
    while(*form) {
        const char *end=strchr(form,'&');size_t n=end?(size_t)(end-form):strlen(form);
        if(n>k && !strncmp(form,key,k) && form[k]=='=') {
            if(found || decode(form+k+1,n-k-1,out,max))return -1;
            found=1;
        }
        if(!end)break;
        form=end+1;
    }
    return found?0:-1;
}
static int selection(const char *form,struct pscloud_snapshot *s) {
    memset(s,0,sizeof *s);
    if(parameter(form,"user",s->user,sizeof s->user) || parameter(form,"title",s->title,sizeof s->title) || parameter(form,"slot",s->slot,sizeof s->slot))return -1;
    memset(s->sha256,'0',64);s->sha256[64]=0;
    if(!pscloud_snapshot_valid(s))return -1;
    if(!strcmp(s->slot,"WholeGame"))return 0;
    return strcmp(s->title,"PPSA02433") || (strcmp(s->slot,"PlayerSaveSlot0Save") && strcmp(s->slot,"PlayerSaveProfileSaveData"))?-1:0;
}
static int atomic_config(const char *path,const char *text) {
    char part[1400];snprintf(part,sizeof part,"%s.new",path);
    int fd=open(part,O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,0600);if(fd<0)return -1;
    size_t n=strlen(text),have=0;int bad=0;
    while(have<n) {ssize_t w=write(fd,text+have,n-have);if(w<0 && errno==EINTR)continue;if(w<=0) {bad=1;break;}have+=(size_t)w;}
    if(fsync(fd))bad=1;
    if(close(fd))bad=1;
    if(!bad && rename(part,path))bad=1;
    if(bad)unlink(part);
    return bad?-1:0;
}
static void send_all(int sock,const char *data,size_t size) {
    while(size) {ssize_t n=send(sock,data,size,0);if(n<=0)break;data+=n;size-=(size_t)n;}
}
static void respond(int sock,int status,const char *type,const char *data,size_t size) {
    char cookie[192]={0};
    if(!strncmp(type,"text/html",9))snprintf(cookie,sizeof cookie,"Set-Cookie: PSCloudSession=%s; Path=/; HttpOnly; SameSite=Strict\r\n",token);
    char header[1024];int n=snprintf(header,sizeof header,"HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\nCache-Control: no-store\r\n%sX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\nContent-Security-Policy: default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; frame-ancestors 'none'\r\n\r\n",status,status==200?"OK":"Error",type,size,cookie);
    send_all(sock,header,(size_t)n);send_all(sock,data,size);
}
static void message(int sock,int status,const char *text) {
    pscloud_log(status>=400?"WARN":"EVENT","Dashboard result (HTTP %d): %s",status,text);
    char json[1024];snprintf(json,sizeof json,"{\"ok\":%s,\"message\":\"%s\"}",status==200?"true":"false",text);
    respond(sock,status,"application/json",json,strlen(json));
}
/* Pre-lock errors cannot log beside a save helper that reopens the shared log. */
static void reject(int sock,int status,const char *text) {
    char json[1024];snprintf(json,sizeof json,"{\"ok\":false,\"message\":\"%s\"}",text);
    respond(sock,status,"application/json",json,strlen(json));
}
static void preferences(int sock,const char *form) {
    if(form) {
        char upload[8],refresh[8];
        if(parameter(form,"auto_upload",upload,sizeof upload) || parameter(form,"activity_refresh",refresh,sizeof refresh) ||
           (strcmp(upload,"0") && strcmp(upload,"1")) || (strcmp(refresh,"0") && strcmp(refresh,"1"))) {
            message(sock,400,"Preferences require explicit on/off values");return;
        }
        char close_value[8];int on_close=game_close_backup;
        if(!parameter(form,"game_close_backup",close_value,sizeof close_value)) {if(strcmp(close_value,"0")&&strcmp(close_value,"1")) {message(sock,400,"Invalid game-close backup option");return;}on_close=!strcmp(close_value,"1");}
        char keep[8];int latest=local_keep_latest;
        if(!parameter(form,"local_keep_latest",keep,sizeof keep)) {if(strcmp(keep,"0")&&strcmp(keep,"1")) {message(sock,400,"Invalid local retention option");return;}latest=!strcmp(keep,"1");}
        char compact[8];int data_only=compact_backups;
        if(!parameter(form,"compact_backups",compact,sizeof compact)) {if(strcmp(compact,"0")&&strcmp(compact,"1")) {message(sock,400,"Invalid backup format option");return;}data_only=!strcmp(compact,"1");}
        char file[1200],text[160];snprintf(file,sizeof file,"%s/preferences.conf",root);
        snprintf(text,sizeof text,"AUTO_UPLOAD=%s\nACTIVITY_REFRESH=%s\nLOCAL_KEEP_LATEST=%d\nCOMPACT_BACKUPS=%d\nGAME_CLOSE_BACKUP=%d\n",upload,refresh,latest,data_only,on_close);
        if(atomic_config(file,text)) {message(sock,500,"Preferences could not be saved");return;}
        auto_upload=!strcmp(upload,"1");activity_refresh=!strcmp(refresh,"1");
        local_keep_latest=latest;compact_backups=data_only;game_close_backup=on_close;if(latest)prune_local_history();
        pscloud_log("EVENT","Preferences saved: automatic upload %s; activity refresh %s; back up on game close %s",auto_upload?"on":"off",activity_refresh?"on":"off",game_close_backup?"on":"off");
    }
    char json[512];snprintf(json,sizeof json,"{\"auto_upload\":%s,\"activity_refresh\":%s,\"local_keep_latest\":%s,\"compact_backups\":%s,\"compact_required\":%s,\"game_close_backup\":%s,\"game_close_available\":true,\"sharing_available\":true,\"message\":\"Preferences saved on this PS5\"}",auto_upload?"true":"false",activity_refresh?"true":"false",local_keep_latest?"true":"false",compact_effective()?"true":"false",legacy_firmware()?"false":"true",game_close_backup?"true":"false");
    respond(sock,200,"application/json",json,strlen(json));
}
static void load_preferences(void) {
    /* Inventory first: the console SDK can return unusual descriptors for missing files. */
    DIR *dir=opendir(root);if(!dir)return;int exists=0;struct dirent *e;
    while((e=readdir(dir)))if(!strcmp(e->d_name,"preferences.conf"))exists=1;
    closedir(dir);if(!exists)return;
    auto_upload=0; /* Existing but unreadable preferences must not enable uploads. */
    char path[1200];snprintf(path,sizeof path,"%s/preferences.conf",root);
    int fd=open(path,O_RDONLY | O_NOFOLLOW | O_NONBLOCK);struct stat st;char data[161]={0};
    if(fd<0)return;
    if(!fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_size>0 && st.st_size<161) {
        ssize_t n=read(fd,data,160);
        if(n>0) {
            int upload,refresh,keep,compact,on_close,used=0;
            if(sscanf(data,"AUTO_UPLOAD=%d\nACTIVITY_REFRESH=%d\nLOCAL_KEEP_LATEST=%d\nCOMPACT_BACKUPS=%d\nGAME_CLOSE_BACKUP=%d\n%n",&upload,&refresh,&keep,&compact,&on_close,&used)==5&&used==(int)strlen(data)&&upload>=0&&upload<=1&&refresh>=0&&refresh<=1&&keep>=0&&keep<=1&&compact>=0&&compact<=1&&on_close>=0&&on_close<=1) {auto_upload=upload;activity_refresh=refresh;local_keep_latest=keep;compact_backups=compact;game_close_backup=on_close;}
            else if(sscanf(data,"AUTO_UPLOAD=%d\nACTIVITY_REFRESH=%d\nLOCAL_KEEP_LATEST=%d\nCOMPACT_BACKUPS=%d\n%n",&upload,&refresh,&keep,&compact,&used)==4&&used==(int)strlen(data)&&upload>=0&&upload<=1&&refresh>=0&&refresh<=1&&keep>=0&&keep<=1&&compact>=0&&compact<=1) {auto_upload=upload;activity_refresh=refresh;local_keep_latest=keep;compact_backups=compact;}
            else if(sscanf(data,"AUTO_UPLOAD=%d\nACTIVITY_REFRESH=%d\nLOCAL_KEEP_LATEST=%d\n%n",&upload,&refresh,&keep,&used)==3 && used==(int)strlen(data) && upload>=0 && upload<=1 && refresh>=0 && refresh<=1 && keep>=0 && keep<=1) {auto_upload=upload;activity_refresh=refresh;local_keep_latest=keep;}
            else if(!strcmp(data,"AUTO_UPLOAD=0\nACTIVITY_REFRESH=0\n")) {auto_upload=0;activity_refresh=0;}
            else if(!strcmp(data,"AUTO_UPLOAD=0\nACTIVITY_REFRESH=1\n")) {auto_upload=0;activity_refresh=1;}
            else if(!strcmp(data,"AUTO_UPLOAD=1\nACTIVITY_REFRESH=0\n")) {auto_upload=1;activity_refresh=0;}
            else if(!strcmp(data,"AUTO_UPLOAD=1\nACTIVITY_REFRESH=1\n")) {auto_upload=1;activity_refresh=1;}
            else pscloud_log("WARN","Invalid preferences; automatic upload paused");
        }
    } else {auto_upload=0;pscloud_log("WARN","Unreadable preferences; automatic upload paused");}
    close(fd);
}
static void game_settings_load(void) {
    char path[1400];snprintf(path,sizeof path,"%s/games.conf",root);FILE *f=fopen(path,"r");if(!f)return;
    pthread_mutex_lock(&settings_mutex);game_settings_count=0;char line[128];
    while(fgets(line,sizeof line,f)&&game_settings_count<512) {
        struct game_setting g={{0},1,1,0,0};if(sscanf(line,"%9s CLOSE=%d ALL=%d SYNC=%d APPLY=%d",g.title,&g.close_backup,&g.backup_all,&g.auto_sync,&g.auto_apply)<3||!pscloud_title_valid(g.title))continue;
        g.close_backup=g.close_backup!=0;g.backup_all=g.backup_all!=0;g.auto_sync=g.auto_sync!=0;g.auto_apply=g.auto_sync&&g.auto_apply!=0;game_settings[game_settings_count++]=g;
    }
    pthread_mutex_unlock(&settings_mutex);fclose(f);
}
/* which: 0 = back up when closed, 1 = include in Back up all, 2 = auto download, 3 = auto replace */
static int game_setting(const char *title,int which) {
    int value=which<2;pthread_mutex_lock(&settings_mutex);
    for(unsigned i=0;i<game_settings_count;i++)if(!strcmp(game_settings[i].title,title))value=which==3?game_settings[i].auto_apply:which==2?game_settings[i].auto_sync:which?game_settings[i].backup_all:game_settings[i].close_backup;
    pthread_mutex_unlock(&settings_mutex);return value;
}
static int game_settings_save(const char *title,int close_backup,int backup_all,int auto_sync,int auto_apply) {
    pthread_mutex_lock(&settings_mutex);unsigned i=0;
    while(i<game_settings_count&&strcmp(game_settings[i].title,title))i++;
    if(i==game_settings_count) {if(i==512) {pthread_mutex_unlock(&settings_mutex);return -1;}snprintf(game_settings[i].title,sizeof game_settings[i].title,"%.9s",title);game_settings_count++;}
    game_settings[i].close_backup=close_backup;game_settings[i].backup_all=backup_all;game_settings[i].auto_sync=auto_sync;game_settings[i].auto_apply=auto_sync&&auto_apply;
    char *text=malloc(512*32);size_t n=0;
    for(unsigned k=0;text&&k<game_settings_count;k++)if(!game_settings[k].close_backup||!game_settings[k].backup_all||game_settings[k].auto_sync)n+=(size_t)snprintf(text+n,512*32-n,"%s CLOSE=%d ALL=%d SYNC=%d APPLY=%d\n",game_settings[k].title,game_settings[k].close_backup,game_settings[k].backup_all,game_settings[k].auto_sync,game_settings[k].auto_apply);
    pthread_mutex_unlock(&settings_mutex);
    char path[1400];snprintf(path,sizeof path,"%s/games.conf",root);
    int bad=!text||atomic_config(path,n?text:"");free(text);return bad?-1:0;
}
static void game_settings_json(int sock,const char *query) {
    char title[16];if(parameter(query,"title",title,sizeof title)||!pscloud_title_valid(title)) {reject(sock,400,"Invalid game");return;}
    char json[240];snprintf(json,sizeof json,"{\"title\":\"%s\",\"close_backup\":%s,\"backup_all\":%s,\"auto_sync\":%s,\"auto_apply\":%s}",title,game_setting(title,0)?"true":"false",game_setting(title,1)?"true":"false",game_setting(title,2)?"true":"false",game_setting(title,3)?"true":"false");
    respond(sock,200,"application/json",json,strlen(json));
}
static void escaped(char *out,size_t max,const char *input) {
    size_t n=0;
    while(*input && n+7<max) {
        unsigned char ch=(unsigned char)*input++;
        if(ch=='"' || ch=='\\') {out[n++]='\\';out[n++]=(char)ch;}
        else if(ch<32) {snprintf(out+n,7,"\\u%04x",ch);n+=6;}
        else out[n++]=(char)ch;
    }
    out[n]=0;
}
static int metadata(const char *text,struct pscloud_snapshot *s) {
    int n=0;memset(s,0,sizeof *s);
    int matched=sscanf(text,"USER_ID=%16[0-9a-f]\nTITLE=%9[A-Z0-9]\nSAVE_NAME=%63[A-Za-z0-9_-]\nSHA256=%64[0-9a-f]\n%n",s->user,s->title,s->slot,s->sha256,&n);
    if(matched!=4 || !pscloud_snapshot_valid(s))return -1;
    if(n==(int)strlen(text))return 0;
    int end=0;if(sscanf(text+n,"CREATED_UNIX=%lld\n%n",&s->created,&end)!=1 || s->created<0 || n+end!=(int)strlen(text))return -1;
    return 0;
}
static int remote_snapshot(const struct settings *cloud,const struct pscloud_snapshot *chosen,const char *file,struct pscloud_snapshot *s) {
    char folder[256],url[4096],text[1025];
    if(!valid_archive(file) || pscloud_snapshot_folder(chosen,folder,sizeof folder))return -1;
    snprintf(url,sizeof url,"%s/%s/.pscloud/%s.identity",cloud->url,folder,file);
    struct response r={text,0,1024};long status=webdav(cloud,url,"GET",&r);
    if(status!=200 || metadata(text,s) || strcmp(s->user,chosen->user) || strcmp(s->title,chosen->title) || strcmp(s->slot,chosen->slot))return -1;
    return 0;
}
static void games(int sock) {
    const size_t cap=262144;char *json=malloc(cap);if(!json) {message(sock,500,"Out of memory");return;}
    snprintf(json,cap,"{\"games\":[");size_t pos=strlen(json);unsigned count=0;
    int dir=pscloud_open_directory(home);if(dir<0) {free(json);message(sock,500,"User save folders unavailable");return;}
    int scan=openat(dir,".",O_RDONLY | O_DIRECTORY);DIR *users=scan>=0?fdopendir(scan):NULL;
    if(!users) {if(scan>=0)close(scan);close(dir);free(json);message(sock,500,"User enumeration failed");return;}
    struct dirent *u;
    for(unsigned priority=0;priority<2;priority++) {
    rewinddir(users);
    while((u=readdir(users)) && count<256) {
        if(!safe_word(u->d_name,16))continue;
        char path[1400];snprintf(path,sizeof path,"%s/%s/savedata_prospero",home,u->d_name);
        int fd=pscloud_open_directory(path);if(fd<0)continue;
        DIR *titles=fdopendir(fd);if(!titles) {close(fd);continue;}struct dirent *t;
        while((t=readdir(titles)) && count<256) {
            if(strlen(t->d_name)!=9 || strncmp(t->d_name,"PPSA",4) || !safe_word(t->d_name,9))continue;
            if((!strcmp(t->d_name,"PPSA02433"))!=(priority==0))continue;
            char name[256],escaped_name[1536],meta[1400];int installed=!pscloud_app_meta_dir(appmeta,t->d_name,meta,sizeof meta);
            pscloud_app_name(appmeta,t->d_name,name,sizeof name);if(!installed)pscloud_save_title_name(home,u->d_name,t->d_name,name,sizeof name);
            escaped(escaped_name,sizeof escaped_name,name);
            int titlefd=openat(fd,t->d_name,O_RDONLY | O_DIRECTORY | O_NOFOLLOW);if(titlefd<0)continue;
            DIR *slots=fdopendir(titlefd);if(!slots) {close(titlefd);continue;}struct dirent *e;
            while((e=readdir(slots)) && count<256) {
                if(strncmp(e->d_name,"sdimg_",6) || !strncmp(e->d_name,"sdimg_sce_bu_",13) || !safe_word(e->d_name+6,63))continue;
                int supported=safe_word(e->d_name+6,63);
                char main[128],sub[128],detail[256],emain[800],esub[800],edetail[1600];
                pscloud_save_slot_info(home,u->d_name,t->d_name,e->d_name+6,main,sizeof main,sub,sizeof sub,detail,sizeof detail);
                escaped(emain,sizeof emain,main);escaped(esub,sizeof esub,sub);escaped(edetail,sizeof edetail,detail);
                int written=snprintf(json+pos,cap-pos,"%s{\"user\":\"%s\",\"title\":\"%s\",\"slot\":\"%s\",\"name\":\"%s\",\"icon\":\"/api/icon?title=%s\",\"installed\":%s,\"supported\":%s,\"label\":\"%s\",\"sub\":\"%s\",\"detail\":\"%s\"}",count?",":"",u->d_name,t->d_name,e->d_name+6,escaped_name,t->d_name,installed?"true":"false",supported?"true":"false",emain,esub,edetail);
                if(written<0 || (size_t)written>=cap-pos-4)break;
                pos+=(size_t)written;count++;
            }
            closedir(slots);
        }
        closedir(titles);
    }
    }
    closedir(users);close(dir);snprintf(json+pos,cap-pos,"],\"truncated\":%s}",count>=256?"true":"false");cache_store(games_cache,sizeof games_cache,json);respond(sock,200,"application/json",json,strlen(json));free(json);
}
/* Read-only: console profiles so a receiver without any saves can still be chosen. */
static void users(int sock) {
    char json[8192]="{\"users\":[";size_t pos=strlen(json);unsigned count=0;
    int dir=pscloud_open_directory(home);if(dir<0) {message(sock,500,"User folders unavailable");return;}
    int scan=openat(dir,".",O_RDONLY | O_DIRECTORY);DIR *list=scan>=0?fdopendir(scan):NULL;
    if(!list) {if(scan>=0)close(scan);close(dir);message(sock,500,"User enumeration failed");return;}
    struct dirent *u;
    while((u=readdir(list)) && count<32) {
        if(!safe_word(u->d_name,16))continue;
        struct stat st;if(fstatat(dir,u->d_name,&st,AT_SYMLINK_NOFOLLOW) || !S_ISDIR(st.st_mode))continue;
        char name[64]={0},escaped_name[400];char path[1400];snprintf(path,sizeof path,"%s/username.dat",u->d_name);
        int fd=openat(dir,path,O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
        if(fd>=0) {ssize_t got=read(fd,name,sizeof name-1);close(fd);if(got<0)got=0;name[got]=0;for(ssize_t i=0;i<got;i++)if((unsigned char)name[i]<32)name[i]=0;}
        escaped(escaped_name,sizeof escaped_name,name);
        int written=snprintf(json+pos,sizeof json-pos,"%s{\"user\":\"%s\",\"name\":\"%s\"}",count?",":"",u->d_name,escaped_name);
        if(written<0 || (size_t)written>=sizeof json-pos-4)break;
        pos+=(size_t)written;count++;
    }
    closedir(list);close(dir);snprintf(json+pos,sizeof json-pos,"]}");respond(sock,200,"application/json",json,strlen(json));
}
/* Read-only: whether a profile already has saves for a game, which slots exist,
 * whether the game is installed and whether any save is mounted (game likely running). */
static void save_status(int sock,const char *query) {
    char user[32],title[16];
    if(parameter(query,"user",user,sizeof user) || !safe_word(user,16) || parameter(query,"title",title,sizeof title) || !pscloud_title_valid(title)) {message(sock,400,"Choose a profile and game");return;}
    char version[32],name[256],escaped_name[1536];int installed=!pscloud_app_version(appmeta,title,version,sizeof version),running=title_running(title);
    pscloud_app_name(appmeta,title,name,sizeof name);if(!installed)pscloud_save_title_name(home,user,title,name,sizeof name);escaped(escaped_name,sizeof escaped_name,name);
    char json[16384];int pos=snprintf(json,sizeof json,"{\"title\":\"%s\",\"name\":\"%s\",\"installed\":%s,\"mounted\":%s,\"running\":%s,\"slots\":[",title,escaped_name,installed?"true":"false",pscloud_no_foreign_mount()?"true":"false",running>0?"true":running<0?"null":"false");
    char path[1400];snprintf(path,sizeof path,"%s/%s/savedata_prospero/%s",home,user,title);
    int fd=pscloud_open_directory(path);DIR *slots=fd>=0?fdopendir(fd):NULL;unsigned count=0;
    if(!slots&&fd>=0)close(fd);
    if(slots) {
        struct dirent *e;
        while((e=readdir(slots)) && count<128) {
            if(strncmp(e->d_name,"sdimg_",6) || !strncmp(e->d_name,"sdimg_sce_bu_",13) || !safe_word(e->d_name+6,63))continue;
            int written=snprintf(json+pos,sizeof json-(size_t)pos,"%s\"%s\"",count?",":"",e->d_name+6);
            if(written<0 || (size_t)written>=sizeof json-(size_t)pos-4)break;
            pos+=written;count++;
        }
        closedir(slots);
    }
    snprintf(json+pos,sizeof json-(size_t)pos,"]}");respond(sock,200,"application/json",json,strlen(json));
}
static void backups(int sock,const char *query) {
    struct pscloud_snapshot chosen;struct settings cloud={0};
    if(selection(query,&chosen) || pscloud_configure(cloudpath,&cloud)) {message(sock,400,"Select a supported save and connect Nextcloud first");return;}
    char folder[256],url[4096];pscloud_snapshot_folder(&chosen,folder,sizeof folder);
    snprintf(url,sizeof url,"%s/%s/.pscloud/",cloud.url,folder);
    char *xml=malloc(1024*1024+1);if(!xml) {message(sock,500,"Out of memory");return;}
    struct response r={xml,0,1024*1024};long status=webdav(&cloud,url,"PROPFIND",&r);
    if(status==404) {free(xml);respond(sock,200,"application/json","{\"backups\":[]}",14);return;}
    if(status!=207 && status!=200) {free(xml);message(sock,502,"Cloud listing failed; check settings or connection");return;}
    char json[32768]="{\"backups\":[";size_t pos=strlen(json);unsigned count=0;char *cursor=xml;
    while((cursor=strstr(cursor,"href>")) && count<48) {
        cursor+=5;char *end=strchr(cursor,'<');if(!end)break;
        size_t len=(size_t)(end-cursor);if(!len || len>4096)continue;
        char *begin=cursor;for(char *p=cursor;p<end;p++)if(*p=='/')begin=p+1;
        size_t n=(size_t)(end-begin);if(n<10 || n>200 || strncmp(end-9,".identity",9))continue;
        char file[128];if(n-9>=sizeof file)continue;memcpy(file,begin,n-9);file[n-9]=0;
        struct pscloud_snapshot snapshot;
        if(remote_snapshot(&cloud,&chosen,file,&snapshot))continue;
        int wrote=snprintf(json+pos,sizeof json-pos,"%s{\"file\":\"%s\",\"sha256\":\"%s\",\"slot\":\"%s\",\"created\":%lld}",count?",":"",file,snapshot.sha256,snapshot.slot,snapshot.created);
        if(wrote<0 || (size_t)wrote>=sizeof json-pos-4)break;
        pos+=(size_t)wrote;count++;
    }
    free(xml);strcat(json,"]}");respond(sock,200,"application/json",json,strlen(json));
}
static int upload_selected(const char *file) {
    background_cancelled=0;background_result=0;
    struct settings s={0};if(pscloud_configure(cloudpath,&s))return 2;
    if(setenv("PSCLOUD_URL",s.url,1) || setenv("PSCLOUD_USER",s.user,1) || setenv("PSCLOUD_PASSWORD",s.password,1) || setenv("PSCLOUD_CA_BUNDLE",s.ca,1))return 2;
    char spool[1400];snprintf(spool,sizeof spool,"%s/spool",root);
    char *args[]={"worker",spool,"--once",(char *)file,NULL};int result=pscloud_worker_main(file?4:3,args);unsetenv("PSCLOUD_PASSWORD");
    signal(SIGTERM,stop_server);signal(SIGINT,stop_server);if(!result)prune_local_history();return result;
}
static int upload_queue(void) {return upload_selected(NULL);}
static int pending_archive(const char *file,struct pscloud_snapshot *s,unsigned char **data,size_t *size) {
    if(!valid_archive(file))return -1;
    char path[1400],ready[144],identity[144];snprintf(path,sizeof path,"%s/spool",root);
    int dir=pscloud_open_directory(path);if(dir<0)return -1;
    snprintf(ready,sizeof ready,"%s.ready",file);snprintf(identity,sizeof identity,"%s.identity",file);
    int scan=openat(dir,".",O_RDONLY | O_DIRECTORY | O_NOFOLLOW);DIR *d=scan>=0?fdopendir(scan):NULL;
    int found=0;struct dirent *e;if(d) {while((e=readdir(d)))if(!strcmp(e->d_name,ready))found=1;closedir(d);}else if(scan>=0)close(scan);
    int bad=!found || pscloud_snapshot_read(dir,identity,s);
    if(!bad && data)bad=pscloud_read_archive(dir,ready,data,size) || pscloud_verify_hash(*data,*size,s->sha256);
    close(dir);return bad?-1:0;
}
static void prune_local_history(void) {
    if(!local_keep_latest)return;
    int parent=pscloud_open_directory(root);if(parent<0)return;
    int active=pscloud_active_marker(parent);close(parent);if(active!=0)return;
    char path[1400];snprintf(path,sizeof path,"%s/spool",root);int dir=pscloud_open_directory(path);if(dir<0)return;
    struct kept {char file[128];struct pscloud_snapshot s;struct timespec modified;};
    struct kept *items=calloc(256,sizeof *items);if(!items) {close(dir);return;}
    int scan=openat(dir,".",O_RDONLY | O_DIRECTORY | O_NOFOLLOW);DIR *d=scan<0?NULL:fdopendir(scan);
    unsigned count=0;int overflow=0;struct dirent *e;
    if(d) {while((e=readdir(d))) {
        size_t n=strlen(e->d_name);if(n<6 || n-5>=128 || strcmp(e->d_name+n-5,".sent"))continue;
        char file[128],identity[144];memcpy(file,e->d_name,n-5);file[n-5]=0;if(!valid_archive(file))continue;
        if(count==256) {overflow=1;break;}
        snprintf(identity,sizeof identity,"%s.identity",file);
        struct stat st;
        if(!pscloud_snapshot_read(dir,identity,&items[count].s)&&!fstatat(dir,e->d_name,&st,AT_SYMLINK_NOFOLLOW)&&S_ISREG(st.st_mode)&&st.st_nlink==1) {strcpy(items[count].file,file);items[count].modified=st.st_mtim;count++;}
    }closedir(d);}else {if(scan>=0)close(scan);overflow=1;}
    unsigned removed=0;unsigned char checked[256]={0};
    for(unsigned i=0;!overflow&&i<count;i++) {
        unsigned latest=i;
        for(unsigned j=0;j<count;j++)if(!strcmp(items[i].s.user,items[j].s.user)&&!strcmp(items[i].s.title,items[j].s.title)&&!strcmp(items[i].s.slot,items[j].s.slot)&&
            (items[j].s.created>items[latest].s.created || (items[j].s.created==items[latest].s.created&&
            (items[j].modified.tv_sec>items[latest].modified.tv_sec || (items[j].modified.tv_sec==items[latest].modified.tv_sec&&
            (items[j].modified.tv_nsec>items[latest].modified.tv_nsec || (items[j].modified.tv_nsec==items[latest].modified.tv_nsec&&strcmp(items[j].file,items[latest].file)>0)))))))latest=j;
        if(i==latest)continue;
        char name[144],hash[65];
        if(!checked[latest]) {snprintf(name,sizeof name,"%s.sent",items[latest].file);int fd=openat(dir,name,O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
            checked[latest]=fd>=0&&!pscloud_file_hash(fd,hash)&&!strcmp(hash,items[latest].s.sha256)?1:2;if(fd>=0)close(fd);}
        if(checked[latest]!=1)continue;
        struct stat st;snprintf(name,sizeof name,"%s.ready",items[i].file);if(!fstatat(dir,name,&st,AT_SYMLINK_NOFOLLOW))continue;
        snprintf(name,sizeof name,"%s.replace",items[i].file);if(!fstatat(dir,name,&st,AT_SYMLINK_NOFOLLOW))continue;
        snprintf(name,sizeof name,"%s.sent",items[i].file);
        if(fstatat(dir,name,&st,AT_SYMLINK_NOFOLLOW)||!S_ISREG(st.st_mode)||st.st_nlink!=1)continue;
        if(!unlinkat(dir,name,0)) {snprintf(name,sizeof name,"%s.identity",items[i].file);(void)unlinkat(dir,name,0);removed++;}
    }
    if(removed) {fsync(dir);pscloud_log("EVENT","Local history cleanup removed %u older uploaded backups; latest, pending uploads and rollback retained",removed);}
    free(items);close(dir);
}
static void queue_list(int sock) {
    char path[1400],json[65536]="{\"items\":[";snprintf(path,sizeof path,"%s/spool",root);
    int dir=pscloud_open_directory(path);size_t pos=strlen(json);unsigned count=0,shown=0;int truncated=0;
    if(dir>=0) {
        int scan=openat(dir,".",O_RDONLY | O_DIRECTORY | O_NOFOLLOW);DIR *d=scan>=0?fdopendir(scan):NULL;struct dirent *e;
        if(d) {
            while((e=readdir(d))) {
                size_t n=strlen(e->d_name);if(n<7 || n>=144 || strcmp(e->d_name+n-6,".ready"))continue;
                char file[128];if(n-6>=sizeof file)continue;memcpy(file,e->d_name,n-6);file[n-6]=0;
                if(!valid_archive(file))continue;
                count++;
                if(truncated)continue;
                struct pscloud_snapshot s={0};int valid=pending_archive(file,&s,NULL,NULL)==0;
                char name[256],label[1536];pscloud_app_name(appmeta,valid?s.title:"PPSA02433",name,sizeof name);escaped(label,sizeof label,name);
                int got=snprintf(json+pos,sizeof json-pos,"%s{\"file\":\"%s\",\"user\":\"%s\",\"title\":\"%s\",\"slot\":\"%s\",\"name\":\"%s\",\"created\":%lld,\"valid\":%s}",shown?",":"",file,s.user,s.title,s.slot,label,s.created,valid?"true":"false");
                if(got<0 || (size_t)got>=sizeof json-pos-64) {truncated=1;json[pos]=0;continue;}
                pos+=(size_t)got;shown++;
            }
            closedir(d);
        }else if(scan>=0)close(scan);
        close(dir);
    }
    snprintf(json+pos,sizeof json-pos,"],\"count\":%u,\"truncated\":%s}",count,truncated?"true":"false");cache_store(queue_cache,sizeof queue_cache,json);respond(sock,200,"application/json",json,strlen(json));
}

/* ---------- Storage transparency and USB drives ----------
 * Everything PSCloud keeps lives under <root> (/data/pscloud) plus the log. USB drives are
 * the console's exFAT mounts /mnt/usb0-7; PSCloud only writes inside <drive>/PSCloud. */
static void tree_size(int dir,unsigned depth,unsigned long long *bytes,unsigned *files) {
    int scan=dup(dir);DIR *d=scan<0?NULL:fdopendir(scan);if(!d) {if(scan>=0)close(scan);return;}
    struct dirent *e;
    while((e=readdir(d))&&*files<100000) {
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        struct stat st;if(fstatat(dir,e->d_name,&st,AT_SYMLINK_NOFOLLOW))continue;
        if(S_ISREG(st.st_mode)) {*bytes+=(unsigned long long)st.st_size;(*files)++;}
        else if(S_ISDIR(st.st_mode)&&depth<6) {int sub=openat(dir,e->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);if(sub>=0) {tree_size(sub,depth+1,bytes,files);close(sub);}}
    }
    closedir(d);
}
static void path_size(const char *path,unsigned long long *bytes,unsigned *files,int *exists) {
    struct stat st;*bytes=0;*files=0;*exists=!lstat(path,&st);if(!*exists)return;
    if(S_ISREG(st.st_mode)) {*bytes=(unsigned long long)st.st_size;*files=1;return;}
    if(!S_ISDIR(st.st_mode))return;
    int dir=pscloud_open_directory(path);if(dir>=0) {tree_size(dir,0,bytes,files);close(dir);}
}
static int usb_mount(unsigned index,char *out,size_t max) {
#ifdef PSCLOUD_HOST_TEST
    const char *base=getenv("PSCLOUD_TEST_USB_ROOT");if(!base)return -1;
    if(snprintf(out,max,"%s/usb%u",base,index)>=(int)max)return -1;
    struct stat st;return !stat(out,&st)&&S_ISDIR(st.st_mode)?0:-1;
#else
    if(snprintf(out,max,"/mnt/usb%u",index)>=(int)max)return -1;
    struct stat st,parent;
    if(stat(out,&st)||!S_ISDIR(st.st_mode)||stat("/mnt",&parent))return -1;
    return st.st_dev!=parent.st_dev?0:-1; /* an empty mount point is not a drive */
#endif
}
static int usb_drive(const char *form,char *out,size_t max) {
    char id[8];if(parameter(form,"drive",id,sizeof id)||strlen(id)!=4||strncmp(id,"usb",3)||id[3]<'0'||id[3]>'7')return -1;
    return usb_mount((unsigned)(id[3]-'0'),out,max);
}
static void space_totals(unsigned long long *leftover_bytes,unsigned *leftover_files,unsigned long long *safety_bytes,unsigned *safety_files);
static void storage_json(int sock) {
    struct place {const char *key,*label,*purpose;char path[1400];} places[9];unsigned n=0;
    #define PLACE(k,l,p,...) do{places[n].key=k;places[n].label=l;places[n].purpose=p;snprintf(places[n].path,sizeof places[n].path,__VA_ARGS__);n++;}while(0)
    PLACE("backups","Local backups","Every backup made on this PS5. Ones waiting are uploaded next; uploaded ones stay as history unless you keep only the newest.","%s/spool",root);
    PLACE("shared","Shared saves","ZIPs exported for sharing and ones you received. The 2 newest per game are kept.","%s/share",root);
    PLACE("rollback","Restore safety copies","Your save as it was before each restore or replace, so it can be put back. The 3 newest per game are kept.","%s/rollback",root);
    PLACE("downloads","Cloud downloads","Backups downloaded from your cloud to this PS5.","%s/downloads",root);
    PLACE("staging","Leftover working files","Copies made while a backup, check or replace runs. Normally removed automatically; leftovers come from interrupted operations or older versions.","%s/staging",root);
    PLACE("log","Activity log","Everything PSCloud did, shown on the Activity page.","%s",logpath);
    PLACE("settings","Settings","Your preferences and cloud sign-in, readable only by PSCloud on this PS5.","%s/preferences.conf",root);
    PLACE("saves","Your PS5 saves","The console's own save files. PSCloud only reads them, except when you choose Restore or Replace.","%s",home);
    #undef PLACE
    char json[16384];int pos=snprintf(json,sizeof json,"{\"root\":\"%s\",\"places\":[",root);
    for(unsigned i=0;i<n;i++) {
        unsigned long long bytes=0;unsigned files=0;int exists=0;
        if(strcmp(places[i].key,"saves"))path_size(places[i].path,&bytes,&files,&exists);else exists=1;
        /* Leftovers and safety copies are classified file by file (old stage folders hold both). */
        if(!strcmp(places[i].key,"staging")) {unsigned long long b2;unsigned f2;space_totals(&bytes,&files,&b2,&f2);exists=files>0;}
        if(!strcmp(places[i].key,"rollback")) {unsigned long long b1;unsigned f1;space_totals(&b1,&f1,&bytes,&files);exists=files>0;}
        char path[2900];escaped(path,sizeof path,places[i].path);
        int w=snprintf(json+pos,sizeof json-(size_t)pos,"%s{\"key\":\"%s\",\"label\":\"%s\",\"purpose\":\"%s\",\"path\":\"%s\",\"exists\":%s,\"files\":%u,\"bytes\":%llu}",
            i?",":"",places[i].key,places[i].label,places[i].purpose,path,exists?"true":"false",files,bytes);
        if(w<0||(size_t)w>=sizeof json-(size_t)pos-64)break;
        pos+=w;
    }
    struct statvfs fs;unsigned long long free_bytes=0,total=0;
    if(!statvfs(root,&fs)) {free_bytes=(unsigned long long)fs.f_bavail*fs.f_frsize;total=(unsigned long long)fs.f_blocks*fs.f_frsize;}
    snprintf(json+pos,sizeof json-(size_t)pos,"],\"free\":%llu,\"total\":%llu}",free_bytes,total);
    respond(sock,200,"application/json",json,strlen(json));
}
static void usb_list(int sock) {
    char json[4096];int pos=snprintf(json,sizeof json,"{\"drives\":[");unsigned count=0;
    for(unsigned i=0;i<8;i++) {
        char path[1200];if(usb_mount(i,path,sizeof path))continue;
        struct statvfs fs;unsigned long long free_bytes=0,total=0;
        if(!statvfs(path,&fs)) {free_bytes=(unsigned long long)fs.f_bavail*fs.f_frsize;total=(unsigned long long)fs.f_blocks*fs.f_frsize;}
        char shown[2400];escaped(shown,sizeof shown,path);
        pos+=snprintf(json+pos,sizeof json-(size_t)pos,"%s{\"id\":\"usb%u\",\"path\":\"%s\",\"free\":%llu,\"total\":%llu}",count++?",":"",i,shown,free_bytes,total);
    }
    snprintf(json+pos,sizeof json-(size_t)pos,"]}");respond(sock,200,"application/json",json,strlen(json));
}
/* A file inside <drive>/PSCloud, given relative to the drive: no "..", no hidden parts, .zip. */
static int usb_relative(const char *rel) {
    size_t n=strlen(rel);
    if(n<12||n>400||strncmp(rel,"PSCloud/",8)||strcasecmp(rel+n-4,".zip"))return 0;
    for(const char *p=rel;*p;p++)if((unsigned char)*p<32||*p=='\\'||*p==':'||*p=='*'||*p=='?'||*p=='"'||*p=='<'||*p=='>'||*p=='|')return 0;
    for(const char *part=rel;part;) {const char *slash=strchr(part,'/');size_t len=slash?(size_t)(slash-part):strlen(part);if(!len||part[0]=='.')return 0;part=slash?slash+1:NULL;}
    return 1;
}
static void usb_scan(const char *drive,const char *rel,unsigned depth,char *json,size_t cap,int *pos,unsigned *count) {
    char path[2000];if(snprintf(path,sizeof path,"%s/%s",drive,rel)>=(int)sizeof path)return;
    DIR *d=opendir(path);if(!d)return;struct dirent *e;
    while((e=readdir(d))&&*count<300) {
        if(e->d_name[0]=='.')continue;
        char child[1000],full[3000];if(snprintf(child,sizeof child,"%s/%s",rel,e->d_name)>=(int)sizeof child)continue;
        snprintf(full,sizeof full,"%s/%s",drive,child);struct stat st;if(lstat(full,&st))continue;
        if(S_ISDIR(st.st_mode)&&depth<3)usb_scan(drive,child,depth+1,json,cap,pos,count);
        else if(S_ISREG(st.st_mode)&&usb_relative(child)) {
            char shown[2100];escaped(shown,sizeof shown,child);
            int w=snprintf(json+*pos,cap-(size_t)*pos,"%s{\"path\":\"%s\",\"size\":%lld,\"modified\":%lld}",*count?",":"",shown,(long long)st.st_size,(long long)st.st_mtime);
            if(w<0||(size_t)w>=cap-(size_t)*pos-16)break;
            *pos+=w;(*count)++;
        }
    }
    closedir(d);
}
static void usb_files(int sock,const char *query) {
    char drive[1200];if(usb_drive(query,drive,sizeof drive)) {reject(sock,400,"USB drive not found");return;}
    static char json[262144];int pos=snprintf(json,sizeof json,"{\"files\":[");unsigned count=0;
    usb_scan(drive,"PSCloud",0,json,sizeof json,&pos,&count);
    snprintf(json+pos,sizeof json-(size_t)pos,"]}");respond(sock,200,"application/json",json,strlen(json));
}
static void usb_file(int sock,const char *query) {
    char drive[1200],rel[512],path[1800];
    if(usb_drive(query,drive,sizeof drive)||parameter(query,"path",rel,sizeof rel)||!usb_relative(rel)) {reject(sock,400,"USB file not found");return;}
    snprintf(path,sizeof path,"%s/%s",drive,rel);
    int fd=open(path,O_RDONLY|O_NOFOLLOW);struct stat st;
    if(fd<0||fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<=0||st.st_size>512LL*1024*1024) {if(fd>=0)close(fd);reject(sock,404,"USB file not found");return;}
    char header[512];int n=snprintf(header,sizeof header,"HTTP/1.1 200 OK\r\nContent-Type: application/zip\r\nContent-Length: %lld\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n\r\n",(long long)st.st_size);
    send_all(sock,header,(size_t)n);
    char *buffer=malloc(1U<<20);long long left=st.st_size;
    while(buffer&&left>0) {ssize_t got=read(fd,buffer,left>(1<<20)?(1<<20):(size_t)left);if(got<=0)break;send_all(sock,buffer,(size_t)got);left-=got;}
    free(buffer);close(fd);
}
/* exFAT-safe folder name for a game: "<name> (<ID>)". */
static void usb_game_folder(const char *title,char *out,size_t max) {
    char name[256],clean[96];pscloud_app_name(appmeta,title,name,sizeof name);size_t n=0;
    for(const char *p=name;*p&&n<sizeof clean-1;p++) {unsigned char c=(unsigned char)*p;clean[n++]=(c<32||strchr("\\/:*?\"<>|",c))?'_':(char)c;}
    while(n&&(clean[n-1]==' '||clean[n-1]=='.'))n--;
    clean[n]=0;
    if(!strcmp(clean,title)||!n)snprintf(out,max,"%s",title);else snprintf(out,max,"%s (%s)",clean,title);
}
/* Copy a PSCloud ZIP to <drive>/PSCloud/<kind>/<game>/ through a temporary file, then read
 * the copy back and compare checksums. 1 = copied, 0 = identical copy already there. */
/* What a save package contains, ignoring when it was made: SHA-256 of its manifest's SLOT
 * lines (each slot's name, size and checksum). Two exports of an unchanged save match. */
static int package_fingerprint(const char *path,char out[65]) {
    int fd=open(path,O_RDONLY|O_NOFOLLOW);if(fd<0)return -1;
    unsigned char head[65536];ssize_t n=pread(fd,head,sizeof head,0);close(fd);
    if(n<30||memcmp(head,"PK\3\4",4))return -1;
    size_t name=head[26]|((size_t)head[27]<<8),extra=head[28]|((size_t)head[29]<<8),size=head[18]|((size_t)head[19]<<8)|((size_t)head[20]<<16)|((size_t)head[21]<<24);
    if(name!=12||memcmp(head+30,"manifest.txt",12)||30+name+extra+size>(size_t)n)return -1;
    const char *text=(const char *)head+30+name+extra,*end=text+size;
    EVP_MD_CTX *ctx=EVP_MD_CTX_new();if(!ctx||!EVP_DigestInit_ex(ctx,EVP_sha256(),NULL)) {EVP_MD_CTX_free(ctx);return -1;}
    unsigned lines=0;
    for(const char *line=text;line<end;) {
        const char *next=memchr(line,'\n',(size_t)(end-line));size_t len=next?(size_t)(next-line):(size_t)(end-line);
        if(len>5&&!memcmp(line,"SLOT=",5)) {EVP_DigestUpdate(ctx,line,len);EVP_DigestUpdate(ctx,"\n",1);lines++;}
        if(!next)break;
        line=next+1;
    }
    unsigned char digest[32];unsigned dsize=0;int ok=lines&&EVP_DigestFinal_ex(ctx,digest,&dsize)&&dsize==32;EVP_MD_CTX_free(ctx);
    if(!ok)return -1;
    for(unsigned i=0;i<32;i++)snprintf(out+2*i,3,"%02x",digest[i]);
    return 0;
}
/* An existing copy of this save on the drive: same file checksum, or the same contents
 * (fingerprint) under another name. Looks in the game folder and the older layout's folders. */
static int usb_find_same(const char *drive,const char *folder,const char *hash,const char *fingerprint,char *found,size_t max) {
    static const char *const places[]={"","Backups/","Shared saves/"};
    for(unsigned i=0;i<3;i++) {
        char dir[1600];snprintf(dir,sizeof dir,"%s/PSCloud/%s%s",drive,places[i],folder);
        DIR *d=opendir(dir);if(!d)continue;struct dirent *e;
        while((e=readdir(d))) {
            size_t n=strlen(e->d_name);if(e->d_name[0]=='.'||n<5||strcasecmp(e->d_name+n-4,".zip"))continue;
            char path[2000],other[65];snprintf(path,sizeof path,"%s/%s",dir,e->d_name);
            int same=(*fingerprint&&!package_fingerprint(path,other)&&!strcmp(other,fingerprint));
            if(!same) {int fd=open(path,O_RDONLY|O_NOFOLLOW);same=fd>=0&&!pscloud_file_hash(fd,other)&&!strcmp(other,hash);if(fd>=0)close(fd);}
            if(same) {snprintf(found,max,"PSCloud/%s%s/%s",places[i],folder,e->d_name);closedir(d);return 1;}
        }
        closedir(d);
    }
    return 0;
}
/* Copy a PSCloud ZIP to <drive>/PSCloud/<Game (ID)>/ through a temporary file, then read the
 * copy back and compare checksums. 1 = copied, 0 = the same save is already there. */
static int usb_copy(const char *drive,const char *kind,const char *source_dir,const char *file,char *shown,size_t shown_max) {
    (void)kind; /* backups and shared saves share one format and one folder per game */
    char title[10];memcpy(title,file+(strncmp(file,"portable-",9)?10:9),9);title[9]=0;
    char folder[200],dir[1600],src[1600],dst[2000],tmp[2000];usb_game_folder(title,folder,sizeof folder);
    snprintf(src,sizeof src,"%s/%s",source_dir,file);
    int in=open(src,O_RDONLY|O_NOFOLLOW);
    if(in<0) {snprintf(src,sizeof src,"%s/%s.ready",source_dir,file);in=open(src,O_RDONLY|O_NOFOLLOW);} /* waiting to upload */
    if(in<0) {snprintf(src,sizeof src,"%s/%s.sent",source_dir,file);in=open(src,O_RDONLY|O_NOFOLLOW);} /* already uploaded */
    if(in<0)return -1;
    char want[65],have[65],fingerprint[65]={0};if(pscloud_file_hash(in,want)) {close(in);return -1;}
    if(package_fingerprint(src,fingerprint))fingerprint[0]=0;
    if(usb_find_same(drive,folder,want,fingerprint,shown,shown_max)) {close(in);return 0;}
    snprintf(dir,sizeof dir,"%s/PSCloud",drive);(void)mkdir(dir,0777);
    snprintf(dir,sizeof dir,"%s/PSCloud/%s",drive,folder);(void)mkdir(dir,0777);
    snprintf(dst,sizeof dst,"%s/%s",dir,file);snprintf(tmp,sizeof tmp,"%s/.%s.part",dir,file);
    snprintf(shown,shown_max,"PSCloud/%s/%s",folder,file);
    if(lseek(in,0,SEEK_SET)) {close(in);return -1;}
    int out=open(tmp,O_WRONLY|O_CREAT|O_TRUNC,0666);if(out<0) {close(in);return -1;}
    char *buffer=malloc(1U<<20);int bad=!buffer;
    while(!bad) {ssize_t got=read(in,buffer,1U<<20);if(got<0)bad=1;if(got<=0)break;
        for(ssize_t done=0;done<got&&!bad;) {ssize_t w=write(out,buffer+done,(size_t)(got-done));if(w<=0)bad=1;else done+=w;}}
    free(buffer);close(in);if(fsync(out))bad=1;if(close(out))bad=1;
    if(!bad&&rename(tmp,dst))bad=1;
    if(!bad) {int check=open(dst,O_RDONLY|O_NOFOLLOW);bad=check<0||pscloud_file_hash(check,have)||strcmp(want,have);if(check>=0)close(check);}
    if(bad) {unlink(tmp);pscloud_log("WARN","USB copy of %s failed or did not verify",file);return -1;}
    pscloud_log("EVENT","Copied %s to USB (%s), checksum verified",file,shown);return 1;
}
/* Save a game's current progress straight to USB: export, copy (skipped when unchanged),
 * then drop the temporary export so it does not pile up on the PS5. */
static void usb_save_game(int sock,const char *form,const struct pscloud_snapshot *chosen) {
    char drive[1200],closed[8],file[128],hash[65],shown[600],text[900];
    if(usb_drive(form,drive,sizeof drive)) {message(sock,400,"Plug in a USB drive (exFAT) and choose it");return;}
    if(parameter(form,"closed",closed,sizeof closed)||strcmp(closed,"yes")) {message(sock,400,"Close the game and switch on Game closed first");return;}
    if(pscloud_share_game(home,root,appmeta,chosen->user,chosen->title,0,NULL,0,NULL,file,hash)) {message(sock,500,"Could not read this game's save; check Activity");return;}
    char share[1400];snprintf(share,sizeof share,"%s/share",root);
    int r=usb_copy(drive,"Saves",share,file,shown,sizeof shown);
    char path[1600];snprintf(path,sizeof path,"%s/%s",share,file);unlink(path);
    if(r<0) {message(sock,500,"Copy to USB failed; check the drive has space and is exFAT");return;}
    snprintf(text,sizeof text,r?"Saved to USB: %s":"Already on USB, unchanged: %s",shown);message(sock,200,text);
}
static void usb_delete(int sock,const char *form) {
    char drive[1200],rel[512],path[1800];
    if(usb_drive(form,drive,sizeof drive)||parameter(form,"path",rel,sizeof rel)||!usb_relative(rel)) {message(sock,400,"USB file not found");return;}
    snprintf(path,sizeof path,"%s/%s",drive,rel);struct stat st;
    if(lstat(path,&st)||!S_ISREG(st.st_mode)||unlink(path)) {message(sock,404,"USB file not found");return;}
    pscloud_log("EVENT","Deleted %s from USB",rel);message(sock,200,"Deleted from USB");
}
static void usb_copy_action(int sock,const char *form) {
    char drive[1200],kind[16],file[128],shown[600];
    if(usb_drive(form,drive,sizeof drive)) {message(sock,400,"Plug in a USB drive (exFAT) and choose it");return;}
    if(parameter(form,"kind",kind,sizeof kind)||(strcmp(kind,"backup")&&strcmp(kind,"shared"))||parameter(form,"file",file,sizeof file)||!valid_archive(file)) {message(sock,400,"Choose a backup or shared save");return;}
    char source[1400];snprintf(source,sizeof source,"%s/%s",root,!strcmp(kind,"backup")?"spool":"share");
    int r=usb_copy(drive,!strcmp(kind,"backup")?"Backups":"Shared saves",source,file,shown,sizeof shown);
    char text[900];
    if(r<0) {message(sock,500,"Copy to USB failed; check the drive has space and is exFAT");return;}
    snprintf(text,sizeof text,r?"Copied to USB: %s":"Already on USB: %s",shown);message(sock,200,text);
}
static void usb_copy_all(int sock,const char *form) {
    char drive[1200];if(usb_drive(form,drive,sizeof drive)) {message(sock,400,"Plug in a USB drive (exFAT) and choose it");return;}
    char only[16]={0}; /* optional: one game's files */
    if(!parameter(form,"title",only,sizeof only)&&!pscloud_title_valid(only)) {message(sock,400,"Invalid game");return;}
    unsigned copied=0,same=0,failed=0;
    for(int k=0;k<2;k++) {
        char source[1400];snprintf(source,sizeof source,"%s/%s",root,k?"share":"spool");
        DIR *d=opendir(source);if(!d)continue;struct dirent *e;
        while((e=readdir(d))&&!stopped) {
            char file[128];size_t n=strlen(e->d_name);
            size_t cut=n>=6&&!strcmp(e->d_name+n-6,".ready")?6:n>=5&&!strcmp(e->d_name+n-5,".sent")?5:0;
            if(n-cut>=sizeof file)continue;
            memcpy(file,e->d_name,n-cut);file[n-cut]=0;
            if(!valid_archive(file))continue;
            if(*only&&strncmp(file+(strncmp(file,"portable-",9)?10:9),only,9))continue;
            char shown[600];int r=usb_copy(drive,k?"Shared saves":"Backups",source,file,shown,sizeof shown);
            if(r>0)copied++;else if(!r)same++;else failed++;
        }
        closedir(d);
    }
    char text[256];snprintf(text,sizeof text,"USB: %u copied, %u already there%s%s",copied,same,failed?", failed: ":"",failed?"check space and format":"");
    message(sock,failed?500:200,text);
}

/* ---------- Free up space ----------
 * Categories, all inside <root>:
 *   leftovers - everything in staging/ and in old *-stage-* folders except before-N.img
 *               (imported copies, empty mount points, unfinished exports)
 *   safety    - before-N.img (originals kept by replaces) in old stage folders, and all of
 *               rollback/ (restore-*, whole-restore-*, pre-restore-*.img)
 *   shared    - share/ ZIPs other than the newest one per game
 *   history   - uploaded backups other than the newest per game (as "keep only the newest")
 * Nothing runs while another operation holds the lock or an interrupted restore/mount marker
 * exists, and a folder with a mounted subfolder is never touched. */
struct space {unsigned long long bytes[4];unsigned files[4];};
enum {SPACE_LEFTOVERS,SPACE_SAFETY,SPACE_SHARED,SPACE_HISTORY};
static int is_mounted_inside(int dir) {
    struct stat self;if(fstat(dir,&self))return 1;
    int scan=dup(dir);DIR *d=scan<0?NULL:fdopendir(scan);if(!d) {if(scan>=0)close(scan);return 1;}
    rewinddir(d);
    struct dirent *e;int mounted=0;
    while((e=readdir(d))&&!mounted) {if(e->d_name[0]=='.')continue;struct stat st;if(!fstatat(dir,e->d_name,&st,AT_SYMLINK_NOFOLLOW)&&S_ISDIR(st.st_mode)&&st.st_dev!=self.st_dev)mounted=1;}
    closedir(d);return mounted;
}
static int before_image(const char *name) {return !strncmp(name,"before-",7)&&strlen(name)>11&&!strcmp(name+strlen(name)-4,".img");}
/* One folder: count or remove its files, keeping or taking before-N.img. Removes the folder
 * itself when it ends up empty. Subfolders are only empty mount points (removed with rmdir). */
static void space_folder(int parent,const char *name,int apply,int take_safety,int take_leftovers,struct space *out) {
    int dir=openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);if(dir<0)return;
    if(is_mounted_inside(dir)) {close(dir);return;}
    int scan=dup(dir);DIR *d=scan<0?NULL:fdopendir(scan);if(!d) {if(scan>=0)close(scan);close(dir);return;}
    rewinddir(d);
    struct dirent *e;
    while((e=readdir(d))) {
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        struct stat st;if(fstatat(dir,e->d_name,&st,AT_SYMLINK_NOFOLLOW))continue;
        int safety=before_image(e->d_name)||(!strncmp(e->d_name,"pre-restore-",12)&&strstr(e->d_name,".img"));
        int kind=safety?SPACE_SAFETY:SPACE_LEFTOVERS;
        if(S_ISDIR(st.st_mode)) {if(apply&&take_leftovers)(void)unlinkat(dir,e->d_name,AT_REMOVEDIR);continue;}
        if(!S_ISREG(st.st_mode))continue;
        if(!strcmp(e->d_name,"info.txt"))kind=SPACE_SAFETY; /* label of a kept safety copy */
        out->bytes[kind]+=(unsigned long long)st.st_size;out->files[kind]++;
        if(apply&&((kind==SPACE_SAFETY&&take_safety)||(kind==SPACE_LEFTOVERS&&take_leftovers)))(void)unlinkat(dir,e->d_name,0);
    }
    closedir(d);fsync(dir);close(dir);
    if(apply)(void)unlinkat(parent,name,AT_REMOVEDIR); /* only succeeds when empty */
}
/* Newest file per game in share/ (by name's title, newest modification time). */
static void space_shared(int apply,struct space *out) {
    char path[1400];snprintf(path,sizeof path,"%s/share",root);int dir=pscloud_open_directory(path);if(dir<0)return;
    struct item {char name[128],title[10];time_t when;off_t size;} *items=calloc(512,sizeof *items);unsigned n=0;
    int scan=dup(dir);DIR *d=items&&scan>=0?fdopendir(scan):NULL;if(d)rewinddir(d);if(!d) {if(scan>=0)close(scan);free(items);close(dir);return;}
    struct dirent *e;
    while((e=readdir(d))&&n<512) {
        if(!valid_archive(e->d_name))continue;
        struct stat st;if(fstatat(dir,e->d_name,&st,AT_SYMLINK_NOFOLLOW)||!S_ISREG(st.st_mode))continue;
        snprintf(items[n].name,sizeof items[n].name,"%.127s",e->d_name);memcpy(items[n].title,e->d_name+(strncmp(e->d_name,"portable-",9)?10:9),9);items[n].title[9]=0;
        items[n].when=st.st_mtime;items[n].size=st.st_size;n++;
    }
    closedir(d);
    for(unsigned i=0;i<n;i++) {
        int newer=0;for(unsigned j=0;j<n&&!newer;j++)if(j!=i&&!strcmp(items[j].title,items[i].title)&&(items[j].when>items[i].when||(items[j].when==items[i].when&&strcmp(items[j].name,items[i].name)>0)))newer=1;
        if(!newer)continue;
        out->bytes[SPACE_SHARED]+=(unsigned long long)items[i].size;out->files[SPACE_SHARED]++;
        if(apply)(void)unlinkat(dir,items[i].name,0);
    }
    if(apply)fsync(dir);
    free(items);close(dir);
}
/* Uploaded history beyond the newest per game: same rule as "keep only the newest". */
static void space_history(struct space *out) {
    char path[1400];snprintf(path,sizeof path,"%s/spool",root);int dir=pscloud_open_directory(path);if(dir<0)return;
    struct item {char title[10],user[17];long long created;off_t size;} *items=calloc(512,sizeof *items);unsigned n=0;
    int scan=dup(dir);DIR *d=items&&scan>=0?fdopendir(scan):NULL;if(d)rewinddir(d);if(!d) {if(scan>=0)close(scan);free(items);close(dir);return;}
    struct dirent *e;
    while((e=readdir(d))&&n<512) {
        size_t len=strlen(e->d_name);if(len<6||len-5>=128||strcmp(e->d_name+len-5,".sent"))continue;
        char file[128],identity[144];memcpy(file,e->d_name,len-5);file[len-5]=0;if(!valid_archive(file))continue;
        struct pscloud_snapshot snap;snprintf(identity,sizeof identity,"%s.identity",file);if(pscloud_snapshot_read(dir,identity,&snap))continue;
        struct stat st;if(fstatat(dir,e->d_name,&st,AT_SYMLINK_NOFOLLOW))continue;
        snprintf(items[n].title,sizeof items[n].title,"%s",snap.title);snprintf(items[n].user,sizeof items[n].user,"%s",snap.user);items[n].created=snap.created;items[n].size=st.st_size;n++;
    }
    closedir(d);
    for(unsigned i=0;i<n;i++) {int newer=0;for(unsigned j=0;j<n&&!newer;j++)if(j!=i&&!strcmp(items[j].title,items[i].title)&&!strcmp(items[j].user,items[i].user)&&items[j].created>items[i].created)newer=1;
        if(newer) {out->bytes[SPACE_HISTORY]+=(unsigned long long)items[i].size;out->files[SPACE_HISTORY]++;}}
    free(items);close(dir);
}
static void space_scan(int apply,const int take[4],struct space *out) {
    memset(out,0,sizeof *out);
    int parent=pscloud_open_directory(root);if(parent<0)return;
    int scan=dup(parent);DIR *d=scan>=0?fdopendir(scan):NULL;if(d)rewinddir(d);
    if(d) {
        char names[128][96];unsigned n=0;struct dirent *e;
        while((e=readdir(d))&&n<128)if((strstr(e->d_name,"-stage-")||!strcmp(e->d_name,"staging"))&&strlen(e->d_name)<96)strcpy(names[n++],e->d_name);
        closedir(d);
        for(unsigned i=0;i<n;i++) {
            if(!strcmp(names[i],"staging")) { /* backup work folders: staging/<id>/... */
                int st=openat(parent,"staging",O_RDONLY|O_DIRECTORY|O_NOFOLLOW);if(st<0)continue;
                int sc=dup(st);DIR *sd=sc>=0?fdopendir(sc):NULL;if(sd)rewinddir(sd);
                if(sd) {char subs[128][96];unsigned m=0;struct dirent *x;while((x=readdir(sd))&&m<128)if(x->d_name[0]!='.'&&strlen(x->d_name)<96)strcpy(subs[m++],x->d_name);closedir(sd);
                    for(unsigned j=0;j<m;j++) {struct stat info;if(fstatat(st,subs[j],&info,AT_SYMLINK_NOFOLLOW))continue;
                        if(S_ISDIR(info.st_mode))space_folder(st,subs[j],apply,take[SPACE_SAFETY],take[SPACE_LEFTOVERS],out);
                        else if(S_ISREG(info.st_mode)) {out->bytes[SPACE_LEFTOVERS]+=(unsigned long long)info.st_size;out->files[SPACE_LEFTOVERS]++;if(apply&&take[SPACE_LEFTOVERS])(void)unlinkat(st,subs[j],0);}}}
                else if(sc>=0)close(sc);
                close(st);
            } else space_folder(parent,names[i],apply,take[SPACE_SAFETY],take[SPACE_LEFTOVERS],out);
        }
    } else if(scan>=0)close(scan);
    /* rollback/: every entry is a safety copy (files, or folders of before-N.img). */
    int rb=openat(parent,"rollback",O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    if(rb>=0) {
        int sc=dup(rb);DIR *rd=sc>=0?fdopendir(sc):NULL;if(rd)rewinddir(rd);
        if(rd) {char names[256][128];unsigned n=0;struct dirent *e;while((e=readdir(rd))&&n<256)if(e->d_name[0]!='.'&&strlen(e->d_name)<128)strcpy(names[n++],e->d_name);closedir(rd);
            for(unsigned i=0;i<n;i++) {struct stat st;if(fstatat(rb,names[i],&st,AT_SYMLINK_NOFOLLOW))continue;
                if(S_ISDIR(st.st_mode)) {struct space part={0};space_folder(rb,names[i],apply&&take[SPACE_SAFETY],1,1,&part);
                    out->bytes[SPACE_SAFETY]+=part.bytes[SPACE_SAFETY]+part.bytes[SPACE_LEFTOVERS];out->files[SPACE_SAFETY]+=part.files[SPACE_SAFETY]+part.files[SPACE_LEFTOVERS];}
                else if(S_ISREG(st.st_mode)) {out->bytes[SPACE_SAFETY]+=(unsigned long long)st.st_size;out->files[SPACE_SAFETY]++;if(apply&&take[SPACE_SAFETY])(void)unlinkat(rb,names[i],0);}}}
        else if(sc>=0)close(sc);
        fsync(rb);close(rb);
    }
    close(parent);
    space_shared(apply&&take[SPACE_SHARED],out);
    if(apply&&take[SPACE_HISTORY]) {int keep=local_keep_latest;local_keep_latest=1;prune_local_history();local_keep_latest=keep;}
    else space_history(out);
}
static int space_blocked(void) {int parent=pscloud_open_directory(root);if(parent<0)return 1;int active=pscloud_active_marker(parent);close(parent);return active!=0;}
static void space_json(int sock,const struct space *sp,const char *note) {
    char json[1024];snprintf(json,sizeof json,"{\"blocked\":%s,\"leftovers\":{\"bytes\":%llu,\"files\":%u},\"safety\":{\"bytes\":%llu,\"files\":%u},\"shared\":{\"bytes\":%llu,\"files\":%u},\"history\":{\"bytes\":%llu,\"files\":%u}%s%s%s}",
        space_blocked()?"true":"false",sp->bytes[0],sp->files[0],sp->bytes[1],sp->files[1],sp->bytes[2],sp->files[2],sp->bytes[3],sp->files[3],note?",\"message\":\"":"",note?note:"",note?"\"":"");
    respond(sock,200,"application/json",json,strlen(json));
}
static void cleanup_preview(int sock) {static const int none[4]={0};struct space sp;space_scan(0,none,&sp);space_json(sock,&sp,NULL);}
static void cleanup_apply(int sock,const char *form) {
    if(space_blocked()) {message(sock,409,"An interrupted restore needs attention first; nothing was removed. See Activity");return;}
    static const char *const keys[4]={"leftovers","safety","shared","history"};int take[4]={0};unsigned chosen=0;
    for(int i=0;i<4;i++) {char v[8];if(!parameter(form,keys[i],v,sizeof v)&&!strcmp(v,"1")) {take[i]=1;chosen++;}}
    if(!chosen) {message(sock,400,"Choose what to remove");return;}
    char lockpath[1400];snprintf(lockpath,sizeof lockpath,"%s/.mount.lock",root);
    int lock=open(lockpath,O_CREAT|O_RDWR|O_NOFOLLOW,0600);
    if(lock<0||flock(lock,LOCK_EX|LOCK_NB)) {if(lock>=0)close(lock);message(sock,409,"Another save operation is running; try again when it finishes");return;}
    struct space before,after;static const int none[4]={0};space_scan(0,none,&before);space_scan(1,take,&after);space_scan(0,none,&after);
    unsigned long long freed=0;for(int i=0;i<4;i++)if(before.bytes[i]>after.bytes[i])freed+=before.bytes[i]-after.bytes[i];
    pscloud_log("EVENT","Freed %.1f MB (%s%s%s%s)",freed/1048576.0,take[0]?"leftovers ":"",take[1]?"safety copies ":"",take[2]?"older shared saves ":"",take[3]?"uploaded history":"");
    close(lock);
    char note[160];snprintf(note,sizeof note,"Freed %.1f MB",freed/1048576.0);space_json(sock,&after,note);
}
static void space_totals(unsigned long long *leftover_bytes,unsigned *leftover_files,unsigned long long *safety_bytes,unsigned *safety_files) {
    static const int none[4]={0};struct space sp;space_scan(0,none,&sp);
    *leftover_bytes=sp.bytes[SPACE_LEFTOVERS];*leftover_files=sp.files[SPACE_LEFTOVERS];*safety_bytes=sp.bytes[SPACE_SAFETY];*safety_files=sp.files[SPACE_SAFETY];
}
/* Keep share/ small automatically: the 2 newest files per game. */
static void prune_shared_keep(unsigned keep) {
    char path[1400];snprintf(path,sizeof path,"%s/share",root);int dir=pscloud_open_directory(path);if(dir<0)return;
    struct item {char name[128],title[10];time_t when;} *items=calloc(512,sizeof *items);unsigned n=0;
    int scan=dup(dir);DIR *d=items&&scan>=0?fdopendir(scan):NULL;if(d)rewinddir(d);if(!d) {if(scan>=0)close(scan);free(items);close(dir);return;}
    struct dirent *e;
    while((e=readdir(d))&&n<512) {if(!valid_archive(e->d_name))continue;struct stat st;if(fstatat(dir,e->d_name,&st,AT_SYMLINK_NOFOLLOW)||!S_ISREG(st.st_mode))continue;
        snprintf(items[n].name,sizeof items[n].name,"%.127s",e->d_name);memcpy(items[n].title,e->d_name+(strncmp(e->d_name,"portable-",9)?10:9),9);items[n].title[9]=0;items[n].when=st.st_mtime;n++;}
    closedir(d);
    for(unsigned i=0;i<n;i++) {unsigned newer=0;for(unsigned j=0;j<n;j++)if(j!=i&&!strcmp(items[j].title,items[i].title)&&(items[j].when>items[i].when||(items[j].when==items[i].when&&strcmp(items[j].name,items[i].name)>0)))newer++;
        if(newer>=keep&&!unlinkat(dir,items[i].name,0))pscloud_log("INFO","Removed older shared save %s (keeping the newest %u for this game)",items[i].name,keep);}
    fsync(dir);free(items);close(dir);
}

/* ---------- Sync between consoles ----------
 * Cloud folders are per console profile (<game>/User-<id>/WholeGame). Each console publishes
 * its profiles to <cloud>/.pscloud-profiles/<id>.txt; links.conf says which profiles on other
 * consoles are "the same person" as a local profile. A game's sync state compares this PS5's
 * newest save time with the newest cloud version (own or linked) and the last sync point. */
static char console_id[17];
static void console_id_load(void) {
    char path[1400];snprintf(path,sizeof path,"%s/console.id",root);int fd=open(path,O_RDONLY|O_NOFOLLOW);
    if(fd>=0) {ssize_t n=read(fd,console_id,16);close(fd);console_id[n>0?n:0]=0;if(safe_word(console_id,16)&&strlen(console_id)==16)return;}
    char id[33];if(pscloud_random_id(id))return;memcpy(console_id,id,16);console_id[16]=0;
    char text[32];snprintf(text,sizeof text,"%s\n",console_id);atomic_config(path,text);console_id[16]=0;
}
static int profile_name(const char *user,char *name,size_t max) {
    char path[1400];snprintf(path,sizeof path,"%s/%s/username.dat",home,user);name[0]=0;
    int fd=open(path,O_RDONLY|O_NOFOLLOW);if(fd<0)return -1;ssize_t n=read(fd,name,max-1);close(fd);if(n<=0)return -1;name[n]=0;
    for(ssize_t i=0;i<n;i++)if((unsigned char)name[i]<32) {name[i]=0;break;}
    return name[0]?0:-1;
}
static void registry_publish(void) {
    struct settings cloud={0};if(pscloud_configure(cloudpath,&cloud)||!*console_id)return;
    webdav_quiet=1;char url[4200],text[512];
    char sink[512];struct response ignored={sink,0,sizeof sink-1};
    snprintf(url,sizeof url,"%s/.pscloud-profiles/",cloud.url);webdav(&cloud,url,"MKCOL",&ignored);
    DIR *d=opendir(home);struct dirent *u;
    while(d&&(u=readdir(d))) {
        if(!safe_word(u->d_name,16))continue;
        char name[64];if(profile_name(u->d_name,name,sizeof name))snprintf(name,sizeof name,"Profile %.16s",u->d_name);
        for(char *q=name;*q;q++)if(*q=='"'||*q=='\\'||(unsigned char)*q<32)*q=' ';
        int n=snprintf(text,sizeof text,"NAME=%s\nCONSOLE=%s\nFIRMWARE=%x.%02x\nUPDATED=%lld\n",name,console_id,firmware_id>>24,(firmware_id>>16)&255,(long long)time(NULL));
        snprintf(url,sizeof url,"%s/.pscloud-profiles/%s.txt",cloud.url,u->d_name);webdav_put(&cloud,url,text,(size_t)n);
    }
    if(d)closedir(d);
    webdav_quiet=0;memset(cloud.password,0,sizeof cloud.password);
}
/* links.conf: "<local profile> <cloud profile>[,<cloud profile>...]" */
static int links_for(const char *user,char linked[4][17]) {
    char path[1400],line[256];snprintf(path,sizeof path,"%s/links.conf",root);FILE *f=fopen(path,"r");int n=0;if(!f)return 0;
    while(fgets(line,sizeof line,f)) {char who[32],list[200];if(sscanf(line,"%31s %199s",who,list)!=2||strcmp(who,user))continue;
        for(char *save=NULL,*tok=strtok_r(list,",",&save);tok&&n<4;tok=strtok_r(NULL,",",&save))if(safe_word(tok,16)&&strcmp(tok,user))snprintf(linked[n++],17,"%s",tok);}
    fclose(f);return n;
}
static int links_save(const char *user,const char *list) {
    char path[1400],line[256],out[8192]={0};size_t n=0;snprintf(path,sizeof path,"%s/links.conf",root);FILE *f=fopen(path,"r");
    while(f&&fgets(line,sizeof line,f)) {char who[32];if(sscanf(line,"%31s",who)==1&&strcmp(who,user)&&n+strlen(line)<sizeof out-1) {strcpy(out+n,line);n+=strlen(line);}}
    if(f)fclose(f);
    if(*list)n+=(size_t)snprintf(out+n,sizeof out-n,"%s %s\n",user,list);
    return atomic_config(path,out);
}
/* HTTP date ("Sat, 11 Oct 2026 07:31:25 GMT") to Unix time. */
static long long http_time(const char *text) {
    static const char months[]="JanFebMarAprMayJunJulAugSepOctNovDec";char month[4];int day,year,h,m,sec;
    if(sscanf(text,"%*3s, %d %3s %d %d:%d:%d",&day,month,&year,&h,&m,&sec)!=6)return 0;
    const char *at=strstr(months,month);if(!at)return 0;int mon=(int)(at-months)/3;
    long long y=year-(mon<2),era=y/400,yoe=y-era*400,mp=(mon+9)%12,doy=(153*mp+2)/5+day-1,doe=yoe*365+yoe/4-yoe/100+doy;
    return (era*146097+doe-719468)*86400LL+h*3600+m*60+sec;
}
struct cloud_version {char file[128],user[17],sha[65],peer[64],current[65];long long created,live;};
/* ---------- Other PS5s on the local network ----------
 * peers.conf lists "ip:port" of other consoles running PSCloud. Each console serves its
 * newest backup per profile and game read-only under /api/peer/, so sync works without a
 * cloud. Discovery: a UDP "who is there" on port 8083 that every PSCloud answers. */
static unsigned peer_udp_port=8083,http_port_number=8082;
static int peer_address_valid(const char *a) {
    unsigned b[4],port;char tail;if(sscanf(a,"%u.%u.%u.%u:%u%c",&b[0],&b[1],&b[2],&b[3],&port,&tail)!=5)return 0;
    for(int i=0;i<4;i++)if(b[i]>255)return 0;
    return port>0&&port<65536&&strlen(a)<64;
}
static int peers_list(char peers[8][64]) {
    char path[1400],line[128];snprintf(path,sizeof path,"%s/peers.conf",root);FILE *f=fopen(path,"r");int n=0;if(!f)return 0;
    while(fgets(line,sizeof line,f)&&n<8) {line[strcspn(line,"\r\n")]=0;if(peer_address_valid(line))snprintf(peers[n++],64,"%.63s",line);}
    fclose(f);return n;
}
struct peer_body {char *data;size_t size,max;};
static size_t peer_collect(char *p,size_t a,size_t b,void *ctx) {
    struct peer_body *r=ctx;size_t n=a*b;if(n>r->max-r->size)return 0;memcpy(r->data+r->size,p,n);r->size+=n;return n;
}
static long peer_get(const char *peer,const char *path,char *out,size_t max,size_t *got,long timeout) {
    CURL *c=curl_easy_init();if(!c)return 0;char url[700];snprintf(url,sizeof url,"http://%s%s",peer,path);
    struct peer_body r={out,0,max};
    curl_easy_setopt(c,CURLOPT_URL,url);curl_easy_setopt(c,CURLOPT_PROTOCOLS_STR,"http");curl_easy_setopt(c,CURLOPT_FOLLOWLOCATION,0L);
    curl_easy_setopt(c,CURLOPT_USERAGENT,"PSCloud/" PSCLOUD_VERSION);curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,4L);curl_easy_setopt(c,CURLOPT_TIMEOUT,timeout);curl_easy_setopt(c,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,peer_collect);curl_easy_setopt(c,CURLOPT_WRITEDATA,&r);
    CURLcode result=curl_easy_perform(c);long status=0;curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);curl_easy_cleanup(c);
    *got=r.size;if(r.size<max)out[r.size]=0;return result==CURLE_OK?status:0;
}
static int json_text(const char *json,const char *key,char *out,size_t max) {
    char pattern[64];snprintf(pattern,sizeof pattern,"\"%s\":\"",key);const char *at=strstr(json,pattern);out[0]=0;if(!at)return -1;
    at+=strlen(pattern);size_t n=0;while(*at&&*at!='"'&&n+1<max) {if(*at=='\\'&&at[1])at++;out[n++]=*at++;}out[n]=0;return 0;
}
static long long json_number(const char *json,const char *key) {
    char pattern[64];snprintf(pattern,sizeof pattern,"\"%s\":",key);const char *at=strstr(json,pattern);return at?strtoll(at+strlen(pattern),NULL,10):0;
}
/* Newest portable whole-game backup on this PS5 for a profile and game. */
static int local_latest(const char *user,const char *title,struct cloud_version *out) {
    char path[1400];snprintf(path,sizeof path,"%s/spool",root);int dir=pscloud_open_directory(path);if(dir<0)return 0;
    int scan=dup(dir);DIR *d=scan>=0?fdopendir(scan):NULL;int found=0;memset(out,0,sizeof *out);
    if(d) {rewinddir(d);struct dirent *e;
        while((e=readdir(d))) {
            size_t n=strlen(e->d_name);if(n<10||n>=200||strcmp(e->d_name+n-9,".identity")||strncmp(e->d_name,"portable-",9))continue;
            char file[128];if(n-9>=sizeof file)continue;memcpy(file,e->d_name,n-9);file[n-9]=0;if(!valid_archive(file))continue;
            struct pscloud_snapshot snap;if(pscloud_snapshot_read(dir,e->d_name,&snap)||strcmp(snap.user,user)||strcmp(snap.title,title)||strcmp(snap.slot,"WholeGame"))continue;
            char ready[150],sent[150];struct stat st;snprintf(ready,sizeof ready,"%s.ready",file);snprintf(sent,sizeof sent,"%s.sent",file);
            if(fstatat(dir,ready,&st,AT_SYMLINK_NOFOLLOW)&&fstatat(dir,sent,&st,AT_SYMLINK_NOFOLLOW)&&fstatat(dir,file,&st,AT_SYMLINK_NOFOLLOW))continue;
            if(snap.created>out->created) {snprintf(out->file,sizeof out->file,"%s",file);snprintf(out->sha,sizeof out->sha,"%s",snap.sha256);snprintf(out->user,sizeof out->user,"%s",user);out->created=snap.created;found=1;}
        }
        closedir(d);
    } else if(scan>=0)close(scan);
    close(dir);return found;
}
static long long live_time(const char *user,const char *title) {
    char path[1400];snprintf(path,sizeof path,"%s/%s/savedata_prospero/%s",home,user,title);int fd=pscloud_open_directory(path);
    long long t=fd>=0?(long long)newest_container(fd):0;if(fd>=0)close(fd);return t;
}
static void local_current(const char *user,const char *title,const struct cloud_version *latest,long long live,char current[65]) {
    current[0]=0;long long received=sync_base(user,title);char sha[65];sync_received(user,title,sha);
    /* Not played since receiving a save: still that save, even if backed up again since
     * (a backup of the same data made here is arranged differently). */
    if(received&&*sha&&live<=received+60) {snprintf(current,65,"%s",sha);return;}
    if(latest&&*latest->file&&live<=latest->created+60)snprintf(current,65,"%s",latest->sha);
}

/* ---------- Comparing the actual saves, slot by slot ----------
 * When the game last wrote each save slot (the container's modification time) plus the
 * PS5's own label for it. After a replace, each slot is given the time it was really saved
 * on the other PS5, so times stay comparable across consoles. */
struct live_slot {char slot[64],label[128],sub[128];long long mtime;};
static char system_root[1024]; /* "" on the PS5; tests point it at a folder */
static void save_db_path(const char *user,char *out,size_t max) {snprintf(out,max,"%s/system_data/savedata_prospero/%s/db/user/savedata.db",system_root,user);}
/* Slots that PSCloud filled from the other PS5: "slot source_time db_time_then". While the
 * game has not saved the slot again (same database time), it counts as the source's time. */
static long long received_slot_time(const char *user,const char *title,const char *slot,long long now_saved) {
    char path[1400],line[256];snprintf(path,sizeof path,"%s/slot-times/%s-%s",root,user,title);FILE *f=fopen(path,"r");long long result=0;if(!f)return 0;
    while(fgets(line,sizeof line,f)) {char name[80];long long source,then;if(sscanf(line,"%79s %lld %lld",name,&source,&then)==3&&!strcmp(name,slot)&&then==now_saved)result=source;}
    fclose(f);return result;
}
static int live_slots(const char *user,const char *title,struct live_slot *out,int max) {
    char path[1400];snprintf(path,sizeof path,"%s/%s/savedata_prospero/%s",home,user,title);DIR *d=opendir(path);int n=0;if(!d)return 0;
    char db[1400];save_db_path(user,db,sizeof db);struct pscloud_db_slot saved[128];int ns=pscloud_save_db_times(db,title,saved,128);
    struct dirent *e;
    while((e=readdir(d))&&n<max) {
        if(strncmp(e->d_name,"sdimg_",6)||!strncmp(e->d_name,"sdimg_sce_bu_",13)||!safe_word(e->d_name+6,63))continue;
        char file[1700];struct stat st;snprintf(file,sizeof file,"%s/%s",path,e->d_name);if(lstat(file,&st)||!S_ISREG(st.st_mode))continue;
        memset(&out[n],0,sizeof out[n]);snprintf(out[n].slot,sizeof out[n].slot,"%s",e->d_name+6);
        long long when=(long long)st.st_mtime; /* fallback when the save database cannot be read */
        for(int k=0;k<ns;k++)if(!strcmp(saved[k].slot,out[n].slot))when=saved[k].saved;
        long long source=received_slot_time(user,title,out[n].slot,when);out[n].mtime=source?source:when;
        char detail[256];pscloud_save_slot_info(home,user,title,out[n].slot,out[n].label,sizeof out[n].label,out[n].sub,sizeof out[n].sub,detail,sizeof detail);n++;
    }
    closedir(d);return n;
}
static void live_slots_json(char *json,size_t cap,const struct live_slot *v,int n) {
    size_t pos=(size_t)snprintf(json,cap,"[");
    for(int i=0;i<n&&pos<cap-1200;i++) {char a[800],b[800];escaped(a,sizeof a,v[i].label);escaped(b,sizeof b,v[i].sub);
        pos+=(size_t)snprintf(json+pos,cap-pos,"%s{\"slot\":\"%s\",\"mtime\":%lld,\"label\":\"%s\",\"sub\":\"%s\"}",i?",":"",v[i].slot,v[i].mtime,a,b);}
    snprintf(json+pos,cap-pos,"]");
}
static void peer_slots_json(int sock,const char *query) {
    char user[32],title[16];if(parameter(query,"user",user,sizeof user)||!safe_word(user,16)||parameter(query,"title",title,sizeof title)||!pscloud_title_valid(title)) {reject(sock,400,"Invalid request");return;}
    struct live_slot v[64];int n=live_slots(user,title,v,64);char *json=malloc(200000);if(!json) {reject(sock,500,"Out of memory");return;}
    memcpy(json,"{\"slots\":",9);live_slots_json(json+9,200000-16,v,n);strcat(json,"}");respond(sock,200,"application/json",json,strlen(json));free(json);
}
/* Parse the other console's list; -1 not reachable, else the count. */
static int peer_slots(const char *peer,const char *user,const char *title,struct live_slot *out,int max) {
    char path[200];snprintf(path,sizeof path,"/api/peer/slots?user=%s&title=%s",user,title);char *body=malloc(200001);size_t got=0;if(!body)return -1;
    long status=peer_get(peer,path,body,200000,&got,8);if(!status) {free(body);return -1;}
    if(status!=200) {free(body);return 0;}
    int n=0;
    for(char *at=body;n<max&&(at=strstr(at,"{\"slot\":\""));at++) {
        memset(&out[n],0,sizeof out[n]);json_text(at,"slot",out[n].slot,sizeof out[n].slot);if(!safe_word(out[n].slot,63))continue;
        out[n].mtime=json_number(at,"mtime");char *next=strstr(at+1,"{\"slot\":\"");char saved=0;if(next) {saved=*next;*next=0;}
        json_text(at,"label",out[n].label,sizeof out[n].label);json_text(at,"sub",out[n].sub,sizeof out[n].sub);
        if(next)*next=saved;
        n++;
    }
    free(body);return n;
}
struct slot_compare {int here_newer,there_newer,only_here,only_there,same;long long here_max,there_max;};
static void compare_slots(const struct live_slot *here,int nh,const struct live_slot *there,int nt,struct slot_compare *c) {
    memset(c,0,sizeof *c);
    for(int i=0;i<nh;i++) {if(here[i].mtime>c->here_max)c->here_max=here[i].mtime;int k=-1;for(int j=0;j<nt;j++)if(!strcmp(here[i].slot,there[j].slot))k=j;
        if(k<0) {c->only_here++;continue;}
        long long d=here[i].mtime-there[k].mtime;if(d>2)c->here_newer++;else if(d<-2)c->there_newer++;else c->same++;}
    for(int j=0;j<nt;j++) {if(there[j].mtime>c->there_max)c->there_max=there[j].mtime;int k=-1;for(int i=0;i<nh;i++)if(!strcmp(here[i].slot,there[j].slot))k=i;if(k<0)c->only_there++;}
}
/* After a replace: remember, per slot, the time the game saved it on the other PS5. */
static void apply_slot_times(const char *user,const char *title,const char *list) {
    char db[1400];save_db_path(user,db,sizeof db);struct pscloud_db_slot saved[128];int ns=pscloud_save_db_times(db,title,saved,128);
    char dir[1400],path[1500];snprintf(dir,sizeof dir,"%s/slot-times",root);(void)mkdir(dir,0700);snprintf(path,sizeof path,"%s/%s-%s",dir,user,title);
    char out[8192]="",copy[4096];size_t n=0;snprintf(copy,sizeof copy,"%.4095s",list);
    for(char *save=NULL,*tok=strtok_r(copy,";",&save);tok;tok=strtok_r(NULL,";",&save)) {
        char *colon=strrchr(tok,':');if(!colon)continue;*colon=0;long long when=strtoll(colon+1,NULL,10);if(!safe_word(tok,63)||when<=0)continue;
        long long now=0;for(int k=0;k<ns;k++)if(!strcmp(saved[k].slot,tok))now=saved[k].saved;
        if(!now) {char file[1600];struct stat st;snprintf(file,sizeof file,"%s/%s/savedata_prospero/%s/sdimg_%s",home,user,title,tok);if(!lstat(file,&st))now=(long long)st.st_mtime;}
        n+=(size_t)snprintf(out+n,sizeof out-n,"%s %lld %lld\n",tok,when,now);if(n>=sizeof out-100)break;
    }
    atomic_config(path,out);
}
static int peer_latest(const char *peer,const char *user,const char *title,struct cloud_version *out) {
    char path[200],body[1024];size_t got=0;snprintf(path,sizeof path,"/api/peer/latest?user=%s&title=%s",user,title);
    long status=peer_get(peer,path,body,sizeof body-1,&got,8);
    if(!status)return -1; /* not reachable */
    if(status!=200)return 0;
    memset(out,0,sizeof *out);json_text(body,"file",out->file,sizeof out->file);json_text(body,"sha256",out->sha,sizeof out->sha);out->created=json_number(body,"created");
    json_text(body,"current",out->current,sizeof out->current);out->live=json_number(body,"live");
    if(!valid_archive(out->file)||strlen(out->sha)!=64)return 0;
    snprintf(out->user,sizeof out->user,"%.16s",user);snprintf(out->peer,sizeof out->peer,"%.63s",peer);return 1;
}
static void peer_info_json(int sock) {
    char json[8192];int pos=snprintf(json,sizeof json,"{\"console\":\"%s\",\"firmware\":\"%x.%02x\",\"version\":\"%s\",\"profiles\":[",console_id,firmware_id>>24,(firmware_id>>16)&255,PSCLOUD_VERSION);
    DIR *d=opendir(home);struct dirent *u;unsigned n=0;
    while(d&&(u=readdir(d))&&n<16) {if(!safe_word(u->d_name,16))continue;char name[64],shown[400];if(profile_name(u->d_name,name,sizeof name))snprintf(name,sizeof name,"Profile %.16s",u->d_name);escaped(shown,sizeof shown,name);
        pos+=snprintf(json+pos,sizeof json-(size_t)pos,"%s{\"user\":\"%s\",\"name\":\"%s\"}",n++?",":"",u->d_name,shown);}
    if(d)closedir(d);
    pos+=snprintf(json+pos,sizeof json-(size_t)pos,"],\"sync\":[");unsigned synced=0;
    pthread_mutex_lock(&settings_mutex);
    for(unsigned i=0;i<game_settings_count&&pos<(int)sizeof json-64;i++)if(game_settings[i].auto_sync)pos+=snprintf(json+pos,sizeof json-(size_t)pos,"%s\"%s\"",synced++?",":"",game_settings[i].title);
    pthread_mutex_unlock(&settings_mutex);
    snprintf(json+pos,sizeof json-(size_t)pos,"]}");respond(sock,200,"application/json",json,strlen(json));
}
/* Games synced by a PS5 on the network count as synced here too ("turn it on once"). */
static char peer_synced[256][10];static unsigned peer_synced_count;static pthread_mutex_t peer_synced_mutex=PTHREAD_MUTEX_INITIALIZER;
static void peer_synced_refresh(void) {
    char peers[8][64],titles[256][10];int n=peers_list(peers);unsigned count=0;
    for(int i=0;i<n;i++) {
        char body[8192];size_t got=0;if(peer_get(peers[i],"/api/peer/info",body,sizeof body-1,&got,5)!=200)continue;
        char *list=strstr(body,"\"sync\":[");if(!list)continue;
        for(char *q=list+8;*q&&*q!=']'&&count<256;q++)if(*q=='"'&&!strncmp(q+1,"PPSA",4)) {
            char t[10];memcpy(t,q+1,9);t[9]=0;if(!pscloud_title_valid(t))continue;
            int dup=0;for(unsigned k=0;k<count;k++)if(!strcmp(titles[k],t))dup=1;
            if(!dup)strcpy(titles[count++],t);
            q+=10;
        }
    }
    pthread_mutex_lock(&peer_synced_mutex);memcpy(peer_synced,titles,sizeof titles[0]*count);peer_synced_count=count;pthread_mutex_unlock(&peer_synced_mutex);
}
static int game_synced(const char *title) {
    if(game_setting(title,2))return 1;
    int on=0;pthread_mutex_lock(&peer_synced_mutex);for(unsigned i=0;i<peer_synced_count;i++)if(!strcmp(peer_synced[i],title))on=1;pthread_mutex_unlock(&peer_synced_mutex);
    return on;
}
static int any_synced(void) {
    int any=peer_synced_count>0;pthread_mutex_lock(&settings_mutex);for(unsigned i=0;i<game_settings_count;i++)if(game_settings[i].auto_sync)any=1;pthread_mutex_unlock(&settings_mutex);
    return any;
}
static void peer_latest_json(int sock,const char *query) {
    char user[32],title[16];struct cloud_version v;
    if(parameter(query,"user",user,sizeof user)||!safe_word(user,16)||parameter(query,"title",title,sizeof title)||!pscloud_title_valid(title)) {reject(sock,400,"Invalid request");return;}
    if(!local_latest(user,title,&v)) {reject(sock,404,"No backup");return;}
    long long live=live_time(user,title);char current[65];local_current(user,title,&v,live,current);
    char json[600];snprintf(json,sizeof json,"{\"file\":\"%s\",\"sha256\":\"%s\",\"created\":%lld,\"current\":\"%s\",\"live\":%lld}",v.file,v.sha,v.created,current,live);respond(sock,200,"application/json",json,strlen(json));
}
static void peer_file(int sock,const char *query) {
    char file[128],path[1600];if(parameter(query,"file",file,sizeof file)||!valid_archive(file)||strncmp(file,"portable-",9)) {reject(sock,400,"Invalid file");return;}
    static const char *const suffix[]={"",".ready",".sent"};int fd=-1;
    for(int i=0;i<3&&fd<0;i++) {snprintf(path,sizeof path,"%s/spool/%s%s",root,file,suffix[i]);fd=open(path,O_RDONLY|O_NOFOLLOW);}
    struct stat st;if(fd<0||fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<=0||st.st_size>PSCLOUD_RESTORE_MAX) {if(fd>=0)close(fd);reject(sock,404,"No such backup");return;}
    char header[256];int n=snprintf(header,sizeof header,"HTTP/1.1 200 OK\r\nContent-Type: application/zip\r\nContent-Length: %lld\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n",(long long)st.st_size);
    send_all(sock,header,(size_t)n);char *buffer=malloc(1U<<20);long long left=st.st_size;
    while(buffer&&left>0) {ssize_t got=read(fd,buffer,left>(1<<20)?(1<<20):(size_t)left);if(got<=0)break;send_all(sock,buffer,(size_t)got);left-=got;}
    free(buffer);close(fd);
}
/* UDP: answer "PSCLOUD-DISCOVER" with "PSCLOUD-HERE <console> <http port> <firmware>". */
static void *peer_beacon(void *unused) {
    (void)unused;int fd=socket(AF_INET,SOCK_DGRAM,0);if(fd<0)return NULL;
    int yes=1;setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof yes);
    struct sockaddr_in a={0};a.sin_family=AF_INET;a.sin_port=htons((uint16_t)peer_udp_port);a.sin_addr.s_addr=htonl(INADDR_ANY);
    if(bind(fd,(struct sockaddr *)&a,sizeof a)) {close(fd);return NULL;}
    struct timeval tv={1,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv);
    while(!stopped) {
        char buffer[64];struct sockaddr_in from;socklen_t len=sizeof from;ssize_t n=recvfrom(fd,buffer,sizeof buffer-1,0,(struct sockaddr *)&from,&len);
        if(n<=0)continue;
        buffer[n]=0;if(strcmp(buffer,"PSCLOUD-DISCOVER"))continue;
        char reply[128];int m=snprintf(reply,sizeof reply,"PSCLOUD-HERE %s %u %x.%02x",console_id,http_port_number,firmware_id>>24,(firmware_id>>16)&255);
        sendto(fd,reply,(size_t)m,0,(struct sockaddr *)&from,len);
    }
    close(fd);return NULL;
}
static void peers_discover_json(int sock) {
    int fd=socket(AF_INET,SOCK_DGRAM,0);if(fd<0) {reject(sock,500,"Network unavailable");return;}
    int yes=1;setsockopt(fd,SOL_SOCKET,SO_BROADCAST,&yes,sizeof yes);struct timeval tv={0,300000};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv);
    struct sockaddr_in to={0};to.sin_family=AF_INET;to.sin_port=htons((uint16_t)peer_udp_port);to.sin_addr.s_addr=htonl(INADDR_BROADCAST);
#ifdef PSCLOUD_HOST_TEST
    if(getenv("PSCLOUD_TEST_DISCOVER_PORT")) {to.sin_port=htons((uint16_t)atoi(getenv("PSCLOUD_TEST_DISCOVER_PORT")));to.sin_addr.s_addr=htonl(INADDR_LOOPBACK);}
#endif
    static const char ask[]="PSCLOUD-DISCOVER";
    char json[4096];int pos=snprintf(json,sizeof json,"{\"found\":[");unsigned n=0;
    for(int round=0;round<3;round++) {
        sendto(fd,ask,sizeof ask-1,0,(struct sockaddr *)&to,sizeof to);
        for(int wait=0;wait<3;wait++) {
            char reply[128];struct sockaddr_in from;socklen_t len=sizeof from;ssize_t got=recvfrom(fd,reply,sizeof reply-1,0,(struct sockaddr *)&from,&len);
            if(got<=0)continue;
            reply[got]=0;char id[32],fw[16];unsigned port;
            if(sscanf(reply,"PSCLOUD-HERE %31s %u %15s",id,&port,fw)!=3||!safe_word(id,16)||!strcmp(id,console_id)||port==0||port>65535)continue;
            for(char *q=fw;*q;q++)if(!strchr("0123456789.",*q))*q=0;
            char ip[INET_ADDRSTRLEN];if(!inet_ntop(AF_INET,&from.sin_addr,ip,sizeof ip))continue;
            char address[64];snprintf(address,sizeof address,"%s:%u",ip,port);if(strstr(json,address))continue;
            pos+=snprintf(json+pos,sizeof json-(size_t)pos,"%s{\"address\":\"%s\",\"console\":\"%s\",\"firmware\":\"%s\"}",n++?",":"",address,id,fw);
            if(pos>(int)sizeof json-200)break;
        }
    }
    close(fd);snprintf(json+pos,sizeof json-(size_t)pos,"]}");respond(sock,200,"application/json",json,strlen(json));
}
/* Configured peers with what they report. */
static void peers_json(int sock) {
    char peers[8][64];int n=peers_list(peers);char *json=malloc(65536);if(!json) {reject(sock,500,"Out of memory");return;}
    int pos=snprintf(json,65536,"{\"peers\":[");
    for(int i=0;i<n;i++) {
        char body[8192];size_t got=0;long status=peer_get(peers[i],"/api/peer/info",body,sizeof body-1,&got,5);
        const char *start=status==200?strchr(body,'{'):NULL;
        pos+=snprintf(json+pos,65536-(size_t)pos,"%s{\"address\":\"%s\",\"reachable\":%s,\"info\":%s}",i?",":"",peers[i],start?"true":"false",start?start:"null");
    }
    snprintf(json+pos,65536-(size_t)pos,"]}");respond(sock,200,"application/json",json,strlen(json));free(json);
}

/* Newest WholeGame version in one profile's cloud folder: PROPFIND for the newest identity
 * by modification date, then read that identity. 1 found, 0 none, -1 error. */
static int cloud_newest(const struct settings *cloud,const char *user,const char *title,struct cloud_version *out) {
    struct pscloud_snapshot who={0};snprintf(who.user,sizeof who.user,"%s",user);snprintf(who.title,sizeof who.title,"%s",title);strcpy(who.slot,"WholeGame");memset(who.sha256,'0',64);
    char folder[256],url[4200];if(pscloud_snapshot_folder(&who,folder,sizeof folder))return -1;
    snprintf(url,sizeof url,"%s/%s/.pscloud/",cloud->url,folder);
    char *xml=malloc(512*1024+1);if(!xml)return -1;struct response r={xml,0,512*1024};long status=webdav(cloud,url,"PROPFIND",&r);
    if(status==404) {free(xml);return 0;}
    if(status!=207&&status!=200) {free(xml);return -1;}
    xml[r.size]=0;char best[128]={0};long long best_time=0;
    for(char *cursor=xml;(cursor=strstr(cursor,"href>"));) {
        cursor+=5;char *end=strchr(cursor,'<');if(!end)break;
        char *begin=cursor;for(char *p=cursor;p<end;p++)if(*p=='/')begin=p+1;
        size_t n=(size_t)(end-begin);if(n<10||n>200||strncmp(end-9,".identity",9))continue;
        char *stop=strstr(end,"href>"),*mod=strstr(end,"getlastmodified");long long when=0;
        if(mod&&(!stop||mod<stop)) {char *gt=strchr(mod,'>');if(gt)when=http_time(gt+1);}
        if(when>=best_time&&n-9<sizeof best) {best_time=when;memcpy(best,begin,n-9);best[n-9]=0;}
    }
    free(xml);if(!*best)return 0;
    struct pscloud_snapshot snap;if(remote_snapshot(cloud,&who,best,&snap))return -1;
    snprintf(out->file,sizeof out->file,"%s",best);snprintf(out->user,sizeof out->user,"%s",user);snprintf(out->sha,sizeof out->sha,"%s",snap.sha256);out->created=snap.created;
    return 1;
}
static long long sync_base(const char *user,const char *title) {
    char path[1400];snprintf(path,sizeof path,"%s/sync/%s-%s",root,user,title);FILE *f=fopen(path,"r");long long when=0;
    if(f) {if(fscanf(f,"WHEN=%lld",&when)!=1)when=0;fclose(f);}return when;
}
static void sync_mark(const char *user,const char *title,const char *sha) {
    char path[1400],text[128];snprintf(path,sizeof path,"%s/sync",root);(void)mkdir(path,0700);
    snprintf(path,sizeof path,"%s/sync/%s-%s",root,user,title);snprintf(text,sizeof text,"WHEN=%lld\nSHA=%s\n",(long long)time(NULL),sha&&strlen(sha)==64?sha:"-");atomic_config(path,text);
}
static void sync_received(const char *user,const char *title,char sha[65]) {
    char path[1400];snprintf(path,sizeof path,"%s/sync/%s-%s",root,user,title);FILE *f=fopen(path,"r");long long when;sha[0]=0;
    if(f) {if(fscanf(f,"WHEN=%lld\nSHA=%64s",&when,sha)!=2||strlen(sha)!=64)sha[0]=0;fclose(f);}
}
enum {SYNC_UNKNOWN,SYNC_NONE,SYNC_IN,SYNC_CLOUD,SYNC_LOCAL,SYNC_CONFLICT,SYNC_ONLY_HERE,SYNC_OFFLINE};
static const char *const sync_names[]={"unknown","not_in_cloud","in_sync","cloud_newer","local_newer","conflict","only_here","other_offline"};
static int sync_state(const char *user,const char *title,struct cloud_version *newest_other,long long *local_time) {
    struct settings cloud={0};int has_cloud=!pscloud_configure(cloudpath,&cloud);
    char peers[8][64];int peer_count=peers_list(peers);
    memset(newest_other,0,sizeof *newest_other);
    if(!has_cloud&&!peer_count)return SYNC_UNKNOWN;
    char path[1400];snprintf(path,sizeof path,"%s/%s/savedata_prospero/%s",home,user,title);int fd=pscloud_open_directory(path);
    *local_time=fd>=0?(long long)newest_container(fd):0;if(fd>=0)close(fd);
    struct cloud_version own={0},v;int got=has_cloud?cloud_newest(&cloud,user,title,&own):0;
    if(got<0&&!peer_count) {memset(cloud.password,0,sizeof cloud.password);return SYNC_UNKNOWN;}
    if(got<0)got=0;
    /* a backup this PS5 made is visible to the other console over the network */
    struct cloud_version mine;if(peer_count&&local_latest(user,title,&mine)&&mine.created>own.created) {own=mine;got=1;}
    char linked[4][17];int n=links_for(user,linked),any_other=0,unreachable=0;
    for(int i=0;i<n;i++) {
        if(has_cloud&&cloud_newest(&cloud,linked[i],title,&v)>0&&v.created>newest_other->created) {*newest_other=v;any_other=1;}
        for(int k=0;k<peer_count;k++) {int r=peer_latest(peers[k],linked[i],title,&v);if(r<0)unreachable=1;else if(r&&v.created>newest_other->created) {*newest_other=v;any_other=1;}}
    }
    memset(cloud.password,0,sizeof cloud.password);
    /* Over the network, compare the actual saves: when the game last wrote each slot on each PS5. */
    for(int k=0;k<peer_count;k++)for(int i=0;i<n;i++) {
        struct live_slot here[64],there[64];int nt=peer_slots(peers[k],linked[i],title,there,64);if(nt<=0)continue;
        int nh=live_slots(user,title,here,64);struct slot_compare c;compare_slots(here,nh,there,nt,&c);
        if(strcmp(newest_other->peer,peers[k])||strcmp(newest_other->user,linked[i])) { /* fetch from this PS5; it backs up first */
            struct cloud_version found;if(peer_latest(peers[k],linked[i],title,&found)>0)*newest_other=found;
            else {memset(newest_other,0,sizeof *newest_other);snprintf(newest_other->user,sizeof newest_other->user,"%.16s",linked[i]);snprintf(newest_other->peer,sizeof newest_other->peer,"%.63s",peers[k]);}
        }
        newest_other->created=c.there_max;*local_time=c.here_max;
        int here_count=c.here_newer+c.only_here,there_count=c.there_newer+c.only_there;
        return here_count&&there_count?SYNC_CONFLICT:there_count?SYNC_CLOUD:here_count?SYNC_LOCAL:SYNC_IN;
    }
    if(!any_other&&n&&unreachable)return SYNC_OFFLINE; /* cannot tell: the other PS5 did not answer */
    if(!any_other&&n)return *local_time?SYNC_ONLY_HERE:SYNC_NONE; /* linked, but the other side has no copy */
    if(!got&&!any_other)return SYNC_NONE;
    if(*newest_other->peer) {
        /* Over the network both sides say which save they hold; the same save means in sync. */
        struct cloud_version latest;int have=local_latest(user,title,&latest);char mine[65];local_current(user,title,have?&latest:NULL,*local_time,mine);
        if(*mine&&*newest_other->current&&!strcmp(mine,newest_other->current))return SYNC_IN;
        long long synced=sync_base(user,title),theirs=newest_other->created;
        int here_changed=!synced||*local_time>synced+60,there_changed=!synced||theirs>synced+60;
        if(synced&&here_changed&&there_changed&&theirs>synced&&*local_time>synced)return SYNC_CONFLICT;
        return theirs>*local_time+60?SYNC_CLOUD:SYNC_LOCAL;
    }
    long long base=sync_base(user,title);if(got&&own.created>base)base=own.created;
    if(!base) /* never synced: the newer side wins */
        return any_other&&newest_other->created>*local_time?SYNC_CLOUD:SYNC_LOCAL;
    int cloud_changed=any_other&&newest_other->created>base,local_changed=*local_time>base+60;
    return cloud_changed&&local_changed?SYNC_CONFLICT:cloud_changed?SYNC_CLOUD:local_changed?SYNC_LOCAL:SYNC_IN;
}
static void sync_status_json(int sock,const char *query) {
    char user[32],title[16];if(parameter(query,"user",user,sizeof user)||!safe_word(user,16)||parameter(query,"title",title,sizeof title)||!pscloud_title_valid(title)) {reject(sock,400,"Choose a profile and game");return;}
    struct cloud_version other;long long local_time=0;webdav_quiet=1;int state=sync_state(user,title,&other,&local_time);webdav_quiet=0;
    char name[64]="",shown[400]="",firmware[16]="";
    if(*other.user) {
        if(profile_name(other.user,name,sizeof name))snprintf(name,sizeof name,"Profile %s",other.user);
        if(*other.peer) {
            char body[8192];size_t got=0;
            if(peer_get(other.peer,"/api/peer/info",body,sizeof body-1,&got,5)==200) {
                char pattern[64];snprintf(pattern,sizeof pattern,"\"user\":\"%s\",\"name\":\"",other.user);char *at=strstr(body,pattern);
                if(at) {at+=strlen(pattern);size_t k=0;while(*at&&*at!='"'&&k+1<sizeof name)name[k++]=*at++;name[k]=0;}
                json_text(body,"firmware",firmware,sizeof firmware);
                for(char *q=firmware;*q;q++)if(!strchr("0123456789.",*q))*q=0;
            }
        }
        escaped(shown,sizeof shown,name);
    }
    char linked[4][17],peers[8][64];int links=links_for(user,linked),peer_count=peers_list(peers);
    struct settings cloud={0};int has_cloud=!pscloud_configure(cloudpath,&cloud);memset(cloud.password,0,sizeof cloud.password);
    /* other_found: the linked profile has a copy somewhere; without it "newer" means nothing to compare */
    char json[1900];snprintf(json,sizeof json,"{\"state\":\"%s\",\"local_time\":%lld,\"base\":%lld,\"cloud\":{\"file\":\"%s\",\"user\":\"%s\",\"name\":\"%s\",\"created\":%lld,\"sha256\":\"%s\",\"source\":\"%s\",\"peer\":\"%s\",\"firmware\":\"%s\"},\"links\":%d,\"other_found\":%s,\"peers\":%d,\"has_cloud\":%s}",
        sync_names[state],local_time,sync_base(user,title),other.file,other.user,shown,other.created,other.sha,*other.peer?"network":"cloud",other.peer,firmware,links,*other.file||*other.peer?"true":"false",peer_count,has_cloud?"true":"false");
    respond(sock,200,"application/json",json,strlen(json));
}
/* Fetch the newest linked cloud version into share/ and run the staged check for this
 * profile. On success the normal Replace my save step can use it. */
static int sync_fetch(const char *user,const char *title,char file[128],char sha[65],char *why,size_t why_max) {
    struct cloud_version other;long long local_time=0;webdav_quiet=1;int state=sync_state(user,title,&other,&local_time);webdav_quiet=0;
    char slot_times[4096]="";
    if(*other.peer) {
        char ask[300],reply[512];size_t got=0;snprintf(ask,sizeof ask,"/api/peer/backup?from=%s&to=%s&title=%s",user,other.user,title);
        CURL *c=curl_easy_init();
        if(c) {char url_backup[700];struct peer_body pr={reply,0,sizeof reply-1};snprintf(url_backup,sizeof url_backup,"http://%s%s",other.peer,ask);
            curl_easy_setopt(c,CURLOPT_URL,url_backup);curl_easy_setopt(c,CURLOPT_PROTOCOLS_STR,"http");curl_easy_setopt(c,CURLOPT_POST,1L);curl_easy_setopt(c,CURLOPT_POSTFIELDS,"");
            curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,4L);curl_easy_setopt(c,CURLOPT_TIMEOUT,600L);curl_easy_setopt(c,CURLOPT_NOSIGNAL,1L);
            curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,peer_collect);curl_easy_setopt(c,CURLOPT_WRITEDATA,&pr);
            CURLcode rc=curl_easy_perform(c);long status=0;curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);curl_easy_cleanup(c);got=pr.size;reply[got]=0;
            if(rc!=CURLE_OK||status!=200)pscloud_log("INFO","The other PS5 did not make a fresh backup first (HTTP %ld); using its newest backup",status);
        }
        struct cloud_version fresh;if(peer_latest(other.peer,other.user,title,&fresh)>0)other=fresh;
        struct live_slot there[64];int nt=peer_slots(other.peer,other.user,title,there,64);size_t at=0;
        for(int i=0;i<nt&&at<sizeof slot_times-80;i++)at+=(size_t)snprintf(slot_times+at,sizeof slot_times-at,"%s%s:%lld",at?";":"",there[i].slot,there[i].mtime);
    }
    if(!*other.file) {snprintf(why,why_max,*other.peer?"Your other PS5 has no backup of this game it can send yet. Close the game there and try again":"No save from your other PS5 was found in the cloud or on your network");return -1;}
    (void)state;unsigned char *data=malloc(PSCLOUD_RESTORE_MAX+1U);if(!data) {snprintf(why,why_max,"Out of memory");return -1;}
    struct response r={(char *)data,0,PSCLOUD_RESTORE_MAX};long status=0;
    if(*other.peer) {
        char path_file[200];snprintf(path_file,sizeof path_file,"/api/peer/file?file=%s",other.file);size_t got=0;
        status=peer_get(other.peer,path_file,(char *)data,PSCLOUD_RESTORE_MAX,&got,300);r.size=got;
        pscloud_log("INFO","Network copy from %s: HTTP %ld, %zu bytes",other.peer,status,got);
    } else {
        struct settings cloud={0};if(pscloud_configure(cloudpath,&cloud)) {free(data);snprintf(why,why_max,"Connect your cloud first");return -1;}
        struct pscloud_snapshot who={0};snprintf(who.user,sizeof who.user,"%s",other.user);snprintf(who.title,sizeof who.title,"%s",title);strcpy(who.slot,"WholeGame");memset(who.sha256,'0',64);
        char folder[256],url[4400];pscloud_snapshot_folder(&who,folder,sizeof folder);snprintf(url,sizeof url,"%s/%s/%s",cloud.url,folder,other.file);
        status=webdav(&cloud,url,"GET",&r);memset(cloud.password,0,sizeof cloud.password);
    }
    int sealed=status==200?pscloud_decrypt_if_needed(data,&r.size):0;
    if(sealed) {free(data);snprintf(why,why_max,sealed==-2?"This cloud save is encrypted. Enter your encryption password in Cloud settings":"Wrong encryption password, or the cloud copy is damaged");return -1;}
    if(status!=200||pscloud_verify_hash(data,r.size,other.sha)||pscloud_share_validate(data,r.size,title)) {free(data);snprintf(why,why_max,"Cloud save could not be downloaded or did not verify");return -1;}
    char share[1400],id[33],path[1600];snprintf(share,sizeof share,"%s/share",root);(void)mkdir(share,0700);
    if(pscloud_random_id(id)) {free(data);return -1;}
    snprintf(file,128,"portable-%s-%s.zip",title,id);snprintf(path,sizeof path,"%s/%s",share,file);
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);size_t at=0;int bad=fd<0;
    while(!bad&&at<r.size) {ssize_t w=write(fd,data+at,r.size-at);if(w<=0)bad=1;else at+=(size_t)w;}
    if(fd>=0) {if(fsync(fd))bad=1;close(fd);}
    if(bad) {free(data);snprintf(why,why_max,"Could not store the cloud save");return -1;}
    char published[128];struct pscloud_share_options options={.skip_missing=1,.system_root=system_root};
    bad=pscloud_share_game(home,root,appmeta,user,title,1,data,r.size,&options,published,sha);free(data);
    if(bad) {unlink(path);const char *e=pscloud_share_error();snprintf(why,why_max,"Check failed: %s",*e?e:"see Activity");return -1;}
    char proof[1700];snprintf(proof,sizeof proof,"%s/%s.check-%s",share,file,user);atomic_config(proof,sha);
    snprintf(path,sizeof path,"%s/sync",root);(void)mkdir(path,0700);
    char pending[1400],text[256];snprintf(pending,sizeof pending,"%s/sync/%s-%s.pending",root,user,title);char pending_text[4600];snprintf(pending_text,sizeof pending_text,"FILE=%s\nSHA=%s\nTIMES=%s\n",file,other.sha,slot_times);atomic_config(pending,pending_text);(void)text;
    pscloud_log("EVENT","Newer save for %s fetched from %s and checked; ready to replace",title,*other.peer?other.peer:"the cloud");
    return 0;
}
/* After a replace: if it used the fetched sync file, this game is now in sync. */
static void sync_after_restore(const char *user,const char *title,const char *file) {
    char pending[1400],line[256]={0},sha_line[128]={0},times_line[4700]={0};snprintf(pending,sizeof pending,"%s/sync/%s-%s.pending",root,user,title);FILE *f=fopen(pending,"r");if(!f)return;
    if(!fgets(line,sizeof line,f))line[0]=0;
    if(!fgets(sha_line,sizeof sha_line,f))sha_line[0]=0;
    if(!fgets(times_line,sizeof times_line,f))times_line[0]=0;
    fclose(f);char expect[200];snprintf(expect,sizeof expect,"FILE=%s\n",file);
    if(strcmp(line,expect))return;
    char sha[65]={0};if(!strncmp(sha_line,"SHA=",4))sscanf(sha_line+4,"%64s",sha);
    if(!strncmp(times_line,"TIMES=",6)) {times_line[strcspn(times_line,"\n")]=0;apply_slot_times(user,title,times_line+6);}
    sync_mark(user,title,sha);unlink(pending);
}
static void cloud_profiles_json(int sock) {
    struct settings cloud={0};if(pscloud_configure(cloudpath,&cloud)) {respond(sock,200,"application/json","{\"profiles\":[],\"links\":[]}",28);return;}
    registry_publish();webdav_quiet=1;
    char url[4200];snprintf(url,sizeof url,"%s/.pscloud-profiles/",cloud.url);char *xml=malloc(256*1024+1);struct response r={xml,0,256*1024};
    long status=xml?webdav(&cloud,url,"PROPFIND",&r):0;
    char json[16384];int pos=snprintf(json,sizeof json,"{\"console\":\"%s\",\"profiles\":[",console_id);unsigned count=0;
    if(xml&&(status==207||status==200)) {xml[r.size]=0;
        for(char *cursor=xml;(cursor=strstr(cursor,"href>"))&&count<32;) {
            cursor+=5;char *end=strchr(cursor,'<');if(!end)break;char *begin=cursor;for(char *p=cursor;p<end;p++)if(*p=='/')begin=p+1;
            size_t n=(size_t)(end-begin);if(n<5||n>40||strncmp(end-4,".txt",4))continue;char id[32];memcpy(id,begin,n-4);id[n-4]=0;if(!safe_word(id,16))continue;
            char text[600];struct response t={text,0,sizeof text-1};snprintf(url,sizeof url,"%s/.pscloud-profiles/%s.txt",cloud.url,id);
            if(webdav(&cloud,url,"GET",&t)!=200)continue;
            text[t.size]=0;
            char name[200]="",console[32]="",fw[16]="";char *a=strstr(text,"NAME="),*b=strstr(text,"CONSOLE="),*c=strstr(text,"FIRMWARE=");
            if(a)sscanf(a+5,"%199[^\n]",name);
            if(b)sscanf(b+8,"%31[^\n]",console);
            if(c)sscanf(c+9,"%15[^\n]",fw);
            for(char *q=name;*q;q++)if(*q=='"'||*q=='\\'||(unsigned char)*q<32)*q=' ';
            if(!safe_word(console,16))console[0]=0;
            for(char *q=fw;*q;q++)if(!strchr("0123456789.",*q))*q=0;
            int w=snprintf(json+pos,sizeof json-(size_t)pos,"%s{\"user\":\"%s\",\"name\":\"%s\",\"console\":\"%s\",\"firmware\":\"%s\",\"this_console\":%s}",count?",":"",id,name,console,fw,!strcmp(console,console_id)?"true":"false");
            if(w<0||(size_t)w>=sizeof json-(size_t)pos-600)break;
            pos+=w;count++;
        }}
    free(xml);webdav_quiet=0;memset(cloud.password,0,sizeof cloud.password);
    pos+=snprintf(json+pos,sizeof json-(size_t)pos,"],\"links\":{");
    char path[1400],line[256];snprintf(path,sizeof path,"%s/links.conf",root);FILE *f=fopen(path,"r");unsigned links=0;
    while(f&&fgets(line,sizeof line,f)) {char who[32],list[200];if(sscanf(line,"%31s %199s",who,list)!=2||!safe_word(who,16))continue;
        for(char *q=list;*q;q++)if(!strchr("0123456789abcdefABCDEF,",*q))*q=0;
        pos+=snprintf(json+pos,sizeof json-(size_t)pos,"%s\"%s\":\"%s\"",links++?",":"",who,list);}
    if(f)fclose(f);
    snprintf(json+pos,sizeof json-(size_t)pos,"}}");respond(sock,200,"application/json",json,strlen(json));
}
/* Background: games with "download newer saves automatically" are checked every 15 minutes. */
static void sync_tick(void) {
    struct settings cloud={0};char peers[8][64];int has_cloud=!pscloud_configure(cloudpath,&cloud);memset(cloud.password,0,sizeof cloud.password);
    if(!has_cloud&&!peers_list(peers))return;
    struct game_setting copy[512];unsigned n;pthread_mutex_lock(&settings_mutex);n=game_settings_count;memcpy(copy,game_settings,n*sizeof copy[0]);pthread_mutex_unlock(&settings_mutex);
    pthread_mutex_lock(&peer_synced_mutex);
    for(unsigned k=0;k<peer_synced_count&&n<512;k++) {
        int known=0;for(unsigned i=0;i<n;i++)if(!strcmp(copy[i].title,peer_synced[k])) {copy[i].auto_sync=1;known=1;}
        if(!known) {memset(&copy[n],0,sizeof copy[n]);strcpy(copy[n].title,peer_synced[k]);copy[n].close_backup=1;copy[n].backup_all=1;copy[n].auto_sync=1;n++;}
    }
    pthread_mutex_unlock(&peer_synced_mutex);
    for(unsigned i=0;i<n&&!stopped;i++) {
        if(!copy[i].auto_sync||title_running(copy[i].title)>0)continue; /* (peer-synced titles are added below) */
        DIR *users=opendir(home);struct dirent *u;
        while(users&&(u=readdir(users))&&!stopped) {
            if(!safe_word(u->d_name,16))continue;
            char linked[4][17];if(!links_for(u->d_name,linked))continue;
            struct cloud_version other;long long local_time;webdav_quiet=1;int state=sync_state(u->d_name,copy[i].title,&other,&local_time);webdav_quiet=0;
            if(state!=SYNC_CLOUD)continue;
            char name[256],file[128],sha[65],why[300];pscloud_app_name(appmeta,copy[i].title,name,sizeof name);
            if(pthread_mutex_trylock(&operation_mutex))return;
            pscloud_log_open(logpath);int bad=sync_fetch(u->d_name,copy[i].title,file,sha,why,sizeof why);
            if(!bad&&copy[i].auto_apply&&title_running(copy[i].title)<=0) {
                char published[128],check[65];unsigned char *data=NULL;size_t size=0;char share[1400];snprintf(share,sizeof share,"%s/share",root);int dir=pscloud_open_directory(share);
                struct pscloud_share_options options={.skip_missing=1,.system_root=system_root};
                bad=dir<0||pscloud_read_archive(dir,file,&data,&size)||pscloud_share_game(home,root,appmeta,u->d_name,copy[i].title,2,data,size,&options,published,check);free(data);if(dir>=0)close(dir);
                if(!bad) {sync_after_restore(u->d_name,copy[i].title,file);pscloud_notify("Updated %s with your newer save from the cloud",name);}
                else pscloud_log("WARN","Automatic replace of %s stopped; your save is unchanged or a rollback copy was kept",name);
            } else if(!bad)pscloud_notify("A newer save for %s is ready. Open PSCloud to use it",name);
            else pscloud_log("WARN","Sync for %s: %s",name,why);
            pthread_mutex_unlock(&operation_mutex);
        }
        if(users)closedir(users);
    }
}

/* ---------- Send a newer save to the other PS5 ----------
 * The other console fetches it itself through the read-only peer API, checks it on a staged
 * copy, and replaces only if its owner allowed that ("remote.conf ALLOW_REPLACE=1"). */
static int remote_replace_allowed(void) {
    char path[1400],line[64]={0};snprintf(path,sizeof path,"%s/remote.conf",root);FILE *f=fopen(path,"r");if(!f)return 0;
    if(!fgets(line,sizeof line,f))line[0]=0;
    fclose(f);return !strncmp(line,"ALLOW_REPLACE=1",15);
}
static int peer_known(const char *ip) {
    char peers[8][64];int n=peers_list(peers);size_t len=strlen(ip);
    for(int i=0;i<n;i++)if(!strncmp(peers[i],ip,len)&&peers[i][len]==':')return 1;
    return 0;
}
/* POST /api/peer/backup?from=<their profile>&to=<our profile>&title=: back up our current save
 * so the other PS5 gets the actual latest one. Only for a known console and a linked profile. */
static void peer_backup(int sock,const char *query,const char *from_ip) {
    char from[32],to[32],title[16],linked[4][17];
    if(parameter(query,"from",from,sizeof from)||!safe_word(from,16)||parameter(query,"to",to,sizeof to)||!safe_word(to,16)||parameter(query,"title",title,sizeof title)||!pscloud_title_valid(title)) {reject(sock,400,"Invalid request");return;}
    if(!peer_known(from_ip)) {reject(sock,403,"Unknown console");return;}
    int n=links_for(to,linked),ok=0;for(int i=0;i<n;i++)if(!strcmp(linked[i],from))ok=1;
    if(!ok) {reject(sock,403,"Profiles not linked");return;}
    if(title_running(title)>0) {reject(sock,409,"The game is open on this PS5");return;}
    if(pthread_mutex_trylock(&operation_mutex)) {reject(sock,409,"Busy");return;}
    char published[144];int unchanged=0;pscloud_log_open(logpath);int bad=backup_one(to,title,published,&unchanged);pthread_mutex_unlock(&operation_mutex);
    if(bad) {reject(sock,500,"Backup failed");return;}
    const char *json=unchanged?"{\"ok\":true,\"unchanged\":true}":"{\"ok\":true,\"unchanged\":false}";respond(sock,200,"application/json",json,strlen(json));
}
/* On the receiving console: POST /api/peer/push?from=<their profile>&to=<our profile>&title=&replace=0|1 */
static void peer_push(int sock,const char *query,const char *from_ip) {
    char from[32],to[32],title[16],replace[8]="0",linked[4][17];
    if(parameter(query,"from",from,sizeof from)||!safe_word(from,16)||parameter(query,"to",to,sizeof to)||!safe_word(to,16)||parameter(query,"title",title,sizeof title)||!pscloud_title_valid(title)) {reject(sock,400,"Invalid request");return;}
    (void)parameter(query,"replace",replace,sizeof replace);
    if(!peer_known(from_ip)) {reject(sock,403,"Add the sending PS5 under Other PS5s on your network on this PS5 first");return;}
    int n=links_for(to,linked),ok=0;for(int i=0;i<n;i++)if(!strcmp(linked[i],from))ok=1;
    if(!ok) {reject(sock,403,"These profiles are not linked on this PS5");return;}
    if(title_running(title)>0) {reject(sock,409,"The game is open on the other PS5. Close it there and try again");return;}
    if(pthread_mutex_trylock(&operation_mutex)) {reject(sock,409,"The other PS5 is busy; try again in a moment");return;}
    char name[256],file[128],sha[65],why[300];pscloud_app_name(appmeta,title,name,sizeof name);pscloud_log_open(logpath);
    pscloud_log("EVENT","Your other PS5 (%s) sent a newer save for %s",from_ip,name);
    int bad=sync_fetch(to,title,file,sha,why,sizeof why);
    if(bad) {pthread_mutex_unlock(&operation_mutex);char msg[400];snprintf(msg,sizeof msg,"The other PS5 could not use it: %s",why);reject(sock,400,msg);return;}
    if(!strcmp(replace,"1")&&remote_replace_allowed()&&title_running(title)<=0) {
        char share[1400],published[128],check[65];snprintf(share,sizeof share,"%s/share",root);int dir=pscloud_open_directory(share);unsigned char *data=NULL;size_t size=0;
        struct pscloud_share_options options={.skip_missing=1,.system_root=system_root};
        bad=dir<0||pscloud_read_archive(dir,file,&data,&size)||pscloud_share_game(home,root,appmeta,to,title,2,data,size,&options,published,check);
        free(data);if(dir>=0)close(dir);
        pthread_mutex_unlock(&operation_mutex);
        if(bad) {const char *e=pscloud_share_error();char msg[400];snprintf(msg,sizeof msg,"Checked, but replacing on the other PS5 stopped: %s. Its save is unchanged or a rollback copy was kept",*e?e:"see Activity there");reject(sock,500,msg);return;}
        sync_after_restore(to,title,file);pscloud_notify("%s was updated with the newer save from your other PS5",name);
        const char *json="{\"ok\":true,\"state\":\"replaced\",\"message\":\"Replaced on the other PS5. A rollback copy of its old save was kept\"}";respond(sock,200,"application/json",json,strlen(json));return;
    }
    pthread_mutex_unlock(&operation_mutex);
    pscloud_notify("A newer save for %s from your other PS5 is ready. Open PSCloud to use it",name);
    const char *json=!strcmp(replace,"1")?"{\"ok\":true,\"state\":\"ready\",\"message\":\"Sent and checked. The other PS5 does not allow remote replacing, so it is waiting there: open the game in PSCloud on that PS5 and choose Replace my save\"}":
        "{\"ok\":true,\"state\":\"ready\",\"message\":\"Sent and checked. On the other PS5, open the game in PSCloud and choose Replace my save\"}";
    respond(sock,200,"application/json",json,strlen(json));
}
/* Both consoles' slots for one game, for the comparison table. */
static void sync_slots_json(int sock,const char *query) {
    char user[32],title[16];if(parameter(query,"user",user,sizeof user)||!safe_word(user,16)||parameter(query,"title",title,sizeof title)||!pscloud_title_valid(title)) {reject(sock,400,"Choose a profile and game");return;}
    struct live_slot here[64],there[64];int nh=live_slots(user,title,here,64),nt=0;char peers[8][64],linked[4][17],from[64]="";
    int np=peers_list(peers),nl=links_for(user,linked);
    for(int k=0;k<np&&!nt;k++)for(int i=0;i<nl&&!nt;i++) {int got=peer_slots(peers[k],linked[i],title,there,64);if(got>0) {nt=got;snprintf(from,sizeof from,"%.63s",peers[k]);}}
    char *json=malloc(420000);if(!json) {reject(sock,500,"Out of memory");return;}
    size_t pos=(size_t)snprintf(json,420000,"{\"peer\":\"%s\",\"here\":",from);live_slots_json(json+pos,200000,here,nh);pos=strlen(json);
    pos+=(size_t)snprintf(json+pos,420000-pos,",\"there\":");live_slots_json(json+pos,200000,there,nt);strcat(json,"}");
    respond(sock,200,"application/json",json,strlen(json));free(json);
}
struct background_job {struct settings settings;char spool[1400],files[256][144];unsigned count;};
static void cloud_prune_uploaded(const struct background_job *job);
static void *background_upload(void *arg) {
    struct background_job *job=arg;int result=0;
    for(unsigned i=0;i<job->count;i++) {
        if(stopped||background_cancelled) {result=1;break;}
        result|=pscloud_worker_upload(job->spool,job->files[i],&job->settings);
    }
    if(background_cancelled)result=1;
    if(!result)cloud_prune_uploaded(job);
    if(!result && !pthread_mutex_trylock(&operation_mutex)) {prune_local_history();pthread_mutex_unlock(&operation_mutex);}
    background_result=result;
    pscloud_log(result?"WARN":"EVENT",result?"Background upload stopped or failed; local backups retained":"Background upload verified and completed");
    volatile unsigned char *secret=(volatile unsigned char *)&job->settings;for(size_t i=0;i<sizeof job->settings;i++)secret[i]=0;
    free(job);background_active=0;
    pthread_mutex_lock(&clients_mutex);clients--;pthread_cond_broadcast(&clients_done);pthread_mutex_unlock(&clients_mutex);return NULL;
}

/* ---------- Cloud housekeeping ----------
 * cloud-keep.conf "KEEP=N": after each upload, keep the N newest versions of that save in
 * the cloud (0 = keep everything). Older versions lose their archive, then their identity. */
static int cloud_keep;
/* encrypt.conf "PASSWORD=..." (owner-only). Never sent back to the browser. */
static void encryption_load(void) {
    char path[1400],line[300]={0};snprintf(path,sizeof path,"%s/encrypt.conf",root);FILE *f=fopen(path,"r");
    if(!f) {pscloud_encryption_set(NULL);return;}
    if(!fgets(line,sizeof line,f))line[0]=0;
    fclose(f);line[strcspn(line,"\n")]=0;
    pscloud_encryption_set(!strncmp(line,"PASSWORD=",9)?line+9:NULL);memset(line,0,sizeof line);
}
static void cloud_keep_load(void) {
    char path[1400];snprintf(path,sizeof path,"%s/cloud-keep.conf",root);FILE *f=fopen(path,"r");int keep=0;
    if(f) {if(fscanf(f,"KEEP=%d",&keep)!=1||keep<0||keep>999)keep=0;fclose(f);}cloud_keep=keep;
}
struct cloud_entry {char file[128];long long created;};
static int newest_first(const void *a,const void *b) {const struct cloud_entry *x=a,*y=b;return x->created<y->created?1:x->created>y->created?-1:strcmp(y->file,x->file);}
/* Returns how many versions were removed, or -1. */
static int cloud_prune(const struct settings *cloud,const struct pscloud_snapshot *who,int keep) {
    if(keep<=0)return 0;
    char folder[256],url[4400];if(pscloud_snapshot_folder(who,folder,sizeof folder))return -1;
    snprintf(url,sizeof url,"%s/%s/.pscloud/",cloud->url,folder);
    char *xml=malloc(512*1024+1);if(!xml)return -1;struct response r={xml,0,512*1024};long status=webdav(cloud,url,"PROPFIND",&r);
    if(status==404) {free(xml);return 0;}
    if(status!=207&&status!=200) {free(xml);return -1;}
    xml[r.size]=0;struct cloud_entry *list=calloc(256,sizeof *list);unsigned n=0;
    for(char *cursor=xml;list&&(cursor=strstr(cursor,"href>"))&&n<256;) {
        cursor+=5;char *end=strchr(cursor,'<');if(!end)break;char *begin=cursor;for(char *p=cursor;p<end;p++)if(*p=='/')begin=p+1;
        size_t len=(size_t)(end-begin);if(len<10||len-9>=sizeof list[0].file||strncmp(end-9,".identity",9))continue;
        memcpy(list[n].file,begin,len-9);list[n].file[len-9]=0;if(!valid_archive(list[n].file))continue;
        struct pscloud_snapshot snap;if(remote_snapshot(cloud,who,list[n].file,&snap))continue; /* unreadable: never delete */
        list[n].created=snap.created;n++;
    }
    free(xml);if(!list)return -1;
    qsort(list,n,sizeof *list,newest_first);int removed=0;
    for(unsigned i=(unsigned)keep;i<n;i++) {
        snprintf(url,sizeof url,"%s/%s/%s",cloud->url,folder,list[i].file);long a=webdav(cloud,url,"DELETE",NULL);
        if(a!=404&&(a<200||a>=300))continue;
        snprintf(url,sizeof url,"%s/%s/.pscloud/%s.identity",cloud->url,folder,list[i].file);webdav(cloud,url,"DELETE",NULL);removed++;
    }
    free(list);return removed;
}
/* Account quota and the PSCloud folder size, from one PROPFIND with explicit properties. */
static long cloud_quota(const struct settings *cloud,unsigned long long *used,unsigned long long *available) {
    *used=*available=0;CURL *c=curl_easy_init();if(!c)return 0;
    char buffer[16384];struct response r={buffer,0,sizeof buffer-1};
    static const char body[]="<?xml version=\"1.0\"?><d:propfind xmlns:d=\"DAV:\"><d:prop><d:quota-used-bytes/><d:quota-available-bytes/></d:prop></d:propfind>";
    curl_easy_setopt(c,CURLOPT_URL,cloud->url);curl_easy_setopt(c,CURLOPT_PROTOCOLS_STR,"https");curl_easy_setopt(c,CURLOPT_USERAGENT,"PSCloud/" PSCLOUD_VERSION);
    curl_easy_setopt(c,CURLOPT_USERNAME,cloud->user);curl_easy_setopt(c,CURLOPT_PASSWORD,cloud->password);curl_easy_setopt(c,CURLOPT_CAINFO,cloud->ca);
    curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,10L);curl_easy_setopt(c,CURLOPT_TIMEOUT,25L);curl_easy_setopt(c,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(c,CURLOPT_CUSTOMREQUEST,"PROPFIND");curl_easy_setopt(c,CURLOPT_POSTFIELDS,body);curl_easy_setopt(c,CURLOPT_POSTFIELDSIZE,(long)(sizeof body-1));
    struct curl_slist *headers=curl_slist_append(NULL,"Depth: 0");headers=curl_slist_append(headers,"Content-Type: application/xml");curl_easy_setopt(c,CURLOPT_HTTPHEADER,headers);
    curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,collect);curl_easy_setopt(c,CURLOPT_WRITEDATA,&r);
    CURLcode result=curl_easy_perform(c);long status=0;curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);curl_slist_free_all(headers);curl_easy_cleanup(c);
    if(result!=CURLE_OK)return 0;
    buffer[r.size]=0;
    char *u=strstr(buffer,"quota-used-bytes>"),*a=strstr(buffer,"quota-available-bytes>");
    if(u)*used=strtoull(u+17,NULL,10);
    if(a) {long long v=strtoll(a+22,NULL,10);*available=v>0?(unsigned long long)v:0;}
    /* 207 with no numbers means the provider does not report space */
    if(status==207&&!u) {static int warned;if(!warned) {warned=1;pscloud_log("INFO","Your cloud does not report how much space is used");}return 0;}
    return status;
}
static void cloud_housekeeping_json(int sock) {
    struct settings cloud={0};unsigned long long used=0,available=0;long status=0;int configured=!pscloud_configure(cloudpath,&cloud);
    if(configured)status=cloud_quota(&cloud,&used,&available);
    memset(cloud.password,0,sizeof cloud.password);
    char json[256];snprintf(json,sizeof json,"{\"configured\":%s,\"keep\":%d,\"known\":%s,\"used\":%llu,\"available\":%llu}",configured?"true":"false",cloud_keep,status==207?"true":"false",used,available);
    respond(sock,200,"application/json",json,strlen(json));
}
/* After uploads: prune each uploaded save's folder. Quiet; runs on the upload thread. */
static void cloud_prune_uploaded(const struct background_job *job) {
    if(cloud_keep<=0)return;
    int dir=pscloud_open_directory(job->spool);if(dir<0)return;
    webdav_quiet=1;
    for(unsigned i=0;i<job->count;i++) {
        char file[144],identity[160];snprintf(file,sizeof file,"%s",job->files[i]);size_t n=strlen(file);
        if(n>6&&!strcmp(file+n-6,".ready"))file[n-6]=0;
        snprintf(identity,sizeof identity,"%s.identity",file);struct pscloud_snapshot snap;
        if(pscloud_snapshot_read(dir,identity,&snap))continue;
        int removed=cloud_prune(&job->settings,&snap,cloud_keep);
        if(removed>0) {char name[256];pscloud_app_name(appmeta,snap.title,name,sizeof name);pscloud_log("EVENT","Removed %d older cloud versions of %s (keeping the newest %d)",removed,name,cloud_keep);}
    }
    webdav_quiet=0;close(dir);
}
/* Starts the upload thread for a prepared job; frees the job on failure. */
static int launch_background(struct background_job *job) {
    /* Freeze this job's filenames: later PC imports are never auto-uploaded. */
    background_result=0;background_cancelled=0;pscloud_worker_prepare();background_active=1;
    pthread_mutex_lock(&clients_mutex);clients++;pthread_mutex_unlock(&clients_mutex);
    pthread_t thread;pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,4U*1024*1024);
    int bad=pthread_create(&thread,&attr,background_upload,job);pthread_attr_destroy(&attr);
    if(bad) {background_active=0;free(job);pthread_mutex_lock(&clients_mutex);clients--;pthread_mutex_unlock(&clients_mutex);return -1;}
    pthread_detach(thread);return 0;
}
static void start_background(int sock,const char *form) {
    if(background_active) {message(sock,409,"A background upload is already running");return;}
    struct background_job *job=calloc(1,sizeof *job);if(!job) {message(sock,500,"Not enough memory");return;}
    if(pscloud_configure(cloudpath,&job->settings)) {free(job);message(sock,400,"Saved cloud connection is unavailable; queued backups are unchanged");return;}
    char file[128]={0};struct pscloud_snapshot snapshot;
    if(!parameter(form,"file",file,sizeof file) && *file) {
        if(pending_archive(file,&snapshot,NULL,NULL)) {free(job);message(sock,400,"Selected pending backup unavailable");return;}
        snprintf(job->files[0],sizeof job->files[0],"%s.ready",file);job->count=1;
        pthread_mutex_lock(&cache_mutex);pscloud_app_name(appmeta,snapshot.title,background_name,sizeof background_name);pthread_mutex_unlock(&cache_mutex);
    }else {pthread_mutex_lock(&cache_mutex);strcpy(background_name,"Queued backups");pthread_mutex_unlock(&cache_mutex);}
    snprintf(job->spool,sizeof job->spool,"%s/spool",root);
    if(!job->count) {
        int dir=pscloud_open_directory(job->spool),bad=dir<0;DIR *d=dir<0?NULL:fdopendir(dir);struct dirent *e;
        if(dir>=0&&!d)close(dir);
        if(d) {while((e=readdir(d))) {
            size_t n=strlen(e->d_name);if(n<7 || n>=144 || strcmp(e->d_name+n-6,".ready"))continue;
            char object[128];if(n-6>=sizeof object)continue;memcpy(object,e->d_name,n-6);object[n-6]=0;
            if(!valid_archive(object))continue;
            if(job->count==256) {bad=1;break;}
            snprintf(job->files[job->count++],144,"%s.ready",object);
        }closedir(d);}else bad=1;
        if(bad || !job->count) {free(job);message(sock,400,bad?"Queue unavailable or exceeds 256 jobs; upload individual backups":"No queued backups to upload");return;}
    }
    if(launch_background(job)) {message(sock,500,"Could not start background upload; local backups retained");return;}
    const char *json="{\"ok\":true,\"background\":true,\"message\":\"Background upload started; you can leave this page\"}";
    respond(sock,202,"application/json",json,strlen(json));
}
static void upload_created(int sock,const char *ready,int background) {
    size_t n=strlen(ready);
    if(n<7 || n>=144 || strcmp(ready+n-6,".ready")) {message(sock,500,"Backup retained locally; exact upload selection unavailable");return;}
    if(background_active) {message(sock,200,"Selected game backed up locally; upload already running, so this backup stays queued");return;}
    if(background) {char form[160];snprintf(form,sizeof form,"file=%.*s",(int)(n-6),ready);start_background(sock,form);return;}
    int result=upload_selected(ready);
    message(sock,result?502:200,result?"Backup retained locally; selected upload pending":"Selected game backup checked and uploaded");
}
static void attachment(int sock,const char *file,const unsigned char *data,size_t size) {
    char header[1024];int n=snprintf(header,sizeof header,"HTTP/1.1 200 OK\r\nContent-Type: application/zip\r\nContent-Disposition: attachment; filename=\"%s\"\r\nContent-Length: %zu\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n\r\n",file,size);
    send_all(sock,header,(size_t)n);send_all(sock,(const char *)data,size);
}
static int store_blob(int dir,const char *name,const unsigned char *data,size_t size) {
    int fd=openat(dir,name,O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,0600);if(fd<0)return -1;
    struct stat st;int bad=fstat(fd,&st) || !S_ISREG(st.st_mode);size_t have=0;
    while(!bad && have<size) {ssize_t n=write(fd,data+have,size-have);if(n<0 && errno==EINTR)continue;if(n<=0)bad=1;else have+=(size_t)n;}
    if(fsync(fd))bad=1;
    if(close(fd))bad=1;
    return bad?-1:0;
}
static int archive_valid(const unsigned char *data,size_t size,const struct pscloud_snapshot *s) {
    struct pscloud_bundle bundle;const unsigned char *payload;size_t length;
    if(!strcmp(s->slot,"WholeGame")&&!pscloud_share_validate(data,size,s->title))return 0;
    return !strcmp(s->slot,"WholeGame")?pscloud_bundle_index(data,size,s,&bundle):pscloud_save_payload(data,size,&payload,&length);
}
static int restore_game_data(const struct pscloud_snapshot *s,const unsigned char *data,size_t size,int check,int smart) {
    if(!pscloud_share_validate(data,size,s->title)) {
        char published[128],hash[65];return pscloud_share_game(home,root,appmeta,s->user,s->title,check?1:2,data,size,NULL,published,hash)?6:0;
    }
    return check?pscloud_bundle_restore_check(home,root,s,data,size):smart?pscloud_bundle_restore_smart(home,root,s,data,size):pscloud_bundle_restore(home,root,s,data,size);
}
static int compact_backup_ex(const struct pscloud_snapshot *chosen,char published[144],int requeue,int *unchanged) {
    if(unchanged)*unchanged=0;
    char file[128],hash[65];published[0]=0;
    if(pscloud_share_game(home,root,appmeta,chosen->user,chosen->title,0,NULL,0,NULL,file,hash))return -1;
    char shared[1400],spool[1400];snprintf(shared,sizeof shared,"%s/share",root);snprintf(spool,sizeof spool,"%s/spool",root);
    int share=pscloud_open_directory(shared),parent=pscloud_open_directory(root),dir=-1,lock=-1,result=-1;
    unsigned char *data=NULL;size_t size=0;struct pscloud_snapshot s=*chosen;strcpy(s.sha256,hash);s.created=(long long)time(NULL);
    if(parent>=0) {(void)mkdirat(parent,"spool",0700);close(parent);}dir=pscloud_open_directory(spool);
    if(share<0||dir<0||pscloud_read_archive(share,file,&data,&size)||pscloud_verify_hash(data,size,hash)||archive_valid(data,size,&s))goto done;
    struct pscloud_dedup_stats duplicate;int found=pscloud_snapshot_exists_checked(dir,&s,&duplicate);
    if(found<0||(!found&&duplicate.hash_failed))goto done;
    if(found) {
        if(unchanged)*unchanged=1;
        if(!requeue) {published[0]=0;result=0;goto done;}
        if(pscloud_snapshot_requeue(dir,duplicate.archive))goto done;
        size_t length=strlen(duplicate.archive),suffix=!strcmp(duplicate.archive+length-6,".ready")?6:5;
        snprintf(published,144,"%.*s.ready",(int)(length-suffix),duplicate.archive);result=0;goto done;
    }
    lock=openat(dir,".worker.lock",O_CREAT|O_RDWR|O_NOFOLLOW,0600);if(lock<0)goto done;
    /* A running upload holds the queue lock; wait for it instead of failing the backup. */
    for(unsigned waited=0;flock(lock,LOCK_EX|LOCK_NB);waited++) {
        if(!waited)pscloud_log("INFO","Waiting for the current upload to finish before adding this backup");
        if(waited>=600||stopped) {pscloud_log("WARN","Backup not added: an upload was still running after 10 minutes");goto done;}
        sleep(1);
    }
    char part[144],ready[144],identity[144],metadata[512],path[1600];
    snprintf(part,sizeof part,"%s.part",file);snprintf(ready,sizeof ready,"%s.ready",file);snprintf(identity,sizeof identity,"%s.identity",file);
    if(store_blob(dir,part,data,size))goto done;
    snprintf(metadata,sizeof metadata,"USER_ID=%s\nTITLE=%s\nSAVE_NAME=WholeGame\nSHA256=%s\nCREATED_UNIX=%lld\n",s.user,s.title,s.sha256,s.created);
    snprintf(path,sizeof path,"%s/%s",spool,identity);
    if(atomic_config(path,metadata)||renameat(dir,part,dir,ready)||fsync(dir))goto done;
    snprintf(published,144,"%s",ready);result=0;
done:
    if(!result&&share>=0) {if(unlinkat(share,file,0)||fsync(share))pscloud_log("WARN","Compact backup queued; temporary export retained");}
    free(data);if(lock>=0)close(lock);if(dir>=0)close(dir);if(share>=0)close(share);return result;
}
static int compact_backup(const struct pscloud_snapshot *chosen,char published[144]) {return compact_backup_ex(chosen,published,1,NULL);}
static int local_restore_copy(const char *file,const struct pscloud_snapshot *s,unsigned char **data,size_t *size) {
    char directory[1400],name[144];
    for(unsigned i=0;i<3;i++) {
        snprintf(directory,sizeof directory,"%s/%s",root,i==2?"downloads":"spool");
        snprintf(name,sizeof name,"%s%s",file,i==0?".ready":i==1?".sent":"");
        int dir=pscloud_open_directory(directory);if(dir<0)continue;
        int bad=pscloud_read_archive(dir,name,data,size);close(dir);
        if(!bad && !pscloud_verify_hash(*data,*size,s->sha256) && !archive_valid(*data,*size,s)) {pscloud_log("INFO","Restore source: checksum-verified local archive; cloud archive download skipped");return 0;}
        free(*data);*data=NULL;*size=0;
    }
    return -1;
}
static int local_restore_identity(const char *file,const struct pscloud_snapshot *chosen,const char *sha,struct pscloud_snapshot *s) {
    if(strlen(sha)!=64)return -1;
    for(unsigned i=0;i<64;i++)if(!strchr("0123456789abcdef",sha[i]))return -1;
    char directory[1400],name[144];snprintf(directory,sizeof directory,"%s/spool",root);snprintf(name,sizeof name,"%s.identity",file);
    int dir=pscloud_open_directory(directory);if(dir<0)return -1;
    int bad=pscloud_snapshot_read(dir,name,s);close(dir);
    return bad || strcmp(s->user,chosen->user) || strcmp(s->title,chosen->title) || strcmp(s->slot,chosen->slot) || strcmp(s->sha256,sha)?-1:0;
}
static void download_pc(int sock,const char *query,int local) {
    char file[128];struct pscloud_snapshot s={0},chosen;unsigned char *data=NULL;size_t size=0;
    if(parameter(query,"file",file,sizeof file) || !valid_archive(file)) {message(sock,400,"Invalid backup filename");return;}
    int bad=0;
    if(local)bad=pending_archive(file,&s,&data,&size);
    else {
        struct settings cloud={0};char folder[256],url[4096];
        bad=selection(query,&chosen) || pscloud_configure(cloudpath,&cloud) || remote_snapshot(&cloud,&chosen,file,&s) || pscloud_snapshot_folder(&s,folder,sizeof folder);
        if(!bad) {
            data=malloc(PSCLOUD_RESTORE_MAX+1U);if(!data)bad=1;
            else {snprintf(url,sizeof url,"%s/%s/%s",cloud.url,folder,file);struct response r={(char *)data,0,PSCLOUD_RESTORE_MAX};bad=webdav(&cloud,url,"GET",&r)!=200;size=r.size;
                if(!bad) {int sealed=pscloud_decrypt_if_needed(data,&size);if(sealed) {free(data);message(sock,400,sealed==-2?"This backup is encrypted. Enter your encryption password in Cloud settings":"Wrong encryption password, or the cloud copy is damaged");return;}}}
        }
    }
    if(!bad)bad=pscloud_verify_hash(data,size,s.sha256) || archive_valid(data,size,&s);
    if(bad)message(sock,502,"Backup download or validation failed");else attachment(sock,file,data,size);
    free(data);
}
static void import_pc(int sock,const char *query,const unsigned char *data,size_t size) {
    struct pscloud_snapshot s;
    if(selection(query,&s) || strcmp(s.slot,"WholeGame") || archive_valid(data,size,&s)) {message(sock,400,"Select Whole game and upload an unmodified PSCloud ZIP for this game and PS5 user");return;}
    unsigned char digest[32];unsigned digest_size=0;
    if(!EVP_Digest(data,size,digest,&digest_size,EVP_sha256(),NULL) || digest_size!=32) {message(sock,500,"Cannot verify ZIP checksum");return;}
    for(unsigned i=0;i<32;i++)snprintf(s.sha256+2*i,3,"%02x",digest[i]);
    char policy[16]="ask";
    if(parameter(query,"policy",policy,sizeof policy))strcpy(policy,"ask");
    if(background_active&&!strcmp(policy,"replace")) {message(sock,409,"Upload is running; import as a separate version now, or replace after it finishes");return;}
    if(strcmp(policy,"ask") && strcmp(policy,"check") && strcmp(policy,"skip") && strcmp(policy,"replace") && strcmp(policy,"new")) {message(sock,400,"Invalid duplicate policy");return;}
    char catalog[1400],matching[128]={0};struct pscloud_snapshot match={0};struct pscloud_dedup_stats stats;
    snprintf(catalog,sizeof catalog,"%s/spool",root);int catalog_fd=pscloud_open_directory(catalog);
    int duplicate=catalog_fd>=0?pscloud_snapshot_exists_checked(catalog_fd,&s,&stats):0;
    if(duplicate<0) {if(catalog_fd>=0)close(catalog_fd);message(sock,500,"Cannot safely check existing backups");return;}
    if(duplicate) {
        size_t n=strlen(stats.archive),suffix=!strcmp(stats.archive+n-6,".ready")?6:5;
        snprintf(matching,sizeof matching,"%.*s",(int)(n-suffix),stats.archive);
        char meta[144];snprintf(meta,sizeof meta,"%s.identity",matching);
        if(!valid_archive(matching) || pscloud_snapshot_read(catalog_fd,meta,&match))duplicate=0;
    }
    if(catalog_fd>=0)close(catalog_fd);
    if(!duplicate && strcmp(policy,"new")) {
        struct settings cloud={0};char folder[256],url[4096];
        if(!pscloud_configure(cloudpath,&cloud) && !pscloud_snapshot_folder(&s,folder,sizeof folder)) {
            snprintf(url,sizeof url,"%s/%s/.pscloud/",cloud.url,folder);
            char *xml=malloc(1024*1024+1);if(!xml) {message(sock,500,"Not enough memory");return;}
            struct response r={xml,0,1024*1024};long status=webdav(&cloud,url,"PROPFIND",&r);
            if(status!=404 && status!=200 && status!=207) {free(xml);message(sock,502,"Cloud duplicate check unavailable; retry later");return;}
            char *cursor=xml;unsigned checked=0;
            while((status==200 || status==207) && (cursor=strstr(cursor,"href>")) && checked<128) {
                cursor+=5;char *end=strchr(cursor,'<');if(!end)break;char *begin=cursor;
                for(char *p=cursor;p<end;p++)if(*p=='/')begin=p+1;
                size_t n=(size_t)(end-begin);if(n<10 || n-9>=128 || strncmp(end-9,".identity",9))continue;
                char candidate[128];memcpy(candidate,begin,n-9);candidate[n-9]=0;
                struct pscloud_snapshot remote;checked++;
                if(!remote_snapshot(&cloud,&s,candidate,&remote) && !strcmp(remote.sha256,s.sha256) && (!duplicate || remote.created>match.created)) {duplicate=1;match=remote;strcpy(matching,candidate);}
            }
            free(xml);
        }
    }
    if(!strcmp(policy,"check") || (duplicate && !strcmp(policy,"ask"))) {
        char json[512];snprintf(json,sizeof json,"{\"ok\":true,\"duplicate\":%s,\"file\":\"%s\",\"created\":%lld,\"sha256\":\"%s\",\"message\":\"%s\"}",duplicate?"true":"false",matching,match.created,s.sha256,duplicate?"An identical backup exists. Choose keep, replace existing, or upload separately":"No identical backup found");
        respond(sock,!strcmp(policy,"check")?200:409,"application/json",json,strlen(json));return;
    }
    if(!strcmp(policy,"skip")) {message(sock,duplicate?200:409,duplicate?"Existing backup kept; no new copy or upload queued":"Matching backup changed; check again");return;}
    if(!strcmp(policy,"replace") && !duplicate) {message(sock,409,"Matching backup changed; check again");return;}
    char expected_file[128];
    if(!parameter(query,"existing",expected_file,sizeof expected_file) && strcmp(expected_file,matching)) {message(sock,409,"Matching version changed; check again");return;}
    char expected_hash[65];
    if(!parameter(query,"expected",expected_hash,sizeof expected_hash) && strcmp(expected_hash,s.sha256)) {message(sock,409,"Selected ZIP changed; check it again");return;}
    char path[1400],id[33],file[128],part[144],ready[144],identity[144];
    snprintf(path,sizeof path,"%s/spool",root);int parent=pscloud_open_directory(root);
    if(parent>=0) {(void)mkdirat(parent,"spool",0700);close(parent);}int dir=pscloud_open_directory(path);
    if(dir<0 || pscloud_random_id(id)) {if(dir>=0)close(dir);message(sock,500,"Cannot open private backup queue");return;}
    int queue_lock=openat(dir,".worker.lock",O_CREAT | O_RDWR | O_NOFOLLOW,0600);
    if(queue_lock<0 || flock(queue_lock,LOCK_EX | LOCK_NB)) {if(queue_lock>=0)close(queue_lock);close(dir);message(sock,409,"Uploader busy; try import again shortly");return;}
    if(!strcmp(policy,"replace")) {
        if(duplicate && *matching) {
            struct pscloud_dedup_stats local;
            if(pscloud_snapshot_exists_checked(dir,&s,&local)>0) {
                int bad=pscloud_snapshot_requeue(dir,local.archive);char flag[144],name[128];size_t n=strlen(local.archive),suffix=!strcmp(local.archive+n-6,".ready")?6:5;
                snprintf(name,sizeof name,"%.*s",(int)(n-suffix),local.archive);snprintf(flag,sizeof flag,"%s.replace",name);
                if(!bad) {
                    char full[1600];snprintf(full,sizeof full,"%s/%s",path,flag);bad=atomic_config(full,s.sha256);
                }
                close(queue_lock);close(dir);message(sock,bad?500:200,bad?"Could not queue existing version":"Existing version queued for replacement; filename and date preserved");return;
            }
            strcpy(file,matching);s.created=match.created;
        }
    } else snprintf(file,sizeof file,!pscloud_share_validate(data,size,s.title)?"portable-%s-%s.zip":"ps5-11.40-%s-%s.zip",s.title,id);
    snprintf(part,sizeof part,"%s.part",file);snprintf(ready,sizeof ready,"%s.ready",file);snprintf(identity,sizeof identity,"%s.identity",file);
    int fd=openat(dir,part,O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,0600),bad=fd<0;size_t have=0;
    while(!bad && have<size) {ssize_t n=write(fd,data+have,size-have);if(n<=0)bad=1;else have+=(size_t)n;}
    if(fd>=0) {if(fsync(fd))bad=1;if(close(fd))bad=1;}
    if(!bad) {fd=openat(dir,part,O_RDONLY | O_NOFOLLOW | O_NONBLOCK);bad=fd<0 || pscloud_file_hash(fd,s.sha256);if(fd>=0)close(fd);}
    if(!bad) {
        if(strcmp(policy,"replace"))s.created=(long long)time(NULL);
        if(s.created<0)s.created=0;
        char metadata[512],full[1600];snprintf(metadata,sizeof metadata,"USER_ID=%s\nTITLE=%s\nSAVE_NAME=WholeGame\nSHA256=%s\nCREATED_UNIX=%lld\n",s.user,s.title,s.sha256,s.created);snprintf(full,sizeof full,"%s/%s",path,identity);bad=atomic_config(full,metadata);
        if(!bad && !strcmp(policy,"replace")) {char flag[144];snprintf(flag,sizeof flag,"%s.replace",file);snprintf(full,sizeof full,"%s/%s",path,flag);bad=atomic_config(full,s.sha256);}
    }
    if(!bad)bad=renameat(dir,part,dir,ready) || fsync(dir);
    if(bad)unlinkat(dir,part,0);
    close(queue_lock);
    close(dir);
    message(sock,bad?500:200,bad?"Import failed; no console save was changed":"PC backup validated and queued. Upload or restore it from the queue");
}
static void configure_cloud(int sock,const char *form) {
    struct settings s={0},previous={0};pscloud_configure(cloudpath,&previous);
    if(parameter(form,"url",s.url,sizeof s.url) || parameter(form,"username",s.user,sizeof s.user) || parameter(form,"password",s.password,sizeof s.password)) {message(sock,400,"Cloud settings missing");return;}
    if(!*s.password && !strcmp(s.url,previous.url) && !strcmp(s.user,previous.user))strcpy(s.password,previous.password);
    if(strncmp(s.url,"https://",8) || strchr(s.url,'?') || strchr(s.url,'#') || !*s.user || !*s.password) {message(sock,400,"Use an HTTPS WebDAV URL, username and app password");return;}
    size_t n=strlen(s.url);while(n>8 && s.url[n-1]=='/')s.url[--n]=0;
    strcpy(s.mode,"once");
    if(*previous.ca)strcpy(s.ca,previous.ca);else snprintf(s.ca,sizeof s.ca,"%.900s/ca.pem",root);
    char body[65537];struct response response={body,0,sizeof body-1};long status=webdav(&s,s.url,"PROPFIND",&response);
    cloud_status=(int)status;
    if(status!=207 && status!=200) {message(sock,502,"Nextcloud rejected the connection; settings were not saved");return;}
    char config[6144];snprintf(config,sizeof config,"URL=%s\nUSER=%s\nPASSWORD=%s\nCA_BUNDLE=%s\nMODE=once\n",s.url,s.user,s.password,s.ca);
    if(atomic_config(cloudpath,config)) {message(sock,500,"Could not save private cloud settings");return;}
    memset(config,0,sizeof config);memset(s.password,0,sizeof s.password);message(sock,200,"Nextcloud connected and settings saved");
}
static void action(int sock,const char *path,const char *form) {
    pscloud_log("EVENT","Dashboard action: %s",path);
    struct pscloud_snapshot chosen;char closed[8];
    if(!strcmp(path,"/api/sync-start")) {start_background(sock,form);return;}
    if(!strcmp(path,"/api/usb-copy")) {usb_copy_action(sock,form);return;}
    if(!strcmp(path,"/api/usb-copy-all")) {usb_copy_all(sock,form);return;}
    if(!strcmp(path,"/api/usb-delete")) {usb_delete(sock,form);return;}
    if(!strcmp(path,"/api/cleanup")) {cleanup_apply(sock,form);return;}
    if(!strcmp(path,"/api/encryption")) {
        char password[300]={0},again[300]={0},path_conf[1400];snprintf(path_conf,sizeof path_conf,"%s/encrypt.conf",root);
        (void)parameter(form,"password",password,sizeof password);(void)parameter(form,"again",again,sizeof again);
        if(!*password) {unlink(path_conf);pscloud_encryption_set(NULL);pscloud_log("EVENT","Cloud encryption turned off for new uploads");message(sock,200,"Encryption is off for new uploads. Encrypted cloud copies still need the password to restore");return;}
        if(strlen(password)<8||strlen(password)>200||strchr(password,'\n')||strcmp(password,again)) {memset(password,0,sizeof password);memset(again,0,sizeof again);message(sock,400,"Use at least 8 characters and type the same password twice");return;}
        char text[320];snprintf(text,sizeof text,"PASSWORD=%s\n",password);int bad=atomic_config(path_conf,text);
        if(!bad)pscloud_encryption_set(password);
        memset(text,0,sizeof text);memset(password,0,sizeof password);memset(again,0,sizeof again);
        if(bad) {message(sock,500,"Password could not be saved");return;}
        pscloud_log("EVENT","Cloud encryption turned on for new uploads");message(sock,200,"New uploads will be encrypted with your password");return;
    }
    if(!strcmp(path,"/api/cloud-keep")) {
        char keep[8];if(parameter(form,"keep",keep,sizeof keep)) {message(sock,400,"Choose how many versions to keep");return;}
        int k=atoi(keep);if(k<0||k>999||(k==0&&strcmp(keep,"0"))) {message(sock,400,"Choose how many versions to keep");return;}
        char path_conf[1400],text[32];snprintf(path_conf,sizeof path_conf,"%s/cloud-keep.conf",root);snprintf(text,sizeof text,"KEEP=%d\n",k);
        if(atomic_config(path_conf,text)) {message(sock,500,"Setting could not be saved");return;}
        cloud_keep=k;message(sock,200,k?"Older cloud versions will be removed after each upload":"All cloud versions are kept");return;
    }
    if(!strcmp(path,"/api/peers")) {
        char list[600]={0},copy[600],text[640]={0};size_t n=0;(void)parameter(form,"peers",list,sizeof list);snprintf(copy,sizeof copy,"%s",list);
        for(char *save=NULL,*tok=strtok_r(copy,",",&save);tok;tok=strtok_r(NULL,",",&save)) {
            if(!peer_address_valid(tok)) {message(sock,400,"Use the other PS5's IP address and port, like 192.168.0.198:8082");return;}
            n+=(size_t)snprintf(text+n,sizeof text-n,"%s\n",tok);
        }
        char path_conf[1400];snprintf(path_conf,sizeof path_conf,"%s/peers.conf",root);
        if(atomic_config(path_conf,text)) {message(sock,500,"Could not save");return;}
        message(sock,200,*list?"Saved the PS5s on your network":"No other PS5s on your network");return;
    }
    if(!strcmp(path,"/api/links")) {
        char user[32],list[200]={0};if(parameter(form,"user",user,sizeof user)||!safe_word(user,16)) {message(sock,400,"Choose a profile");return;}
        if(!parameter(form,"linked",list,sizeof list)&&*list) {char copy[200];snprintf(copy,sizeof copy,"%s",list);
            for(char *save=NULL,*tok=strtok_r(copy,",",&save);tok;tok=strtok_r(NULL,",",&save))if(!safe_word(tok,16)) {message(sock,400,"Invalid profile link");return;}}
        if(links_save(user,list)) {message(sock,500,"Link could not be saved");return;}
        message(sock,200,*list?"Profiles linked":"Link removed");return;
    }
    if(!strcmp(path,"/api/backup-all-start")) {
        char user[32]={0};int one=!parameter(form,"user",user,sizeof user);
        if(one&&!safe_word(user,16)) {message(sock,400,"Invalid profile");return;}
        int r=batch_start(one?user:NULL);
        message(sock,r>0?409:r<0?400:200,r>0?"Back up all games is already running":r<0?"No games with saves were found":"Backing up all games");return;
    }
    if(!strcmp(path,"/api/schedule")) {
        char hour[8];if(parameter(form,"hour",hour,sizeof hour)) {message(sock,400,"Choose an hour");return;}
        int h=atoi(hour);if(h<-1||h>23||(h==0&&strcmp(hour,"0"))) {message(sock,400,"Choose an hour");return;}
        schedule_hour=h;if(schedule_save()) {message(sock,500,"Schedule could not be saved");return;}
        pscloud_log("EVENT","Nightly backup %s",h<0?"off":"scheduled");message(sock,200,h<0?"Nightly backup is off":"Nightly backup scheduled");return;
    }
    if(!strcmp(path,"/api/game-settings")) {
        char title[16],close_value[8],all_value[8];
        if(parameter(form,"title",title,sizeof title)||!pscloud_title_valid(title)||parameter(form,"close_backup",close_value,sizeof close_value)||parameter(form,"backup_all",all_value,sizeof all_value)) {message(sock,400,"Invalid game settings");return;}
        char sync_value[8]="0",apply_value[8]="0";(void)parameter(form,"auto_sync",sync_value,sizeof sync_value);(void)parameter(form,"auto_apply",apply_value,sizeof apply_value);
        if(game_settings_save(title,!strcmp(close_value,"1"),!strcmp(all_value,"1"),!strcmp(sync_value,"1"),!strcmp(apply_value,"1"))) {message(sock,500,"Game settings could not be saved");return;}
        message(sock,200,"Game settings saved");return;
    }
    if(!strcmp(path,"/api/sync")) {int result=upload_queue();message(sock,result?502:200,result?"Upload pending; local backups retained":"Queued backups uploaded");return;}
    if(!strcmp(path,"/api/sync-one")) {
        char file[128],ready[144];struct pscloud_snapshot s;
        if(parameter(form,"file",file,sizeof file) || pending_archive(file,&s,NULL,NULL)) {message(sock,400,"Selected pending backup unavailable");return;}
        snprintf(ready,sizeof ready,"%s.ready",file);int result=upload_selected(ready);
        message(sock,result?502:200,result?"Selected upload pending; local backup retained":"Selected backup available in cloud");return;
    }
    if(selection(form,&chosen)) {message(sock,400,"Unsupported save selection");return;}
    if(!strcmp(path,"/api/usb-save-game")) {usb_save_game(sock,form,&chosen);return;}
    if(!strcmp(path,"/api/cloud-prune")) {
        char keep[8];int k=cloud_keep;if(!parameter(form,"keep",keep,sizeof keep))k=atoi(keep);
        if(k<=0) {message(sock,400,"Choose how many versions to keep first");return;}
        if(background_active) {message(sock,409,"Wait for the upload to finish");return;}
        struct settings cloud={0};if(pscloud_configure(cloudpath,&cloud)) {message(sock,400,"Connect your cloud first");return;}
        int removed=cloud_prune(&cloud,&chosen,k);memset(cloud.password,0,sizeof cloud.password);
        if(removed<0) {message(sock,502,"Cloud cleanup failed; nothing on this PS5 changed");return;}
        char text[160];snprintf(text,sizeof text,removed?"Removed %d older cloud versions":"Nothing to remove (%d)",removed);message(sock,200,text);return;
    }
    if(!strcmp(path,"/api/sync-push")) {
        char closed[8],replace[8]="0",peers[8][64],linked[4][17];(void)parameter(form,"replace",replace,sizeof replace);
        if(parameter(form,"closed",closed,sizeof closed)||strcmp(closed,"yes")) {message(sock,400,"Close the game and switch on Game closed first");return;}
        int np=peers_list(peers),nl=links_for(chosen.user,linked);
        if(!np||!nl) {message(sock,400,"Add your other PS5 under Other PS5s on your network and link your profile first");return;}
        char published[144];int unchanged=0;pscloud_log_open(logpath);
        if(backup_one(chosen.user,chosen.title,published,&unchanged)) {message(sock,500,"Could not back up this game first; check Activity");return;}
        char result[600]="",last[600]="No linked profile was found on your other PS5";int sent=0;
        for(int i=0;i<np&&!sent;i++)for(int k=0;k<nl&&!sent;k++) {
            char info[8192];size_t got=0;if(peer_get(peers[i],"/api/peer/info",info,sizeof info-1,&got,5)!=200) {snprintf(last,sizeof last,"Your other PS5 at %s is not reachable",peers[i]);continue;}
            char needle[40];snprintf(needle,sizeof needle,"\"user\":\"%.16s\"",linked[k]);if(!strstr(info,needle))continue;
            CURL *c=curl_easy_init();if(!c)continue;char url_push[600],body[1024];struct peer_body r={body,0,sizeof body-1};
            snprintf(url_push,sizeof url_push,"http://%.63s/api/peer/push?from=%.16s&to=%.16s&title=%.9s&replace=%s",peers[i],chosen.user,linked[k],chosen.title,!strcmp(replace,"1")?"1":"0");
            curl_easy_setopt(c,CURLOPT_URL,url_push);curl_easy_setopt(c,CURLOPT_PROTOCOLS_STR,"http");curl_easy_setopt(c,CURLOPT_POST,1L);curl_easy_setopt(c,CURLOPT_POSTFIELDS,"");
            curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,4L);curl_easy_setopt(c,CURLOPT_TIMEOUT,600L);curl_easy_setopt(c,CURLOPT_NOSIGNAL,1L);
            curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,peer_collect);curl_easy_setopt(c,CURLOPT_WRITEDATA,&r);
            CURLcode rc=curl_easy_perform(c);long status=0;curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);curl_easy_cleanup(c);body[r.size]=0;
            char text[500]="";json_text(body,"message",text,sizeof text);
            if(rc==CURLE_OK&&status==200) {snprintf(result,sizeof result,"%s",*text?text:"Sent to your other PS5");sent=1;}
            else snprintf(last,sizeof last,"%s",*text?text:"Your other PS5 did not answer");
        }
        message(sock,sent?200:409,sent?result:last);return;
    }
    if(!strcmp(path,"/api/remote")) {
        char allow[8];if(parameter(form,"allow_replace",allow,sizeof allow)||(strcmp(allow,"0")&&strcmp(allow,"1"))) {message(sock,400,"Invalid option");return;}
        char path_conf[1400],text[32];snprintf(path_conf,sizeof path_conf,"%s/remote.conf",root);snprintf(text,sizeof text,"ALLOW_REPLACE=%s\n",allow);
        if(atomic_config(path_conf,text)) {message(sock,500,"Could not save");return;}
        message(sock,200,!strcmp(allow,"1")?"Your other PS5 can now replace saves here (a rollback copy is always kept)":"Your other PS5 can only send saves for you to replace");return;
    }
    if(!strcmp(path,"/api/sync-get")) {
        char closed[8],file[128],sha[65],why[300];
        if(parameter(form,"closed",closed,sizeof closed)||strcmp(closed,"yes")) {message(sock,400,"Close the game and switch on Game closed first");return;}
        if(sync_fetch(chosen.user,chosen.title,file,sha,why,sizeof why)) {message(sock,400,why);return;}
        char json[400];snprintf(json,sizeof json,"{\"ok\":true,\"file\":\"%s\",\"sha256\":\"%s\",\"message\":\"Newer save downloaded and checked. Your save is not changed yet\"}",file,sha);
        respond(sock,200,"application/json",json,strlen(json));return;
    }
    if(!strncmp(path,"/api/share-",11)) {
        char file[128]={0},sha[65]={0},confirm[8],expected[65],directory[1400];
        if(parameter(form,"closed",closed,sizeof closed)||strcmp(closed,"yes")) {message(sock,400,"Close the selected game and confirm before sharing");return;}
        int exporting=!strcmp(path,"/api/share-export"),checking=!strcmp(path,"/api/share-check"),restoring=!strcmp(path,"/api/share-restore");
        if(!exporting&&!checking&&!restoring) {message(sock,404,"Unknown sharing action");return;}
        unsigned char *data=NULL;size_t size=0;
        snprintf(directory,sizeof directory,"%s/share",root);int dir=pscloud_open_directory(directory);
        if(!exporting) {
            if(parameter(form,"file",file,sizeof file)||strncmp(file,"portable-",9)||strchr(file,'/')||strchr(file,'\\')||
               parameter(form,"sha256",expected,sizeof expected)||strlen(expected)!=64||dir<0||pscloud_read_archive(dir,file,&data,&size)||pscloud_verify_hash(data,size,expected)) {free(data);if(dir>=0)close(dir);message(sock,400,"Selected shared package changed or is unavailable");return;}
        }
        if(restoring) {
            char proof[240],checked[65]={0};snprintf(proof,sizeof proof,"%s.check-%s",file,chosen.user);
            int fd=openat(dir,proof,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);int valid=fd>=0&&read(fd,checked,64)==64&&!strcmp(checked,expected);if(fd>=0)close(fd);
            if(!valid||parameter(form,"confirm",confirm,sizeof confirm)||strcmp(confirm,"yes")) {free(data);close(dir);message(sock,400,"Run the staged check for this profile and confirm live replacement first");return;}
        }
        char published[128],only[1024]={0},flag[8];int has_slots=exporting&&strstr(form,"slots=");
        if(has_slots&&parameter(form,"slots",only,sizeof only)) {free(data);if(dir>=0)close(dir);message(sock,400,"Invalid slot selection");return;}
        struct pscloud_share_options options={.only=only,.system_root=system_root};
        if(!exporting) {
            options.allow_version=!parameter(form,"allow_version",flag,sizeof flag)&&!strcmp(flag,"yes");
            options.skip_missing=!parameter(form,"skip_missing",flag,sizeof flag)&&!strcmp(flag,"yes");
            options.create_missing=!parameter(form,"create_missing",flag,sizeof flag)&&!strcmp(flag,"yes");
            options.not_installed=!parameter(form,"not_installed",flag,sizeof flag)&&!strcmp(flag,"yes");
        }
        int bad=pscloud_share_game(home,root,appmeta,chosen.user,chosen.title,exporting?0:checking?1:2,data,size,&options,published,sha);free(data);
        if(!bad&&restoring)sync_after_restore(chosen.user,chosen.title,file);
        if(!bad&&checking) {char proof[240],proofpath[1800];snprintf(proof,sizeof proof,"%s.check-%s",file,chosen.user);snprintf(proofpath,sizeof proofpath,"%s/%s",directory,proof);bad=atomic_config(proofpath,sha);}
        if(dir>=0)close(dir);
        if(bad) {
            char reason[300];const char *why=pscloud_share_error();
            snprintf(reason,sizeof reason,"Sharing refused: %s",*why?why:"see Activity for details. Live restores retain rollback");
            message(sock,400,reason);return;
        }
        if(exporting)prune_shared_keep(2);
        char json[640];snprintf(json,sizeof json,"{\"ok\":true,\"file\":\"%s\",\"sha256\":\"%s\",\"message\":\"%s\"}",exporting?published:file,sha,exporting?"Shared package ready to download; live saves unchanged":checking?"Staged check passed; live saves unchanged. In-game compatibility is not guaranteed":"Shared save restored; rollback copies retained. Check progress in-game");respond(sock,200,"application/json",json,strlen(json));return;
    }
    if(!strcmp(path,"/api/delete-cloud")) {
        char file[128],confirm[8],folder[256],url[4096];struct settings s={0};struct pscloud_snapshot identity;
        if(background_active) {message(sock,409,"Wait for upload completion before deleting a cloud backup");return;}
        if(parameter(form,"confirm",confirm,sizeof confirm)||strcmp(confirm,"yes")||parameter(form,"file",file,sizeof file)||
            pscloud_configure(cloudpath,&s)||remote_snapshot(&s,&chosen,file,&identity)||pscloud_snapshot_folder(&identity,folder,sizeof folder)) {message(sock,400,"Confirm deletion of a verified cloud backup");return;}
        snprintf(url,sizeof url,"%s/%s/%s",s.url,folder,file);long status=webdav(&s,url,"DELETE",NULL);
        if(status!=404&&(status<200||status>=300)) {message(sock,502,"Cloud deletion failed; local backups and PS5 saves untouched");return;}
        snprintf(url,sizeof url,"%s/%s/.pscloud/%s.identity",s.url,folder,file);status=webdav(&s,url,"DELETE",NULL);
        message(sock,status==404||(status>=200&&status<300)?200:502,status==404||(status>=200&&status<300)?"Cloud backup deleted; local copies and PS5 saves unchanged":"Cloud archive deleted; cloud metadata cleanup failed");return;
    }
    if(!strcmp(path,"/api/backup") || !strcmp(path,"/api/backup-start")) {
        int background=!strcmp(path,"/api/backup-start");
        if(parameter(form,"closed",closed,sizeof closed) || strcmp(closed,"yes")) {message(sock,400,"Close the game and confirm before backup");return;}
        if(!strcmp(chosen.slot,"WholeGame")) {
            pscloud_log_open(logpath);
            if(compact_effective()) {
                char published[144];
                if(compact_backup(&chosen,published)) {message(sock,500,"Compact backup failed; live saves untouched. Check Activity for metadata, slot, version or mount errors");return;}
                if(!auto_upload) {message(sock,200,"Compact game-data backup saved in local queue; automatic upload is off");return;}
                upload_created(sock,published,background);return;
            }
            char published[144];
            if(pscloud_game_backup_named(home,root,chosen.user,chosen.title,published)) {message(sock,500,"Whole-game backup failed; originals untouched. Check Activity for invalid/changing files, slot or size limits, or an active restore marker");return;}
            if(!auto_upload) {message(sock,200,"Whole-game backup saved in local queue; automatic upload is off");return;}
            upload_created(sock,published,background);return;
        }
        char config[512],file[1200];snprintf(file,sizeof file,"%s/dashboard-backup.conf",root);
        snprintf(config,sizeof config,"USER_ID=%s\nTITLE=%s\nSAVE_NAME=%s\nCONFIRM_GAME_CLOSED=yes\n",chosen.user,chosen.title,chosen.slot);
        if(atomic_config(file,config)) {message(sock,500,"Backup selection could not be saved");return;}
        char *args[]={"backup",file,home,root,logpath,NULL};
#ifdef PSCLOUD_HOST_TEST
        int result=pscloud_backup_main(5,args);
#else
        /* Console entrypoint reads its standard config; copy only validated fields. */
        int result=atomic_config("/data/pscloud/backup.conf",config)?2:pscloud_backup_main(1,args);
#endif
        pscloud_log_open(logpath);
        if(result) {message(sock,500,"Backup failed; inspect log. Original save was not replaced");return;}
        if(!auto_upload) {message(sock,200,"Backup saved in local queue; automatic upload is off");return;}
        upload_created(sock,pscloud_backup_archive(),background);return;
    }
    if(!strcmp(path,"/api/restore-local")) {
        char file[128],confirm[8];struct pscloud_snapshot snapshot;unsigned char *data=NULL;size_t size=0;
        if(parameter(form,"closed",closed,sizeof closed) || strcmp(closed,"yes") || parameter(form,"confirm",confirm,sizeof confirm) || strcmp(confirm,"yes") ||
           parameter(form,"file",file,sizeof file) || pending_archive(file,&snapshot,&data,&size)) {free(data);message(sock,400,"Select a valid queued backup and confirm the game is closed");return;}
        int bad=strcmp(snapshot.user,chosen.user) || strcmp(snapshot.title,chosen.title) || strcmp(snapshot.slot,chosen.slot) || strcmp(snapshot.slot,"WholeGame");
        char skip[8];int smart=!parameter(form,"skip_identical",skip,sizeof skip)&&!strcmp(skip,"yes");
        if(!bad)bad=restore_game_data(&snapshot,data,size,0,smart);
        free(data);
        message(sock,bad&&bad!=5?500:200,bad==5?"Save already matches this backup; no save changes or mounts needed":bad==2?"This game's keys changed; recreated-save recovery is only available for the Crash UE4 format":bad==4?"Backup and current save slots differ. No saves changed; preserve the backup and do not delete other slots":bad==3?"Restore metadata could not confirm game, slot and account identity":bad?"Restore refused or failed. Keep game closed; inspect activity and rollback":"Whole-game restore complete; original images retained in rollback");return;
    }
    char file[128];struct settings cloud={0};struct pscloud_snapshot snapshot;
    int checking=!strcmp(path,"/api/restore-check");
    int restoring=!strcmp(path,"/api/restore") || checking;
    if(parameter(form,"file",file,sizeof file) || !valid_archive(file)) {message(sock,400,"Invalid backup selection");return;}
    unsigned char *local_data=NULL;size_t local_size=0;char expected_sha[65]={0};
    int local=restoring && !strcmp(chosen.slot,"WholeGame") && !parameter(form,"sha256",expected_sha,sizeof expected_sha) &&
        !local_restore_identity(file,&chosen,expected_sha,&snapshot) && !local_restore_copy(file,&snapshot,&local_data,&local_size);
    if(!local && (pscloud_configure(cloudpath,&cloud) || remote_snapshot(&cloud,&chosen,file,&snapshot))) {message(sock,400,"Cloud backup identity could not be verified and no matching verified local copy was available");return;}
    if(*expected_sha && strcmp(expected_sha,snapshot.sha256)) {free(local_data);message(sock,409,"Selected cloud version changed; refresh versions before restoring");return;}
    if(checking && strcmp(chosen.slot,"WholeGame")) {message(sock,400,"Staged recovery check only supports whole-game archives");return;}
    if(!strcmp(chosen.slot,"WholeGame")) {
        if(!restoring && strcmp(path,"/api/download")) {message(sock,404,"Unknown action");return;}
        char folder[256],url[4096];pscloud_snapshot_folder(&snapshot,folder,sizeof folder);snprintf(url,sizeof url,"%s/%s/%s",cloud.url,folder,file);
        unsigned char *data=local_data;size_t size=local_size;
        if(!data && restoring)(void)local_restore_copy(file,&snapshot,&data,&size);
        int bad=0;
        if(!data) {
            data=malloc(PSCLOUD_RESTORE_MAX+1U);if(!data) {message(sock,500,"Not enough memory");return;}
            struct response r={(char *)data,0,PSCLOUD_RESTORE_MAX};bad=webdav(&cloud,url,"GET",&r)!=200;size=r.size;
            if(!bad) {int sealed=pscloud_decrypt_if_needed(data,&size);if(sealed) {free(data);message(sock,400,sealed==-2?"This backup is encrypted. Enter your encryption password in Cloud settings":"Wrong encryption password, or the cloud copy is damaged");return;}}
            if(!bad)bad=pscloud_verify_hash(data,size,snapshot.sha256) || archive_valid(data,size,&snapshot);
        }
        if(!bad && restoring) {
            char confirm[8];
            if(parameter(form,"closed",closed,sizeof closed) || strcmp(closed,"yes") || parameter(form,"confirm",confirm,sizeof confirm) || strcmp(confirm,"yes"))bad=1;
            else {char skip[8];int smart=!parameter(form,"skip_identical",skip,sizeof skip)&&!strcmp(skip,"yes");bad=restore_game_data(&snapshot,data,size,checking,smart);}
        }
        if(!restoring && !bad) {
            char path[1400],part[144];snprintf(path,sizeof path,"%s/downloads",root);int parent=pscloud_open_directory(root);if(parent>=0) {(void)mkdirat(parent,"downloads",0700);close(parent);}int dir=pscloud_open_directory(path);
            snprintf(part,sizeof part,"%s.part",file);
            bad=dir<0 || store_blob(dir,part,data,size);
            if(!bad)bad=renameat(dir,part,dir,file) || fsync(dir);
            if(dir>=0) {if(bad)unlinkat(dir,part,0);close(dir);}
        }
        free(data);message(sock,bad&&bad!=5?500:200,bad==5?"Save already matches this backup; no save changes or mounts needed":bad==4?"Backup and current save slots differ. No saves changed; preserve the backup and do not delete other slots":bad==2?"This game's keys changed; recreated-save recovery is only available for the Crash UE4 format":bad==3?"Recovery metadata could not confirm matching game, save slot and account. No live save was replaced":bad?"Whole-game operation failed; keep game closed if restoring and inspect activity":checking?"Recovery staged check passed; all live saves untouched":restoring?"Whole-game restore complete; rollback retained":"Whole-game ZIP downloaded and verified on PS5");return;
    }
    if(restoring) {
        char confirm[8];
        if(parameter(form,"closed",closed,sizeof closed) || strcmp(closed,"yes") || parameter(form,"confirm",confirm,sizeof confirm) || strcmp(confirm,"yes")) {message(sock,400,"Restore requires game-closed and replacement confirmation");return;}
    } else if(strcmp(path,"/api/download")) {message(sock,404,"Unknown action");return;}
    char config[512],selection_path[1200],downloads[1200];
    snprintf(selection_path,sizeof selection_path,"%s/dashboard-download.conf",root);snprintf(downloads,sizeof downloads,"%s/downloads",root);
    snprintf(config,sizeof config,"BACKUP=%s\nSHA256=%s\nTITLE=%s\nUSER_ID=%s\nSAVE_NAME=%s\n",file,snapshot.sha256,snapshot.title,snapshot.user,snapshot.slot);
    if(atomic_config(selection_path,config)) {message(sock,500,"Download selection failed");return;}
    int parent=pscloud_open_directory(root);if(parent>=0) {(void)mkdirat(parent,"downloads",0700);close(parent);}
    char *args[]={"download",cloudpath,selection_path,downloads,logpath,NULL};
#ifdef PSCLOUD_HOST_TEST
    int result=pscloud_download_main(5,args);
#else
    int result=atomic_config("/data/pscloud/download.conf",config)?2:pscloud_download_main(1,args);
#endif
    pscloud_log_open(logpath);
    if(result) {message(sock,502,"Cloud download or checksum validation failed");return;}
    if(!restoring) {message(sock,200,"Downloaded to PS5 and SHA-256 verified");return;}
    result=pscloud_managed_restore(home,root,&snapshot,file);
    message(sock,result?500:200,result?"Restore failed; preserve rollback and inspect log":"Restore complete. Encrypted rollback retained; launch game to verify progress");
}
static int header_value(const char *headers,const char *name,char *out,size_t max) {
    size_t k=strlen(name);int found=0;*out=0;const char *line=strstr(headers,"\r\n");
    while(line && line[2] && line[2]!='\r') {
        line+=2;const char *end=strstr(line,"\r\n");if(!end)return -1;
        if((size_t)(end-line)>k && !strncasecmp(line,name,k) && line[k]==':') {
            const char *value=line+k+1;while(value<end && *value==' ')value++;
            size_t n=(size_t)(end-value);if(found || n>=max)return -1;
            memcpy(out,value,n);out[n]=0;found=1;
        }
        line=end;
    }
    return found?0:-1;
}
static int session(const char *request) {
    char cookies[2048];if(header_value(request,"Cookie",cookies,sizeof cookies))return 0;
    char *state=NULL,*part=strtok_r(cookies,";",&state);
    while(part) {while(*part==' ')part++;if(!strncmp(part,"PSCloudSession=",15) && !strcmp(part+15,token))return 1;part=strtok_r(NULL,";",&state);}
    return 0;
}
static void game_icon(int sock,const char *query) {
    char title[10],path[1400];
    if(parameter(query,"title",title,sizeof title) || !pscloud_title_valid(title)) {reject(sock,400,"Invalid game ID");return;}
    int dir=pscloud_app_meta_dir(appmeta,title,path,sizeof path)?-1:pscloud_open_directory(path);
    int fd=dir>=0?openat(dir,"icon0.png",O_RDONLY | O_NOFOLLOW | O_NONBLOCK):-1;if(dir>=0)close(dir);
    struct stat st;if(fd<0 || fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size<8 || st.st_size>4*1024*1024) {if(fd>=0)close(fd);reject(sock,404,"Installed game icon unavailable");return;}
    unsigned char *data=malloc((size_t)st.st_size);if(!data) {close(fd);reject(sock,500,"Out of memory");return;}
    size_t have=0;while(have<(size_t)st.st_size) {ssize_t n=read(fd,data+have,(size_t)st.st_size-have);if(n<=0)break;have+=(size_t)n;}
    close(fd);
    if(have!=(size_t)st.st_size || memcmp(data,"\x89PNG\r\n\x1a\n",8))reject(sock,404,"Invalid installed icon");
    else respond(sock,200,"image/png",(const char *)data,have);
    free(data);
}
/* The small picture the PS5 shows for one save slot. */
static void slot_icon(int sock,const char *query) {
    char user[32],title[16],slot[80],path[1600];
    if(parameter(query,"user",user,sizeof user)||!safe_word(user,16)||parameter(query,"title",title,sizeof title)||!pscloud_title_valid(title)||parameter(query,"slot",slot,sizeof slot)||!safe_word(slot,63)) {reject(sock,400,"Invalid slot");return;}
    snprintf(path,sizeof path,"%s/%s/savedata_prospero_meta/user/%s/%s_icon0.png",home,user,title,slot);
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);struct stat st;
    if(fd<0||fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<8||st.st_size>2*1024*1024) {if(fd>=0)close(fd);reject(sock,404,"No slot picture");return;}
    unsigned char *data=malloc((size_t)st.st_size);size_t have=0;
    while(data&&have<(size_t)st.st_size) {ssize_t n=read(fd,data+have,(size_t)st.st_size-have);if(n<=0)break;have+=(size_t)n;}
    close(fd);
    if(!data||have!=(size_t)st.st_size||memcmp(data,"\x89PNG\r\n\x1a\n",8))reject(sock,404,"No slot picture");else respond(sock,200,"image/png",(const char *)data,have);
    free(data);
}
static void google_action(int sock,const char *url,const char *body) {
    if(!strcmp(url,"/api/google/status")) {
        char json[256];snprintf(json,sizeof json,"{\"configured\":%s,\"authorized\":%s,\"pending\":%s,\"transfers_available\":false}",*google.client?"true":"false",*google.refresh?"true":"false",google_pending.expires>time(NULL)?"true":"false");
        respond(sock,200,"application/json",json,strlen(json));return;
    }
    struct settings cloud={0};
    /* Reuse the existing trusted CA path, never disable certificate checks. */
    char ca[1200];snprintf(ca,sizeof ca,
#ifdef PSCLOUD_HOST_TEST
        "%s/ca.pem",root
#else
        "/data/pscloud/ca.pem"
#endif
    );
    if(!pscloud_configure(cloudpath,&cloud))snprintf(ca,sizeof ca,"%s",cloud.ca);
    if(!strcmp(url,"/api/google/begin")) {
        struct pscloud_google next={0};char secret[256];
        if(parameter(body,"client_id",next.client,sizeof next.client) || !*next.client)strcpy(next.client,google.client);
        if(!parameter(body,"client_secret",secret,sizeof secret) && *secret)strcpy(next.secret,secret);
        else if(!strcmp(next.client,google.client))strcpy(next.secret,google.secret);
        if(pscloud_google_begin(&next,ca)) {message(sock,502,"Google sign-in could not start. Check the registered TV/device OAuth client and trusted CA bundle");return;}
        /* Do not discard an existing saved authorization until approval succeeds. */
        google_pending=next;
        char code[128],json[512];escaped(code,sizeof code,google_pending.user_code);
        snprintf(json,sizeof json,"{\"verification_url\":\"https://www.google.com/device\",\"user_code\":\"%s\",\"interval\":%ld,\"expires_in\":%ld}",code,google_pending.interval,google_pending.expires-(long)time(NULL));
        respond(sock,200,"application/json",json,strlen(json));return;
    }
    if(!strcmp(url,"/api/google/poll")) {
        int result=pscloud_google_poll(&google_pending,ca);
        if(result==0) {
            if(pscloud_google_save(googlepath,&google_pending)) {message(sock,500,"Google approved access but connection storage failed; retry sign-in");return;}
            google=google_pending;memset(&google_pending,0,sizeof google_pending);
        }
        if(result<0) {message(sock,502,"Google sign-in check failed; retry without changing your existing cloud provider");return;}
        const char *json=result==0?"{\"authorized\":true,\"pending\":false,\"transfers_available\":false}":result==1?"{\"authorized\":false,\"pending\":true}":"{\"authorized\":false,\"pending\":false,\"expired\":true}";
        respond(sock,200,"application/json",json,strlen(json));return;
    }
    message(sock,404,"Unknown Google sign-in endpoint");
}
static void serve(int sock,int *locked) {
    char request[16385];size_t size=0;char *body=NULL;
    while(size<8192) {
        ssize_t n=recv(sock,request+size,8192-size,0);if(n<=0)return;size+=(size_t)n;request[size]=0;
        body=strstr(request,"\r\n\r\n");if(body)break;
    }
    if(!body) {reject(sock,400,"Request headers too large");return;}
    size_t headers=(size_t)(body-request)+4;body+=4;
    char method[8],url[2048];if(sscanf(request,"%7s %2047s",method,url)!=2) {reject(sock,400,"Malformed request");return;}
    struct sockaddr_in local={0};socklen_t local_size=sizeof local;char ip[INET_ADDRSTRLEN],host[256],expected_host[64];
    if(getsockname(sock,(struct sockaddr *)&local,&local_size) || !inet_ntop(AF_INET,&local.sin_addr,ip,sizeof ip) || header_value(request,"Host",host,sizeof host)) {reject(sock,400,"Invalid dashboard host");return;}
    snprintf(expected_host,sizeof expected_host,"%s:%u",ip,ntohs(local.sin_port));
    if(strcmp(host,expected_host) && !(ntohs(local.sin_port)==80 && !strcmp(host,ip))) {reject(sock,403,"Open the dashboard using the console IP address");return;}
    if(!strcmp(method,"GET") && !strcmp(url,"/")) {respond(sock,200,"text/html; charset=utf-8",(const char *)pscloud_ui,pscloud_ui_size);return;}
    /* Read-only, for other consoles running PSCloud on this network. */
    if(!strcmp(method,"GET") && !strcmp(url,"/api/peer/info")) {peer_info_json(sock);return;}
    if(!strcmp(method,"GET") && !strncmp(url,"/api/peer/latest?",17)) {peer_latest_json(sock,url+17);return;}
    if(!strcmp(method,"GET") && !strncmp(url,"/api/peer/file?",15)) {peer_file(sock,url+15);return;}
    if(!strcmp(method,"GET") && !strncmp(url,"/api/peer/slots?",16)) {peer_slots_json(sock,url+16);return;}
    if(!strcmp(method,"POST") && !strncmp(url,"/api/peer/backup?",17)) {
        struct sockaddr_in remote={0};socklen_t remote_size=sizeof remote;char from_ip[INET_ADDRSTRLEN]="";
        if(getpeername(sock,(struct sockaddr *)&remote,&remote_size)||!inet_ntop(AF_INET,&remote.sin_addr,from_ip,sizeof from_ip)) {reject(sock,400,"Unknown sender");return;}
        peer_backup(sock,url+17,from_ip);return;
    }
    if(!strcmp(method,"POST") && !strncmp(url,"/api/peer/push?",15)) {
        struct sockaddr_in remote={0};socklen_t remote_size=sizeof remote;char from_ip[INET_ADDRSTRLEN]="";
        if(getpeername(sock,(struct sockaddr *)&remote,&remote_size)||!inet_ntop(AF_INET,&remote.sin_addr,from_ip,sizeof from_ip)) {reject(sock,400,"Unknown sender");return;}
        peer_push(sock,url+15,from_ip);return;
    }
    char supplied[64];
    int legacy=header_value(request,"X-PSCloud-Token",supplied,sizeof supplied)==0 && !strcmp(supplied,token);
    if(!legacy && !session(request)) {reject(sock,401,"Reload the dashboard to refresh your browser session");return;}
    if(!strcmp(method,"POST") && !legacy) {
        char marker[8],origin[2048],host[256],expected_origin[320];
        if(header_value(request,"X-PSCloud-Request",marker,sizeof marker) || strcmp(marker,"1")) {reject(sock,403,"Cross-site request rejected");return;}
        if(!header_value(request,"Origin",origin,sizeof origin)) {
            if(header_value(request,"Host",host,sizeof host)) {reject(sock,403,"Missing host");return;}
            snprintf(expected_origin,sizeof expected_origin,"http://%s",host);
            if(strcmp(origin,expected_origin)) {reject(sock,403,"Cross-site request rejected");return;}
        }
    }
    if(!strcmp(method,"GET") && !strcmp(url,"/api/transfer")) {
        struct pscloud_transfer_status s;pscloud_worker_status(&s);char json[4096],name[1536],file[1536];
        pthread_mutex_lock(&cache_mutex);escaped(name,sizeof name,background_name);pthread_mutex_unlock(&cache_mutex);escaped(file,sizeof file,s.file);
        snprintf(json,sizeof json,"{\"sequence\":%u,\"phase\":%d,\"failed_phase\":%d,\"done\":%llu,\"total\":%llu,\"http\":%ld,\"transport\":%d,\"timeout_seconds\":%ld,\"active\":%s,\"result\":%d,\"cancelled\":%s,\"name\":\"%s\",\"file\":\"%s\"}",s.sequence,s.phase,s.failed_phase,s.done,s.total,s.http,s.transport,pscloud_transfer_timeout(s.total),background_active?"true":"false",background_result,background_cancelled?"true":"false",name,file);
        respond(sock,200,"application/json",json,strlen(json));return;
    }
    if(!strcmp(method,"POST") && !strcmp(url,"/api/sync-cancel")) {
        if(background_active) {background_cancelled=1;pscloud_worker_cancel();}
        message(sock,200,"Upload cancellation requested; local backups retained");return;
    }
    if(!strcmp(method,"GET") && !strcmp(url,"/api/health")) {
        int available=pthread_mutex_trylock(&operation_mutex)==0;
        if(available)pthread_mutex_unlock(&operation_mutex);
        const char *json=background_active?(available?"{\"busy\":false,\"background_upload\":true}":"{\"busy\":true,\"background_upload\":true}"):available?"{\"busy\":false}":"{\"busy\":true}";
        respond(sock,200,"application/json",json,strlen(json));return;
    }
    /* These read-only resources do not share mutable transfer buffers. */
    if(!strcmp(method,"GET") && !strncmp(url,"/api/icon?",10)) {game_icon(sock,url+10);return;}
    if(!strcmp(method,"GET") && !strcmp(url,"/api/preferences")) {preferences(sock,NULL);return;}
    if(!strcmp(method,"GET") && !strcmp(url,"/api/backup-all")) {batch_json(sock);return;}
    if(!strcmp(method,"GET") && !strcmp(url,"/api/state")) {
        struct settings s={0};int configured=pscloud_configure(cloudpath,&s)==0;
        char address[6144],username[512],json[8192];escaped(address,sizeof address,s.url);escaped(username,sizeof username,s.user);
        int status=cloud_status;
        unsigned firmware=firmware_id;
        char firmware_text[16];snprintf(firmware_text,sizeof firmware_text,"%x.%02x",firmware>>24,(firmware>>16)&255);
        snprintf(json,sizeof json,"{\"version\":\"%s\",\"firmware\":\"%s\",\"firmware_tested\":%s,\"pid\":%ld,\"cloud_http\":%d,\"configured\":%s,\"connected\":%s,\"url\":\"%s\",\"username\":\"%s\"}",PSCLOUD_VERSION,firmware_text,firmware_tested()?"true":"false",(long)getpid(),status,configured?"true":"false",status==200 || status==207?"true":"false",address,username);
        respond(sock,200,"application/json",json,strlen(json));return;
    }
    if(!strcmp(method,"GET") && (!strcmp(url,"/api/log") || !strncmp(url,"/api/log?",9))) {
        const char *logquery=url[8]=='?'?url+9:NULL;
        /* With wait=1&since=<size>, hold the request until the log changes (up to 25 s),
         * so the Activity page updates as things happen instead of on a timer. */
        char value[32];long long since=-1;
        int wait=logquery&&!parameter(logquery,"wait",value,sizeof value)&&!strcmp(value,"1");
        if(logquery&&!parameter(logquery,"since",value,sizeof value))since=strtoll(value,NULL,10);
        if(wait&&since>=0) {
            int slot=atomic_fetch_add(&log_waiters,1)<4; /* Leave client threads for other requests. */
            for(int i=0;slot&&i<25&&!stopped;i++) {struct stat now;if(stat(logpath,&now)||(long long)now.st_size!=since)break;sleep(1);}
            atomic_fetch_sub(&log_waiters,1);
        }
        int fd=open(logpath,O_RDONLY | O_NOFOLLOW | O_NONBLOCK);struct stat st;char data[8193]={0};long long size=0;
        if(fd>=0) {if(!fstat(fd,&st) && S_ISREG(st.st_mode)) {size=(long long)st.st_size;off_t offset=st.st_size>8192?st.st_size-8192:0;ssize_t got=pread(fd,data,8192,offset);if(got>=0)data[got]=0;}close(fd);}
        char text[50000],json[50100];escaped(text,sizeof text,data);snprintf(json,sizeof json,"{\"log\":\"%s\",\"size\":%lld}",text,size);respond(sock,200,"application/json",json,strlen(json));return;
    }
    /* Slow cloud/save operations remain serialized for safety. Other clients
     * get an immediate busy response, never wait behind a multi-minute upload.
     * Header/body reads occur on bounded client threads; idle browser sockets
     * no longer stall the accept loop or page/health requests. */
    if(background_active && !strcmp(method,"POST") && (!strcmp(url,"/api/sync") || !strcmp(url,"/api/sync-one") || !strcmp(url,"/api/sync-start") || !strcmp(url,"/api/stop"))) {message(sock,409,"An upload is running; cancel it and wait for completion before starting another or stopping PSCloud");return;}
    if(pthread_mutex_trylock(&operation_mutex)) {
        const char *json="{\"ok\":false,\"busy\":true,\"message\":\"PSCloud is completing another operation. Your saves are safe; try again when it finishes\"}";
        respond(sock,503,"application/json",json,strlen(json));return;
    }
    *locked=1;
    char transfer[64];if(!header_value(request,"Transfer-Encoding",transfer,sizeof transfer)) {message(sock,400,"Chunked requests are not supported");return;}
    char length[32];size_t expected=0;
    if(!strcmp(method,"POST")) {
        if(header_value(request,"Content-Length",length,sizeof length)) {message(sock,400,"Content length missing");return;}
        char *end=NULL;unsigned long n=strtoul(length,&end,10);
        if(!strncmp(url,"/api/import?",12)||!strncmp(url,"/api/share-import?",18)) {
            if(!*length || *end || !n || n>PSCLOUD_RESTORE_MAX) {message(sock,400,"ZIP upload limit is 512 MiB");return;}
            unsigned char *data=malloc(n);if(!data) {message(sock,500,"Not enough memory");return;}
            size_t have=size-headers;if(have>n)have=n;memcpy(data,body,have);time_t deadline=time(NULL)+120;
            while(have<n && time(NULL)<deadline) {ssize_t got=recv(sock,data+have,n-have,0);if(got<=0)break;have+=(size_t)got;}
            if(have!=n)message(sock,400,"Incomplete ZIP upload");
            else if(!strncmp(url,"/api/share-import?",18)) {
                struct pscloud_snapshot chosen;
                if(selection(url+18,&chosen)||pscloud_share_validate(data,n,chosen.title))message(sock,400,"Invalid portable multi-slot package or wrong game; normal backup ZIPs are not sharing packages");
                else {
                    char id[33],path[1800],file[128],sha[65];unsigned char digest[32];unsigned length=0;
                    snprintf(path,sizeof path,"%s/share",root);int parent=pscloud_open_directory(root);int bad=parent<0;
                    if(!bad) {
                        (void)mkdirat(parent,"share",0700);
                        int directory=openat(parent,"share",O_RDONLY|O_DIRECTORY|O_NOFOLLOW);struct stat st;
                        bad=directory<0||fstat(directory,&st)||!S_ISDIR(st.st_mode);if(directory>=0)close(directory);
                    }
                    if(parent>=0)close(parent);
                    if(!bad)bad=pscloud_random_id(id)||!EVP_Digest(data,n,digest,&length,EVP_sha256(),NULL)||length!=32;
                    if(!bad) {
                        for(unsigned i=0;i<32;i++)snprintf(sha+2*i,3,"%02x",digest[i]);
                        snprintf(file,sizeof file,"portable-%s-%s.zip",chosen.title,id);snprintf(path,sizeof path,"%s/share/%s",root,file);
                        int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);size_t at=0;bad=fd<0;
                        while(!bad&&at<n) {ssize_t wrote=write(fd,data+at,n-at);if(wrote<0&&errno==EINTR)continue;if(wrote<=0)bad=1;else at+=(size_t)wrote;}
                        if(fd>=0) {if(fsync(fd))bad=1;if(close(fd))bad=1;}
                        int verify=bad?-1:open(path,O_RDONLY|O_NOFOLLOW);char actual[65];bad=bad||verify<0||pscloud_file_hash(verify,actual)||strcmp(actual,sha);if(verify>=0)close(verify);
                    }
                    if(bad)message(sock,500,"Shared package could not be saved; no live saves changed");
                    else {prune_shared_keep(2);char json[512];snprintf(json,sizeof json,"{\"ok\":true,\"file\":\"%s\",\"sha256\":\"%s\",\"message\":\"Shared package imported locally. Run staged check before restoring\"}",file,sha);respond(sock,200,"application/json",json,strlen(json));}
                }
            } else import_pc(sock,url+12,data,n);
            free(data);return;
        }
        if(!*length || *end || n>8192 || headers+n>=sizeof request) {message(sock,400,"Request body too large");return;}expected=(size_t)n;
        while(size<headers+expected) {ssize_t got=recv(sock,request+size,headers+expected-size,0);if(got<=0)return;size+=(size_t)got;}
        request[headers+expected]=0;
    }
    char *query=strchr(url,'?');if(query)*query++=0;else query="";
    if((!strcmp(method,"GET") && !strcmp(url,"/api/google/status")) ||
       (!strcmp(method,"POST") && (!strcmp(url,"/api/google/begin") || !strcmp(url,"/api/google/poll")))) {google_action(sock,url,body);return;}
    if(!strcmp(url,"/api/preferences") && (!strcmp(method,"GET") || !strcmp(method,"POST"))) {preferences(sock,!strcmp(method,"POST")?body:NULL);return;}
    if(!strcmp(method,"GET") && !strcmp(url,"/api/games")) {games(sock);return;}
    if(!strcmp(method,"GET") && !strcmp(url,"/api/users")) {users(sock);return;}
    if(!strcmp(method,"GET") && !strcmp(url,"/api/save-status")) {save_status(sock,query);return;}
    if(!strcmp(method,"GET") && !strcmp(url,"/api/slot-icon")) {slot_icon(sock,query);return;}
    if(!strcmp(method,"GET") && !strcmp(url,"/api/sync-status")) {sync_status_json(sock,query);return;}
    if(!strcmp(method,"GET") && !strcmp(url,"/api/sync-slots")) {sync_slots_json(sock,query);return;}
    if(!strcmp(method,"GET") && !strcmp(url,"/api/cloud-profiles")) {cloud_profiles_json(sock);return;}
    if(!strcmp(method,"GET") && !strcmp(url,"/api/peers")) {peers_json(sock);return;}
    if(!strcmp(method,"GET") && !strcmp(url,"/api/peers/discover")) {peers_discover_json(sock);return;}
    if(!strcmp(method,"GET") && !strcmp(url,"/api/remote")) {const char *json=remote_replace_allowed()?"{\"allow_replace\":true}":"{\"allow_replace\":false}";respond(sock,200,"application/json",json,strlen(json));return;}
    if(!strcmp(method,"GET") && !strcmp(url,"/api/cloud-housekeeping")) {cloud_housekeeping_json(sock);return;}
    if(!strcmp(method,"GET") && !strcmp(url,"/api/encryption")) {const char *json=pscloud_encryption_on()?"{\"on\":true}":"{\"on\":false}";respond(sock,200,"application/json",json,strlen(json));return;}
    if(!strcmp(method,"GET") && !strcmp(url,"/api/game-settings")) {game_settings_json(sock,query);return;}
    if(!strcmp(method,"GET") && !strcmp(url,"/api/storage")) {storage_json(sock);return;}
    if(!strcmp(method,"GET") && !strcmp(url,"/api/cleanup")) {cleanup_preview(sock);return;}
    if(!strcmp(method,"GET") && !strcmp(url,"/api/usb")) {usb_list(sock);return;}
    if(!strcmp(method,"GET") && !strcmp(url,"/api/usb-files")) {usb_files(sock,query);return;}
    if(!strcmp(method,"GET") && !strcmp(url,"/api/usb-file")) {usb_file(sock,query);return;}
    if(!strcmp(method,"GET") && !strcmp(url,"/api/running")) {
        char title[16],name[256],escaped_name[1536],json[2048];int found=running_game(title);
        if(found>0) {pscloud_app_name(appmeta,title,name,sizeof name);escaped(escaped_name,sizeof escaped_name,name);}
        if(found>0)snprintf(json,sizeof json,"{\"known\":true,\"running\":true,\"title\":\"%s\",\"name\":\"%s\",\"icon\":\"/api/icon?title=%s\",\"backup_on_close\":%s}",title,escaped_name,title,game_close_backup?"true":"false");
        else snprintf(json,sizeof json,"{\"known\":%s,\"running\":false,\"backup_on_close\":%s}",found<0?"false":"true",game_close_backup?"true":"false");
        respond(sock,200,"application/json",json,strlen(json));return;
    }
    if(!strcmp(method,"GET") && !strcmp(url,"/api/icon")) {game_icon(sock,query);return;}
    if(!strcmp(method,"GET") && !strcmp(url,"/api/backups")) {backups(sock,query);return;}
    if(!strcmp(method,"GET") && !strcmp(url,"/api/queue")) {queue_list(sock);return;}
    if(!strcmp(method,"GET") && !strcmp(url,"/api/download-pc")) {download_pc(sock,query,0);return;}
    if(!strcmp(method,"GET") && !strcmp(url,"/api/queue-download")) {download_pc(sock,query,1);return;}
    if(!strcmp(method,"GET")&&!strcmp(url,"/api/share-download")) {
        char file[128],directory[1400];unsigned char *data=NULL;size_t bytes=0;
        if(parameter(query,"file",file,sizeof file)||strncmp(file,"portable-",9)||strchr(file,'/')||strchr(file,'\\')) {message(sock,400,"Invalid shared package filename");return;}
        snprintf(directory,sizeof directory,"%s/share",root);int dir=pscloud_open_directory(directory);int bad=dir<0||pscloud_read_archive(dir,file,&data,&bytes);if(dir>=0)close(dir);
        if(bad)message(sock,404,"Shared package unavailable");else respond(sock,200,"application/zip",(const char *)data,bytes);free(data);return;
    }
    if(!strcmp(method,"POST") && !strcmp(url,"/api/stop")) {message(sock,200,"Dashboard stopping");stopped=1;return;}
    if(!strcmp(method,"POST") && !strcmp(url,"/api/connect")) {configure_cloud(sock,body);return;}
    if(!strcmp(method,"POST")) {action(sock,url,body);return;}
    message(sock,404,"Unknown endpoint");
}
static void *client_main(void *arg) {
    int sock=*(int *)arg;free(arg);int locked=0;serve(sock,&locked);
    if(locked)pthread_mutex_unlock(&operation_mutex);
    close(sock);pthread_mutex_lock(&clients_mutex);clients--;pthread_cond_broadcast(&clients_done);pthread_mutex_unlock(&clients_mutex);return NULL;
}
/* Back up on game close. A game whose save containers changed is backed up once no
 * save is mounted and its files have been unchanged for two checks. The normal
 * backup path runs under the operation lock (skipped and retried while another
 * operation runs); backups read staged copies and refuse saves that change while
 * being copied. Only changes made while this option is on are backed up. */
struct watched_save {char user[17],title[10];time_t mtime;unsigned stable,attempts;int pending,waiting;};
static struct watched_save watched[512];static unsigned watched_count;static int watch_ready,detection_warned;
static time_t newest_container(int titledir) {
    int scan=dup(titledir);DIR *d=scan<0?NULL:fdopendir(scan);if(!d) {if(scan>=0)close(scan);return 0;}
    time_t newest=0;struct dirent *e;
    while((e=readdir(d))) {
        if(strncmp(e->d_name,"sdimg_",6) || !strncmp(e->d_name,"sdimg_sce_bu_",13) || !safe_word(e->d_name+6,63))continue;
        struct stat st;if(!fstatat(titledir,e->d_name,&st,AT_SYMLINK_NOFOLLOW) && S_ISREG(st.st_mode) && st.st_mtime>newest)newest=st.st_mtime;
    }
    closedir(d);return newest;
}
static void watch_scan(void) {
    int dir=pscloud_open_directory(home);DIR *users=NULL;if(dir>=0) {int scan=dup(dir);users=scan<0?NULL:fdopendir(scan);if(!users&&scan>=0)close(scan);}
    if(!users) {if(dir>=0)close(dir);return;}
    struct dirent *u;
    while((u=readdir(users))) {
        if(!safe_word(u->d_name,16))continue;
        char path[1400];snprintf(path,sizeof path,"%s/%s/savedata_prospero",home,u->d_name);
        int saves=pscloud_open_directory(path);if(saves<0)continue;
        int scan=dup(saves);DIR *titles=scan<0?NULL:fdopendir(scan);if(!titles) {if(scan>=0)close(scan);close(saves);continue;}
        struct dirent *t;
        while((t=readdir(titles))) {
            if(!pscloud_title_valid(t->d_name))continue;
            int titledir=openat(saves,t->d_name,O_RDONLY | O_DIRECTORY | O_NOFOLLOW);if(titledir<0)continue;
            time_t newest=newest_container(titledir);close(titledir);if(!newest)continue;
            struct watched_save *w=NULL;
            for(unsigned i=0;i<watched_count;i++)if(!strcmp(watched[i].user,u->d_name)&&!strcmp(watched[i].title,t->d_name))w=&watched[i];
            if(!w) {
                if(watched_count==sizeof watched/sizeof watched[0])continue;
                /* Lengths are already validated (user <=16, title exactly 9). */
                w=&watched[watched_count++];memset(w,0,sizeof *w);snprintf(w->user,sizeof w->user,"%.16s",u->d_name);snprintf(w->title,sizeof w->title,"%.9s",t->d_name);
                w->mtime=newest;w->pending=watch_ready; /* A save that appears later is a new first save. */
                if(w->pending)pscloud_log("INFO","New save for %s; backup after the game closes",w->title);
                continue;
            }
            if(newest!=w->mtime) {
                w->mtime=newest;w->stable=0;
                if(!w->pending) {w->pending=1;w->attempts=0;w->waiting=0;pscloud_log("INFO","Save changed for %s; backup after the game closes",w->title);}
            } else if(w->pending)w->stable++;
        }
        closedir(titles);close(saves);
    }
    closedir(users);close(dir);
}
static void watch_backup(struct watched_save *w) {
    struct pscloud_snapshot s={0};snprintf(s.user,sizeof s.user,"%s",w->user);snprintf(s.title,sizeof s.title,"%s",w->title);
    strcpy(s.slot,"WholeGame");memset(s.sha256,'0',64);if(!pscloud_snapshot_valid(&s)) {w->pending=0;return;}
    char name[256];pscloud_app_name(appmeta,w->title,name,sizeof name);
    pscloud_log_open(logpath);pscloud_log("EVENT","Game closed: backing up %s automatically",name);
    char published[144]={0};int bad;
    if(compact_effective())bad=compact_backup(&s,published);
    else bad=pscloud_game_backup_named(home,root,w->user,w->title,published);
    pscloud_log_open(logpath);
    if(bad) {
        w->stable=0;
        if(++w->attempts>=3) {w->pending=0;pscloud_log("WARN","Automatic backup of %s failed 3 times; it will retry after the game saves again",name);}
        else pscloud_log("WARN","Automatic backup of %s failed; retrying shortly",name);
        return;
    }
    w->pending=0;pscloud_notify("Saved a backup of %s after you closed it",name);
    if(!auto_upload||background_active)return;
    struct background_job *job=calloc(1,sizeof *job);if(!job)return;
    if(pscloud_configure(cloudpath,&job->settings)) {free(job);return;}
    snprintf(job->files[0],sizeof job->files[0],"%s",published);job->count=1;snprintf(job->spool,sizeof job->spool,"%s/spool",root);
    pthread_mutex_lock(&cache_mutex);pscloud_app_name(appmeta,w->title,background_name,sizeof background_name);pthread_mutex_unlock(&cache_mutex);
    if(launch_background(job))pscloud_log("WARN","Automatic upload of %s could not start; backup stays queued",name);
}
/* ---------- Back up all games ----------
 * One game at a time, each under the operation lock (so the dashboard stays usable between
 * games). Running games and games switched off for this are skipped; unchanged saves are
 * counted, not re-queued. New backups upload together at the end when automatic upload is on. */
static struct {_Atomic int running;unsigned total,done,backed,unchanged,skipped,failed;char current[256],scope[32];long long started,finished;} batch;
static int schedule_hour=-1;static char schedule_last[16];
static void schedule_load(void) {
    char path[1400];snprintf(path,sizeof path,"%s/schedule.conf",root);FILE *f=fopen(path,"r");if(!f)return;
    int hour=-1;char last[16]={0};if(fscanf(f,"HOUR=%d\nLAST=%15s",&hour,last)>=1&&hour>=-1&&hour<=23) {schedule_hour=hour;snprintf(schedule_last,sizeof schedule_last,"%s",last);}
    fclose(f);
}
static int schedule_save(void) {
    char path[1400],text[64];snprintf(path,sizeof path,"%s/schedule.conf",root);snprintf(text,sizeof text,"HOUR=%d\nLAST=%s\n",schedule_hour,*schedule_last?schedule_last:"-");
    return atomic_config(path,text);
}
static void batch_json(int sock) {
    char current[256],escaped_current[1536],json[2400];pthread_mutex_lock(&cache_mutex);snprintf(current,sizeof current,"%s",batch.current);pthread_mutex_unlock(&cache_mutex);
    escaped(escaped_current,sizeof escaped_current,current);
    snprintf(json,sizeof json,"{\"running\":%s,\"total\":%u,\"done\":%u,\"backed\":%u,\"unchanged\":%u,\"skipped\":%u,\"failed\":%u,\"current\":\"%s\",\"started\":%lld,\"finished\":%lld,\"schedule_hour\":%d,\"schedule_last\":\"%s\"}",
        batch.running?"true":"false",batch.total,batch.done,batch.backed,batch.unchanged,batch.skipped,batch.failed,escaped_current,batch.started,batch.finished,schedule_hour,schedule_last);
    respond(sock,200,"application/json",json,strlen(json));
}
/* Back up one game the way the game-close backup does. 0 ok, -1 failed. */
static int backup_one(const char *user,const char *title,char published[144],int *unchanged) {
    struct pscloud_snapshot s={0};snprintf(s.user,sizeof s.user,"%s",user);snprintf(s.title,sizeof s.title,"%s",title);
    strcpy(s.slot,"WholeGame");memset(s.sha256,'0',64);published[0]=0;*unchanged=0;if(!pscloud_snapshot_valid(&s))return -1;
    if(compact_effective())return compact_backup_ex(&s,published,0,unchanged);
    return pscloud_game_backup_named(home,root,user,title,published);
}
struct batch_item {char user[17],title[10];};
static void *batch_main(void *arg) {
    struct batch_item *items=arg;unsigned n=batch.total;
    struct background_job *job=calloc(1,sizeof *job);
    for(unsigned i=0;i<n&&!stopped;i++) {
        char name[256];pscloud_app_name(appmeta,items[i].title,name,sizeof name);
        pthread_mutex_lock(&cache_mutex);snprintf(batch.current,sizeof batch.current,"%s",name);pthread_mutex_unlock(&cache_mutex);
        if(!game_setting(items[i].title,1)||title_running(items[i].title)>0) {batch.skipped++;batch.done++;continue;}
        pthread_mutex_lock(&operation_mutex);
        char published[144];int unchanged=0;pscloud_log_open(logpath);int bad=backup_one(items[i].user,items[i].title,published,&unchanged);
        pthread_mutex_unlock(&operation_mutex);
        if(bad)batch.failed++;else if(unchanged)batch.unchanged++;else {batch.backed++;if(job&&*published&&job->count<256)snprintf(job->files[job->count++],sizeof job->files[0],"%s",published);}
        batch.done++;
    }
    free(items);
    pscloud_log_open(logpath);
    pscloud_log("EVENT","Back up all games: %u backed up, %u unchanged, %u skipped, %u failed",batch.backed,batch.unchanged,batch.skipped,batch.failed);
    pscloud_notify("PSCloud: %u games backed up, %u unchanged%s",batch.backed,batch.unchanged,batch.failed?"; some failed - see Activity":"");
    if(job&&job->count&&auto_upload&&!background_active&&!pscloud_configure(cloudpath,&job->settings)) {
        snprintf(job->spool,sizeof job->spool,"%s/spool",root);
        pthread_mutex_lock(&cache_mutex);snprintf(background_name,sizeof background_name,"%u new backups",job->count);pthread_mutex_unlock(&cache_mutex);
        pthread_mutex_lock(&operation_mutex);int bad=launch_background(job);pthread_mutex_unlock(&operation_mutex);
        if(bad)pscloud_log("WARN","Uploading the new backups could not start; they stay queued");
    } else free(job);
    pthread_mutex_lock(&cache_mutex);batch.current[0]=0;pthread_mutex_unlock(&cache_mutex);
    batch.finished=(long long)time(NULL);batch.running=0;
    pthread_mutex_lock(&clients_mutex);clients--;pthread_cond_broadcast(&clients_done);pthread_mutex_unlock(&clients_mutex);
    return NULL;
}
/* user: one profile, or NULL for every profile. */
static int batch_start(const char *user) {
    if(batch.running)return 1;
    struct batch_item *items=calloc(512,sizeof *items);if(!items)return -1;unsigned n=0;
    DIR *users=opendir(home);struct dirent *u;
    while(users&&(u=readdir(users))&&n<512) {
        if(!safe_word(u->d_name,16)||(user&&strcmp(user,u->d_name)))continue;
        char path[1400];snprintf(path,sizeof path,"%s/%s/savedata_prospero",home,u->d_name);DIR *titles=opendir(path);struct dirent *t;
        while(titles&&(t=readdir(titles))&&n<512) {
            if(!pscloud_title_valid(t->d_name))continue;
            char dir[1500];snprintf(dir,sizeof dir,"%.1400s/%.9s",path,t->d_name);int fd=pscloud_open_directory(dir);if(fd<0)continue;
            int has=newest_container(fd)!=0;close(fd);if(!has)continue;
            snprintf(items[n].user,sizeof items[n].user,"%.16s",u->d_name);snprintf(items[n].title,sizeof items[n].title,"%.9s",t->d_name);n++;
        }
        if(titles)closedir(titles);
    }
    if(users)closedir(users);
    if(!n) {free(items);return -1;}
    memset(&batch,0,sizeof batch);batch.total=n;batch.started=(long long)time(NULL);batch.running=1;snprintf(batch.scope,sizeof batch.scope,"%s",user?user:"all");
    pthread_t thread;pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,4U*1024*1024);
    pthread_mutex_lock(&clients_mutex);clients++;pthread_mutex_unlock(&clients_mutex);
    int bad=pthread_create(&thread,&attr,batch_main,items);pthread_attr_destroy(&attr);
    if(bad) {pthread_mutex_lock(&clients_mutex);clients--;pthread_mutex_unlock(&clients_mutex);batch.running=0;free(items);return -1;}
    pthread_detach(thread);pscloud_log("EVENT","Back up all games started (%u games, %s)",n,user?"one profile":"all profiles");return 0;
}
/* Nightly: at the chosen hour, once per day, when no game is running. */
static void schedule_tick(void) {
    if(schedule_hour<0||batch.running)return;
    time_t now=time(NULL);struct tm local;localtime_r(&now,&local);
    char today[16];strftime(today,sizeof today,"%Y-%m-%d",&local);
#ifdef PSCLOUD_HOST_TEST
    if(getenv("PSCLOUD_TEST_SCHEDULE_NOW"))local.tm_hour=schedule_hour;
#endif
    /* at or after the chosen hour, once a day: a backup missed in rest mode runs on wake */
    if(local.tm_hour<schedule_hour||!strcmp(today,schedule_last))return;
    char game[16];if(running_game(game)>0)return; /* try again next minute */
    snprintf(schedule_last,sizeof schedule_last,"%s",today);schedule_save();
    pscloud_log_open(logpath);pscloud_log("EVENT","Scheduled backup of all games");
    if(batch_start(NULL)<0)pscloud_log("WARN","Scheduled backup found no games");
}
#ifndef PSCLOUD_HOST_TEST
/* Versions up to 0.22 kept the log, cloud sign-in, CA bundle and tool settings beside the
 * folder as /data/pscloud*. Move them in once ("pscloud-x" becomes "x"); never overwrite. */
static void move_into_folder(void) {
    (void)mkdir("/data/pscloud",0700);
    DIR *d=opendir("/data");if(!d)return;struct dirent *e;unsigned moved=0;
    while((e=readdir(d))) {
        if(strncmp(e->d_name,"pscloud",7)||!strcmp(e->d_name,"pscloud")||strchr(e->d_name,'/'))continue;
        char from[600],to[700];struct stat st;snprintf(from,sizeof from,"/data/%s",e->d_name);
        if(lstat(from,&st)||!S_ISREG(st.st_mode))continue;
        const char *name=!strncmp(e->d_name,"pscloud-",8)?e->d_name+8:e->d_name;
        snprintf(to,sizeof to,"/data/pscloud/%s",name);
        if(!lstat(to,&st))continue;
        if(!rename(from,to))moved++;
    }
    closedir(d);
    /* The saved cloud settings point at the CA bundle's old place. */
    char text[8192];int fd=open("/data/pscloud/upload.conf",O_RDONLY|O_NOFOLLOW);ssize_t n=fd>=0?read(fd,text,sizeof text-1):-1;if(fd>=0)close(fd);
    if(n>0) {text[n]=0;char *at=strstr(text,"CA_BUNDLE=/data/pscloud-ca.pem\n");
        if(at) {char fixed[8300];snprintf(fixed,sizeof fixed,"%.*sCA_BUNDLE=/data/pscloud/ca.pem\n%s",(int)(at-text),text,at+strlen("CA_BUNDLE=/data/pscloud-ca.pem\n"));atomic_config("/data/pscloud/upload.conf",fixed);memset(fixed,0,sizeof fixed);}
        memset(text,0,sizeof text);}
    if(moved) {pscloud_log_open("/data/pscloud/pscloud.log");pscloud_log("EVENT","Moved %u PSCloud files from /data into /data/pscloud",moved);}
}
#endif
static void *game_close_watch(void *unused) {
    (void)unused;unsigned seconds=0,interval=10;
#ifdef PSCLOUD_HOST_TEST
    if(getenv("PSCLOUD_TEST_WATCH_SECONDS"))interval=(unsigned)strtoul(getenv("PSCLOUD_TEST_WATCH_SECONDS"),NULL,10);
    if(!interval)interval=1;
#endif
    time_t last_beat=time(NULL);
    while(!stopped) {
        sleep(1);++seconds;
        time_t now_beat=time(NULL);
        if(now_beat-last_beat>120) { /* the process was suspended, e.g. the PS5 was in rest mode */
            pscloud_log_open(logpath);pscloud_log("EVENT","PSCloud was paused for %lld minutes (the PS5 was probably in rest mode); running again",(long long)(now_beat-last_beat)/60);
        }
        last_beat=now_beat;
        if(!(seconds%60)
#ifdef PSCLOUD_HOST_TEST
           ||getenv("PSCLOUD_TEST_SCHEDULE_NOW")
#endif
          )schedule_tick();
        if(seconds==20||!(seconds%3600))registry_publish(); /* let other consoles find these profiles */
        if(seconds==30||!(seconds%300))peer_synced_refresh();
        if(!(seconds%300))sync_tick();
        if(seconds%interval)continue;
        if(!game_close_backup&&!any_synced()) {watch_ready=0;watched_count=0;continue;}
        watch_scan();
        if(!watch_ready) {watch_ready=1;continue;}
        if(pscloud_no_foreign_mount())continue; /* A mounted save means a game is still running. */
        for(unsigned i=0;i<watched_count&&!stopped;i++) {
            struct watched_save *w=&watched[i];
            if(!w->pending||w->stable<2)continue;
            int synced=game_synced(w->title);
            if(!synced&&(!game_close_backup||!game_setting(w->title,0))) {w->pending=0;if(game_close_backup)pscloud_log("INFO","%s: back up when closed is off for this game",w->title);continue;}
            int running=title_running(w->title);
            if(running>0) {if(!w->waiting) {w->waiting=1;pscloud_log("INFO","%s is still running; backup waits until it is closed",w->title);}continue;}
            /* Without a process list, only back up after five quiet minutes. */
            if(running<0) {if(!detection_warned) {detection_warned=1;pscloud_log("WARN","Running games cannot be detected on this console; game-close backups wait for 5 minutes without save changes");}if(w->stable<30)continue;}
            if(pthread_mutex_trylock(&operation_mutex))break;
            watch_backup(&watched[i]);pthread_mutex_unlock(&operation_mutex);
        }
    }
    pthread_mutex_lock(&clients_mutex);clients--;pthread_cond_broadcast(&clients_done);pthread_mutex_unlock(&clients_mutex);return NULL;
}
int main(int argc,char **argv) {
    unsigned port=8082;
#ifdef PSCLOUD_HOST_TEST
    if(argc!=4)return 2;
    if(getenv("PSCLOUD_TEST_FIRMWARE"))firmware_id=(unsigned)strtoul(getenv("PSCLOUD_TEST_FIRMWARE"),NULL,16);
    snprintf(root,sizeof root,"%s",argv[1]);
    snprintf(system_root,sizeof system_root,"%s/system",argv[1]);snprintf(home,sizeof home,"%s",argv[2]);port=(unsigned)strtoul(argv[3],NULL,10);
#else
    (void)argc;(void)argv;
    firmware_id=kernel_get_fw_version()&0xffff0000U;
    strcpy(root,"/data/pscloud");strcpy(home,"/user/home");
#endif
    snprintf(appmeta,sizeof appmeta,
#ifdef PSCLOUD_HOST_TEST
        "%s/appmeta",root
#else
        "/user/appmeta"
#endif
    );
    if(port>65535 || pscloud_random_id(token))return 2;
    snprintf(cloudpath,sizeof cloudpath,
#ifdef PSCLOUD_HOST_TEST
        "%s/upload.conf",root
#else
        "/data/pscloud/upload.conf"
#endif
    );
    snprintf(logpath,sizeof logpath,
#ifdef PSCLOUD_HOST_TEST
        "%s/pscloud.log",root
#else
        "/data/pscloud/pscloud.log"
#endif
    );
#ifndef PSCLOUD_HOST_TEST
    move_into_folder();
#endif
    snprintf(googlepath,sizeof googlepath,"%s/google.conf",root);pscloud_google_load(googlepath,&google);
    game_settings_load();schedule_load();console_id_load();cloud_keep_load();encryption_load();
    pscloud_log_open(logpath);load_preferences();signal(SIGPIPE,SIG_IGN);signal(SIGTERM,stop_server);signal(SIGINT,stop_server);
    if(curl_global_init(CURL_GLOBAL_DEFAULT))return 2;
    int server=socket(AF_INET,SOCK_STREAM,0);if(server<0)return 2;
    int yes=1;setsockopt(server,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof yes);
    struct sockaddr_in address={0};address.sin_family=AF_INET;address.sin_port=htons((uint16_t)port);
    address.sin_addr.s_addr=htonl(INADDR_ANY);
    if(bind(server,(struct sockaddr *)&address,sizeof address) || listen(server,8)) {close(server);return 2;}
    socklen_t length=sizeof address;getsockname(server,(struct sockaddr *)&address,&length);
    http_port_number=ntohs(address.sin_port);
#ifdef PSCLOUD_HOST_TEST
    if(getenv("PSCLOUD_TEST_UDP_PORT"))peer_udp_port=(unsigned)atoi(getenv("PSCLOUD_TEST_UDP_PORT"));
#endif
    {pthread_t beacon;pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,256U*1024);
     if(!pthread_create(&beacon,&attr,peer_beacon,NULL))pthread_detach(beacon);
     pthread_attr_destroy(&attr);}
    printf("PSCLOUD_DASHBOARD_PORT=%u\nPSCLOUD_DASHBOARD_READY=1\n",ntohs(address.sin_port));fflush(stdout);
    pscloud_notify("PSCloud dashboard ready on port %u - open in your browser",ntohs(address.sin_port));
    if(!firmware_tested()) {pscloud_log_open(logpath);pscloud_log("WARN","Firmware %x.%02x has not been tested with PSCloud yet; every replace still runs the staged check first and keeps a rollback copy",firmware_id>>24,(firmware_id>>16)&255);}
    {
        pthread_t watcher;pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,4U*1024*1024);
        pthread_mutex_lock(&clients_mutex);clients++;pthread_mutex_unlock(&clients_mutex);
        if(pthread_create(&watcher,&attr,game_close_watch,NULL)) {pthread_mutex_lock(&clients_mutex);clients--;pthread_mutex_unlock(&clients_mutex);pscloud_log("WARN","Game-close backup watcher could not start");}
        else pthread_detach(watcher);
        pthread_attr_destroy(&attr);
    }
    while(!stopped) {
        fd_set readset;FD_ZERO(&readset);FD_SET(server,&readset);struct timeval wait={1,0};
        int ready=select(server+1,&readset,NULL,NULL,&wait);if(ready<=0)continue;
        int sock=accept(server,NULL,NULL);if(sock<0)continue;
        struct timeval timeout={15,0};setsockopt(sock,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);
        setsockopt(sock,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout);
        pthread_mutex_lock(&clients_mutex);
        if(clients>=12) {pthread_mutex_unlock(&clients_mutex);close(sock);continue;}
        clients++;pthread_mutex_unlock(&clients_mutex);
        int *arg=malloc(sizeof *arg);pthread_t thread;pthread_attr_t attr;
        pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,4U*1024*1024);
        if(arg)*arg=sock;
        int bad=!arg || pthread_create(&thread,&attr,client_main,arg);
        pthread_attr_destroy(&attr);
        if(bad) {free(arg);close(sock);pthread_mutex_lock(&clients_mutex);clients--;pthread_mutex_unlock(&clients_mutex);}
        else pthread_detach(thread);
    }
    pthread_mutex_lock(&clients_mutex);while(clients)pthread_cond_wait(&clients_done,&clients_mutex);pthread_mutex_unlock(&clients_mutex);
    close(server);curl_global_cleanup();pscloud_log_close();return 0;
}
