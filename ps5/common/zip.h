#ifndef PSCLOUD_ZIP_H
#define PSCLOUD_ZIP_H
/* Export regular files from an already-consistent, mounted directory.
 * Excludes root sce_sys metadata. Rejects symlinks and special files.
 * Caller owns removal of an incomplete destination on failure. */
int pscloud_zip_export(int source_fd, const char *destination, unsigned *files,
                      unsigned long long *bytes);
#endif
