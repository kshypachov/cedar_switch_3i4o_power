/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * coprocessor-updater: the UART install as one job.
 * See include/coprocessor_updater/coprocessor_updater.h for the contract.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <coprocessor_manager/coprocessor_manager.h>
#include <coprocessor_updater/coprocessor_updater.h>

LOG_MODULE_REGISTER(coprocessor_updater, CONFIG_COPROCESSOR_UPDATER_LOG_LEVEL);

static K_MUTEX_DEFINE(lock);
static const struct coprocessor_updater_platform *pf;
/* The RAM copy of the journal; journal.active is "an install is accepted or running". */
static struct coprocessor_update_journal journal;
static bool pending;

static uint8_t block[CONFIG_COPROCESSOR_UPDATER_READ_BLOCK];
static char timeout_message[COPROCESSOR_UPDATE_MESSAGE_MAX_LEN + 1];

static const char *const phase_names[COPROCESSOR_UPDATE_PHASE_COUNT] = {
	[COPROCESSOR_UPDATE_PREFLIGHT] = "preflight",
	[COPROCESSOR_UPDATE_ENTERING_BOOTLOADER] = "entering_bootloader",
	[COPROCESSOR_UPDATE_BEGIN] = "begin",
	[COPROCESSOR_UPDATE_WRITING] = "writing",
	[COPROCESSOR_UPDATE_VERIFYING] = "verifying",
	[COPROCESSOR_UPDATE_RECONNECTING] = "reconnecting",
	[COPROCESSOR_UPDATE_HEALTH_CHECK] = "health_check",
	[COPROCESSOR_UPDATE_COMPLETE] = "complete",
};

const char *coprocessor_update_phase_str(enum coprocessor_update_phase phase)
{
	return (unsigned int)phase < COPROCESSOR_UPDATE_PHASE_COUNT ? phase_names[phase] : NULL;
}

const char *coprocessor_update_outcome_str(enum coprocessor_update_outcome outcome)
{
	switch (outcome) {
	case COPROCESSOR_UPDATE_SUCCEEDED:
		return "succeeded";
	case COPROCESSOR_UPDATE_FAILED:
		return "failed";
	case COPROCESSOR_UPDATE_INTERRUPTED:
		return "interrupted";
	default:
		return NULL;
	}
}

static uint32_t phase_timeout_ms(enum coprocessor_update_phase phase)
{
	switch (phase) {
	case COPROCESSOR_UPDATE_PREFLIGHT:
		return CONFIG_COPROCESSOR_UPDATER_PREFLIGHT_TIMEOUT_MS;
	case COPROCESSOR_UPDATE_ENTERING_BOOTLOADER:
		return CONFIG_COPROCESSOR_UPDATER_CONNECT_TIMEOUT_MS;
	case COPROCESSOR_UPDATE_BEGIN:
		return CONFIG_COPROCESSOR_UPDATER_BEGIN_TIMEOUT_MS;
	case COPROCESSOR_UPDATE_WRITING:
		return CONFIG_COPROCESSOR_UPDATER_WRITE_TIMEOUT_S * 1000U;
	case COPROCESSOR_UPDATE_VERIFYING:
		return CONFIG_COPROCESSOR_UPDATER_VERIFY_TIMEOUT_MS;
	case COPROCESSOR_UPDATE_RECONNECTING:
		return CONFIG_COPROCESSOR_UPDATER_RECONNECT_TIMEOUT_MS;
	case COPROCESSOR_UPDATE_HEALTH_CHECK:
		return CONFIG_COPROCESSOR_UPDATER_HEALTH_TIMEOUT_MS;
	default:
		return UINT32_MAX;
	}
}

static void copy_str(char *dst, size_t cap, const char *src)
{
	if (src == NULL) {
		src = "";
	}
	strncpy(dst, src, cap - 1U);
	dst[cap - 1U] = '\0';
}

