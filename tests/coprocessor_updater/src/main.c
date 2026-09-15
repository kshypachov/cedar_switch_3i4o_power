/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * coprocessor-updater unit tests.
 *
 * Section 12 asks this tier for the phase automaton on a fake loader: a
 * failure and a timeout of every phase, recovery_required, no continuation
 * after a restart, and the refusals that belong to the updater (a second
 * install, the USB bridge). The platform is tests/fakes/fake_update_platform.c;
 * job-manager and coprocessor-manager (on the fake UART) are the real ones.
 * ethernet_required and "the upload is not ready" are the HTTP binding's
 * checks and are tested with it.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <coprocessor_manager/coprocessor_manager.h>
#include <coprocessor_updater/coprocessor_updater.h>
#include <job_manager/job_manager.h>

#include "fake_uart.h"
#include "fake_update_platform.h"

#define PREFLIGHT   COPROCESSOR_UPDATE_PREFLIGHT
#define ENTERING    COPROCESSOR_UPDATE_ENTERING_BOOTLOADER
#define BEGIN       COPROCESSOR_UPDATE_BEGIN
#define WRITING     COPROCESSOR_UPDATE_WRITING
#define VERIFYING   COPROCESSOR_UPDATE_VERIFYING
#define RECONNECT   COPROCESSOR_UPDATE_RECONNECTING
#define HEALTH      COPROCESSOR_UPDATE_HEALTH_CHECK
#define COMPLETE    COPROCESSOR_UPDATE_COMPLETE

static char job_id[JOB_ID_MAX_LEN + 1];

static void case_before(void *fixture)
{
	ARG_UNUSED(fixture);

	fake_uart_init();
	fake_uart.now_ms = 10000;
	zassert_ok(coprocessor_manager_init(&fake_uart_platform));
	job_manager_init();
	fake_update_init();
	zassert_ok(coprocessor_updater_init(&fake_update_platform));
	job_id[0] = '\0';
}

ZTEST_SUITE(coprocessor_updater, NULL, NULL, case_before, NULL, NULL);

/* --- helpers ------------------------------------------------------------ */

static void create_job(void)
{
	struct job_create_params p = {
		.kind = JOB_KIND_COPROCESSOR_UPDATE,
		.cancellable = true,
	};
	struct job_snapshot snap;

	zassert_equal(job_create(&p, &snap), JOB_CREATE_NEW);
	strcpy(job_id, snap.id);
}

static void start(void)
{
	create_job();
	zassert_ok(coprocessor_updater_start("upload_1", job_id));
}

static void start_and_run(void)
{
	start();
	coprocessor_updater_run();
}

static struct job_snapshot job(void)
{
	struct job_snapshot snap;

	zassert_ok(job_get(job_id, &snap));
	return snap;
}

static struct coprocessor_updater_state state(void)
{
	struct coprocessor_updater_state s;

	coprocessor_updater_get_state(&s);
	return s;
}

/* A failed install: its job, its summary, what was left behind. */
static void assert_failed(enum coprocessor_update_phase phase, const char *code, bool recovery)
{
	struct job_snapshot snap = job();
	struct coprocessor_updater_state s = state();

	zassert_equal(snap.state, JOB_STATE_FAILED, "job state %d", snap.state);
	zassert_true(snap.has_error);
	zassert_str_equal(snap.error.code, code);
	zassert_false(s.active);
	zassert_true(s.has_last);
	zassert_str_equal(s.last.job_id, job_id);
	zassert_equal(s.last.state, COPROCESSOR_UPDATE_FAILED);
	zassert_equal(s.last.phase, phase, "failed in phase %d", s.last.phase);
	zassert_str_equal(s.last.error_code, code);
	zassert_true(s.last.has_error);
	zassert_true(strlen(s.last.error_message) > 0);
	zassert_equal(s.last.recovery_required, recovery);
	zassert_str_equal(s.last.version, "", "no confirmed version on a failure");
	zassert_false(fake_update.loader_open, "the loader session was closed");
	zassert_false(fake_update.image_open, "the image was closed");
	zassert_true(fake_update.count[FUP_IMAGE_CLOSE] <= 1, "closed at most once");
	zassert_equal(fake_update.outside_session, 0);
	zassert_false(fake_update.journal.active, "the journal says it is over");
	zassert_equal(fake_update.journal.last.state, COPROCESSOR_UPDATE_FAILED);
}

static void assert_timeout_message(const char *phase)
{
	struct coprocessor_updater_state s = state();

	zassert_not_null(strstr(s.last.error_message, phase), "%s", s.last.error_message);
	zassert_not_null(strstr(s.last.error_message, "longer than"));
	zassert_true(s.last.error_retryable);
}

/* --- init and the journal ----------------------------------------------- */

