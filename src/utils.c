#include "utils.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>

/* --- log func --- */

FILE *g_log_file = NULL;
log_level_t g_log_level = LOG_INFO;

bool g_log_initialized = false;

struct log_entry {
    char *line;
};

static struct log_entry *g_log_buf = NULL;
static size_t g_log_count = 0;
static size_t g_log_cap = 0;

static const char *log_level_str(log_level_t lv) {
    switch (lv) {
    case LOG_ERROR:
        return "ERROR";
    case LOG_WARN:
        return "WARN";
    case LOG_INFO:
        return "INFO";
    case LOG_DEBUG:
        return "DEBUG";
    }
    return "?";
}

static void log_buffer_append(const char *line) {
    if (!line)
        return;

    if (g_log_count == g_log_cap) {
        size_t new_cap = g_log_cap ? g_log_cap * 2 : 16;
        struct log_entry *new_buf = realloc(g_log_buf, new_cap * sizeof(*new_buf));
        if (!new_buf) {
            fprintf(stderr, "%s\n", line);
            return;
        }
        g_log_buf = new_buf;
        g_log_cap = new_cap;
    }

    g_log_buf[g_log_count].line = strdup(line);
    if (!g_log_buf[g_log_count].line) {
        fprintf(stderr, "%s\n", line);
        return;
    }
    g_log_count++;
}

static void log_flush_buffer(FILE *out) {
    if (!out)
        out = stderr;

    for (size_t i = 0; i < g_log_count; i++) {
        if (g_log_buf[i].line) {
            fputs(g_log_buf[i].line, out);
            fputc('\n', out);
            free(g_log_buf[i].line);
        }
    }

    free(g_log_buf);
    g_log_buf = NULL;
    g_log_count = 0;
    g_log_cap = 0;

    fflush(out);
}

void log_set_file(FILE *fp) {
    g_log_file = fp;

    FILE *out = g_log_file ? g_log_file : stderr;

    if (!g_log_initialized) {
        g_log_initialized = true;
        if (g_log_count > 0) {
            log_flush_buffer(out);
        }
    }
}

void log_set_level(log_level_t lv) { g_log_level = lv; }

void log_write(log_level_t lv, const char *file, int line, const char *fmt, ...) {
    (void)lv;

    char buf[1024];

    int off = snprintf(buf, sizeof(buf), "[%s] %s:%d: ", log_level_str(lv), file, line);
    if (off < 0)
        return;
    if ((size_t)off >= sizeof(buf))
        off = (int)(sizeof(buf) - 1);

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + off, sizeof(buf) - (size_t)off, fmt, ap);
    va_end(ap);

    if (n < 0)
        return;

    buf[sizeof(buf) - 1] = '\0';

    if (!g_log_initialized) {
        log_buffer_append(buf);
        return;
    }

    FILE *out = g_log_file ? g_log_file : stderr;

    fputs(buf, out);
    fputc('\n', out);
    fflush(out);
}

/* --- path helpers --- */

int path_join(const char *base, const char *name, char *buf, size_t n) {
    if (!base || !buf || n == 0) {
        errno = EINVAL;
        return -1;
    }

    if (!name || name[0] == '\0') {
        if (snprintf(buf, n, "%s", base) >= (int)n) {
            errno = ENAMETOOLONG;
            return -1;
        }
        return 0;
    }

    if (base[0] == '\0' || (base[0] == '/' && base[1] == '\0')) {
        if (snprintf(buf, n, "/%s", name) >= (int)n) {
            errno = ENAMETOOLONG;
            return -1;
        }
        return 0;
    }

    size_t len = strlen(base);

    if (base[len - 1] == '/') {
        if (snprintf(buf, n, "%s%s", base, name) >= (int)n) {
            errno = ENAMETOOLONG;
            return -1;
        }
    } else {
        if (snprintf(buf, n, "%s/%s", base, name) >= (int)n) {
            errno = ENAMETOOLONG;
            return -1;
        }
    }
    return 0;
}

bool path_exists(const char *p) {
    struct stat st;
    return (stat(p, &st) == 0);
}

bool path_is_dir(const char *p) {
    struct stat st;
    return (stat(p, &st) == 0) && S_ISDIR(st.st_mode);
}

bool path_is_symlink(const char *p) {
    struct stat st;
    return (lstat(p, &st) == 0) && S_ISLNK(st.st_mode);
}