static void save(const struct coprocessor_update_journal *copy)
{
	int rc = pf->journal_save(pf->ctx, copy);

	if (rc != 0) {
		/* The install goes on: losing the journal only loses `interrupted`. */
		LOG_ERR("saving the update journal failed: %d", rc);
	}
}

int coprocessor_updater_init(const struct coprocessor_updater_platform *platform)
{
	struct coprocessor_update_journal j;

	if (platform == NULL || platform->loader == NULL || platform->loader->open == NULL ||
	    platform->loader->begin == NULL || platform->loader->write == NULL ||
	    platform->loader->finish == NULL || platform->loader->close == NULL ||
	    platform->image_open == NULL || platform->image_read == NULL ||
	    platform->image_close == NULL || platform->journal_load == NULL ||
	    platform->journal_save == NULL || platform->boot_evidence_arm == NULL ||
	    platform->boot_evidence == NULL || platform->transport_restart == NULL ||
	    platform->now_ms == NULL || platform->sleep_ms == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);
	if (pf != NULL && journal.active) {
		k_mutex_unlock(&lock);
		return -EBUSY;
	}
	pf = platform;
	pending = false;
	k_mutex_unlock(&lock);

	memset(&j, 0, sizeof(j));
	if (pf->journal_load(pf->ctx, &j) != 0 || j.format != COPROCESSOR_UPDATE_JOURNAL_FORMAT ||
	    (unsigned int)j.phase >= COPROCESSOR_UPDATE_PHASE_COUNT ||
	    (unsigned int)j.last.phase >= COPROCESSOR_UPDATE_PHASE_COUNT) {
		memset(&j, 0, sizeof(j));
		j.format = COPROCESSOR_UPDATE_JOURNAL_FORMAT;
	}
	j.job_id[sizeof(j.job_id) - 1U] = '\0';
	j.upload_id[sizeof(j.upload_id) - 1U] = '\0';
	j.last.job_id[sizeof(j.last.job_id) - 1U] = '\0';
	j.last.version[sizeof(j.last.version) - 1U] = '\0';
	j.last.error_code[sizeof(j.last.error_code) - 1U] = '\0';
	j.last.error_message[sizeof(j.last.error_message) - 1U] = '\0';

	if (j.active) {
		struct coprocessor_update_summary *s = &j.last;

		/* Never continued: the chip's state is unknown, a person decides. */
		memset(s, 0, sizeof(*s));
		copy_str(s->job_id, sizeof(s->job_id), j.job_id);
		s->state = COPROCESSOR_UPDATE_INTERRUPTED;
		s->recovery_required = j.phase >= COPROCESSOR_UPDATE_BEGIN;
		s->phase = j.phase;
		s->has_error = true;
		s->error_retryable = true;
		copy_str(s->error_code, sizeof(s->error_code), "internal_error");
		snprintf(s->error_message, sizeof(s->error_message),
			 "The STM32 restarted during %s; the install was not continued",
			 phase_names[j.phase]);
		j.has_last = true;
		j.active = false;
		LOG_WRN("an install was interrupted in %s (job %s)", phase_names[j.phase], j.job_id);
		save(&j);
	}

	k_mutex_lock(&lock, K_FOREVER);
	journal = j;
	k_mutex_unlock(&lock);
	return 0;
}

int coprocessor_updater_check(void)
{
	struct coprocessor_status st;
	bool active;

	if (pf == NULL) {
		return -EAGAIN;
	}
	k_mutex_lock(&lock, K_FOREVER);
	active = journal.active;
	k_mutex_unlock(&lock);
	if (active) {
		return -EBUSY;
	}
	coprocessor_manager_get_status(&st);
	if (st.uart_mode == COPROCESSOR_UART_USB_BRIDGE) {
		return -EBUSY;
	}
	return 0;
}

