/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Job manager: identity, lifecycle and idempotency for every long-running
 * operation the web API exposes. A request that cannot finish inside an HTTP
 * handler creates a job, returns 202 with its id, and the caller polls it.
 *
 * The contract this implements is docs/device-development/api-contract.md,
 * section "Задачи", and the Job schema in openapi.json. Two rules from there
 * shape the whole design:
 *
 *  - 202 is not success. A job carries its own terminal outcome, including
 *    the error detail, so a failure discovered by hardware minutes later is
 *    never retro-fitted into an HTTP status that was already sent.
 *  - Repeating a request must not repeat the action. The same idempotency
 *    key with the same body returns the same job; the same key with a
 *    different body is a conflict, never a silent second execution.
 *
 * Design notes worth knowing before using this:
 *
 *  - No dynamic allocation. Everything lives in fixed pools sized by Kconfig,
 *    so behaviour under exhaustion is a defined error rather than a heap
 *    failure at the worst moment.
 *  - Two retention tiers. Full records (CONFIG_JOB_MANAGER_MAX_JOBS) hold the
 *    complete job. When one is evicted, a much smaller compact record
 *    (CONFIG_JOB_MANAGER_DEDUPE_SLOTS) keeps the id, the key and the outcome
 *    alive so a late retry still gets a truthful answer instead of a new
 *    action. Active jobs are never evicted from either tier.
 *  - The clock is injectable. Everything time-related goes through a function
 *    pointer defaulting to k_uptime_get(), so retention and expiry are
 *    testable without sleeping. This is the "pure core, thin adapter" split
 *    that section 12 of the development plan requires.
 *  - Ids are opaque to callers and match ^[A-Za-z0-9_-]{1,64}$ so they can go
 *    straight into a URL. Do not parse them; a slot index is an internal
 *    detail and is deliberately not recoverable from the string.
 *
 * Threading: every entry point takes an internal mutex. Snapshots are copied
 * out under that lock, so a caller can format JSON from one without holding
 * anything. Do not call into the job manager from an ISR.
 */

#ifndef JOB_MANAGER_H_
#define JOB_MANAGER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum length of a job id string, excluding the terminator. */
#define JOB_ID_MAX_LEN 31
/**
 * Maximum length of an idempotency key.
 *
 * Not the contract's 64: the key a caller stores here is the client's key
 * scoped to the operation it was sent to (web-api prefixes a 16-digit digest of
 * principal, method and URL and a colon, so the same client key on two
 * operations names two actions, as the contract's scope rule says). 96 holds
 * that with room to spare.
 */
#define JOB_IDEMPOTENCY_KEY_MAX_LEN 96
/** Maximum length of a phase name, e.g. "writing", "health_check". */
#define JOB_PHASE_MAX_LEN 23
/** Maximum length of a machine-readable error code, e.g. "storage_full". */
#define JOB_ERROR_CODE_MAX_LEN 31

/**
 * @brief What kind of work a job represents.
 *
 * Mirrors the `kind` enum in openapi.json. Kept as an enum rather than a
 * free string so an unknown kind cannot reach the wire.
 */
enum job_kind {
	JOB_KIND_MATTER_OPEN = 0,
	JOB_KIND_MATTER_CLOSE,
	JOB_KIND_NETWORK_APPLY,
	JOB_KIND_NETWORK_DISCARD,
	JOB_KIND_WIFI_SCAN,
	JOB_KIND_UPLOAD_CHUNK,
	JOB_KIND_FIRMWARE_VERIFY,
	JOB_KIND_FIRMWARE_DELETE,
	JOB_KIND_COPROCESSOR_UPDATE,
	JOB_KIND_PASSWORD_CHANGE,

	JOB_KIND_COUNT
};

/**
 * @brief Lifecycle state.
 *
 * Legal transitions, enforced by job_set_state():
 *
 *   QUEUED  -> RUNNING | CANCELLED | FAILED | INTERRUPTED
 *   RUNNING -> WAITING_CONFIRMATION | SUCCEEDED | FAILED | CANCELLED | INTERRUPTED
 *   WAITING_CONFIRMATION -> RUNNING | SUCCEEDED | FAILED | CANCELLED | INTERRUPTED
 *   terminal states -> nothing
 *
 * Not every kind uses WAITING_CONFIRMATION; only those with a confirm step
 * (network apply, and a coprocessor update that must be confirmed after
 * reboot) ever enter it. Leaving it for RUNNING is the job working again once
 * the wait is over — a network apply writing its commit, or restoring the
 * previous configuration — which a client polls as `running`, as the mock
 * shows it.
 */
