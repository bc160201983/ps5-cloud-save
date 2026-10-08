#ifndef PSCLOUD_LOG_H
#define PSCLOUD_LOG_H
int pscloud_log_open(const char *path);
void pscloud_log(const char *level, const char *format, ...);
void pscloud_notify(const char *format, ...);
void pscloud_log_close(void);
#endif
