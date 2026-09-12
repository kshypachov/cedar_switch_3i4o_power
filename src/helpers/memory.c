//
// Created by Kirill Shypachov on 03.04.2026.
//

#include "memory.h"

#include <string.h>

#include "zephyr/kernel.h"
#include "zephyr/init.h"
#include "zephyr/devicetree.h"
#include "zephyr/sys/util.h"

/* Окно PSRAM из DTS платы (memory@70000000, регион линкера "PSRAM") */
#define EXT_RAM_NODE DT_NODELABEL(ext_psram)
#define EXT_RAM_ADDR DT_REG_ADDR(EXT_RAM_NODE)
#define EXT_RAM_SIZE DT_REG_SIZE(EXT_RAM_NODE)

/* Символы из src/helpers/psram_sections.ld */
extern char __psram_ext_bss_start[];
extern char __psram_ext_bss_end[];
extern char __psram_used_end[];

static struct k_heap ext_heap;

/*
 * .bss библиотек из psram_sections.ld штатный z_bss_zero() не обнуляет.
 * PRE_KERNEL_1 с приоритетом 0: раньше объектов ядра и конструкторов C++.
 */
static int psram_ext_bss_zero(void) {
    memset(__psram_ext_bss_start, 0, __psram_ext_bss_end - __psram_ext_bss_start);
    return 0;
}

SYS_INIT(psram_ext_bss_zero, PRE_KERNEL_1, 0);

/* Внешняя куча — всё, что осталось в PSRAM после слинкованных туда секций */
void init_memory_helpers(void) {
    uintptr_t start = ROUND_UP((uintptr_t)__psram_used_end, 2 * sizeof(void *));
    size_t size = (size_t)(EXT_RAM_ADDR + EXT_RAM_SIZE - start);

    k_heap_init(&ext_heap, (void *)start, size);
}

void * allocate_external_memory( size_t size) {
    return k_heap_alloc(&ext_heap, size, K_NO_WAIT);
}

void free_external_memory( void *ptr) {
    k_heap_free(&ext_heap, ptr);
}
