/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Job manager unit tests. Everything here runs on native_sim: the module has
 * no hardware dependency, and its clock is injected so retention and expiry
 * are tested without sleeping.
 */

#include <zephyr/ztest.h>

#include <job_manager/job_manager.h>

/* Injected clock. Tests move time explicitly; nothing here waits. */
static int64_t fake_now;

static int64_t fake_clock(void)
{
	return fake_now;
}

static void advance_ms(int64_t ms)
{
	fake_now += ms;
}

static void *suite_setup(void)
{
	job_manager_set_clock(fake_clock);
	return NULL;
}

static void case_before(void *fixture)
{
	ARG_UNUSED(fixture);
	fake_now = 1000;
	job_manager_init();
}

ZTEST_SUITE(job_manager, NULL, suite_setup, case_before, NULL, NULL);

static enum job_create_result create(const char *key, uint32_t hash, bool cancellable,
				     struct job_snapshot *out)
{
	const struct job_create_params p = {
		.kind = JOB_KIND_WIFI_SCAN,
		.cancellable = cancellable,
		.idempotency_key = key,
		.request_hash = hash,
	};

	return job_create(&p, out);
}

ZTEST(job_manager, test_create_assigns_queued_job)
{
	struct job_snapshot s;

	zassert_equal(create("key-a", 1, true, &s), JOB_CREATE_NEW);
	zassert_equal(s.state, JOB_STATE_QUEUED);
	zassert_equal(s.kind, JOB_KIND_WIFI_SCAN);
	zassert_true(s.cancellable);
	zassert_false(s.compact);
	zassert_equal(s.created_uptime_ms, 1000);
	zassert_true(strlen(s.id) > 0, "job id must not be empty");
	zassert_equal(job_active_count(), 1);
}

ZTEST(job_manager, test_ids_are_unique)
{
	struct job_snapshot a, b;

	zassert_equal(create(NULL, 0, false, &a), JOB_CREATE_NEW);
	zassert_equal(create(NULL, 0, false, &b), JOB_CREATE_NEW);
	zassert_not_equal(strcmp(a.id, b.id), 0, "ids must differ");
}

/* The central promise of the contract: a retry must not act twice. */
ZTEST(job_manager, test_same_key_same_body_returns_same_job)
{
	struct job_snapshot first, again;

	zassert_equal(create("key-a", 0xAABB, false, &first), JOB_CREATE_NEW);
	zassert_equal(create("key-a", 0xAABB, false, &again), JOB_CREATE_EXISTING);
	zassert_equal(strcmp(first.id, again.id), 0);
	zassert_equal(job_active_count(), 1, "no second job may be created");
}

ZTEST(job_manager, test_same_key_different_body_conflicts)
{
	struct job_snapshot s;

	zassert_equal(create("key-a", 0xAABB, false, &s), JOB_CREATE_NEW);
	zassert_equal(create("key-a", 0xCCDD, false, NULL), JOB_CREATE_CONFLICT);
	zassert_equal(job_active_count(), 1);
}

ZTEST(job_manager, test_no_key_never_deduplicates)
{
	struct job_snapshot a, b;

	zassert_equal(create(NULL, 0x1234, false, &a), JOB_CREATE_NEW);
	zassert_equal(create(NULL, 0x1234, false, &b), JOB_CREATE_NEW);
	zassert_equal(job_active_count(), 2);
}

ZTEST(job_manager, test_legal_state_path)
{
	struct job_snapshot s;

	zassert_equal(create("k", 1, false, &s), JOB_CREATE_NEW);
	zassert_ok(job_set_state(s.id, JOB_STATE_RUNNING));
	zassert_ok(job_set_state(s.id, JOB_STATE_WAITING_CONFIRMATION));
	zassert_ok(job_set_state(s.id, JOB_STATE_SUCCEEDED));

	zassert_ok(job_get(s.id, &s));
	zassert_equal(s.state, JOB_STATE_SUCCEEDED);
	zassert_equal(job_active_count(), 0);
}

ZTEST(job_manager, test_illegal_transitions_rejected)
{
	struct job_snapshot s;

	zassert_equal(create("k", 1, false, &s), JOB_CREATE_NEW);

	/* Skipping RUNNING is not allowed. */
	zassert_equal(job_set_state(s.id, JOB_STATE_WAITING_CONFIRMATION), -EINVAL);

	zassert_ok(job_set_state(s.id, JOB_STATE_RUNNING));
	zassert_ok(job_set_state(s.id, JOB_STATE_SUCCEEDED));

	/* Terminal is final, in every direction. */
	zassert_equal(job_set_state(s.id, JOB_STATE_RUNNING), -EINVAL);
	zassert_equal(job_set_state(s.id, JOB_STATE_FAILED), -EINVAL);
}

