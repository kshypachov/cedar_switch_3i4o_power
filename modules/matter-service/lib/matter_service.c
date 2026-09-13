/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * matter-service core. The design is in include/matter_service/matter_service.h.
 *
 * Two kinds of caller meet here. The web binding reads and requests from the
 * HTTP server thread; the adapter reports and runs requests on the Matter
 * thread. The mutex guards the snapshot and the request slots and is held only
 * to copy them: no platform function is ever called with it held.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <matter_service/matter_service.h>

enum request_kind {
	REQUEST_OPEN,
	REQUEST_CLOSE,
};

struct request {
	bool used;
	enum request_kind kind;
	uint32_t timeout_seconds;
	matter_service_done_t done;
	void *ctx;
};

static K_MUTEX_DEFINE(lock);
static const struct matter_service_platform *pf;

static enum matter_state state;
static int32_t failure;
static uint32_t sdk_min_seconds;
static uint32_t sdk_max_seconds;

static struct matter_window_reading window;
/* The open window is one this service opened, and when and for how long. */
static bool web_window;
static int64_t web_opened_ms;
static uint32_t web_timeout_seconds;
/* An accepted open that has not run yet counts as an open window. */
static bool open_pending;

static struct matter_codes codes;
/* The device's own codes, generated once: they do not change while it runs,
 * and generating them held the Matter thread for 5-6 s on the board. */
static struct matter_codes device_codes;
static bool device_codes_valid;

static struct matter_fabric fabrics[MATTER_SERVICE_MAX_FABRICS];
static size_t fabric_count;

static struct request requests[CONFIG_MATTER_SERVICE_REQUEST_SLOTS];

static void clear_codes(struct matter_codes *c, enum matter_codes_reason reason)
{
	memset(c, 0, sizeof(*c));
	c->reason = reason;
}

static void reset_snapshot(void)
{
	state = MATTER_STATE_NOT_READY;
	failure = 0;
	sdk_min_seconds = 0;
	sdk_max_seconds = 0;
	memset(&window, 0, sizeof(window));
	web_window = false;
	web_opened_ms = 0;
	web_timeout_seconds = 0;
	open_pending = false;
	clear_codes(&codes, MATTER_CODES_WINDOW_CLOSED);
	clear_codes(&device_codes, MATTER_CODES_AVAILABLE);
	device_codes_valid = false;
	memset(fabrics, 0, sizeof(fabrics));
	fabric_count = 0;
	memset(requests, 0, sizeof(requests));
}

int matter_service_init(const struct matter_service_platform *platform)
{
	if (platform == NULL || platform->schedule == NULL || platform->now_ms == NULL ||
	    platform->open_basic_window == NULL || platform->close_window == NULL ||
	    platform->read_window == NULL || platform->read_codes == NULL ||
	    platform->read_fabrics == NULL || platform->min_window_seconds == NULL ||
	    platform->max_window_seconds == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);
	pf = platform;
	reset_snapshot();
	k_mutex_unlock(&lock);

	return 0;
}

/* -- Matter thread: refreshing the snapshot ---------------------------- */

static bool basic_window(const struct matter_window_reading *r)
{
	return r->open && (!r->opened_by_controller || r->controller_mode == MATTER_WINDOW_MODE_BASIC);
}

/*
 * Generate the device's codes if they are not known yet. On the Matter thread.
 * A failure is not remembered, so the next window tries again.
 */
static bool ensure_device_codes(void)
{
	struct matter_codes c;

	if (device_codes_valid) {
		return true;
	}
	clear_codes(&c, MATTER_CODES_AVAILABLE);
	if (pf->read_codes(&c) != 0) {
		return false;
	}
	c.reason = MATTER_CODES_AVAILABLE;
	device_codes = c;
	device_codes_valid = true;
	return true;
}

