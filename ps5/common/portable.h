#ifndef PSCLOUD_PORTABLE_H
#define PSCLOUD_PORTABLE_H
#include <stddef.h>
#define PSCLOUD_PORTABLE_MAX (64U*1024*1024+4096U)
struct pscloud_portable {const unsigned char *payload[2];size_t size[2];unsigned firmware;long long created;};
/* Strict stored ZIP: progress.dat, profile.dat, manifest.txt. No keys or SFO. */
int pscloud_portable_parse(const unsigned char *,size_t,struct pscloud_portable *);
int pscloud_portable_export(int stage,const char *destination,unsigned firmware,long long created);
#endif
