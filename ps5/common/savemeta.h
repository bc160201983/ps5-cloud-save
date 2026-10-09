#ifndef PSCLOUD_SAVEMETA_H
#define PSCLOUD_SAVEMETA_H
#include <stddef.h>
struct pscloud_save_meta {char title[32],slot[64];unsigned char account[16];size_t account_size;};
/* Read bounded PSF metadata without changing it. Require explicit identity. */
int pscloud_save_meta_read(int payload,struct pscloud_save_meta *meta,char hash[65]);
/* System-memory slot: valid SFO without normal identity fields, or exact 3 KiB
 * zero placeholder. Caller must independently anchor local container identity.
 * Never relax normal save validation. */
int pscloud_save_meta_read_memory(int payload,struct pscloud_save_meta *meta,char hash[65]);
/* Generic sharing: hash all local metadata; decode identities when available.
 * Caller anchors identity to an existing local user/title/slot and own keys. */
int pscloud_save_meta_local(int payload,struct pscloud_save_meta *meta,char hash[65]);
int pscloud_save_meta_matches(const struct pscloud_save_meta *meta,const char *title,const char *slot);
#endif
