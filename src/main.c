#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/net/ethernet_mgmt.h>
#include <zephyr/logging/log.h>
//#include <zephyr/version.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/net/net_config.h>
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_backend_net.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/cache.h>

#include <job_manager/job_manager.h>
#include <settings_registry/settings_registry.h>

#include "helpers/memory.h"
#include "services/coprocessor/coprocessor_service.h"
#include "services/network/network_service.h"
#include "services/system/system_service.h"
#include "web/api/v1/web_api_v1.h"
#include "web/web_server.h"
#include "mqtt/ha_mqtt.h"
#include "io/io.h"
#include "energy_monitoring/energy_monitoring.h"
#include "littlefs/littlefs_mount.h"
#include "plugin_wifi/wifi.h"
#include "matter/matter_init.h"
#include "matter/matter_service_chip.h"

#include "test_functions.h"
#include  "diagnostic/diag_report.h"
#include "lib-init/net_init.h"

LOG_MODULE_REGISTER(main_app);

/* 1000 msec = 1 sec */
#define SLEEP_TIME_MS   1000

/* The devicetree node identifier for the "led0" alias. */
// #define LED0_NODE DT_ALIAS(led0)
//
// #define KEY1_NODE DT_NODELABEL(key1)
// static const struct gpio_dt_spec key1 = GPIO_DT_SPEC_GET(KEY1_NODE, gpios);
// static struct gpio_callback key1_cb;

//
// #define FLASH_NODE DT_NODELABEL(spi_flash)
//
// #if !DT_NODE_HAS_STATUS(FLASH_NODE, okay)
// #error "DT node 'spi_flash' not found or not okay"
// #endif
//
// #define STM32_UID_BASE  0x1FFF7A10U

// #define STORAGE_PARTITION	storage_partition
// #define STORAGE_PARTITION_ID	FIXED_PARTITION_ID(STORAGE_PARTITION)

/* Получаем numeric ID разделов из DTS по алиасам узлов fixed-partitions */
/* storage_zms: хранилище настроек ZMS (бывший storage_lfs); стирание уничтожает все настройки */
#define ZMS_PART_ID  FIXED_PARTITION_ID(storage_zms_partition)
//#define NVS_PART_ID  FIXED_PARTITION_ID(storage_partition)

//static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);




static void ipv4_addr_add_handler(struct net_mgmt_event_callback *cb,
								  uint64_t mgmt_event, struct net_if *iface)
{
	//const struct log_backend *backend = log_backend_net_get();

	//if (!log_backend_is_active(backend)) {

		/* Specifying an address by calling this function will
		 * override the value given to LOG_BACKEND_NET_SERVER.
		   It can also be called at any other time after the backend
		   is started. The net context will be released and
		   restarted with the newly specified address.
		 */
		//log_backend_net_set_addr("192.168.88.182:514");
		//log_backend_init(backend);
		//log_backend_enable(backend, backend->cb->ctx, LOG_LEVEL_DBG);
		//log_backend_net_start();
	//}


	// app_mqtt_ha_client_init();

	if (mgmt_event != NET_EVENT_IPV4_ADDR_ADD) {
		return;
	}


	// char addr_str[NET_IPV4_ADDR_LEN];
	// struct net_if_addr *ifaddr = net_if_ipv4_get_global_addr(iface, NET_ADDR_DHCP);
	// if (!ifaddr) {
	// 	ifaddr = net_if_ipv4_get_global_addr(iface, NET_ADDR_MANUAL);
	// }
	// if (ifaddr) {
	// 	net_addr_ntop(AF_INET, &ifaddr->address.in_addr, addr_str, sizeof(addr_str));
	// 	LOG_INF("IPv4 address acquired: %s", addr_str);
	// } else {
	// 	LOG_INF("IPv4 address acquired (type unknown)");
	// }
}

/* Универсальная функция стирания раздела целиком */
// static int erase_partition_by_id(uint8_t part_id)
// {
// 	const struct flash_area *fa;
// 	int rc = flash_area_open(part_id, &fa);
// 	if (rc) {
// 		LOG_ERR("flash_area_open(%u) failed: %d", part_id, rc);
// 		return rc;
// 	}
//
// 	LOG_INF("Erasing partition id=%u, offset=0x%lx, size=0x%lx",
// 			(unsigned)part_id, (unsigned long)fa->fa_off, (unsigned long)fa->fa_size);
//
// 	/* Важно: перед стиранием убедитесь, что ФС на этом разделе размонтирована,
// 	   и никакие задачи его не используют. */
//
// 	rc = flash_area_erase(fa, 0, fa->fa_size);  /* стираем ВСЮ область */
// 	flash_area_close(fa);
//
// 	if (rc) {
// 		LOG_ERR("flash_area_erase failed: %d", rc);
// 		return rc;
// 	}
//
// 	LOG_INF("Erase OK");
// 	return 0;
// }

