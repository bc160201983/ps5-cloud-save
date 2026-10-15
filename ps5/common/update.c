/* Online updates. The newest published release of PSCloud is asked from GitHub; its "latest
 * release" never includes drafts or pre-releases, so a version reaches consoles only once it is
 * published. The dashboard file is downloaded over HTTPS with certificate checks and accepted only
 * when its SHA-256 matches the release's SHA256SUMS.txt. Installing sends it to the PS5's own ELF
 * loader (port 9021); the new dashboard then takes over from this one, as when it is sent from a
 * PC. Without a running loader nothing can start it, so every verified download is also kept as
 * <root>/pscloud-dashboard.elf: an autoloader pointed at that file starts the newest version when
 * the PS5 starts. Nothing is downloaded or installed without the user asking. */
#ifndef __FreeBSD__
#define _POSIX_C_SOURCE 200809L
#endif
#include "update.h"
#include "log.h"
#include <curl/curl.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define REPO "bc160201983/ps5-cloud-save"
#define ELF_NAME "pscloud-dashboard.elf"
#define SUMS_NAME "SHA256SUMS.txt"
#define ELF_LIMIT (64u*1024*1024)
#define CHECK_EVERY (24*3600)
#define RETRY_AFTER (6*3600)

enum {U_IDLE,U_CHECKING,U_CURRENT,U_AVAILABLE,U_DOWNLOADING,U_READY,U_INSTALLING,U_FAILED};
static const char *const STATE_NAMES[]={"idle","checking","current","available","downloading","ready","installing","failed"};
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static struct {
    int state,auto_check,files,newer;
    char root[1100],ca[1200],current[32],latest[32],notified[32],notes[4096],error[600],path[1300];
    unsigned long long done,total;time_t checked,attempted;
} U;

static int busy(void) {return U.state==U_CHECKING||U.state==U_DOWNLOADING||U.state==U_INSTALLING;}
/* "v0.34.27" or "0.34.27": two to four numbers. */
static int version_parts(const char *v,unsigned out[4]) {
    if(*v=='v')v++;
    unsigned n=0;memset(out,0,4*sizeof *out);
    while(n<4) {
        if(*v<'0'||*v>'9')return -1;
        unsigned long x=0;while(*v>='0'&&*v<='9') {x=x*10+(unsigned)(*v-'0');if(x>999999)return -1;v++;}
        out[n++]=(unsigned)x;
        if(!*v)break;
        if(*v!='.')return -1;
        v++;
    }
    return *v||n<2?-1:(int)n;
}
static int version_compare(const char *a,const char *b) {
    unsigned x[4],y[4];if(version_parts(a,x)<0||version_parts(b,y)<0)return 0;
    for(int i=0;i<4;i++)if(x[i]!=y[i])return x[i]>y[i]?1:-1;
    return 0;
}
static void save_config(void) {
    char path[1200],temporary[1300],text[128];snprintf(path,sizeof path,"%s/update.conf",U.root);snprintf(temporary,sizeof temporary,"%s.tmp",path);
    int n=snprintf(text,sizeof text,"AUTO=%d\nNOTIFIED=%s\n",U.auto_check,U.notified);
    int fd=open(temporary,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);
    if(fd<0)return;
    int bad=write(fd,text,(size_t)n)!=n||fsync(fd);close(fd);
    if(bad||rename(temporary,path))unlink(temporary);
}
static void load_config(void) {
    char path[1200],text[256];snprintf(path,sizeof path,"%s/update.conf",U.root);U.auto_check=1;U.notified[0]=0;
    int fd=open(path,O_RDONLY|O_NOFOLLOW);if(fd<0)return;
    ssize_t n=read(fd,text,sizeof text-1);close(fd);if(n<=0)return;
    text[n]=0;
    char *a=strstr(text,"AUTO=");if(a)U.auto_check=a[5]!='0';
    char *v=strstr(text,"NOTIFIED=");
    if(v) {char seen[32]={0};if(sscanf(v+9,"%31[0-9.]",seen)==1)snprintf(U.notified,sizeof U.notified,"%s",seen);}
}
static void urls(char *api,size_t api_max,char *files,size_t files_max) {
#ifdef PSCLOUD_HOST_TEST
    const char *base=getenv("PSCLOUD_TEST_UPDATE_URL");
    if(base) {snprintf(api,api_max,"%s/releases/latest",base);snprintf(files,files_max,"%s/download",base);return;}
#endif
    snprintf(api,api_max,"https://api.github.com/repos/" REPO "/releases/latest");
    snprintf(files,files_max,"https://github.com/" REPO "/releases/download");
}

