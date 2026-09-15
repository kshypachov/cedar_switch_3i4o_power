/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * system-updater: the STM32 install through MCUboot's slot 2 as one job.
 * See include/system_updater/system_updater.h for the contract.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <system_updater/system_updater.h>

LOG_MODULE_REGISTER(system_updater, CONFIG_SYSTEM_UPDATER_LOG_LEVEL);

static K_MUTEX_DEFINE(lock);
/* Serialises the confirmation itself between tick() and confirm_now(). */
static K_MUTEX_DEFINE(confirm_lock);

static const struct system_updater_platform *pf;
/* The RAM copy of the journal. */
static struct system_update_journal journal;
/* An install is accepted or running, up to the restart. */
static bool active;
/* Accepted, run() has not taken it yet. */
static bool pending;
/* run() is executing. */
static bool running_now;
static enum system_update_phase phase;

static bool running_known;
static struct system_image running;
static bool running_confirmed;
static bool swap_pending;

static bool confirm_armed;
static int64_t confirm_deadline;
static int64_t confirm_retry_at;
static uint32_t confirm_backoff;

static const char *const phase_names[SYSTEM_UPDATE_PHASE_COUNT] = {
	[SYSTEM_UPDATE_PREPARING] = "preparing",
	[SYSTEM_UPDATE_REQUESTING] = "requesting",
	[SYSTEM_UPDATE_REBOOTING] = "rebooting",
};

static const char *const outcome_names[SYSTEM_UPDATE_OUTCOME_COUNT] = {
	[SYSTEM_UPDATE_AWAITING_CONFIRMATION] = "awaiting_confirmation",
	[SYSTEM_UPDATE_SUCCEEDED] = "succeeded",
	[SYSTEM_UPDATE_FAILED] = "failed",
	[SYSTEM_UPDATE_ROLLED_BACK] = "rolled_back",
	[SYSTEM_UPDATE_INTERRUPTED] = "interrupted",
};

#define MSG_INTERRUPTED "The device restarted before the swap was requested; nothing changed"
#define MSG_NOT_UP                                                                                 \
	"After the restart the previous firmware runs: MCUboot rejected the new image, or it did " \
	"not start"
#define MSG_ROLLED_BACK                                                                            \
	"The new firmware was reset before it was confirmed; MCUboot restored the previous one"

const char *system_update_phase_str(enum system_update_phase p)
{
	return (unsigned int)p < SYSTEM_UPDATE_PHASE_COUNT ? phase_names[p] : NULL;
}

const char *system_update_outcome_str(enum system_update_outcome outcome)
{
	return (unsigned int)outcome < SYSTEM_UPDATE_OUTCOME_COUNT ? outcome_names[outcome] : NULL;
}

int system_image_version_cmp(const struct system_image_version *a,
			     const struct system_image_version *b)
{
	if (a->major != b->major) {
		return a->major < b->major ? -1 : 1;
	}
	if (a->minor != b->minor) {
		return a->minor < b->minor ? -1 : 1;
	}
	if (a->revision != b->revision) {
		return a->revision < b->revision ? -1 : 1;
	}
	if (a->build != b->build) {
		return a->build < b->build ? -1 : 1;
	}
	return 0;
}

int system_image_version_str(const struct system_image_version *v, char *buf, size_t cap)
{
	int n = snprintf(buf, cap, "%u.%u.%u+%u", v->major, v->minor, v->revision, v->build);

	if (n < 0 || (size_t)n >= cap) {
		return -ENOSPC;
	}
	return n;
}

static void copy_str(char *dst, size_t cap, const char *src)
{
	if (src == NULL) {
		src = "";
	}
	strncpy(dst, src, cap - 1U);
	dst[cap - 1U] = '\0';
}

static bool same_image(const struct system_image *a, const struct system_image *b)
{
	return memcmp(a->hash, b->hash, sizeof(a->hash)) == 0 &&
	       system_image_version_cmp(&a->version, &b->version) == 0;
}

