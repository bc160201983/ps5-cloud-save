/* Direct console WebDAV transport. Does not mount saves or restore data. */
#define _POSIX_C_SOURCE 200809L
#include "common/log.h"
#include "common/cloud.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef PSCLOUD_HOST_TEST
#include <ps5/kernel.h>
#endif

extern int pscloud_worker_main(int argc, char **argv);

int main(int argc,char **argv) {
#ifdef PSCLOUD_HOST_TEST
    if(argc!=4) return 2;
    const char *config=argv[1], *spool=argv[2], *log=argv[3];
#else
    (void)argc; (void)argv;
    const char *config="/data/pscloud-upload.conf", *spool="/data/pscloud/spool",
        *log="/data/pscloud.log";
#endif
    pscloud_log_open(log);
    pscloud_notify("Cloud uploader started");
#ifndef PSCLOUD_HOST_TEST
    if((kernel_get_fw_version() & 0xffff0000U)!=0x11400000U) {
        pscloud_notify("Uploader stopped: initial test targets firmware 11.40");
        pscloud_log_close(); return 2;
    }
#endif
    struct settings s={0};
    if(pscloud_configure(config,&s)) {
        pscloud_notify("Uploader stopped: missing or invalid configuration");
        pscloud_log_close(); return 2;
    }
    /* A console cannot use a build-machine CA path. Require a deployed bundle. */
    int ca=open(s.ca,O_RDONLY | O_NOFOLLOW | O_NONBLOCK); struct stat st;
    if(ca<0 || fstat(ca,&st) || !S_ISREG(st.st_mode) || st.st_size<=0) {
        if(ca>=0)close(ca);
        pscloud_notify("Uploader stopped: TLS CA bundle unavailable");
        pscloud_log_close(); return 2;
    }
    close(ca);
    int failed=setenv("PSCLOUD_URL",s.url,1) || setenv("PSCLOUD_USER",s.user,1) ||
        setenv("PSCLOUD_PASSWORD",s.password,1) || setenv("PSCLOUD_CA_BUNDLE",s.ca,1);
    if(failed) {pscloud_notify("Uploader configuration failed"); pscloud_log_close(); return 2;}
    char *args[]={"pscloud-upload",(char *)spool,
        !strcmp(s.mode,"watch") ? "--watch" : "--once",NULL};
    pscloud_notify("Cloud upload %s - queued backups use HTTPS",s.mode);
    int result=pscloud_worker_main(3,args);
    unsetenv("PSCLOUD_PASSWORD");
    pscloud_notify(result ? "Cloud uploader stopped with errors - backups retained" :
        "Cloud uploader finished - server accepted queued backups");
    pscloud_log_close(); return result;
}
