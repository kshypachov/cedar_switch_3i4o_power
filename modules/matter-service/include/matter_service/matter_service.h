/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * matter-service: what the web interface may know about, and ask of, the
 * Matter stack.
 *
 * Contract: "Matter" in docs/device-development/api-contract.md, the
 * MatterStatus, CommissioningWindow, OnboardingCodes and Fabric schemas in
 * openapi.json, and sections 3 and 6 of the development plan.
 *
 * What is decided here and why:
 *
 * - **No caller ever waits for Matter.** The HTTP server's only thread is
 *   cooperative (measured in P2: a blocking handler stops the whole board). The
 *   Matter stack, in turn, may only be touched on its own event loop. So every
 *   read in this API answers from a snapshot the event loop keeps, under a
 *   mutex held for a copy and nothing else, and every change is a request that
 *   returns at once and finishes later through a callback on the Matter thread.
 *   The HTTP binding turns that into a 202 and a job.
 *
 * - **The core holds no Matter types.** It is C, and the Matter stack reaches
 *   it through struct matter_service_platform: schedule work on the event loop,
 *   open and close the window, and read the window, the codes and the fabric
 *   table. The firmware's adapter (src/matter/matter_service_chip.cpp)
 *   implements those with the SDK; tests/matter_service implements them with a
 *   fake fabric table and a queue it drains by hand, so ordering is tested
 *   without threads.
 *
 * - **The snapshot is refreshed on events, not on a timer.** The adapter
 *   reports the stack starting, the stack started or failed, the window opened
 *   or closed, and a fabric committed, updated or removed; each report
 *   re-reads what it could have changed. The remaining time of a window is not
 *   stored: it is computed from when the window opened and its timeout, so a
 *   window that runs out needs no event to count down.
 *
 * - **Who opened the window is recorded here.** The SDK can say whether a
 *   controller opened it through the Administrator Commissioning cluster, and
 *   then whether that window is basic or enhanced. A window the device opens
 *   itself shows as "not opened via the cluster", so the service remembers the
 *   windows it opened for the web; any other window with no controller behind
 *   it was opened locally (the debug shell) and is basic.
 *
 * - **Codes are generated once, when the stack starts.** They are the device's
 *   own and do not change while it runs, and generating them held the Matter
 *   thread for 5-6 s on the board; regenerated on a window event during
 *   commissioning, that made a controller time out. A failed generation is
 *   retried when a window next needs them.
 *
 * - **Codes only for a basic window.** A basic window uses the device's own
 *   onboarding passcode, whoever opened it, so its codes are valid. An enhanced
 *   window carries a verifier from a controller; the device never knew the
 *   passcode, and showing the factory codes would send someone the wrong one.
 *   The service never logs a code.
 *
 * - **A second open is refused, not queued.** The contract forbids silently
 *   replacing another administrator's window. An open that was accepted but has
 *   not run yet counts as open, so two quick requests cannot both be accepted.
 *   Closing is allowed whoever opened the window, and closing a closed window
 *   succeeds.
 *
 * - **A fabric's id includes its root identity.** A fabric index is reused
 *   after a fabric is removed and a fabric id is chosen by the controller, so
 *   neither identifies a fabric alone. The adapter builds the id from a digest
 *   of the root public key and the fabric id.
 */

#ifndef MATTER_SERVICE_MATTER_SERVICE_H_
#define MATTER_SERVICE_MATTER_SERVICE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/autoconf.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Fabrics the snapshot holds; the SDK's own table must not be larger. */
#define MATTER_SERVICE_MAX_FABRICS CONFIG_MATTER_SERVICE_MAX_FABRICS

