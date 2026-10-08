#ifndef __FreeBSD__
#define _POSIX_C_SOURCE 200809L
#endif
#include "common/cloud.h"
#include "common/snapshot.h"
#include "common/restore.h"
#include "common/managed.h"
#include "common/log.h"
#include "ui.h"
#include <curl/curl.h>
#include <sys/socket.h>
#include <sys/stat.h>
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
#ifndef PSCLOUD_HOST_TEST
#include <ps5/kernel.h>
#endif
extern int pscloud_backup_main(int,char **);
extern int pscloud_worker_main(int,char **);
extern int pscloud_download_main(int,char **);
static char root[1024],home[1024],cloudpath[1200],logpath[1200],token[33];
static int cloud_status;
static volatile sig_atomic_t stopped;
static void stop_server(int sig) {(void)sig;stopped=1;}
struct response {char *data;size_t size,limit;};
static size_t collect(char *p,size_t a,size_t b,void *ctx) {
    struct response *r=ctx;if(b && a>r->limit/b)return 0;size_t n=a*b;
    if(n>r->limit-r->size)return 0;
    memcpy(r->data+r->size,p,n);r->size+=n;r->data[r->size]=0;return n;
}
static long webdav(const struct settings *s,const char *url,const char *method,struct response *r) {
    CURL *c=curl_easy_init();if(!c)return 0;
    curl_easy_setopt(c,CURLOPT_URL,url);curl_easy_setopt(c,CURLOPT_PROTOCOLS_STR,"https");
    curl_easy_setopt(c,CURLOPT_USERNAME,s->user);curl_easy_setopt(c,CURLOPT_PASSWORD,s->password);
    curl_easy_setopt(c,CURLOPT_CAINFO,s->ca);curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,10L);
    curl_easy_setopt(c,CURLOPT_TIMEOUT,25L);curl_easy_setopt(c,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(c,CURLOPT_CUSTOMREQUEST,method);curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,collect);
    curl_easy_setopt(c,CURLOPT_WRITEDATA,r);struct curl_slist *headers=NULL;
    if(!strcmp(method,"PROPFIND")) {headers=curl_slist_append(headers,"Depth: 1");curl_easy_setopt(c,CURLOPT_HTTPHEADER,headers);}
    CURLcode result=curl_easy_perform(c);long status=0;curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);
    curl_slist_free_all(headers);curl_easy_cleanup(c);return result==CURLE_OK?status:0;
}
static int safe_word(const char *s,size_t max) {
    if(!*s || strlen(s)>max)return 0;
    for(;*s;s++)if(!strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-",*s))return 0;
    return 1;
}
static int valid_archive(const char *s) {
    const char *prefix="ps5-11.40-PPSA02433-";size_t n=strlen(prefix);
    if(strlen(s)!=n+36 || strncmp(s,prefix,n) || strcmp(s+n+32,".zip"))return 0;
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
    return !pscloud_snapshot_valid(s) || strcmp(s->title,"PPSA02433") ||
        (strcmp(s->slot,"PlayerSaveSlot0Save") && strcmp(s->slot,"PlayerSaveProfileSaveData"))?-1:0;
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
    char header[768];int n=snprintf(header,sizeof header,"HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\nContent-Security-Policy: default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; frame-ancestors 'none'\r\n\r\n",status,status==200?"OK":"Error",type,size);
    send_all(sock,header,(size_t)n);send_all(sock,data,size);
}
static void message(int sock,int status,const char *text) {
    char json[1024];snprintf(json,sizeof json,"{\"ok\":%s,\"message\":\"%s\"}",status==200?"true":"false",text);
    respond(sock,status,"application/json",json,strlen(json));
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
    char json[32768]="{\"games\":[";size_t pos=strlen(json);unsigned count=0;
    int dir=pscloud_open_directory(home);if(dir<0) {message(sock,500,"User save folders unavailable");return;}
    int scan=openat(dir,".",O_RDONLY | O_DIRECTORY);DIR *users=scan>=0?fdopendir(scan):NULL;
    if(!users) {if(scan>=0)close(scan);close(dir);message(sock,500,"User enumeration failed");return;}
    struct dirent *u;
    while((u=readdir(users)) && count<64) {
        if(!safe_word(u->d_name,16))continue;
        char path[1400];snprintf(path,sizeof path,"%s/%s/savedata_prospero",home,u->d_name);
        int fd=pscloud_open_directory(path);if(fd<0)continue;
        DIR *titles=fdopendir(fd);if(!titles) {close(fd);continue;}struct dirent *t;
        while((t=readdir(titles)) && count<64) {
            if(strlen(t->d_name)!=9 || strncmp(t->d_name,"PPSA",4) || !safe_word(t->d_name,9))continue;
            int titlefd=openat(fd,t->d_name,O_RDONLY | O_DIRECTORY | O_NOFOLLOW);if(titlefd<0)continue;
            DIR *slots=fdopendir(titlefd);if(!slots) {close(titlefd);continue;}struct dirent *e;
            while((e=readdir(slots)) && count<64) {
                if(strncmp(e->d_name,"sdimg_",6) || !strncmp(e->d_name,"sdimg_sce_bu_",13) || !safe_word(e->d_name+6,63))continue;
                int supported=!strcmp(t->d_name,"PPSA02433") && (!strcmp(e->d_name+6,"PlayerSaveSlot0Save") || !strcmp(e->d_name+6,"PlayerSaveProfileSaveData"));
                int written=snprintf(json+pos,sizeof json-pos,"%s{\"user\":\"%s\",\"title\":\"%s\",\"slot\":\"%s\",\"name\":\"%s\",\"supported\":%s}",count?",":"",u->d_name,t->d_name,e->d_name+6,!strcmp(t->d_name,"PPSA02433")?"Crash Bandicoot 4":t->d_name,supported?"true":"false");
                if(written<0 || (size_t)written>=sizeof json-pos-4)break;
                pos+=(size_t)written;count++;
            }
            closedir(slots);
        }
        closedir(titles);
    }
    closedir(users);close(dir);strcat(json,"]}");respond(sock,200,"application/json",json,strlen(json));
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
static int upload_queue(void) {
    struct settings s={0};if(pscloud_configure(cloudpath,&s))return 2;
    if(setenv("PSCLOUD_URL",s.url,1) || setenv("PSCLOUD_USER",s.user,1) || setenv("PSCLOUD_PASSWORD",s.password,1) || setenv("PSCLOUD_CA_BUNDLE",s.ca,1))return 2;
    char spool[1400];snprintf(spool,sizeof spool,"%s/spool",root);
    char *args[]={"worker",spool,"--once",NULL};int result=pscloud_worker_main(3,args);unsetenv("PSCLOUD_PASSWORD");
    signal(SIGTERM,stop_server);signal(SIGINT,stop_server);return result;
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
    struct pscloud_snapshot chosen;char closed[8];
    if(!strcmp(path,"/api/sync")) {int result=upload_queue();message(sock,result?502:200,result?"Upload pending; local backups retained":"Queued backups uploaded");return;}
    if(selection(form,&chosen)) {message(sock,400,"Unsupported save selection");return;}
    if(!strcmp(path,"/api/backup")) {
        if(parameter(form,"closed",closed,sizeof closed) || strcmp(closed,"yes")) {message(sock,400,"Close the game and confirm before backup");return;}
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
        result=upload_queue();message(sock,result?502:200,result?"Backup retained locally; cloud upload pending":"Backup checked and cloud queue uploaded");return;
    }
    char file[128];struct settings cloud={0};struct pscloud_snapshot snapshot;
    if(parameter(form,"file",file,sizeof file) || pscloud_configure(cloudpath,&cloud) || remote_snapshot(&cloud,&chosen,file,&snapshot)) {message(sock,400,"Cloud backup identity could not be verified");return;}
    int restoring=!strcmp(path,"/api/restore");
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
static void serve(int sock) {
    char request[16385];size_t size=0;char *body=NULL;
    while(size<8192) {
        ssize_t n=recv(sock,request+size,8192-size,0);if(n<=0)return;size+=(size_t)n;request[size]=0;
        body=strstr(request,"\r\n\r\n");if(body)break;
    }
    if(!body) {message(sock,400,"Request headers too large");return;}
    size_t headers=(size_t)(body-request)+4;body+=4;
    char method[8],url[2048];if(sscanf(request,"%7s %2047s",method,url)!=2) {message(sock,400,"Malformed request");return;}
    if(!strcmp(method,"GET") && !strcmp(url,"/")) {respond(sock,200,"text/html; charset=utf-8",(const char *)pscloud_ui,pscloud_ui_size);return;}
    char supplied[64];
    if(header_value(request,"X-PSCloud-Token",supplied,sizeof supplied) || strcmp(supplied,token)) {message(sock,401,"Enter the pairing code shown by the PS5 payload");return;}
    char transfer[64];if(!header_value(request,"Transfer-Encoding",transfer,sizeof transfer)) {message(sock,400,"Chunked requests are not supported");return;}
    char length[32];size_t expected=0;
    if(!strcmp(method,"POST")) {
        if(header_value(request,"Content-Length",length,sizeof length)) {message(sock,400,"Content length missing");return;}
        char *end=NULL;unsigned long n=strtoul(length,&end,10);
        if(!*length || *end || n>8192 || headers+n>=sizeof request) {message(sock,400,"Request body too large");return;}expected=(size_t)n;
        while(size<headers+expected) {ssize_t got=recv(sock,request+size,headers+expected-size,0);if(got<=0)return;size+=(size_t)got;}
        request[headers+expected]=0;
    }
    char *query=strchr(url,'?');if(query)*query++=0;else query="";
    if(!strcmp(method,"GET") && !strcmp(url,"/api/state")) {
        struct settings s={0};int configured=pscloud_configure(cloudpath,&s)==0;
        char address[6144],username[512],json[8192];escaped(address,sizeof address,s.url);escaped(username,sizeof username,s.user);
        snprintf(json,sizeof json,"{\"version\":\"%s\",\"configured\":%s,\"connected\":%s,\"url\":\"%s\",\"username\":\"%s\"}",PSCLOUD_VERSION,configured?"true":"false",cloud_status==200 || cloud_status==207?"true":"false",address,username);
        respond(sock,200,"application/json",json,strlen(json));return;
    }
    if(!strcmp(method,"GET") && !strcmp(url,"/api/games")) {games(sock);return;}
    if(!strcmp(method,"GET") && !strcmp(url,"/api/backups")) {backups(sock,query);return;}
    if(!strcmp(method,"GET") && !strcmp(url,"/api/log")) {
        int fd=open(logpath,O_RDONLY | O_NOFOLLOW | O_NONBLOCK);struct stat st;char data[8193]={0};
        if(fd>=0) {if(!fstat(fd,&st) && S_ISREG(st.st_mode)) {off_t offset=st.st_size>8192?st.st_size-8192:0;ssize_t got=pread(fd,data,8192,offset);if(got>=0)data[got]=0;}close(fd);}
        char text[50000],json[50100];escaped(text,sizeof text,data);snprintf(json,sizeof json,"{\"log\":\"%s\"}",text);respond(sock,200,"application/json",json,strlen(json));return;
    }
    if(!strcmp(method,"POST") && !strcmp(url,"/api/connect")) {configure_cloud(sock,body);return;}
    if(!strcmp(method,"POST")) {action(sock,url,body);return;}
    message(sock,404,"Unknown endpoint");
}
int main(int argc,char **argv) {
    unsigned port=8082;
#ifdef PSCLOUD_HOST_TEST
    if(argc!=4)return 2;
    snprintf(root,sizeof root,"%s",argv[1]);snprintf(home,sizeof home,"%s",argv[2]);port=(unsigned)strtoul(argv[3],NULL,10);
#else
    (void)argc;(void)argv;
    if((kernel_get_fw_version()&0xffff0000U)!=0x11400000U)return 2;
    strcpy(root,"/data/pscloud");strcpy(home,"/user/home");
#endif
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
    pscloud_log_open(logpath);signal(SIGPIPE,SIG_IGN);signal(SIGTERM,stop_server);signal(SIGINT,stop_server);
    if(curl_global_init(CURL_GLOBAL_DEFAULT))return 2;
    int server=socket(AF_INET,SOCK_STREAM,0);if(server<0)return 2;
    int yes=1;setsockopt(server,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof yes);
    struct sockaddr_in address={0};address.sin_family=AF_INET;address.sin_port=htons((uint16_t)port);
    address.sin_addr.s_addr=htonl(INADDR_ANY);
    if(bind(server,(struct sockaddr *)&address,sizeof address) || listen(server,8)) {close(server);return 2;}
    socklen_t length=sizeof address;getsockname(server,(struct sockaddr *)&address,&length);
    printf("PSCLOUD_DASHBOARD_PORT=%u\nPSCLOUD_PAIRING_CODE=%s\n",ntohs(address.sin_port),token);fflush(stdout);
    pscloud_notify("PSCloud dashboard port %u; pairing code %s",ntohs(address.sin_port),token);
    while(!stopped) {
        fd_set readset;FD_ZERO(&readset);FD_SET(server,&readset);struct timeval wait={1,0};
        int ready=select(server+1,&readset,NULL,NULL,&wait);if(ready<=0)continue;
        int sock=accept(server,NULL,NULL);if(sock<0)continue;
        struct timeval timeout={15,0};setsockopt(sock,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);
        setsockopt(sock,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout);
        serve(sock);close(sock);
    }
    close(server);curl_global_cleanup();pscloud_log_close();return 0;
}
