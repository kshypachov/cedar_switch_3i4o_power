/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * system-updater unit tests.
 *
 * The platform is tests/fakes/fake_system_platform.c; job-manager is the real
 * one. A "restart" is fake_system_reboot_into() - the image MCUboot runs and
 * whether it is confirmed - followed by system_updater_init() over whatever
 * journal was last saved. The HTTP refusals that are the binding's (the upload
 * is not an STM32 one, a network transaction is active) are tested with it.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <job_manager/job_manager.h>
#include <system_updater/system_updater.h>

#include "fake_system_platform.h"

#define CONFIRM_MS ((int64_t)CONFIG_SYSTEM_UPDATER_CONFIRM_SECONDS * 1000)
#define RETRY_MS   ((int64_t)CONFIG_SYSTEM_UPDATER_CONFIRM_RETRY_MS)

static char job_id[JOB_ID_MAX_LEN + 1];
static struct system_image from_img;
static struct system_image to_img;

static void case_before(void *fixture)
{
	ARG_UNUSED(fixture);

	job_manager_init();
	fake_system_init();
	from_img = fake_system.running;
	to_img = fake_system.staged;
	zassert_ok(system_updater_init(&fake_system_platform));
	job_id[0] = '\0';
}

ZTEST_SUITE(system_updater, NULL, NULL, case_before, NULL, NULL);

/* --- helpers ------------------------------------------------------------ */

static void create_job(void)
{
	struct job_create_params p = {
		.kind = JOB_KIND_SYSTEM_UPDATE,
		.cancellable = true,
	};
	struct job_snapshot snap;

	zassert_equal(job_create(&p, &snap), JOB_CREATE_NEW);
	strcpy(job_id, snap.id);
}

static int start(bool ack)
{
	create_job();
	return system_updater_start("upload_1", job_id, ack);
}

static void start_and_run(void)
{
	zassert_ok(start(false));
	system_updater_run();
}

static struct job_snapshot job(void)
{
	struct job_snapshot snap;

	zassert_ok(job_get(job_id, &snap));
	return snap;
}

static struct system_updater_state state(void)
{
	struct system_updater_state s;

	system_updater_get_state(&s);
	return s;
}

static void reboot_into(const struct system_image *img, bool confirmed)
{
	fake_system_reboot_into(img, confirmed);
	zassert_ok(system_updater_init(&fake_system_platform));
}

static void assert_last(enum system_update_outcome outcome, const char *code)
{
	struct system_updater_state s = state();

	zassert_true(s.has_last);
	zassert_equal(s.last.state, outcome, "outcome %d", s.last.state);
	if (code == NULL) {
		zassert_false(s.last.has_error);
	} else {
		zassert_true(s.last.has_error);
		zassert_str_equal(s.last.error_code, code);
		zassert_true(s.last.error_message[0] != '\0');
	}
}

static void assert_job_failed(const char *code)
{
	struct job_snapshot snap = job();

	zassert_equal(snap.state, JOB_STATE_FAILED, "job state %d", snap.state);
	zassert_str_equal(snap.error.code, code);
}

/* Index of the first saved journal with @p stage, or -1. */
static int save_with_stage(enum system_update_stage stage)
{
	for (size_t i = 0; i < fake_system.save_count; i++) {
		if (fake_system.saves[i].stage == stage) {
			return (int)i;
		}
	}
	return -1;
}

/* --- init and check ----------------------------------------------------- */

ZTEST(system_updater, test_init_requires_every_function)
{
	static const size_t offsets[] = {
		offsetof(struct system_updater_platform, running_image),
		offsetof(struct system_updater_platform, staged_image),
		offsetof(struct system_updater_platform, set_upload_in_use),
		offsetof(struct system_updater_platform, upload_consumed),
		offsetof(struct system_updater_platform, request_swap),
		offsetof(struct system_updater_platform, swap_pending),
		offsetof(struct system_updater_platform, confirm),
		offsetof(struct system_updater_platform, reboot),
		offsetof(struct system_updater_platform, journal_load),
		offsetof(struct system_updater_platform, journal_save),
		offsetof(struct system_updater_platform, now_ms),
		offsetof(struct system_updater_platform, sleep_ms),
	};

	zassert_equal(system_updater_init(NULL), -EINVAL);
	for (size_t i = 0; i < ARRAY_SIZE(offsets); i++) {
		struct system_updater_platform p = fake_system_platform;

		memset((uint8_t *)&p + offsets[i], 0, sizeof(void *));
		zassert_equal(system_updater_init(&p), -EINVAL, "function %u", (unsigned int)i);
	}
}

ZTEST(system_updater, test_fresh_confirmed_image_accepts_an_install)
{
	struct system_updater_state s = state();

	zassert_ok(system_updater_check());
	zassert_false(s.active);
	zassert_true(s.running_known);
	zassert_true(s.running_confirmed);
	zassert_false(s.confirm_pending);
	zassert_false(s.swap_pending);
	zassert_false(s.has_last);
	zassert_mem_equal(s.running.hash, from_img.hash, 32);
	zassert_equal(s.running.version.minor, 0);
	zassert_equal(fake_system.count[FSP_SAVE], 0, "nothing to save");
}

ZTEST(system_updater, test_unconfirmed_image_without_journal_confirms_itself)
{
	struct system_updater_state s;
	int64_t t0 = fake_system.now_ms;

	fake_system.running_confirmed = false;
	zassert_ok(system_updater_init(&fake_system_platform));
	zassert_equal(system_updater_check(), -EACCES);
	s = state();
	zassert_true(s.confirm_pending);
	zassert_equal(s.confirm_remaining_seconds, CONFIG_SYSTEM_UPDATER_CONFIRM_SECONDS);

	system_updater_tick(t0 + CONFIRM_MS - 1);
	zassert_equal(fake_system.count[FSP_CONFIRM], 0);
	system_updater_tick(t0 + CONFIRM_MS);
	zassert_equal(fake_system.count[FSP_CONFIRM], 1);

	s = state();
	zassert_true(s.running_confirmed);
	zassert_false(s.confirm_pending);
	zassert_false(s.has_last, "no update was involved");
	zassert_ok(system_updater_check());
	zassert_equal(fake_system.count[FSP_SAVE], 0);
}

