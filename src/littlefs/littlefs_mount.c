//
// Created by Kirill Shypachov on 14.09.2025.
//

#include "littlefs_mount.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/storage/flash_map.h>

/*
 * ВНИМАНИЕ: storage_zms (бывший storage_lfs) теперь хранит настройки в ZMS
 * (zephyr,settings-partition). fs_service_init() на нём сделает fs_mkfs и
 * уничтожит все настройки, включая fabrics Matter. Вызов в main.c закомментирован;
 * /lfs монтируется через fstab на storage_partition.
 */
#define LFS_PARTITION_ID FIXED_PARTITION_ID(storage_zms_partition)

LOG_MODULE_REGISTER(fs_srv, LOG_LEVEL_INF);
FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(lfs_cfg);

static struct fs_mount_t lfs_mnt = {
    .type        = FS_LITTLEFS,
    .fs_data     = &lfs_cfg,
    .storage_dev = (void *)LFS_PARTITION_ID,
    .mnt_point   = "/lfs",
};

static int mount_or_mkfs(struct fs_mount_t *mnt)
{
    int rc = fs_mount(mnt);
    if (rc == 0) return 0;

#ifdef CONFIG_FILE_SYSTEM_MKFS
    rc = fs_mkfs(FS_LITTLEFS, (uintptr_t)LFS_PARTITION_ID, NULL, 0); /* 4-арг API */
    if (rc) return rc;
    return fs_mount(mnt);
#else
    return rc;
#endif
}

int fs_service_init(void)
{
    int rc = mount_or_mkfs(&lfs_mnt);
    if (rc) {
        LOG_ERR("FS mount failed: %d", rc);
        //fs_evt_publish_not_ready(rc);
        return rc;
    }
    LOG_INF("FS mounted at %s", lfs_mnt.mnt_point);
    //fs_evt_publish_ready();
    return 0;
}