/* ---------- HTTPS ---------- */
struct buffer {unsigned char *data;size_t size,cap,limit;int too_big;int progress;};
static size_t collect(char *p,size_t s,size_t n,void *user) {
    struct buffer *b=user;size_t add=s*n;
    if(b->size+add>b->limit) {b->too_big=1;return 0;}
    if(b->size+add+1>b->cap) {
        size_t cap=b->cap?b->cap:65536;while(cap<b->size+add+1)cap*=2;
        unsigned char *bigger=realloc(b->data,cap);if(!bigger)return 0;
        b->data=bigger;b->cap=cap;
    }
    memcpy(b->data+b->size,p,add);b->size+=add;b->data[b->size]=0;
    if(b->progress) {pthread_mutex_lock(&lock);U.done=b->size;pthread_mutex_unlock(&lock);}
    return add;
}
static int on_progress(void *user,curl_off_t total,curl_off_t now,curl_off_t up_total,curl_off_t up_now) {
    (void)user;(void)now;(void)up_total;(void)up_now;
    if(total>0) {pthread_mutex_lock(&lock);U.total=(unsigned long long)total;pthread_mutex_unlock(&lock);}
    return 0;
}
/* HTTP status, or -1 with the reason in why. */
static long fetch(const char *url,struct buffer *b,int github_api,long timeout,char *why,size_t max) {
    CURL *c=curl_easy_init();if(!c) {snprintf(why,max,"Not enough memory");return -1;}
    const char *protocols="https";
#ifdef PSCLOUD_HOST_TEST
    if(getenv("PSCLOUD_TEST_UPDATE_URL"))protocols="http,https";
#endif
    struct curl_slist *headers=NULL;
    if(github_api) {headers=curl_slist_append(headers,"Accept: application/vnd.github+json");headers=curl_slist_append(headers,"X-GitHub-Api-Version: 2022-11-28");}
    curl_easy_setopt(c,CURLOPT_URL,url);
    curl_easy_setopt(c,CURLOPT_PROTOCOLS_STR,protocols);curl_easy_setopt(c,CURLOPT_REDIR_PROTOCOLS_STR,protocols);
    curl_easy_setopt(c,CURLOPT_FOLLOWLOCATION,1L);curl_easy_setopt(c,CURLOPT_MAXREDIRS,5L);
    curl_easy_setopt(c,CURLOPT_USERAGENT,"PSCloud-updater");
    if(headers)curl_easy_setopt(c,CURLOPT_HTTPHEADER,headers);
    struct stat st;if(*U.ca&&!stat(U.ca,&st))curl_easy_setopt(c,CURLOPT_CAINFO,U.ca);
    curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,10L);curl_easy_setopt(c,CURLOPT_TIMEOUT,timeout);
    curl_easy_setopt(c,CURLOPT_LOW_SPEED_LIMIT,512L);curl_easy_setopt(c,CURLOPT_LOW_SPEED_TIME,60L);
    curl_easy_setopt(c,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,collect);curl_easy_setopt(c,CURLOPT_WRITEDATA,b);
    if(b->progress) {curl_easy_setopt(c,CURLOPT_XFERINFOFUNCTION,on_progress);curl_easy_setopt(c,CURLOPT_NOPROGRESS,0L);}
    CURLcode result=curl_easy_perform(c);long status=0;curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);
    curl_easy_cleanup(c);curl_slist_free_all(headers);
    if(b->too_big) {snprintf(why,max,"The download was larger than expected");return -1;}
    if(result!=CURLE_OK) {
        if(result==CURLE_PEER_FAILED_VERIFICATION||result==CURLE_SSL_CACERT_BADFILE)snprintf(why,max,"The HTTPS certificate could not be checked: the trusted certificate bundle (%.120s) is missing or out of date",U.ca);
        else if(result==CURLE_COULDNT_RESOLVE_HOST||result==CURLE_COULDNT_CONNECT||result==CURLE_OPERATION_TIMEDOUT)snprintf(why,max,"GitHub could not be reached. Check that the PS5 is connected to the internet");
        else snprintf(why,max,"Download failed: %s",curl_easy_strerror(result));
        return -1;
    }
    return status;
}

