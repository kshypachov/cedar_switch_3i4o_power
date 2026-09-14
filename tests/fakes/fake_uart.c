/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * See fake_uart.h.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "fake_uart.h"

struct fake_uart fake_uart;

static void during_call(void)
{
	void (*hook)(void *arg) = fake_uart.during_call;

	fake_uart.during_call = NULL;
	if (hook != NULL) {
		hook(fake_uart.during_call_arg);
	}
}

static int fk_attach(void *ctx, enum coprocessor_uart_mode owner)
{
	ARG_UNUSED(ctx);

	fake_uart.attach_calls++;
	if ((unsigned int)owner >= COPROCESSOR_UART_MODE_COUNT) {
		return -EINVAL;
	}
	if (fake_uart.attach_errno[owner] != 0) {
		return fake_uart.attach_errno[owner];
	}
	if (fake_uart.flasher_open && owner != COPROCESSOR_UART_FLASHING) {
		fake_uart.double_ownership++;
	}
	fake_uart.attached = true;
	fake_uart.owner = owner;
	/* The flasher opens the UART itself; attaching it installs nothing. */
	fake_uart.handler =
		(owner == COPROCESSOR_UART_CONSOLE || owner == COPROCESSOR_UART_USB_BRIDGE);

	return 0;
}

static int fk_detach(void *ctx)
{
	ARG_UNUSED(ctx);

	fake_uart.detach_calls++;
	if (fake_uart.detach_errno != 0) {
		return fake_uart.detach_errno;
	}
	fake_uart.attached = false;
	fake_uart.handler = false;

	return 0;
}

static uint32_t fk_activity(void *ctx)
{
	ARG_UNUSED(ctx);

	if (!fake_uart.handler && fake_uart.noisy_reads > 0U) {
		fake_uart.activity++;
		if (fake_uart.noisy_reads != FAKE_UART_NEVER_QUIET) {
			fake_uart.noisy_reads--;
		}
	}

	return fake_uart.activity;
}

static int fk_reset(void *ctx, bool download)
{
	ARG_UNUSED(ctx);

	during_call();
	fake_uart.reset_calls++;
	fake_uart.last_reset_download = download;

	return fake_uart.reset_errno;
}

static bool fk_transport_ready(void *ctx)
{
	ARG_UNUSED(ctx);

	return fake_uart.transport_ready;
}

static void fk_marker(void *ctx, enum log_store_kind kind, uint32_t generation, const char *text)
{
	ARG_UNUSED(ctx);

	if (fake_uart.marker_count >= FAKE_UART_MAX_MARKERS) {
		return;
	}

	struct fake_uart_marker *m = &fake_uart.markers[fake_uart.marker_count++];

	m->kind = kind;
	m->generation = generation;
	strncpy(m->text, text, sizeof(m->text) - 1U);
	m->text[sizeof(m->text) - 1U] = '\0';
	m->handler_attached = fake_uart.handler;
}

static int64_t fk_now(void *ctx)
{
	ARG_UNUSED(ctx);

	return fake_uart.real_time ? k_uptime_get() : fake_uart.now_ms;
}

static void fk_sleep(void *ctx, uint32_t ms)
{
	ARG_UNUSED(ctx);

	during_call();
	fake_uart.sleeps++;
	if (fake_uart.real_time) {
		k_msleep((int32_t)ms);
	} else {
		fake_uart.now_ms += ms;
	}
}

const struct coprocessor_platform fake_uart_platform = {
	.uart_attach = fk_attach,
	.uart_detach = fk_detach,
	.uart_rx_activity = fk_activity,
	.c6_reset = fk_reset,
	.transport_ready = fk_transport_ready,
	.marker = fk_marker,
	.now_ms = fk_now,
	.sleep_ms = fk_sleep,
	.ctx = &fake_uart,
};

void fake_uart_init(void)
{
	memset(&fake_uart, 0, sizeof(fake_uart));
}

void fake_uart_receive(size_t n)
{
	if (fake_uart.handler) {
		fake_uart.activity++;
		if (fake_uart.owner == COPROCESSOR_UART_CONSOLE) {
			fake_uart.console_bytes += n;
		} else {
			fake_uart.bridge_bytes += n;
		}
	} else if (fake_uart.flasher_open) {
		fake_uart.flasher_bytes += n;
	} else {
		fake_uart.lost_bytes += n;
	}
}

int fake_uart_flasher_open(void)
{
	if (fake_uart.handler) {
		fake_uart.double_ownership++;
		return -EBUSY;
	}
	fake_uart.flasher_open = true;

	return 0;
}

void fake_uart_flasher_close(void)
{
	fake_uart.flasher_open = false;
}