static int save(const struct system_update_journal *copy)
{
	int rc = pf->journal_save(pf->ctx, copy);

	if (rc != 0) {
		LOG_ERR("saving the update journal failed: %d", rc);
	}
	return rc;
}

/* -- reconciliation ------------------------------------------------------------ */

static void set_last(struct system_update_journal *j, enum system_update_outcome outcome,
		     const char *code, const char *message, bool retryable)
{
	struct system_update_summary *s = &j->last;

	memset(s, 0, sizeof(*s));
	copy_str(s->job_id, sizeof(s->job_id), j->job_id);
	s->state = outcome;
	s->has_from_version = true;
	s->from_version = j->from.version;
	s->has_version = true;
	s->version = j->to.version;
	if (code != NULL) {
		s->has_error = true;
		copy_str(s->error_code, sizeof(s->error_code), code);
		copy_str(s->error_message, sizeof(s->error_message), message);
		s->error_retryable = retryable;
	}
	j->has_last = true;
}

static void finish(struct system_update_journal *j, enum system_update_outcome outcome,
		   const char *code, const char *message)
{
	set_last(j, outcome, code, message, true);
	j->stage = SYSTEM_UPDATE_STAGE_NONE;
	LOG_INF("update %s: %s", j->job_id, outcome_names[outcome]);
}

/* The new image runs for the first time since the swap. */
static void first_boot(struct system_update_journal *j, bool confirmed)
{
	if (confirmed) {
		finish(j, SYSTEM_UPDATE_SUCCEEDED, NULL, NULL);
		return;
	}
	set_last(j, SYSTEM_UPDATE_AWAITING_CONFIRMATION, NULL, NULL, false);
	j->stage = SYSTEM_UPDATE_STAGE_BOOTED;
	LOG_INF("update %s: the new firmware runs, awaiting confirmation", j->job_id);
}

static void reconcile(struct system_update_journal *j, const struct system_image *run,
		      bool confirmed)
{
	bool reinstall = same_image(&j->to, &j->from);
	bool is_to = same_image(run, &j->to);
	/* A swapped-in test image is unconfirmed: that is how a reinstall shows the swap. */
	bool new_runs = is_to && (!reinstall || !confirmed);

	switch (j->stage) {
	case SYSTEM_UPDATE_STAGE_PREPARING:
		finish(j, SYSTEM_UPDATE_INTERRUPTED, "boot_changed", MSG_INTERRUPTED);
		break;
	case SYSTEM_UPDATE_STAGE_REQUESTING:
		pf->upload_consumed(pf->ctx, j->upload_id);
		if (new_runs) {
			first_boot(j, confirmed);
		} else {
			finish(j, SYSTEM_UPDATE_INTERRUPTED, "boot_changed", MSG_INTERRUPTED);
		}
		break;
	case SYSTEM_UPDATE_STAGE_REBOOTING:
		pf->upload_consumed(pf->ctx, j->upload_id);
		if (new_runs) {
			first_boot(j, confirmed);
		} else {
			finish(j, SYSTEM_UPDATE_FAILED, "internal_error", MSG_NOT_UP);
		}
		break;
	case SYSTEM_UPDATE_STAGE_BOOTED:
		if (!is_to) {
			finish(j, SYSTEM_UPDATE_ROLLED_BACK, "boot_changed", MSG_ROLLED_BACK);
		} else if (confirmed) {
			finish(j, SYSTEM_UPDATE_SUCCEEDED, NULL, NULL);
		} else {
			first_boot(j, false);
		}
		break;
	default:
		break;
	}
}

static bool terminated(const char *s, size_t cap)
{
	return memchr(s, '\0', cap) != NULL;
}