#define WITHOUT(field)                                                                             \
	do {                                                                                       \
		struct coprocessor_updater_platform p = fake_update_platform;                      \
                                                                                                   \
		p.field = NULL;                                                                    \
		zassert_equal(coprocessor_updater_init(&p), -EINVAL, #field);                      \
	} while (0)

#define WITHOUT_LOADER(field)                                                                      \
	do {                                                                                       \
		struct coprocessor_updater_platform p = fake_update_platform;                      \
		struct coprocessor_updater_loader l = *fake_update_platform.loader;                \
                                                                                                   \
		l.field = NULL;                                                                    \
		p.loader = &l;                                                                     \
		zassert_equal(coprocessor_updater_init(&p), -EINVAL, "loader " #field);            \
	} while (0)

ZTEST(coprocessor_updater, test_init_requires_every_function)
{
	zassert_equal(coprocessor_updater_init(NULL), -EINVAL);
	WITHOUT(loader);
	WITHOUT_LOADER(open);
	WITHOUT_LOADER(begin);
	WITHOUT_LOADER(write);
	WITHOUT_LOADER(finish);
	WITHOUT_LOADER(close);
	WITHOUT(image_open);
	WITHOUT(image_read);
	WITHOUT(image_close);
	WITHOUT(journal_load);
	WITHOUT(journal_save);
	WITHOUT(boot_evidence_arm);
	WITHOUT(boot_evidence);
	WITHOUT(transport_restart);
	WITHOUT(now_ms);
	WITHOUT(sleep_ms);
	zassert_ok(coprocessor_updater_init(&fake_update_platform));
}

ZTEST(coprocessor_updater, test_fresh_device_has_no_last_update)
{
	struct coprocessor_updater_state s = state();

	zassert_false(s.active);
	zassert_false(s.has_last);
	zassert_ok(coprocessor_updater_check());
	zassert_equal(fake_update.save_count, 0, "nothing to save without a journal");
}

static void restart_with_journal(enum coprocessor_update_phase phase)
{
	struct coprocessor_update_journal j = {0};

	j.format = COPROCESSOR_UPDATE_JOURNAL_FORMAT;
	j.active = true;
	j.phase = phase;
	strcpy(j.job_id, "job_before");
	strcpy(j.upload_id, "upload_9");
	fake_update_init();
	fake_update.journal = j;
	fake_update.has_journal = true;
	zassert_ok(coprocessor_updater_init(&fake_update_platform));
}

ZTEST(coprocessor_updater, test_restart_in_writing_is_interrupted_with_recovery_and_not_continued)
{
	struct coprocessor_updater_state s;

	restart_with_journal(WRITING);
	s = state();
	zassert_false(s.active);
	zassert_true(s.has_last);
	zassert_equal(s.last.state, COPROCESSOR_UPDATE_INTERRUPTED);
	zassert_str_equal(s.last.job_id, "job_before");
	zassert_true(s.last.recovery_required, "the erase had begun");
	zassert_equal(s.last.phase, WRITING);
	zassert_true(s.last.has_error);
	zassert_str_equal(s.last.error_code, "internal_error");
	zassert_not_null(strstr(s.last.error_message, "writing"));
	zassert_str_equal(s.last.version, "");
	zassert_true(s.last.error_retryable, "a person may try again");

	zassert_equal(fake_update.save_count, 1, "saved as interrupted at once");
	zassert_false(fake_update.journal.active);
	zassert_equal(fake_update.journal.last.state, COPROCESSOR_UPDATE_INTERRUPTED);

	coprocessor_updater_run();
	zassert_equal(fake_update.call_count, 0, "never continued");
	zassert_ok(coprocessor_updater_check(), "a person may start a new install");
}

ZTEST(coprocessor_updater, test_interrupted_summary_keeps_nothing_of_an_older_outcome)
{
	struct coprocessor_update_journal j = {0};
	struct coprocessor_updater_state s;

	j.format = COPROCESSOR_UPDATE_JOURNAL_FORMAT;
	j.active = true;
	j.phase = VERIFYING;
	strcpy(j.job_id, "job_now");
	j.has_last = true;
	strcpy(j.last.job_id, "job_old");
	strcpy(j.last.version, "9.9.9");
	j.last.uart_evidence = true;
	j.last.stm32_restart_needed = true;
	j.last.state = COPROCESSOR_UPDATE_SUCCEEDED;
	fake_update_init();
	fake_update.journal = j;
	fake_update.has_journal = true;
	zassert_ok(coprocessor_updater_init(&fake_update_platform));

	s = state();
	zassert_equal(s.last.state, COPROCESSOR_UPDATE_INTERRUPTED);
	zassert_str_equal(s.last.job_id, "job_now");
	zassert_str_equal(s.last.version, "", "the old version was not confirmed by this install");
	zassert_false(s.last.uart_evidence);
	zassert_false(s.last.stm32_restart_needed);
}

ZTEST(coprocessor_updater, test_restart_before_begin_needs_no_recovery)
{
	restart_with_journal(ENTERING);
	zassert_equal(state().last.state, COPROCESSOR_UPDATE_INTERRUPTED);
	zassert_false(state().last.recovery_required, "nothing was erased yet");

	restart_with_journal(BEGIN);
	zassert_true(state().last.recovery_required, "begin is where the ROM erases");
}

ZTEST(coprocessor_updater, test_finished_journal_is_kept_as_is)
{
	struct coprocessor_update_journal j = {0};

	j.format = COPROCESSOR_UPDATE_JOURNAL_FORMAT;
	j.has_last = true;
	j.last.state = COPROCESSOR_UPDATE_SUCCEEDED;
	strcpy(j.last.version, "3.0.6");
	strcpy(j.last.job_id, "job_old");
	fake_update_init();
	fake_update.journal = j;
	fake_update.has_journal = true;
	zassert_ok(coprocessor_updater_init(&fake_update_platform));
	zassert_equal(fake_update.save_count, 0);
	zassert_true(state().has_last);
	zassert_equal(state().last.state, COPROCESSOR_UPDATE_SUCCEEDED);
	zassert_str_equal(state().last.version, "3.0.6");
}

ZTEST(coprocessor_updater, test_unreadable_journal_is_no_journal)
{
	struct coprocessor_update_journal j = {0};

	j.format = COPROCESSOR_UPDATE_JOURNAL_FORMAT + 1;
	j.active = true;
	j.phase = WRITING;
	fake_update_init();
	fake_update.journal = j;
	fake_update.has_journal = true;
	zassert_ok(coprocessor_updater_init(&fake_update_platform));
	zassert_false(state().has_last, "another format is not guessed at");
	zassert_false(state().active);

	j.format = COPROCESSOR_UPDATE_JOURNAL_FORMAT;
	j.phase = COPROCESSOR_UPDATE_PHASE_COUNT;
	fake_update.journal = j;
	zassert_ok(coprocessor_updater_init(&fake_update_platform));
	zassert_false(state().has_last, "a phase out of range is corruption");

	j.phase = WRITING;
	j.last.phase = COPROCESSOR_UPDATE_PHASE_COUNT + 3;
	fake_update.journal = j;
	zassert_ok(coprocessor_updater_init(&fake_update_platform));
	zassert_false(state().has_last);

	j.last.phase = COPROCESSOR_UPDATE_PHASE_COUNT;
	fake_update.journal = j;
	zassert_ok(coprocessor_updater_init(&fake_update_platform));
	zassert_false(state().has_last, "the first value out of range");
}

ZTEST(coprocessor_updater, test_journal_strings_are_terminated_on_load)
{
	struct coprocessor_update_journal j;

	memset(&j, 'x', sizeof(j));
	j.format = COPROCESSOR_UPDATE_JOURNAL_FORMAT;
	j.active = false;
	j.has_last = true;
	j.phase = PREFLIGHT;
	j.last.phase = COMPLETE;
	j.last.state = COPROCESSOR_UPDATE_SUCCEEDED;
	fake_update_init();
	fake_update.journal = j;
	fake_update.has_journal = true;
	zassert_ok(coprocessor_updater_init(&fake_update_platform));
	zassert_equal(strlen(state().last.version), COPROCESSOR_UPDATE_VERSION_MAX_LEN);
	zassert_equal(strlen(state().last.error_message), COPROCESSOR_UPDATE_MESSAGE_MAX_LEN);
	zassert_equal(strlen(state().last.error_code), COPROCESSOR_UPDATE_CODE_MAX_LEN);
	zassert_equal(strlen(state().last.job_id), JOB_ID_MAX_LEN);
	zassert_equal(strlen(state().job_id), JOB_ID_MAX_LEN);
	zassert_equal(strlen(state().upload_id), COPROCESSOR_UPDATE_UPLOAD_ID_MAX_LEN);
	zassert_true(state().has_last, "terminating a string does not spill into the next field");
	zassert_equal(state().last.state, COPROCESSOR_UPDATE_SUCCEEDED);
}

ZTEST(coprocessor_updater, test_init_while_an_install_is_active_is_busy)
{
	start();
	zassert_equal(coprocessor_updater_init(&fake_update_platform), -EBUSY);
	coprocessor_updater_run();
	zassert_ok(coprocessor_updater_init(&fake_update_platform));
}

/* --- start -------------------------------------------------------------- */

ZTEST(coprocessor_updater, test_start_saves_the_journal_before_anything_runs)
{
	start();
	zassert_true(state().active);
	zassert_equal(state().phase, PREFLIGHT);
	zassert_str_equal(state().job_id, job_id);
	zassert_str_equal(state().upload_id, "upload_1");
	zassert_equal(fake_update.save_count, 1);
	zassert_true(fake_update.journal.active);
	zassert_equal(fake_update.call_count, 0, "the work is the run's");
	zassert_equal(job().state, JOB_STATE_QUEUED);
	coprocessor_updater_run();
}

ZTEST(coprocessor_updater, test_second_install_is_busy)
{
	start();
	zassert_equal(coprocessor_updater_check(), -EBUSY);
	zassert_equal(coprocessor_updater_start("upload_1", "job_other"), -EBUSY);
	coprocessor_updater_run();
	zassert_ok(coprocessor_updater_check(), "free again when the run is over");
}

ZTEST(coprocessor_updater, test_usb_bridge_refuses_an_install)
{
	create_job();
	zassert_ok(coprocessor_manager_set_mode(COPROCESSOR_UART_USB_BRIDGE));
	zassert_equal(coprocessor_updater_check(), -EBUSY);
	zassert_equal(coprocessor_updater_start("upload_1", job_id), -EBUSY);
	zassert_false(state().active);
	zassert_equal(fake_update.save_count, 0);
}

ZTEST(coprocessor_updater, test_start_rejects_bad_ids)
{
	char long_upload[COPROCESSOR_UPDATE_UPLOAD_ID_MAX_LEN + 2];
	char long_job[JOB_ID_MAX_LEN + 2];

	memset(long_upload, 'u', sizeof(long_upload) - 1);
	long_upload[sizeof(long_upload) - 1] = '\0';
	memset(long_job, 'j', sizeof(long_job) - 1);
	long_job[sizeof(long_job) - 1] = '\0';

	zassert_equal(coprocessor_updater_start(NULL, "job_1"), -EINVAL);
	zassert_equal(coprocessor_updater_start("upload_1", NULL), -EINVAL);
	zassert_equal(coprocessor_updater_start("", "job_1"), -EINVAL);
	zassert_equal(coprocessor_updater_start("upload_1", ""), -EINVAL);
	zassert_equal(coprocessor_updater_start(long_upload, "job_1"), -EINVAL);
	zassert_equal(coprocessor_updater_start("upload_1", long_job), -EINVAL);
	long_upload[COPROCESSOR_UPDATE_UPLOAD_ID_MAX_LEN] = '\0';
	long_job[JOB_ID_MAX_LEN] = '\0';
	zassert_ok(coprocessor_updater_start(long_upload, long_job), "the longest allowed");
	zassert_true(state().active);
	/* No such job: the run ends at once, as a cancel. */
	coprocessor_updater_run();
	zassert_false(state().active);
}

ZTEST(coprocessor_updater, test_journal_that_cannot_be_saved_accepts_nothing)
{
	create_job();
	fake_update.save_rc = -ENOSPC;
	zassert_equal(coprocessor_updater_start("upload_1", job_id), -EIO);
	zassert_false(state().active);
	zassert_ok(coprocessor_updater_check());
	coprocessor_updater_run();
	zassert_equal(fake_update.call_count, 0, "nothing was accepted");
}

ZTEST(coprocessor_updater, test_run_with_nothing_accepted_does_nothing)
{
	coprocessor_updater_run();
	zassert_equal(fake_update.call_count, 0);
	zassert_equal(fake_update.save_count, 0);
}

/* --- the whole install -------------------------------------------------- */

ZTEST(coprocessor_updater, test_install_runs_every_phase_and_confirms_the_version)
{
	static const enum coprocessor_update_phase phases[] = {
		PREFLIGHT, PREFLIGHT, ENTERING, BEGIN, WRITING, VERIFYING, RECONNECT, HEALTH, COMPLETE,
	};
	static const enum fake_update_call order[] = {
		FUP_IMAGE_OPEN, FUP_OPEN,        FUP_BEGIN, FUP_IMAGE_READ, FUP_WRITE,
		FUP_IMAGE_READ, FUP_WRITE,       FUP_IMAGE_READ, FUP_WRITE, FUP_FINISH,
		FUP_IMAGE_CLOSE, FUP_ARM, FUP_CLOSE, FUP_EVIDENCE, FUP_RESTART,
	};
	struct job_snapshot snap;
	struct coprocessor_updater_state s;

	start_and_run();

	snap = job();
	zassert_equal(snap.state, JOB_STATE_SUCCEEDED);
	zassert_str_equal(snap.phase, "complete");
	zassert_false(snap.cancellable);

	s = state();
	zassert_false(s.active);
	zassert_true(s.has_last);
	zassert_equal(s.last.state, COPROCESSOR_UPDATE_SUCCEEDED);
	zassert_str_equal(s.last.version, "3.0.6", "the transport's version, not the file's");
	zassert_false(s.last.recovery_required);
	zassert_false(s.last.has_error);
	zassert_true(s.last.uart_evidence);
	zassert_false(s.last.stm32_restart_needed);
	zassert_equal(s.last.phase, COMPLETE);
	zassert_str_equal(s.last.job_id, job_id);

	zassert_equal(fake_update.begin_size, 10000);
	zassert_equal(fake_update.written, 10000);
	zassert_equal(fake_update.mismatches, 0, "the image's bytes, in order");
	zassert_equal(fake_update.restart_timeout, CONFIG_COPROCESSOR_UPDATER_HEALTH_TIMEOUT_MS);
	zassert_equal(fake_update.count[FUP_CLOSE], 1);
	zassert_equal(fake_update.outside_session, 0);
	zassert_str_equal(fake_update.upload_id, "upload_1");
	zassert_equal(fake_update.overreads, 0, "never asks for bytes past the end");

	zassert_equal(fake_update.call_count, ARRAY_SIZE(order));
	for (size_t i = 0; i < ARRAY_SIZE(order); i++) {
		zassert_equal(fake_update.calls[i], order[i], "call %zu", i);
	}

	/* The journal on "disk": every phase, then the outcome. */
	zassert_equal(fake_update.save_count, ARRAY_SIZE(phases) + 1);
	for (size_t i = 0; i < ARRAY_SIZE(phases); i++) {
		zassert_true(fake_update.saves[i].active, "save %zu", i);
		zassert_equal(fake_update.saves[i].phase, phases[i], "save %zu", i);
	}
	zassert_false(fake_update.saves[ARRAY_SIZE(phases)].active);
	zassert_equal(fake_update.journal.last.state, COPROCESSOR_UPDATE_SUCCEEDED);

	/* Once run, an install is not run again. */
	coprocessor_updater_run();
	zassert_equal(fake_update.call_count, ARRAY_SIZE(order));
	zassert_equal(fake_update.save_count, ARRAY_SIZE(phases) + 1);
}

ZTEST(coprocessor_updater, test_the_longest_upload_id_reaches_the_image_whole)
{
	char upload[COPROCESSOR_UPDATE_UPLOAD_ID_MAX_LEN + 1];

	memset(upload, 'u', COPROCESSOR_UPDATE_UPLOAD_ID_MAX_LEN);
	upload[COPROCESSOR_UPDATE_UPLOAD_ID_MAX_LEN] = '\0';
	create_job();
	zassert_ok(coprocessor_updater_start(upload, job_id));
	coprocessor_updater_run();
	zassert_str_equal(fake_update.upload_id, upload);
	zassert_equal(job().state, JOB_STATE_SUCCEEDED);
}

static struct job_snapshot hooked;

static void snapshot_hook(void *arg)
{
	ARG_UNUSED(arg);
	zassert_ok(job_get(job_id, &hooked));
}

ZTEST(coprocessor_updater, test_writing_reports_bytes_and_verifying_starts_from_zero)
{
	start();
	fake_update.hook_call = FUP_FINISH;
	fake_update.hook = snapshot_hook;
	coprocessor_updater_run();
	zassert_str_equal(hooked.phase, "verifying");
	zassert_equal(hooked.progress.completed, 0, "progress is per phase");

	fake_update_init();
	zassert_ok(coprocessor_updater_init(&fake_update_platform));
	start();
	fake_update.hook_call = FUP_IMAGE_CLOSE;
	fake_update.hook = snapshot_hook;
	coprocessor_updater_run();
	zassert_str_equal(hooked.phase, "reconnecting");

	fake_update_init();
	zassert_ok(coprocessor_updater_init(&fake_update_platform));
	fake_update.image_size = 3 * CONFIG_COPROCESSOR_UPDATER_READ_BLOCK + 17;
	start();
	fake_update.write_fail_at = 0;
	fake_update.hook_call = FUP_FINISH;
	fake_update.hook = NULL;
	coprocessor_updater_run();
	zassert_equal(fake_update.written, 3 * CONFIG_COPROCESSOR_UPDATER_READ_BLOCK + 17);
	zassert_equal(fake_update.count[FUP_WRITE], 4);
}

static void check_writing_progress(void *arg)
{
	ARG_UNUSED(arg);
	zassert_ok(job_get(job_id, &hooked));
	zassert_str_equal(hooked.phase, "writing");
	zassert_equal(hooked.progress.unit, JOB_PROGRESS_UNIT_BYTES);
	zassert_true(hooked.progress.total_known);
	zassert_equal(hooked.progress.total, fake_update.image_size);
	zassert_equal(hooked.progress.completed, fake_update.written, "after each block");
}

ZTEST(coprocessor_updater, test_writing_progress_follows_the_blocks)
{
	fake_update.image_size = 2 * CONFIG_COPROCESSOR_UPDATER_READ_BLOCK + 1;
	start();
	/* The third read sees the progress of the first two writes. */
	fake_update.hook_call = FUP_IMAGE_READ;
	fake_update.hook_nth = 3;
	fake_update.hook = check_writing_progress;
	coprocessor_updater_run();
	zassert_equal(hooked.progress.completed, 2 * CONFIG_COPROCESSOR_UPDATER_READ_BLOCK);
	zassert_equal(job().state, JOB_STATE_SUCCEEDED);
	zassert_equal(job().progress.completed, 0, "complete has its own, empty progress");
}

static void check_writing_starts_at_zero(void *arg)
{
	ARG_UNUSED(arg);
	zassert_ok(job_get(job_id, &hooked));
	zassert_str_equal(hooked.phase, "writing");
	zassert_equal(hooked.progress.unit, JOB_PROGRESS_UNIT_BYTES, "bytes from the start");
	zassert_true(hooked.progress.total_known, "the size is known before the first block");
	zassert_equal(hooked.progress.total, fake_update.image_size);
	zassert_equal(hooked.progress.completed, 0);
}

ZTEST(coprocessor_updater, test_writing_progress_is_set_before_the_first_block)
{
	start();
	fake_update.hook_call = FUP_IMAGE_READ;
	fake_update.hook_nth = 1;
	fake_update.hook = check_writing_starts_at_zero;
	coprocessor_updater_run();
	zassert_equal(job().state, JOB_STATE_SUCCEEDED);
	zassert_is_null(fake_update.hook, "the hook ran");
}

ZTEST(coprocessor_updater, test_offline_coprocessor_does_not_block_an_install)
{
	fake_uart.transport_ready = false;
	zassert_ok(coprocessor_updater_check());
	start_and_run();
	zassert_equal(job().state, JOB_STATE_SUCCEEDED);
}

ZTEST(coprocessor_updater, test_last_update_survives_the_next_install_until_it_ends)
{
	fake_update.rc[FUP_OPEN] = -EIO;
	start_and_run();
	zassert_equal(state().last.state, COPROCESSOR_UPDATE_FAILED);

	fake_update.rc[FUP_OPEN] = 0;
	start();
	zassert_true(state().has_last, "the old outcome stays visible while the new one runs");
	zassert_equal(state().last.state, COPROCESSOR_UPDATE_FAILED);
	zassert_true(fake_update.journal.has_last);
	coprocessor_updater_run();
	zassert_equal(state().last.state, COPROCESSOR_UPDATE_SUCCEEDED);
}

/* --- cancel ------------------------------------------------------------- */

ZTEST(coprocessor_updater, test_cancel_while_queued_does_nothing_to_the_chip)
{
	start();
	zassert_ok(job_cancel(job_id));
	coprocessor_updater_run();
	zassert_equal(job().state, JOB_STATE_CANCELLED);
	zassert_equal(fake_update.count[FUP_IMAGE_OPEN], 0);
	zassert_equal(fake_update.count[FUP_OPEN], 0);
	zassert_false(state().active);
	zassert_false(state().has_last, "a cancel is not an update outcome");
	zassert_false(fake_update.journal.active);
	zassert_equal(fake_update.save_count, 2, "saved at start and at the end, no phase between");
}

static void cancel_hook(void *arg)
{
	int *rc = arg;

	*rc = job_cancel(job_id);
}

ZTEST(coprocessor_updater, test_cancel_during_preflight_stops_before_the_rom_loader)
{
	int rc = 1;

	start();
	fake_update.hook_call = FUP_IMAGE_OPEN;
	fake_update.hook = cancel_hook;
	fake_update.hook_arg = &rc;
	coprocessor_updater_run();
	zassert_ok(rc, "cancellable during preflight");
	zassert_equal(job().state, JOB_STATE_CANCELLED);
	zassert_equal(fake_update.count[FUP_OPEN], 0);
	zassert_false(fake_update.image_open);
	zassert_false(state().has_last);
	zassert_false(state().active);
}

ZTEST(coprocessor_updater, test_cancel_as_preflight_starts_opens_nothing)
{
	int rc = 1;

	start();
	/* Save 1 is start's, save 2 is entering preflight. */
	fake_update.hook_call = FUP_SAVE;
	fake_update.hook_nth = 2;
	fake_update.hook = cancel_hook;
	fake_update.hook_arg = &rc;
	coprocessor_updater_run();
	zassert_ok(rc);
	zassert_equal(job().state, JOB_STATE_CANCELLED);
	zassert_equal(fake_update.count[FUP_IMAGE_OPEN], 0, "checked before the image is opened");
	zassert_false(state().active);
	zassert_false(state().has_last);
}

ZTEST(coprocessor_updater, test_cancel_while_connecting_closes_the_session_before_begin)
{
	int rc = 1;

	start();
	fake_update.hook_call = FUP_OPEN;
	fake_update.hook = cancel_hook;
	fake_update.hook_arg = &rc;
	coprocessor_updater_run();
	zassert_ok(rc, "cancellable while entering the bootloader");
	zassert_equal(job().state, JOB_STATE_CANCELLED);
	zassert_equal(fake_update.count[FUP_BEGIN], 0, "nothing erased");
	zassert_equal(fake_update.count[FUP_CLOSE], 1);
	zassert_false(fake_update.loader_open);
	zassert_false(fake_update.image_open);
	zassert_false(state().has_last);
	zassert_false(state().active);
}

static void snapshot_and_cancel(void *arg)
{
	int *rc = arg;

	zassert_ok(job_get(job_id, &hooked));
	*rc = job_cancel(job_id);
}

ZTEST(coprocessor_updater, test_job_is_cancellable_until_begin)
{
	int rc = 1;

	start();
	zassert_true(job().cancellable);
	fake_update.hook_call = FUP_OPEN;
	fake_update.hook = snapshot_hook;
	coprocessor_updater_run();
	zassert_true(hooked.cancellable, "still cancellable while connecting");

	fake_update_init();
	zassert_ok(coprocessor_updater_init(&fake_update_platform));
	start();
	fake_update.hook_call = FUP_BEGIN;
	fake_update.hook = snapshot_and_cancel;
	fake_update.hook_arg = &rc;
	coprocessor_updater_run();
	zassert_false(hooked.cancellable, "not once begin started");
	zassert_equal(rc, -EPERM, "409 invalid_state for the binding");
	zassert_equal(job().state, JOB_STATE_SUCCEEDED, "the install went on");
}

/* --- preflight ---------------------------------------------------------- */

ZTEST(coprocessor_updater, test_missing_image_fails_preflight_without_recovery)
{
	fake_update.rc[FUP_IMAGE_OPEN] = -ENOENT;
	start_and_run();
	assert_failed(PREFLIGHT, "not_found", false);
	zassert_false(state().last.error_retryable);
	zassert_not_null(strstr(state().last.error_message, "no longer exists"));
	zassert_equal(fake_update.count[FUP_OPEN], 0);
}

ZTEST(coprocessor_updater, test_image_not_ready_fails_preflight)
{
	fake_update.rc[FUP_IMAGE_OPEN] = -EBUSY;
	start_and_run();
	assert_failed(PREFLIGHT, "invalid_state", false);
	zassert_not_null(strstr(state().last.error_message, "not ready"));
	zassert_false(state().last.error_retryable);
}

ZTEST(coprocessor_updater, test_empty_image_fails_preflight_and_is_closed)
{
	fake_update.image_size = 0;
	start_and_run();
	zassert_equal(fake_update.count[FUP_IMAGE_CLOSE], 1);
	assert_failed(PREFLIGHT, "invalid_state", false);
}

ZTEST(coprocessor_updater, test_bridge_taken_after_start_fails_preflight)
{
	start();
	zassert_ok(coprocessor_manager_set_mode(COPROCESSOR_UART_USB_BRIDGE));
	coprocessor_updater_run();
	assert_failed(PREFLIGHT, "busy", false);
	zassert_true(state().last.error_retryable, "the bridge will be closed again");
	zassert_equal(fake_update.count[FUP_OPEN], 0);
}

ZTEST(coprocessor_updater, test_preflight_timeout)
{
	fake_update.cost_ms[FUP_IMAGE_OPEN] = CONFIG_COPROCESSOR_UPDATER_PREFLIGHT_TIMEOUT_MS + 1;
	start_and_run();
	assert_failed(PREFLIGHT, "internal_error", false);
	assert_timeout_message("preflight");
	zassert_equal(fake_update.count[FUP_OPEN], 0);
}

ZTEST(coprocessor_updater, test_preflight_at_the_limit_is_not_a_timeout)
{
	fake_update.cost_ms[FUP_IMAGE_OPEN] = CONFIG_COPROCESSOR_UPDATER_PREFLIGHT_TIMEOUT_MS;
	start_and_run();
	zassert_equal(job().state, JOB_STATE_SUCCEEDED);
}

/* --- entering_bootloader ------------------------------------------------ */

ZTEST(coprocessor_updater, test_rom_loader_silence_fails_without_recovery)
{
	fake_update.rc[FUP_OPEN] = -EIO;
	fake_update.err_code[FUP_OPEN] = "service_not_ready";
	start_and_run();
	assert_failed(ENTERING, "service_not_ready", false);
	zassert_equal(fake_update.count[FUP_CLOSE], 0, "a failed open cleaned up itself");
	zassert_equal(fake_update.count[FUP_BEGIN], 0);
}

ZTEST(coprocessor_updater, test_other_chip_keeps_its_code)
{
	fake_update.rc[FUP_OPEN] = -ENOTSUP;
	fake_update.err_code[FUP_OPEN] = "unsupported_target";
	start_and_run();
	assert_failed(ENTERING, "unsupported_target", false);
}

ZTEST(coprocessor_updater, test_entering_bootloader_timeout_closes_the_session)
{
	fake_update.cost_ms[FUP_OPEN] = CONFIG_COPROCESSOR_UPDATER_CONNECT_TIMEOUT_MS + 1;
	start_and_run();
	assert_failed(ENTERING, "internal_error", false);
	assert_timeout_message("entering_bootloader");
	zassert_equal(fake_update.count[FUP_CLOSE], 1);
	zassert_equal(fake_update.count[FUP_BEGIN], 0);
}

/* --- begin, writing, verifying ------------------------------------------ */

ZTEST(coprocessor_updater, test_refused_begin_needs_recovery)
{
	fake_update.rc[FUP_BEGIN] = -EIO;
	start_and_run();
	assert_failed(BEGIN, "internal_error", true);
	zassert_equal(fake_update.count[FUP_CLOSE], 1);
	zassert_equal(fake_update.count[FUP_WRITE], 0);
}

ZTEST(coprocessor_updater, test_begin_timeout_needs_recovery)
{
	fake_update.cost_ms[FUP_BEGIN] = CONFIG_COPROCESSOR_UPDATER_BEGIN_TIMEOUT_MS + 1;
	start_and_run();
	assert_failed(BEGIN, "internal_error", true);
	assert_timeout_message("begin");
}

ZTEST(coprocessor_updater, test_block_refused_in_the_middle_needs_recovery)
{
	fake_update.image_size = 3 * CONFIG_COPROCESSOR_UPDATER_READ_BLOCK;
	fake_update.rc[FUP_WRITE] = -EIO;
	fake_update.write_fail_at = 2;
	start_and_run();
	assert_failed(WRITING, "internal_error", true);
	zassert_equal(fake_update.count[FUP_WRITE], 2);
	zassert_equal(fake_update.count[FUP_FINISH], 0);
	zassert_equal(fake_update.count[FUP_CLOSE], 1, "normal boot even of a half-written chip");
}

ZTEST(coprocessor_updater, test_unreadable_image_needs_recovery)
{
	fake_update.image_size = 3 * CONFIG_COPROCESSOR_UPDATER_READ_BLOCK;
	fake_update.read_fail_at = CONFIG_COPROCESSOR_UPDATER_READ_BLOCK;
	start_and_run();
	assert_failed(WRITING, "internal_error", true);
	zassert_not_null(strstr(state().last.error_message, "could not be read"));
	zassert_true(state().last.error_retryable);
	zassert_equal(fake_update.written, CONFIG_COPROCESSOR_UPDATER_READ_BLOCK);
}

ZTEST(coprocessor_updater, test_image_that_returns_nothing_is_an_error_not_a_loop)
{
	fake_update.image_size = 3 * CONFIG_COPROCESSOR_UPDATER_READ_BLOCK;
	fake_update.read_zero_at = CONFIG_COPROCESSOR_UPDATER_READ_BLOCK;
	fake_update.cost_ms[FUP_IMAGE_READ] = 1000;
	start_and_run();
	assert_failed(WRITING, "internal_error", true);
	zassert_not_null(strstr(state().last.error_message, "could not be read"),
			 "%s", state().last.error_message);
	zassert_equal(fake_update.count[FUP_IMAGE_READ], 2);
}

ZTEST(coprocessor_updater, test_short_read_past_the_request_is_an_error)
{
	fake_update.rc[FUP_IMAGE_READ] = CONFIG_COPROCESSOR_UPDATER_READ_BLOCK + 1;
	start_and_run();
	assert_failed(WRITING, "internal_error", true);
	zassert_equal(fake_update.count[FUP_WRITE], 0);
}

ZTEST(coprocessor_updater, test_writing_timeout_needs_recovery)
{
	fake_update.image_size = 2 * CONFIG_COPROCESSOR_UPDATER_READ_BLOCK;
	fake_update.cost_ms[FUP_WRITE] = CONFIG_COPROCESSOR_UPDATER_WRITE_TIMEOUT_S * 1000LL;
	start_and_run();
	assert_failed(WRITING, "internal_error", true);
	assert_timeout_message("writing");
	zassert_equal(fake_update.count[FUP_WRITE], 2, "the limit is for all blocks together");
}

ZTEST(coprocessor_updater, test_md5_failure_needs_recovery)
{
	fake_update.rc[FUP_FINISH] = -EIO;
	start_and_run();
	assert_failed(VERIFYING, "internal_error", true);
	zassert_equal(fake_update.count[FUP_CLOSE], 1);
	zassert_equal(fake_update.count[FUP_RESTART], 0);
}

ZTEST(coprocessor_updater, test_verifying_timeout_needs_recovery)
{
	fake_update.cost_ms[FUP_FINISH] = CONFIG_COPROCESSOR_UPDATER_VERIFY_TIMEOUT_MS + 1;
	start_and_run();
	assert_failed(VERIFYING, "internal_error", true);
	assert_timeout_message("verifying");
}

/* --- reconnecting ------------------------------------------------------- */

ZTEST(coprocessor_updater, test_console_not_handed_back_fails_once_closed)
{
	fake_update.rc[FUP_CLOSE] = -EIO;
	start_and_run();
	assert_failed(RECONNECT, "internal_error", true);
	zassert_true(state().last.error_retryable);
	zassert_equal(fake_update.count[FUP_CLOSE], 1, "not closed a second time");
	zassert_equal(fake_update.count[FUP_RESTART], 0);
}

ZTEST(coprocessor_updater, test_evidence_is_armed_before_the_normal_boot)
{
	start_and_run();
	zassert_true(fake_update_first(FUP_ARM) < fake_update_first(FUP_CLOSE),
		     "only what this boot prints counts");
	zassert_true(fake_update_first(FUP_IMAGE_CLOSE) < fake_update_first(FUP_CLOSE));
}

ZTEST(coprocessor_updater, test_late_evidence_is_waited_for)
{
	fake_update.evidence_after = 5;
	start_and_run();
	zassert_equal(job().state, JOB_STATE_SUCCEEDED);
	zassert_true(state().last.uart_evidence);
	zassert_equal(fake_update.count[FUP_EVIDENCE], 6);
	zassert_equal(fake_update.sleeps, 5);
}

ZTEST(coprocessor_updater, test_silent_console_goes_on_to_the_health_check)
{
	fake_update.evidence_after = -1;
	start_and_run();
	zassert_equal(job().state, JOB_STATE_SUCCEEDED, "the version is the proof");
	zassert_false(state().last.uart_evidence);
	zassert_equal(fake_update.count[FUP_RESTART], 1);
	zassert_equal(fake_update.sleeps, CONFIG_COPROCESSOR_UPDATER_RECONNECT_TIMEOUT_MS /
						  CONFIG_COPROCESSOR_UPDATER_POLL_MS,
		      "polled until the reconnect limit");
}

/* --- health_check ------------------------------------------------------- */

ZTEST(coprocessor_updater, test_no_answer_over_esp_hosted_needs_recovery)
{
	fake_update.rc[FUP_RESTART] = -ETIMEDOUT;
	fake_update.restart_version = "";
	start_and_run();
	assert_failed(HEALTH, "service_not_ready", true);
	zassert_not_null(strstr(state().last.error_message, "ESP-Hosted"));
	zassert_true(state().last.error_retryable, "the C6 may just be slow to start");
	zassert_equal(fake_update.count[FUP_CLOSE], 1);
}

ZTEST(coprocessor_updater, test_answer_without_a_version_is_not_health)
{
	fake_update.restart_version = "";
	start_and_run();
	assert_failed(HEALTH, "service_not_ready", true);
	zassert_not_null(strstr(state().last.error_message, "version"));
}

ZTEST(coprocessor_updater, test_other_restart_error_is_not_health)
{
	fake_update.rc[FUP_RESTART] = -EIO;
	start_and_run();
	assert_failed(HEALTH, "service_not_ready", true);
}

ZTEST(coprocessor_updater, test_wifi_device_dead_since_boot_succeeds_and_says_restart)
{
	fake_update.rc[FUP_RESTART] = -ENODEV;
	start_and_run();
	zassert_equal(job().state, JOB_STATE_SUCCEEDED);
	zassert_true(state().last.stm32_restart_needed);
	zassert_str_equal(state().last.version, "3.0.6");
	zassert_false(state().last.recovery_required);
}

ZTEST(coprocessor_updater, test_wifi_device_dead_and_no_version_fails)
{
	fake_update.rc[FUP_RESTART] = -ENODEV;
	fake_update.restart_version = "";
	start_and_run();
	assert_failed(HEALTH, "service_not_ready", true);
	zassert_false(state().last.stm32_restart_needed);
}

ZTEST(coprocessor_updater, test_health_check_timeout_needs_recovery)
{
	fake_update.cost_ms[FUP_RESTART] = CONFIG_COPROCESSOR_UPDATER_HEALTH_TIMEOUT_MS + 1;
	fake_update.rc[FUP_RESTART] = -ENODEV;
	start_and_run();
	assert_failed(HEALTH, "internal_error", true);
	assert_timeout_message("health_check");
	zassert_false(state().last.stm32_restart_needed, "no half-success on a timeout");
}

/* --- the lock ----------------------------------------------------------- */

K_THREAD_STACK_DEFINE(probe_stack, 2048);
static struct k_thread probe_thread;
static K_SEM_DEFINE(probe_done, 0, 1);

static void probe(void *a, void *b, void *c)
{
	struct coprocessor_updater_state s;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	coprocessor_updater_get_state(&s);
	k_sem_give(&probe_done);
}

/* Another thread can read the state: nothing left the mutex held. */
static void assert_lock_free(const char *after)
{
	int rc;

	k_sem_reset(&probe_done);
	k_thread_create(&probe_thread, probe_stack, K_THREAD_STACK_SIZEOF(probe_stack), probe, NULL,
			NULL, NULL, K_PRIO_PREEMPT(1), 0, K_NO_WAIT);
	rc = k_sem_take(&probe_done, K_MSEC(200));
	if (rc != 0) {
		k_thread_abort(&probe_thread);
	}
	zassert_ok(rc, "the lock was left held after %s", after);
	(void)k_thread_join(&probe_thread, K_MSEC(100));
}

ZTEST(coprocessor_updater, test_no_call_leaves_the_lock_held)
{
	struct coprocessor_updater_state s;

	assert_lock_free("init");
	coprocessor_updater_run();
	assert_lock_free("a run with nothing accepted");
	zassert_ok(coprocessor_updater_check());
	assert_lock_free("check");
	coprocessor_updater_get_state(&s);
	assert_lock_free("get_state");
	start();
	assert_lock_free("start");
	zassert_equal(coprocessor_updater_init(&fake_update_platform), -EBUSY);
	assert_lock_free("init refused while active");
	coprocessor_updater_run();
	assert_lock_free("a whole install");

	fake_update.save_rc = -EIO;
	create_job();
	zassert_equal(coprocessor_updater_start("upload_1", job_id), -EIO);
	assert_lock_free("a start whose journal failed");
	fake_update.save_rc = 0;

	restart_with_journal(WRITING);
	assert_lock_free("init of an interrupted journal");
}

/* --- names -------------------------------------------------------------- */

ZTEST(coprocessor_updater, test_wire_names)
{
	static const char *const names[] = {
		"preflight", "entering_bootloader", "begin",        "writing",
		"verifying", "reconnecting",        "health_check", "complete",
	};

	for (int i = 0; i < COPROCESSOR_UPDATE_PHASE_COUNT; i++) {
		zassert_str_equal(coprocessor_update_phase_str(i), names[i]);
	}
	zassert_is_null(coprocessor_update_phase_str(COPROCESSOR_UPDATE_PHASE_COUNT));
	zassert_is_null(coprocessor_update_phase_str(-1));
	zassert_str_equal(coprocessor_update_outcome_str(COPROCESSOR_UPDATE_SUCCEEDED), "succeeded");
	zassert_str_equal(coprocessor_update_outcome_str(COPROCESSOR_UPDATE_FAILED), "failed");
	zassert_str_equal(coprocessor_update_outcome_str(COPROCESSOR_UPDATE_INTERRUPTED),
			  "interrupted");
	zassert_is_null(coprocessor_update_outcome_str(COPROCESSOR_UPDATE_INTERRUPTED + 1));
}
