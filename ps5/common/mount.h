#ifndef PSCLOUD_MOUNT_H
#define PSCLOUD_MOUNT_H
#include <stdint.h>
struct pscloud_mount_state {
    uint64_t authid;
    uint8_t caps[16];
    unsigned uid;
    int credentials_saved, mounted;
};
int pscloud_mount_begin(struct pscloud_mount_state *state);
int pscloud_mount_copy(struct pscloud_mount_state *state,const char *image,const char *mount);
int pscloud_mount_end(struct pscloud_mount_state *state,const char *mount);
int pscloud_mount_leave(struct pscloud_mount_state *state);
#endif
