//
// Created by Kirill Shypachov on 03.04.2026.
//

#include "memory.h"
#include "zephyr/kernel.h"

#define EXT_RAM_ADDR 0x70000000
#define EXT_RAM_SIZE 0x80000

static struct k_heap ext_heap;

void init_memory_helpers(void) {
    k_heap_init(&ext_heap, (void *)EXT_RAM_ADDR, EXT_RAM_SIZE);
}

void * allocate_external_memory( size_t size) {
    return k_heap_alloc(&ext_heap, size, K_NO_WAIT);
}

void free_external_memory( void *ptr) {
    k_heap_free(&ext_heap, ptr);
}
