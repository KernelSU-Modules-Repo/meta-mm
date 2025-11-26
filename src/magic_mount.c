#include "magic_mount.h"
#include "ksu.h"
#include "module_tree.h"
#include "utils.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

void magic_mount_init(MagicMount *ctx) {
    if (!ctx)
        return;
    memset(ctx, 0, sizeof(*ctx));
    ctx->module_dir = DEFAULT_MODULE_DIR;
    ctx->mount_source = DEFAULT_MOUNT_SOURCE;
    ctx->enable_unmountable = true;
}

void magic_mount_cleanup(MagicMount *ctx) {
    if (!ctx)
        return;
    module_tree_cleanup(ctx);
}

static int mm_clone_symlink(const char *src, const char *dst) {
    char target[PATH_MAX];

    ssize_t len = readlink(src, target, sizeof(target) - 1);
    if (len < 0) {
        LOGE("readlink %s: %s", src, strerror(errno));
        return -1;
    }

    target[len] = '\0';

    if (symlink(target, dst) < 0) {
        LOGE("symlink %s->%s: %s", dst, target, strerror(errno));
        return -1;
    }

    (void)copy_selcon(src, dst);

    LOGD("clone symlink %s -> %s (%s)", src, dst, target);
    return 0;
}

static int mm_mirror_entry(MagicMount *ctx, const char *path, const char *work, const char *name);

static int mm_apply_node_recursive(MagicMount *ctx, const char *base, const char *wbase, Node *node,
                                   bool has_tmpfs);

static int mm_mirror_entry(MagicMount *ctx, const char *path, const char *work, const char *name) {
    char src[PATH_MAX];
    char dst[PATH_MAX];

    if (path_join(path, name, src, sizeof(src)) != 0 ||
        path_join(work, name, dst, sizeof(dst)) != 0)
        return -1;

    struct stat st;
    if (lstat(src, &st) < 0) {
        LOGW("lstat %s: %s", src, strerror(errno));
        return 0;
    }

    if (S_ISREG(st.st_mode)) {
        int fd = open(dst, O_CREAT | O_WRONLY, st.st_mode & 07777);
        if (fd < 0) {
            LOGE("create %s: %s", dst, strerror(errno));
            return -1;
        }
        close(fd);

        if (mount(src, dst, NULL, MS_BIND, NULL) < 0) {
            LOGE("bind %s->%s: %s", src, dst, strerror(errno));
            return -1;
        }
    } else if (S_ISDIR(st.st_mode)) {
        if (mkdir(dst, st.st_mode & 07777) < 0 && errno != EEXIST) {
            LOGE("mkdir %s: %s", dst, strerror(errno));
            return -1;
        }

        chmod(dst, st.st_mode & 07777);
        chown(dst, st.st_uid, st.st_gid);

        (void)copy_selcon(src, dst);

        DIR *d = opendir(src);
        if (!d) {
            LOGE("opendir %s: %s", src, strerror(errno));
            return -1;
        }

        struct dirent *de;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) {
                continue;
            }
            if (mm_mirror_entry(ctx, src, dst, de->d_name) != 0) {
                closedir(d);
                return -1;
            }
        }
        closedir(d);
    } else if (S_ISLNK(st.st_mode)) {
        if (mm_clone_symlink(src, dst) != 0)
            return -1;
    }

    return 0;
}

static int mm_apply_regular_file(MagicMount *ctx, const char *path, const char *wpath, Node *node,
                                 bool has_tmpfs) {
    const char *target = has_tmpfs ? wpath : path;

    if (has_tmpfs) {
        char parent_dir[PATH_MAX];
        snprintf(parent_dir, sizeof(parent_dir), "%s", wpath);
        char *last_slash = strrchr(parent_dir, '/');
        if (last_slash && last_slash != parent_dir) {
            *last_slash = '\0';
            if (mkdir_p(parent_dir) != 0)
                return -1;
        }

        int fd = open(wpath, O_CREAT | O_WRONLY, 0644);
        if (fd < 0) {
            LOGE("create %s: %s", wpath, strerror(errno));
            return -1;
        }
        close(fd);
    }

    if (!node->module_path) {
        LOGE("no module file for %s", path);
        errno = EINVAL;
        return -1;
    }

    LOGD("bind %s -> %s", node->module_path, target);

    if (mount(node->module_path, target, NULL, MS_BIND, NULL) < 0) {
        LOGE("bind %s->%s: %s", node->module_path, target, strerror(errno));
        return -1;
    } else if (!strstr(target, ".magic_mount/workdir/")) {
        if (ctx->enable_unmountable)
            ksu_send_unmountable(path);
    }

    (void)mount(NULL, target, NULL, MS_REMOUNT | MS_BIND | MS_RDONLY, NULL);

    ctx->stats.nodes_mounted++;
    return 0;
}

