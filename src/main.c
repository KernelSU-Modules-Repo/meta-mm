#include "magic_mount.h"
#include "module_tree.h"
#include "utils.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define DEFAULT_CONFIG_PATH "/data/adb/magic_mount/mm.conf"

/* --- Configuration structure --- */
typedef struct {
    const char *module_dir;
    const char *temp_dir;
    const char *mount_source;
    const char *log_file;
    const char *partitions;
    bool debug;
    bool umount;
} Config;

/* --- Forward declarations --- */
static void usage(const char *prog);
static int load_config_file(const char *path, Config *cfg, MagicMount *ctx);
static int parse_partitions(const char *list, MagicMount *ctx);
static int setup_logging(const char *log_path);
static void print_summary(const MagicMount *ctx);
static void cleanup_resources(MagicMount *ctx);

/* --- Helper function implementations --- */
static void usage(const char *prog) {
    fprintf(stderr,
            "Magic Mount: %s\n"
            "\n"
            "Usage: %s [options]\n"
            "\n"
            "Options:\n"
            "  -m, --module-dir DIR      Module directory (default: %s)\n"
            "  -t, --temp-dir DIR        Temporary directory (default: auto-detected)\n"
            "  -s, --mount-source SRC    Mount source (default: %s)\n"
            "  -p, --partitions LIST     Extra partitions (eg. mi_ext,my_stock)\n"
            "  -l, --log-file FILE       Log file (default: stderr, '-' for stdout)\n"
            "  -c, --config FILE         Config file (default: %s)\n"
            "  -v, --verbose             Enable debug logging\n"
            "      --no_umount           Disable umount\n"
            "  -h, --help                Show this help message\n"
            "\n",
            VERSION, prog, DEFAULT_MODULE_DIR, DEFAULT_MOUNT_SOURCE, DEFAULT_CONFIG_PATH);
}

static int load_config_file(const char *path, Config *cfg, MagicMount *ctx) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        if (errno != ENOENT) {
            LOGW("config file %s: %s", path, strerror(errno));
        }
        return -1;
    }

    LOGI("Loading config file: %s", path);

    char line[1024];
    int line_num = 0;

    while (fgets(line, sizeof(line), fp)) {
        line_num++;

        str_trim(line);

        if (line[0] == '\0' || line[0] == '#')
            continue;

        char *eq = strchr(line, '=');
        if (!eq) {
            LOGW("config:%d: invalid line (no '=')", line_num);
            continue;
        }

        *eq = '\0';
        char *key = str_trim(line);
        char *val = str_trim(eq + 1);

        if (*key == '\0' || *val == '\0')
            continue;

        /* Process key-value pairs */
        if (!strcasecmp(key, "module_dir")) {
            cfg->module_dir = strdup(val);

        } else if (!strcasecmp(key, "temp_dir")) {
            cfg->temp_dir = strdup(val);

        } else if (!strcasecmp(key, "mount_source")) {
            cfg->mount_source = strdup(val);

        } else if (!strcasecmp(key, "log_file")) {
            cfg->log_file = strdup(val);

        } else if (!strcasecmp(key, "debug")) {
            cfg->debug = str_is_true(val);

        } else if (!strcasecmp(key, "umount")) {
            cfg->umount = str_is_true(val);

        } else if (!strcasecmp(key, "partitions")) {
            cfg->partitions = strdup(val);

        } else {
            LOGW("config:%d: unknown key '%s'", line_num, key);
        }
    }

    fclose(fp);
    return 0;
}

static int parse_partitions(const char *list, MagicMount *ctx) {
    if (!list || !*list)
        return 0;

    const char *p = list;
    while (*p) {
        /* Find start of token */
        while (*p && (*p == ',' || isspace((unsigned char)*p)))
            p++;

        if (!*p)
            break;

        const char *start = p;

        /* Find end of token */
        while (*p && *p != ',' && !isspace((unsigned char)*p))
            p++;

        size_t len = (size_t)(p - start);
        if (len > 0) {
            extra_partition_register(ctx, start, len);
            LOGD("Added extra partition: %.*s", (int)len, start);
        }
    }

    return 0;
}

static int setup_logging(const char *log_path) {
    if (!log_path)
        return 0;

    FILE *fp = NULL;

    if (!strcmp(log_path, "-")) {
        fp = stdout;
    } else {
        fp = fopen(log_path, "a");
        if (!fp) {
            fprintf(stderr, "Error: Cannot open log file %s: %s\n", log_path, strerror(errno));
            return -1;
        }
        /* Ensure log is line-buffered */
        setvbuf(fp, NULL, _IOLBF, 0);
    }

    log_set_file(fp);
    return 0;
}

static void print_summary(const MagicMount *ctx) {
    LOGI("Summary");
    LOGI("Modules processed:     %d", ctx->stats.modules_total);
    LOGI("Nodes total:           %d", ctx->stats.nodes_total);
    LOGI("Nodes mounted:         %d", ctx->stats.nodes_mounted);
    LOGI("Nodes skipped:         %d", ctx->stats.nodes_skipped);
    LOGI("Whiteouts:             %d", ctx->stats.nodes_whiteout);
    LOGI("Failures:              %d", ctx->stats.nodes_fail);

    if (ctx->failed_modules_count > 0) {
        LOGE("Failed modules (%d):", ctx->failed_modules_count);
        for (int i = 0; i < ctx->failed_modules_count; i++) {
            LOGE("  - %s", ctx->failed_modules[i]);
        }
    } else {
        LOGI("No module failures");
    }
}