/* ---------- release details ---------- */
static void put_utf8(char *out,size_t *n,size_t max,unsigned cp) {
    unsigned char b[4];size_t k=0;
    if(cp<0x80)b[k++]=(unsigned char)cp;
    else if(cp<0x800) {b[k++]=(unsigned char)(0xc0|cp>>6);b[k++]=(unsigned char)(0x80|(cp&63));}
    else if(cp<0x10000) {b[k++]=(unsigned char)(0xe0|cp>>12);b[k++]=(unsigned char)(0x80|((cp>>6)&63));b[k++]=(unsigned char)(0x80|(cp&63));}
    else {b[k++]=(unsigned char)(0xf0|cp>>18);b[k++]=(unsigned char)(0x80|((cp>>12)&63));b[k++]=(unsigned char)(0x80|((cp>>6)&63));b[k++]=(unsigned char)(0x80|(cp&63));}
    if(*n+k<max) {memcpy(out+*n,b,k);*n+=k;}
}
static int hex4(const char *p,unsigned *v) {
    *v=0;for(int i=0;i<4;i++) {char c=p[i];unsigned d=c>='0'&&c<='9'?(unsigned)(c-'0'):c>='a'&&c<='f'?(unsigned)(c-'a'+10):c>='A'&&c<='F'?(unsigned)(c-'A'+10):16;if(d>15)return -1;*v=*v*16+d;}
    return 0;
}
/* The first string value of "key" in a JSON text (longer values are cut at max). */
static int json_text(const char *json,const char *key,char *out,size_t max) {
    char pattern[64];snprintf(pattern,sizeof pattern,"\"%s\"",key);
    const char *p=strstr(json,pattern);if(!p)return -1;
    p+=strlen(pattern);while(*p==' '||*p=='\t'||*p=='\r'||*p=='\n')p++;
    if(*p++!=':')return -1;
    while(*p==' '||*p=='\t'||*p=='\r'||*p=='\n')p++;
    if(*p++!='"')return -1;
    size_t n=0;
    while(*p&&*p!='"') {
        if(*p!='\\') {if(n+1<max)out[n++]=*p;p++;continue;}
        p++;unsigned cp;
        switch(*p) {
            case 'n':cp='\n';break;case 't':cp='\t';break;case 'r':cp='\r';break;case 'b':cp='\b';break;case 'f':cp='\f';break;
            case '/':cp='/';break;case '\\':cp='\\';break;case '"':cp='"';break;
            case 'u':
                if(hex4(p+1,&cp))return -1;
                p+=4;
                if(cp>=0xd800&&cp<0xdc00&&p[1]=='\\'&&p[2]=='u') {unsigned low;if(!hex4(p+3,&low)&&low>=0xdc00&&low<0xe000) {cp=0x10000+((cp-0xd800)<<10)+(low-0xdc00);p+=6;}}
                break;
            default:return -1;
        }
        put_utf8(out,&n,max,cp);p++;
    }
    if(*p!='"')return -1;
    out[n]=0;return 0;
}
static void escape_json(char *out,size_t max,const char *in) {
    size_t n=0;
    for(;*in&&n+7<max;in++) {
        unsigned char c=(unsigned char)*in;
        if(c=='"'||c=='\\') {out[n++]='\\';out[n++]=(char)c;}
        else if(c=='\n') {out[n++]='\\';out[n++]='n';}
        else if(c<32) {snprintf(out+n,7,"\\u%04x",c);n+=6;}
        else out[n++]=(char)c;
    }
    out[n]=0;
}

