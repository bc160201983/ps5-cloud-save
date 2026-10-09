#ifndef PSCLOUD_GOOGLE_H
#define PSCLOUD_GOOGLE_H
#include <stddef.h>
/* Device OAuth only. Drive transfers are deliberately not enabled yet. */
struct pscloud_google {
    char client[256], secret[256], refresh[2048], access[2048];
    char device[2048], user_code[64];
    long expires, next_poll, interval;
};
int pscloud_google_load(const char *,struct pscloud_google *);
int pscloud_google_save(const char *,const struct pscloud_google *);
int pscloud_google_begin(struct pscloud_google *,const char *);
/* 0 connected; 1 pending; 2 expired/denied; -1 transport/protocol error. */
int pscloud_google_poll(struct pscloud_google *,const char *);
int pscloud_google_refresh(struct pscloud_google *,const char *);
int pscloud_google_value(const char *,const char *,char *,size_t);
#endif
