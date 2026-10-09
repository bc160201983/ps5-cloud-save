#ifndef PSCLOUD_SNAPSHOT_H
#define PSCLOUD_SNAPSHOT_H
struct pscloud_snapshot {
    char user[17],title[10],slot[64],sha256[65];
    long long created;
};
int pscloud_snapshot_valid(const struct pscloud_snapshot *snapshot);
int pscloud_snapshot_read(int directory,const char *name,struct pscloud_snapshot *snapshot);
int pscloud_file_hash(int fd,char hex[65]);
int pscloud_file_hash_checked(int fd,char hex[65],unsigned *phase);
int pscloud_snapshot_exists(int directory,const struct pscloud_snapshot *snapshot);
struct pscloud_dedup_stats {unsigned scanned,invalid,matched,missing,hash_failed,different,hash_phase;};
int pscloud_snapshot_exists_checked(int directory,const struct pscloud_snapshot *snapshot,struct pscloud_dedup_stats *stats);
/* URL-encoded relative game/user/slot directory, with no leading/trailing slash. */
int pscloud_snapshot_folder(const struct pscloud_snapshot *snapshot,char *folder,unsigned size);
#endif
