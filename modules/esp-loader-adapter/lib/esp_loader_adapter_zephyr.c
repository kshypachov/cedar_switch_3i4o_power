/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The library table over the chosen espressif,esp-loader device.
 *
 * The application marks the node `zephyr,deferred-init`: the library's device
 * init (esp_loader_init_serial() at POST_KERNEL) would install tty's receive
 * handler on USART3 with nobody reading, and an unflashed C6's ROM flood then
 * blocks that ISR forever (reports/p6, hw/logs/02). So the kernel never runs
 * it, device_is_ready() stays false and is not asked, and device_init() is
 * never called. Everything this file needs is static device data, valid
 * without init:
 *
 * - the loader: esp_loader_from_device();
 * - the connect arguments: esp_loader_connect_args_from_device();
 * - the port: zephyr_port.h has no accessor for it. zephyr_port.c's device
 *   data is `struct esp_loader_dev_data { zephyr_port_t interface; esp_loader_t
 *   loader; }` with `interface.port.ops` and `interface.config` set statically
 *   by ESP_LOADER_DEFINE, so the port is the first member of the device data.
 *   That layout is the library's private detail: z_port() checks it at run
 *   time against the public config accessor and refuses to initialise a port
 *   it cannot confirm.
 *
 * A session initialises the port right before connect (in `flashing`, the
 * console detached) and esp_loader_deinit() ends it, which turns the UART's
 * interrupts off and clears the loader; the next session initialises the same
 * port again.
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <esp_loader.h>
#include <zephyr_port.h>

#include <esp_loader_adapter/esp_loader_adapter.h>

BUILD_ASSERT(ESP32C6_CHIP == ESP_LOADER_ADAPTER_CHIP_ESP32C6);
BUILD_ASSERT(ESP_LOADER_ERROR_TIMEOUT == ESP_LOADER_ADAPTER_LIB_TIMEOUT);
BUILD_ASSERT(ESP_LOADER_ERROR_INVALID_MD5 == ESP_LOADER_ADAPTER_LIB_INVALID_MD5);
BUILD_ASSERT(ESP_LOADER_SUCCESS == 0);
BUILD_ASSERT(offsetof(zephyr_port_t, port) == 0);

static const struct device *const loader_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_esp_loader));

static esp_loader_flash_cfg_t flash_cfg;

static esp_loader_t *loader(void)
{
	return esp_loader_from_device(loader_dev);
}

/* The device's port, or NULL when the data does not have the layout described above. */
static esp_loader_port_t *z_port(void)
{
	zephyr_port_t *intf = (zephyr_port_t *)loader_dev->data;

	if (intf == NULL || intf->config != esp_loader_config_from_device(loader_dev) ||
	    intf->port.ops == NULL) {
		return NULL;
	}
	return &intf->port;
}

/*
 * Debug (reports/p6 hw, attempt 5): the port's read and write, timed. A FLASH_DATA
 * attempt has 1000 ms for the frame out and the answer back; one ran out with no
 * error from the ROM. These say whether the time went waiting for bytes in or for
 * bytes out. The library's own table is kept and called through.
 */
static const esp_loader_port_ops_t *port_ops;
static esp_loader_port_ops_t timed_ops;
static struct esp_loader_adapter_zephyr_io io;

static void io_note(uint32_t *count, int64_t *ticks, uint32_t *max_ms, int *rc_last,
		    uint32_t *fail_timeout_ms, uint32_t *fail_ms, int64_t t0, int rc,
		    uint32_t timeout)
{
	const int64_t dt = k_uptime_ticks() - t0;
	const uint32_t ms = (uint32_t)k_ticks_to_ms_floor64(dt);

	(*count)++;
	*ticks += dt;
	*max_ms = MAX(*max_ms, ms);
	if (rc != ESP_LOADER_SUCCESS) {
		*rc_last = rc;
		*fail_timeout_ms = timeout;
		*fail_ms = ms;
	}
}

static void rx_tail_push(uint16_t v)
{
	io.rx_tail[io.rx_tail_next] = v;
	io.rx_tail_next = (io.rx_tail_next + 1U) % ARRAY_SIZE(io.rx_tail);
	io.rx_tail_count = MIN(io.rx_tail_count + 1U, ARRAY_SIZE(io.rx_tail));
}

