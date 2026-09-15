/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Job manager implementation. See include/job_manager/job_manager.h for the
 * contract; this file only documents how it is achieved.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <job_manager/job_manager.h>

/* Full records: the complete job, including phase and progress. */
struct job_record {
	bool used;
	char id[JOB_ID_MAX_LEN + 1];
	enum job_kind kind;
	enum job_state state;
	char phase[JOB_PHASE_MAX_LEN + 1];
	struct job_progress progress;
	bool cancellable;
	int64_t created_ms;
	int64_t updated_ms;
	struct job_error error;
	bool has_error;
	/* Zero when the job carries no idempotency key. */
	char key[JOB_IDEMPOTENCY_KEY_MAX_LEN + 1];
	uint32_t request_hash;
};

/*
 * Compact records outlive full ones. They exist so that a client retrying
 * after its full record was evicted still gets the original job and its
 * outcome, rather than triggering the action a second time. They are much
 * smaller, so many more fit.
 */
struct job_compact {
	bool used;
	char id[JOB_ID_MAX_LEN + 1];
	char key[JOB_IDEMPOTENCY_KEY_MAX_LEN + 1];
	uint32_t request_hash;
	enum job_kind kind;
	enum job_state state;
	struct job_error error;
	bool has_error;
	int64_t created_ms;
	int64_t retired_ms;
};

static struct job_record records[CONFIG_JOB_MANAGER_MAX_JOBS];
static struct job_compact compacts[CONFIG_JOB_MANAGER_DEDUPE_SLOTS];
static uint32_t id_counter;
static K_MUTEX_DEFINE(lock);

static int64_t default_clock(void)
{
	return k_uptime_get();
}

static int64_t (*clock_fn)(void) = default_clock;

static int64_t now_ms(void)
{
	return clock_fn();
}

void job_manager_set_clock(int64_t (*fn)(void))
{
	k_mutex_lock(&lock, K_FOREVER);
	clock_fn = (fn != NULL) ? fn : default_clock;
	k_mutex_unlock(&lock);
}

bool job_state_is_terminal(enum job_state state)
{
	switch (state) {
	case JOB_STATE_SUCCEEDED:
	case JOB_STATE_FAILED:
	case JOB_STATE_CANCELLED:
	case JOB_STATE_INTERRUPTED:
		return true;
	default:
		return false;
	}
}

static const char *const kind_names[JOB_KIND_COUNT] = {
	[JOB_KIND_MATTER_OPEN] = "matter_open",
	[JOB_KIND_MATTER_CLOSE] = "matter_close",
	[JOB_KIND_NETWORK_APPLY] = "network_apply",
	[JOB_KIND_NETWORK_DISCARD] = "network_discard",
	[JOB_KIND_WIFI_SCAN] = "wifi_scan",
	[JOB_KIND_UPLOAD_CHUNK] = "upload_chunk",
	[JOB_KIND_FIRMWARE_VERIFY] = "firmware_verify",
	[JOB_KIND_FIRMWARE_DELETE] = "firmware_delete",
	[JOB_KIND_COPROCESSOR_UPDATE] = "coprocessor_update",
	[JOB_KIND_PASSWORD_CHANGE] = "password_change",
	[JOB_KIND_SYSTEM_UPDATE] = "system_update",
};

static const char *const state_names[JOB_STATE_COUNT] = {
	[JOB_STATE_QUEUED] = "queued",
	[JOB_STATE_RUNNING] = "running",
	[JOB_STATE_WAITING_CONFIRMATION] = "waiting_confirmation",
	[JOB_STATE_SUCCEEDED] = "succeeded",
	[JOB_STATE_FAILED] = "failed",
	[JOB_STATE_CANCELLED] = "cancelled",
	[JOB_STATE_INTERRUPTED] = "interrupted",
};

const char *job_kind_str(enum job_kind kind)
{
	return (kind < JOB_KIND_COUNT) ? kind_names[kind] : NULL;
}

const char *job_state_str(enum job_state state)
{
	return (state < JOB_STATE_COUNT) ? state_names[state] : NULL;
}

const char *job_progress_unit_str(enum job_progress_unit unit)
{
	switch (unit) {
	case JOB_PROGRESS_UNIT_BYTES:
		return "bytes";
	case JOB_PROGRESS_UNIT_RECORDS:
		return "records";
	case JOB_PROGRESS_UNIT_PERCENT:
		return "percent";
	default:
		return NULL;
	}
}

