/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * See fake_update_platform.h.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include "fake_update_platform.h"

struct fake_update fake_update;

static bool collapsed(enum fake_update_call call)
{
	return call == FUP_WRITE || call == FUP_IMAGE_READ || call == FUP_EVIDENCE;
}

static int record(enum fake_update_call call)
{
	struct fake_update *f = &fake_update;

	f->count[call]++;
	if (!(collapsed(call) && f->call_count > 0 && f->calls[f->call_count - 1] == call) &&
	    f->call_count < ARRAY_SIZE(f->calls)) {
		f->calls[f->call_count++] = call;
	}
	if (f->hook != NULL && f->hook_call == call &&
	    (f->hook_nth == 0U || f->count[call] == f->hook_nth)) {
		void (*hook)(void *arg) = f->hook;

		f->hook = NULL;
		hook(f->hook_arg);
	}
	f->now_ms += f->cost_ms[call];
	return f->rc[call];
}

static int fail_with(enum fake_update_call call, int rc, struct coprocessor_update_error *err)
{
	if (rc != 0 && err != NULL) {
		err->code = fake_update.err_code[call] != NULL ? fake_update.err_code[call]
							       : "internal_error";
		err->message = "the fake loader failed";
		err->retryable = true;
	}
	return rc;
}

static int fk_open(void *ctx, struct coprocessor_update_error *err)
{
	int rc = record(FUP_OPEN);

	ARG_UNUSED(ctx);
	fake_update.loader_open = rc == 0;
	return fail_with(FUP_OPEN, rc, err);
}

static int fk_begin(void *ctx, uint32_t size, struct coprocessor_update_error *err)
{
	int rc = record(FUP_BEGIN);

	ARG_UNUSED(ctx);
	if (!fake_update.loader_open) {
		fake_update.outside_session++;
	}
	fake_update.begin_size = size;
	return fail_with(FUP_BEGIN, rc, err);
}

static int fk_write(void *ctx, const uint8_t *data, size_t len, struct coprocessor_update_error *err)
{
	struct fake_update *f = &fake_update;
	int rc = record(FUP_WRITE);

	ARG_UNUSED(ctx);
	if (!f->loader_open) {
		f->outside_session++;
	}
	if (f->write_fail_at != 0U && f->count[FUP_WRITE] != f->write_fail_at) {
		rc = 0;
	}
	if (rc != 0) {
		return fail_with(FUP_WRITE, rc, err);
	}
	for (size_t i = 0; i < len; i++) {
		if (data[i] != fake_update_image_byte(f->written + (uint32_t)i)) {
			f->mismatches++;
		}
	}
	f->written += (uint32_t)len;
	return 0;
}

static int fk_finish(void *ctx, struct coprocessor_update_error *err)
{
	int rc = record(FUP_FINISH);

	ARG_UNUSED(ctx);
	if (!fake_update.loader_open) {
		fake_update.outside_session++;
	}
	return fail_with(FUP_FINISH, rc, err);
}

static int fk_close(void *ctx)
{
	int rc = record(FUP_CLOSE);

	ARG_UNUSED(ctx);
	if (!fake_update.loader_open) {
		fake_update.outside_session++;
	}
	/* A close always ends the session, whatever it reports. */
	fake_update.loader_open = false;
	return rc;
}

static const struct coprocessor_updater_loader fake_loader = {
	.open = fk_open,
	.begin = fk_begin,
	.write = fk_write,
	.finish = fk_finish,
	.close = fk_close,
};

static int fk_image_open(void *ctx, const char *upload_id, uint32_t *size)
{
	int rc = record(FUP_IMAGE_OPEN);

	ARG_UNUSED(ctx);
	strncpy(fake_update.upload_id, upload_id, sizeof(fake_update.upload_id) - 1U);
	fake_update.upload_id[sizeof(fake_update.upload_id) - 1U] = '\0';
	if (rc == 0) {
		fake_update.image_open = true;
		*size = fake_update.image_size;
	}
	return rc;
}