ZTEST(system_updater, test_remaining_seconds_round_up)
{
	fake_system.running_confirmed = false;
	zassert_ok(system_updater_init(&fake_system_platform));

	fake_system.now_ms += 500;
	zassert_equal(state().confirm_remaining_seconds, CONFIG_SYSTEM_UPDATER_CONFIRM_SECONDS);
	fake_system.now_ms += 1000;
	zassert_equal(state().confirm_remaining_seconds,
		      CONFIG_SYSTEM_UPDATER_CONFIRM_SECONDS - 1);
	fake_system.now_ms += CONFIRM_MS;
	zassert_equal(state().confirm_remaining_seconds, 0);
	zassert_true(state().confirm_pending);
}

ZTEST(system_updater, test_tick_without_a_deadline_does_nothing)
{
	system_updater_tick(INT64_MAX / 2);
	zassert_equal(fake_system.count[FSP_CONFIRM], 0);
	zassert_equal(fake_system.count[FSP_RUNNING], 1, "init only");
}

ZTEST(system_updater, test_unreadable_running_image_leaves_the_journal)
{
	start_and_run();
	fake_system.count[FSP_SAVE] = 0;

	fake_system.rc[FSP_RUNNING] = -EIO;
	fake_system_reboot_into(&to_img, false);
	zassert_ok(system_updater_init(&fake_system_platform));

	zassert_equal(fake_system.count[FSP_SAVE], 0);
	zassert_equal(fake_system.journal.stage, SYSTEM_UPDATE_STAGE_REBOOTING);
	zassert_equal(fake_system.count[FSP_CONSUMED], 0);
	zassert_false(state().running_known);
	zassert_false(state().confirm_pending);
	zassert_equal(system_updater_check(), -EIO);
	create_job();
	zassert_equal(system_updater_start("upload_2", job_id, false), -EIO);
	zassert_equal(system_updater_confirm_now(), -EIO);
	zassert_equal(fake_system.count[FSP_CONFIRM], 0);

	/* Readable again at the next start: reconciled then. */
	fake_system.rc[FSP_RUNNING] = 0;
	zassert_ok(system_updater_init(&fake_system_platform));
	assert_last(SYSTEM_UPDATE_AWAITING_CONFIRMATION, NULL);
}

ZTEST(system_updater, test_corrupt_journal_reads_as_none)
{
	struct system_update_journal j = {
		.format = SYSTEM_UPDATE_JOURNAL_FORMAT,
		.stage = SYSTEM_UPDATE_STAGE_REBOOTING,
	};

	j.from = from_img;
	j.to = to_img;
	strcpy(j.job_id, "job_x");
	strcpy(j.upload_id, "upload_x");

	for (int defect = 0; defect < 8; defect++) {
		struct system_update_journal bad = j;

		switch (defect) {
		case 0:
			bad.format = SYSTEM_UPDATE_JOURNAL_FORMAT + 1;
			break;
		case 1:
			bad.stage = SYSTEM_UPDATE_STAGE_COUNT;
			break;
		case 2:
			bad.last.state = SYSTEM_UPDATE_OUTCOME_COUNT;
			break;
		case 3:
			memset(bad.job_id, 'j', sizeof(bad.job_id));
			break;
		case 4:
			memset(bad.upload_id, 'u', sizeof(bad.upload_id));
			break;
		case 5:
			memset(bad.last.job_id, 'j', sizeof(bad.last.job_id));
			break;
		case 6:
			memset(bad.last.error_code, 'c', sizeof(bad.last.error_code));
			break;
		default:
			memset(bad.last.error_message, 'm', sizeof(bad.last.error_message));
			break;
		}
		fake_system.journal = bad;
		fake_system.has_journal = true;
		zassert_ok(system_updater_init(&fake_system_platform));
		zassert_false(state().has_last, "defect %d", defect);
		zassert_equal(fake_system.count[FSP_CONSUMED], 0, "defect %d", defect);
		zassert_equal(fake_system.count[FSP_SAVE], 0, "defect %d", defect);
		zassert_ok(system_updater_check(), "defect %d", defect);
	}

	/* The intact journal is reconciled. */
	fake_system.journal = j;
	zassert_ok(system_updater_init(&fake_system_platform));
	assert_last(SYSTEM_UPDATE_FAILED, "internal_error");
	zassert_str_equal(fake_system.consumed_id, "upload_x");
	zassert_str_equal(state().last.job_id, "job_x");
}

ZTEST(system_updater, test_journal_load_failure_reads_as_none)
{
	struct system_update_journal j = {
		.format = SYSTEM_UPDATE_JOURNAL_FORMAT,
		.stage = SYSTEM_UPDATE_STAGE_BOOTED,
	};

	j.from = from_img;
	j.to = to_img;
	fake_system.journal = j;
	fake_system.has_journal = true;
	fake_system.rc[FSP_LOAD] = -EIO;
	/* A valid journal whose read still reported an error is not trusted. */
	fake_system.load_fills_on_error = true;
	zassert_ok(system_updater_init(&fake_system_platform));
	zassert_false(state().has_last);
	zassert_ok(system_updater_check());
	zassert_equal(fake_system.count[FSP_SAVE], 0);
}

static int init_rc_in_hook;

static void init_from_hook(void *arg)
{
	ARG_UNUSED(arg);
	init_rc_in_hook = system_updater_init(&fake_system_platform);
}

ZTEST(system_updater, test_init_refused_while_running)
{
	init_rc_in_hook = 1;
	zassert_ok(start(false));
	fake_system.hook_call = FSP_IN_USE;
	fake_system.hook = init_from_hook;
	system_updater_run();
	zassert_equal(init_rc_in_hook, -EBUSY);
	zassert_equal(fake_system.count[FSP_REBOOT], 1, "the run went on");

	/* After the run returns, init works again. */
	zassert_ok(system_updater_init(&fake_system_platform));
}

/* --- start ------------------------------------------------------------------ */