static bool transition_allowed(enum job_state from, enum job_state to)
{
	if (job_state_is_terminal(from)) {
		return false;
	}
	if (job_state_is_terminal(to)) {
		return true;
	}

	switch (from) {
	case JOB_STATE_QUEUED:
		return to == JOB_STATE_RUNNING;
	case JOB_STATE_RUNNING:
		return to == JOB_STATE_WAITING_CONFIRMATION;
	case JOB_STATE_WAITING_CONFIRMATION:
		/* The confirmation arrived, or the rollback began: work again. */
		return to == JOB_STATE_RUNNING;
	default:
		return false;
	}
}

static struct job_record *find_record(const char *id)
{
	if (id == NULL) {
		return NULL;
	}
	for (size_t i = 0; i < ARRAY_SIZE(records); i++) {
		if (records[i].used && strcmp(records[i].id, id) == 0) {
			return &records[i];
		}
	}
	return NULL;
}

static struct job_compact *find_compact_by_id(const char *id)
{
	for (size_t i = 0; i < ARRAY_SIZE(compacts); i++) {
		if (compacts[i].used && strcmp(compacts[i].id, id) == 0) {
			return &compacts[i];
		}
	}
	return NULL;
}

/* Drop compact records past their retention window. Active jobs never get
 * here: only terminal ones are retired into this tier.
 */
static void expire_compacts(void)
{
	const int64_t cutoff = now_ms() - (int64_t)CONFIG_JOB_MANAGER_RETENTION_SECONDS * 1000;

	for (size_t i = 0; i < ARRAY_SIZE(compacts); i++) {
		if (compacts[i].used && compacts[i].retired_ms < cutoff) {
			memset(&compacts[i], 0, sizeof(compacts[i]));
		}
	}
}

static void retire_to_compact(const struct job_record *rec)
{
	struct job_compact *slot = NULL;
	int64_t oldest = INT64_MAX;

	for (size_t i = 0; i < ARRAY_SIZE(compacts); i++) {
		if (!compacts[i].used) {
			slot = &compacts[i];
			break;
		}
		if (compacts[i].retired_ms < oldest) {
			oldest = compacts[i].retired_ms;
			slot = &compacts[i];
		}
	}
	if (slot == NULL) {
		return;
	}

	memset(slot, 0, sizeof(*slot));
	slot->used = true;
	strncpy(slot->id, rec->id, sizeof(slot->id) - 1);
	strncpy(slot->key, rec->key, sizeof(slot->key) - 1);
	slot->request_hash = rec->request_hash;
	slot->kind = rec->kind;
	slot->state = rec->state;
	slot->error = rec->error;
	slot->has_error = rec->has_error;
	slot->created_ms = rec->created_ms;
	slot->retired_ms = now_ms();
}

/*
 * Free one full record by retiring the oldest terminal job. Active jobs are
 * never evicted — losing the identity of work still in flight is exactly the
 * failure the API contract forbids.
 */
static struct job_record *claim_record(void)
{
	struct job_record *victim = NULL;
	int64_t oldest = INT64_MAX;

	for (size_t i = 0; i < ARRAY_SIZE(records); i++) {
		if (!records[i].used) {
			return &records[i];
		}
	}
	for (size_t i = 0; i < ARRAY_SIZE(records); i++) {
		if (job_state_is_terminal(records[i].state) &&
		    records[i].updated_ms < oldest) {
			oldest = records[i].updated_ms;
			victim = &records[i];
		}
	}
	if (victim == NULL) {
		return NULL;
	}

	retire_to_compact(victim);
	memset(victim, 0, sizeof(*victim));
	return victim;
}

static void snapshot_from_record(const struct job_record *rec, struct job_snapshot *out)
{
	memset(out, 0, sizeof(*out));
	strncpy(out->id, rec->id, sizeof(out->id) - 1);
	out->kind = rec->kind;
	out->state = rec->state;
	strncpy(out->phase, rec->phase, sizeof(out->phase) - 1);
	out->progress = rec->progress;
	out->cancellable = rec->cancellable;
	out->created_uptime_ms = rec->created_ms;
	out->updated_uptime_ms = rec->updated_ms;
	out->error = rec->error;
	out->has_error = rec->has_error;
	out->compact = false;
}

static void snapshot_from_compact(const struct job_compact *c, struct job_snapshot *out)
{
	memset(out, 0, sizeof(*out));
	strncpy(out->id, c->id, sizeof(out->id) - 1);
	out->kind = c->kind;
	out->state = c->state;
	out->created_uptime_ms = c->created_ms;
	out->updated_uptime_ms = c->retired_ms;
	out->error = c->error;
	out->has_error = c->has_error;
	out->compact = true;
}

/* Find a key in either tier. A live record wins over a retired one: it is more
 * complete. Lock held.
 */
