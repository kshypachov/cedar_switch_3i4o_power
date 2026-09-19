/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * cedar,flash-sram-proxy: a flash device that forwards every operation to its
 * backend from one thread, with the data copied through one buffer; both the
 * thread's stack and the buffer are in SRAM.
 *
 * Why (2026-09-19, reports/littlefs-speed): SPI1 to the SPI NOR runs 10-25x faster
 * with DMA, but on STM32U5 with DCACHE the SPI driver takes DMA only to and from
 * SRAM, and DMA to the OCTOSPI PSRAM also timed out on 32 KiB reads. The firmware
 * keeps most data and thread stacks in PSRAM, and spi_nor puts its command bytes
 * on the caller's stack while LittleFS and ZMS read straight into caller buffers.
 * Going through this device, the SPI driver only ever sees the proxy thread's
 * stack and the bounce buffer.
 *
 * One request at a time (mutex); callers block until the proxy thread has done
 * it. Reads and writes longer than CONFIG_FLASH_SRAM_PROXY_BUF_SIZE are split;
 * a split write is not atomic, which flash writes are not anyway.
 */

#define DT_DRV_COMPAT cedar_flash_sram_proxy

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(flash_sram_proxy, CONFIG_FLASH_SRAM_PROXY_LOG_LEVEL);

/* flash_read_jedec_id() fills 3 bytes (SPI_NOR_MAX_ID_LEN in drivers/flash/spi_nor.h) */
#define JEDEC_ID_LEN 3

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1,
	     "one cedar,flash-sram-proxy instance is supported");

enum proxy_op {
	OP_READ,
	OP_WRITE,
	OP_ERASE,
	OP_SFDP,
	OP_JEDEC,
};

struct proxy_config {
	const struct device *backend;
};

struct proxy_data {
	struct k_mutex lock;
	struct k_sem start;
	struct k_sem done;
	enum proxy_op op;
	off_t off;
	size_t len;
	int rc;
	bool running;
};

static const struct proxy_config proxy_cfg = {
	.backend = DEVICE_DT_GET(DT_INST_PHANDLE(0, backend)),
};

static struct proxy_data proxy_data;
static uint8_t bounce[CONFIG_FLASH_SRAM_PROXY_BUF_SIZE] __aligned(4);
static K_THREAD_STACK_DEFINE(proxy_stack, CONFIG_FLASH_SRAM_PROXY_STACK_SIZE);
static struct k_thread proxy_thread;

/* In the proxy thread (or the caller before it runs): the backend call itself. */
static int backend_do(enum proxy_op op, off_t off, size_t len)
{
	const struct device *be = proxy_cfg.backend;

	switch (op) {
	case OP_READ:
		return flash_read(be, off, bounce, len);
	case OP_WRITE:
		return flash_write(be, off, bounce, len);
	case OP_ERASE:
		return flash_erase(be, off, len);
#if defined(CONFIG_FLASH_JESD216_API)
	case OP_SFDP:
		return flash_sfdp_read(be, off, bounce, len);
	case OP_JEDEC:
		return flash_read_jedec_id(be, bounce);
#endif
	default:
		return -ENOTSUP;
	}
}

static void proxy_main(void *p1, void *p2, void *p3)
{
	struct proxy_data *d = p1;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		k_sem_take(&d->start, K_FOREVER);
		d->rc = backend_do(d->op, d->off, d->len);
		k_sem_give(&d->done);
	}
}

/* With the lock held: run one piece in the proxy thread. */
static int run(enum proxy_op op, off_t off, size_t len)
{
	struct proxy_data *d = &proxy_data;

	if (!d->running || k_current_get() == &proxy_thread) {
		/* init before the thread exists runs on the main stack, in SRAM */
		return backend_do(op, off, len);
	}
	d->op = op;
	d->off = off;
	d->len = len;
	k_sem_give(&d->start);
	k_sem_take(&d->done, K_FOREVER);
	return d->rc;
}

static int proxy_read(const struct device *dev, off_t off, void *data, size_t len)
{
	uint8_t *out = data;
	int rc = 0;

	ARG_UNUSED(dev);
	k_mutex_lock(&proxy_data.lock, K_FOREVER);
	while (len > 0U && rc == 0) {
		const size_t n = MIN(len, sizeof(bounce));

		rc = run(OP_READ, off, n);
		if (rc == 0) {
			memcpy(out, bounce, n);
		}
		out += n;
		off += (off_t)n;
		len -= n;
	}
	k_mutex_unlock(&proxy_data.lock);
	return rc;
}