static bool journal_valid(const struct system_update_journal *j)
{
	return j->format == SYSTEM_UPDATE_JOURNAL_FORMAT &&
	       (unsigned int)j->stage < SYSTEM_UPDATE_STAGE_COUNT &&
	       (unsigned int)j->last.state < SYSTEM_UPDATE_OUTCOME_COUNT &&
	       terminated(j->job_id, sizeof(j->job_id)) &&
	       terminated(j->upload_id, sizeof(j->upload_id)) &&
	       terminated(j->last.job_id, sizeof(j->last.job_id)) &&
	       terminated(j->last.error_code, sizeof(j->last.error_code)) &&
	       terminated(j->last.error_message, sizeof(j->last.error_message));
}

int system_updater_init(const struct system_updater_platform *platform)
{
	struct system_update_journal j;
	struct system_image run = {0};
	bool confirmed = false;
	bool known;
	bool changed = false;
	bool pending_swap;
	int64_t now;

	if (platform == NULL || platform->running_image == NULL ||
	    platform->staged_image == NULL || platform->set_upload_in_use == NULL ||
	    platform->upload_consumed == NULL || platform->request_swap == NULL ||
	    platform->swap_pending == NULL || platform->confirm == NULL ||
	    platform->reboot == NULL || platform->journal_load == NULL ||
	    platform->journal_save == NULL || platform->now_ms == NULL ||
	    platform->sleep_ms == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);
	if (running_now) {
		k_mutex_unlock(&lock);
		return -EBUSY;
	}
	pf = platform;
	active = false;
	pending = false;
	confirm_armed = false;
	confirm_backoff = 0U;
	k_mutex_unlock(&lock);

	memset(&j, 0, sizeof(j));
	if (pf->journal_load(pf->ctx, &j) != 0 || !journal_valid(&j)) {
		memset(&j, 0, sizeof(j));
		j.format = SYSTEM_UPDATE_JOURNAL_FORMAT;
	}

	known = pf->running_image(pf->ctx, &run, &confirmed) == 0;
	if (!known) {
		/* Nothing to compare with: leave the journal for a start that can read. */
		LOG_ERR("the running image could not be read; no update is accepted");
	} else if (j.stage != SYSTEM_UPDATE_STAGE_NONE) {
		reconcile(&j, &run, confirmed);
		changed = true;
	}
	pending_swap = pf->swap_pending(pf->ctx);
	now = pf->now_ms(pf->ctx);

	k_mutex_lock(&lock, K_FOREVER);
	journal = j;
	running_known = known;
	running = run;
	running_confirmed = known && confirmed;
	swap_pending = pending_swap;
	if (known && !confirmed) {
		confirm_armed = true;
		confirm_deadline = now + (int64_t)CONFIG_SYSTEM_UPDATER_CONFIRM_SECONDS * 1000;
		confirm_retry_at = confirm_deadline;
	}
	k_mutex_unlock(&lock);

	if (known && !confirmed) {
		LOG_INF("the running image is not confirmed; it confirms itself in %u s",
			CONFIG_SYSTEM_UPDATER_CONFIRM_SECONDS);
	}
	if (changed) {
		(void)save(&j);
	}
	return 0;
}

int system_updater_check(void)
{
	int rc = 0;

	if (pf == NULL) {
		return -EAGAIN;
	}
	k_mutex_lock(&lock, K_FOREVER);
	if (active) {
		rc = -EBUSY;
	} else if (!running_known) {
		rc = -EIO;
	} else if (!running_confirmed || swap_pending) {
		rc = -EACCES;
	}
	k_mutex_unlock(&lock);
	return rc;
}