int coprocessor_updater_start(const char *upload_id, const char *job_id)
{
	struct coprocessor_update_journal copy;
	int rc;

	if (pf == NULL) {
		return -EAGAIN;
	}
	if (upload_id == NULL || job_id == NULL || upload_id[0] == '\0' || job_id[0] == '\0' ||
	    strlen(upload_id) > COPROCESSOR_UPDATE_UPLOAD_ID_MAX_LEN ||
	    strlen(job_id) > JOB_ID_MAX_LEN) {
		return -EINVAL;
	}
	rc = coprocessor_updater_check();
	if (rc != 0) {
		return rc;
	}

	k_mutex_lock(&lock, K_FOREVER);
	if (journal.active) {
		k_mutex_unlock(&lock);
		return -EBUSY;
	}
	/* Reserved under the lock, so two starts cannot both pass. */
	journal.active = true;
	journal.phase = COPROCESSOR_UPDATE_PREFLIGHT;
	copy_str(journal.job_id, sizeof(journal.job_id), job_id);
	copy_str(journal.upload_id, sizeof(journal.upload_id), upload_id);
	copy = journal;
	k_mutex_unlock(&lock);

	rc = pf->journal_save(pf->ctx, &copy);

	k_mutex_lock(&lock, K_FOREVER);
	if (rc != 0) {
		journal.active = false;
	} else {
		pending = true;
	}
	k_mutex_unlock(&lock);

	if (rc != 0) {
		LOG_ERR("the update journal could not be saved: %d", rc);
		return -EIO;
	}
	return 0;
}

void coprocessor_updater_get_state(struct coprocessor_updater_state *out)
{
	k_mutex_lock(&lock, K_FOREVER);
	out->active = journal.active;
	out->phase = journal.phase;
	copy_str(out->job_id, sizeof(out->job_id), journal.job_id);
	copy_str(out->upload_id, sizeof(out->upload_id), journal.upload_id);
	out->has_last = journal.has_last;
	out->last = journal.last;
	k_mutex_unlock(&lock);
}

/* -- the run ------------------------------------------------------------------- */

struct run {
	char job_id[JOB_ID_MAX_LEN + 1];
	char upload_id[COPROCESSOR_UPDATE_UPLOAD_ID_MAX_LEN + 1];
	enum coprocessor_update_phase phase;
	int64_t phase_started;
};

static int64_t now(void)
{
	return pf->now_ms(pf->ctx);
}

static void enter(struct run *r, enum coprocessor_update_phase phase)
{
	struct coprocessor_update_journal copy;

	r->phase = phase;
	(void)job_set_phase(r->job_id, phase_names[phase]);

	k_mutex_lock(&lock, K_FOREVER);
	journal.phase = phase;
	copy = journal;
	k_mutex_unlock(&lock);
	save(&copy);
	/*
	 * The phase's time starts once the journal is on storage: persisting it is
	 * not the phase's work, and on board B a sync on /lfs right after boot took
	 * longer than the whole preflight budget (reports/p6, hw/logs/10).
	 */
	r->phase_started = now();
	LOG_INF("install %s: %s", r->job_id, phase_names[phase]);
}

static bool timed_out(const struct run *r, struct coprocessor_update_error *err)
{
	uint32_t limit = phase_timeout_ms(r->phase);

	if (now() - r->phase_started <= (int64_t)limit) {
		return false;
	}
	snprintf(timeout_message, sizeof(timeout_message), "%s took longer than %u ms",
		 phase_names[r->phase], limit);
	err->code = "internal_error";
	err->message = timeout_message;
	err->retryable = true;
	return true;
}

static bool cancelled(const struct run *r)
{
	struct job_snapshot snap;

	return job_get(r->job_id, &snap) != 0 || snap.state == JOB_STATE_CANCELLED;
}

static void end(const struct run *r, const struct coprocessor_update_summary *s)
{
	struct coprocessor_update_journal copy;

	k_mutex_lock(&lock, K_FOREVER);
	journal.active = false;
	journal.phase = r->phase;
	if (s != NULL) {
		journal.has_last = true;
		journal.last = *s;
	}
	copy = journal;
	k_mutex_unlock(&lock);
	save(&copy);
}

