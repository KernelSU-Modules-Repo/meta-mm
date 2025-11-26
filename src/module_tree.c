#include "module_tree.h"
#include "magic_mount.h"
#include "utils.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>

/* --- Node basic mgr --- */

static Node *node_new(const char *name, NodeFileType t) {
    Node *n = calloc(1, sizeof(Node));
    if (!n)
        return NULL;

    n->name = strdup(name ? name : "");
    n->type = t;
    return n;
}

void node_free(Node *n) {
    if (!n)
        return;

    for (size_t i = 0; i < n->child_count; ++i)
        node_free(n->children[i]);

    free(n->children);
    free(n->name);
    free(n->module_path);
    free(n->module_name);
    free(n);
}

NodeFileType node_type_from_stat(const struct stat *st) {
    if (S_ISCHR(st->st_mode) && st->st_rdev == 0)
        return NFT_WHITEOUT;
    if (S_ISREG(st->st_mode))
        return NFT_REGULAR;
    if (S_ISDIR(st->st_mode))
        return NFT_DIRECTORY;
    if (S_ISLNK(st->st_mode))
        return NFT_SYMLINK;
    return NFT_WHITEOUT;
}

static bool dir_is_replace(const char *path) {
    char buf[8];
    ssize_t len = lgetxattr(path, REPLACE_DIR_XATTR, buf, sizeof(buf) - 1);

    if (len > 0) {
        buf[len] = '\0';
        if (strcmp(buf, "y") == 0)
            return true;
    }

    int dirfd = open(path, O_RDONLY | O_DIRECTORY);
    if (dirfd < 0)
        return false;

    bool exists = (faccessat(dirfd, REPLACE_DIR_FILE_NAME, F_OK, 0) == 0);
    close(dirfd);
    return exists;
}

static Node *node_create_from_fs(MagicMount *ctx, const char *name, const char *path,
                                 const char *module_name) {
    struct stat st;
    if (lstat(path, &st) < 0) {
        LOGD("node_create_from_fs: lstat(%s) failed: %s", path, strerror(errno));
        return NULL;
    }

    if (!(S_ISCHR(st.st_mode) || S_ISREG(st.st_mode) || S_ISDIR(st.st_mode) ||
          S_ISLNK(st.st_mode))) {
        LOGD("node_create_from_fs: skip unsupported file type for %s (mode=%o)", path, st.st_mode);
        return NULL;
    }

    NodeFileType t = node_type_from_stat(&st);
    Node *n = node_new(name, t);
    if (!n) {
        LOGE("node_create_from_fs: failed to allocate node for %s", path);
        return NULL;
    }

    n->module_path = strdup(path);
    if (module_name)
        n->module_name = strdup(module_name);
    n->replace = (t == NFT_DIRECTORY) && dir_is_replace(path);

    LOGD("node_create_from_fs: created node '%s' (type=%d, replace=%d, module=%s, path=%s)", name,
         t, n->replace, module_name ? module_name : "(none)", path);

    ctx->stats.nodes_total++;
    return n;
}

static int node_child_append(Node *parent, Node *child) {
    if (!parent || !child) {
        LOGE("node_child_append: parent or child is NULL");
        errno = EINVAL;
        return -1;
    }

    LOGD("node_child_append: parent='%s' add child='%s'", parent->name ? parent->name : "(root)",
         child->name ? child->name : "(null)");

    Node **arr = realloc(parent->children, (parent->child_count + 1) * sizeof(Node *));
    if (!arr) {
        LOGE("node_child_append: realloc failed (parent='%s', child='%s')",
             parent->name ? parent->name : "(root)", child->name ? child->name : "(null)");
        errno = ENOMEM;
        return -1;
    }

    parent->children = arr;
    parent->children[parent->child_count++] = child;
    return 0;
}

Node *node_child_find(Node *parent, const char *name) {
    for (size_t i = 0; i < parent->child_count; ++i) {
        if (strcmp(parent->children[i]->name, name) == 0)
            return parent->children[i];
    }
    return NULL;
}

static Node *node_child_detach(Node *parent, const char *name) {
    for (size_t i = 0; i < parent->child_count; ++i) {
        if (strcmp(parent->children[i]->name, name) == 0) {
            Node *n = parent->children[i];
            memmove(&parent->children[i], &parent->children[i + 1],
                    (parent->child_count - i - 1) * sizeof(Node *));
            parent->child_count--;
            return n;
        }
    }
    return NULL;
}