int system_updater_start(const char *upload_id, const char *job_id, bool acknowledge_downgrade)
{
	struct system_update_journal before;
	struct system_update_journal copy;
	struct system_image to;
	uint32_t size = 0;
	int rc;

	if (pf == NULL) {
		return -EAGAIN;
	}
	if (upload_id == NULL || job_id == NULL || upload_id[0] == '\0' || job_id[0] == '\0' ||
	    strlen(upload_id) > SYSTEM_UPDATE_UPLOAD_ID_MAX_LEN || strlen(job_id) > JOB_ID_MAX_LEN) {
		return -EINVAL;
	}
	rc = system_updater_check();
	if (rc != 0) {
		return rc;
	}
	rc = pf->staged_image(pf->ctx, upload_id, &to, &size);
	if (rc == -ENOENT) {
		return -ENOENT;
	} else if (rc != 0) {
		return -ENODATA;
	}

	k_mutex_lock(&lock, K_FOREVER);
	if (active) {
		/* Reserved under the lock, so two starts cannot both pass. */
		k_mutex_unlock(&lock);
		return -EBUSY;
	}
	if (system_image_version_cmp(&to.version, &running.version) < 0 && !acknowledge_downgrade) {
		k_mutex_unlock(&lock);
		return -EDOM;
	}
	before = journal;
	active = true;
	phase = SYSTEM_UPDATE_PREPARING;
	journal.stage = SYSTEM_UPDATE_STAGE_PREPARING;
	copy_str(journal.job_id, sizeof(journal.job_id), job_id);
	copy_str(journal.upload_id, sizeof(journal.upload_id), upload_id);
	journal.from = running;
	journal.to = to;
	copy = journal;
	k_mutex_unlock(&lock);

	rc = save(&copy);

	k_mutex_lock(&lock, K_FOREVER);
	if (rc != 0) {
		journal = before;
		active = false;
	} else {
		pending = true;
	}
	k_mutex_unlock(&lock);
	return rc != 0 ? -ENOSPC : 0;
}

/* -- the run ---------------------------------------------------------------------- */

struct run {
	char job_id[JOB_ID_MAX_LEN + 1];
	char upload_id[SYSTEM_UPDATE_UPLOAD_ID_MAX_LEN + 1];
	struct system_image from;
	struct system_image to;
	enum system_update_phase phase;
};

struct run_error {
	const char *code;
	const char *message;
	bool retryable;
};

static int enter(struct run *r, enum system_update_phase p, enum system_update_stage stage)
{
	struct system_update_journal copy;

	r->phase = p;
	(void)job_set_phase(r->job_id, phase_names[p]);

	k_mutex_lock(&lock, K_FOREVER);
	phase = p;
	journal.stage = stage;
	copy = journal;
	k_mutex_unlock(&lock);
	LOG_INF("update %s: %s", r->job_id, phase_names[p]);
	return save(&copy);
}

static bool cancelled(const struct run *r)
{
	struct job_snapshot snap;

	return job_get(r->job_id, &snap) != 0 || snap.state == JOB_STATE_CANCELLED;
}

static void upload_error(int rc, struct run_error *err)
{
	if (rc == -ENOENT) {
		err->code = "not_found";
		err->message = "The staged image no longer exists";
	} else {
		err->code = "invalid_state";
		err->message = "The staged image is not ready to install";
	}
	err->retryable = false;
}

