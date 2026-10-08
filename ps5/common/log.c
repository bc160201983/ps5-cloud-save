#define _POSIX_C_SOURCE 200809L
#include "log.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

static FILE *log_file;
int pscloud_log_open(const char *path) {
    if(log_file)pscloud_log_close();
    signal(SIGPIPE, SIG_IGN); /* The sending PC can disconnect; keep local logging. */
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW, 0600);
    if(fd < 0) return -1;
    log_file = fdopen(fd, "a");
    if(!log_file) {close(fd); return -1;}
    return 0;
}
void pscloud_log(const char *level, const char *format, ...) {
    int saved = errno;
    char message[2048], stamp[32] = "unknown-time";
    va_list ap; va_start(ap, format);
    vsnprintf(message, sizeof message, format, ap); va_end(ap);
    time_t now = time(NULL); struct tm tm;
    if(localtime_r(&now, &tm)) strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(stdout, "[PSCloud] %s %s %s\n", stamp, level, message); fflush(stdout);
    if(log_file) {
        fprintf(log_file, "%s %s %s\n", stamp, level, message); fflush(log_file);
    }
    errno = saved;
}
void pscloud_notify(const char *format, ...) {
    char message[512]; va_list ap; va_start(ap, format);
    vsnprintf(message, sizeof message, format, ap); va_end(ap);
    pscloud_log("EVENT", "%s", message);
#ifdef PSCLOUD_HOST_TEST
    printf("[PS5 notification] %s\n", message); fflush(stdout);
#else
    /* Notification request ABI documented in the SDK hello_world sample. */
    typedef struct {char reserved[45]; char message[3075];} notification;
    extern int sceKernelSendNotificationRequest(int, notification *, size_t, int);
    notification req; memset(&req, 0, sizeof req);
    snprintf(req.message, sizeof req.message, "PSCloud: %s", message);
    int result = sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
    if(result < 0) pscloud_log("WARN", "PS5 notification API failed: 0x%x", (unsigned)result);
#endif
}
void pscloud_log_close(void) {
    if(log_file) {fflush(log_file); fsync(fileno(log_file)); fclose(log_file); log_file = NULL;}
}