void module_mark_failed(MagicMount *ctx, const char *module_name) {
    if (!ctx || !module_name)
        return;

    // Check for duplicates
    for (int i = 0; i < ctx->failed_modules_count; ++i) {
        if (strcmp(ctx->failed_modules[i], module_name) == 0)
            return;
    }

    if (!str_array_append(&ctx->failed_modules, &ctx->failed_modules_count, module_name)) {
        LOGW("failed to record module failure for %s (OOM)", module_name);
    }
}

/* --- Extra partition blacklist --- */

static bool extra_part_blacklisted(const char *name) {
    if (!name || !*name)
        return false;

    while (*name == '/')
        name++;

    char buf[16];
    size_t i = 0;
    while (name[i] != '\0' && name[i] != '/' && i + 1 < sizeof(buf)) {
        buf[i] = name[i];
        i++;
    }
    buf[i] = '\0';

    static const char *blacklist[] = {
        "bin", "etc",  "data", "data_mirror", "sdcard",  "tmp",    "dev",        "sys",
        "mnt", "proc", "d",    "test",        "product", "vendor", "system_ext", "odm"};
    size_t n = sizeof(blacklist) / sizeof(blacklist[0]);

    for (size_t j = 0; j < n; ++j) {
        if (strcmp(buf, blacklist[j]) == 0)
            return true;
    }
    return false;
}

void extra_partition_register(MagicMount *ctx, const char *start, size_t len) {
    if (!ctx) {
        LOGE("extra_partition_register: NULL context");
        return;
    }

    if (!start || len == 0) {
        LOGW("extra_partition_register: invalid input (start=%p, len=%zu)", (void *)start, len);
        return;
    }

    char *buf = malloc(len + 1);
    if (!buf) {
        LOGE("extra_partition_register: malloc failed for %zu bytes", len + 1);
        return;
    }

    memcpy(buf, start, len);
    buf[len] = '\0';

    LOGD("extra_partition_register: processing '%s' (len=%zu)", buf, len);

    size_t original_len = strlen(buf);
    str_trim(buf);
    size_t trimmed_len = strlen(buf);

    if (original_len != trimmed_len) {
        LOGD("extra_partition_register: trimmed whitespace (%zu -> %zu bytes)", original_len,
             trimmed_len);
    }

    if (buf[0] == '\0') {
        LOGW("extra_partition_register: rejected empty string after trim");
        free(buf);
        return;
    }

    if (extra_part_blacklisted(buf)) {
        LOGW("extra_partition_register: rejected '%s' (blacklisted)", buf);
        free(buf);
        return;
    }

    if (!str_array_append(&ctx->extra_parts, &ctx->extra_parts_count, buf)) {
        LOGE("extra_partition_register: failed to add '%s' (OOM or array error, count=%d)", buf,
             ctx->extra_parts_count);
        free(buf);
        return;
    }

    LOGI("extra_partition_register: success added '%s' (total: %d partitions)", buf,
         ctx->extra_parts_count);

    free(buf);
}

static bool module_is_disabled(const char *mod_dir) {
    char buf[PATH_MAX];
    const char *disable_files[] = {DISABLE_FILE_NAME, REMOVE_FILE_NAME, SKIP_MOUNT_FILE_NAME};

    for (size_t i = 0; i < sizeof(disable_files) / sizeof(disable_files[0]); ++i) {
        if (path_join(mod_dir, disable_files[i], buf, sizeof(buf)) == 0 && path_exists(buf))
            return true;
    }
    return false;
}

/* --- Node collect --- */

