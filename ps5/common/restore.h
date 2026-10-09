#ifndef PSCLOUD_RESTORE_H
#define PSCLOUD_RESTORE_H
#include <stddef.h>
#define PSCLOUD_RESTORE_MAX (256U*1024U*1024U)
struct restore_settings {
    char backup[128], sha256[65], title[10], target[1024],user[17],slot[64];
};
int pscloud_restore_config(const char *path, struct restore_settings *s, int restoring);
int pscloud_open_directory(const char *path);
int pscloud_verify_hash(const unsigned char *data, size_t size, const char *expected);
/* Narrow first gate: exactly one stored ue4savegame.dpx.sav, ZIP32, no extras. */
int pscloud_save_payload(const unsigned char *archive, size_t length,
                         const unsigned char **data, size_t *size);
int pscloud_read_archive(int directory, const char *name, unsigned char **data, size_t *size);
#endif
