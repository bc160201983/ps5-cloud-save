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
#ifndef PSCLOUD_HOST_TEST
#include <ps5/kernel.h>
#endif
extern int pscloud_backup_main(int,char **);
extern const char *pscloud_backup_archive(void);
extern int pscloud_worker_main(int,char **);
extern int pscloud_download_main(int,char **);
static char root[1024],home[1024],cloudpath[1200],logpath[1200],token[33],appmeta[1200];
static _Atomic int cloud_status;
static _Atomic int auto_upload=1,activity_refresh=1;
static _Atomic int local_keep_latest;
static void prune_local_history(void);
static struct pscloud_google google,google_pending;
static char googlepath[1200];
static pthread_mutex_t operation_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t clients_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t clients_done=PTHREAD_COND_INITIALIZER;
static unsigned clients;
static _Atomic int background_active,background_result,background_cancelled;
static pthread_mutex_t cache_mutex=PTHREAD_MUTEX_INITIALIZER;
static char games_cache[131072],queue_cache[65536];
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
    pscloud_log("INFO","WebDAV %s: transport=%d HTTP=%ld",method,(int)result,status);
    curl_slist_free_all(headers);curl_easy_cleanup(c);return result==CURLE_OK?status:0;
}
static int safe_word(const char *s,size_t max) {
    if(!*s || strlen(s)>max)return 0;
    for(;*s;s++)if(!strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-",*s))return 0;
    return 1;
}
static int valid_archive(const char *s) {
    const char *prefix="ps5-11.40-";size_t n=20;
    if(strlen(s)!=n+36 || strncmp(s,prefix,strlen(prefix)) || s[19]!='-' || strcmp(s+n+32,".zip"))return 0;
    char title[10];memcpy(title,s+10,9);title[9]=0;if(!pscloud_title_valid(title))return 0;
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
        char unavailable[8];
        if(!parameter(form,"game_close_backup",unavailable,sizeof unavailable) && strcmp(unavailable,"0")) {
            message(sock,400,"Automatic game-close backup is not available yet");return;
        }
        char keep[8];int latest=local_keep_latest;
        if(!parameter(form,"local_keep_latest",keep,sizeof keep)) {if(strcmp(keep,"0")&&strcmp(keep,"1")) {message(sock,400,"Invalid local retention option");return;}latest=!strcmp(keep,"1");}
        char file[1200],text[128];snprintf(file,sizeof file,"%s/preferences.conf",root);
        snprintf(text,sizeof text,"AUTO_UPLOAD=%s\nACTIVITY_REFRESH=%s\nLOCAL_KEEP_LATEST=%d\n",upload,refresh,latest);
        if(atomic_config(file,text)) {message(sock,500,"Preferences could not be saved");return;}
        auto_upload=!strcmp(upload,"1");activity_refresh=!strcmp(refresh,"1");
        local_keep_latest=latest;if(latest)prune_local_history();
        pscloud_log("EVENT","Preferences saved: automatic upload %s; activity refresh %s",auto_upload?"on":"off",activity_refresh?"on":"off");
    }
    char json[512];snprintf(json,sizeof json,"{\"auto_upload\":%s,\"activity_refresh\":%s,\"local_keep_latest\":%s,\"game_close_backup\":false,\"game_close_available\":false,\"sharing_available\":true,\"message\":\"Preferences saved on this PS5\"}",auto_upload?"true":"false",activity_refresh?"true":"false",local_keep_latest?"true":"false");
    respond(sock,200,"application/json",json,strlen(json));
}
static void load_preferences(void) {
    /* Inventory first: the console SDK can return unusual descriptors for missing files. */
    DIR *dir=opendir(root);if(!dir)return;int exists=0;struct dirent *e;
    while((e=readdir(dir)))if(!strcmp(e->d_name,"preferences.conf"))exists=1;
    closedir(dir);if(!exists)return;
    auto_upload=0; /* Existing but unreadable preferences must not enable uploads. */
    char path[1200];snprintf(path,sizeof path,"%s/preferences.conf",root);
    int fd=open(path,O_RDONLY | O_NOFOLLOW | O_NONBLOCK);struct stat st;char data[129]={0};
    if(fd<0)return;
    if(!fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_size>0 && st.st_size<129) {
        ssize_t n=read(fd,data,128);
        if(n>0) {
            int upload,refresh,keep,used=0;
            if(sscanf(data,"AUTO_UPLOAD=%d\nACTIVITY_REFRESH=%d\nLOCAL_KEEP_LATEST=%d\n%n",&upload,&refresh,&keep,&used)==3 && used==(int)strlen(data) && upload>=0 && upload<=1 && refresh>=0 && refresh<=1 && keep>=0 && keep<=1) {auto_upload=upload;activity_refresh=refresh;local_keep_latest=keep;}
            else if(!strcmp(data,"AUTO_UPLOAD=0\nACTIVITY_REFRESH=0\n")) {auto_upload=0;activity_refresh=0;}
            else if(!strcmp(data,"AUTO_UPLOAD=0\nACTIVITY_REFRESH=1\n")) {auto_upload=0;activity_refresh=1;}
            else if(!strcmp(data,"AUTO_UPLOAD=1\nACTIVITY_REFRESH=0\n")) {auto_upload=1;activity_refresh=0;}
            else if(!strcmp(data,"AUTO_UPLOAD=1\nACTIVITY_REFRESH=1\n")) {auto_upload=1;activity_refresh=1;}
            else pscloud_log("WARN","Invalid preferences; automatic upload paused");
        }
    } else {auto_upload=0;pscloud_log("WARN","Unreadable preferences; automatic upload paused");}
    close(fd);
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
    char json[131072]="{\"games\":[";size_t pos=strlen(json);unsigned count=0;
    int dir=pscloud_open_directory(home);if(dir<0) {message(sock,500,"User save folders unavailable");return;}
    int scan=openat(dir,".",O_RDONLY | O_DIRECTORY);DIR *users=scan>=0?fdopendir(scan):NULL;
    if(!users) {if(scan>=0)close(scan);close(dir);message(sock,500,"User enumeration failed");return;}
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
            char name[256],escaped_name[1536];pscloud_app_name(appmeta,t->d_name,name,sizeof name);escaped(escaped_name,sizeof escaped_name,name);
            int titlefd=openat(fd,t->d_name,O_RDONLY | O_DIRECTORY | O_NOFOLLOW);if(titlefd<0)continue;
            DIR *slots=fdopendir(titlefd);if(!slots) {close(titlefd);continue;}struct dirent *e;
            while((e=readdir(slots)) && count<256) {
                if(strncmp(e->d_name,"sdimg_",6) || !strncmp(e->d_name,"sdimg_sce_bu_",13) || !safe_word(e->d_name+6,63))continue;
                int supported=safe_word(e->d_name+6,63);
                int written=snprintf(json+pos,sizeof json-pos,"%s{\"user\":\"%s\",\"title\":\"%s\",\"slot\":\"%s\",\"name\":\"%s\",\"icon\":\"/api/icon?title=%s\",\"supported\":%s}",count?",":"",u->d_name,t->d_name,e->d_name+6,escaped_name,t->d_name,supported?"true":"false");
                if(written<0 || (size_t)written>=sizeof json-pos-4)break;
                pos+=(size_t)written;count++;
            }
            closedir(slots);
        }
        closedir(titles);
    }
    }
    closedir(users);close(dir);snprintf(json+pos,sizeof json-pos,"],\"truncated\":%s}",count>=256?"true":"false");cache_store(games_cache,sizeof games_cache,json);respond(sock,200,"application/json",json,strlen(json));
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
struct background_job {struct settings settings;char spool[1400],files[256][144];unsigned count;};
static void *background_upload(void *arg) {
    struct background_job *job=arg;int result=0;
    for(unsigned i=0;i<job->count;i++) {
        if(stopped||background_cancelled) {result=1;break;}
        result|=pscloud_worker_upload(job->spool,job->files[i],&job->settings);
    }
    if(background_cancelled)result=1;
    if(!result && !pthread_mutex_trylock(&operation_mutex)) {prune_local_history();pthread_mutex_unlock(&operation_mutex);}
    background_result=result;
    pscloud_log(result?"WARN":"EVENT",result?"Background upload stopped or failed; local backups retained":"Background upload verified and completed");
    volatile unsigned char *secret=(volatile unsigned char *)&job->settings;for(size_t i=0;i<sizeof job->settings;i++)secret[i]=0;
    free(job);background_active=0;
    pthread_mutex_lock(&clients_mutex);clients--;pthread_cond_broadcast(&clients_done);pthread_mutex_unlock(&clients_mutex);return NULL;
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
    /* Freeze this job's filenames: later PC imports are never auto-uploaded. */
    background_result=0;background_cancelled=0;pscloud_worker_prepare();background_active=1;
    pthread_mutex_lock(&clients_mutex);clients++;pthread_mutex_unlock(&clients_mutex);
    pthread_t thread;pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,4U*1024*1024);
    int bad=pthread_create(&thread,&attr,background_upload,job);pthread_attr_destroy(&attr);
    if(bad) {background_active=0;free(job);pthread_mutex_lock(&clients_mutex);clients--;pthread_mutex_unlock(&clients_mutex);message(sock,500,"Could not start background upload; local backups retained");return;}
    pthread_detach(thread);
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
    return !strcmp(s->slot,"WholeGame")?pscloud_bundle_index(data,size,s,&bundle):pscloud_save_payload(data,size,&payload,&length);
}
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
            else {snprintf(url,sizeof url,"%s/%s/%s",cloud.url,folder,file);struct response r={(char *)data,0,PSCLOUD_RESTORE_MAX};bad=webdav(&cloud,url,"GET",&r)!=200;size=r.size;}
        }
    }
    if(!bad)bad=pscloud_verify_hash(data,size,s.sha256) || archive_valid(data,size,&s);
    if(bad)message(sock,502,"Backup download or validation failed");else attachment(sock,file,data,size);
    free(data);
}
static void import_pc(int sock,const char *query,const unsigned char *data,size_t size) {
    struct pscloud_snapshot s;struct pscloud_bundle bundle;
    if(selection(query,&s) || strcmp(s.slot,"WholeGame") || pscloud_bundle_index(data,size,&s,&bundle)) {message(sock,400,"Select Whole game and upload an unmodified PSCloud ZIP for this game and PS5 user");return;}
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
    } else snprintf(file,sizeof file,"ps5-11.40-%s-%s.zip",s.title,id);
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
    if(*previous.ca)strcpy(s.ca,previous.ca);else strcpy(s.ca,"/data/pscloud-ca.pem");
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
    if(!strcmp(path,"/api/sync")) {int result=upload_queue();message(sock,result?502:200,result?"Upload pending; local backups retained":"Queued backups uploaded");return;}
    if(!strcmp(path,"/api/sync-one")) {
        char file[128],ready[144];struct pscloud_snapshot s;
        if(parameter(form,"file",file,sizeof file) || pending_archive(file,&s,NULL,NULL)) {message(sock,400,"Selected pending backup unavailable");return;}
        snprintf(ready,sizeof ready,"%s.ready",file);int result=upload_selected(ready);
        message(sock,result?502:200,result?"Selected upload pending; local backup retained":"Selected backup available in cloud");return;
    }
    if(selection(form,&chosen)) {message(sock,400,"Unsupported save selection");return;}
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
        char published[128];int bad=pscloud_share_game(home,root,appmeta,chosen.user,chosen.title,exporting?0:checking?1:2,data,size,published,sha);free(data);
        if(!bad&&checking) {char proof[240],proofpath[1800];snprintf(proof,sizeof proof,"%s.check-%s",file,chosen.user);snprintf(proofpath,sizeof proofpath,"%s/%s",directory,proof);bad=atomic_config(proofpath,sha);}
        if(dir>=0)close(dir);
        if(bad) {message(sock,400,"Sharing refused or failed. Requires matching game version, existing same-named receiver slots of sufficient size, valid metadata and clean mounts. Live restores retain rollback; inspect Activity before retrying");return;}
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
#ifndef PSCLOUD_HOST_TEST
            if((kernel_get_fw_version()&0xffff0000U)!=0x11400000U) {message(sock,400,"Whole-game backup requires validated firmware 11.40");return;}
