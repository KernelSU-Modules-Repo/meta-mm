#include "ksu.h"
#include "utils.h"

#include <errno.h>
#include <stdatomic.h>
#include <sys/syscall.h>
#include <unistd.h>

long syscall(long number, ...);

static atomic_int g_driver_fd = -1;
static atomic_flag g_driver_fd_initialized = ATOMIC_FLAG_INIT;

static void ksu_grab_fd_once(void) {
    int fd = -1;

    syscall(SYS_reboot, KSU_INSTALL_MAGIC1, KSU_INSTALL_MAGIC2, 0, (void *)&fd);

    if (fd < 0) {
        LOGW("failed to grab KSU driver fd: %d", fd);
    } else {
        LOGD("grabbed KSU driver fd: %d", fd);
    }

    atomic_store(&g_driver_fd, fd);
}

static int ksu_grab_fd(void) {
    if (!atomic_flag_test_and_set(&g_driver_fd_initialized)) {
        ksu_grab_fd_once();
    }
    return atomic_load(&g_driver_fd);
}

int ksu_send_unmountable(const char *mntpoint) {
    struct ksu_add_try_umount_cmd cmd = {0};
    int fd = ksu_grab_fd();

    if (fd < 0)
        return -1;

    cmd.arg = (uint64_t)mntpoint;
    cmd.flags = 0x2;
    cmd.mode = 1;

    if (ioctl(fd, KSU_IOCTL_ADD_TRY_UMOUNT, &cmd) < 0) {
        LOGE("ioctl KSU_IOCTL_ADD_TRY_UMOUNT failed: %s", strerror(errno));
        return -1;
    }

    return 0;
}