static int fk_image_read(void *ctx, uint32_t offset, uint8_t *buf, size_t len)
{
	int rc = record(FUP_IMAGE_READ);
	uint32_t n;

	ARG_UNUSED(ctx);
	if (rc != 0) {
		return rc;
	}
	if (!fake_update.image_open || offset >= fake_update.read_fail_at) {
		return -EIO;
	}
	if ((uint64_t)offset + len > fake_update.image_size) {
		fake_update.overreads++;
	}
	if (offset >= fake_update.read_zero_at) {
		return 0;
	}
	n = MIN((uint32_t)len, fake_update.image_size - offset);
	for (uint32_t i = 0; i < n; i++) {
		buf[i] = fake_update_image_byte(offset + i);
	}
	return (int)n;
}

static void fk_image_close(void *ctx)
{
	ARG_UNUSED(ctx);
	(void)record(FUP_IMAGE_CLOSE);
	fake_update.image_open = false;
}

static int fk_journal_load(void *ctx, struct coprocessor_update_journal *out)
{
	ARG_UNUSED(ctx);
	if (!fake_update.has_journal) {
		return -ENOENT;
	}
	*out = fake_update.journal;
	return 0;
}

static int fk_journal_save(void *ctx, const struct coprocessor_update_journal *journal)
{
	struct fake_update *f = &fake_update;

	ARG_UNUSED(ctx);
	f->count[FUP_SAVE]++;
	if (f->hook != NULL && f->hook_call == FUP_SAVE &&
	    (f->hook_nth == 0U || f->count[FUP_SAVE] == f->hook_nth)) {
		void (*hook)(void *arg) = f->hook;

		f->hook = NULL;
		hook(f->hook_arg);
	}
	if (f->save_rc != 0) {
		return f->save_rc;
	}
	f->journal = *journal;
	f->has_journal = true;
	if (f->save_count < ARRAY_SIZE(f->saves)) {
		f->saves[f->save_count].active = journal->active;
		f->saves[f->save_count].phase = journal->phase;
		f->save_count++;
	}
	return 0;
}

static void fk_arm(void *ctx)
{
	ARG_UNUSED(ctx);
	(void)record(FUP_ARM);
	fake_update.polls = 0;
}

static bool fk_evidence(void *ctx, char *app_version, size_t cap)
{
	struct fake_update *f = &fake_update;

	ARG_UNUSED(ctx);
	(void)record(FUP_EVIDENCE);
	f->polls++;
	if (f->evidence_after < 0 || f->polls <= (unsigned int)f->evidence_after) {
		return false;
	}
	strncpy(app_version, f->app_version != NULL ? f->app_version : "", cap - 1U);
	app_version[cap - 1U] = '\0';
	return true;
}

static int fk_restart(void *ctx, uint32_t timeout_ms, char *version, size_t cap)
{
	struct fake_update *f = &fake_update;
	int rc = record(FUP_RESTART);

	ARG_UNUSED(ctx);
	f->restart_timeout = timeout_ms;
	strncpy(version, f->restart_version != NULL ? f->restart_version : "", cap - 1U);
	version[cap - 1U] = '\0';
	return rc;
}

static int64_t fk_now(void *ctx)
{
	ARG_UNUSED(ctx);
	return fake_update.now_ms;
}

static void fk_sleep(void *ctx, uint32_t ms)
{
	ARG_UNUSED(ctx);
	fake_update.sleeps++;
	fake_update.now_ms += ms;
}

const struct coprocessor_updater_platform fake_update_platform = {
	.loader = &fake_loader,
	.image_open = fk_image_open,
	.image_read = fk_image_read,
	.image_close = fk_image_close,
	.journal_load = fk_journal_load,
	.journal_save = fk_journal_save,
	.boot_evidence_arm = fk_arm,
	.boot_evidence = fk_evidence,
	.transport_restart = fk_restart,
	.now_ms = fk_now,
	.sleep_ms = fk_sleep,
};

void fake_update_init(void)
{
	memset(&fake_update, 0, sizeof(fake_update));
	fake_update.image_size = 10000;
	fake_update.read_fail_at = UINT32_MAX;
	fake_update.read_zero_at = UINT32_MAX;
	fake_update.evidence_after = 0;
	fake_update.app_version = "1";
	fake_update.restart_version = "3.0.6";
	fake_update.now_ms = 1000;
}

int fake_update_first(enum fake_update_call call)
{
	for (size_t i = 0; i < fake_update.call_count; i++) {
		if (fake_update.calls[i] == call) {
			return (int)i;
		}
	}
	return -1;
}