static void refresh_window(void)
{
	struct matter_window_reading r = {0};
	struct matter_codes c;

	pf->read_window(&r);

	if (!r.open) {
		clear_codes(&c, MATTER_CODES_WINDOW_CLOSED);
	} else if (!basic_window(&r)) {
		clear_codes(&c, MATTER_CODES_PASSCODE_UNAVAILABLE);
	} else if (ensure_device_codes()) {
		c = device_codes;
	} else {
		clear_codes(&c, MATTER_CODES_GENERATION_FAILED);
	}

	k_mutex_lock(&lock, K_FOREVER);
	window = r;
	if (!r.open || r.opened_by_controller) {
		/* Closed, or a controller's window now stands where ours was. */
		web_window = false;
	}
	codes = c;
	k_mutex_unlock(&lock);
}

static void sort_by_index(struct matter_fabric *list, size_t n)
{
	for (size_t i = 1; i < n; i++) {
		struct matter_fabric item = list[i];
		size_t j = i;

		while (j > 0 && list[j - 1].fabric_index > item.fabric_index) {
			list[j] = list[j - 1];
			j--;
		}
		list[j] = item;
	}
}

static void refresh_fabrics(void)
{
	/* Only the Matter thread refreshes, so one scratch table is enough. */
	static struct matter_fabric scratch[MATTER_SERVICE_MAX_FABRICS];
	size_t n;

	memset(scratch, 0, sizeof(scratch));
	/* Not inside MIN(): the macro evaluates its arguments twice, and this one
	 * walks the SDK's fabric table. */
	n = pf->read_fabrics(scratch, ARRAY_SIZE(scratch));
	n = MIN(n, ARRAY_SIZE(scratch));
	for (size_t i = 0; i < n; i++) {
		scratch[i].id[MATTER_FABRIC_ID_MAX_LEN] = '\0';
		scratch[i].label[MATTER_FABRIC_LABEL_MAX_LEN] = '\0';
	}
	sort_by_index(scratch, n);

	k_mutex_lock(&lock, K_FOREVER);
	memcpy(fabrics, scratch, sizeof(fabrics));
	fabric_count = n;
	k_mutex_unlock(&lock);
}

void matter_service_report_starting(void)
{
	k_mutex_lock(&lock, K_FOREVER);
	if (state == MATTER_STATE_NOT_READY) {
		state = MATTER_STATE_STARTING;
	}
	k_mutex_unlock(&lock);
}

void matter_service_report_started(int32_t result)
{
	if (pf == NULL) {
		return;
	}

	if (result != 0) {
		k_mutex_lock(&lock, K_FOREVER);
		state = MATTER_STATE_FAILED;
		failure = result;
		k_mutex_unlock(&lock);
		return;
	}

	uint32_t min_s = pf->min_window_seconds();
	uint32_t max_s = pf->max_window_seconds();

	/* Now, before anyone commissions: later the seconds this takes would
	 * fall inside a PASE or CASE exchange (a controller timed out on the
	 * board when they did). */
	(void)ensure_device_codes();
	refresh_window();
	refresh_fabrics();

	k_mutex_lock(&lock, K_FOREVER);
	sdk_min_seconds = min_s;
	sdk_max_seconds = max_s;
	failure = 0;
	state = MATTER_STATE_READY;
	k_mutex_unlock(&lock);
}

void matter_service_report_window_changed(void)
{
	if (pf != NULL) {
		refresh_window();
	}
}

void matter_service_report_fabrics_changed(void)
{
	if (pf != NULL) {
		refresh_fabrics();
	}
}

/* -- Matter thread: running requests ----------------------------------- */

