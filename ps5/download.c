#define _POSIX_C_SOURCE 200809L
#include "common/cloud.h"
#include "common/restore.h"
#include "common/log.h"
#include <curl/curl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifndef PSCLOUD_HOST_TEST
#include <ps5/kernel.h>
#endif
struct buffer {unsigned char *data; size_t size;};
static size_t receive(char *data,size_t size,size_t count,void *context) {
    struct buffer *b=context;
    if(count && size>PSCLOUD_RESTORE_MAX/count)return 0;
    size_t n=size*count;
    if(n>PSCLOUD_RESTORE_MAX-b->size)return 0;
    memcpy(b->data+b->size,data,n);b->size+=n;return n;
}
int main(int argc,char **argv) {
#ifdef PSCLOUD_HOST_TEST
    if(argc!=5)return 2;
    const char *cloud=argv[1],*selection=argv[2],*folder=argv[3],*log=argv[4];
#else
    (void)argc;(void)argv;
    const char *cloud="/data/pscloud-upload.conf",*selection="/data/pscloud-download.conf",
        *folder="/data/pscloud/downloads",*log="/data/pscloud.log";
#endif
    pscloud_log_open(log);pscloud_notify("Cloud download started");
#ifndef PSCLOUD_HOST_TEST
    if((kernel_get_fw_version()&0xffff0000U)!=0x11400000U) {
        pscloud_notify("Download stopped: firmware 11.40 required");pscloud_log_close();return 2;
    }
#endif
    struct settings c={0};struct restore_settings s={0};
    if(pscloud_configure(cloud,&c) || pscloud_restore_config(selection,&s,0)) {
        pscloud_notify("Download stopped: invalid configuration");pscloud_log_close();return 2;
    }
#ifndef PSCLOUD_HOST_TEST
    int parent=pscloud_open_directory("/data/pscloud");
    if(parent<0 || (mkdirat(parent,"downloads",0700) && errno!=EEXIST)) {
        if(parent>=0)close(parent);
        pscloud_notify("Download folder unavailable");pscloud_log_close();return 2;
    }
    close(parent);
#endif
    int dir=pscloud_open_directory(folder);
    if(dir<0) {pscloud_notify("Download folder unavailable");pscloud_log_close();return 2;}
    unsigned char *existing=NULL;size_t length=0;struct stat st;
    if(!fstatat(dir,s.backup,&st,AT_SYMLINK_NOFOLLOW)) {
        const unsigned char *p=NULL;size_t n=0;
        int valid=pscloud_read_archive(dir,s.backup,&existing,&length)==0 &&
            pscloud_verify_hash(existing,length,s.sha256)==0 && pscloud_save_payload(existing,length,&p,&n)==0;
        free(existing);close(dir);
        pscloud_notify(valid?"Verified cloud backup already downloaded":"Download stopped: existing archive differs");
        pscloud_log_close();return valid?0:1;
    }
    char part[160],url[4096];snprintf(part,sizeof part,"%s.part",s.backup);
    int fd=openat(dir,part,O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,0600);
    if(fd<0) {close(dir);pscloud_notify("Download stopped: staging file unavailable");pscloud_log_close();return 1;}
    struct buffer b={malloc(PSCLOUD_RESTORE_MAX),0};int failed=!b.data;
    CURL *curl=NULL;int initialized=0;long status=0;CURLcode rc=CURLE_FAILED_INIT;
    if(!failed) {
        initialized=curl_global_init(CURL_GLOBAL_DEFAULT)==CURLE_OK;
        curl=initialized?curl_easy_init():NULL;
        failed=!curl;
    }
    if(!failed) {
        if(snprintf(url,sizeof url,"%s/%s",c.url,s.backup)>=(int)sizeof url)failed=1;
#define SET(option,value) do {if(curl_easy_setopt(curl,option,value)!=CURLE_OK)failed=1;} while(0)
        SET(CURLOPT_URL,url);SET(CURLOPT_PROTOCOLS_STR,"https");
        SET(CURLOPT_USERNAME,c.user);SET(CURLOPT_PASSWORD,c.password);
        SET(CURLOPT_CAINFO,c.ca);SET(CURLOPT_SSL_VERIFYPEER,1L);SET(CURLOPT_SSL_VERIFYHOST,2L);
        SET(CURLOPT_CONNECTTIMEOUT,15L);SET(CURLOPT_TIMEOUT,300L);SET(CURLOPT_NOSIGNAL,1L);
        SET(CURLOPT_WRITEFUNCTION,receive);SET(CURLOPT_WRITEDATA,&b);
#undef SET
        if(!failed)rc=curl_easy_perform(curl);
        curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);
        if(rc!=CURLE_OK || status!=200)failed=1;
    }
    if(curl)curl_easy_cleanup(curl);
    if(initialized)curl_global_cleanup();
    const unsigned char *payload=NULL;size_t payload_size=0;
    if(!failed && (pscloud_verify_hash(b.data,b.size,s.sha256) ||
       pscloud_save_payload(b.data,b.size,&payload,&payload_size)))failed=1;
    size_t written=0;
    while(!failed && written<b.size) {
        ssize_t n=write(fd,b.data+written,b.size-written);
        if(n<0 && errno==EINTR)continue;
        if(n<=0) {failed=1;break;}written+=(size_t)n;
    }
    if(!failed && fsync(fd))failed=1;
    if(close(fd))failed=1;
    if(!failed && (renameat(dir,part,dir,s.backup) || fsync(dir)))failed=1;
    if(failed)unlinkat(dir,part,0);
    free(b.data);close(dir);
    pscloud_log("INFO","Download transport=%d HTTP=%ld; archive bytes=%zu",rc,status,b.size);
    pscloud_notify(failed?"Download failed - no restore performed":"Cloud backup downloaded and SHA-256 verified");
    pscloud_log_close();return failed?1:0;
}
