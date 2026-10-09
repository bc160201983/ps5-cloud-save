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
#include "../ps5/common/snapshot.h"

/* Input contract: producer publishes an immutable archive by atomic rename
 * from *.part to a globally unique *.zip.ready in a private spool directory.
 * No live save directories are read or modified by this worker. */
static volatile sig_atomic_t stopped;
static void stop(int sig) { (void)sig; stopped=1; }
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
    CURLcode rc=curl_easy_perform(c);long status=0;curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);
    if(rc!=CURLE_OK || (status!=200 && status!=404)) {curl_easy_cleanup(c);return -1;}
    if(status==404) {curl_easy_cleanup(c);return 0;}
    if(strcmp(body.text,expected)) {curl_easy_cleanup(c);return -1;}
    curl_easy_setopt(c,CURLOPT_URL,archive);curl_easy_setopt(c,CURLOPT_NOBODY,1L);
    rc=curl_easy_perform(c);curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);
    curl_off_t length=-1;curl_easy_getinfo(c,CURLINFO_CONTENT_LENGTH_DOWNLOAD_T,&length);curl_easy_cleanup(c);
    if(rc!=CURLE_OK || (status!=200 && status!=404))return -1;
    if(status==404)return 0;
    if(length<0)return -1;
    return length==size?1:0;
}
static int collection(const char *url,const char *user,const char *pass,const char *ca) {
    CURL *c=curl_easy_init();if(!c)return 1;
    curl_easy_setopt(c,CURLOPT_URL,url);curl_easy_setopt(c,CURLOPT_PROTOCOLS_STR,"https");
    curl_easy_setopt(c,CURLOPT_USERNAME,user);curl_easy_setopt(c,CURLOPT_PASSWORD,pass);
    curl_easy_setopt(c,CURLOPT_CUSTOMREQUEST,"MKCOL");curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,discard);
    curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,15L);curl_easy_setopt(c,CURLOPT_TIMEOUT,60L);curl_easy_setopt(c,CURLOPT_NOSIGNAL,1L);
    if(ca && *ca)curl_easy_setopt(c,CURLOPT_CAINFO,ca);
    CURLcode rc=curl_easy_perform(c);long status=0;curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);
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
    CURLcode rc=curl_easy_perform(c);long status=0;curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);curl_easy_cleanup(c);
    return rc!=CURLE_OK || status<200 || status>=300;
}
static int upload(const char *base,const char *user,const char *pass,const char *ca,
                  const char *name,int dir) {
    int fd=openat(dir,name,O_RDONLY|O_NOFOLLOW);
    struct stat st;
    if(fd<0) return 1;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<=0) {close(fd); return 1;}
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
    if(structured) {
        char folder[256],hash[65];
        if(pscloud_snapshot_folder(&snapshot,folder,sizeof folder) || pscloud_file_hash(fd,hash) || strcmp(hash,snapshot.sha256)) {
            curl_easy_cleanup(c);fclose(f);return 1;
        }
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
        int present=cloud_copy(url,manifest,user,pass,ca,&snapshot,(curl_off_t)st.st_size);
        if(present<0) {fprintf(stderr,"Cloud presence check failed; backup retained: %s\n",object);curl_easy_cleanup(c);fclose(f);return 1;}
        if(present) {
            curl_easy_cleanup(c);fclose(f);char done[272];snprintf(done,sizeof done,"%s.sent",object);
            if(renameat(dir,name,dir,done) || fsync(dir))return 1;
            printf("Already in cloud; upload skipped: %s\n",object);fflush(stdout);return 0;
        }
    }
    curl_easy_setopt(c,CURLOPT_URL,url);
    curl_easy_setopt(c,CURLOPT_PROTOCOLS_STR,"https");
    curl_easy_setopt(c,CURLOPT_USERNAME,user);
    curl_easy_setopt(c,CURLOPT_PASSWORD,pass);
    curl_easy_setopt(c,CURLOPT_UPLOAD,1L);
    curl_easy_setopt(c,CURLOPT_READDATA,f);
    curl_easy_setopt(c,CURLOPT_INFILESIZE_LARGE,(curl_off_t)st.st_size);
    curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,15L);
    curl_easy_setopt(c,CURLOPT_TIMEOUT,300L);
    curl_easy_setopt(c,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,discard);
    if(ca && *ca) curl_easy_setopt(c,CURLOPT_CAINFO,ca);
    CURLcode rc=curl_easy_perform(c);
    long status=0;
    curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);
    curl_easy_cleanup(c); fclose(f);
    if(rc!=CURLE_OK || status<200 || status>=300) {
        fprintf(stderr,"Upload retained for retry: %s (transport=%d, HTTP=%ld)\n",object,rc,status);
        return 1;
    }
    if(structured) {
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
    printf("Uploaded: %s\n",object); fflush(stdout);
    return 0;
}
#ifdef PSCLOUD_EMBEDDED
int pscloud_worker_main(int argc,char **argv) {
#else
int main(int argc,char **argv) {
#endif
    stopped=0;
    if(argc!=3 || (strcmp(argv[2],"--once") && strcmp(argv[2],"--watch"))) {
        fprintf(stderr,"Usage: %s SPOOL --once|--watch\n",argv[0]); return 2;
    }
    const char *base=getenv("PSCLOUD_URL"),*user=getenv("PSCLOUD_USER"),
        *pass=getenv("PSCLOUD_PASSWORD"),*ca=getenv("PSCLOUD_CA_BUNDLE");
    if(!base||strncmp(base,"https://",8)||strchr(base,'?')||strchr(base,'#')||
       !user||!pass||!*user||!*pass) {
        fprintf(stderr,"Set HTTPS PSCLOUD_URL (existing WebDAV folder), PSCLOUD_USER and PSCLOUD_PASSWORD.\n"); return 2;
    }
    int dir=open(argv[1],O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    if(dir<0) {perror("spool"); return 2;}
    int lock=openat(dir,".worker.lock",O_CREAT|O_RDWR|O_NOFOLLOW,0600);
    if(lock<0||flock(lock,LOCK_EX|LOCK_NB)) {fprintf(stderr,"Cannot lock spool\n"); close(dir); return 2;}
    if(curl_global_init(CURL_GLOBAL_DEFAULT)) return 2;
    signal(SIGTERM,stop); signal(SIGINT,stop);
    unsigned delay=5; int failed=0;
    do {
        int scan=openat(dir,".",O_RDONLY|O_DIRECTORY);
        DIR *d=scan<0?NULL:fdopendir(scan);
        if(!d) {if(scan>=0)close(scan); failed=1; break;}
        struct dirent *e; failed=0;
        while(!stopped && (e=readdir(d)))
            if(valid(e->d_name)) failed |= upload(base,user,pass,ca,e->d_name,dir);
        closedir(d);
        if(!strcmp(argv[2],"--once")) break;
        for(unsigned i=0;i<delay&&!stopped;i++) sleep(1);
        delay=failed?(delay<150?delay*2:300):5;
    } while(!stopped);
    curl_global_cleanup(); close(lock); close(dir);
    return failed?1:0;
}