static int node_scan_dir(MagicMount *ctx, Node *self, const char *dir, const char *module_name,
                         bool *has_any) {
    LOGD("node_scan_dir: enter dir=%s module=%s node='%s'", dir,
         module_name ? module_name : "(none)", self && self->name ? self->name : "(null)");

    DIR *d = opendir(dir);
    if (!d) {
        LOGE("opendir %s: %s", dir, strerror(errno));
        return -1;
    }

    struct dirent *de;
    bool any = false;
    char path[PATH_MAX];

    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;

        if (path_join(dir, de->d_name, path, sizeof(path)) != 0) {
            LOGE("node_scan_dir: path_join failed for dir=%s name=%s", dir, de->d_name);
            closedir(d);
            return -1;
        }

        LOGD("node_scan_dir: processing '%s' (full=%s)", de->d_name, path);

        Node *child = node_child_find(self, de->d_name);
        if (!child) {
            Node *n = node_create_from_fs(ctx, de->d_name, path, module_name);
            if (n && node_child_append(self, n) == 0) {
                child = n;
            } else if (n) {
                LOGE("node_scan_dir: failed to add child '%s' to '%s'", de->d_name,
                     self && self->name ? self->name : "(null)");
                node_free(n);
            } else {
                LOGD("node_scan_dir: node_create_from_fs returned NULL for %s", path);
            }
        }

        if (child) {
            if (child->type == NFT_DIRECTORY) {
                bool sub = false;
                if (node_scan_dir(ctx, child, path, module_name, &sub) != 0) {
                    LOGE("node_scan_dir: recurse failed for dir=%s", path);
                    closedir(d);
                    return -1;
                }
                if (sub || child->replace) {
                    LOGD("node_scan_dir: directory '%s' has content (sub=%d, replace=%d)",
                         child->name, sub, child->replace);
                    any = true;
                }
            } else {
                LOGD("node_scan_dir: file node '%s' has content (type=%d)", child->name,
                     child->type);
                any = true;
            }
        } else {
            LOGD("node_scan_dir: no child node created for %s", path);
        }
    }

    closedir(d);
    *has_any = any;
    LOGD("node_scan_dir: leave dir=%s has_any=%d", dir, any);
    return 0;
}

/* --- Symlink compatibility --- */

static bool is_compatible_symlink(const char *link_target, const char *part_name,
                                  const MagicMount *ctx, const char *module_name) {
    // Remove trailing slashes
    size_t target_len = strlen(link_target);
    while (target_len > 0 && link_target[target_len - 1] == '/') {
        target_len--;
    }
    if (target_len == 0)
        return false;

    // Check relative: ../part_name
    char expected_relative[PATH_MAX];
    snprintf(expected_relative, sizeof(expected_relative), "../%s", part_name);
    if (strncmp(link_target, expected_relative, target_len) == 0 &&
        expected_relative[target_len] == '\0')
        return true;

    // Check absolute: /module_dir/module_name/part_name
    if (ctx && ctx->module_dir && module_name) {
        char expected_absolute[PATH_MAX];
        char tmp[PATH_MAX];

        if (path_join(ctx->module_dir, module_name, tmp, sizeof(tmp)) == 0 &&
            path_join(tmp, part_name, expected_absolute, sizeof(expected_absolute)) == 0) {
            if (strncmp(link_target, expected_absolute, target_len) == 0 &&
                expected_absolute[target_len] == '\0')
                return true;
        }
    }

    return false;
}

static int find_real_partition_dir(MagicMount *ctx, const char *part_name, char *out_path,
                                   char *out_module, size_t buf_size) {
    DIR *mod_dir = opendir(ctx->module_dir);
    if (!mod_dir) {
        LOGE("opendir %s: %s", ctx->module_dir, strerror(errno));
        return -1;
    }

    struct dirent *mod_de;
    int result = -1;

    while ((mod_de = readdir(mod_dir))) {
        if (!strcmp(mod_de->d_name, ".") || !strcmp(mod_de->d_name, ".."))
            continue;

        char mod_path[PATH_MAX], part_path[PATH_MAX];

        if (path_join(ctx->module_dir, mod_de->d_name, mod_path, sizeof(mod_path)) != 0)
            continue;

        struct stat mod_st;
        if (stat(mod_path, &mod_st) < 0 || !S_ISDIR(mod_st.st_mode))
            continue;

        if (module_is_disabled(mod_path))
            continue;

        if (path_join(mod_path, part_name, part_path, sizeof(part_path)) != 0)
            continue;

        if (path_is_dir(part_path)) {
            strncpy(out_path, part_path, buf_size - 1);
            out_path[buf_size - 1] = '\0';
            strncpy(out_module, mod_de->d_name, buf_size - 1);
            out_module[buf_size - 1] = '\0';
            result = 0;
            break;
        }
    }

    closedir(mod_dir);
    return result;
}

