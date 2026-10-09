#ifndef PSCLOUD_BUNDLE_H
#define PSCLOUD_BUNDLE_H
#include "snapshot.h"
#include <stddef.h>
/* Closed-game, same-console encrypted backup. Does not mount or modify saves.
 * Full-game restore is deliberately not supported by the single-slot restorer. */
int pscloud_game_backup(const char *home,const char *root,const char *user,const char *title);
int pscloud_bundle_parse(const unsigned char *archive,size_t length,const struct pscloud_snapshot *identity,
                         const unsigned char *images[2],size_t sizes[2]);
int pscloud_bundle_restore(const char *home,const char *root,const struct pscloud_snapshot *identity,
                           const unsigned char *archive,size_t length);
int pscloud_bundle_restore_check(const char *home,const char *root,const struct pscloud_snapshot *identity,
                                 const unsigned char *archive,size_t length);
#endif
