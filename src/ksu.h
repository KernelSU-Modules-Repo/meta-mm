#ifndef KSU_H
#define KSU_H

#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>

#define KSU_INSTALL_MAGIC1 0xDEADBEEF
#define KSU_INSTALL_MAGIC2 0xCAFEBABE

struct ksu_add_try_umount_cmd {
    uint64_t arg;   // char *, mountpoint
    uint32_t flags; // flags
    uint8_t mode;   // 0:wipe_list 1:add_to_list 2:delete_entry
};

#define KSU_IOCTL_ADD_TRY_UMOUNT _IOC(_IOC_WRITE, 'K', 18, 0)

int ksu_send_unmountable(const char *mntpoint);

#endif /* KSU_H */
