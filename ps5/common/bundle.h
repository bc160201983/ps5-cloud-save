#ifndef PSCLOUD_BUNDLE_H
#define PSCLOUD_BUNDLE_H
/* Closed-game, same-console encrypted backup. Does not mount or modify saves.
 * Full-game restore is deliberately not supported by the single-slot restorer. */
int pscloud_game_backup(const char *home,const char *root,const char *user,const char *title);
#endif
