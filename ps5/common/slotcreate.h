#ifndef PSCLOUD_SLOTCREATE_H
#define PSCLOUD_SLOTCREATE_H
#include <stddef.h>
/* Creates save slot <slot> for <title> in profile <user> on this console, the way the
 * console makes saves: a new save container (console create call), sce_sys copied from
 * another slot of the same game in the same profile (its keystone and identity) with
 * this slot's name, size, revision and time, the slot icon, and one row in the save
 * database. The new slot holds no game files yet; the caller fills it with a normal
 * import. Needs at least one existing slot of the game in that profile.
 * system_root is "" on the PS5. Returns 0 on success; *why is set on failure. */
int pscloud_create_slot(const char *home,const char *system_root,const char *root,const char *user,const char *title,const char *slot,size_t capacity,const char *subtitle,const char **why);
/* Removes a slot that pscloud_create_slot made (container, icon and database row). */
int pscloud_remove_created_slot(const char *home,const char *system_root,const char *user,const char *title,const char *slot);
#endif
