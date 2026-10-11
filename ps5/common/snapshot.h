#ifndef PSCLOUD_SNAPSHOT_H
#define PSCLOUD_SNAPSHOT_H
#include <stddef.h>
struct pscloud_snapshot {
    char user[17],title[10],slot[64],sha256[65];
    long long created;
};
int pscloud_snapshot_valid(const struct pscloud_snapshot *snapshot);
int pscloud_snapshot_read(int directory,const char *name,struct pscloud_snapshot *snapshot);
int pscloud_file_hash(int fd,char hex[65]);
int pscloud_file_hash_checked(int fd,char hex[65],unsigned *phase);
int pscloud_snapshot_exists(int directory,const struct pscloud_snapshot *snapshot);
struct pscloud_dedup_stats {unsigned scanned,invalid,matched,missing,hash_failed,different,hash_phase;char archive[256];};
int pscloud_snapshot_exists_checked(int directory,const struct pscloud_snapshot *snapshot,struct pscloud_dedup_stats *stats);
/* Recheck this selected backup in the cloud, without recreating history. */
int pscloud_snapshot_requeue(int directory,const char *archive);
/* URL-encoded relative game/user/slot directory, with no leading/trailing slash. */
int pscloud_snapshot_folder(const struct pscloud_snapshot *snapshot,char *folder,unsigned size);
/* Cloud encryption (AES-256-GCM; see snapshot.c). Files grow by PSCLOUD_ENC_OVERHEAD bytes. */
#define PSCLOUD_ENC_OVERHEAD 56
void pscloud_encryption_set(const char *password);
int pscloud_encryption_on(void);
int pscloud_encrypted(const unsigned char *data,size_t size);
/* 0: plain or decrypted in place (size updated); -2: encrypted but no password; -1: wrong password or damaged */
int pscloud_decrypt_if_needed(unsigned char *data,size_t *size);
int pscloud_encrypt_fd(int in,int out,char hash[65],unsigned long long *out_size);
#endif
