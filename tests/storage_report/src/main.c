/* P0 measurement: how much room does /lfs actually have for a staged ESP32
 * image? Relays are opened first so this image is safe to leave running. */
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/fs/fs.h>
#include <zephyr/storage/flash_map.h>

#define RELAY(node) GPIO_DT_SPEC_GET(DT_NODELABEL(node), gpios)
static const struct gpio_dt_spec relays[] = {
	RELAY(relay_ac1), RELAY(relay_ac2), RELAY(relay_ac3), RELAY(relay_ac4),
};

static void relays_off(void)
{
	for (int i = 0; i < ARRAY_SIZE(relays); i++) {
		gpio_pin_configure_dt(&relays[i], GPIO_OUTPUT_INACTIVE);
	}
	printk("relays: all four driven inactive\n");
}

static void part_info(const char *name, uint8_t id)
{
	const struct flash_area *fa;

	if (flash_area_open(id, &fa) == 0) {
		printk("  %-14s id=%u off=0x%08lx size=%lu KiB\n", name, id,
		       (unsigned long)fa->fa_off, (unsigned long)fa->fa_size / 1024);
		flash_area_close(fa);
	} else {
		printk("  %-14s id=%u  <open failed>\n", name, id);
	}
}

int main(void)
{
	struct fs_statvfs st;
	int rc;

	printk("\n=== storage report ===\n");
	relays_off();

	printk("flash partitions:\n");
#if FIXED_PARTITION_EXISTS(storage_partition)
	part_info("storage_nvs", FIXED_PARTITION_ID(storage_partition));
#endif
#if FIXED_PARTITION_EXISTS(storage_lfs_partition)
	part_info("storage_lfs", FIXED_PARTITION_ID(storage_lfs_partition));
#endif
#if FIXED_PARTITION_EXISTS(slot1_partition)
	part_info("image-1", FIXED_PARTITION_ID(slot1_partition));
#endif

	rc = fs_statvfs("/lfs", &st);
	if (rc != 0) {
		printk("/lfs statvfs failed: %d (not mounted?)\n", rc);
	} else {
		unsigned long total = (unsigned long)st.f_frsize * st.f_blocks;
		unsigned long freeb = (unsigned long)st.f_frsize * st.f_bfree;

		printk("/lfs: block=%lu blocks=%lu free_blocks=%lu\n",
		       (unsigned long)st.f_frsize, (unsigned long)st.f_blocks,
		       (unsigned long)st.f_bfree);
		printk("/lfs: total=%lu KiB  free=%lu KiB  used=%lu KiB\n",
		       total / 1024, freeb / 1024, (total - freeb) / 1024);
		printk("/lfs: fits a 1.75 MiB C6 image? %s\n",
		       freeb >= (1792UL * 1024) ? "YES" : "NO");
	}

	printk("=== done, idling ===\n");
	while (1) {
		k_sleep(K_FOREVER);
	}
	return 0;
}