ZTEST(system_updater, test_start_refuses_bad_ids)
{
	char long_upload[SYSTEM_UPDATE_UPLOAD_ID_MAX_LEN + 2];
	char long_job[JOB_ID_MAX_LEN + 2];

	memset(long_upload, 'u', sizeof(long_upload) - 1U);
	long_upload[sizeof(long_upload) - 1U] = '\0';
	memset(long_job, 'j', sizeof(long_job) - 1U);
	long_job[sizeof(long_job) - 1U] = '\0';

	create_job();
	zassert_equal(system_updater_start(NULL, job_id, false), -EINVAL);
	zassert_equal(system_updater_start("", job_id, false), -EINVAL);
	zassert_equal(system_updater_start("upload_1", NULL, false), -EINVAL);
	zassert_equal(system_updater_start("upload_1", "", false), -EINVAL);
	zassert_equal(system_updater_start(long_upload, job_id, false), -EINVAL);
	zassert_equal(system_updater_start("upload_1", long_job, false), -EINVAL);

	/* The longest ids are accepted. */
	long_upload[SYSTEM_UPDATE_UPLOAD_ID_MAX_LEN] = '\0';
	zassert_ok(system_updater_start(long_upload, job_id, false));
	zassert_str_equal(state().upload_id, long_upload);
	zassert_equal(fake_system.count[FSP_SAVE], 1);
}

ZTEST(system_updater, test_start_longest_job_id)
{
	char long_job[JOB_ID_MAX_LEN + 1];

	memset(long_job, 'j', JOB_ID_MAX_LEN);
	long_job[JOB_ID_MAX_LEN] = '\0';
	zassert_ok(system_updater_start("upload_1", long_job, false));
	zassert_str_equal(state().job_id, long_job);
}

ZTEST(system_updater, test_start_unknown_or_unready_upload)
{
	create_job();
	fake_system.rc[FSP_STAGED] = -ENOENT;
	zassert_equal(system_updater_start("upload_1", job_id, false), -ENOENT);
	fake_system.rc[FSP_STAGED] = -EINVAL;
	zassert_equal(system_updater_start("upload_1", job_id, false), -ENODATA);
	fake_system.rc[FSP_STAGED] = -EIO;
	zassert_equal(system_updater_start("upload_1", job_id, false), -ENODATA);
	zassert_equal(fake_system.count[FSP_SAVE], 0);
	zassert_false(state().active);
	zassert_ok(system_updater_check());
}

ZTEST(system_updater, test_second_start_is_busy)
{
	zassert_ok(start(false));
	zassert_equal(system_updater_check(), -EBUSY);
	zassert_true(state().active);
	zassert_equal(state().phase, SYSTEM_UPDATE_PREPARING);
	create_job();
	zassert_equal(system_updater_start("upload_2", job_id, false), -EBUSY);
	zassert_equal(fake_system.count[FSP_SAVE], 1);
}

ZTEST(system_updater, test_pending_swap_refuses)
{
	fake_system.swap_pending = true;
	zassert_ok(system_updater_init(&fake_system_platform));
	zassert_true(state().swap_pending);
	zassert_equal(system_updater_check(), -EACCES);
	zassert_equal(start(false), -EACCES);
	zassert_equal(fake_system.count[FSP_SAVE], 0);
}

ZTEST(system_updater, test_unconfirmed_image_refuses)
{
	fake_system.running_confirmed = false;
	zassert_ok(system_updater_init(&fake_system_platform));
	zassert_equal(start(false), -EACCES);
	zassert_equal(start(true), -EACCES);
}

ZTEST(system_updater, test_downgrade_needs_acknowledgement)
{
	fake_system.staged = fake_system_image(0, 9, 0, 0, 0xB1);
	zassert_equal(start(false), -EDOM);
	zassert_equal(fake_system.count[FSP_SAVE], 0);
	zassert_false(state().active);
	zassert_ok(system_updater_start("upload_1", job_id, true));
}

ZTEST(system_updater, test_downgrade_by_build_only)
{
	fake_system.running = fake_system_image(1, 0, 0, 6, 0xA0);
	zassert_ok(system_updater_init(&fake_system_platform));
	fake_system.staged = fake_system_image(1, 0, 0, 5, 0xB1);
	zassert_equal(start(false), -EDOM);
	fake_system.staged = fake_system_image(1, 0, 0, 6, 0xB1);
	zassert_ok(system_updater_start("upload_1", job_id, false), "the same version");
}

ZTEST(system_updater, test_init_drops_an_accepted_install)
{
	/* An install accepted and not run, then init again: the accepted work is
	 * gone, and the journal it saved reads interrupted. */
	zassert_ok(start(false));
	zassert_ok(system_updater_init(&fake_system_platform));
	assert_last(SYSTEM_UPDATE_INTERRUPTED, "boot_changed");
	system_updater_run();
	zassert_equal(fake_system.count[FSP_IN_USE], 0);
	zassert_equal(fake_system.count[FSP_REQUEST], 0);
	zassert_false(state().active);
}

ZTEST(system_updater, test_run_twice_runs_once)
{
	unsigned int saves;

	zassert_ok(start(false));
	fake_system.rc[FSP_IN_USE] = -ENOENT;
	system_updater_run();
	saves = fake_system.count[FSP_SAVE];
	system_updater_run();
	zassert_equal(fake_system.count[FSP_IN_USE], 1);
	zassert_equal(fake_system.count[FSP_SAVE], saves, "the second run touched the journal");
	assert_job_failed("not_found");
	zassert_equal(state().job_id[0], '\0', "no job id once inactive");
	zassert_equal(state().upload_id[0], '\0');
}

ZTEST(system_updater, test_start_journal_failure_accepts_nothing)
{
	fake_system.rc[FSP_SAVE] = -EIO;
	zassert_equal(start(false), -ENOSPC);
	zassert_false(state().active);
	zassert_ok(system_updater_check());
	system_updater_run();
	zassert_equal(fake_system.count[FSP_IN_USE], 0, "nothing to run");
	zassert_equal(job().state, JOB_STATE_QUEUED);

	fake_system.rc[FSP_SAVE] = 0;
	zassert_ok(system_updater_start("upload_1", job_id, false));
}

ZTEST(system_updater, test_run_without_start_does_nothing)
{
	system_updater_run();
	for (int c = 0; c < FSP_CALL_COUNT; c++) {
		if (c != FSP_RUNNING && c != FSP_SWAP_PENDING && c != FSP_LOAD) {
			zassert_equal(fake_system.count[c], 0, "call %d", c);
		}
	}
}

