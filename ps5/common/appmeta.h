#ifndef PSCLOUD_APPMETA_H
#define PSCLOUD_APPMETA_H
#include <stddef.h>
int pscloud_title_valid(const char *title);
int pscloud_app_name(const char *metadata_root,const char *title,char *name,size_t max);
int pscloud_app_meta_dir(const char *metadata_root,const char *title,char *out,size_t max);
/* Name a game that is not installed by the title its saves agree on (save param.sfo MAINTITLE). */
int pscloud_save_title_name(const char *home,const char *user,const char *title,char *name,size_t max);
int pscloud_app_version(const char *metadata_root,const char *title,char *version,size_t max);
#endif
