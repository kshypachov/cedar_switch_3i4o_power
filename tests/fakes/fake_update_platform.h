/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Everything coprocessor-updater asks the board for, in RAM and on demand.
 *
 * The updater's rules are about what happens when a step fails or takes too
 * long - each of eight phases - and about what survives an STM32 restart. The
 * fake loader, image, journal, console evidence and ESP-Hosted restart can
 * each fail, spend clock time inside a call, or run a test's hook in the middle
 * of a call (a cancel arriving while the ROM loader connects). Calls are
 * recorded in order, and every journal save is kept, so the phase sequence on
 * "disk" is a test too.
 */

#ifndef FAKE_UPDATE_PLATFORM_H_
#define FAKE_UPDATE_PLATFORM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <coprocessor_updater/coprocessor_updater.h>

enum fake_update_call {
	FUP_OPEN = 0,
	FUP_BEGIN,
	FUP_WRITE,
	FUP_FINISH,
	FUP_CLOSE,
	FUP_IMAGE_OPEN,
	FUP_IMAGE_READ,
	FUP_IMAGE_CLOSE,
	FUP_ARM,
	FUP_EVIDENCE,
	FUP_RESTART,
	/** A journal save: counted and hookable, not recorded in calls[]. */
	FUP_SAVE,

	FUP_CALL_COUNT
};

#define FAKE_UPDATE_MAX_CALLS 64
#define FAKE_UPDATE_MAX_SAVES 32

/** Byte @p offset of the fake image. */
static inline uint8_t fake_update_image_byte(uint32_t offset)
{
	return (uint8_t)(offset * 7U + 3U);
}

struct fake_update {
	/** Calls in order; repeated writes, reads and evidence polls are recorded once. */
	enum fake_update_call calls[FAKE_UPDATE_MAX_CALLS];
	size_t call_count;
	unsigned int count[FUP_CALL_COUNT];

	/** Return value of each call. Sticky. */
	int rc[FUP_CALL_COUNT];
	/** Error code a failing loader call reports. */
	const char *err_code[FUP_CALL_COUNT];
	/** Clock time spent inside each call. */
	int64_t cost_ms[FUP_CALL_COUNT];
	/** Fail only the Nth write (1-based) with rc[FUP_WRITE]; 0: every one. */
	unsigned int write_fail_at;

	/* The image. */
	uint32_t image_size;
	uint32_t written;
	unsigned int mismatches;
	uint32_t begin_size;
	/** A read at or past this offset fails (-EIO); UINT32_MAX: never. */
	uint32_t read_fail_at;
	/** A read at or past this offset returns 0 bytes; UINT32_MAX: never. */
	uint32_t read_zero_at;
	/** Reads that asked for bytes past the end of the image. */
	unsigned int overreads;
	/** The upload id image_open was given. */
	char upload_id[COPROCESSOR_UPDATE_UPLOAD_ID_MAX_LEN + 1];
	bool image_open;

	/* The journal. */
	bool has_journal;
	struct coprocessor_update_journal journal;
	int save_rc;
	/** Every save's active flag and phase, in order. */
	struct {
		bool active;
		enum coprocessor_update_phase phase;
	} saves[FAKE_UPDATE_MAX_SAVES];
	size_t save_count;

	/* The console after the normal boot. */
	/** Polls after the arm before evidence appears; -1: never. */
	int evidence_after;
	unsigned int polls;
	const char *app_version;

	/* ESP-Hosted. */
	const char *restart_version;
	uint32_t restart_timeout;

	/* The loader session. */
	bool loader_open;
	/** Loader calls made while no session was open (begin/write/finish/close). */
	unsigned int outside_session;

	int64_t now_ms;
	unsigned int sleeps;

	/** Run inside a call of kind @ref hook_call, once. */
	void (*hook)(void *arg);
	void *hook_arg;
	enum fake_update_call hook_call;
	/** Only at the Nth call of that kind (1-based); 0: the next one. */
	unsigned int hook_nth;
};

extern struct fake_update fake_update;
extern const struct coprocessor_updater_platform fake_update_platform;

/**
 * Every call succeeds, a 10 000-byte image, no journal, evidence on the first
 * poll with application version "1", ESP-Hosted answers "3.0.6", clock at 1000.
 */
void fake_update_init(void);

/** Position of the first @p call in calls[], or -1. */
int fake_update_first(enum fake_update_call call);

#endif /* FAKE_UPDATE_PLATFORM_H_ */