/** "MT:" and the base-38 payload (the SDK bounds the latter at 128). */
#define MATTER_QR_PAYLOAD_MAX_LEN 131
/** 11 digits, or 21 for the long form. */
#define MATTER_MANUAL_CODE_MAX_LEN 21
/** Always 8 digits, leading zeros kept. */
#define MATTER_PASSCODE_LEN 8
/** 16 hex digits of the root key digest, ':' and 16 of the fabric id. */
#define MATTER_FABRIC_ID_MAX_LEN 33
/** The SDK's kFabricLabelMaxLengthInBytes. */
#define MATTER_FABRIC_LABEL_MAX_LEN 32

/** The contract's bounds on a web-opened window, which the SDK's may narrow. */
#define MATTER_WINDOW_MIN_SECONDS 180
#define MATTER_WINDOW_MAX_SECONDS 900

enum matter_state {
	/** The stack has not been started: no network address yet. */
	MATTER_STATE_NOT_READY = 0,
	/** Initialisation is running; it takes seconds on this board. */
	MATTER_STATE_STARTING,
	MATTER_STATE_READY,
	/** Initialisation failed; the stack does not recover without a reboot. */
	MATTER_STATE_FAILED,
};

enum matter_window_mode {
	MATTER_WINDOW_MODE_NONE = 0,
	MATTER_WINDOW_MODE_BASIC,
	MATTER_WINDOW_MODE_ENHANCED,
};

enum matter_window_source {
	MATTER_WINDOW_SOURCE_NONE = 0,
	/** Opened through this service. */
	MATTER_WINDOW_SOURCE_WEB,
	/** Opened by a controller through the Administrator Commissioning cluster. */
	MATTER_WINDOW_SOURCE_CONTROLLER,
	/** Opened on the device some other way (the debug shell). */
	MATTER_WINDOW_SOURCE_LOCAL,
};

enum matter_codes_reason {
	MATTER_CODES_AVAILABLE = 0,
	MATTER_CODES_WINDOW_CLOSED,
	MATTER_CODES_PASSCODE_UNAVAILABLE,
	MATTER_CODES_SERVICE_NOT_READY,
	MATTER_CODES_GENERATION_FAILED,
};

struct matter_status {
	enum matter_state state;
	bool commissioned;
	uint8_t fabric_count;
	/** Why the stack failed, as the adapter reported it; 0 unless FAILED. */
	int32_t failure;
};

struct matter_window {
	bool open;
	enum matter_window_mode mode;
	enum matter_window_source source;
	/**
	 * Seconds until a window this service opened closes by itself. 0 when
	 * closed, and 0 for a window opened elsewhere: the SDK does not expose
	 * the timeout a controller or the shell asked for.
	 */
	uint32_t remaining_seconds;
	bool codes_available;
};

struct matter_codes {
	enum matter_codes_reason reason;
	char qr_payload[MATTER_QR_PAYLOAD_MAX_LEN + 1];
	char manual_pairing_code[MATTER_MANUAL_CODE_MAX_LEN + 1];
	char setup_passcode[MATTER_PASSCODE_LEN + 1];
};

struct matter_fabric {
	char id[MATTER_FABRIC_ID_MAX_LEN + 1];
	uint8_t fabric_index;
	uint64_t fabric_id;
	uint64_t node_id;
	uint16_t vendor_id;
	/** May be empty; a vendor id does not name an ecosystem either. */
	char label[MATTER_FABRIC_LABEL_MAX_LEN + 1];
};

/** What the adapter reads about the window on the Matter thread. */
struct matter_window_reading {
	bool open;
	/** True when a controller opened it through the cluster. */
	bool opened_by_controller;
	/** Meaningful with opened_by_controller: basic or enhanced. */
	enum matter_window_mode controller_mode;
};

/** Called on the Matter thread when a requested change has been attempted. */
typedef void (*matter_service_done_t)(void *ctx, int result);

/**
 * The Matter stack, as the core sees it.
 *
 * `schedule` may be called from any thread and must not block; everything else
 * is called only from work that `schedule` ran, that is on the Matter thread.
 */
struct matter_service_platform {
	/** Run @p fn(@p arg) on the Matter event loop. 0, or negative if not queued. */
	int (*schedule)(void (*fn)(void *arg), void *arg);
	/** Monotonic milliseconds. */
	int64_t (*now_ms)(void);

