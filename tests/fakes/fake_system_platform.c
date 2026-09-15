/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The fake board of system-updater (fake_system_platform.h).
 */

#include "fake_system_platform.h"

#include <errno.h>
#include <string.h>

struct fake_system fake_system;

static void call(enum fake_system_call c)
{
	fake_system.count[c]++;
	if (fake_system.hook != NULL && fake_system.hook_call == c) {
		void (*hook)(void *arg) = fake_system.hook;

		fake_system.hook = NULL;
		hook(fake_system.hook_arg);
	}
}

static void copy_id(char *dst, size_t cap, const char *src)
{
	strncpy(dst, src != NULL ? src : "", cap - 1U);
	dst[cap - 1U] = '\0';
}

static int running_image(void *ctx, struct system_image *out, bool *confirmed)
{
	(void)ctx;
	call(FSP_RUNNING);
	if (fake_system.rc[FSP_RUNNING] == 0 || fake_system.running_fills_on_error) {
		*out = fake_system.running;
		*confirmed = fake_system.running_confirmed;
	}
	return fake_system.rc[FSP_RUNNING];
}

static int staged_image(void *ctx, const char *upload_id, struct system_image *out,
			uint32_t *size)
{
	(void)ctx;
	(void)upload_id;
	call(FSP_STAGED);
	if (fake_system.rc[FSP_STAGED] != 0) {
		return fake_system.rc[FSP_STAGED];
	}
	*out = fake_system.staged;
	*size = fake_system.staged_size;
	return 0;
}

static int set_upload_in_use(void *ctx, const char *upload_id, bool in_use)
{
	(void)ctx;
	call(FSP_IN_USE);
	if (fake_system.rc[FSP_IN_USE] != 0) {
		return fake_system.rc[FSP_IN_USE];
	}
	fake_system.in_use = in_use;
	copy_id(fake_system.in_use_id, sizeof(fake_system.in_use_id), upload_id);
	return 0;
}

static void upload_consumed(void *ctx, const char *upload_id)
{
	(void)ctx;
	call(FSP_CONSUMED);
	copy_id(fake_system.consumed_id, sizeof(fake_system.consumed_id), upload_id);
}

static int request_swap(void *ctx)
{
	(void)ctx;
	call(FSP_REQUEST);
	fake_system.request_at_ms = fake_system.now_ms;
	if (fake_system.rc[FSP_REQUEST] != 0) {
		return fake_system.rc[FSP_REQUEST];
	}
	if (fake_system.request_sets_pending) {
		fake_system.swap_pending = true;
	}
	return 0;
}

static bool swap_pending(void *ctx)
{
	(void)ctx;
	call(FSP_SWAP_PENDING);
	return fake_system.swap_pending;
}

static int confirm(void *ctx)
{
	(void)ctx;
	call(FSP_CONFIRM);
	if (fake_system.rc[FSP_CONFIRM] != 0) {
		return fake_system.rc[FSP_CONFIRM];
	}
	if (fake_system.confirm_takes) {
		fake_system.running_confirmed = true;
	}
	return 0;
}

static void reboot(void *ctx)
{
	(void)ctx;
	call(FSP_REBOOT);
	fake_system.reboot_at_ms = fake_system.now_ms;
}

static int journal_load(void *ctx, struct system_update_journal *out)
{
	(void)ctx;
	call(FSP_LOAD);
	if (fake_system.rc[FSP_LOAD] != 0) {
		if (fake_system.load_fills_on_error && fake_system.has_journal) {
			*out = fake_system.journal;
		}
		return fake_system.rc[FSP_LOAD];
	}
	if (!fake_system.has_journal) {
		return -ENOENT;
	}
	*out = fake_system.journal;
	return 0;
}

static int journal_save(void *ctx, const struct system_update_journal *journal)
{
	(void)ctx;
	call(FSP_SAVE);
	if (fake_system.rc[FSP_SAVE] != 0 &&
	    (fake_system.save_fail_at == 0U ||
	     fake_system.count[FSP_SAVE] == fake_system.save_fail_at)) {
		return fake_system.rc[FSP_SAVE];
	}
	fake_system.journal = *journal;
	fake_system.has_journal = true;
	if (fake_system.save_count < FAKE_SYSTEM_MAX_SAVES) {
		fake_system.saves[fake_system.save_count++] = *journal;
	}
	return 0;
}

static int64_t now_ms(void *ctx)
{
	(void)ctx;
	return fake_system.now_ms;
}

static void sleep_ms(void *ctx, uint32_t ms)
{
	(void)ctx;
	call(FSP_SLEEP);
	fake_system.now_ms += ms;
	fake_system.slept_ms += ms;
}

const struct system_updater_platform fake_system_platform = {
	.running_image = running_image,
	.staged_image = staged_image,
	.set_upload_in_use = set_upload_in_use,
	.upload_consumed = upload_consumed,
	.request_swap = request_swap,
	.swap_pending = swap_pending,
	.confirm = confirm,
	.reboot = reboot,
	.journal_load = journal_load,
	.journal_save = journal_save,
	.now_ms = now_ms,
	.sleep_ms = sleep_ms,
	.ctx = NULL,
};

struct system_image fake_system_image(uint8_t major, uint8_t minor, uint16_t revision,
				      uint32_t build, uint8_t seed)
{
	struct system_image img = {
		.version = {.major = major, .minor = minor, .revision = revision, .build = build},
	};

	memset(img.hash, seed, sizeof(img.hash));
	return img;
}

void fake_system_init(void)
{
	memset(&fake_system, 0, sizeof(fake_system));
	fake_system.running = fake_system_image(1, 0, 0, 0, 0xA0);
	fake_system.running_confirmed = true;
	fake_system.staged = fake_system_image(1, 1, 0, 0, 0xB0);
	fake_system.staged_size = 100000U;
	fake_system.request_sets_pending = true;
	fake_system.confirm_takes = true;
	fake_system.now_ms = 1000;
}

void fake_system_reboot_into(const struct system_image *image, bool confirmed)
{
	fake_system.running = *image;
	fake_system.running_confirmed = confirmed;
	fake_system.swap_pending = false;
	fake_system.in_use = false;
}