#endif
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
        int result=atomic_config("/data/pscloud-backup.conf",config)?2:pscloud_backup_main(1,args);
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
        if(!bad)bad=smart?pscloud_bundle_restore_smart(home,root,&snapshot,data,size):pscloud_bundle_restore(home,root,&snapshot,data,size);
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
            struct response r={(char *)data,0,PSCLOUD_RESTORE_MAX};bad=webdav(&cloud,url,"GET",&r)!=200 || pscloud_verify_hash(data,r.size,snapshot.sha256) || archive_valid(data,r.size,&snapshot);size=r.size;
        }
        if(!bad && restoring) {
            char confirm[8];
            if(parameter(form,"closed",closed,sizeof closed) || strcmp(closed,"yes") || parameter(form,"confirm",confirm,sizeof confirm) || strcmp(confirm,"yes"))bad=1;
            else {char skip[8];int smart=!parameter(form,"skip_identical",skip,sizeof skip)&&!strcmp(skip,"yes");bad=checking?pscloud_bundle_restore_check(home,root,&snapshot,data,size):smart?pscloud_bundle_restore_smart(home,root,&snapshot,data,size):pscloud_bundle_restore(home,root,&snapshot,data,size);}
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
    int result=atomic_config("/data/pscloud-download.conf",config)?2:pscloud_download_main(1,args);
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
    snprintf(path,sizeof path,"%s/%s",appmeta,title);int dir=pscloud_open_directory(path);
    int fd=dir>=0?openat(dir,"icon0.png",O_RDONLY | O_NOFOLLOW | O_NONBLOCK):-1;if(dir>=0)close(dir);
    struct stat st;if(fd<0 || fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size<8 || st.st_size>4*1024*1024) {if(fd>=0)close(fd);reject(sock,404,"Installed game icon unavailable");return;}
    unsigned char *data=malloc((size_t)st.st_size);if(!data) {close(fd);reject(sock,500,"Out of memory");return;}
    size_t have=0;while(have<(size_t)st.st_size) {ssize_t n=read(fd,data+have,(size_t)st.st_size-have);if(n<=0)break;have+=(size_t)n;}
    close(fd);
    if(have!=(size_t)st.st_size || memcmp(data,"\x89PNG\r\n\x1a\n",8))reject(sock,404,"Invalid installed icon");
    else respond(sock,200,"image/png",(const char *)data,have);
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
        "/data/pscloud-ca.pem"
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
    if(!strcmp(method,"GET") && !strcmp(url,"/api/state")) {
        struct settings s={0};int configured=pscloud_configure(cloudpath,&s)==0;
        char address[6144],username[512],json[8192];escaped(address,sizeof address,s.url);escaped(username,sizeof username,s.user);
        int status=cloud_status;
        unsigned firmware=0x11400000U;
#ifndef PSCLOUD_HOST_TEST
        firmware=kernel_get_fw_version()&0xffff0000U;
#endif
        char firmware_text[16];snprintf(firmware_text,sizeof firmware_text,"%x.%02x",firmware>>24,(firmware>>16)&255);
        snprintf(json,sizeof json,"{\"version\":\"%s\",\"firmware\":\"%s\",\"pid\":%ld,\"cloud_http\":%d,\"configured\":%s,\"connected\":%s,\"url\":\"%s\",\"username\":\"%s\"}",PSCLOUD_VERSION,firmware_text,(long)getpid(),status,configured?"true":"false",status==200 || status==207?"true":"false",address,username);
        respond(sock,200,"application/json",json,strlen(json));return;
    }
    if(!strcmp(method,"GET") && !strcmp(url,"/api/log")) {
        int fd=open(logpath,O_RDONLY | O_NOFOLLOW | O_NONBLOCK);struct stat st;char data[8193]={0};
        if(fd>=0) {if(!fstat(fd,&st) && S_ISREG(st.st_mode)) {off_t offset=st.st_size>8192?st.st_size-8192:0;ssize_t got=pread(fd,data,8192,offset);if(got>=0)data[got]=0;}close(fd);}
        char text[50000],json[50100];escaped(text,sizeof text,data);snprintf(json,sizeof json,"{\"log\":\"%s\"}",text);respond(sock,200,"application/json",json,strlen(json));return;
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
                    if(!bad&&mkdirat(parent,"share",0700)&&errno!=EEXIST)bad=1;
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
                    else {char json[512];snprintf(json,sizeof json,"{\"ok\":true,\"file\":\"%s\",\"sha256\":\"%s\",\"message\":\"Shared package imported locally. Run staged check before restoring\"}",file,sha);respond(sock,200,"application/json",json,strlen(json));}
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
int main(int argc,char **argv) {
    unsigned port=8082;
#ifdef PSCLOUD_HOST_TEST
    if(argc!=4)return 2;
    snprintf(root,sizeof root,"%s",argv[1]);snprintf(home,sizeof home,"%s",argv[2]);port=(unsigned)strtoul(argv[3],NULL,10);
#else
    (void)argc;(void)argv;
    unsigned fw=kernel_get_fw_version()&0xffff0000U;
    if(fw!=0x11400000U&&fw!=0x07000000U)return 2;
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
        "/data/pscloud-upload.conf"
#endif
    );
    snprintf(logpath,sizeof logpath,
#ifdef PSCLOUD_HOST_TEST
        "%s/pscloud.log",root
#else
        "/data/pscloud.log"
#endif
    );
    snprintf(googlepath,sizeof googlepath,"%s/google.conf",root);pscloud_google_load(googlepath,&google);
    pscloud_log_open(logpath);load_preferences();signal(SIGPIPE,SIG_IGN);signal(SIGTERM,stop_server);signal(SIGINT,stop_server);
    if(curl_global_init(CURL_GLOBAL_DEFAULT))return 2;
    int server=socket(AF_INET,SOCK_STREAM,0);if(server<0)return 2;
    int yes=1;setsockopt(server,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof yes);
    struct sockaddr_in address={0};address.sin_family=AF_INET;address.sin_port=htons((uint16_t)port);
    address.sin_addr.s_addr=htonl(INADDR_ANY);
    if(bind(server,(struct sockaddr *)&address,sizeof address) || listen(server,8)) {close(server);return 2;}
    socklen_t length=sizeof address;getsockname(server,(struct sockaddr *)&address,&length);
    printf("PSCLOUD_DASHBOARD_PORT=%u\nPSCLOUD_DASHBOARD_READY=1\n",ntohs(address.sin_port));fflush(stdout);
    pscloud_notify("PSCloud dashboard ready on port %u - open in your browser",ntohs(address.sin_port));
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
