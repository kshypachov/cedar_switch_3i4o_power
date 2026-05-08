//
// Created by Kirill Shypachov on 18.09.2025.
//

#ifndef CEDAR_SWITCH_3IN3OUT_POWER_FILESYSTEM_DELETE_TREE_H
#define CEDAR_SWITCH_3IN3OUT_POWER_FILESYSTEM_DELETE_TREE_H
#include <stddef.h>

int fs_delete_tree(const char *path);
int fs_calc_dir_size(const char *path, size_t *total_size);

#endif //CEDAR_SWITCH_3IN3OUT_POWER_FILESYSTEM_DELETE_TREE_H