/* --- the run ----------------------------------------------------------------- */

ZTEST(system_updater, test_install_runs_to_the_restart)
{
	struct job_snapshot snap;
	struct system_updater_state s;

	start_and_run();

	zassert_equal(fake_system.save_count, 4);
	zassert_equal(fake_system.saves[0].stage, SYSTEM_UPDATE_STAGE_PREPARING);
	zassert_equal(fake_system.saves[1].stage, SYSTEM_UPDATE_STAGE_PREPARING);
	zassert_equal(fake_system.saves[2].stage, SYSTEM_UPDATE_STAGE_REQUESTING);
	zassert_equal(fake_system.saves[3].stage, SYSTEM_UPDATE_STAGE_REBOOTING);
	zassert_str_equal(fake_system.journal.job_id, job_id);
	zassert_str_equal(fake_system.journal.upload_id, "upload_1");
	zassert_mem_equal(fake_system.journal.from.hash, from_img.hash, 32);
	zassert_mem_equal(fake_system.journal.to.hash, to_img.hash, 32);
	zassert_equal(fake_system.journal.to.version.minor, 1);

	zassert_equal(fake_system.count[FSP_REQUEST], 1);
	zassert_equal(fake_system.count[FSP_REBOOT], 1);
	zassert_equal(fake_system.reboot_at_ms - fake_system.request_at_ms,
		      CONFIG_SYSTEM_UPDATER_REBOOT_DELAY_MS);
	zassert_true(fake_system.in_use);
	zassert_str_equal(fake_system.in_use_id, "upload_1");

	snap = job();
	zassert_equal(snap.state, JOB_STATE_RUNNING);
	zassert_str_equal(snap.phase, "rebooting");
	zassert_false(snap.cancellable);

	s = state();
	zassert_true(s.active);
	zassert_equal(s.phase, SYSTEM_UPDATE_REBOOTING);
	zassert_true(s.swap_pending);
	zassert_str_equal(s.job_id, job_id);
	zassert_equal(system_updater_check(), -EBUSY);
}

static enum system_update_stage stage_at_request;
static enum system_update_stage stage_at_reboot;

static void note_stage_at_request(void *arg)
{
	ARG_UNUSED(arg);
	stage_at_request = fake_system.journal.stage;
	fake_system.hook_call = FSP_REBOOT;
	fake_system.hook = note_stage_at_request;
	stage_at_reboot = fake_system.journal.stage;
}

ZTEST(system_updater, test_journal_reaches_storage_before_each_step)
{
	stage_at_request = SYSTEM_UPDATE_STAGE_NONE;
	stage_at_reboot = SYSTEM_UPDATE_STAGE_NONE;
	zassert_ok(start(false));
	fake_system.hook_call = FSP_REQUEST;
	fake_system.hook = note_stage_at_request;
	system_updater_run();
	/* The hook re-arms itself for the reboot; the second call overwrites both. */
	zassert_equal(stage_at_reboot, SYSTEM_UPDATE_STAGE_REBOOTING);
	zassert_equal(fake_system.saves[2].stage, SYSTEM_UPDATE_STAGE_REQUESTING);
	zassert_true(fake_system.count[FSP_SAVE] >= 3);
}

static enum system_update_stage stage_seen_by_request;

static void note_request(void *arg)
{
	ARG_UNUSED(arg);
	stage_seen_by_request = fake_system.journal.stage;
}

ZTEST(system_updater, test_requesting_is_on_storage_before_the_request)
{
	stage_seen_by_request = SYSTEM_UPDATE_STAGE_NONE;
	zassert_ok(start(false));
	fake_system.hook_call = FSP_REQUEST;
	fake_system.hook = note_request;
	system_updater_run();
	zassert_equal(stage_seen_by_request, SYSTEM_UPDATE_STAGE_REQUESTING);
}

/* --- reconciliation ---------------------------------------------------------- */

ZTEST(system_updater, test_new_image_unconfirmed_awaits_confirmation)
{
	struct system_updater_state s;

	start_and_run();
	reboot_into(&to_img, false);

	assert_last(SYSTEM_UPDATE_AWAITING_CONFIRMATION, NULL);
	s = state();
	zassert_str_equal(s.last.job_id, job_id);
	zassert_true(s.last.has_from_version);
	zassert_equal(s.last.from_version.minor, 0);
	zassert_true(s.last.has_version);
	zassert_equal(s.last.version.minor, 1);
	zassert_false(s.active);
	zassert_true(s.confirm_pending);
	zassert_false(s.swap_pending);
	zassert_equal(fake_system.journal.stage, SYSTEM_UPDATE_STAGE_BOOTED);
	zassert_equal(fake_system.count[FSP_CONSUMED], 1);
	zassert_str_equal(fake_system.consumed_id, "upload_1");
	zassert_equal(system_updater_check(), -EACCES);
}

ZTEST(system_updater, test_new_image_confirmed_succeeds)
{
	start_and_run();
	reboot_into(&to_img, true);
	assert_last(SYSTEM_UPDATE_SUCCEEDED, NULL);
	zassert_equal(fake_system.journal.stage, SYSTEM_UPDATE_STAGE_NONE);
	zassert_equal(fake_system.count[FSP_CONSUMED], 1);
	zassert_false(state().confirm_pending);
	zassert_ok(system_updater_check());
}

ZTEST(system_updater, test_old_image_after_the_request_failed)
{
	start_and_run();
	reboot_into(&from_img, true);
	assert_last(SYSTEM_UPDATE_FAILED, "internal_error");
	zassert_true(state().last.error_retryable);
	zassert_equal(fake_system.journal.stage, SYSTEM_UPDATE_STAGE_NONE);
	zassert_equal(fake_system.count[FSP_CONSUMED], 1);
	zassert_ok(system_updater_check());
}