static esp_loader_error_t timed_read(esp_loader_port_t *port, uint8_t *data, uint16_t size,
				     uint32_t timeout)
{
	const int64_t t0 = k_uptime_ticks();
	const esp_loader_error_t rc = port_ops->read(port, data, size, timeout);
	const uint32_t ms = (uint32_t)k_ticks_to_ms_floor64(k_uptime_ticks() - t0);

	io_note(&io.reads, &io.read_ticks, &io.read_max_ms, &io.read_rc, &io.read_fail_timeout_ms,
		&io.read_fail_ms, t0, rc, timeout);
	if (ms > timeout && ms - timeout > io.overrun_max_ms) {
		io.overrun_max_ms = ms - timeout;
		io.overrun_at_ms = k_uptime_get();
	}
	if (rc == ESP_LOADER_SUCCESS) {
		io.bytes_since_write += size;
		for (uint16_t i = 0; i < size; i++) {
			rx_tail_push(data[i]);
		}
	} else {
		/* 0: no answer; part of a frame: a byte lost; more: frames the filter discarded. */
		io.read_fail_bytes = io.bytes_since_write;
	}
	return rc;
}

static esp_loader_error_t timed_write(esp_loader_port_t *port, const uint8_t *data, uint16_t size,
				      uint32_t timeout)
{
	const int64_t t0 = k_uptime_ticks();
	const esp_loader_error_t rc = port_ops->write(port, data, size, timeout);

	if (io.rx_tail_count == 0U ||
	    io.rx_tail[(io.rx_tail_next + ARRAY_SIZE(io.rx_tail) - 1U) % ARRAY_SIZE(io.rx_tail)] !=
		    0x100U) {
		rx_tail_push(0x100U);
	}
	io.bytes_since_write = 0;
	io_note(&io.writes, &io.write_ticks, &io.write_max_ms, &io.write_rc,
		&io.write_fail_timeout_ms, &io.write_fail_ms, t0, rc, timeout);
	return rc;
}

void esp_loader_adapter_zephyr_io_get(struct esp_loader_adapter_zephyr_io *out)
{
	*out = io;
}

void esp_loader_adapter_zephyr_io_reset(void)
{
	memset(&io, 0, sizeof(io));
}

static int z_port_init(void *ctx)
{
	esp_loader_port_t *port = z_port();

	ARG_UNUSED(ctx);
	if (port == NULL) {
		return ESP_LOADER_ERROR_FAIL;
	}
	if (port->ops != &timed_ops) {
		port_ops = port->ops;
		timed_ops = *port_ops;
		timed_ops.read = timed_read;
		timed_ops.write = timed_write;
		port->ops = &timed_ops;
	}
	return esp_loader_init_serial(loader(), port);
}

static void z_port_deinit(void *ctx)
{
	ARG_UNUSED(ctx);
	esp_loader_deinit(loader());
}

static int z_connect(void *ctx)
{
	ARG_UNUSED(ctx);
	/* esp_loader_connect() takes a mutable pointer; keep the DT values intact. */
	esp_loader_connect_args_t args = *esp_loader_connect_args_from_device(loader_dev);

	return esp_loader_connect(loader(), &args);
}

static int z_get_target(void *ctx)
{
	ARG_UNUSED(ctx);
	return (int)esp_loader_get_target(loader());
}

static int z_flash_start(void *ctx, uint32_t offset, uint32_t size, uint32_t block_size)
{
	ARG_UNUSED(ctx);
	memset(&flash_cfg, 0, sizeof(flash_cfg));
	flash_cfg.offset = offset;
	flash_cfg.image_size = size;
	flash_cfg.block_size = block_size;
	flash_cfg.skip_verify = false;
	return esp_loader_flash_start(loader(), &flash_cfg);
}

static int z_flash_write(void *ctx, const uint8_t *data, uint32_t len)
{
	ARG_UNUSED(ctx);
	return esp_loader_flash_write(loader(), &flash_cfg, data, len);
}

static int z_flash_finish(void *ctx)
{
	ARG_UNUSED(ctx);
	return esp_loader_flash_finish(loader(), &flash_cfg);
}

static void z_reset_target(void *ctx)
{
	ARG_UNUSED(ctx);
	esp_loader_reset_target(loader());
}

static int z_change_rate(void *ctx, uint32_t baud)
{
	ARG_UNUSED(ctx);
	return esp_loader_change_transmission_rate(loader(), baud);
}

void esp_loader_adapter_zephyr_fill(struct esp_loader_adapter_lib *lib)
{
	lib->port_init = z_port_init;
	lib->port_deinit = z_port_deinit;
	lib->connect = z_connect;
	lib->get_target = z_get_target;
	lib->flash_start = z_flash_start;
	lib->flash_write = z_flash_write;
	lib->flash_finish = z_flash_finish;
	lib->reset_target = z_reset_target;
	lib->change_rate = z_change_rate;
}
