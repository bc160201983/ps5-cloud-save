#ifndef PSCLOUD_MANAGED_H
#define PSCLOUD_MANAGED_H
#include "snapshot.h"
int pscloud_random_id(char id[33]);
int pscloud_copy_image(int source,int destination);
int pscloud_no_foreign_mount(void);
int pscloud_active_marker(int parent);
/* Selected identity must come from a verified cloud commit. Caller confirms closure. */
int pscloud_managed_restore(const char *home,const char *root,const struct pscloud_snapshot *s,const char *archive);
#endif