void system_updater_run(void)
{
	struct run r = {0};
	struct run_error err = {0};
	struct system_update_journal copy;
	struct system_image img;
	bool confirmed = false;
	bool in_use = false;
	bool pending_swap;
	uint32_t size = 0;
	int rc;

	k_mutex_lock(&lock, K_FOREVER);
	if (pf == NULL || !pending) {
		k_mutex_unlock(&lock);
		return;
	}
	pending = false;
	running_now = true;
	copy_str(r.job_id, sizeof(r.job_id), journal.job_id);
	copy_str(r.upload_id, sizeof(r.upload_id), journal.upload_id);
	r.from = journal.from;
	r.to = journal.to;
	k_mutex_unlock(&lock);

	if (job_set_state(r.job_id, JOB_STATE_RUNNING) != 0) {
		/* Cancelled while queued. */
		goto cancelled;
	}

	/* preparing */
	(void)enter(&r, SYSTEM_UPDATE_PREPARING, SYSTEM_UPDATE_STAGE_PREPARING);
	if (cancelled(&r)) {
		goto cancelled;
	}
	rc = pf->set_upload_in_use(pf->ctx, r.upload_id, true);
	if (rc != 0) {
		upload_error(rc, &err);
		goto failed;
	}
	in_use = true;
	rc = pf->staged_image(pf->ctx, r.upload_id, &img, &size);
	if (rc != 0) {
		upload_error(rc, &err);
		goto failed;
	}
	if (!same_image(&img, &r.to)) {
		err.code = "invalid_state";
		err.message = "The staged image changed since the install was accepted";
		err.retryable = false;
		goto failed;
	}
	rc = pf->running_image(pf->ctx, &img, &confirmed);
	if (rc != 0) {
		err.code = "internal_error";
		err.message = "The running image could not be read";
		err.retryable = true;
		goto failed;
	}
	if (!confirmed || !same_image(&img, &r.from)) {
		err.code = "invalid_state";
		err.message = "The running firmware changed since the install was accepted";
		err.retryable = false;
		goto failed;
	}

	/* The point of no return: a cancel that landed first is reported here. */
	if (job_set_cancellable(r.job_id, false) != 0) {
		goto cancelled;
	}

	/* requesting: the journal must be on storage before the trailer changes */
	if (enter(&r, SYSTEM_UPDATE_REQUESTING, SYSTEM_UPDATE_STAGE_REQUESTING) != 0) {
		err.code = "internal_error";
		err.message = "The update journal could not be saved";
		err.retryable = true;
		goto failed;
	}
	rc = pf->request_swap(pf->ctx);
	if (rc != 0) {
		err.code = "internal_error";
		err.message = "MCUboot's swap request could not be written";
		err.retryable = true;
		goto failed;
	}
	if (!pf->swap_pending(pf->ctx)) {
		err.code = "internal_error";
		err.message = "MCUboot did not record the swap request";
		err.retryable = true;
		goto failed;
	}
	k_mutex_lock(&lock, K_FOREVER);
	swap_pending = true;
	k_mutex_unlock(&lock);

	/* rebooting: the request is on flash; a lost journal here still reads right */
	(void)enter(&r, SYSTEM_UPDATE_REBOOTING, SYSTEM_UPDATE_STAGE_REBOOTING);
	pf->sleep_ms(pf->ctx, CONFIG_SYSTEM_UPDATER_REBOOT_DELAY_MS);
	k_mutex_lock(&lock, K_FOREVER);
	running_now = false;
	k_mutex_unlock(&lock);
	LOG_INF("update %s: restarting into the swap", r.job_id);
	pf->reboot(pf->ctx);
	return;

failed:
	if (in_use) {
		(void)pf->set_upload_in_use(pf->ctx, r.upload_id, false);
	}
	/* A request that failed half way may still have left the trailer set. */
	pending_swap = r.phase >= SYSTEM_UPDATE_REQUESTING ? pf->swap_pending(pf->ctx) : false;
	(void)job_fail(r.job_id, err.code, err.retryable);
	k_mutex_lock(&lock, K_FOREVER);
	set_last(&journal, SYSTEM_UPDATE_FAILED, err.code, err.message, err.retryable);
	journal.stage = SYSTEM_UPDATE_STAGE_NONE;
	if (r.phase >= SYSTEM_UPDATE_REQUESTING) {
		swap_pending = pending_swap;
	}
	active = false;
	running_now = false;
	copy = journal;
	k_mutex_unlock(&lock);
	(void)save(&copy);
	LOG_ERR("update %s failed in %s: %s", r.job_id, phase_names[r.phase], err.message);
	return;

cancelled:
	if (in_use) {
		(void)pf->set_upload_in_use(pf->ctx, r.upload_id, false);
	}
	/* Nothing on flash changed: last_update stays what it was. */
	k_mutex_lock(&lock, K_FOREVER);
	journal.stage = SYSTEM_UPDATE_STAGE_NONE;
	active = false;
	running_now = false;
	copy = journal;
	k_mutex_unlock(&lock);
	(void)save(&copy);
	LOG_INF("update %s cancelled before requesting", r.job_id);
}

