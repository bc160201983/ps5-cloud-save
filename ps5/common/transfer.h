#ifndef PSCLOUD_TRANSFER_H
#define PSCLOUD_TRANSFER_H
enum pscloud_transfer_phase {PSCLOUD_IDLE,PSCLOUD_LOCAL_CHECK,PSCLOUD_FOLDERS,
    PSCLOUD_PRESENCE,PSCLOUD_UPLOAD,PSCLOUD_READBACK,PSCLOUD_COMMIT,PSCLOUD_DONE,PSCLOUD_FAILED};
struct pscloud_transfer_status {
    unsigned sequence;
    int phase,failed_phase,transport;
    long http;
    unsigned long long done,total;
};
void pscloud_worker_status(struct pscloud_transfer_status *);
long pscloud_transfer_timeout(unsigned long long);
#endif
