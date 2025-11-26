#ifndef UTILS_H
#define UTILS_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef enum {
    LOG_ERROR = 0,
    LOG_WARN = 1,
    LOG_INFO = 2,
    LOG_DEBUG = 3,
} log_level_t;

/* global log level and log file */
extern FILE *g_log_file;
extern log_level_t g_log_level;

void log_set_file(FILE *fp);
void log_set_level(log_level_t lv);

/* log core func */
void log_write(log_level_t lv, const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

#define LOG(lv, ...)                                                                               \
    do {                                                                                           \
        if ((lv) <= g_log_level)                                                                   \
            log_write((lv), __FILE__, __LINE__, __VA_ARGS__);                                      \
    } while (0)

#define LOGE(...) LOG(LOG_ERROR, __VA_ARGS__)
#define LOGW(...) LOG(LOG_WARN, __VA_ARGS__)
#define LOGI(...) LOG(LOG_INFO, __VA_ARGS__)
#define LOGD(...) LOG(LOG_DEBUG, __VA_ARGS__)

#ifndef TMPFS_MAGIC
#define TMPFS_MAGIC 0x01021994
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define SELINUX_XATTR "security.selinux"
#define DEFAULT_TEMP_DIR "/dev/.magic_mount"

/* path helpers */
int path_join(const char *base, const char *name, char *buf, size_t n);
bool path_exists(const char *p);
bool path_is_dir(const char *p);
bool path_is_symlink(const char *p);
int mkdir_p(const char *dir);

/* temp directory auto-selection */
const char *select_auto_tempdir(char buf[PATH_MAX]);

/* str utils */

char *str_trim(char *str);
bool str_is_true(const char *str);
bool str_array_append(char ***arr, int *count, const char *str);
void str_array_free(char ***arr, int *count);

/* SELinux xattr helpers */
int set_selcon(const char *path, const char *con);
int get_selcon(const char *path, char **out);
int copy_selcon(const char *src, const char *dst);

/* Permission check */
int root_check(void);

#endif /* UTILS_H */