int mkdir_p(const char *dir) {
    if (!dir || !dir[0]) {
        errno = EINVAL;
        return -1;
    }

    struct stat st;
    if (stat(dir, &st) == 0) {
        if (S_ISDIR(st.st_mode))
            return 0;
        errno = ENOTDIR;
        return -1;
    }

    char tmp[PATH_MAX];
    if (strlen(dir) >= sizeof(tmp)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    strcpy(tmp, dir);
    char *s = strrchr(tmp, '/');

    if (s && s != tmp) {
        *s = '\0';
        if (mkdir_p(tmp) < 0)
            return -1;
    }

    if (mkdir(dir, 0755) == 0 || errno == EEXIST)
        return 0;

    return -1;
}

/* --- tmpfs check and tempdir set --- */

static bool is_rw_tmpfs(const char *path) {
    if (!path_is_dir(path))
        return false;

    struct statfs s;
    if (statfs(path, &s) < 0)
        return false;

    if ((unsigned long)s.f_type != TMPFS_MAGIC)
        return false;

    char tmpl[PATH_MAX];
    if (path_join(path, ".magic_mount_testXXXXXX", tmpl, sizeof(tmpl)) != 0)
        return false;

    int fd = mkstemp(tmpl);
    if (fd < 0)
        return false;

    close(fd);
    unlink(tmpl);
    return true;
}

const char *select_auto_tempdir(char buf[PATH_MAX]) {
    const char *candidates[] = {
        "/mnt/vendor",
        "/mnt",
        "/debug_ramdisk",
    };

    size_t n = sizeof(candidates) / sizeof(candidates[0]);

    for (size_t i = 0; i < n; i++) {

        if (!is_rw_tmpfs(candidates[i]))
            continue;

        if (path_join(candidates[i], ".magic_mount", buf, PATH_MAX) == 0) {
            LOGI("auto tempdir selected: %s (from %s)", buf, candidates[i]);
            return buf;
        }
    }

    LOGW("no rw tmpfs found, fallback to %s", DEFAULT_TEMP_DIR);
    return DEFAULT_TEMP_DIR;
}

/* --- str utils --- */

char *str_trim(char *str) {
    if (!str)
        return NULL;

    char *s = str;

    /* Trim leading whitespace */
    while (*s && isspace((unsigned char)*s))
        s++;

    if (*s == '\0') {
        *str = '\0';
        return str;
    }

    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end))
        end--;

    end[1] = '\0';

    if (s != str)
        memmove(str, s, strlen(s) + 1);

    return str;
}

bool str_is_true(const char *str) {
    if (!str)
        return false;

    return !strcasecmp(str, "true") || !strcasecmp(str, "yes") || !strcasecmp(str, "1") ||
           !strcasecmp(str, "on");
}

bool str_array_append(char ***arr, int *count, const char *str) {
    if (!arr || !count || !str) {
        errno = EINVAL;
        return false;
    }

    char **old = *arr;
    int old_count = *count;
    int new_count = old_count + 1;

    char **tmp = realloc(old, (size_t)new_count * sizeof(char *));
    if (!tmp)
        return false;

    tmp[old_count] = strdup(str);
    if (!tmp[old_count]) {
        *arr = tmp;
        *count = old_count;
        return false;
    }

    *arr = tmp;
    *count = new_count;
    return true;
}

void str_array_free(char ***arr, int *count) {
    if (!arr || !*arr || !count)
        return;

    for (int i = 0; i < *count; i++)
        free((*arr)[i]);

    free(*arr);
    *arr = NULL;
    *count = 0;
}

/* --- SELinux xattr --- */

int set_selcon(const char *path, const char *con) {
    if (!path || !con) {
        LOGD("set_selcon: skip null args");
        return 0;
    }

    LOGD("set_selcon(%s, \"%s\")", path, con);

    if (lsetxattr(path, SELINUX_XATTR, con, strlen(con), 0) < 0) {
        LOGW("setcon %s: %s", path, strerror(errno));
        return -1;
    }

    return 0;
}

int get_selcon(const char *path, char **out) {
    *out = NULL;

    if (!path) {
        LOGD("get_selcon: null path");
        errno = EINVAL;
        return -1;
    }

    ssize_t sz = lgetxattr(path, SELINUX_XATTR, NULL, 0);
    if (sz < 0) {
        LOGW("getcon %s: %s", path, strerror(errno));
        return -1;
    }

    char *buf = malloc(sz + 1);
    if (!buf) {
        errno = ENOMEM;
        return -1;
    }

    sz = lgetxattr(path, SELINUX_XATTR, buf, sz);
    if (sz < 0) {
        LOGW("getcon %s: %s", path, strerror(errno));
        free(buf);
        return -1;
    }

    buf[sz] = '\0';
    *out = buf;

    LOGD("get_selcon(%s) -> \"%s\"", path, buf);
    return 0;
}

int copy_selcon(const char *src, const char *dst) {
    if (!src || !dst) {
        LOGD("copy_selcon: skip null args");
        errno = EINVAL;
        return -1;
    }

    LOGD("copy_selcon(%s -> %s)", src, dst);

    char *con = NULL;
    if (get_selcon(src, &con) < 0)
        return -1;

    int ret = set_selcon(dst, con);
    free(con);

    return ret;
}

/* --- Permission check --- */

int root_check(void) {
    /* Check root privileges */
    if (geteuid() != 0) {
        LOGE("Must run as root (current euid=%d)", (int)geteuid());
        return -1;
    }

    return 0;
}