static int symlink_resolve_partition(MagicMount *ctx, Node *system, const char *part_name) {
    if (!system || !part_name)
        return -1;

    Node *sys_child = node_child_find(system, part_name);
    if (!sys_child || sys_child->type != NFT_SYMLINK || !sys_child->module_path)
        return 0;

    char link_target[PATH_MAX];
    ssize_t len = readlink(sys_child->module_path, link_target, sizeof(link_target) - 1);
    if (len < 0) {
        LOGW("readlink %s failed: %s", sys_child->module_path, strerror(errno));
        return 0;
    }
    link_target[len] = '\0';

    if (!is_compatible_symlink(link_target, part_name, ctx, sys_child->module_name)) {
        LOGD("symlink %s -> %s (not compatible)", part_name, link_target);
        return 0;
    }

    LOGI("found compatible symlink: system/%s -> %s", part_name, link_target);

    char real_part_path[PATH_MAX], module_name_buf[256];
    if (find_real_partition_dir(ctx, part_name, real_part_path, module_name_buf,
                                sizeof(module_name_buf)) != 0) {
        LOGD("no real directory found for %s, keeping symlink", part_name);
        return 0;
    }

    LOGI("symlink compatibility: system/%s -> %s, real dir in module '%s'", part_name, link_target,
         module_name_buf);

    Node *new_part = node_new(part_name, NFT_DIRECTORY);
    if (!new_part) {
        LOGE("failed to create node for %s", part_name);
        return -1;
    }

    bool part_has_any = false;
    if (node_scan_dir(ctx, new_part, real_part_path, module_name_buf, &part_has_any) != 0) {
        LOGE("failed to collect %s from %s", part_name, real_part_path);
        node_free(new_part);
        return -1;
    }

    if (!part_has_any) {
        LOGD("no content in %s, keeping symlink", part_name);
        node_free(new_part);
        return 0;
    }

    Node *removed = node_child_detach(system, part_name);
    if (removed) {
        node_free(removed);
        LOGD("removed symlink node: system/%s", part_name);
    }

    new_part->module_name = strdup(module_name_buf);

    if (node_child_append(system, new_part) != 0) {
        LOGE("failed to add directory node for %s", part_name);
        node_free(new_part);
        return -1;
    }

    LOGI("replaced symlink with directory node: %s (from module '%s')", part_name, module_name_buf);

    return 0;
}

static int symlink_resolve_all_partition_links(MagicMount *ctx, Node *system) {
    if (!system)
        return -1;

    const char *builtin_parts[] = {"vendor", "system_ext", "product", "odm"};

    for (size_t i = 0; i < sizeof(builtin_parts) / sizeof(builtin_parts[0]); ++i) {
        if (symlink_resolve_partition(ctx, system, builtin_parts[i]) != 0) {
            LOGE("failed to handle symlink compatibility for %s", builtin_parts[i]);
        }
    }

    for (int i = 0; i < ctx->extra_parts_count; ++i) {
        if (symlink_resolve_partition(ctx, system, ctx->extra_parts[i]) != 0) {
            LOGE("failed to handle symlink compatibility for extra part %s", ctx->extra_parts[i]);
        }
    }

    return 0;
}

/* --- Extra partition collect --- */

static int partition_scan_from_modules(MagicMount *ctx, const char *part_name, Node *parent_node) {
    if (!part_name || !parent_node) {
        LOGE("partition_scan_from_modules: invalid args part=%s node=%p",
             part_name ? part_name : "(null)", (void *)parent_node);
        return -1;
    }

    LOGD("partition_scan_from_modules: part=%s", part_name);

    DIR *mod_dir = opendir(ctx->module_dir);
    if (!mod_dir) {
        LOGE("opendir %s: %s", ctx->module_dir, strerror(errno));
        return -1;
    }

    struct dirent *mod_de;
    bool has_any = false;

    while ((mod_de = readdir(mod_dir))) {
        if (!strcmp(mod_de->d_name, ".") || !strcmp(mod_de->d_name, ".."))
            continue;

        char mod_path[PATH_MAX], part_path[PATH_MAX];

        if (path_join(ctx->module_dir, mod_de->d_name, mod_path, sizeof(mod_path)) != 0) {
            LOGE("partition_scan_from_modules: path_join failed for module=%s", mod_de->d_name);
            continue;
        }

        struct stat mod_st;
        if (stat(mod_path, &mod_st) < 0 || !S_ISDIR(mod_st.st_mode)) {
            LOGD("partition_scan_from_modules: skip non-dir module=%s", mod_path);
            continue;
        }

        if (module_is_disabled(mod_path)) {
            LOGD("partition_scan_from_modules: module %s disabled, skip", mod_path);
            continue;
        }

        if (path_join(mod_path, part_name, part_path, sizeof(part_path)) != 0) {
            LOGE("partition_scan_from_modules: path_join failed for part=%s in module=%s",
                 part_name, mod_path);
            continue;
        }

        if (!path_is_dir(part_path)) {
            LOGD("partition_scan_from_modules: module %s has no dir %s", mod_path, part_path);
            continue;
        }

        LOGD("partition_scan_from_modules: collecting part=%s from module=%s", part_name,
             mod_de->d_name);

        bool sub = false;
        if (node_scan_dir(ctx, parent_node, part_path, mod_de->d_name, &sub) != 0) {
            LOGE("partition_scan_from_modules: node_scan_dir failed for module=%s part=%s",
                 mod_de->d_name, part_name);
            closedir(mod_dir);
            return -1;
        }

        if (sub) {
            LOGD("partition_scan_from_modules: module=%s contributed content to part=%s",
                 mod_de->d_name, part_name);
            has_any = true;
        } else {
            LOGD("partition_scan_from_modules: module=%s had no effective content for part=%s",
                 mod_de->d_name, part_name);
        }
    }

    closedir(mod_dir);
    LOGD("partition_scan_from_modules: result for part=%s has_any=%d", part_name, has_any);
    return has_any ? 0 : 1;
}