ZTEST(system_updater, test_revert_after_first_boot_rolls_back)
{
	start_and_run();
	reboot_into(&to_img, false);
	reboot_into(&from_img, true);
	assert_last(SYSTEM_UPDATE_ROLLED_BACK, "boot_changed");
	zassert_equal(state().last.version.minor, 1);
	zassert_equal(fake_system.journal.stage, SYSTEM_UPDATE_STAGE_NONE);
	zassert_equal(fake_system.count[FSP_CONSUMED], 1, "consumed once");
	zassert_ok(system_updater_check());
}

ZTEST(system_updater, test_booted_image_confirmed_before_restart_succeeds)
{
	start_and_run();
	reboot_into(&to_img, false);
	reboot_into(&to_img, true);
	assert_last(SYSTEM_UPDATE_SUCCEEDED, NULL);
	zassert_equal(fake_system.journal.stage, SYSTEM_UPDATE_STAGE_NONE);
}

ZTEST(system_updater, test_booted_image_still_unconfirmed_awaits_again)
{
	start_and_run();
	reboot_into(&to_img, false);
	fake_system.now_ms += 5000;
	reboot_into(&to_img, false);
	assert_last(SYSTEM_UPDATE_AWAITING_CONFIRMATION, NULL);
	zassert_equal(fake_system.journal.stage, SYSTEM_UPDATE_STAGE_BOOTED);
	zassert_equal(state().confirm_remaining_seconds, CONFIG_SYSTEM_UPDATER_CONFIRM_SECONDS);
}

ZTEST(system_updater, test_restart_at_each_phase)
{
	struct system_update_journal preparing;
	struct system_update_journal requesting;
	struct system_update_journal rebooting;
	int i;

	start_and_run();
	i = save_with_stage(SYSTEM_UPDATE_STAGE_PREPARING);
	zassert_true(i >= 0);
	preparing = fake_system.saves[i];
	i = save_with_stage(SYSTEM_UPDATE_STAGE_REQUESTING);
	zassert_true(i >= 0);
	requesting = fake_system.saves[i];
	i = save_with_stage(SYSTEM_UPDATE_STAGE_REBOOTING);
	zassert_true(i >= 0);
	rebooting = fake_system.saves[i];

	/* preparing: nothing on flash changed, the upload stays */
	fake_system.journal = preparing;
	fake_system.count[FSP_CONSUMED] = 0;
	reboot_into(&from_img, true);
	assert_last(SYSTEM_UPDATE_INTERRUPTED, "boot_changed");
	zassert_equal(fake_system.count[FSP_CONSUMED], 0);
	zassert_equal(fake_system.journal.stage, SYSTEM_UPDATE_STAGE_NONE);
	zassert_ok(system_updater_check());

	/* requesting, the old image runs: interrupted, the upload may be stale */
	fake_system.journal = requesting;
	reboot_into(&from_img, true);
	assert_last(SYSTEM_UPDATE_INTERRUPTED, "boot_changed");
	zassert_equal(fake_system.count[FSP_CONSUMED], 1);

	/* requesting, but the request reached flash: the swap happened */
	fake_system.journal = requesting;
	reboot_into(&to_img, false);
	assert_last(SYSTEM_UPDATE_AWAITING_CONFIRMATION, NULL);
	zassert_equal(fake_system.journal.stage, SYSTEM_UPDATE_STAGE_BOOTED);

	/* requesting, swapped and somehow confirmed */
	fake_system.journal = requesting;
	reboot_into(&to_img, true);
	assert_last(SYSTEM_UPDATE_SUCCEEDED, NULL);

	/* rebooting, the old image runs */
	fake_system.journal = rebooting;
	reboot_into(&from_img, true);
	assert_last(SYSTEM_UPDATE_FAILED, "internal_error");

	/* preparing, even with the new image somehow running */
	fake_system.journal = preparing;
	reboot_into(&to_img, false);
	assert_last(SYSTEM_UPDATE_INTERRUPTED, "boot_changed");
}

ZTEST(system_updater, test_reinstall_of_the_running_image)
{
	struct system_update_journal rebooting;
	struct system_image same = from_img;

	fake_system.staged = same;
	start_and_run();
	zassert_equal(fake_system.count[FSP_REBOOT], 1);
	rebooting = fake_system.journal;

	/* Swapped in: MCUboot leaves it unconfirmed. */
	reboot_into(&same, false);
	assert_last(SYSTEM_UPDATE_AWAITING_CONFIRMATION, NULL);

	/* A confirmed identical image means no swap happened. */
	fake_system.journal = rebooting;
	reboot_into(&same, true);
	assert_last(SYSTEM_UPDATE_FAILED, "internal_error");

	/* Once booted, a revert of identical images still shows the same image. */
	fake_system.journal = rebooting;
	reboot_into(&same, false);
	reboot_into(&same, true);
	assert_last(SYSTEM_UPDATE_SUCCEEDED, NULL);
}

ZTEST(system_updater, test_same_hash_other_version_is_another_image)
{
	/* An image is its hash and its version: a different header is a different image. */
	struct system_image other = from_img;

	other.version.build = 7;
	fake_system.staged = other;
	start_and_run();
	reboot_into(&from_img, true);
	assert_last(SYSTEM_UPDATE_FAILED, "internal_error");
}

/* --- cancellation -------------------------------------------------------------- */

ZTEST(system_updater, test_cancel_while_queued)
{
	zassert_ok(start(false));
	zassert_ok(job_cancel(job_id));
	system_updater_run();

	zassert_equal(job().state, JOB_STATE_CANCELLED);
	zassert_equal(fake_system.count[FSP_IN_USE], 0);
	zassert_equal(fake_system.count[FSP_REQUEST], 0);
	zassert_equal(fake_system.journal.stage, SYSTEM_UPDATE_STAGE_NONE);
	zassert_false(state().has_last);
	zassert_false(state().active);
	zassert_ok(system_updater_check());
}

static void cancel_hook(void *arg)
{
	ARG_UNUSED(arg);
	zassert_ok(job_cancel(job_id));
}

ZTEST(system_updater, test_cancel_while_preparing)
{
	zassert_ok(start(false));
	fake_system.hook_call = FSP_STAGED;
	fake_system.hook = cancel_hook;
	system_updater_run();

	zassert_equal(job().state, JOB_STATE_CANCELLED);
	zassert_equal(fake_system.count[FSP_IN_USE], 2, "held and released");
	zassert_false(fake_system.in_use);
	zassert_equal(fake_system.count[FSP_REQUEST], 0);
	zassert_equal(fake_system.journal.stage, SYSTEM_UPDATE_STAGE_NONE);
	zassert_false(state().has_last);
	zassert_ok(system_updater_check());
}