void coprocessor_updater_run(void)
{
	struct run r = {0};
	struct coprocessor_update_summary s = {0};
	struct coprocessor_update_error err = {0};
	const struct coprocessor_updater_loader *ld;
	struct coprocessor_status st;
	bool image_open = false;
	bool loader_open = false;
	bool past_begin = false;
	uint32_t size = 0;
	int rc;

	k_mutex_lock(&lock, K_FOREVER);
	if (pf == NULL || !pending) {
		k_mutex_unlock(&lock);
		return;
	}
	pending = false;
	copy_str(r.job_id, sizeof(r.job_id), journal.job_id);
	copy_str(r.upload_id, sizeof(r.upload_id), journal.upload_id);
	k_mutex_unlock(&lock);
	ld = pf->loader;

	copy_str(s.job_id, sizeof(s.job_id), r.job_id);
	if (job_set_state(r.job_id, JOB_STATE_RUNNING) != 0) {
		/* Cancelled while queued. */
		goto cancelled;
	}

	/* preflight */
	enter(&r, COPROCESSOR_UPDATE_PREFLIGHT);
	if (cancelled(&r)) {
		goto cancelled;
	}
	rc = pf->image_open(pf->ctx, r.upload_id, &size);
	if (rc != 0 || size == 0U) {
		if (rc == 0) {
			pf->image_close(pf->ctx);
		}
		err.code = rc == -ENOENT ? "not_found" : "invalid_state";
		err.message = rc == -ENOENT ? "The staged image no longer exists"
					    : "The staged image is not ready to install";
		err.retryable = false;
		goto failed;
	}
	image_open = true;
	coprocessor_manager_get_status(&st);
	if (st.uart_mode == COPROCESSOR_UART_USB_BRIDGE) {
		err.code = "busy";
		err.message = "The coprocessor's UART belongs to the USB bridge";
		err.retryable = true;
		goto failed;
	}
	if (timed_out(&r, &err)) {
		goto failed;
	}

	/* entering_bootloader */
	enter(&r, COPROCESSOR_UPDATE_ENTERING_BOOTLOADER);
	if (cancelled(&r)) {
		goto cancelled;
	}
	rc = ld->open(ld->ctx, &err);
	if (rc != 0) {
		goto failed;
	}
	loader_open = true;
	if (timed_out(&r, &err)) {
		goto failed;
	}

	/* The point of no return: a cancel that landed first is reported here. */
	if (job_set_cancellable(r.job_id, false) != 0) {
		goto cancelled;
	}

	/* begin */
	enter(&r, COPROCESSOR_UPDATE_BEGIN);
	past_begin = true;
	rc = ld->begin(ld->ctx, size, &err);
	if (rc != 0 || timed_out(&r, &err)) {
		goto failed;
	}

	/* writing */
	enter(&r, COPROCESSOR_UPDATE_WRITING);
	(void)job_set_progress(r.job_id, 0, size, true, JOB_PROGRESS_UNIT_BYTES);
	for (uint32_t off = 0; off < size;) {
		size_t want = MIN(sizeof(block), (size_t)(size - off));
		int got = pf->image_read(pf->ctx, off, block, want);

		if (got <= 0 || (size_t)got > want) {
			err.code = "internal_error";
			err.message = "The staged image could not be read";
			err.retryable = true;
			goto failed;
		}
		rc = ld->write(ld->ctx, block, (size_t)got, &err);
		if (rc != 0) {
			goto failed;
		}
		off += (uint32_t)got;
		(void)job_set_progress(r.job_id, off, size, true, JOB_PROGRESS_UNIT_BYTES);
		if (timed_out(&r, &err)) {
			goto failed;
		}
	}

	/* verifying */
	enter(&r, COPROCESSOR_UPDATE_VERIFYING);
	rc = ld->finish(ld->ctx, &err);
	if (rc != 0 || timed_out(&r, &err)) {
		goto failed;
	}

	/* reconnecting: normal boot, the console back, the early sign */
	enter(&r, COPROCESSOR_UPDATE_RECONNECTING);
	pf->image_close(pf->ctx);
	image_open = false;
	pf->boot_evidence_arm(pf->ctx);
	loader_open = false;
	rc = ld->close(ld->ctx);
	if (rc != 0) {
		err.code = "internal_error";
		err.message = "The coprocessor's UART could not be handed back to the console";
		err.retryable = true;
		goto failed;
	}
	{
		char app_version[COPROCESSOR_UPDATE_VERSION_MAX_LEN + 1] = "";

		while (!(s.uart_evidence = pf->boot_evidence(pf->ctx, app_version,
							     sizeof(app_version)))) {
			if (now() - r.phase_started >=
			    (int64_t)CONFIG_COPROCESSOR_UPDATER_RECONNECT_TIMEOUT_MS) {
				/* Not decisive: the version below is the proof. */
				LOG_WRN("the console showed no application start");
				break;
			}
			pf->sleep_ms(pf->ctx, CONFIG_COPROCESSOR_UPDATER_POLL_MS);
		}
		if (s.uart_evidence) {
			LOG_INF("console: application %s started", app_version);
		}
	}

	/* health_check */
	enter(&r, COPROCESSOR_UPDATE_HEALTH_CHECK);
	rc = pf->transport_restart(pf->ctx, CONFIG_COPROCESSOR_UPDATER_HEALTH_TIMEOUT_MS, s.version,
				   sizeof(s.version));
	s.version[sizeof(s.version) - 1U] = '\0';
	if (rc == -ENODEV && s.version[0] != '\0') {
		s.stm32_restart_needed = true;
	} else if (rc != 0 || s.version[0] == '\0') {
		s.version[0] = '\0';
		err.code = "service_not_ready";
		err.message = rc == -ETIMEDOUT
				      ? "The coprocessor did not answer over ESP-Hosted after the write"
				      : "The coprocessor did not confirm its firmware version after the write";
		err.retryable = true;
		goto failed;
	}
	if (timed_out(&r, &err)) {
		s.version[0] = '\0';
		s.stm32_restart_needed = false;
		goto failed;
	}

	enter(&r, COPROCESSOR_UPDATE_COMPLETE);
	s.state = COPROCESSOR_UPDATE_SUCCEEDED;
	s.phase = r.phase;
	(void)job_set_state(r.job_id, JOB_STATE_SUCCEEDED);
	end(&r, &s);
	LOG_INF("install %s succeeded: firmware %s%s", r.job_id, s.version,
		s.stm32_restart_needed ? ", Wi-Fi after an STM32 restart" : "");
	return;

failed:
	if (loader_open) {
		(void)ld->close(ld->ctx);
	}
	if (image_open) {
		pf->image_close(pf->ctx);
	}
	s.state = COPROCESSOR_UPDATE_FAILED;
	s.phase = r.phase;
	s.recovery_required = past_begin;
	s.has_error = true;
	s.error_retryable = err.retryable;
	copy_str(s.error_code, sizeof(s.error_code), err.code != NULL ? err.code : "internal_error");
	copy_str(s.error_message, sizeof(s.error_message),
		 err.message != NULL ? err.message : "The install failed");
	(void)job_fail(r.job_id, s.error_code, s.error_retryable);
	end(&r, &s);
	LOG_ERR("install %s failed in %s: %s%s", r.job_id, phase_names[r.phase], s.error_message,
		s.recovery_required ? " (recovery required)" : "");
	return;

cancelled:
	if (loader_open) {
		(void)ld->close(ld->ctx);
	}
	if (image_open) {
		pf->image_close(pf->ctx);
	}
	/* Nothing on the chip changed: last_update stays what it was. */
	end(&r, NULL);
	LOG_INF("install %s cancelled before begin", r.job_id);
}