static int mm_apply_symlink(MagicMount *ctx, const char *path, const char *wpath, Node *node) {
    if (!node->module_path) {
        LOGE("no module symlink for %s", path);
        errno = EINVAL;
        return -1;
    }

    if (mm_clone_symlink(node->module_path, wpath) != 0)
        return -1;

    ctx->stats.nodes_mounted++;
    return 0;
}

static bool mm_check_need_tmpfs(Node *node, const char *path) {
    for (size_t i = 0; i < node->child_count; ++i) {
        Node *c = node->children[i];
        char rp[PATH_MAX];

        if (path_join(path, c->name, rp, sizeof(rp)) != 0)
            continue;

        LOGD("checking child: parent=%s, child=%s, joined_path=%s", path, c->name, rp);

        bool need = false;

        if (c->type == NFT_SYMLINK) {
            need = true;
            LOGD("child %s is SYMLINK", c->name);
        } else if (c->type == NFT_WHITEOUT) {
            need = path_exists(rp);
            LOGD("child %s is WHITEOUT, path_exists=%d, need=%d", c->name, need, need);
        } else {
            struct stat st;
            if (lstat(rp, &st) == 0) {
                NodeFileType rt = node_type_from_stat(&st);
                LOGD("type mismatch check: %s - expected=%d, actual=%d, is_symlink=%d", rp, c->type,
                     rt, rt == NFT_SYMLINK ? 1 : 0);
                if (rt != c->type || rt == NFT_SYMLINK)
                    need = true;
            } else {
                LOGD("lstat failed for %s: %s (errno=%d), path_exists=%d", rp, strerror(errno),
                     errno, path_exists(rp) ? 1 : 0);
                need = true;
            }
        }

        LOGD("child check: parent=%s, child=%s, type=%d, need=%d, has_module_path=%d", path,
             c->name, c->type, need, node->module_path ? 1 : 0);

        if (need) {
            if (!node->module_path) {
                LOGE("cannot create tmpfs on %s (%s) - child type: %d, target exists: %d", path,
                     c->name, c->type, path_exists(rp) ? 1 : 0);
                c->skip = true;
                continue;
            }
            return true;
        }
    }
    return false;
}

static int mm_setup_dir_tmpfs(const char *path, const char *wpath, Node *node) {
    if (mkdir_p(wpath) != 0)
        return -1;

    struct stat st;
    const char *meta_path = NULL;

    if (stat(path, &st) == 0) {
        meta_path = path;
    } else if (node->module_path && stat(node->module_path, &st) == 0) {
        meta_path = node->module_path;
    } else {
        LOGE("no dir meta for %s", path);
        errno = ENOENT;
        return -1;
    }

    chmod(wpath, st.st_mode & 07777);
    chown(wpath, st.st_uid, st.st_gid);
    (void)copy_selcon(meta_path, wpath);

    return 0;
}

static int mm_process_dir_children(MagicMount *ctx, const char *path, const char *wpath, Node *node,
                                   bool now_tmp) {
    if (!path_exists(path) || node->replace)
        return 0;

    DIR *d = opendir(path);
    if (!d) {
        LOGE("opendir %s: %s", path, strerror(errno));
        return now_tmp ? -1 : 0;
    }

    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;

        Node *c = node_child_find(node, de->d_name);
        int r = 0;

        if (c) {
            if (c->skip) {
                c->done = true;
                continue;
            }
            c->done = true;
            r = mm_apply_node_recursive(ctx, path, wpath, c, now_tmp);
        } else if (now_tmp) {
            r = mm_mirror_entry(ctx, path, wpath, de->d_name);
        }

        if (r != 0) {
            const char *mn = NULL;
            if (c && c->module_name)
                mn = c->module_name;
            else if (node->module_name)
                mn = node->module_name;

            if (mn) {
                LOGE("child %s/%s failed (module: %s)", path, c ? c->name : de->d_name, mn);
                module_mark_failed(ctx, mn);
            } else {
                LOGE("child %s/%s failed (no module_name)", path, c ? c->name : de->d_name);
            }

            ctx->stats.nodes_fail++;

            if (now_tmp) {
                closedir(d);
                return -1;
            }
        }
    }
    closedir(d);
    return 0;
}