// static void key1_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins) {
// 	LOG_INF("Key1 pressed");
// 	sys_reboot(SYS_REBOOT_COLD);
//
// }

// static void erase_flash(void) {
// 	(void)erase_partition_by_id(ZMS_PART_ID);
// 	(void)erase_partition_by_id(NVS_PART_ID);
// 	sys_reboot(SYS_REBOOT_COLD);
// }

//#include <stm32u5xx_hal_icache.h>

/*
 * PSRAM (OCTOSPI2, окно 0x70000000) поднимает MCUboot (sysbuild/mcuboot.conf,
 * драйвер qspi-psram с chip-variant = "AUTO") и оставляет в memory-mapped
 * режиме. Здесь инициализируем внешнюю кучу helpers/memory.c поверх этого окна
 * и делаем короткий write/read тест, чтобы в логе было видно, что PSRAM
 * доступна приложению.
 */
#define PSRAM_SELFTEST_WORDS 1024U

static void psram_selftest(void)
{
	uint32_t *buf = NULL;
	uint32_t bad = 0;

	init_memory_helpers();

	buf = allocate_external_memory(PSRAM_SELFTEST_WORDS * sizeof(uint32_t));
	if (buf == NULL) {
		LOG_ERR("PSRAM: allocation from external heap failed");
		return;
	}

	for (uint32_t i = 0; i < PSRAM_SELFTEST_WORDS; i++) {
		buf[i] = 0xA5000000U ^ (i * 0x01010101U);
	}
	/* Дочитать до самой PSRAM, а не до строки D-cache */
	sys_cache_data_flush_and_invd_range(buf, PSRAM_SELFTEST_WORDS * sizeof(uint32_t));

	for (uint32_t i = 0; i < PSRAM_SELFTEST_WORDS; i++) {
		if (buf[i] != (0xA5000000U ^ (i * 0x01010101U))) {
			bad++;
		}
	}

	if (bad == 0) {
		LOG_INF("PSRAM @%p: write/read %u words OK", (void *)buf,
			PSRAM_SELFTEST_WORDS);
	} else {
		LOG_ERR("PSRAM @%p: %u of %u words mismatch", (void *)buf, bad,
			PSRAM_SELFTEST_WORDS);
	}

	free_external_memory(buf);
}

/* The coprocessor update must be requested over Ethernet (plan section 8). */
static bool request_over_ethernet(const struct web_auth_peer *local)
{
	return local != NULL && coprocessor_service_request_over_ethernet(local->family, local->addr);
}

