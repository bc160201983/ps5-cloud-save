#ifndef PSCLOUD_SHARING_H
#define PSCLOUD_SHARING_H
#include <stddef.h>
#define PSCLOUD_SHARE_LIMIT (512U*1024U*1024U)
/* Generic decrypted, multi-slot format; distinct from same-console backups.
 * mode: 0 export, 1 staged check, 2 explicitly confirmed live restore.
 * only: export-only comma-separated slot names to include (NULL/empty = all);
 * every named slot must exist. Imports always apply what the package contains.
 * Caller must establish game closure and require confirmation for mode 2. */
int pscloud_share_game(const char *home,const char *root,const char *appmeta,
                      const char *user,const char *title,int mode,
                      const unsigned char *archive,size_t size,const char *only,
                      char published[128],char checksum[65]);
int pscloud_share_validate(const unsigned char *archive,size_t size,const char *title);
#endif
