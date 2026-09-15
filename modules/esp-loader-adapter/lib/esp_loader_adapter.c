/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * esp-loader-adapter: a flashing session with guaranteed cleanup.
 * See include/esp_loader_adapter/esp_loader_adapter.h for the contract.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <coprocessor_manager/coprocessor_manager.h>
#include <esp_loader_adapter/esp_loader_adapter.h>

LOG_MODULE_REGISTER(esp_loader_adapter, CONFIG_ESP_LOADER_ADAPTER_LOG_LEVEL);

#define BLOCK ESP_LOADER_ADAPTER_BLOCK_SIZE

/* The ROM protocol counts in multiples of 4, and so does the padding below. */
BUILD_ASSERT(BLOCK % 4 == 0, "CONFIG_ESP_LOADER_ADAPTER_BLOCK_SIZE must be a multiple of 4");

static const struct esp_loader_adapter_lib *lib;
static bool session;
static bool port_up;
static bool writing;
static uint32_t announced;
static uint32_t rounded;
static uint32_t accepted;
static uint8_t block[BLOCK];
static size_t fill;

static void fail(struct esp_loader_adapter_error *err, const char *code, const char *message,
		 bool retryable, int cause)
{
	LOG_WRN("%s (%s, cause %d)", message, code, cause);
	if (err != NULL) {
		err->code = code;
		err->message = message;
		err->retryable = retryable;
		err->cause = cause;
	}
}

int esp_loader_adapter_init(const struct esp_loader_adapter_lib *l)
{
	if (session) {
		return -EBUSY;
	}
	if (l == NULL || l->port_init == NULL || l->port_deinit == NULL || l->connect == NULL ||
	    l->get_target == NULL || l->flash_start == NULL || l->flash_write == NULL ||
	    l->flash_finish == NULL || l->reset_target == NULL || l->lines_idle == NULL ||
	    l->console_restore == NULL) {
		return -EINVAL;
	}
	lib = l;
	port_up = false;
	writing = false;
	return 0;
}

/*
 * Every step, in order, whatever failed before: a C6 left in its ROM loader or
 * a UART left with the library's handler is worse than a reported error.
 */
static int teardown(void)
{
	int first = 0;
	int rc;

	if (port_up) {
		/* Normal boot needs the library's port, so it goes before deinit. */
		lib->reset_target(lib->ctx);
		lib->port_deinit(lib->ctx);
		port_up = false;
	}
	/* esp_loader_deinit() leaves EN and BOOT disconnected. */
	rc = lib->lines_idle(lib->ctx);
	if (rc != 0 && first == 0) {
		first = rc;
	}
	/* Before the console attaches, so it receives at its own rate. */
	rc = lib->console_restore(lib->ctx);
	if (rc != 0 && first == 0) {
		first = rc;
	}
	rc = coprocessor_manager_set_mode(COPROCESSOR_UART_CONSOLE);
	if (rc != 0 && first == 0) {
		first = rc;
	}
	if (first != 0) {
		LOG_ERR("handing the coprocessor back failed: %d", first);
	}
	session = false;
	writing = false;
	return first;
}

int esp_loader_adapter_open(struct esp_loader_adapter_error *err)
{
	int rc;

	if (lib == NULL) {
		fail(err, "internal_error", "The flasher is not initialised", false, -EAGAIN);
		return -EAGAIN;
	}
	if (session) {
		fail(err, "busy", "A flashing session is already open", true, -EBUSY);
		return -EBUSY;
	}

	rc = coprocessor_manager_set_mode(COPROCESSOR_UART_FLASHING);
	if (rc == -EBUSY) {
		/* The bridge, a network apply or a scan has it: nothing was taken. */
		fail(err, "busy", "The coprocessor's UART is in use (USB bridge or a network change)",
		     true, rc);
		return -EBUSY;
	}
	if (rc == -ETIMEDOUT) {
		/* The old owner keeps the UART; nothing to give back. */
		fail(err, "internal_error", "The coprocessor's UART did not go quiet", true, rc);
		return -EIO;
	}
	if (rc != 0) {
		/* The mode may be `unavailable` now: put the console back if it can be. */
		fail(err, "internal_error", "The coprocessor's UART could not be taken", true, rc);
		session = true;
		(void)teardown();
		return -EIO;
	}
	session = true;

	rc = lib->port_init(lib->ctx);
	if (rc != 0) {
		fail(err, "internal_error", "The flasher could not take the UART and the straps", true,
		     rc);
		(void)teardown();
		return -EIO;
	}
	port_up = true;

	rc = lib->connect(lib->ctx);
	if (rc != 0) {
		if (rc == ESP_LOADER_ADAPTER_LIB_TIMEOUT) {
			fail(err, "service_not_ready", "The coprocessor's ROM loader did not answer",
			     true, rc);
		} else {
			fail(err, "internal_error",
			     "The coprocessor's ROM loader answered wrongly while connecting", true, rc);
		}
		(void)teardown();
		return -EIO;
	}

	rc = lib->get_target(lib->ctx);
	if (rc != ESP_LOADER_ADAPTER_CHIP_ESP32C6) {
		fail(err, "unsupported_target", "The coprocessor is not an ESP32-C6", false, rc);
		(void)teardown();
		return -ENOTSUP;
	}

	if (CONFIG_ESP_LOADER_ADAPTER_HIGH_BAUD > 0 && lib->change_rate != NULL) {
		rc = lib->change_rate(lib->ctx, CONFIG_ESP_LOADER_ADAPTER_HIGH_BAUD);
		if (rc != 0) {
			fail(err, "internal_error", "The coprocessor refused the higher baud rate", true,
			     rc);
			(void)teardown();
			return -EIO;
		}
	}

	LOG_INF("connected to the ESP32-C6 ROM loader");
	return 0;
}