static int mm_process_remaining_children(MagicMount *ctx, const char *path, const char *wpath,
                                         Node *node, bool now_tmp) {
    for (size_t i = 0; i < node->child_count; ++i) {
        Node *c = node->children[i];
        if (c->skip || c->done)
            continue;

        int r = mm_apply_node_recursive(ctx, path, wpath, c, now_tmp);
        if (r != 0) {
            const char *mn = c->module_name ? c->module_name : node->module_name;

            if (mn) {
                LOGE("child %s/%s failed (module: %s)", path, c->name, mn);
                module_mark_failed(ctx, mn);
            } else {
                LOGE("child %s/%s failed (no module_name)", path, c->name);
            }

            ctx->stats.nodes_fail++;
            if (now_tmp)
                return -1;
        }
    }
    return 0;
}

static int mm_apply_node_recursive(MagicMount *ctx, const char *base, const char *wbase, Node *node,
                                   bool has_tmpfs) {
    char path[PATH_MAX];
    char wpath[PATH_MAX];

    if (path_join(base, node->name, path, sizeof(path)) != 0 ||
        path_join(wbase, node->name, wpath, sizeof(wpath)) != 0)
        return -1;

    switch (node->type) {
    case NFT_REGULAR:
        return mm_apply_regular_file(ctx, path, wpath, node, has_tmpfs);

    case NFT_SYMLINK:
        return mm_apply_symlink(ctx, path, wpath, node);

    case NFT_WHITEOUT:
        LOGD("whiteout %s", path);
        ctx->stats.nodes_whiteout++;
        return 0;

    case NFT_DIRECTORY: {
        bool create_tmp = (!has_tmpfs && node->replace && node->module_path);

        if (!has_tmpfs && !create_tmp) {
            create_tmp = mm_check_need_tmpfs(node, path);
        }

        bool now_tmp = has_tmpfs || create_tmp;

        if (now_tmp) {
            if (mm_setup_dir_tmpfs(path, wpath, node) != 0)
                return -1;
        }

        if (create_tmp) {
            if (mount(wpath, wpath, NULL, MS_BIND, NULL) < 0) {
                LOGE("bind self %s: %s", wpath, strerror(errno));
                return -1;
            }
        }

        if (mm_process_dir_children(ctx, path, wpath, node, now_tmp) != 0)
            return -1;

        if (mm_process_remaining_children(ctx, path, wpath, node, now_tmp) != 0)
            return -1;

        if (create_tmp) {
            (void)mount(NULL, wpath, NULL, MS_REMOUNT | MS_BIND | MS_RDONLY, NULL);

            if (mount(wpath, path, NULL, MS_MOVE, NULL) < 0) {
                LOGE("move %s->%s failed: %s", wpath, path, strerror(errno));
                if (node->module_name)
                    module_mark_failed(ctx, node->module_name);
                return -1;
            }

            LOGI("move mountpoint success: %s -> %s", wpath, path);
            (void)mount(NULL, path, NULL, MS_REC | MS_PRIVATE, NULL);

            if (ctx->enable_unmountable)
                ksu_send_unmountable(path);
        }

        ctx->stats.nodes_mounted++;
        return 0;
    }
    }

    return 0;
}

int magic_mount(MagicMount *ctx, const char *tmp_root) {
    if (!ctx)
        return -1;

    Node *root = build_mount_tree(ctx);
    if (!root) {
        LOGI("no modules, magic_mount skipped");
        return 0;
    }

    char tmp_dir[PATH_MAX];
    if (path_join(tmp_root, "workdir", tmp_dir, sizeof(tmp_dir)) != 0) {
        node_free(root);
        return -1;
    }

    if (mkdir_p(tmp_dir) != 0) {
        node_free(root);
        return -1;
    }

    LOGI("starting magic_mount core logic: tmpfs_source=%s tmp_dir=%s", ctx->mount_source, tmp_dir);

    if (mount(ctx->mount_source, tmp_dir, "tmpfs", 0, "") < 0) {
        LOGE("mount tmpfs %s: %s", tmp_dir, strerror(errno));
        node_free(root);
        return -1;
    }

    (void)mount(NULL, tmp_dir, NULL, MS_REC | MS_PRIVATE, NULL);

    int rc = mm_apply_node_recursive(ctx, "/", tmp_dir, root, false);
    if (rc != 0)
        ctx->stats.nodes_fail++;

    if (umount2(tmp_dir, MNT_DETACH) < 0)
        LOGE("umount %s: %s", tmp_dir, strerror(errno));

    (void)rmdir(tmp_dir);

    node_free(root);
    return rc;
}
