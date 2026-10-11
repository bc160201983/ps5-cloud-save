#ifndef PSCLOUD_SLOTCREATE_H
#define PSCLOUD_SLOTCREATE_H
#include <stddef.h>
/* Creates save slot <slot> for <title> in profile <user> on this console, the way the
 * console makes saves: a new save container (console create call), sce_sys copied from
 * another slot of the same game in the same profile (its keystone and identity) with
 * this slot's name, size, revision and time, the slot icon, and one row in the save
 * database. The new slot holds no game files yet; the caller fills it with a normal
 * import. Needs a slot of the game in that profile, or the source's sce_sys (see below).
 * system_root is "" on the PS5. Returns 0 on success; *why is set on failure. */
/* The source slot's sce_sys from a package (V2); fields may be NULL. With it, a game the
 * profile never had can get a slot: keystone and texts from the source, the account and
 * profile number from another save in this profile. */
struct pscloud_source_sys {const unsigned char *sfo,*keystone;size_t sfo_size,keystone_size;};
int pscloud_create_slot(const char *home,const char *system_root,const char *root,const char *user,const char *title,const char *slot,size_t capacity,const struct pscloud_source_sys *from,const char **why);
/* Removes a slot that pscloud_create_slot made (container, icon and database row). */
int pscloud_remove_created_slot(const char *home,const char *system_root,const char *user,const char *title,const char *slot);
#endif