int esp_loader_adapter_begin(uint32_t size, struct esp_loader_adapter_error *err)
{
	int rc;

	if (!session || writing) {
		fail(err, "internal_error", "No flashing session to start a write in", false, -EPERM);
		return -EPERM;
	}
	if (size == 0U) {
		fail(err, "internal_error", "An empty image writes nothing", false, -EINVAL);
		return -EINVAL;
	}

	rounded = ROUND_UP(size, 4U);
	rc = lib->flash_start(lib->ctx, 0x0, rounded, BLOCK);
	if (rc != 0) {
		fail(err, "internal_error", "The coprocessor refused to start the write", true, rc);
		(void)teardown();
		return -EIO;
	}
	writing = true;
	announced = size;
	accepted = 0U;
	fill = 0U;
	return 0;
}

static int send_block(size_t len, struct esp_loader_adapter_error *err)
{
	int rc = lib->flash_write(lib->ctx, block, (uint32_t)len);

	fill = 0U;
	if (rc != 0) {
		/* The ROM's sequence is broken: nothing is left but to hand everything back. */
		fail(err, "internal_error", "A block of the image was not written", true, rc);
		(void)teardown();
		return -EIO;
	}
	return 0;
}

int esp_loader_adapter_write(const uint8_t *data, size_t len, struct esp_loader_adapter_error *err)
{
	if (!writing) {
		fail(err, "internal_error", "No write in progress", false, -EPERM);
		return -EPERM;
	}
	if (len > (size_t)(announced - accepted)) {
		fail(err, "internal_error", "More bytes than the image size", false, -EINVAL);
		return -EINVAL;
	}

	accepted += (uint32_t)len;
	while (len > 0U) {
		size_t n = MIN(len, BLOCK - fill);

		memcpy(&block[fill], data, n);
		fill += n;
		data += n;
		len -= n;
		if (fill == BLOCK) {
			int rc = send_block(BLOCK, err);

			if (rc != 0) {
				return rc;
			}
		}
	}
	return 0;
}

int esp_loader_adapter_finish(struct esp_loader_adapter_error *err)
{
	int rc;

	if (!writing) {
		fail(err, "internal_error", "No write in progress", false, -EPERM);
		return -EPERM;
	}
	if (accepted != announced) {
		fail(err, "internal_error", "Fewer bytes were written than the image size", false,
		     -EINVAL);
		return -EINVAL;
	}

	/*
	 * Pad to the size announced to FLASH_BEGIN; 0xFF is what erased flash
	 * holds. A block is a multiple of 4 and full blocks were already sent,
	 * so the padding always fits in the one that is left.
	 */
	memset(&block[fill], 0xFF, rounded - announced);
	fill += rounded - announced;
	if (fill > 0U) {
		rc = send_block(fill, err);
		if (rc != 0) {
			return rc;
		}
	}

	writing = false;
	rc = lib->flash_finish(lib->ctx);
	if (rc != 0) {
		if (rc == ESP_LOADER_ADAPTER_LIB_INVALID_MD5) {
			fail(err, "internal_error", "The written image failed its MD5 check", true, rc);
		} else {
			fail(err, "internal_error", "The coprocessor did not confirm the end of the write",
			     true, rc);
		}
		(void)teardown();
		return -EIO;
	}
	LOG_INF("wrote and verified %u bytes", announced);
	return 0;
}

int esp_loader_adapter_close(void)
{
	if (!session) {
		return 0;
	}
	return teardown();
}

bool esp_loader_adapter_is_open(void)
{
	return session;
}

uint32_t esp_loader_adapter_written(void)
{
	return accepted;
}