/* ---------- check ---------- */
int pscloud_update_check(int notify,char *message,size_t max) {
    pthread_mutex_lock(&lock);
    if(busy()) {pthread_mutex_unlock(&lock);snprintf(message,max,"An update step is already running");return -1;}
    int previous=U.state;U.state=U_CHECKING;U.error[0]=0;U.attempted=time(NULL);
    pthread_mutex_unlock(&lock);
    char api[300],files[300],why[240]="";urls(api,sizeof api,files,sizeof files);
    struct buffer b={.limit=2u*1024*1024};
    long status=fetch(api,&b,1,30,why,sizeof why);
    char tag[40]="",latest[32]="",notes[4096]="";int ok=0,has_files=0;
    if(status==404)snprintf(why,sizeof why,"No PSCloud release has been published yet");
    else if(status==403||status==429)snprintf(why,sizeof why,"GitHub's limit for update checks was reached. Try again in an hour");
    else if(status>=0&&status!=200)snprintf(why,sizeof why,"GitHub answered with HTTP %ld",status);
    else if(status==200) {
        unsigned parts[4];
        if(!b.data||json_text((char *)b.data,"tag_name",tag,sizeof tag)||version_parts(tag,parts)<0)snprintf(why,sizeof why,"The release information could not be read");
        else {
            snprintf(latest,sizeof latest,"%.30s",tag[0]=='v'?tag+1:tag);
            if(json_text((char *)b.data,"body",notes,sizeof notes))notes[0]=0;
            has_files=strstr((char *)b.data,"\"" ELF_NAME "\"")&&strstr((char *)b.data,"\"" SUMS_NAME "\"");
            ok=1;
        }
    }
    free(b.data);
    int tell=0;
    pthread_mutex_lock(&lock);
    if(!ok) {U.state=previous==U_READY?U_READY:U_FAILED;snprintf(U.error,sizeof U.error,"%s",why);}
    else {
        U.checked=time(NULL);U.files=has_files;U.newer=version_compare(latest,U.current)>0;
        if(strcmp(U.latest,latest))U.path[0]=0;  /* a downloaded file belongs to its version */
        snprintf(U.latest,sizeof U.latest,"%s",latest);snprintf(U.notes,sizeof U.notes,"%s",notes);
        U.state=!U.newer?U_CURRENT:*U.path?U_READY:U_AVAILABLE;
        if(U.newer&&notify&&strcmp(U.notified,latest)) {snprintf(U.notified,sizeof U.notified,"%s",latest);save_config();tell=1;}
    }
    char current[32];snprintf(current,sizeof current,"%s",U.current);int newer=U.newer;
    pthread_mutex_unlock(&lock);
    if(!ok) {pscloud_log("INFO","Update check did not finish: %s",why);snprintf(message,max,"%s",why);return -1;}
    pscloud_log(newer?"EVENT":"INFO",newer?"PSCloud %s is available (this is %s)":"PSCloud is up to date (latest release %s, this is %s)",latest,current);
    if(tell)pscloud_notify("PSCloud %s is available (you have %s). Open the dashboard to update",latest,current);
    if(newer)snprintf(message,max,"PSCloud %s is available",latest);else snprintf(message,max,"PSCloud %s is up to date",current);
    return 0;
}
static void *check_main(void *arg) {(void)arg;char message[240];(void)pscloud_update_check(1,message,sizeof message);return NULL;}
static int start_thread(void *(*fn)(void *)) {
    pthread_t t;pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,256U*1024);pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
    int bad=pthread_create(&t,&attr,fn,NULL);pthread_attr_destroy(&attr);return bad?-1:0;
}
void pscloud_update_tick(void) {
#ifdef PSCLOUD_HOST_TEST
    if(!getenv("PSCLOUD_TEST_UPDATE_URL"))return;  /* host tests never contact GitHub */
#endif
    pthread_mutex_lock(&lock);time_t now=time(NULL);
    int due=U.auto_check&&!busy()&&now-U.checked>=CHECK_EVERY&&now-U.attempted>=(U.attempted>U.checked?RETRY_AFTER:CHECK_EVERY);
    pthread_mutex_unlock(&lock);
    if(due)(void)start_thread(check_main);
}

