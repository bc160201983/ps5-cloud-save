#ifndef PSCLOUD_CLOUD_H
#define PSCLOUD_CLOUD_H
struct settings {
    char url[3072], user[256], password[1024], ca[1024], mode[16];
};
int pscloud_configure(const char *path, struct settings *s);
#endif
