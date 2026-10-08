/* Independently written staged-image adapter. Never copies back to a live save.
 * Interfaces referenced: SDK kernel.h and Garlic SaveMgr's public API usage.
 * No Garlic implementation is incorporated. Firmware ABI remains a test gate. */
#define _POSIX_C_SOURCE 200809L
#include "mount.h"
#include "log.h"
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#ifndef PSCLOUD_HOST_TEST
#include <ps5/kernel.h>
struct mount_options {uint8_t reserved; const char *budget;};
struct unmount_options {uint8_t reserved;};
extern int sceFsInitMountSaveDataOpt(struct mount_options *options);
extern int sceFsMountSaveData(struct mount_options *options,const char *image,
                             const char *mount,uint8_t *key);
extern int sceFsInitUmountSaveDataOpt(struct unmount_options *options);
extern int sceFsUmountSaveData(struct unmount_options *options,const char *mount,int handle,int flags);
#endif

int pscloud_mount_begin(struct pscloud_mount_state *s) {
#ifdef PSCLOUD_HOST_TEST
    s->credentials_saved=1;
    return getenv("PSCLOUD_TEST_PRIVILEGE_FAIL")?-1:0;
#else
    pid_t pid=getpid();
    s->authid=kernel_get_ucred_authid(pid);
    s->uid=kernel_get_ucred_uid(pid);
    if(kernel_get_ucred_caps(pid,s->caps)<0)return -1;
    s->credentials_saved=1;
    uint8_t elevated[16];memcpy(elevated,s->caps,sizeof elevated);elevated[7]|=0x40;
    if(kernel_set_ucred_authid(pid,0x4800000000000010ULL)<0 ||
       kernel_set_ucred_caps(pid,elevated)<0 || kernel_set_ucred_uid(pid,0)<0)return -1;
    return 0;
#endif
}
int pscloud_mount_copy(struct pscloud_mount_state *s,const char *image,const char *mount) {
#ifdef PSCLOUD_HOST_TEST
    (void)image;(void)mount;
    if(getenv("PSCLOUD_TEST_MOUNT_FAIL"))return -1;
    s->mounted=1;pscloud_log("INFO","Host simulated mount");return 0;
#else
    /* ioctl writes beyond the 0x60-byte sealed-key input; use a larger heap buffer. */
    uint8_t *request=calloc(1,4096);uint8_t key[32]={0};
    if(!request)return -1;
    int imagefd=open(image,O_RDONLY | O_NOFOLLOW);int result=-1;
    if(imagefd<0) {free(request);return -1;}
    ssize_t n=pread(imagefd,request,0x60,0x800);close(imagefd);
    if(n!=0x60) {free(request);return -1;}
    int device=open("/dev/pfsmgr",O_RDWR);
    if(device<0) {free(request);return -1;}
    int decoded=ioctl(device,0xc0845302UL,request);close(device);
    if(decoded<0) {pscloud_log("ERROR","Sealed-key operation failed: %d",decoded);free(request);return -1;}
    memcpy(key,request+0x60,sizeof key);memset(request,0,4096);free(request);
    struct mount_options options={0};
    int init=sceFsInitMountSaveDataOpt(&options);
    if(init>=0) {
        options.budget="system";
        result=sceFsMountSaveData(&options,image,mount,key);
    }
    memset(key,0,sizeof key);
    pscloud_log("INFO","Staged mount result: 0x%x",(unsigned)result);
    if(result<0)return -1;
    s->mounted=1;return 0;
#endif
}
int pscloud_mount_end(struct pscloud_mount_state *s,const char *mount) {
    if(!s->mounted)return 0;
#ifdef PSCLOUD_HOST_TEST
    (void)mount;
    if(getenv("PSCLOUD_TEST_UNMOUNT_FAIL"))return -1;
    pscloud_log("INFO","Host simulated unmount");
#else
    struct unmount_options options={0};
    if(sceFsInitUmountSaveDataOpt(&options)<0)return -1;
    int result=sceFsUmountSaveData(&options,mount,0,0);
    pscloud_log("INFO","Staged unmount result: 0x%x",(unsigned)result);
    if(result<0)return -1;
#endif
    s->mounted=0;return 0;
}
int pscloud_mount_leave(struct pscloud_mount_state *s) {
    if(!s->credentials_saved)return 0;
#ifdef PSCLOUD_HOST_TEST
    pscloud_log("INFO","Host simulated credentials restored");
    return getenv("PSCLOUD_TEST_CREDENTIAL_RESTORE_FAIL")?-1:0;
#else
    int failed=0;pid_t pid=getpid();
    if(kernel_set_ucred_uid(pid,s->uid)<0)failed=1;
    if(kernel_set_ucred_caps(pid,s->caps)<0)failed=1;
    if(kernel_set_ucred_authid(pid,s->authid)<0)failed=1;
    if(!failed)s->credentials_saved=0;
    return failed?-1:0;
#endif
}
