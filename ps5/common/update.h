#ifndef PSCLOUD_UPDATE_H
#define PSCLOUD_UPDATE_H
#include <stddef.h>
/* Online updates from the newest published PSCloud release on GitHub (drafts and pre-releases
 * are never offered). root: PSCloud's folder; ca: trusted certificate bundle; current: this
 * version. Checks, downloads and installs run in the background; json reports the state. */
void pscloud_update_init(const char *root,const char *ca,const char *current);
int pscloud_update_check(int notify,char *message,size_t max);   /* waits for the answer */
void pscloud_update_tick(void);                                  /* daily automatic check */
int pscloud_update_download(char *message,size_t max);
int pscloud_update_install(char *message,size_t max);
int pscloud_update_set_auto(int on);
void pscloud_update_json(char *out,size_t max);
/* The downloaded dashboard file, for saving it on a PC (empty when there is none). */
int pscloud_update_file(char *path,size_t max);
#endif