static void run_request(void *arg)
{
	struct request *req = arg;
	struct request copy;
	int result = 0;

	k_mutex_lock(&lock, K_FOREVER);
	copy = *req;
	k_mutex_unlock(&lock);

	if (copy.kind == REQUEST_OPEN) {
		struct matter_window_reading r = {0};

		/* Something else may have opened a window since this was accepted;
		 * that window is not replaced. */
		pf->read_window(&r);
		result = r.open ? -EBUSY : pf->open_basic_window(copy.timeout_seconds);
		if (result == 0) {
			k_mutex_lock(&lock, K_FOREVER);
			web_window = true;
			web_opened_ms = pf->now_ms();
			web_timeout_seconds = copy.timeout_seconds;
			k_mutex_unlock(&lock);
		}
	} else {
		pf->close_window();
	}

	refresh_window();

	k_mutex_lock(&lock, K_FOREVER);
	if (copy.kind == REQUEST_OPEN) {
		open_pending = false;
		if (result != 0) {
			web_window = false;
		}
	}
	memset(req, 0, sizeof(*req));
	k_mutex_unlock(&lock);

	copy.done(copy.ctx, result);
}

static struct request *take_slot(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(requests); i++) {
		if (!requests[i].used) {
			requests[i].used = true;
			return &requests[i];
		}
	}
	return NULL;
}

static void window_limits_locked(uint32_t *min_s, uint32_t *max_s)
{
	*min_s = MATTER_WINDOW_MIN_SECONDS;
	*max_s = MATTER_WINDOW_MAX_SECONDS;
	if (sdk_min_seconds != 0U || sdk_max_seconds != 0U) {
		*min_s = MAX(*min_s, sdk_min_seconds);
		*max_s = MIN(*max_s, sdk_max_seconds);
	}
}

static enum matter_request_result submit(struct request *req)
{
	if (pf->schedule(run_request, req) != 0) {
		k_mutex_lock(&lock, K_FOREVER);
		if (req->kind == REQUEST_OPEN) {
			open_pending = false;
		}
		memset(req, 0, sizeof(*req));
		k_mutex_unlock(&lock);
		return MATTER_REQUEST_SCHEDULE_FAILED;
	}
	return MATTER_REQUEST_ACCEPTED;
}

enum matter_request_result matter_service_open_window(uint32_t timeout_seconds,
						      matter_service_done_t done, void *ctx)
{
	struct request *req;
	uint32_t min_s;
	uint32_t max_s;

	if (pf == NULL || done == NULL) {
		return MATTER_REQUEST_NOT_READY;
	}

	k_mutex_lock(&lock, K_FOREVER);
	window_limits_locked(&min_s, &max_s);
	if (timeout_seconds < min_s || timeout_seconds > max_s) {
		k_mutex_unlock(&lock);
		return MATTER_REQUEST_OUT_OF_RANGE;
	}
	if (state != MATTER_STATE_READY) {
		k_mutex_unlock(&lock);
		return MATTER_REQUEST_NOT_READY;
	}
	if (open_pending || window.open) {
		k_mutex_unlock(&lock);
		return MATTER_REQUEST_WINDOW_OPEN;
	}
	req = take_slot();
	if (req == NULL) {
		k_mutex_unlock(&lock);
		return MATTER_REQUEST_BUSY;
	}
	req->kind = REQUEST_OPEN;
	req->timeout_seconds = timeout_seconds;
	req->done = done;
	req->ctx = ctx;
	open_pending = true;
	k_mutex_unlock(&lock);

	return submit(req);
}

enum matter_request_result matter_service_close_window(matter_service_done_t done, void *ctx)
{
	struct request *req;

	if (pf == NULL || done == NULL) {
		return MATTER_REQUEST_NOT_READY;
	}

	k_mutex_lock(&lock, K_FOREVER);
	if (state != MATTER_STATE_READY) {
		k_mutex_unlock(&lock);
		return MATTER_REQUEST_NOT_READY;
	}
	req = take_slot();
	if (req == NULL) {
		k_mutex_unlock(&lock);
		return MATTER_REQUEST_BUSY;
	}
	req->kind = REQUEST_CLOSE;
	req->done = done;
	req->ctx = ctx;
	k_mutex_unlock(&lock);

	return submit(req);
}

/* -- Any thread: reads -------------------------------------------------- */