/* ---------- download ---------- */
static void fail(const char *format,const char *detail) {
    pthread_mutex_lock(&lock);snprintf(U.error,sizeof U.error,format,detail);U.state=U_FAILED;pthread_mutex_unlock(&lock);
    pscloud_log("WARN","Update: %s",U.error);
}
static int sha256_hex(const unsigned char *data,size_t size,char out[65]) {
    unsigned char digest[32];unsigned length=0;
    if(!EVP_Digest(data,size,digest,&length,EVP_sha256(),NULL)||length!=32)return -1;
    for(unsigned i=0;i<32;i++)snprintf(out+2*i,3,"%02x",digest[i]);
    return 0;
}
static void remove_downloads(const char *keep) {
    char folder[1200];snprintf(folder,sizeof folder,"%s/update",U.root);DIR *d=opendir(folder);struct dirent *e;
    while(d&&(e=readdir(d))) {
        if(strncmp(e->d_name,"pscloud-dashboard-",18))continue;
        char path[1500];snprintf(path,sizeof path,"%s/%s",folder,e->d_name);
        if(!keep||strcmp(path,keep))unlink(path);
    }
    if(d)closedir(d);
}
static void *download_main(void *arg) {
    (void)arg;char api[300],files[300],url[500],why[240]="",latest[32];urls(api,sizeof api,files,sizeof files);
    pthread_mutex_lock(&lock);snprintf(latest,sizeof latest,"%s",U.latest);pthread_mutex_unlock(&lock);
    /* The checksum list first: the dashboard file is accepted only when it matches. */
    struct buffer sums={.limit=65536};snprintf(url,sizeof url,"%s/v%s/" SUMS_NAME,files,latest);
    long status=fetch(url,&sums,0,60,why,sizeof why);
    char expected[65]="";
    for(char *line=sums.data?(char *)sums.data:NULL;status==200&&line&&*line;) {
        char *end=strchr(line,'\n');if(end)*end=0;
        /* "<sha256>  <name>" (sha256sum's text mode) or "<sha256> *<name>" (binary mode). */
        if(strspn(line,"0123456789abcdef")==64&&(line[64]==' '||line[64]=='\t')) {
            const char *name=line+64;while(*name==' '||*name=='\t')name++;
            if(*name=='*')name++;
            if(strcspn(name,"\r")==strlen(ELF_NAME)&&!strncmp(name,ELF_NAME,strlen(ELF_NAME))) {memcpy(expected,line,64);expected[64]=0;}
        }
        line=end?end+1:NULL;
    }
    free(sums.data);
    if(status!=200||!*expected) {fail("%s",*why?why:"The release has no checksum for the dashboard file, so it was not downloaded");return NULL;}
    struct buffer elf={.limit=ELF_LIMIT,.progress=1};snprintf(url,sizeof url,"%s/v%s/" ELF_NAME,files,latest);
    status=fetch(url,&elf,0,900,why,sizeof why);
    char actual[65]="";
    if(status!=200) {free(elf.data);fail("%s",*why?why:"The dashboard file could not be downloaded");return NULL;}
    if(elf.size<64||memcmp(elf.data,"\177ELF",4)) {free(elf.data);fail("%s","The downloaded file is not a PS5 program");return NULL;}
    if(sha256_hex(elf.data,elf.size,actual)||strcmp(actual,expected)) {free(elf.data);fail("%s","The downloaded file does not match the release's checksum, so it was discarded");return NULL;}
    char folder[1200],path[1300],temporary[1400];snprintf(folder,sizeof folder,"%s/update",U.root);(void)mkdir(folder,0700);
    snprintf(path,sizeof path,"%s/pscloud-dashboard-%s.elf",folder,latest);snprintf(temporary,sizeof temporary,"%s.part",path);
    int fd=open(temporary,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);size_t at=0;int bad=fd<0;
    while(!bad&&at<elf.size) {ssize_t n=write(fd,elf.data+at,elf.size-at);if(n<0&&errno==EINTR)continue;if(n<=0)bad=1;else at+=(size_t)n;}
    if(fd>=0) {if(fsync(fd))bad=1;if(close(fd))bad=1;}
    if(bad||rename(temporary,path)) {unlink(temporary);free(elf.data);fail("%s","The update could not be stored on the PS5");return NULL;}
    /* The same verified file at a fixed place, for an autoloader. */
    char stable[1200],stable_part[1300];snprintf(stable,sizeof stable,"%s/" ELF_NAME,U.root);snprintf(stable_part,sizeof stable_part,"%s.part",stable);
    fd=open(stable_part,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0644);at=0;bad=fd<0;
    while(!bad&&at<elf.size) {ssize_t n=write(fd,elf.data+at,elf.size-at);if(n<0&&errno==EINTR)continue;if(n<=0)bad=1;else at+=(size_t)n;}
    if(fd>=0) {if(fsync(fd))bad=1;if(close(fd))bad=1;}
    if(bad||rename(stable_part,stable)) {unlink(stable_part);pscloud_log("WARN","Update: the copy for autoloaders (%s) could not be written",stable);}
    else pscloud_log("INFO","Update: %s now holds PSCloud %s for autoloaders",stable,latest);
    size_t size=elf.size;free(elf.data);remove_downloads(path);
    pthread_mutex_lock(&lock);snprintf(U.path,sizeof U.path,"%s",path);U.state=U_READY;U.done=U.total=size;pthread_mutex_unlock(&lock);
    pscloud_log("EVENT","Downloaded PSCloud %s (%zu bytes, SHA-256 matches the release)",latest,size);
    pscloud_notify("PSCloud %s is downloaded. Open the dashboard to install it",latest);
    return NULL;
}
int pscloud_update_download(char *message,size_t max) {
    pthread_mutex_lock(&lock);int result=-1;
    if(busy())snprintf(message,max,"An update step is already running");
    else if(!U.newer||!*U.latest)snprintf(message,max,"There is no newer version to download. Check for updates first");
    else if(!U.files)snprintf(message,max,"This release has no files for automatic download. Open its page to get it");
    else {U.state=U_DOWNLOADING;U.error[0]=0;U.done=U.total=0;
        if(start_thread(download_main)) {U.state=U_FAILED;snprintf(message,max,"The download could not start");}
        else {snprintf(message,max,"Downloading PSCloud %s",U.latest);result=0;}}
    pthread_mutex_unlock(&lock);return result;
}