/* --- Helper for partition promotion --- */

static int partition_promote_to_root(Node *root, Node *system, const char *part_name,
                                     bool need_symlink) {
    char rp[PATH_MAX], sp[PATH_MAX];

    LOGD("partition_promote_to_root: part=%s need_symlink=%d", part_name, need_symlink);

    if (path_join("/", part_name, rp, sizeof(rp)) != 0 ||
        path_join("/system", part_name, sp, sizeof(sp)) != 0)
        return -1;

    if (!path_is_dir(rp)) {
        LOGD("partition_promote_to_root: skip %s (real path %s not a dir)", part_name, rp);
        return 0;
    }

    if (need_symlink && !path_is_symlink(sp)) {
        LOGD("partition_promote_to_root: skip %s (no symlink at %s)", part_name, sp);
        return 0;
    }

    Node *child = node_child_detach(system, part_name);
    if (!child) {
        LOGD("partition_promote_to_root: system node has no child '%s' to promote", part_name);
        return 0;
    }

    LOGD("partition_promote_to_root: promoting '%s' from /system to /", part_name);

    if (node_child_append(root, child) != 0) {
        LOGE("partition_promote_to_root: failed to attach '%s' to root", part_name);
        node_free(child);
        return -1;
    }

    return 0;
}

/* --- Root collection --- */