static void cleanup_resources(MagicMount *ctx) {
    if (!ctx)
        return;

    magic_mount_cleanup(ctx);

    if (g_log_file && g_log_file != stdout && g_log_file != stderr) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
}

/* --- Main function --- */
int main(int argc, char **argv) {
    MagicMount ctx;
    Config cfg = {0};
    cfg.umount = true;
    char auto_tmp[PATH_MAX] = {0};

    const char *config_path = DEFAULT_CONFIG_PATH;
    const char *tmp_dir = NULL;
    const char *cli_log_path = NULL;
    bool cli_has_partitions = false;
    int rc;

    magic_mount_init(&ctx);

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if ((!strcmp(arg, "-c") || !strcmp(arg, "--config")) && i + 1 < argc) {
            config_path = argv[++i];
        } else if ((!strcmp(arg, "-l") || !strcmp(arg, "--log-file")) && i + 1 < argc) {
            cli_log_path = argv[++i];
        }
    }

    if (cli_log_path && setup_logging(cli_log_path) < 0) {
        fprintf(stderr, "Error: Failed to setup logging to %s\n", cli_log_path);
        return 1;
    }

    load_config_file(config_path, &cfg, &ctx);

    if (!cli_log_path && cfg.log_file && setup_logging(cfg.log_file) < 0) {
        fprintf(stderr, "Error: Failed to setup logging to %s\n", cfg.log_file);
        return 1;
    }

    if (cfg.module_dir)
        ctx.module_dir = cfg.module_dir;
    if (cfg.mount_source)
        ctx.mount_source = cfg.mount_source;
    if (cfg.temp_dir)
        tmp_dir = cfg.temp_dir;
    if (cfg.debug)
        log_set_level(LOG_DEBUG);
    if (cfg.umount)
        ctx.enable_unmountable = true;
    else
        ctx.enable_unmountable = false;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if ((!strcmp(arg, "-c") || !strcmp(arg, "--config")) && i + 1 < argc) {
            i++;
            continue;
        }
        if ((!strcmp(arg, "-l") || !strcmp(arg, "--log-file")) && i + 1 < argc) {
            i++;
            continue;
        }

        if ((!strcmp(arg, "-m") || !strcmp(arg, "--module-dir")) && i + 1 < argc) {
            ctx.module_dir = argv[++i];

        } else if ((!strcmp(arg, "-t") || !strcmp(arg, "--temp-dir")) && i + 1 < argc) {
            tmp_dir = argv[++i];

        } else if ((!strcmp(arg, "-s") || !strcmp(arg, "--mount-source")) && i + 1 < argc) {
            ctx.mount_source = argv[++i];

        } else if (!strcmp(arg, "-v") || !strcmp(arg, "--verbose")) {
            log_set_level(LOG_DEBUG);

        } else if (!strcmp(arg, "--no-umount")) {
            ctx.enable_unmountable = false;

        } else if ((!strcmp(arg, "-p") || !strcmp(arg, "--partitions")) && i + 1 < argc) {
            cli_has_partitions = true;
            if (parse_partitions(argv[++i], &ctx) < 0) {
                LOGE("failed to parse partitions");
                cleanup_resources(&ctx);
                return 1;
            }

        } else if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
            usage(argv[0]);
            cleanup_resources(&ctx);
            return 0;

        } else {
            fprintf(stderr, "Error: Unknown argument: %s\n\n", arg);
            usage(argv[0]);
            cleanup_resources(&ctx);
            return 1;
        }
    }

    if (!cli_has_partitions && cfg.partitions) {
        if (parse_partitions(cfg.partitions, &ctx) < 0) {
            LOGW("failed to parse partitions from config");
            cleanup_resources(&ctx);
            return 1;
        }
    }

    /* Determine temp directory */
    if (!tmp_dir)
        tmp_dir = select_auto_tempdir(auto_tmp);

    if (!tmp_dir || *tmp_dir == '\0') {
        LOGE("failed to determine temp directory");
        cleanup_resources(&ctx);
        return 1;
    }

    /* Validate environment */
    if (root_check() < 0) {
        cleanup_resources(&ctx);
        return 1;
    }

    /* Log startup information */
    LOGI("Magic Mount %s Starting", VERSION);
    LOGI("Configuration:");
    LOGI("  Module directory:  %s", ctx.module_dir);
    LOGI("  Temp directory:    %s", tmp_dir);
    LOGI("  Mount source:      %s", ctx.mount_source);
    LOGI("  Log level:         %s", g_log_level == LOG_DEBUG ? "DEBUG" : "INFO");
    if (ctx.extra_parts_count > 0) {
        LOGI("  Extra partitions:  %d", ctx.extra_parts_count);
        for (int i = 0; i < ctx.extra_parts_count; i++) {
            LOGI("    - %s", ctx.extra_parts[i]);
        }
    }

    /* Perform magic mount */
    rc = magic_mount(&ctx, tmp_dir);

    /* Print results */
    if (rc == 0) {
        LOGI("Magic Mount Completed Successfully");
    } else {
        LOGE("Magic Mount Failed (rc=%d)", rc);
    }

    print_summary(&ctx);
    cleanup_resources(&ctx);

    return rc == 0 ? 0 : 1;
}
