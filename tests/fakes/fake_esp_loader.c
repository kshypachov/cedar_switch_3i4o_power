/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * See fake_esp_loader.h.
 */

#include <string.h>

#include <zephyr/sys/util.h>

#include "fake_esp_loader.h"

struct fake_esp_loader fake_esp_loader;

static int record(enum fake_esp_call call)
{
	struct fake_esp_loader *f = &fake_esp_loader;
	struct coprocessor_status st;

	coprocessor_manager_get_status(&st);
	f->mode_at[call] = st.uart_mode;
	f->count[call]++;
	if (!(call == FEL_FLASH_WRITE && f->call_count > 0 &&
	      f->calls[f->call_count - 1] == FEL_FLASH_WRITE) &&
	    f->call_count < ARRAY_SIZE(f->calls)) {
		f->calls[f->call_count++] = call;
	}
	return f->result[call];
}

static int fk_port_init(void *ctx)
{
	int rc = record(FEL_PORT_INIT);

	ARG_UNUSED(ctx);
	fake_esp_loader.port_live = rc == 0;
	return rc;
}

static void fk_port_deinit(void *ctx)
{
	ARG_UNUSED(ctx);
	(void)record(FEL_PORT_DEINIT);
	fake_esp_loader.port_live = false;
}

static int fk_connect(void *ctx)
{
	ARG_UNUSED(ctx);
	return record(FEL_CONNECT);
}

static int fk_get_target(void *ctx)
{
	ARG_UNUSED(ctx);
	(void)record(FEL_GET_TARGET);
	return fake_esp_loader.target;
}

static int fk_flash_start(void *ctx, uint32_t offset, uint32_t size, uint32_t block_size)
{
	ARG_UNUSED(ctx);
	fake_esp_loader.start_offset = offset;
	fake_esp_loader.start_size = size;
	fake_esp_loader.start_block = block_size;
	return record(FEL_FLASH_START);
}

static int fk_flash_write(void *ctx, const uint8_t *data, uint32_t len)
{
	struct fake_esp_loader *f = &fake_esp_loader;
	int rc = record(FEL_FLASH_WRITE);

	ARG_UNUSED(ctx);
	if (f->write_fail_at != 0U && f->count[FEL_FLASH_WRITE] != f->write_fail_at) {
		rc = 0;
	}
	if (rc != 0) {
		return rc;
	}
	for (uint32_t i = 0; i < len && f->written < sizeof(f->bytes); i++) {
		f->bytes[f->written++] = data[i];
	}
	f->largest_write = MAX(f->largest_write, len);
	return 0;
}

static int fk_flash_finish(void *ctx)
{
	ARG_UNUSED(ctx);
	return record(FEL_FLASH_FINISH);
}

static void fk_reset_target(void *ctx)
{
	ARG_UNUSED(ctx);
	(void)record(FEL_RESET_TARGET);
}

static int fk_change_rate(void *ctx, uint32_t baud)
{
	ARG_UNUSED(ctx);
	fake_esp_loader.rate = baud;
	return record(FEL_CHANGE_RATE);
}

static int fk_lines_idle(void *ctx)
{
	ARG_UNUSED(ctx);
	return record(FEL_LINES_IDLE);
}

static int fk_console_restore(void *ctx)
{
	ARG_UNUSED(ctx);
	return record(FEL_CONSOLE_RESTORE);
}

const struct esp_loader_adapter_lib fake_esp_loader_lib = {
	.port_init = fk_port_init,
	.port_deinit = fk_port_deinit,
	.connect = fk_connect,
	.get_target = fk_get_target,
	.flash_start = fk_flash_start,
	.flash_write = fk_flash_write,
	.flash_finish = fk_flash_finish,
	.reset_target = fk_reset_target,
	.change_rate = fk_change_rate,
	.lines_idle = fk_lines_idle,
	.console_restore = fk_console_restore,
};

void fake_esp_loader_init(void)
{
	memset(&fake_esp_loader, 0, sizeof(fake_esp_loader));
	fake_esp_loader.target = ESP_LOADER_ADAPTER_CHIP_ESP32C6;
}

int fake_esp_loader_first(enum fake_esp_call call)
{
	for (size_t i = 0; i < fake_esp_loader.call_count; i++) {
		if (fake_esp_loader.calls[i] == call) {
			return (int)i;
		}
	}
	return -1;
}

int fake_esp_loader_last(enum fake_esp_call call)
{
	for (size_t i = fake_esp_loader.call_count; i > 0; i--) {
		if (fake_esp_loader.calls[i - 1] == call) {
			return (int)(i - 1);
		}
	}
	return -1;
}
