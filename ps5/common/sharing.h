#ifndef PSCLOUD_SHARING_H
#define PSCLOUD_SHARING_H
#include <stddef.h>
#define PSCLOUD_SHARE_LIMIT (512U*1024U*1024U)
/* Optional behavior; NULL means the strict defaults (all slots, exact version,
 * every package slot must exist on the receiver). */
struct pscloud_share_options {
    const char *only;   /* export: comma-separated slot names, NULL/empty = all */
    int allow_version;  /* check/restore: accept a package from an older or unknown game version */
    int skip_missing;   /* check/restore: skip package slots the receiver profile does not have */
};
/* Generic decrypted, multi-slot format; distinct from same-console backups.
 * mode: 0 export, 1 staged check, 2 explicitly confirmed live restore.
 * Export of a game that is not installed records GAME_VERSION=unknown.
 * Caller must establish game closure and require confirmation for mode 2. */
int pscloud_share_game(const char *home,const char *root,const char *appmeta,
                      const char *user,const char *title,int mode,
                      const unsigned char *archive,size_t size,
                      const struct pscloud_share_options *options,
                      char published[128],char checksum[65]);
int pscloud_share_validate(const unsigned char *archive,size_t size,const char *title);
/* Reason for the last refusal (empty when none was recorded). */
const char *pscloud_share_error(void);
#endif
