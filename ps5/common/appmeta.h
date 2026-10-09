#ifndef PSCLOUD_APPMETA_H
#define PSCLOUD_APPMETA_H
#include <stddef.h>
int pscloud_title_valid(const char *title);
int pscloud_app_name(const char *metadata_root,const char *title,char *name,size_t max);
#endif