static enum job_lookup_result lookup_locked(const char *key, uint32_t request_hash,
					    struct job_snapshot *out)
{
	for (size_t i = 0; i < ARRAY_SIZE(records); i++) {
		if (!records[i].used || records[i].key[0] == '\0' ||
		    strcmp(records[i].key, key) != 0) {
			continue;
		}
		if (records[i].request_hash != request_hash) {
			return JOB_LOOKUP_CONFLICT;
		}
		if (out != NULL) {
			snapshot_from_record(&records[i], out);
		}
		return JOB_LOOKUP_EXISTING;
	}
	for (size_t i = 0; i < ARRAY_SIZE(compacts); i++) {
		if (!compacts[i].used || compacts[i].key[0] == '\0' ||
		    strcmp(compacts[i].key, key) != 0) {
			continue;
		}
		if (compacts[i].request_hash != request_hash) {
			return JOB_LOOKUP_CONFLICT;
		}
		if (out != NULL) {
			snapshot_from_compact(&compacts[i], out);
		}
		return JOB_LOOKUP_EXISTING;
	}

	return JOB_LOOKUP_NONE;
}

enum job_lookup_result job_find_by_key(const char *key, uint32_t request_hash,
				       struct job_snapshot *out)
{
	enum job_lookup_result res;

	if (key == NULL || key[0] == '\0') {
		return JOB_LOOKUP_NONE;
	}
	k_mutex_lock(&lock, K_FOREVER);
	expire_compacts();
	res = lookup_locked(key, request_hash, out);
	k_mutex_unlock(&lock);

	return res;
}

void job_manager_init(void)
{
	k_mutex_lock(&lock, K_FOREVER);
	memset(records, 0, sizeof(records));
	memset(compacts, 0, sizeof(compacts));
	id_counter = 0;
	k_mutex_unlock(&lock);
}

enum job_create_result job_create(const struct job_create_params *params,
				  struct job_snapshot *out)
{
	enum job_create_result res;

	if (params == NULL || params->kind >= JOB_KIND_COUNT) {
		return JOB_CREATE_INVALID;
	}
	if (params->idempotency_key != NULL &&
	    (params->idempotency_key[0] == '\0' ||
	     strlen(params->idempotency_key) > JOB_IDEMPOTENCY_KEY_MAX_LEN)) {
		return JOB_CREATE_INVALID;
	}

	k_mutex_lock(&lock, K_FOREVER);
	expire_compacts();

	if (params->idempotency_key != NULL) {
		switch (lookup_locked(params->idempotency_key, params->request_hash, out)) {
		case JOB_LOOKUP_EXISTING:
			res = JOB_CREATE_EXISTING;
			goto done;
		case JOB_LOOKUP_CONFLICT:
			res = JOB_CREATE_CONFLICT;
			goto done;
		default:
			break;
		}
	}

	struct job_record *rec = claim_record();

	if (rec == NULL) {
		/* Every slot holds work still in flight. Tell the caller to
		 * come back rather than dropping a live job on the floor.
		 */
		res = JOB_CREATE_EXHAUSTED;
		goto done;
	}

	memset(rec, 0, sizeof(*rec));
	rec->used = true;
	rec->kind = params->kind;
	rec->state = JOB_STATE_QUEUED;
	rec->cancellable = params->cancellable;
	rec->created_ms = now_ms();
	rec->updated_ms = rec->created_ms;
	rec->request_hash = params->request_hash;
	if (params->idempotency_key != NULL) {
		strncpy(rec->key, params->idempotency_key, sizeof(rec->key) - 1);
	}
	snprintf(rec->id, sizeof(rec->id), "job_%08x", ++id_counter);

	if (out != NULL) {
		snapshot_from_record(rec, out);
	}
	res = JOB_CREATE_NEW;

done:
	k_mutex_unlock(&lock);
	return res;
}

int job_set_state(const char *id, enum job_state state)
{
	int ret;

	if (state >= JOB_STATE_COUNT) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);
	struct job_record *rec = find_record(id);

	if (rec == NULL) {
		ret = -ENOENT;
	} else if (!transition_allowed(rec->state, state)) {
		ret = -EINVAL;
	} else {
		rec->state = state;
		rec->updated_ms = now_ms();
		if (job_state_is_terminal(state)) {
			rec->cancellable = false;
		}
		ret = 0;
	}
	k_mutex_unlock(&lock);
	return ret;
}

