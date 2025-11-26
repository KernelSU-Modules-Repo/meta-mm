#ifndef MAGIC_MOUNT_H
#define MAGIC_MOUNT_H

#include <stdbool.h>
#include <stddef.h>

#define DISABLE_FILE_NAME "disable"
#define REMOVE_FILE_NAME "remove"
#define SKIP_MOUNT_FILE_NAME "skip_mount"

#define REPLACE_DIR_XATTR "trusted.overlay.opaque"
#define REPLACE_DIR_FILE_NAME ".replace"

#define DEFAULT_MOUNT_SOURCE "KSU"
#define DEFAULT_MODULE_DIR "/data/adb/modules"

/* Mount statistics */
typedef struct {
    int modules_total;
    int nodes_total;
    int nodes_mounted;
    int nodes_skipped;
    int nodes_whiteout;
    int nodes_fail;
} MountStats;

/* Core ctx */
typedef struct MagicMount {
    const char *module_dir;
    const char *mount_source;

    MountStats stats;

    char **failed_modules;
    int failed_modules_count;

    char **extra_parts;
    int extra_parts_count;

    bool enable_unmountable;
} MagicMount;

/* Initialization ctx (module_dir/mount_source) */
void magic_mount_init(MagicMount *ctx);

/* Main func */
int magic_mount(MagicMount *ctx, const char *tmp_root);

/* (failure module / extra_parts) */
void magic_mount_cleanup(MagicMount *ctx);

#endif /* MAGIC_MOUNT_H */