	/** Open a basic window. 0, -EBUSY if the stack refuses in its current
	 *  state (a commissioning in progress), -EINVAL for a timeout it will not
	 *  take, -EIO otherwise. */
	int (*open_basic_window)(uint32_t timeout_seconds);
	/** Close whatever window is open. */
	void (*close_window)(void);
	void (*read_window)(struct matter_window_reading *out);
	/** Fill QR payload, manual code and passcode for the current window. 0 or
	 *  negative. */
	int (*read_codes)(struct matter_codes *out);
	/** Copy up to @p max fabrics; returns how many. */
	size_t (*read_fabrics)(struct matter_fabric *out, size_t max);
	/** The SDK's own window limits, in seconds. */
	uint32_t (*min_window_seconds)(void);
	uint32_t (*max_window_seconds)(void);
};

enum matter_request_result {
	/** Accepted; the callback will run exactly once. */
	MATTER_REQUEST_ACCEPTED = 0,
	/** The stack is not READY. */
	MATTER_REQUEST_NOT_READY,
	/** A window is open, or an accepted open has not run yet. */
	MATTER_REQUEST_WINDOW_OPEN,
	/** The timeout is outside matter_service_window_limits(). */
	MATTER_REQUEST_OUT_OF_RANGE,
	/** Every request slot is taken; retry shortly. */
	MATTER_REQUEST_BUSY,
	/** The event loop did not take the work. */
	MATTER_REQUEST_SCHEDULE_FAILED,
};

/**
 * @brief Install the platform. Call once, before the stack starts.
 * @retval 0 or -EINVAL for a platform with a missing function.
 */
int matter_service_init(const struct matter_service_platform *platform);

/* -- Reported by the adapter ------------------------------------------ */

/** Initialisation has begun. Any thread. */
void matter_service_report_starting(void);
/** Initialisation finished: @p failure 0 for success. On the Matter thread
 *  when it succeeded (the snapshot is read then); any thread otherwise. */
void matter_service_report_started(int32_t failure);
/** The window may have opened or closed. On the Matter thread. */
void matter_service_report_window_changed(void);
/** The fabric table may have changed. On the Matter thread. */
void matter_service_report_fabrics_changed(void);

/* -- Read by the web binding: any thread, never waits for Matter ------- */

void matter_service_get_status(struct matter_status *out);
void matter_service_get_window(struct matter_window *out);
/** Codes for the current window, or a reason and empty strings. */
void matter_service_get_codes(struct matter_codes *out);
/** Copy up to @p max fabrics in fabric-index order; returns how many. */
size_t matter_service_get_fabrics(struct matter_fabric *out, size_t max);
/** The range a web-opened window may take: the contract's, narrowed by the
 *  SDK's once the stack has started. */
void matter_service_window_limits(uint32_t *min_seconds, uint32_t *max_seconds);

/* -- Requested by the web binding -------------------------------------- */

/**
 * @brief Ask for a basic commissioning window.
 *
 * On MATTER_REQUEST_ACCEPTED, @p done(@p ctx, result) runs later on the Matter
 * thread with 0 once the window is open, or the platform's negative result.
 */
enum matter_request_result matter_service_open_window(uint32_t timeout_seconds,
						      matter_service_done_t done, void *ctx);

/**
 * @brief Ask for the window to be closed. A closed window closes successfully.
 */
enum matter_request_result matter_service_close_window(matter_service_done_t done, void *ctx);

const char *matter_state_str(enum matter_state state);
const char *matter_window_mode_str(enum matter_window_mode mode);
const char *matter_window_source_str(enum matter_window_source source);
/** NULL for MATTER_CODES_AVAILABLE. */
const char *matter_codes_reason_str(enum matter_codes_reason reason);

#ifdef __cplusplus
}
#endif

#endif /* MATTER_SERVICE_MATTER_SERVICE_H_ */