Node *build_mount_tree(MagicMount *ctx) {
    if (!ctx) {
        LOGE("build_mount_tree: ctx is NULL");
        return NULL;
    }

    const char *mdir = ctx->module_dir ? ctx->module_dir : DEFAULT_MODULE_DIR;

    LOGI("build_mount_tree: module_dir=%s", mdir);

    Node *root = node_new("", NFT_DIRECTORY);
    Node *system = node_new("system", NFT_DIRECTORY);

    if (!root || !system) {
        LOGE("build_mount_tree: failed to allocate root/system nodes");
        node_free(root);
        node_free(system);
        return NULL;
    }

    DIR *d = opendir(mdir);
    if (!d) {
        LOGE("opendir %s: %s", mdir, strerror(errno));
        node_free(root);
        node_free(system);
        return NULL;
    }

    struct dirent *de;
    bool has_any = false;

    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;

        char mod[PATH_MAX], mod_sys[PATH_MAX];

        if (path_join(mdir, de->d_name, mod, sizeof(mod)) != 0) {
            LOGE("build_mount_tree: path_join failed for module=%s", de->d_name);
            closedir(d);
            node_free(root);
            node_free(system);
            return NULL;
        }

        struct stat st;
        if (stat(mod, &st) < 0 || !S_ISDIR(st.st_mode)) {
            LOGD("build_mount_tree: skip non-dir entry %s", mod);
            continue;
        }

        if (module_is_disabled(mod)) {
            LOGI("build_mount_tree: module %s is disabled", mod);
            continue;
        }

        if (path_join(mod, "system", mod_sys, sizeof(mod_sys)) != 0) {
            LOGE("build_mount_tree: path_join failed for module=%s system dir", mod);
            closedir(d);
            node_free(root);
            node_free(system);
            return NULL;
        }

        if (!path_is_dir(mod_sys)) {
            LOGD("build_mount_tree: module %s has no system dir (%s), skip", mod, mod_sys);
            continue;
        }

        LOGI("build_mount_tree: collecting module %s", de->d_name);
        ctx->stats.modules_total++;

        bool sub = false;
        if (node_scan_dir(ctx, system, mod_sys, de->d_name, &sub) != 0) {
            LOGE("build_mount_tree: node_scan_dir failed for module=%s", de->d_name);
            closedir(d);
            node_free(root);
            node_free(system);
            return NULL;
        }
        if (sub) {
            LOGD("build_mount_tree: module %s contributed content", de->d_name);
            has_any = true;
        } else {
            LOGD("build_mount_tree: module %s had no effective content", de->d_name);
        }
    }

    closedir(d);

    if (!has_any) {
        LOGW("build_mount_tree: no module contributed any content, abort");
        node_free(root);
        node_free(system);
        return NULL;
    }

    ctx->stats.nodes_total += 2;

    if (symlink_resolve_all_partition_links(ctx, system) != 0) {
        LOGW("symlink compatibility handling encountered errors (continuing anyway)");
    }

    // Promote builtin partitions to root
    struct {
        const char *name;
        bool need_symlink;
    } builtin_parts[] = {
        {"vendor", true},
        {"system_ext", true},
        {"product", true},
        {"odm", false},
    };

    for (size_t i = 0; i < sizeof(builtin_parts) / sizeof(builtin_parts[0]); ++i) {
        const char *part = builtin_parts[i].name;

        LOGD("build_mount_tree: trying to promote builtin partition '%s' to /", part);

        if (partition_promote_to_root(root, system, part, builtin_parts[i].need_symlink) != 0) {
            LOGE("build_mount_tree: partition_promote_to_root failed for builtin partition '%s'",
                 part);
            node_free(root);
            node_free(system);
            return NULL;
        } else {
            LOGD("build_mount_tree: partition_promote_to_root finished for builtin partition '%s'",
                 part);
        }
    }

    // Handle extra partitions
    for (int i = 0; i < ctx->extra_parts_count; ++i) {
        const char *name = ctx->extra_parts[i];
        char rp[PATH_MAX];

        LOGD("build_mount_tree: handling extra partition '%s' (index=%d)", name, i);

        if (path_join("/", name, rp, sizeof(rp)) != 0) {
            LOGE("build_mount_tree: path_join failed for extra partition '%s'", name);
            continue;
        }

        if (!path_is_dir(rp)) {
            LOGD("build_mount_tree: extra partition '%s' skipped, real path '%s' is not a dir",
                 name, rp);
            continue;
        }

        LOGD("build_mount_tree: extra partition '%s' has real dir '%s', creating node", name, rp);

        Node *child = node_new(name, NFT_DIRECTORY);
        if (!child) {
            LOGE("build_mount_tree: failed to allocate node for extra partition '%s'", name);
            node_free(root);
            node_free(system);
            return NULL;
        }

        LOGD("build_mount_tree: collecting extra partition '%s' from modules", name);

        int ret = partition_scan_from_modules(ctx, name, child);
        if (ret == 0) {
            LOGI("build_mount_tree: collected extra partition '%s' from module root", name);
            if (node_child_append(root, child) != 0) {
                LOGE("build_mount_tree: failed to attach extra partition '%s' node to root", name);
                node_free(child);
                node_free(root);
                node_free(system);
                return NULL;
            }
            LOGD("build_mount_tree: extra partition '%s' attached to root", name);
        } else if (ret == 1) {
            LOGD("build_mount_tree: no content found for extra partition '%s', dropping node",
                 name);
            node_free(child);
        } else {
            LOGE("build_mount_tree: partition_scan_from_modules failed for extra partition '%s' "
                 "(ret=%d)",
                 name, ret);
            node_free(child);
            node_free(root);
            node_free(system);
            return NULL;
        }
    }

    LOGD("build_mount_tree: attaching /system node to root");
    if (node_child_append(root, system) != 0) {
        LOGE("build_mount_tree: failed to attach /system node to root");
        node_free(root);
        node_free(system);
        return NULL;
    }

    LOGI("build_mount_tree: root tree successfully built");
    return root;
}

void module_tree_cleanup(MagicMount *ctx) {
    if (!ctx)
        return;

    str_array_free(&ctx->failed_modules, &ctx->failed_modules_count);
    str_array_free(&ctx->extra_parts, &ctx->extra_parts_count);
}