int main(void)
{
	LOG_INF("Start main app (build: %s %s) version 8", __DATE__, __TIME__);

	psram_selftest();
	/* Reset cause and the running MCUboot image (STM32 update, reports/stm32-update). */
	system_service_start();

	/*
	 * Реестр настроек: /lfs уже смонтирован через fstab (automount), поэтому
	 * store /lfs/settings доступен. Вызываем до первых потребителей настроек
	 * (io_init и далее), чтобы RAM-зеркала ключей "reg/" были загружены.
	 */
	int err = setting_registry_init();
	if (err != 0) {
		LOG_ERR("settings registry init failed: %d", err);
	}

	io_init();
	/* Before the network: the first IPv6 address starts the Matter stack. */
	matter_service_chip_init();
	/* Before the network too: network-manager tracks its work as jobs. */
	job_manager_init();
	/* The C6's UART and EN/BOOT: before the network service, whose apply and
	 * scan claim against the UART's owner. */
	coprocessor_service_start();
	static const struct web_api_v1_coprocessor coprocessor_hooks = {
		.firmware_version = coprocessor_service_firmware_version,
		.rx_seen = coprocessor_service_rx_seen,
		.request_over_ethernet = request_over_ethernet,
	};
	web_api_v1_set_coprocessor(&coprocessor_hooks);
	/* The upload bindings open only over a store that opened (/lfs/firmware). */
	static const struct web_api_v1_firmware firmware_hooks = {
		.now_ms = NULL,
	};
	if (coprocessor_service_firmware_ready()) {
		web_api_v1_set_firmware(&firmware_hooks);
	}
	/* The STM32 update bindings open over the slot store and the updater that
	 * system_service_start() opened (reports/stm32-update). */
	static struct web_api_v1_system system_hooks = {
		.now_ms = NULL,
	};
	if (system_service_update_ready()) {
		system_hooks.upload_max_bytes = system_service_upload_max_bytes();
		web_api_v1_set_system(&system_hooks);
	}
	/* The W5500 with its EEPROM MAC; addressing is the network service's. */
	ethernet_interfaces_init();
	network_service_start();
	static const struct web_api_v1_network network_hooks = {
		.kick = network_service_kick,
		.wifi_security_modes = network_service_wifi_security_modes,
	};
	web_api_v1_set_network(&network_hooks);
	app_web_init();

	while (1)
	{
		k_msleep(2100);
	}
	//int usb_ret = usb_enable(NULL);
	// if (!gpio_is_ready_dt(&led)) {
	// 	LOG_ERR("Error: LED device is not ready\n");
	// 	return 0;
	// }

	// ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
	// if (ret < 0) {
	// 	LOG_ERR("Error %d: failed to configure LED\n", ret);
	// 	return 0;
	// }

	// const struct device *flash = DEVICE_DT_GET(FLASH_NODE);
 //
	// if (!device_is_ready(flash)) {
	// 	LOG_ERR("SPI_FLASH not ready (probe failed or disabled)");
	// 	return 0;
	// }
 //
	// const struct flash_parameters *p = flash_get_parameters(flash);
	// if (p) {
	// 	printk("erase_value=0x%02x, write_block=%u",
	// 			p->erase_value, p->write_block_size);
	// }
 //
	// const struct device *flash_dev = DEVICE_DT_GET(DT_NODELABEL(spi_flash));
	// if (!device_is_ready(flash_dev)) {
	// 	printk("Flash device not ready\n");
	// 	return 0;
	// }
 //
 //    /* Получаем устройство энтропии, выбранное devicetree (chosen: zephyr,entropy) */
 //    const struct device *entropy = DEVICE_DT_GET(DT_CHOSEN(zephyr_entropy));
 //    if (!device_is_ready(entropy)) {
 //        LOG_ERR("Entropy device is not ready! Check your Kconfig/DTS.");
 //        return 0;
 //    }
 //
	// const struct flash_parameters *params = flash_get_parameters(flash_dev);
	// if (params) {
	// 	printk("Erase value: 0x%02x\n", params->erase_value);
	// 	printk("Write block size: %u\n", params->write_block_size);
	// }

	// if (!device_is_ready(key1.port)) {
	// 	LOG_ERR("Key1 GPIO not ready");
	// 	return 0;
	// }

	// ret = gpio_pin_configure_dt(&key1, GPIO_INPUT);
	// if (ret < 0) {
	// 	LOG_ERR("Failed to configure KEY1");
	// 	return 0;
	// }
	//
	// ret = gpio_pin_interrupt_configure_dt(&key1, GPIO_INT_EDGE_TO_INACTIVE);
	// if (ret < 0) {
	// 	LOG_ERR("Failed to set interrupt on KEY1");
	// 	return 0;
	// }
	//
	// gpio_init_callback(&key1_cb, key1_pressed, BIT(key1.pin));
	// gpio_add_callback(key1.port, &key1_cb);
	//
	// int key_state = gpio_pin_get_dt(&key1);
	//LOG_INF("KEY1 state on boot: %d", key_state);

	// if (!key_state) {
	// 	LOG_INF("KEY1 pressed — jumping to erase_flash()");
	// 	erase_flash();
	// } else {
	// 	LOG_INF("Normal boot — KEY1 not pressed");
	// }


	/* Подписка на событие получения IPv4 адреса */


	/* Включаем интерфейс по умолчанию (w5500) */
	// struct net_if *iface = net_if_get_default();
	//
	// (void)settings_subsys_init();
	// (void)settings_load();
	// (void)fs_service_init();
	// io_init();
	// energy_monitoring_init();
	//
	//
	// if (iface) {
	// 	struct ethernet_req_params mac_params = {
	// 		.mac_address.addr = { 0x02, 0x00, 0x00, 0x12, 0x34, 0x56 }
	// 	};
	//
	// 	net_if_down(iface);
	// 	ret = net_mgmt(NET_REQUEST_ETHERNET_SET_MAC_ADDRESS, iface, &mac_params, sizeof(struct ethernet_req_params));
	// 	net_if_up(iface); /* DHCP стартует автоматически при CONFIG_NET_DHCPV4=y */
	// 	(void)net_dhcpv4_start(iface);    // ЯВНО запустить DHCP
	// 	LOG_INF("Network interface up");
	// }
	//
	//
	//
	// int answ = 0;
	// answ = fs_mkdir("/lfs/test1");
	// answ = fs_mkdir("/lfs/test1/test2");
	// answ = fs_mkdir("/lfs/test1/test2/test3");
	// answ = fs_mkdir("/lfs/log");
	// //fs_delete_tree("/lfs/test1");
	//
	// fs_mkdir("/lfs/www");
	//
	// create_index_html();
	//
	// LOG_INF("answ = %d", answ);
	//
	// while (1) {
	// 	//ret = gpio_pin_toggle_dt(&led);
	// 	if (ret < 0) {
	// 		printk("Error %d: failed to toggle LED\n", ret);
	// 		return 0;
	// 	}
	//
	// 	k_msleep(SLEEP_TIME_MS);
	// }
}
