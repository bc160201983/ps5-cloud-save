#ifndef __FreeBSD__
#define _POSIX_C_SOURCE 200809L
#endif
#include <curl/curl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <openssl/evp.h>
#include <stdatomic.h>
#include <time.h>
#include <errno.h>
#include "../ps5/common/transfer.h"
#ifdef PSCLOUD_EMBEDDED
#include "../ps5/common/log.h"
#include "../ps5/common/cloud.h"
#endif
#include "../ps5/common/snapshot.h"

/* Input contract: producer publishes an immutable archive by atomic rename
 * from *.part to a globally unique *.zip.ready in a private spool directory.
 * No live save directories are read or modified by this worker. */
static _Atomic int stopped;
static _Atomic unsigned transfer_sequence;
static _Atomic int transfer_phase,transfer_failed_phase,transfer_transport;
static _Atomic long transfer_http;
static _Atomic unsigned long long transfer_done,transfer_total;
static _Atomic unsigned char transfer_file[256];
static _Atomic unsigned file_generation;
void pscloud_worker_status(struct pscloud_transfer_status *s) {
    s->sequence=transfer_sequence;s->phase=transfer_phase;s->failed_phase=transfer_failed_phase;
    s->transport=transfer_transport;s->http=transfer_http;s->done=transfer_done;s->total=transfer_total;
    unsigned generation;
    do {generation=file_generation;if(generation&1)continue;for(unsigned i=0;i<256;i++)s->file[i]=(char)transfer_file[i];}while((generation&1) || generation!=file_generation);
}
void pscloud_worker_cancel(void) {stopped=1;}
void pscloud_worker_prepare(void) {stopped=0;transfer_phase=PSCLOUD_IDLE;transfer_done=0;transfer_total=0;transfer_transport=0;transfer_http=0;transfer_failed_phase=0;}
/* Bound active transfers by size, not a blanket five minutes. A separate
 * low-speed guard aborts stalls; a continuing large upload can take longer. */
long pscloud_transfer_timeout(unsigned long long bytes) {
    unsigned long long seconds=120+bytes/131072+(bytes%131072!=0);
    return seconds<300?300:seconds>7200?7200:(long)seconds;
}
static void phase(int value) {
    transfer_done=0;transfer_phase=value;
#ifdef PSCLOUD_EMBEDDED
    pscloud_log("INFO","Cloud transfer phase=%d",value);
#endif
}
static void transport(const char *operation,CURLcode rc,long status) {
    transfer_transport=(int)rc;transfer_http=status;
#ifdef PSCLOUD_EMBEDDED
    pscloud_log("INFO","Cloud %s: transport=%d HTTP=%ld",operation,(int)rc,status);
#else
    fprintf(stderr,"Cloud %s: transport=%d HTTP=%ld\n",operation,(int)rc,status);
#endif
}
struct upload_cursor {int fd;curl_off_t offset,size;};
static size_t read_archive(char *p,size_t a,size_t b,void *ctx) {
    struct upload_cursor *r=ctx;if(b && a>(size_t)-1/b)return CURL_READFUNC_ABORT;
    size_t n=a*b;if(!n)return 0;
    if(r->offset>=r->size)return 0;
    if((curl_off_t)n>r->size-r->offset)n=(size_t)(r->size-r->offset);
    ssize_t got;do {got=pread(r->fd,p,n,(off_t)r->offset);}while(got<0 && errno==EINTR);
    if(got<=0)return CURL_READFUNC_ABORT;
    r->offset+=got;return (size_t)got;
}
static int upload_progress(void *ctx,curl_off_t dt,curl_off_t dn,curl_off_t ut,curl_off_t un) {
    (void)ctx;(void)dt;(void)dn;(void)ut;transfer_done=un>0?(unsigned long long)un:0;
    return stopped?1:0;
}
#ifndef PSCLOUD_EMBEDDED
static void stop(int sig) { (void)sig; stopped=1; }
#endif
static size_t discard(char *p,size_t a,size_t b,void *ctx) {
    (void)p; (void)ctx; return a*b;
}
static int valid(const char *s) {
    size_t n=strlen(s);
    if(n<11 || n>200 || strcmp(s+n-10,".zip.ready")) return 0;
    for(size_t i=0;i<n;i++)
        if(!isalnum((unsigned char)s[i]) && s[i]!='-' && s[i]!='_' && s[i]!='.') return 0;
    return s[0]!='.';
}
struct remote_body {char text[1025];size_t size;};
struct remote_hash {EVP_MD_CTX *ctx;curl_off_t size,limit;};
static size_t hash_remote(char *p,size_t a,size_t b,void *ctx) {
    struct remote_hash *h=ctx;if(b && a>(size_t)-1/b)return 0;size_t n=a*b;
    if((curl_off_t)n<0 || (curl_off_t)n>h->limit-h->size || EVP_DigestUpdate(h->ctx,p,n)!=1)return 0;
    h->size+=(curl_off_t)n;transfer_done=(unsigned long long)h->size;return n;
}
/* 1 verified, 0 complete but different/missing, -1 cannot establish. Streaming
 * readback costs bandwidth but prevents a successful PUT from committing a
 * corrupt archive. No full second copy is allocated in console memory. */