ZTEST(system_updater, test_cancel_at_the_start_of_preparing)
{
	zassert_ok(start(false));
	/* The first save of the run is the `preparing` phase. */
	fake_system.hook_call = FSP_SAVE;
	fake_system.hook = cancel_hook;
	system_updater_run();

	zassert_equal(job().state, JOB_STATE_CANCELLED);
	zassert_equal(fake_system.count[FSP_IN_USE], 0);
	zassert_equal(fake_system.count[FSP_REQUEST], 0);
}

static int cancel_rc;

static void late_cancel_hook(void *arg)
{
	ARG_UNUSED(arg);
	cancel_rc = job_cancel(job_id);
}

ZTEST(system_updater, test_cancel_after_the_gate_is_refused)
{
	cancel_rc = 0;
	zassert_ok(start(false));
	fake_system.hook_call = FSP_REQUEST;
	fake_system.hook = late_cancel_hook;
	system_updater_run();

	zassert_equal(cancel_rc, -EPERM);
	zassert_equal(job().state, JOB_STATE_RUNNING);
	zassert_equal(fake_system.count[FSP_REBOOT], 1);
}

/* --- failures before the request ----------------------------------------------- */

static void assert_failed_before_request(const char *code)
{
	assert_job_failed(code);
	assert_last(SYSTEM_UPDATE_FAILED, code);
	zassert_equal(fake_system.count[FSP_REQUEST], 0);
	zassert_equal(fake_system.count[FSP_REBOOT], 0);
	zassert_false(fake_system.in_use);
	zassert_equal(fake_system.journal.stage, SYSTEM_UPDATE_STAGE_NONE);
	zassert_false(state().active);
	zassert_false(state().swap_pending);
	zassert_ok(system_updater_check());
}

ZTEST(system_updater, test_upload_cannot_be_held)
{
	zassert_ok(start(false));
	fake_system.rc[FSP_IN_USE] = -ENOENT;
	system_updater_run();
	assert_failed_before_request("not_found");
	zassert_false(state().last.error_retryable);
	zassert_equal(fake_system.count[FSP_IN_USE], 1, "nothing to release");
	zassert_equal(state().last.version.minor, 1);
}

ZTEST(system_updater, test_upload_not_ready_when_held)
{
	zassert_ok(start(false));
	fake_system.rc[FSP_IN_USE] = -EINVAL;
	system_updater_run();
	assert_failed_before_request("invalid_state");
}

static void change_staged(void *arg)
{
	ARG_UNUSED(arg);
	fake_system.staged.hash[5] ^= 1U;
}

ZTEST(system_updater, test_staged_image_changed)
{
	zassert_ok(start(false));
	fake_system.hook_call = FSP_IN_USE;
	fake_system.hook = change_staged;
	system_updater_run();
	assert_failed_before_request("invalid_state");
	zassert_false(state().last.error_retryable);
	zassert_false(job().error.retryable);
	zassert_equal(fake_system.count[FSP_IN_USE], 2, "released");
}

static void change_staged_version(void *arg)
{
	ARG_UNUSED(arg);
	fake_system.staged.version.revision++;
}

ZTEST(system_updater, test_staged_image_version_changed)
{
	zassert_ok(start(false));
	fake_system.hook_call = FSP_IN_USE;
	fake_system.hook = change_staged_version;
	system_updater_run();
	assert_failed_before_request("invalid_state");
}

static void staged_gone(void *arg)
{
	ARG_UNUSED(arg);
	fake_system.rc[FSP_STAGED] = -ENOENT;
}

ZTEST(system_updater, test_staged_image_gone)
{
	zassert_ok(start(false));
	fake_system.hook_call = FSP_IN_USE;
	fake_system.hook = staged_gone;
	system_updater_run();
	assert_failed_before_request("not_found");
}

static void running_unconfirmed(void *arg)
{
	ARG_UNUSED(arg);
	fake_system.running_confirmed = false;
}

ZTEST(system_updater, test_running_image_became_unconfirmed)
{
	zassert_ok(start(false));
	fake_system.hook_call = FSP_IN_USE;
	fake_system.hook = running_unconfirmed;
	system_updater_run();
	assert_failed_before_request("invalid_state");
}

static void running_changed(void *arg)
{
	ARG_UNUSED(arg);
	fake_system.running.hash[0] ^= 1U;
}

ZTEST(system_updater, test_running_image_changed)
{
	zassert_ok(start(false));
	fake_system.hook_call = FSP_IN_USE;
	fake_system.hook = running_changed;
	system_updater_run();
	assert_failed_before_request("invalid_state");
}

static void running_unreadable(void *arg)
{
	ARG_UNUSED(arg);
	fake_system.rc[FSP_RUNNING] = -EIO;
}

ZTEST(system_updater, test_running_image_unreadable_at_run)
{
	zassert_ok(start(false));
	fake_system.hook_call = FSP_IN_USE;
	fake_system.hook = running_unreadable;
	system_updater_run();
	assert_job_failed("internal_error");
	zassert_true(state().last.error_retryable);
	zassert_equal(fake_system.count[FSP_REQUEST], 0);
	zassert_false(fake_system.in_use);
}

ZTEST(system_updater, test_requesting_journal_failure_stops_before_the_request)
{
	zassert_ok(start(false));
	/* Saves: 1 start, 2 preparing, 3 requesting. */
	fake_system.rc[FSP_SAVE] = -EIO;
	fake_system.save_fail_at = 3;
	system_updater_run();
	assert_failed_before_request("internal_error");
}

/* --- failures of the request ----------------------------------------------------- */

ZTEST(system_updater, test_request_failure)
{
	zassert_ok(start(false));
	fake_system.rc[FSP_REQUEST] = -EIO;
	system_updater_run();
	assert_job_failed("internal_error");
	assert_last(SYSTEM_UPDATE_FAILED, "internal_error");
	zassert_equal(fake_system.count[FSP_REBOOT], 0);
	zassert_false(fake_system.in_use);
	zassert_false(state().swap_pending);
	zassert_equal(fake_system.journal.stage, SYSTEM_UPDATE_STAGE_NONE);
	zassert_ok(system_updater_check());
}