/* -- confirmation ------------------------------------------------------------------ */

static int confirm(int64_t now)
{
	struct system_update_journal copy;
	struct system_image img;
	bool confirmed = false;
	bool changed = false;
	bool ok;
	int rc;
	int read_rc;

	k_mutex_lock(&confirm_lock, K_FOREVER);
	rc = pf->confirm(pf->ctx);
	read_rc = pf->running_image(pf->ctx, &img, &confirmed);
	ok = rc == 0 && read_rc == 0 && confirmed;

	k_mutex_lock(&lock, K_FOREVER);
	if (ok) {
		running = img;
		running_known = true;
		running_confirmed = true;
		confirm_armed = false;
		confirm_backoff = 0U;
		if (journal.stage == SYSTEM_UPDATE_STAGE_BOOTED) {
			finish(&journal, SYSTEM_UPDATE_SUCCEEDED, NULL, NULL);
			changed = true;
		}
	} else {
		confirm_backoff = confirm_backoff == 0U
					  ? CONFIG_SYSTEM_UPDATER_CONFIRM_RETRY_MS
					  : MIN(confirm_backoff * 2U,
						(uint32_t)CONFIG_SYSTEM_UPDATER_CONFIRM_RETRY_MAX_MS);
		confirm_retry_at = now + confirm_backoff;
	}
	copy = journal;
	k_mutex_unlock(&lock);

	if (changed) {
		(void)save(&copy);
	}
	k_mutex_unlock(&confirm_lock);

	if (!ok) {
		LOG_ERR("confirming the running image failed (%d, read %d, confirmed %d)", rc,
			read_rc, confirmed);
		return -EIO;
	}
	LOG_INF("the running image is confirmed");
	return 0;
}

void system_updater_tick(int64_t now_ms)
{
	bool due;

	if (pf == NULL) {
		return;
	}
	k_mutex_lock(&lock, K_FOREVER);
	due = confirm_armed && now_ms >= confirm_deadline && now_ms >= confirm_retry_at;
	k_mutex_unlock(&lock);
	if (due) {
		(void)confirm(now_ms);
	}
}

int system_updater_confirm_now(void)
{
	bool known;
	bool confirmed;

	if (pf == NULL) {
		return -EAGAIN;
	}
	k_mutex_lock(&lock, K_FOREVER);
	known = running_known;
	confirmed = running_confirmed;
	k_mutex_unlock(&lock);
	if (!known) {
		return -EIO;
	}
	if (confirmed) {
		return 0;
	}
	return confirm(pf->now_ms(pf->ctx));
}

void system_updater_get_state(struct system_updater_state *out)
{
	int64_t now;

	memset(out, 0, sizeof(*out));
	if (pf == NULL) {
		return;
	}
	now = pf->now_ms(pf->ctx);

	k_mutex_lock(&lock, K_FOREVER);
	out->active = active;
	out->phase = phase;
	if (active) {
		copy_str(out->job_id, sizeof(out->job_id), journal.job_id);
		copy_str(out->upload_id, sizeof(out->upload_id), journal.upload_id);
	}
	out->running_known = running_known;
	out->running = running;
	out->running_confirmed = running_confirmed;
	out->confirm_pending = confirm_armed;
	if (confirm_armed && confirm_deadline > now) {
		out->confirm_remaining_seconds = (uint32_t)((confirm_deadline - now + 999) / 1000);
	}
	out->swap_pending = swap_pending;
	out->has_last = journal.has_last;
	out->last = journal.last;
	k_mutex_unlock(&lock);
}