static int verify_remote(const char *url,const char *user,const char *pass,const char *ca,
                         const char *expected,curl_off_t size) {
    CURL *c=curl_easy_init();EVP_MD_CTX *ctx=EVP_MD_CTX_new();
    if(!c || !ctx) {if(c)curl_easy_cleanup(c);EVP_MD_CTX_free(ctx);return -1;}
    if(EVP_DigestInit_ex(ctx,EVP_sha256(),NULL)!=1) {curl_easy_cleanup(c);EVP_MD_CTX_free(ctx);return -1;}
    struct remote_hash body={ctx,0,size};
    phase(PSCLOUD_READBACK);transfer_total=(unsigned long long)size;
    curl_easy_setopt(c,CURLOPT_URL,url);curl_easy_setopt(c,CURLOPT_PROTOCOLS_STR,"https");
    curl_easy_setopt(c,CURLOPT_USERNAME,user);curl_easy_setopt(c,CURLOPT_PASSWORD,pass);
    curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,15L);curl_easy_setopt(c,CURLOPT_TIMEOUT,pscloud_transfer_timeout((unsigned long long)size));curl_easy_setopt(c,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(c,CURLOPT_LOW_SPEED_LIMIT,1024L);curl_easy_setopt(c,CURLOPT_LOW_SPEED_TIME,60L);
    curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,hash_remote);curl_easy_setopt(c,CURLOPT_WRITEDATA,&body);
    if(ca && *ca)curl_easy_setopt(c,CURLOPT_CAINFO,ca);
    CURLcode rc=curl_easy_perform(c);long status=0;curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);transport("archive readback",rc,status);curl_easy_cleanup(c);
    unsigned char digest[32];unsigned length=0;int final=EVP_DigestFinal_ex(ctx,digest,&length);EVP_MD_CTX_free(ctx);
    if(status==404 && rc==CURLE_OK)return 0;
    if(rc!=CURLE_OK || status!=200 || final!=1 || length!=32)return -1;
    char hash[65];for(unsigned i=0;i<32;i++)snprintf(hash+2*i,3,"%02x",digest[i]);
    return body.size==size && !strcmp(hash,expected)?1:0;
}
static size_t collect_identity(char *p,size_t a,size_t b,void *ctx) {
    struct remote_body *r=ctx;if(b && a>1024/b)return 0;
    size_t n=a*b;if(n>1024-r->size)return 0;
    memcpy(r->text+r->size,p,n);r->size+=n;r->text[r->size]=0;return n;
}
/* 1: committed archive present, 0: missing/incomplete, -1: cannot establish.
 * Errors never count as deletion. No redirects or credential forwarding. */