ZTEST(system_updater, test_request_not_recorded)
{
	zassert_ok(start(false));
	fake_system.request_sets_pending = false;
	system_updater_run();
	assert_job_failed("internal_error");
	zassert_equal(fake_system.count[FSP_REBOOT], 0);
	zassert_ok(system_updater_check());
}

static void half_request(void *arg)
{
	ARG_UNUSED(arg);
	fake_system.swap_pending = true;
}

ZTEST(system_updater, test_failed_request_that_left_the_trailer_set)
{
	zassert_ok(start(false));
	fake_system.rc[FSP_REQUEST] = -EIO;
	fake_system.hook_call = FSP_REQUEST;
	fake_system.hook = half_request;
	system_updater_run();
	assert_job_failed("internal_error");
	zassert_true(state().swap_pending);
	zassert_equal(system_updater_check(), -EACCES);
}

ZTEST(system_updater, test_rebooting_journal_failure_still_restarts)
{
	zassert_ok(start(false));
	/* Saves: 1 start, 2 preparing, 3 requesting, 4 rebooting. */
	fake_system.rc[FSP_SAVE] = -EIO;
	fake_system.save_fail_at = 4;
	system_updater_run();
	zassert_equal(fake_system.count[FSP_REBOOT], 1);
	zassert_equal(fake_system.journal.stage, SYSTEM_UPDATE_STAGE_REQUESTING);

	fake_system.rc[FSP_SAVE] = 0;
	reboot_into(&to_img, false);
	assert_last(SYSTEM_UPDATE_AWAITING_CONFIRMATION, NULL);
}

/* --- confirmation ------------------------------------------------------------------ */

static int64_t awaiting(void)
{
	int64_t t0;

	start_and_run();
	t0 = fake_system.now_ms;
	reboot_into(&to_img, false);
	assert_last(SYSTEM_UPDATE_AWAITING_CONFIRMATION, NULL);
	return t0;
}

ZTEST(system_updater, test_new_image_confirms_itself_at_the_deadline)
{
	int64_t t0 = awaiting();

	system_updater_tick(t0 + CONFIRM_MS - 1);
	zassert_equal(fake_system.count[FSP_CONFIRM], 0);
	system_updater_tick(t0 + CONFIRM_MS);
	zassert_equal(fake_system.count[FSP_CONFIRM], 1);

	assert_last(SYSTEM_UPDATE_SUCCEEDED, NULL);
	zassert_equal(fake_system.journal.stage, SYSTEM_UPDATE_STAGE_NONE);
	zassert_equal(fake_system.journal.last.state, SYSTEM_UPDATE_SUCCEEDED);
	zassert_true(state().running_confirmed);
	zassert_false(state().confirm_pending);
	zassert_ok(system_updater_check());

	system_updater_tick(t0 + 10 * CONFIRM_MS);
	zassert_equal(fake_system.count[FSP_CONFIRM], 1, "only once");

	reboot_into(&to_img, true);
	assert_last(SYSTEM_UPDATE_SUCCEEDED, NULL);
	zassert_false(state().confirm_pending);
}

ZTEST(system_updater, test_failed_confirmation_retries_with_backoff)
{
	int64_t d = awaiting() + CONFIRM_MS;

	fake_system.rc[FSP_CONFIRM] = -EIO;
	system_updater_tick(d);
	zassert_equal(fake_system.count[FSP_CONFIRM], 1);
	assert_last(SYSTEM_UPDATE_AWAITING_CONFIRMATION, NULL);
	zassert_true(state().confirm_pending);

	system_updater_tick(d + RETRY_MS - 1);
	zassert_equal(fake_system.count[FSP_CONFIRM], 1);
	system_updater_tick(d + RETRY_MS);
	zassert_equal(fake_system.count[FSP_CONFIRM], 2);

	d += RETRY_MS;
	system_updater_tick(d + 2 * RETRY_MS - 1);
	zassert_equal(fake_system.count[FSP_CONFIRM], 2);
	system_updater_tick(d + 2 * RETRY_MS);
	zassert_equal(fake_system.count[FSP_CONFIRM], 3);

	d += 2 * RETRY_MS;
	fake_system.rc[FSP_CONFIRM] = 0;
	system_updater_tick(d + 4 * RETRY_MS);
	zassert_equal(fake_system.count[FSP_CONFIRM], 4);
	assert_last(SYSTEM_UPDATE_SUCCEEDED, NULL);
}

ZTEST(system_updater, test_confirmation_backoff_is_capped)
{
	int64_t t = awaiting() + CONFIRM_MS;
	int64_t delay = RETRY_MS;

	fake_system.rc[FSP_CONFIRM] = -EIO;
	system_updater_tick(t);
	for (int i = 0; i < 12; i++) {
		unsigned int before = fake_system.count[FSP_CONFIRM];

		system_updater_tick(t + delay - 1);
		zassert_equal(fake_system.count[FSP_CONFIRM], before, "retry %d early", i);
		system_updater_tick(t + delay);
		zassert_equal(fake_system.count[FSP_CONFIRM], before + 1, "retry %d", i);
		t += delay;
		delay = MIN(delay * 2, (int64_t)CONFIG_SYSTEM_UPDATER_CONFIRM_RETRY_MAX_MS);
	}
	zassert_equal(delay, CONFIG_SYSTEM_UPDATER_CONFIRM_RETRY_MAX_MS);
}

ZTEST(system_updater, test_confirmation_that_does_not_take)
{
	int64_t d = awaiting() + CONFIRM_MS;

	fake_system.confirm_takes = false;
	system_updater_tick(d);
	zassert_equal(fake_system.count[FSP_CONFIRM], 1);
	assert_last(SYSTEM_UPDATE_AWAITING_CONFIRMATION, NULL);
	zassert_false(state().running_confirmed);
	zassert_equal(system_updater_confirm_now(), -EIO);
}