/* ---------- install ---------- */
static unsigned loader_port(void) {
#ifdef PSCLOUD_HOST_TEST
    if(getenv("PSCLOUD_TEST_LOADER_PORT"))return (unsigned)strtoul(getenv("PSCLOUD_TEST_LOADER_PORT"),NULL,10);
#endif
    return 9021;
}
static void *install_main(void *arg) {
    (void)arg;sleep(1);  /* let the page receive its answer first */
    char path[1300],latest[32];pthread_mutex_lock(&lock);snprintf(path,sizeof path,"%s",U.path);snprintf(latest,sizeof latest,"%s",U.latest);pthread_mutex_unlock(&lock);
    int in=open(path,O_RDONLY|O_NOFOLLOW);struct stat st;
    if(in<0||fstat(in,&st)||!S_ISREG(st.st_mode)||st.st_size<64||st.st_size>(off_t)ELF_LIMIT) {if(in>=0)close(in);fail("%s","The downloaded update is missing. Download it again");return NULL;}
    int sock=socket(AF_INET,SOCK_STREAM,0);struct sockaddr_in to;memset(&to,0,sizeof to);to.sin_family=AF_INET;to.sin_port=htons((unsigned short)loader_port());to.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    struct timeval wait={30,0};
    if(sock>=0) {setsockopt(sock,SOL_SOCKET,SO_SNDTIMEO,&wait,sizeof wait);setsockopt(sock,SOL_SOCKET,SO_RCVTIMEO,&wait,sizeof wait);}
    if(sock<0||connect(sock,(struct sockaddr *)&to,sizeof to)) {
        if(sock>=0)close(sock);
        close(in);
        char stable[1200];snprintf(stable,sizeof stable,"%s/" ELF_NAME,U.root);
        fail("No ELF loader answered on port 9021, so PSCloud cannot start the new version itself. It is saved as %.200s: an autoloader that starts PSCloud from that file runs it the next time the PS5 starts. Or start your ELF loader and press Install again, or save the file to your PC and send it with your loader",stable);
        return NULL;
    }
    pscloud_log("EVENT","Installing PSCloud %s: sending it to the ELF loader on this PS5; the new version takes over from this one",latest);
    char *chunk=malloc(65536);int bad=!chunk;off_t sent=0;
    while(!bad&&sent<st.st_size) {
        ssize_t n=read(in,chunk,65536);if(n<0&&errno==EINTR)continue;if(n<=0) {bad=1;break;}
        for(ssize_t at=0;!bad&&at<n;) {ssize_t w=send(sock,chunk+at,(size_t)(n-at),0);if(w<0&&errno==EINTR)continue;if(w<=0)bad=1;else at+=w;}
        sent+=n;
    }
    free(chunk);close(in);shutdown(sock,SHUT_WR);
    char reply[512];while(!bad&&recv(sock,reply,sizeof reply,0)>0) {}
    close(sock);
    if(bad) {fail("%s","Sending the update to the ELF loader stopped before the end. Try again");return NULL;}
    /* The new dashboard stops this one. If it is still running after a minute, say so. */
    for(int i=0;i<60;i++)sleep(1);
    fail("%s","The new version did not take over within a minute. Check the PS5 for a message from the ELF loader");
    return NULL;
}
int pscloud_update_install(char *message,size_t max) {
    pthread_mutex_lock(&lock);int result=-1;struct stat st;
    if(busy())snprintf(message,max,"An update step is already running");
    else if(!*U.path||stat(U.path,&st))snprintf(message,max,"Download the update first");
    else {U.state=U_INSTALLING;U.error[0]=0;
        if(start_thread(install_main)) {U.state=U_FAILED;snprintf(message,max,"The installation could not start");}
        else {snprintf(message,max,"Installing PSCloud %s. The page reloads when the new version is running",U.latest);result=0;}}
    pthread_mutex_unlock(&lock);return result;
}