enum job_state {
	JOB_STATE_QUEUED = 0,
	JOB_STATE_RUNNING,
	JOB_STATE_WAITING_CONFIRMATION,
	JOB_STATE_SUCCEEDED,
	JOB_STATE_FAILED,
	JOB_STATE_CANCELLED,
	JOB_STATE_INTERRUPTED,

	JOB_STATE_COUNT
};

/** Unit for the progress counters. */
enum job_progress_unit {
	JOB_PROGRESS_UNIT_NONE = 0,
	JOB_PROGRESS_UNIT_BYTES,
	JOB_PROGRESS_UNIT_RECORDS,
	JOB_PROGRESS_UNIT_PERCENT,
};

/**
 * @brief Progress within the CURRENT phase.
 *
 * Deliberately not a monotonic overall percentage: progress restarts at zero
 * when the phase changes, and the API contract tells clients not to render it
 * as one continuous bar. @p total_known false means the size is not yet known
 * and serialises as `total: null`.
 */
struct job_progress {
	uint64_t completed;
	uint64_t total;
	bool total_known;
	enum job_progress_unit unit;
};

/** Terminal failure detail, carried in the job rather than in an HTTP status. */
struct job_error {
	char code[JOB_ERROR_CODE_MAX_LEN + 1];
	bool retryable;
};

/**
 * @brief A copied-out view of a job. Never points into manager state.
 */
struct job_snapshot {
	char id[JOB_ID_MAX_LEN + 1];
	enum job_kind kind;
	enum job_state state;
	char phase[JOB_PHASE_MAX_LEN + 1];
	struct job_progress progress;
	bool cancellable;
	int64_t created_uptime_ms;
	int64_t updated_uptime_ms;
	/** Valid only when state is JOB_STATE_FAILED. */
	struct job_error error;
	bool has_error;
	/**
	 * True when the full record was evicted and this snapshot was rebuilt
	 * from the compact retention tier. Id, kind, state and error are
	 * truthful; phase and progress are not retained.
	 */
	bool compact;
};

/** Result of asking for a job by idempotency key. */
/** What job_find_by_key() found. */
enum job_lookup_result {
	/** No job carries this key. */
	JOB_LOOKUP_NONE = 0,
	/** A job carries this key with the same request hash; it is in @p out. */
	JOB_LOOKUP_EXISTING,
	/** A job carries this key for a different request. */
	JOB_LOOKUP_CONFLICT,
};

enum job_create_result {
	/** A new job was created. */
	JOB_CREATE_NEW = 0,
	/** The same key and body already exist; @p out is that job. */
	JOB_CREATE_EXISTING,
	/** The key exists with a different body. Answer 409 idempotency_conflict. */
	JOB_CREATE_CONFLICT,
	/** No capacity left. Answer 429 and let the caller retry later. */
	JOB_CREATE_EXHAUSTED,
	/** Arguments rejected. */
	JOB_CREATE_INVALID,
};

/** Parameters for creating a job. */
struct job_create_params {
	enum job_kind kind;
	bool cancellable;
	/**
	 * Idempotency key from the request, or NULL for internally-originated
	 * work that has no key. A NULL key never matches anything and never
	 * occupies a dedupe slot.
	 */
	const char *idempotency_key;
	/**
	 * Hash of the canonical request (method, URL, significant query and
	 * body). Two requests with the same key must have the same hash or the
	 * second one is a conflict. Ignored when idempotency_key is NULL.
	 */
	uint32_t request_hash;
};

/**
 * @brief Replace the clock used for timestamps and retention.
 *
 * Intended for tests. Pass NULL to restore k_uptime_get(). Changing the clock
 * does not rewrite timestamps already recorded.
 */
void job_manager_set_clock(int64_t (*clock_fn)(void));

/**
 * @brief Reset the manager to an empty state.
 *
 * Safe to call more than once; tests use it between cases. In production it
 * runs once at boot, which is also what makes every job id unique only within
 * a boot — the API pairs ids with a boot id for exactly that reason.
 */
void job_manager_init(void);

/**
 * @brief Create a job, or return the one an identical earlier request made.
 *
 * @param params  What to create and how to deduplicate it.
 * @param out     Receives the new or existing job. May be NULL.
 * @return one of @ref job_create_result.
 */
