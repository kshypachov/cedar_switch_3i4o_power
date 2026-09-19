//
// Created by Kirill Shypachov on 03.04.2026.
//

#include "memory.h"

#include <string.h>

#include "zephyr/kernel.h"
#include "zephyr/cache.h"
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

#if defined(CONFIG_DCACHE)
/*
 * DCACHE1 (владелец, 2026-09-19): кеширует данные только внешней памяти — PSRAM
 * (0x70000000) и прямые чтения флеши 0x90000000, — SRAM и периферию обходит; код через
 * ремап ICACHE (0x02…) идёт по C-AHB и его не касается. Zephyr на U5 включает только
 * ICACHE (soc/st/stm32/stm32u5x/soc.c), DCACHE1 до этого был выключен (DCACHE1_CR = 0x300).
 * Условия (reports/littlefs-speed): DMA в PSRAM нет — SPI1 работает через
 * modules/flash-sram-proxy только с SRAM, у остальной периферии DMA нет; драйвер записи
 * XIP-флеши инвалидирует DCACHE по записанному диапазону.
 */
static int dcache1_enable(void) {
    (void)sys_cache_data_invd_all();
    sys_cache_data_enable();
    return 0;
}

SYS_INIT(dcache1_enable, PRE_KERNEL_1, 1);
#endif

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
