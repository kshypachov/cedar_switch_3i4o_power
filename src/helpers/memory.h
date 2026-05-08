//
// Created by Kirill Shypachov on 03.04.2026.
//

#ifndef CEDAR_SWITCH_3IN3OUT_POWER_MEMORY_H
#define CEDAR_SWITCH_3IN3OUT_POWER_MEMORY_H
#include <stddef.h>
void init_memory_helpers(void);
void * allocate_external_memory( size_t size);
void free_external_memory( void *ptr);

#endif //CEDAR_SWITCH_3IN3OUT_POWER_MEMORY_H
