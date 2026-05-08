//
// Created by Kirill Shypachov on 17.09.2025.
//

#include "test_functions.h"
#include <zephyr/kernel.h>
#include <stdio.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>


#include <zephyr/sys/printk.h>
#include <zephyr/drivers/clock_control/stm32_clock_control.h>
#include <errno.h>
#include <soc.h>
#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/stm32_clock_control.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/logging/log.h>
#include <zephyr/multi_heap/shared_multi_heap.h>

#ifndef PATH_MAX
#define PATH_MAX 256
#endif

LOG_MODULE_REGISTER(test_functions);

// static int join_path(char *out, size_t out_sz, const char *base, const char *name)
// {
//     int n = snprintf(out, out_sz, "%s/%s", base, name);
//     if (n < 0 || (size_t)n >= out_sz) {
//         return -ENAMETOOLONG;
//     }
//     return 0;
// }

// static int fs_delete_tree(const char *path)
// {
//     int rc;
//     struct fs_dir_t dir;
//     struct fs_dirent ent;
//
//     if (!path || !*path) {
//         return -EINVAL;
//     }
//     fs_dir_t_init(&dir);
//
//     /* Попробуем открыть как каталог. Если это файл — просто удалить. */
//     rc = fs_opendir(&dir, path);
//     if (rc < 0) {
//         /* Не каталог? — пробуем удалить как файл */
//         rc = fs_unlink(path);
//         return rc;
//     }
//
//     /* Это каталог: обходим содержимое */
//     for (;;) {
//         rc = fs_readdir(&dir, &ent);
//         if (rc < 0) {
//             fs_closedir(&dir);
//             return rc;
//         }
//         if (ent.name[0] == 0) {
//             /* конец каталога */
//             break;
//         }
//
//         /* Zephyr обычно не возвращает "." и "..", но на всякий случай: */
//         if ((strcmp(ent.name, ".") == 0) || (strcmp(ent.name, "..") == 0)) {
//             continue;
//         }
//
//         char child[PATH_MAX];
//         rc = join_path(child, sizeof(child), path, ent.name);
//         if (rc < 0) {
//             fs_closedir(&dir);
//             return rc;
//         }
//
//         if (ent.type == FS_DIR_ENTRY_DIR) {
//             /* рекурсивно удалить подкаталог, затем rmdir */
//             fs_closedir(&dir);
//             rc = fs_delete_tree(child);
//
//             fs_dir_t_init(&dir);
//             fs_opendir(&dir, path);
//
//             if (rc < 0) {
//                 fs_closedir(&dir);
//                 return rc;
//             }
//             rc = fs_unlink(path);
//             if (rc < 0) {
//                 fs_closedir(&dir);
//                 return rc;
//             }
//         } else if (ent.type == FS_DIR_ENTRY_FILE) {
//             rc = fs_unlink(child);
//             if (rc < 0) {
//                 fs_closedir(&dir);
//                 return rc;
//             }
//         } else {
//             /* На всякий случай: неизвестный тип — пробуем удалить как файл */
//             rc = fs_unlink(child);
//             if (rc < 0) {
//                 fs_closedir(&dir);
//                 return rc;
//             }
//         }
//     }
//
//     fs_closedir(&dir);
//
//     /* После очистки содержимого — удалить сам каталог (вызовите отдельно при необходимости) */
//     rc = fs_unlink(path);
//     return rc;
// }


int create_index_html(void)
{
    struct fs_file_t file;
    int ret;

    /* Подготовим дескриптор */
    fs_file_t_init(&file);

    /* Создаём/открываем файл на запись (с перезаписью) */
    ret = fs_open(&file, "/lfs/www/index.html", FS_O_CREATE | FS_O_RDWR);
    if (ret < 0) {
        LOG_ERR("fs_open failed (%d)", ret);
        return ret;
    }

    const char *html =
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head><title>Test Page</title></head>\n"
        "<body>\n"
        "  <h1>Hello from Zephyr!</h1>\n"
        "</body>\n"
        "</html>\n";

    /* Запишем данные */
    ret = fs_write(&file, html, strlen(html));
    if (ret < 0) {
        LOG_ERR("fs_write failed (%d)", ret);
        fs_close(&file);
        return ret;
    }

    LOG_INF("index.html created, %d bytes written", ret);

    fs_close(&file);
    return 0;
}
/* Read-ID (0x9F) for APS6404L/APSxxx PSRAM requires an address phase (24-bit),
 * per datasheet: Cmd=S, Addr=S, Wait=0, Data=S.
 *
 * So: send 0x9F + 3-byte address (usually 0x000000) + read 3 bytes.
 */

