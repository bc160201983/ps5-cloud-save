#ifndef PSCLOUD_SAVEMETA_H
#define PSCLOUD_SAVEMETA_H
#include <stddef.h>
struct pscloud_save_meta {char title[32],slot[64];unsigned char account[16];size_t account_size;};
/* Read bounded PSF metadata without changing it. Require explicit identity. */
int pscloud_save_meta_read(int payload,struct pscloud_save_meta *meta,char hash[65]);
/* System-memory slot has a structurally valid SFO without the normal identity
 * fields. Only accept that exact layout; never relax normal save validation. */
int pscloud_save_meta_read_memory(int payload,struct pscloud_save_meta *meta,char hash[65]);
int pscloud_save_meta_matches(const struct pscloud_save_meta *meta,const char *title,const char *slot);
#endif