static int proxy_write(const struct device *dev, off_t off, const void *data, size_t len)
{
	const uint8_t *in = data;
	int rc = 0;

	ARG_UNUSED(dev);
	k_mutex_lock(&proxy_data.lock, K_FOREVER);
	while (len > 0U && rc == 0) {
		const size_t n = MIN(len, sizeof(bounce));

		memcpy(bounce, in, n);
		rc = run(OP_WRITE, off, n);
		in += n;
		off += (off_t)n;
		len -= n;
	}
	k_mutex_unlock(&proxy_data.lock);
	return rc;
}

static int proxy_erase(const struct device *dev, off_t off, size_t size)
{
	int rc;

	ARG_UNUSED(dev);
	k_mutex_lock(&proxy_data.lock, K_FOREVER);
	rc = run(OP_ERASE, off, size);
	k_mutex_unlock(&proxy_data.lock);
	return rc;
}

static const struct flash_parameters *proxy_get_parameters(const struct device *dev)
{
	ARG_UNUSED(dev);
	return flash_get_parameters(proxy_cfg.backend);
}

static int proxy_get_size(const struct device *dev, uint64_t *size)
{
	ARG_UNUSED(dev);
	return flash_get_size(proxy_cfg.backend, size);
}

#if defined(CONFIG_FLASH_PAGE_LAYOUT)
static void proxy_page_layout(const struct device *dev, const struct flash_pages_layout **layout,
			      size_t *layout_size)
{
	const struct flash_driver_api *api = proxy_cfg.backend->api;

	ARG_UNUSED(dev);
	api->page_layout(proxy_cfg.backend, layout, layout_size);
}
#endif

#if defined(CONFIG_FLASH_JESD216_API)
static int proxy_sfdp_read(const struct device *dev, off_t off, void *data, size_t len)
{
	int rc;

	ARG_UNUSED(dev);
	if (len > sizeof(bounce)) {
		return -EINVAL;
	}
	k_mutex_lock(&proxy_data.lock, K_FOREVER);
	rc = run(OP_SFDP, off, len);
	if (rc == 0) {
		memcpy(data, bounce, len);
	}
	k_mutex_unlock(&proxy_data.lock);
	return rc;
}

static int proxy_read_jedec_id(const struct device *dev, uint8_t *id)
{
	int rc;

	ARG_UNUSED(dev);
	k_mutex_lock(&proxy_data.lock, K_FOREVER);
	rc = run(OP_JEDEC, 0, JEDEC_ID_LEN);
	if (rc == 0) {
		memcpy(id, bounce, JEDEC_ID_LEN);
	}
	k_mutex_unlock(&proxy_data.lock);
	return rc;
}
#endif

static DEVICE_API(flash, proxy_api) = {
	.read = proxy_read,
	.write = proxy_write,
	.erase = proxy_erase,
	.get_parameters = proxy_get_parameters,
	.get_size = proxy_get_size,
#if defined(CONFIG_FLASH_PAGE_LAYOUT)
	.page_layout = proxy_page_layout,
#endif
#if defined(CONFIG_FLASH_JESD216_API)
	.sfdp_read = proxy_sfdp_read,
	.read_jedec_id = proxy_read_jedec_id,
#endif
};

static int proxy_init(const struct device *dev)
{
	struct proxy_data *d = &proxy_data;

	ARG_UNUSED(dev);
	if (!device_is_ready(proxy_cfg.backend)) {
		LOG_ERR("backend %s not ready", proxy_cfg.backend->name);
		return -ENODEV;
	}
	k_mutex_init(&d->lock);
	k_sem_init(&d->start, 0, 1);
	k_sem_init(&d->done, 0, 1);
	k_thread_create(&proxy_thread, proxy_stack, K_THREAD_STACK_SIZEOF(proxy_stack), proxy_main,
			d, NULL, NULL, CONFIG_FLASH_SRAM_PROXY_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&proxy_thread, "flash_proxy");
	d->running = true;
	return 0;
}

DEVICE_DT_INST_DEFINE(0, proxy_init, NULL, &proxy_data, &proxy_cfg, POST_KERNEL,
		      CONFIG_FLASH_SRAM_PROXY_INIT_PRIORITY, &proxy_api);