ZTEST(job_manager, test_failure_carries_its_reason)
{
	struct job_snapshot s;

	zassert_equal(create("k", 1, false, &s), JOB_CREATE_NEW);
	zassert_ok(job_set_state(s.id, JOB_STATE_RUNNING));
	zassert_ok(job_fail(s.id, "storage_full", false));

	zassert_ok(job_get(s.id, &s));
	zassert_equal(s.state, JOB_STATE_FAILED);
	zassert_true(s.has_error);
	zassert_equal(strcmp(s.error.code, "storage_full"), 0);
	zassert_false(s.error.retryable);

	/* A failure without a reason must be impossible to record. */
	zassert_equal(job_fail(s.id, NULL, false), -EINVAL);
}

ZTEST(job_manager, test_phase_change_resets_progress)
{
	struct job_snapshot s;

	zassert_equal(create("k", 1, false, &s), JOB_CREATE_NEW);
	zassert_ok(job_set_state(s.id, JOB_STATE_RUNNING));
	zassert_ok(job_set_phase(s.id, "writing"));
	zassert_ok(job_set_progress(s.id, 4096, 8192, true, JOB_PROGRESS_UNIT_BYTES));

	zassert_ok(job_get(s.id, &s));
	zassert_equal(s.progress.completed, 4096);
	zassert_true(s.progress.total_known);

	zassert_ok(job_set_phase(s.id, "verifying"));
	zassert_ok(job_get(s.id, &s));
	zassert_equal(strcmp(s.phase, "verifying"), 0);
	zassert_equal(s.progress.completed, 0, "progress is per phase");
	zassert_false(s.progress.total_known);
}

ZTEST(job_manager, test_unknown_total_is_reported_as_unknown)
{
	struct job_snapshot s;

	zassert_equal(create("k", 1, false, &s), JOB_CREATE_NEW);
	zassert_ok(job_set_state(s.id, JOB_STATE_RUNNING));
	zassert_ok(job_set_progress(s.id, 10, 0, false, JOB_PROGRESS_UNIT_RECORDS));

	zassert_ok(job_get(s.id, &s));
	zassert_equal(s.progress.completed, 10);
	zassert_false(s.progress.total_known, "serialises as total: null");
}

ZTEST(job_manager, test_cancel_only_when_cancellable)
{
	struct job_snapshot yes, no;

	zassert_equal(create("a", 1, true, &yes), JOB_CREATE_NEW);
	zassert_equal(create("b", 2, false, &no), JOB_CREATE_NEW);

	zassert_equal(job_cancel(no.id), -EPERM, "destructive work refuses cancel");
	zassert_ok(job_cancel(yes.id));

	zassert_ok(job_get(yes.id, &yes));
	zassert_equal(yes.state, JOB_STATE_CANCELLED);

	/* Already terminal. */
	zassert_equal(job_cancel(yes.id), -EINVAL);
	zassert_equal(job_cancel("job_nope"), -ENOENT);
}

ZTEST(job_manager, test_unknown_id_is_not_found)
{
	struct job_snapshot s;

	zassert_equal(job_get("job_absent", &s), -ENOENT);
	zassert_equal(job_set_state("job_absent", JOB_STATE_RUNNING), -ENOENT);
	zassert_equal(job_set_phase("job_absent", "x"), -ENOENT);
}

/*
 * Pool is 4 in this build. Fill it with terminal jobs, then create more: the
 * oldest must be retired into the compact tier, not forgotten.
 */
ZTEST(job_manager, test_evicted_job_survives_as_compact)
{
	struct job_snapshot first, s;
	char first_id[JOB_ID_MAX_LEN + 1];

	zassert_equal(create("k0", 0, false, &first), JOB_CREATE_NEW);
	strcpy(first_id, first.id);
	zassert_ok(job_set_state(first_id, JOB_STATE_RUNNING));
	zassert_ok(job_fail(first_id, "boom", true));

	for (int i = 1; i < 8; i++) {
		char key[8];

		advance_ms(10);
		snprintf(key, sizeof(key), "k%d", i);
		zassert_equal(create(key, i, false, &s), JOB_CREATE_NEW);
		zassert_ok(job_set_state(s.id, JOB_STATE_RUNNING));
		zassert_ok(job_set_state(s.id, JOB_STATE_SUCCEEDED));
	}

	/* The full record is gone, but the outcome is still truthful. */
	zassert_ok(job_get(first_id, &s));
	zassert_true(s.compact, "expected the compact tier to answer");
	zassert_equal(s.state, JOB_STATE_FAILED);
	zassert_equal(strcmp(s.error.code, "boom"), 0);

	/* And a retry still must not act twice. */
	zassert_equal(create("k0", 0, false, &s), JOB_CREATE_EXISTING);
	zassert_equal(strcmp(s.id, first_id), 0);
}