ZTEST(system_updater, test_confirmation_with_unreadable_image)
{
	int64_t d = awaiting() + CONFIRM_MS;

	fake_system.rc[FSP_RUNNING] = -EIO;
	/* The failed read even says "confirmed": it does not count. */
	fake_system.running_fills_on_error = true;
	system_updater_tick(d);
	zassert_equal(fake_system.count[FSP_CONFIRM], 1);
	assert_last(SYSTEM_UPDATE_AWAITING_CONFIRMATION, NULL);
	zassert_true(state().confirm_pending);
}

static void confirmed_anyway(void *arg)
{
	ARG_UNUSED(arg);
	fake_system.running_confirmed = true;
}

ZTEST(system_updater, test_confirm_error_counts_even_if_the_image_reads_confirmed)
{
	awaiting();
	fake_system.rc[FSP_CONFIRM] = -EIO;
	fake_system.hook_call = FSP_CONFIRM;
	fake_system.hook = confirmed_anyway;
	zassert_equal(system_updater_confirm_now(), -EIO);
	assert_last(SYSTEM_UPDATE_AWAITING_CONFIRMATION, NULL);
	zassert_true(state().confirm_pending);
}

ZTEST(system_updater, test_no_remaining_seconds_once_confirmed)
{
	awaiting();
	zassert_ok(system_updater_confirm_now());
	zassert_false(state().confirm_pending);
	zassert_equal(state().confirm_remaining_seconds, 0);
}

ZTEST(system_updater, test_confirm_now)
{
	awaiting();
	zassert_ok(system_updater_confirm_now());
	zassert_equal(fake_system.count[FSP_CONFIRM], 1);
	assert_last(SYSTEM_UPDATE_SUCCEEDED, NULL);
	zassert_equal(fake_system.journal.stage, SYSTEM_UPDATE_STAGE_NONE);
	zassert_false(state().confirm_pending);

	zassert_ok(system_updater_confirm_now());
	zassert_equal(fake_system.count[FSP_CONFIRM], 1, "already confirmed");
}

ZTEST(system_updater, test_confirm_now_failure)
{
	awaiting();
	fake_system.rc[FSP_CONFIRM] = -EIO;
	zassert_equal(system_updater_confirm_now(), -EIO);
	assert_last(SYSTEM_UPDATE_AWAITING_CONFIRMATION, NULL);
	zassert_true(state().confirm_pending);
}

ZTEST(system_updater, test_confirm_now_on_a_confirmed_image)
{
	zassert_ok(system_updater_confirm_now());
	zassert_equal(fake_system.count[FSP_CONFIRM], 0);
	zassert_false(state().has_last);
}

ZTEST(system_updater, test_confirmed_without_journal_leaves_last_update)
{
	/* A finished update, then a hand-flashed unconfirmed image. */
	start_and_run();
	reboot_into(&to_img, true);
	assert_last(SYSTEM_UPDATE_SUCCEEDED, NULL);

	reboot_into(&from_img, false);
	assert_last(SYSTEM_UPDATE_SUCCEEDED, NULL);
	zassert_ok(system_updater_confirm_now());
	assert_last(SYSTEM_UPDATE_SUCCEEDED, NULL);
	zassert_equal(state().last.version.minor, 1);
}

/* --- names and versions ----------------------------------------------------------- */

ZTEST(system_updater, test_version_compare)
{
	struct system_image_version a = {1, 0, 0, 9};
	struct system_image_version b = {1, 0, 1, 0};
	struct system_image_version c = {1, 1, 0, 0};
	struct system_image_version d = {2, 0, 0, 0};
	struct system_image_version e = {1, 0, 0, 10};

	zassert_true(system_image_version_cmp(&a, &b) < 0);
	zassert_true(system_image_version_cmp(&b, &a) > 0);
	zassert_true(system_image_version_cmp(&b, &c) < 0);
	zassert_true(system_image_version_cmp(&c, &b) > 0);
	zassert_true(system_image_version_cmp(&c, &d) < 0);
	zassert_true(system_image_version_cmp(&d, &c) > 0);
	zassert_true(system_image_version_cmp(&a, &e) < 0);
	zassert_true(system_image_version_cmp(&e, &a) > 0);
	zassert_equal(system_image_version_cmp(&a, &a), 0);
}

ZTEST(system_updater, test_version_string)
{
	struct system_image_version v = {1, 2, 3, 4};
	struct system_image_version max = {255, 255, 65535, UINT32_MAX};
	char buf[SYSTEM_UPDATE_VERSION_STR_LEN];

	zassert_equal(system_image_version_str(&v, buf, sizeof(buf)), 7);
	zassert_str_equal(buf, "1.2.3+4");
	zassert_equal(system_image_version_str(&max, buf, sizeof(buf)), 24);
	zassert_str_equal(buf, "255.255.65535+4294967295");
	zassert_equal(system_image_version_str(&v, buf, 7), -ENOSPC);
	zassert_equal(system_image_version_str(&v, buf, 8), 7);
}

ZTEST(system_updater, test_wire_names)
{
	zassert_str_equal(system_update_phase_str(SYSTEM_UPDATE_PREPARING), "preparing");
	zassert_str_equal(system_update_phase_str(SYSTEM_UPDATE_REQUESTING), "requesting");
	zassert_str_equal(system_update_phase_str(SYSTEM_UPDATE_REBOOTING), "rebooting");
	zassert_is_null(system_update_phase_str(SYSTEM_UPDATE_PHASE_COUNT));
	zassert_str_equal(system_update_outcome_str(SYSTEM_UPDATE_AWAITING_CONFIRMATION),
			  "awaiting_confirmation");
	zassert_str_equal(system_update_outcome_str(SYSTEM_UPDATE_SUCCEEDED), "succeeded");
	zassert_str_equal(system_update_outcome_str(SYSTEM_UPDATE_FAILED), "failed");
	zassert_str_equal(system_update_outcome_str(SYSTEM_UPDATE_ROLLED_BACK), "rolled_back");
	zassert_str_equal(system_update_outcome_str(SYSTEM_UPDATE_INTERRUPTED), "interrupted");
	zassert_is_null(system_update_outcome_str(SYSTEM_UPDATE_OUTCOME_COUNT));
	zassert_str_equal(job_kind_str(JOB_KIND_SYSTEM_UPDATE), "system_update");
}
