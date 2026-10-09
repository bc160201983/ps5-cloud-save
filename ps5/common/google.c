#define _POSIX_C_SOURCE 200809L
#include "google.h"
#include <curl/curl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>

/* Bounded parser for Google's flat OAuth responses. Never search for a key
 * inside another field, accept duplicate requested keys, or decode control characters. */
static const char *space(const char *p) {while(isspace((unsigned char)*p))p++;return p;}
static const char *string(const char *p,char *out,size_t cap) {
    size_t n=0;if(*p++!='"')return NULL;
    while(*p && *p!='"') {
        unsigned char c=(unsigned char)*p++;
        if(c=='\\') {c=(unsigned char)*p++;if(c!='"' && c!='\\' && c!='/')return NULL;}
        if(c<32 || c>=127 || n+1>=cap)return NULL;
        out[n++]=(char)c;
    }
    if(*p!='"')return NULL;
    out[n]=0;return p+1;
}
int pscloud_google_value(const char *json,const char *key,char *out,size_t cap) {
    const char *p=space(json);if(*p++!='{')return -1;
    int found=0;char name[128],value[4096];
    p=space(p);
    while(*p!='}') {
        p=string(p,name,sizeof name);if(!p)return -1;
        p=space(p);if(*p++!=':')return -1;p=space(p);
        if(*p=='"') {p=string(p,value,sizeof value);if(!p)return -1;}
        else {size_t n=0;while(*p && *p!=',' && *p!='}' && !isspace((unsigned char)*p)) {
            if(!isdigit((unsigned char)*p) || n+1>=sizeof value)return -1;
            value[n++]=*p++;
        }if(!n)return -1;value[n]=0;}
        if(!strcmp(name,key)) {if(found || strlen(value)>=cap)return -1;strcpy(out,value);found=1;}
        p=space(p);if(*p=='}')break;if(*p++!=',')return -1;p=space(p);if(*p=='}')return -1;
    }
    if(*p++!='}' || *space(p))return -1;
    return found?0:-1;
}
static int clean(const char *s,size_t max,int required) {
    if((required && !*s) || strlen(s)>=max)return 0;
    for(;*s;s++)if((unsigned char)*s<33 || (unsigned char)*s>126)return 0;
    return 1;
}
int pscloud_google_load(const char *path,struct pscloud_google *g) {
    memset(g,0,sizeof *g);int fd=open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);struct stat st;
    if(fd<0)return -1;
    if(fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_nlink!=1 || st.st_size>8192 || (st.st_mode&077)) {close(fd);return -1;}
    FILE *f=fdopen(fd,"r");if(!f) {close(fd);return -1;}
    char line[4096];unsigned seen=0;int bad=0;
    const char *keys[]={"CLIENT_ID","CLIENT_SECRET","REFRESH_TOKEN"};
    char *values[]={g->client,g->secret,g->refresh};size_t caps[]={sizeof g->client,sizeof g->secret,sizeof g->refresh};
    while(fgets(line,sizeof line,f)) {
        size_t n=strlen(line);if(!n || line[n-1]!='\n') {bad=1;break;}line[--n]=0;
        char *eq=strchr(line,'=');if(!eq) {bad=1;break;}*eq++=0;unsigned i;
        for(i=0;i<3;i++)if(!strcmp(line,keys[i]))break;
        if(i==3 || (seen&(1U<<i)) || !clean(eq,caps[i],i==0)) {bad=1;break;}
        strcpy(values[i],eq);seen|=1U<<i;
    }
    if(ferror(f))bad=1;
    fclose(f);if(bad || seen!=7) {memset(g,0,sizeof *g);return -1;}return 0;
}
int pscloud_google_save(const char *path,const struct pscloud_google *g) {
    if(!clean(g->client,sizeof g->client,1) || !clean(g->secret,sizeof g->secret,0) || !clean(g->refresh,sizeof g->refresh,0))return -1;
    char part[1400],text[4096];if(snprintf(part,sizeof part,"%s.new",path)>=(int)sizeof part)return -1;
    int n=snprintf(text,sizeof text,"CLIENT_ID=%s\nCLIENT_SECRET=%s\nREFRESH_TOKEN=%s\n",g->client,g->secret,g->refresh);
    if(n<0 || n>=(int)sizeof text)return -1;
    int fd=open(part,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);if(fd<0)return -1;
    size_t have=0;int bad=0;
    while(have<(size_t)n) {ssize_t w=write(fd,text+have,(size_t)n-have);if(w<0 && errno==EINTR)continue;if(w<=0) {bad=1;break;}have+=(size_t)w;}
    if(fsync(fd))bad=1;
    if(close(fd))bad=1;
    if(!bad && rename(part,path))bad=1;
    if(bad)unlink(part);
    return bad?-1:0;
}
struct reply {char text[16385];size_t size;};
static size_t collect(char *p,size_t a,size_t b,void *ctx) {
    struct reply *r=ctx;if(b && a>16384/b)return 0;size_t n=a*b;
    if(n>16384-r->size)return 0;
    memcpy(r->text+r->size,p,n);r->size+=n;r->text[r->size]=0;return n;
}
static long request(const char *endpoint,const char *ca,const char *form,struct reply *r) {
    char url[4096];
#ifdef PSCLOUD_HOST_TEST
    const char *base=getenv("PSCLOUD_TEST_GOOGLE_URL");
    if(!base)base="https://oauth2.googleapis.com";
#else
    const char *base="https://oauth2.googleapis.com";
#endif
    if(snprintf(url,sizeof url,"%s/%s",base,endpoint)>=(int)sizeof url)return -1;
    CURL *c=curl_easy_init();if(!c)return -1;
    curl_easy_setopt(c,CURLOPT_URL,url);curl_easy_setopt(c,CURLOPT_PROTOCOLS_STR,"https");
    curl_easy_setopt(c,CURLOPT_CAINFO,ca);curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,10L);
    curl_easy_setopt(c,CURLOPT_TIMEOUT,25L);curl_easy_setopt(c,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(c,CURLOPT_POSTFIELDS,form);curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,collect);curl_easy_setopt(c,CURLOPT_WRITEDATA,r);
    CURLcode rc=curl_easy_perform(c);long status=0;curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);curl_easy_cleanup(c);
    return rc==CURLE_OK?status:-1;
}
static int form(struct pscloud_google *g,char *out,size_t cap,const char *kind) {
    char *client=curl_easy_escape(NULL,g->client,0),*secret=curl_easy_escape(NULL,g->secret,0);
    const char *value=!strcmp(kind,"device")?"https://www.googleapis.com/auth/drive.file":!strcmp(kind,"refresh")?g->refresh:g->device;
    char *encoded=curl_easy_escape(NULL,value,0);int n=-1;
    if(client && secret && encoded) {
        if(!strcmp(kind,"device"))n=snprintf(out,cap,"client_id=%s&scope=%s",client,encoded);
        else n=snprintf(out,cap,"client_id=%s&client_secret=%s&grant_type=%s&%s=%s",client,secret,!strcmp(kind,"refresh")?"refresh_token":"urn:ietf:params:oauth:grant-type:device_code",!strcmp(kind,"refresh")?"refresh_token":"device_code",encoded);
    }
    curl_free(client);curl_free(secret);curl_free(encoded);return n<0 || (size_t)n>=cap?-1:0;
}
static long number(const char *text,const char *key,long max) {
    char value[32],*end=NULL;if(pscloud_google_value(text,key,value,sizeof value))return -1;
    errno=0;long n=strtol(value,&end,10);return errno || *end || n<=0 || n>max?-1:n;
}
int pscloud_google_begin(struct pscloud_google *g,const char *ca) {
    char body[8192];struct reply r={{0},0};
    memset(g->device,0,sizeof g->device);memset(g->user_code,0,sizeof g->user_code);g->expires=0;
    if(!clean(g->client,sizeof g->client,1) || form(g,body,sizeof body,"device") || request("device/code",ca,body,&r)!=200)return -1;
    char uri[256];long expires=number(r.text,"expires_in",3600),interval=number(r.text,"interval",300);
    if(expires<0 || interval<0 || pscloud_google_value(r.text,"device_code",g->device,sizeof g->device) ||
       pscloud_google_value(r.text,"user_code",g->user_code,sizeof g->user_code) ||
       pscloud_google_value(r.text,"verification_url",uri,sizeof uri) ||
       (strcmp(uri,"https://www.google.com/device") && strcmp(uri,"https://google.com/device")) ||
       !clean(g->device,sizeof g->device,1) || !clean(g->user_code,sizeof g->user_code,1)) {
        memset(g->device,0,sizeof g->device);memset(g->user_code,0,sizeof g->user_code);return -1;
    }
    g->interval=interval;g->next_poll=time(NULL)+interval;g->expires=time(NULL)+expires;return 0;
}
static int tokens(struct pscloud_google *g,const char *text,int refresh) {
    char access[2048],token[2048],type[32],scope[512];
    if(pscloud_google_value(text,"access_token",access,sizeof access) || !clean(access,sizeof access,1) ||
       pscloud_google_value(text,"token_type",type,sizeof type) || strcmp(type,"Bearer") || number(text,"expires_in",86400)<0)return -1;
    if(!refresh && (pscloud_google_value(text,"refresh_token",token,sizeof token) || !clean(token,sizeof token,1) ||
       pscloud_google_value(text,"scope",scope,sizeof scope) || strcmp(scope,"https://www.googleapis.com/auth/drive.file")))return -1;
    strcpy(g->access,access);if(!refresh)strcpy(g->refresh,token);return 0;
}
int pscloud_google_poll(struct pscloud_google *g,const char *ca) {
    long now=time(NULL);if(!g->expires || now>=g->expires) {memset(g->device,0,sizeof g->device);g->expires=0;return 2;}
    if(now<g->next_poll)return 1;
    g->next_poll=now+g->interval;char body[8192],error[128];struct reply r={{0},0};
    if(form(g,body,sizeof body,"poll"))return -1;
    long status=request("token",ca,body,&r);
    if(status==200) {if(tokens(g,r.text,0))return -1;memset(g->device,0,sizeof g->device);g->expires=0;return 0;}
    if(status<0 || pscloud_google_value(r.text,"error",error,sizeof error))return -1;
    if(!strcmp(error,"authorization_pending"))return 1;
    if(!strcmp(error,"slow_down")) {g->interval+=5;g->next_poll=now+g->interval;return 1;}
    if(!strcmp(error,"access_denied") || !strcmp(error,"expired_token")) {memset(g->device,0,sizeof g->device);g->expires=0;return 2;}
    return -1;
}
int pscloud_google_refresh(struct pscloud_google *g,const char *ca) {
    char body[8192];struct reply r={{0},0};if(!*g->refresh || form(g,body,sizeof body,"refresh"))return -1;
    if(request("token",ca,body,&r)!=200)return -1;
    return tokens(g,r.text,1);
}