ZTEST(job_manager, test_active_jobs_are_never_evicted)
{
	struct job_snapshot s[4];

	for (int i = 0; i < 4; i++) {
		char key[8];

		snprintf(key, sizeof(key), "a%d", i);
		zassert_equal(create(key, i, false, &s[i]), JOB_CREATE_NEW);
		zassert_ok(job_set_state(s[i].id, JOB_STATE_RUNNING));
	}

	/* Nothing terminal to reclaim: the caller must get a clear refusal. */
	zassert_equal(create("a4", 4, false, NULL), JOB_CREATE_EXHAUSTED);

	for (int i = 0; i < 4; i++) {
		struct job_snapshot got;

		zassert_ok(job_get(s[i].id, &got));
		zassert_equal(got.state, JOB_STATE_RUNNING);
	}
}

ZTEST(job_manager, test_retention_expires_compact_records)
{
	struct job_snapshot first, s;
	char first_id[JOB_ID_MAX_LEN + 1];

	zassert_equal(create("k0", 0, false, &first), JOB_CREATE_NEW);
	strcpy(first_id, first.id);
	zassert_ok(job_set_state(first_id, JOB_STATE_RUNNING));
	zassert_ok(job_set_state(first_id, JOB_STATE_SUCCEEDED));

	for (int i = 1; i < 8; i++) {
		char key[8];

		snprintf(key, sizeof(key), "k%d", i);
		zassert_equal(create(key, i, false, &s), JOB_CREATE_NEW);
		zassert_ok(job_set_state(s.id, JOB_STATE_RUNNING));
		zassert_ok(job_set_state(s.id, JOB_STATE_SUCCEEDED));
	}
	zassert_ok(job_get(first_id, &s), "still retained just after eviction");

	/* Contract floor is 900 s; step past it. */
	advance_ms(901 * 1000);
	zassert_equal(job_get(first_id, &s), -ENOENT, "retention must expire");

	/* With the record gone the key is free again, and reusing it starts a
	 * genuinely new job rather than resurrecting the old one.
	 */
	zassert_equal(create("k0", 0, false, &s), JOB_CREATE_NEW);
	zassert_not_equal(strcmp(s.id, first_id), 0);
}

ZTEST(job_manager, test_rejects_bad_arguments)
{
	struct job_create_params p = {
		.kind = JOB_KIND_COUNT,
		.idempotency_key = NULL,
	};

	zassert_equal(job_create(&p, NULL), JOB_CREATE_INVALID);
	zassert_equal(job_create(NULL, NULL), JOB_CREATE_INVALID);

	p.kind = JOB_KIND_WIFI_SCAN;
	p.idempotency_key = "";
	zassert_equal(job_create(&p, NULL), JOB_CREATE_INVALID, "empty key");
}

/* Wire names are part of the HTTP contract; a typo here is a broken API. */
ZTEST(job_manager, test_wire_names_match_the_contract)
{
	zassert_equal(strcmp(job_kind_str(JOB_KIND_COPROCESSOR_UPDATE), "coprocessor_update"), 0);
	zassert_equal(strcmp(job_kind_str(JOB_KIND_NETWORK_APPLY), "network_apply"), 0);
	zassert_equal(strcmp(job_state_str(JOB_STATE_WAITING_CONFIRMATION),
			     "waiting_confirmation"), 0);
	zassert_equal(strcmp(job_state_str(JOB_STATE_INTERRUPTED), "interrupted"), 0);
	zassert_equal(strcmp(job_progress_unit_str(JOB_PROGRESS_UNIT_BYTES), "bytes"), 0);
	zassert_is_null(job_progress_unit_str(JOB_PROGRESS_UNIT_NONE));

	for (int k = 0; k < JOB_KIND_COUNT; k++) {
		zassert_not_null(job_kind_str(k), "kind %d has no wire name", k);
	}
	for (int s = 0; s < JOB_STATE_COUNT; s++) {
		zassert_not_null(job_state_str(s), "state %d has no wire name", s);
	}
}

/* -- listing and lookup, for web-api ------------------------------------- */