int job_fail(const char *id, const char *error_code, bool retryable)
{
	int ret;

	if (error_code == NULL || error_code[0] == '\0') {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);
	struct job_record *rec = find_record(id);

	if (rec == NULL) {
		ret = -ENOENT;
	} else if (!transition_allowed(rec->state, JOB_STATE_FAILED)) {
		ret = -EINVAL;
	} else {
		rec->state = JOB_STATE_FAILED;
		rec->cancellable = false;
		rec->has_error = true;
		rec->error.retryable = retryable;
		memset(rec->error.code, 0, sizeof(rec->error.code));
		strncpy(rec->error.code, error_code, sizeof(rec->error.code) - 1);
		rec->updated_ms = now_ms();
		ret = 0;
	}
	k_mutex_unlock(&lock);
	return ret;
}

int job_set_phase(const char *id, const char *phase)
{
	int ret;

	if (phase == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);
	struct job_record *rec = find_record(id);

	if (rec == NULL) {
		ret = -ENOENT;
	} else if (job_state_is_terminal(rec->state)) {
		ret = -EINVAL;
	} else {
		memset(rec->phase, 0, sizeof(rec->phase));
		strncpy(rec->phase, phase, sizeof(rec->phase) - 1);
		memset(&rec->progress, 0, sizeof(rec->progress));
		rec->updated_ms = now_ms();
		ret = 0;
	}
	k_mutex_unlock(&lock);
	return ret;
}

int job_set_progress(const char *id, uint64_t completed, uint64_t total,
		     bool total_known, enum job_progress_unit unit)
{
	int ret;

	k_mutex_lock(&lock, K_FOREVER);
	struct job_record *rec = find_record(id);

	if (rec == NULL) {
		ret = -ENOENT;
	} else if (job_state_is_terminal(rec->state)) {
		ret = -EINVAL;
	} else {
		rec->progress.completed = completed;
		rec->progress.total = total_known ? total : 0;
		rec->progress.total_known = total_known;
		rec->progress.unit = unit;
		rec->updated_ms = now_ms();
		ret = 0;
	}
	k_mutex_unlock(&lock);
	return ret;
}

int job_get(const char *id, struct job_snapshot *out)
{
	int ret = -ENOENT;

	if (id == NULL || out == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);
	expire_compacts();

	struct job_record *rec = find_record(id);

	if (rec != NULL) {
		snapshot_from_record(rec, out);
		ret = 0;
	} else {
		struct job_compact *c = find_compact_by_id(id);

		if (c != NULL) {
			snapshot_from_compact(c, out);
			ret = 0;
		}
	}
	k_mutex_unlock(&lock);
	return ret;
}

int job_set_cancellable(const char *id, bool cancellable)
{
	int ret;

	k_mutex_lock(&lock, K_FOREVER);
	struct job_record *rec = find_record(id);

	if (rec == NULL) {
		ret = -ENOENT;
	} else if (job_state_is_terminal(rec->state)) {
		ret = -EINVAL;
	} else {
		rec->cancellable = cancellable;
		rec->updated_ms = now_ms();
		ret = 0;
	}
	k_mutex_unlock(&lock);
	return ret;
}

int job_cancel(const char *id)
{
	int ret;

	k_mutex_lock(&lock, K_FOREVER);
	struct job_record *rec = find_record(id);

	if (rec == NULL) {
		ret = -ENOENT;
	} else if (job_state_is_terminal(rec->state)) {
		ret = -EINVAL;
	} else if (!rec->cancellable) {
		/* Cancelling a destructive phase is refused by design: the
		 * API contract says this is 409 invalid_state, not a way to
		 * yank power from a coprocessor mid-write.
		 */
		ret = -EPERM;
	} else {
		rec->state = JOB_STATE_CANCELLED;
		rec->cancellable = false;
		rec->updated_ms = now_ms();
		ret = 0;
	}
	k_mutex_unlock(&lock);
	return ret;
}

size_t job_active_count(void)
{
	size_t n = 0;

	k_mutex_lock(&lock, K_FOREVER);
	for (size_t i = 0; i < ARRAY_SIZE(records); i++) {
		if (records[i].used && !job_state_is_terminal(records[i].state)) {
			n++;
		}
	}
	k_mutex_unlock(&lock);
	return n;
}

size_t job_active_ids(char ids[][JOB_ID_MAX_LEN + 1], size_t max)
{
	size_t n = 0;

	k_mutex_lock(&lock, K_FOREVER);
	for (size_t i = 0; i < ARRAY_SIZE(records); i++) {
		if (!records[i].used || job_state_is_terminal(records[i].state)) {
			continue;
		}
		if (ids != NULL && n < max) {
			memcpy(ids[n], records[i].id, sizeof(records[i].id));
		}
		n++;
	}
	k_mutex_unlock(&lock);
	return n;
}