/* ---------- state ---------- */
int pscloud_update_set_auto(int on) {
    pthread_mutex_lock(&lock);U.auto_check=!!on;save_config();pthread_mutex_unlock(&lock);
    pscloud_log("EVENT","Automatic update checks %s",on?"on":"off");return 0;
}
int pscloud_update_file(char *path,size_t max) {
    pthread_mutex_lock(&lock);struct stat st;int ok=*U.path&&!stat(U.path,&st);if(ok)snprintf(path,max,"%s",U.path);pthread_mutex_unlock(&lock);
    return ok?0:-1;
}
void pscloud_update_json(char *out,size_t max) {
    static char notes[9000];char error[1400];
    pthread_mutex_lock(&lock);
    escape_json(notes,sizeof notes,U.notes);escape_json(error,sizeof error,U.error);
    struct stat st;int ready=*U.path&&!stat(U.path,&st);
    snprintf(out,max,"{\"current\":\"%s\",\"latest\":\"%s\",\"state\":\"%s\",\"newer\":%s,\"files\":%s,\"ready\":%s,\"auto\":%s,\"checked\":%lld,\"done\":%llu,\"total\":%llu,\"page\":\"%s%s%s\",\"notes\":\"%s\",\"error\":\"%s\"}",
        U.current,U.latest,STATE_NAMES[U.state],U.newer?"true":"false",U.files?"true":"false",ready?"true":"false",U.auto_check?"true":"false",(long long)U.checked,U.done,U.total,
        *U.latest?"https://github.com/" REPO "/releases/tag/v":"https://github.com/" REPO "/releases",*U.latest?U.latest:"","",notes,error);
    pthread_mutex_unlock(&lock);
}
void pscloud_update_init(const char *root,const char *ca,const char *current) {
    pthread_mutex_lock(&lock);
    memset(&U,0,sizeof U);snprintf(U.root,sizeof U.root,"%s",root);snprintf(U.ca,sizeof U.ca,"%s",ca);snprintf(U.current,sizeof U.current,"%s",current);
    load_config();
    pthread_mutex_unlock(&lock);
    remove_downloads(NULL);  /* a download is installed or replaced by a newer one before it is used */
}