ZTEST(job_manager, test_active_ids)
{
	struct job_snapshot a, b, c;
	char ids[2][JOB_ID_MAX_LEN + 1];

	zassert_equal(job_active_ids(ids, ARRAY_SIZE(ids)), 0);
	zassert_equal(create(NULL, 0, false, &a), JOB_CREATE_NEW);
	zassert_equal(create(NULL, 0, false, &b), JOB_CREATE_NEW);
	zassert_equal(create(NULL, 0, false, &c), JOB_CREATE_NEW);
	zassert_ok(job_set_state(b.id, JOB_STATE_SUCCEEDED));

	memset(ids, 0, sizeof(ids));
	zassert_equal(job_active_ids(ids, ARRAY_SIZE(ids)), 2, "terminal jobs are not active");
	zassert_true((strcmp(ids[0], a.id) == 0 && strcmp(ids[1], c.id) == 0) ||
		     (strcmp(ids[0], c.id) == 0 && strcmp(ids[1], a.id) == 0));

	memset(ids, 0, sizeof(ids));
	zassert_equal(job_active_ids(ids, 1), 2, "the total, even when fewer fit");
	zassert_true(ids[0][0] != '\0');
	zassert_equal(ids[1][0], '\0', "only max are copied");
	zassert_equal(job_active_ids(NULL, 0), 2);
}

ZTEST(job_manager, test_find_by_key)
{
	struct job_snapshot s, found;

	zassert_equal(job_find_by_key("key-a", 1, &found), JOB_LOOKUP_NONE);
	zassert_equal(job_active_count(), 0, "looking does not create");
	zassert_equal(job_find_by_key(NULL, 1, &found), JOB_LOOKUP_NONE);
	zassert_equal(job_find_by_key("", 1, &found), JOB_LOOKUP_NONE);

	zassert_equal(create("key-a", 7, false, &s), JOB_CREATE_NEW);
	zassert_equal(job_find_by_key("key-a", 7, &found), JOB_LOOKUP_EXISTING);
	zassert_str_equal(found.id, s.id);
	zassert_equal(job_find_by_key("key-a", 8, &found), JOB_LOOKUP_CONFLICT);
	zassert_equal(job_find_by_key("key-b", 7, NULL), JOB_LOOKUP_NONE);
	zassert_equal(job_find_by_key("key-a", 7, NULL), JOB_LOOKUP_EXISTING, "out may be NULL");
}

ZTEST(job_manager, test_find_by_key_in_compact_tier_and_after_retention)
{
	struct job_snapshot first, found;

	zassert_equal(create("key-old", 3, false, &first), JOB_CREATE_NEW);
	zassert_ok(job_set_state(first.id, JOB_STATE_SUCCEEDED));
	/* Push it out of the full records. */
	for (int i = 0; i < CONFIG_JOB_MANAGER_MAX_JOBS; i++) {
		struct job_snapshot s;

		zassert_equal(create(NULL, 0, false, &s), JOB_CREATE_NEW);
		zassert_ok(job_set_state(s.id, JOB_STATE_SUCCEEDED));
		advance_ms(1);
	}
	zassert_equal(job_find_by_key("key-old", 3, &found), JOB_LOOKUP_EXISTING);
	zassert_true(found.compact);
	zassert_str_equal(found.id, first.id);
	zassert_equal(job_find_by_key("key-old", 4, &found), JOB_LOOKUP_CONFLICT);

	advance_ms((int64_t)CONFIG_JOB_MANAGER_RETENTION_SECONDS * 1000 + 1000);
	zassert_equal(job_find_by_key("key-old", 3, &found), JOB_LOOKUP_NONE,
		      "past retention the key is free again");
}

ZTEST(job_manager, test_scoped_key_length)
{
	char key[JOB_IDEMPOTENCY_KEY_MAX_LEN + 2];
	struct job_snapshot s;

	/* web-api's scoped key: 16 hex digits, a colon, a 64-character key. */
	zassert_true(JOB_IDEMPOTENCY_KEY_MAX_LEN >= 16 + 1 + 64);
	memset(key, 'k', JOB_IDEMPOTENCY_KEY_MAX_LEN);
	key[JOB_IDEMPOTENCY_KEY_MAX_LEN] = '\0';
	zassert_equal(create(key, 1, false, &s), JOB_CREATE_NEW);
	zassert_equal(job_find_by_key(key, 1, NULL), JOB_LOOKUP_EXISTING,
		      "the full length is stored, not a prefix");
	key[JOB_IDEMPOTENCY_KEY_MAX_LEN] = 'k';
	key[JOB_IDEMPOTENCY_KEY_MAX_LEN + 1] = '\0';
	zassert_equal(create(key, 1, false, &s), JOB_CREATE_INVALID);
}
