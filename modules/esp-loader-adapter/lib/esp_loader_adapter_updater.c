/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * esp-loader-adapter's session as struct coprocessor_updater_loader.
 */

#include <zephyr/sys/util.h>

#include <esp_loader_adapter/esp_loader_adapter.h>
#include <esp_loader_adapter/updater_loader.h>

static void copy(struct coprocessor_update_error *out, const struct esp_loader_adapter_error *in)
{
	if (out != NULL) {
		out->code = in->code != NULL ? in->code : "internal_error";
		out->message = in->message != NULL ? in->message : "The flasher failed";
		out->retryable = in->retryable;
	}
}

static int u_open(void *ctx, struct coprocessor_update_error *err)
{
	struct esp_loader_adapter_error e = {0};
	int rc = esp_loader_adapter_open(&e);

	ARG_UNUSED(ctx);
	if (rc != 0) {
		copy(err, &e);
	}
	return rc;
}

static int u_begin(void *ctx, uint32_t size, struct coprocessor_update_error *err)
{
	struct esp_loader_adapter_error e = {0};
	int rc = esp_loader_adapter_begin(size, &e);

	ARG_UNUSED(ctx);
	if (rc != 0) {
		copy(err, &e);
	}
	return rc;
}

static int u_write(void *ctx, const uint8_t *data, size_t len, struct coprocessor_update_error *err)
{
	struct esp_loader_adapter_error e = {0};
	int rc = esp_loader_adapter_write(data, len, &e);

	ARG_UNUSED(ctx);
	if (rc != 0) {
		copy(err, &e);
	}
	return rc;
}

static int u_finish(void *ctx, struct coprocessor_update_error *err)
{
	struct esp_loader_adapter_error e = {0};
	int rc = esp_loader_adapter_finish(&e);

	ARG_UNUSED(ctx);
	if (rc != 0) {
		copy(err, &e);
	}
	return rc;
}

static int u_close(void *ctx)
{
	ARG_UNUSED(ctx);
	return esp_loader_adapter_close();
}

static const struct coprocessor_updater_loader loader = {
	.open = u_open,
	.begin = u_begin,
	.write = u_write,
	.finish = u_finish,
	.close = u_close,
};

const struct coprocessor_updater_loader *esp_loader_adapter_updater_loader(void)
{
	return &loader;
}