enum job_create_result job_create(const struct job_create_params *params,
				  struct job_snapshot *out);

/**
 * @brief Look a key up without creating anything.
 *
 * job_create() already answers a retry with the original job. This exists for
 * the handler that has to check something expensive or refusable before it may
 * create a job at all - a password change verifies the current password first.
 * Creating the job before that check would record a refusal under the client's
 * key, and the client's corrected retry would be answered with the refusal;
 * the contract wants a rejected request to leave nothing behind. So the handler
 * looks first, checks, and only then creates.
 *
 * Searches full records before compact ones, and expires compact records past
 * retention first, exactly as job_create() does.
 *
 * @param key           The (scoped) idempotency key; NULL or empty finds nothing.
 * @param request_hash  Hash of the request body.
 * @param out           The job on JOB_LOOKUP_EXISTING; may be NULL.
 */
enum job_lookup_result job_find_by_key(const char *key, uint32_t request_hash,
				       struct job_snapshot *out);

/**
 * @brief Advance a job's state.
 *
 * @retval 0        transition applied
 * @retval -ENOENT  no such job, or only a compact record survives
 * @retval -EINVAL  transition not legal from the current state
 */
int job_set_state(const char *id, enum job_state state);

/**
 * @brief Finish a job as failed, recording why.
 *
 * Separate from job_set_state() so that a failure can never be recorded
 * without its reason.
 */
int job_fail(const char *id, const char *error_code, bool retryable);

/**
 * @brief Name the current phase and reset progress to zero.
 *
 * Resetting is deliberate: progress is per-phase, and carrying a stale
 * counter across a phase boundary is how bars go backwards on screen.
 */
int job_set_phase(const char *id, const char *phase);

/** @brief Update progress within the current phase. */
int job_set_progress(const char *id, uint64_t completed, uint64_t total,
		     bool total_known, enum job_progress_unit unit);

/**
 * @brief Copy out a job by id, including compact records.
 *
 * @retval 0        found
 * @retval -ENOENT  unknown id, or retention has expired
 */
int job_get(const char *id, struct job_snapshot *out);

/**
 * @brief Request cancellation.
 *
 * @retval 0        moved to JOB_STATE_CANCELLED
 * @retval -ENOENT  unknown id
 * @retval -EPERM   the job declared itself not cancellable
 * @retval -EINVAL  already terminal
 */
int job_cancel(const char *id);

/**
 * @brief Change whether a running job may still be cancelled.
 *
 * A job that is cancellable while it only prepares and stops being so once it
 * starts something destructive (a coprocessor update at `begin`) needs the
 * change and the check for a cancellation that already happened to be one
 * step: under the manager's lock, a cancel either lands before this call -
 * which then reports it - or is refused with -EPERM afterwards.
 *
 * @retval 0        applied
 * @retval -ENOENT  unknown id, or only a compact record survives
 * @retval -EINVAL  the job is terminal (for example it was just cancelled)
 */
int job_set_cancellable(const char *id, bool cancellable);

/** @brief Number of jobs currently in a non-terminal state. */
size_t job_active_count(void);

/**
 * @brief Copy out the ids of the jobs currently in a non-terminal state.
 *
 * `GET /system/status` publishes these as `active_job_ids`, so that a browser
 * which reloads in the middle of an operation can find the job it lost track
 * of instead of starting a second one. The ids come from one pass under the
 * lock, so the list is a consistent snapshot even while workers move jobs on.
 *
 * Order is the order of the internal pool, which is not creation order.
 *
 * @param ids  Destination array of @p max id buffers.
 * @param max  Capacity of @p ids.
 * @return the number of active jobs, which may exceed @p max; only the first
 *         @p max are copied. A caller that wants to know whether it saw all of
 *         them compares the two.
 */
size_t job_active_ids(char ids[][JOB_ID_MAX_LEN + 1], size_t max);

/** @brief True when the job is in a terminal state. */
bool job_state_is_terminal(enum job_state state);

/** @brief Stable lowercase wire name, e.g. "coprocessor_update". */
const char *job_kind_str(enum job_kind kind);
/** @brief Stable lowercase wire name, e.g. "waiting_confirmation". */
const char *job_state_str(enum job_state state);
/** @brief Stable lowercase wire name, e.g. "bytes"; NONE maps to NULL. */
const char *job_progress_unit_str(enum job_progress_unit unit);

#ifdef __cplusplus
}
#endif

#endif /* JOB_MANAGER_H_ */