static int cloud_copy(const char *archive,const char *identity,const char *user,const char *pass,const char *ca,
                      const struct pscloud_snapshot *s,curl_off_t size) {
    CURL *c=curl_easy_init();if(!c)return -1;
    struct remote_body body={{0},0};char expected[512];
    snprintf(expected,sizeof expected,"USER_ID=%s\nTITLE=%s\nSAVE_NAME=%s\nSHA256=%s\nCREATED_UNIX=%lld\n",s->user,s->title,s->slot,s->sha256,s->created);
    curl_easy_setopt(c,CURLOPT_URL,identity);curl_easy_setopt(c,CURLOPT_PROTOCOLS_STR,"https");
    curl_easy_setopt(c,CURLOPT_USERNAME,user);curl_easy_setopt(c,CURLOPT_PASSWORD,pass);
    curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,15L);curl_easy_setopt(c,CURLOPT_TIMEOUT,60L);curl_easy_setopt(c,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,collect_identity);curl_easy_setopt(c,CURLOPT_WRITEDATA,&body);
    if(ca && *ca)curl_easy_setopt(c,CURLOPT_CAINFO,ca);
    CURLcode rc=curl_easy_perform(c);long status=0;curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);transport("identity lookup",rc,status);
    if(rc!=CURLE_OK || (status!=200 && status!=404)) {curl_easy_cleanup(c);return -1;}
    if(status==404) {curl_easy_cleanup(c);return 0;}
    if(strcmp(body.text,expected)) {curl_easy_cleanup(c);return -1;}
    curl_easy_setopt(c,CURLOPT_URL,archive);curl_easy_setopt(c,CURLOPT_NOBODY,1L);
    rc=curl_easy_perform(c);curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);transport("archive presence",rc,status);
    curl_off_t length=-1;curl_easy_getinfo(c,CURLINFO_CONTENT_LENGTH_DOWNLOAD_T,&length);curl_easy_cleanup(c);
    if(rc!=CURLE_OK || (status!=200 && status!=404))return -1;
    if(status==404)return 0;
    if(length<0)return -1;
    return length==size?verify_remote(archive,user,pass,ca,s->sha256,size):0;
}
static int collection(const char *url,const char *user,const char *pass,const char *ca) {
    CURL *c=curl_easy_init();if(!c)return 1;
    curl_easy_setopt(c,CURLOPT_URL,url);curl_easy_setopt(c,CURLOPT_PROTOCOLS_STR,"https");
    curl_easy_setopt(c,CURLOPT_USERNAME,user);curl_easy_setopt(c,CURLOPT_PASSWORD,pass);
    curl_easy_setopt(c,CURLOPT_CUSTOMREQUEST,"MKCOL");curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,discard);
    curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,15L);curl_easy_setopt(c,CURLOPT_TIMEOUT,60L);curl_easy_setopt(c,CURLOPT_NOSIGNAL,1L);
    if(ca && *ca)curl_easy_setopt(c,CURLOPT_CAINFO,ca);
    CURLcode rc=curl_easy_perform(c);long status=0;curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);transport("folder preparation",rc,status);
    curl_easy_cleanup(c);return rc!=CURLE_OK || (status!=201 && status!=405);
}
static int put_identity(const char *url,const char *user,const char *pass,const char *ca,const struct pscloud_snapshot *s) {
    char text[512];snprintf(text,sizeof text,"USER_ID=%s\nTITLE=%s\nSAVE_NAME=%s\nSHA256=%s\nCREATED_UNIX=%lld\n",s->user,s->title,s->slot,s->sha256,s->created);
    CURL *c=curl_easy_init();if(!c)return 1;
    curl_easy_setopt(c,CURLOPT_URL,url);curl_easy_setopt(c,CURLOPT_PROTOCOLS_STR,"https");
    curl_easy_setopt(c,CURLOPT_USERNAME,user);curl_easy_setopt(c,CURLOPT_PASSWORD,pass);
    curl_easy_setopt(c,CURLOPT_CUSTOMREQUEST,"PUT");curl_easy_setopt(c,CURLOPT_POSTFIELDS,text);
    curl_easy_setopt(c,CURLOPT_POSTFIELDSIZE,(long)strlen(text));curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,discard);
    curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,15L);curl_easy_setopt(c,CURLOPT_TIMEOUT,60L);curl_easy_setopt(c,CURLOPT_NOSIGNAL,1L);
    if(ca && *ca)curl_easy_setopt(c,CURLOPT_CAINFO,ca);
    CURLcode rc=curl_easy_perform(c);long status=0;curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);transport("identity commit",rc,status);curl_easy_cleanup(c);
    return rc!=CURLE_OK || status<200 || status>=300;
}
static int replace_requested(int dir,const char *object,const char *hash) {
    char flag[272];snprintf(flag,sizeof flag,"%s.replace",object);
    int scan=openat(dir,".",O_RDONLY | O_DIRECTORY | O_NOFOLLOW);if(scan<0)return -1;
    DIR *d=fdopendir(scan);if(!d) {close(scan);return -1;}
    struct dirent *entry;int found=0;while((entry=readdir(d)))if(!strcmp(entry->d_name,flag)) {found=1;break;}
    closedir(d);if(!found)return 0;
    int fd=openat(dir,flag,O_RDONLY | O_NOFOLLOW | O_NONBLOCK);struct stat st;char text[65]={0};
    int bad=fd<0 || fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size!=64 || read(fd,text,64)!=64 || strcmp(text,hash);
    if(fd>=0)close(fd);
    return bad?-1:1;
}
static int upload(const char *base,const char *user,const char *pass,const char *ca,
                  const char *name,int dir) {
    file_generation++;size_t name_length=strlen(name);for(unsigned i=0;i<256;i++)transfer_file[i]=i<name_length?(unsigned char)name[i]:0;file_generation++;
    transfer_sequence++;transfer_failed_phase=0;transfer_transport=0;transfer_http=0;transfer_total=0;phase(PSCLOUD_LOCAL_CHECK);
    char queue_name[256];snprintf(queue_name,sizeof queue_name,"%s",name);name=queue_name;
    int fd=openat(dir,name,O_RDONLY|O_NOFOLLOW);
    struct stat st;
    if(fd<0) return 1;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<=0) {close(fd); return 1;}
    transfer_total=(unsigned long long)st.st_size;
    char local_hash[65];if(pscloud_file_hash(fd,local_hash)) {close(fd);return 1;}
    FILE *f=fdopen(fd,"rb");
    if(!f) {close(fd); return 1;}
    CURL *c=curl_easy_init();
    if(!c) {fclose(f); return 1;}
    char object[256],url[4096],destination[3600];
    snprintf(object,sizeof object,"%.*s",(int)strlen(name)-6,name); /* remove .ready */
    struct pscloud_snapshot snapshot={0};char identity[272];snprintf(identity,sizeof identity,"%s.identity",object);
    int meta=openat(dir,identity,O_RDONLY | O_NOFOLLOW | O_NONBLOCK);int structured=meta>=0;
    if(meta>=0)close(meta);
    if(structured && pscloud_snapshot_read(dir,identity,&snapshot)) {
        fprintf(stderr,"Invalid backup identity; upload retained: %s\n",object);
        curl_easy_cleanup(c);fclose(f);return 1;
    }
    if(snprintf(destination,sizeof destination,"%s",base)>=(int)sizeof destination) {
        curl_easy_cleanup(c);fclose(f);return 1;
    }
    int replace=0;
    if(structured) {
        char folder[256];
        if(pscloud_snapshot_folder(&snapshot,folder,sizeof folder) || strcmp(local_hash,snapshot.sha256)) {
            curl_easy_cleanup(c);fclose(f);return 1;
        }
        replace=replace_requested(dir,object,snapshot.sha256);
        if(replace<0) {curl_easy_cleanup(c);fclose(f);return 1;}
        phase(PSCLOUD_FOLDERS);
        char *state=NULL,*component=strtok_r(folder,"/",&state);
        while(component) {
            size_t have=strlen(destination),length=strlen(component);
            if(have+length+2>sizeof destination) {curl_easy_cleanup(c);fclose(f);return 1;}
            destination[have++]='/';memcpy(destination+have,component,length+1);
            if(collection(destination,user,pass,ca)) {curl_easy_cleanup(c);fclose(f);return 1;}
            component=strtok_r(NULL,"/",&state);
        }
    }
    if(snprintf(url,sizeof url,"%s/%s",destination,object)>=(int)sizeof url) {
        curl_easy_cleanup(c); fclose(f); return 1;
    }
    if(structured) {
        char manifest[4096];
        if(snprintf(manifest,sizeof manifest,"%s/.pscloud/%s.identity",destination,object)>=(int)sizeof manifest) {curl_easy_cleanup(c);fclose(f);return 1;}
        phase(PSCLOUD_PRESENCE);
        int present=cloud_copy(url,manifest,user,pass,ca,&snapshot,(curl_off_t)st.st_size);
        if(present<0) {fprintf(stderr,"Cloud presence check failed; backup retained: %s\n",object);curl_easy_cleanup(c);fclose(f);return 1;}
        if(present && !replace) {
            curl_easy_cleanup(c);fclose(f);char done[272];snprintf(done,sizeof done,"%s.sent",object);
            phase(PSCLOUD_COMMIT);if(renameat(dir,name,dir,done) || fsync(dir))return 1;
            phase(PSCLOUD_DONE);
            printf("Already in cloud; upload skipped: %s\n",object);fflush(stdout);return 0;
        }
    }
    curl_easy_setopt(c,CURLOPT_URL,url);
    curl_easy_setopt(c,CURLOPT_PROTOCOLS_STR,"https");
    curl_easy_setopt(c,CURLOPT_USERNAME,user);
    curl_easy_setopt(c,CURLOPT_PASSWORD,pass);
    curl_easy_setopt(c,CURLOPT_UPLOAD,1L);
    struct upload_cursor cursor={fd,0,(curl_off_t)st.st_size};
    curl_easy_setopt(c,CURLOPT_READFUNCTION,read_archive);curl_easy_setopt(c,CURLOPT_READDATA,&cursor);
    curl_easy_setopt(c,CURLOPT_UPLOAD_BUFFERSIZE,256L*1024);
    curl_easy_setopt(c,CURLOPT_XFERINFOFUNCTION,upload_progress);curl_easy_setopt(c,CURLOPT_NOPROGRESS,0L);
    curl_easy_setopt(c,CURLOPT_INFILESIZE_LARGE,(curl_off_t)st.st_size);
    curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,15L);
    curl_easy_setopt(c,CURLOPT_TIMEOUT,pscloud_transfer_timeout((unsigned long long)st.st_size));
    curl_easy_setopt(c,CURLOPT_LOW_SPEED_LIMIT,1024L);curl_easy_setopt(c,CURLOPT_LOW_SPEED_TIME,60L);
    curl_easy_setopt(c,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,discard);
    if(ca && *ca) curl_easy_setopt(c,CURLOPT_CAINFO,ca);
    phase(PSCLOUD_UPLOAD);CURLcode rc=curl_easy_perform(c);
    long status=0;
    curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);
    transport("archive upload",rc,status);
    curl_easy_cleanup(c); fclose(f);
    if(rc!=CURLE_OK || status<200 || status>=300) {
        fprintf(stderr,"Upload retained for retry: %s (transport=%d, HTTP=%ld)\n",object,rc,status);
        return 1;
    }
    if(verify_remote(url,user,pass,ca,local_hash,(curl_off_t)st.st_size)!=1) {
        fprintf(stderr,"Remote archive verification failed; local backup retained for retry: %s\n",object);return 1;
    }
    if(structured) {
        phase(PSCLOUD_COMMIT);
        char manifest[4096];
        char manifest_folder[4096];
        if(snprintf(manifest_folder,sizeof manifest_folder,"%s/.pscloud",destination)>=(int)sizeof manifest_folder || collection(manifest_folder,user,pass,ca) ||
           snprintf(manifest,sizeof manifest,"%s/.pscloud/%s.identity",destination,object)>=(int)sizeof manifest ||
           put_identity(manifest,user,pass,ca,&snapshot)) {
            fprintf(stderr,"Cloud identity commit failed; upload retained: %s\n",object);return 1;
        }
    }
    char done[272]; snprintf(done,sizeof done,"%s.sent",object);
    if(renameat(dir,name,dir,done)||fsync(dir)) {perror("queue commit"); return 1;}
    if(replace) {char flag[272];snprintf(flag,sizeof flag,"%s.replace",object);if(unlinkat(dir,flag,0) || fsync(dir))return 1;}
    printf("Uploaded: %s\n",object); fflush(stdout);
    phase(PSCLOUD_DONE);
    return 0;
}
static int worker_run(int argc,char **argv,const char *base,const char *user,const char *pass,const char *ca) {
    if((argc!=3 && argc!=4) || (strcmp(argv[2],"--once") && strcmp(argv[2],"--watch")) ||
       (argc==4 && (strcmp(argv[2],"--once") || !valid(argv[3])))) {
        fprintf(stderr,"Usage: %s SPOOL --once|--watch\n",argv[0]); return 2;
    }
    if(!base||strncmp(base,"https://",8)||strchr(base,'?')||strchr(base,'#')||
       !user||!pass||!*user||!*pass) {
        fprintf(stderr,"Set HTTPS PSCLOUD_URL (existing WebDAV folder), PSCLOUD_USER and PSCLOUD_PASSWORD.\n"); return 2;
    }
    int dir=open(argv[1],O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    if(dir<0) {perror("spool"); return 2;}
    int lock=openat(dir,".worker.lock",O_CREAT|O_RDWR|O_NOFOLLOW,0600);
    if(lock<0||flock(lock,LOCK_EX|LOCK_NB)) {fprintf(stderr,"Cannot lock spool\n");if(lock>=0)close(lock);close(dir);return 2;}
#ifndef PSCLOUD_EMBEDDED
    if(curl_global_init(CURL_GLOBAL_DEFAULT)) {close(lock);close(dir);return 2;}
    signal(SIGTERM,stop); signal(SIGINT,stop);
#endif
    unsigned delay=5; int failed=0;
    do {
        int scan=openat(dir,".",O_RDONLY|O_DIRECTORY);
        DIR *d=scan<0?NULL:fdopendir(scan);
        if(!d) {if(scan>=0)close(scan); failed=1; break;}
        struct dirent *e; failed=0;
        while(!stopped && (e=readdir(d)))
            if(valid(e->d_name) && (argc==3 || !strcmp(e->d_name,argv[3]))) {
                int result=upload(base,user,pass,ca,e->d_name,dir);failed|=result;
                if(result) {transfer_failed_phase=transfer_phase;transfer_phase=PSCLOUD_FAILED;}
            }
        closedir(d);
        if(!strcmp(argv[2],"--once")) break;
        for(unsigned i=0;i<delay&&!stopped;i++) sleep(1);
        delay=failed?(delay<150?delay*2:300):5;
    } while(!stopped);
    if(stopped)failed=1;
#ifndef PSCLOUD_EMBEDDED
    curl_global_cleanup();
#endif
    close(lock); close(dir);
    return failed?1:0;
}
#ifdef PSCLOUD_EMBEDDED
int pscloud_worker_upload(const char *spool,const char *file,const struct settings *s) {
    char *args[]={"worker",(char *)spool,"--once",(char *)file,NULL};
    int result=worker_run(file?4:3,args,s->url,s->user,s->password,s->ca);
    if(result && transfer_phase!=PSCLOUD_FAILED) {transfer_failed_phase=transfer_phase;transfer_phase=PSCLOUD_FAILED;}
    return result;
}
int pscloud_worker_main(int argc,char **argv) {
#else
int main(int argc,char **argv) {
#endif
    pscloud_worker_prepare();
#ifdef PSCLOUD_EMBEDDED
    if(curl_global_init(CURL_GLOBAL_DEFAULT))return 2;
    int result=worker_run(argc,argv,getenv("PSCLOUD_URL"),getenv("PSCLOUD_USER"),getenv("PSCLOUD_PASSWORD"),getenv("PSCLOUD_CA_BUNDLE"));
    curl_global_cleanup();return result;
#else
    return worker_run(argc,argv,getenv("PSCLOUD_URL"),getenv("PSCLOUD_USER"),getenv("PSCLOUD_PASSWORD"),getenv("PSCLOUD_CA_BUNDLE"));
#endif
}