void matter_service_get_status(struct matter_status *out)
{
	k_mutex_lock(&lock, K_FOREVER);
	out->state = state;
	out->fabric_count = (uint8_t)fabric_count;
	out->commissioned = fabric_count > 0U;
	out->failure = state == MATTER_STATE_FAILED ? failure : 0;
	k_mutex_unlock(&lock);
}

void matter_service_get_window(struct matter_window *out)
{
	memset(out, 0, sizeof(*out));

	k_mutex_lock(&lock, K_FOREVER);
	if (state == MATTER_STATE_READY && window.open) {
		out->open = true;
		if (window.opened_by_controller) {
			out->source = MATTER_WINDOW_SOURCE_CONTROLLER;
			out->mode = window.controller_mode;
		} else {
			out->source = web_window ? MATTER_WINDOW_SOURCE_WEB : MATTER_WINDOW_SOURCE_LOCAL;
			out->mode = MATTER_WINDOW_MODE_BASIC;
		}
		if (out->source == MATTER_WINDOW_SOURCE_WEB) {
			int64_t elapsed_s = (pf->now_ms() - web_opened_ms) / 1000;

			out->remaining_seconds =
				elapsed_s >= (int64_t)web_timeout_seconds
					? 0U
					: web_timeout_seconds - (uint32_t)MAX(elapsed_s, 0);
		}
		out->codes_available = codes.reason == MATTER_CODES_AVAILABLE;
	}
	k_mutex_unlock(&lock);
}

void matter_service_get_codes(struct matter_codes *out)
{
	k_mutex_lock(&lock, K_FOREVER);
	if (state != MATTER_STATE_READY) {
		clear_codes(out, MATTER_CODES_SERVICE_NOT_READY);
	} else {
		*out = codes;
	}
	k_mutex_unlock(&lock);
}

size_t matter_service_get_fabrics(struct matter_fabric *out, size_t max)
{
	size_t n;

	k_mutex_lock(&lock, K_FOREVER);
	n = MIN(max, fabric_count);
	memcpy(out, fabrics, n * sizeof(*out));
	k_mutex_unlock(&lock);

	return n;
}

void matter_service_window_limits(uint32_t *min_seconds, uint32_t *max_seconds)
{
	k_mutex_lock(&lock, K_FOREVER);
	window_limits_locked(min_seconds, max_seconds);
	k_mutex_unlock(&lock);
}

const char *matter_state_str(enum matter_state s)
{
	switch (s) {
	case MATTER_STATE_NOT_READY:
		return "not_ready";
	case MATTER_STATE_STARTING:
		return "starting";
	case MATTER_STATE_READY:
		return "ready";
	case MATTER_STATE_FAILED:
		return "failed";
	}
	return "failed";
}

const char *matter_window_mode_str(enum matter_window_mode mode)
{
	switch (mode) {
	case MATTER_WINDOW_MODE_BASIC:
		return "basic";
	case MATTER_WINDOW_MODE_ENHANCED:
		return "enhanced";
	default:
		return NULL;
	}
}

const char *matter_window_source_str(enum matter_window_source source)
{
	switch (source) {
	case MATTER_WINDOW_SOURCE_WEB:
		return "web";
	case MATTER_WINDOW_SOURCE_CONTROLLER:
		return "controller";
	case MATTER_WINDOW_SOURCE_LOCAL:
		return "local";
	default:
		return NULL;
	}
}

const char *matter_codes_reason_str(enum matter_codes_reason reason)
{
	switch (reason) {
	case MATTER_CODES_WINDOW_CLOSED:
		return "window_closed";
	case MATTER_CODES_PASSCODE_UNAVAILABLE:
		return "passcode_unavailable";
	case MATTER_CODES_SERVICE_NOT_READY:
		return "service_not_ready";
	case MATTER_CODES_GENERATION_FAILED:
		return "generation_failed";
	default:
		return NULL;
	}
}
