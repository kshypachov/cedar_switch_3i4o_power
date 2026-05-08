//
// Created by Kirill Shypachov on 18.09.2025.
//

#include "filesystem_helpers.h"
#include <zephyr/kernel.h>
#include <stdio.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>

#ifndef PATH_MAX
#define PATH_MAX 256
#endif

LOG_MODULE_REGISTER(fs_del_tree);

static int join_path(char *out, size_t out_sz, const char *base, const char *name)
{
    int n = snprintf(out, out_sz, "%s/%s", base, name);
    if (n < 0 || (size_t)n >= out_sz) {
        return -ENAMETOOLONG;
    }
    return 0;
}

int fs_delete_tree(const char *path)
{
    int rc;
    struct fs_dir_t dir;
    struct fs_dirent ent;

    if (!path || !*path) {
        return -EINVAL;
    }
    fs_dir_t_init(&dir);

    /* Попробуем открыть как каталог. Если это файл — просто удалить. */
    rc = fs_opendir(&dir, path);
    if (rc < 0) {
        /* Не каталог? — пробуем удалить как файл */
        rc = fs_unlink(path);
        return rc;
    }

    /* Это каталог: обходим содержимое */
    for (;;) {
        rc = fs_readdir(&dir, &ent);
        if (rc < 0) {
            fs_closedir(&dir);
            return rc;
        }
        if (ent.name[0] == 0) {
            /* конец каталога */
            break;
        }

        /* Zephyr обычно не возвращает "." и "..", но на всякий случай: */
        if ((strcmp(ent.name, ".") == 0) || (strcmp(ent.name, "..") == 0)) {
            continue;
        }

        char child[PATH_MAX] = {0};
        rc = join_path(child, sizeof(child), path, ent.name);
        if (rc < 0) {
            fs_closedir(&dir);
            return rc;
        }

        if (ent.type == FS_DIR_ENTRY_DIR) {
            /* рекурсивно удалить подкаталог, затем rmdir */
            fs_closedir(&dir);
            rc = fs_delete_tree(child);

            fs_dir_t_init(&dir);
            fs_opendir(&dir, path);

            if (rc < 0) {
                fs_closedir(&dir);
                return rc;
            }
            rc = fs_unlink(path);
            if (rc < 0) {
                fs_closedir(&dir);
                return rc;
            }
        } else if (ent.type == FS_DIR_ENTRY_FILE) {
            rc = fs_unlink(child);
            if (rc < 0) {
                fs_closedir(&dir);
                return rc;
            }
        } else {
            /* На всякий случай: неизвестный тип — пробуем удалить как файл */
            rc = fs_unlink(child);
            if (rc < 0) {
                fs_closedir(&dir);
                return rc;
            }
        }
    }

    fs_closedir(&dir);

    /* После очистки содержимого — удалить сам каталог (вызовите отдельно при необходимости) */
    rc = fs_unlink(path);
    return rc;
}

int fs_calc_dir_size(const char *path, size_t *total_size) {
    struct fs_dir_t dir;
    struct fs_dirent entry;
    int rc;

    fs_dir_t_init(&dir);

    rc = fs_opendir(&dir, path);
    if (rc < 0) {
        LOG_ERR("fs_opendir(%s) failed (%d)", path, rc);
        return rc;
    }

    while (true) {
        rc = fs_readdir(&dir, &entry);
        if (rc < 0) {
            LOG_ERR("fs_readdir failed (%d)", rc);
            break;
        }
        if (entry.name[0] == 0) {  // конец списка
            break;
        }

        if (entry.type == FS_DIR_ENTRY_FILE) {
            *total_size += entry.size;
        } else if (entry.type == FS_DIR_ENTRY_DIR) {
            char subpath[260];
            snprintf(subpath, sizeof(subpath), "%s/%s", path, entry.name);
            fs_calc_dir_size(subpath, total_size); // рекурсивный вызов
        }
    }

    fs_closedir(&dir);
    return 0;
}