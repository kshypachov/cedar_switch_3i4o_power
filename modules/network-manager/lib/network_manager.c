/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Network manager: the transaction state machine. See
 * include/network_manager/network_manager.h for the contract and
 * network_validate.c for the rules a proposal has to satisfy; this file
 * documents only how the transaction is driven.
 *
 * One transaction exists at a time. That is not a simplification — the
 * contract requires it, because two administrators applying different network
 * changes at once would leave neither able to say what the device is running,
 * and a rollback would have no single thing to roll back to.
 *
 * Two halves. The request calls — stage, apply, confirm, rollback, scan —
 * decide and record under the mutex and never touch an interface.
 * network_manager_process() does what they recorded: it copies what it needs
 * under the mutex, lets go of it for the adapter call, and takes it again to
 * record the outcome, checking that the state it started from still holds. A
 * handler is therefore never kept waiting on a Wi-Fi association, and a
 * rollback asked for while a candidate is still being pushed is simply the
 * next thing process() does.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "network_internal.h"

LOG_MODULE_REGISTER(network_manager, CONFIG_NETWORK_MANAGER_LOG_LEVEL);

/*
 * Rejoining a Wi-Fi network that dropped: the first retry after five seconds,
 * each next one twice as late, never more than a minute apart. A coprocessor
 * asked to associate every second with a password that no longer works spends
 * its air time failing, and the AP may count the attempts against it.
 */
#define RECONNECT_FIRST_MS 5000
#define RECONNECT_MAX_MS   60000

/* Why a transaction is rolling back decides how it ends. */
enum rollback_reason {
	/* DELETE on an applied transaction: rolled_back, the job succeeds. */
	ROLLBACK_REQUESTED = 0,
	/* Nobody confirmed in time: rolled_back with resource_expired. */
	ROLLBACK_TIMED_OUT,
	/* The interfaces refused the candidate: failed. */
	ROLLBACK_APPLY_FAILED,
	/* The confirmed generation could not be written: failed. */
	ROLLBACK_COMMIT_FAILED,
};

struct transaction {
	bool used;
	char id[NETWORK_TXN_ID_MAX_LEN + 1];
	enum network_transaction_state state;
	uint32_t base_revision;
	struct device_config candidate;
	/*
	 * The credential action is kept alongside the candidate because the
	 * bytes themselves go straight into the pending generation at apply
	 * time and are never held here. What survives staging is only the
	 * intent plus, for a replace, the value — which is why a candidate is
	 * bounded in time.
	 */
	struct device_config_secret_update credential;
	uint8_t credential_value[DEVICE_CONFIG_SECRET_MAX_LEN];
	size_t credential_len;

	char job_id[JOB_ID_MAX_LEN + 1];
	bool has_job;
	/* Absolute instant on the injected clock; 0 when no deadline runs. */
	int64_t deadline_ms;
	enum api_error_code error_code;
	bool has_error;

	/* The candidate reached the interfaces and has not been undone. */
	bool pushed;
	/* A confirm was accepted; process() writes the commit. */
	bool commit_requested;
	enum rollback_reason rollback_reason;
};

struct scan_record {
	bool used;
	char job_id[JOB_ID_MAX_LEN + 1];
	/* Accepted and not yet started by process(). */
	bool requested;
	bool complete;
	bool failed;
	struct network_scan_results results;
};

static struct {
	const struct network_iface_ops *ops;
	struct transaction txn;
	struct scan_record scan;
	uint32_t id_counter;
	bool initialised;
	/* The committed generation has to be put on the interfaces. */
	bool push_committed;
	/* The transaction the store discarded at boot, if any. */
	char lost_txn[NETWORK_TXN_ID_MAX_LEN + 1];
	/* The interface last set as the default, once one has been. */
	bool has_default;
	enum device_config_interface default_iface;
	/* When Wi-Fi may be asked to join again, and how long the wait after that is. */
	int64_t reconnect_at_ms;
	int32_t reconnect_backoff_ms;
} nm;

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

void network_manager_set_clock(int64_t (*fn)(void))
{
	k_mutex_lock(&lock, K_FOREVER);
	clock_fn = (fn != NULL) ? fn : default_clock;
	k_mutex_unlock(&lock);
}

static void secure_wipe(void *buf, size_t len)
{
	volatile uint8_t *p = buf;

	while (len-- > 0U) {
		*p++ = 0U;
	}
}

/* --- wire names --------------------------------------------------------- */

bool network_transaction_state_is_terminal(enum network_transaction_state state)
{
	switch (state) {
	case NETWORK_TXN_COMMITTED:
	case NETWORK_TXN_ROLLED_BACK:
	case NETWORK_TXN_FAILED:
	case NETWORK_TXN_EXPIRED:
		return true;
	default:
		return false;
	}
}

const char *network_transaction_state_str(enum network_transaction_state state)
{
	switch (state) {
	case NETWORK_TXN_STAGED:
		return "staged";
	case NETWORK_TXN_APPLYING:
		return "applying";
	case NETWORK_TXN_AWAITING_CONFIRMATION:
		return "awaiting_confirmation";
	case NETWORK_TXN_COMMITTED:
		return "committed";
	case NETWORK_TXN_ROLLING_BACK:
		return "rolling_back";
	case NETWORK_TXN_ROLLED_BACK:
		return "rolled_back";
	case NETWORK_TXN_FAILED:
		return "failed";
	case NETWORK_TXN_EXPIRED:
		return "expired";
	default:
		return NULL;
	}
}

const char *network_iface_state_str(enum network_iface_state state)
{
	static const char *const names[NETWORK_IFACE_STATE_COUNT] = {
		[NETWORK_IFACE_DISABLED] = "disabled",   [NETWORK_IFACE_DOWN] = "down",
		[NETWORK_IFACE_CONNECTING] = "connecting", [NETWORK_IFACE_CONNECTED] = "connected",
		[NETWORK_IFACE_ADDRESSING] = "addressing", [NETWORK_IFACE_READY] = "ready",
		[NETWORK_IFACE_FAILED] = "failed",
	};

	return ((unsigned int)state < ARRAY_SIZE(names)) ? names[state] : NULL;
}

const char *network_ap_security_str(enum network_ap_security security)
{
	static const char *const names[NETWORK_AP_SECURITY_COUNT] = {
		[NETWORK_AP_OPEN] = "open",
		[NETWORK_AP_WPA2_PSK] = "wpa2_psk",
		[NETWORK_AP_WPA3_SAE] = "wpa3_sae",
		[NETWORK_AP_WPA2_WPA3_TRANSITION] = "wpa2_wpa3_transition",
		[NETWORK_AP_ENTERPRISE] = "enterprise",
		[NETWORK_AP_UNKNOWN] = "unknown",
	};

	return ((unsigned int)security < ARRAY_SIZE(names)) ? names[security] : NULL;
}

bool network_ap_security_connectable(enum network_ap_security security)
{
	/*
	 * A transition network accepts either of the two personal modes this
	 * build supports. Enterprise needs certificates the device does not
	 * have, and an unknown security cannot be joined on purpose.
	 */
	switch (security) {
	case NETWORK_AP_OPEN:
	case NETWORK_AP_WPA2_PSK:
	case NETWORK_AP_WPA3_SAE:
	case NETWORK_AP_WPA2_WPA3_TRANSITION:
		return true;
	default:
		return false;
	}
}

const char *network_addr_source_str(enum network_addr_source source)
{
	static const char *const names[NETWORK_ADDR_SOURCE_COUNT] = {
		[NETWORK_ADDR_DHCP] = "dhcp",
		[NETWORK_ADDR_STATIC] = "static",
		[NETWORK_ADDR_SLAAC] = "slaac",
		[NETWORK_ADDR_LINK_LOCAL] = "link_local",
	};

	return ((unsigned int)source < ARRAY_SIZE(names)) ? names[source] : NULL;
}

/* --- helpers ------------------------------------------------------------ */

static bool txn_matches(const char *id)
{
	return nm.txn.used && id != NULL && strncmp(nm.txn.id, id, NETWORK_TXN_ID_MAX_LEN) == 0;
}

static void copy_job_id(char out[JOB_ID_MAX_LEN + 1], const char *id)
{
	strncpy(out, id, JOB_ID_MAX_LEN);
	out[JOB_ID_MAX_LEN] = '\0';
}

static void reject_state(struct api_error *err, const char *verb)
{
	char message[API_ERROR_MESSAGE_MAX_LEN + 1];

	(void)snprintf(message, sizeof(message), "A transaction in state '%s' cannot be %s",
		       network_transaction_state_str(nm.txn.state), verb);
	(void)api_error_init(err, API_ERR_INVALID_STATE, message, NULL);
}

static void reject_exhausted(struct api_error *err)
{
	(void)api_error_init(err, API_ERR_RATE_LIMITED,
			     "Too many operations are in flight; retry shortly", NULL);
	(void)api_error_set_retry_after(err, 1);
}

/* The retry of a request that created a job, a conflicting reuse of its key,
 * or neither. True when @p err or @p job_id now holds the answer.
 */
static bool answered_by_key(const char *key, uint32_t hash, struct api_error *err,
			    char job_id[JOB_ID_MAX_LEN + 1], int *rc)
{
	struct job_snapshot job;

	if (key == NULL) {
		return false;
	}
	switch (job_find_by_key(key, hash, &job)) {
	case JOB_LOOKUP_EXISTING:
		copy_job_id(job_id, job.id);
		*rc = 0;
		return true;
	case JOB_LOOKUP_CONFLICT:
		(void)api_error_init(err, API_ERR_IDEMPOTENCY_CONFLICT,
				     "This Idempotency-Key was used with a different request",
				     NULL);
		*rc = -EINVAL;
		return true;
	default:
		return false;
	}
}

static void fail_job(const char *id, enum api_error_code code)
{
	(void)job_fail(id, api_error_str(code), api_error_is_retryable(code));
}

/* The configuration the interfaces are running: the candidate once it has been
 * pushed and until it is undone or committed, the committed one otherwise.
 */
static void in_force_locked(struct device_config *out)
{
	if (nm.txn.used && nm.txn.pushed) {
		*out = nm.txn.candidate;
	} else if (device_config_get(DEVICE_CONFIG_COMMITTED, out) != 0) {
		device_config_defaults(out);
	}
}

/*
 * A finished transaction is kept rather than erased so a client that was
 * polling still learns the outcome. It is replaced only when a new one is
 * staged, which the staging path allows exactly when this one is terminal.
 */
static void finish(enum network_transaction_state state, enum api_error_code code, bool has_error)
{
	nm.txn.state = state;
	nm.txn.deadline_ms = 0;
	nm.txn.commit_requested = false;
	nm.txn.error_code = code;
	nm.txn.has_error = has_error;
	secure_wipe(nm.txn.credential_value, sizeof(nm.txn.credential_value));
	nm.txn.credential_len = 0;
	nm.txn.credential.action = DEVICE_CONFIG_SECRET_KEEP;
	nm.txn.credential.value = NULL;
	nm.txn.credential.len = 0;
}

static void begin_rollback_locked(enum rollback_reason reason)
{
	nm.txn.state = NETWORK_TXN_ROLLING_BACK;
	nm.txn.deadline_ms = 0;
	nm.txn.rollback_reason = reason;
	if (reason == ROLLBACK_TIMED_OUT) {
		/*
		 * The mock's DECISION, shared: the transaction is the
		 * non-transferable resource of the 410 row, and re-reading the
		 * configuration and staging again is the recovery that code
		 * implies. busy or invalid_state would suggest retrying a
		 * confirm that can never work.
		 */
		nm.txn.error_code = API_ERR_RESOURCE_EXPIRED;
		nm.txn.has_error = true;
	}
	if (nm.txn.has_job) {
		(void)job_set_state(nm.txn.job_id, JOB_STATE_RUNNING);
		(void)job_set_phase(nm.txn.job_id, "rolling_back");
	}
}

/* Expire a candidate nobody applied and time out a change nobody confirmed. */
static int expire_locked(void)
{
	if (!nm.initialised || !nm.txn.used || nm.txn.deadline_ms <= 0 ||
	    now_ms() < nm.txn.deadline_ms) {
		return 0;
	}

	switch (nm.txn.state) {
	case NETWORK_TXN_STAGED:
		/* Nothing was changed, so it simply goes away — a candidate kept
		 * indefinitely would block the next one.
		 */
		finish(NETWORK_TXN_EXPIRED, API_ERR_RESOURCE_EXPIRED, true);
		LOG_INF("candidate %s expired", nm.txn.id);
		return 1;

	case NETWORK_TXN_APPLYING:
	case NETWORK_TXN_AWAITING_CONFIRMATION:
		if (nm.txn.commit_requested) {
			/* Confirmed in time; the commit is only waiting its turn. */
			return 0;
		}
		/*
		 * The case this module exists for: the change went live and
		 * nobody could confirm it. Silence is a rollback.
		 */
		LOG_WRN("transaction %s not confirmed in time; rolling back", nm.txn.id);
		begin_rollback_locked(ROLLBACK_TIMED_OUT);
		return 1;

	default:
		return 0;
	}
}

/* --- status ------------------------------------------------------------- */

static bool dns_matches(const struct network_status *st, const struct device_config_dns *dns)
{
	if (st->dns_count != dns->server_count) {
		return false;
	}
	for (uint8_t i = 0; i < dns->server_count; i++) {
		const size_t len = (dns->servers[i].family == DEVICE_CONFIG_AF_INET) ? 4U : 16U;

		if (st->dns[i].family != dns->servers[i].family ||
		    memcmp(st->dns[i].bytes, dns->servers[i].bytes, len) != 0) {
			return false;
		}
	}
	return true;
}

static int read_status_locked(struct network_status *out)
{
	struct device_config cfg;

	memset(out, 0, sizeof(*out));

	int err = nm.ops->get_status(nm.ops->ctx, DEVICE_CONFIG_INTERFACE_ETHERNET,
				     &out->ethernet);

	if (err != 0) {
		return err;
	}
	err = nm.ops->get_status(nm.ops->ctx, DEVICE_CONFIG_INTERFACE_WIFI, &out->wifi);
	if (err != 0) {
		return err;
	}

	in_force_locked(&cfg);
	out->ethernet.enabled = cfg.ethernet.enabled;
	out->wifi.enabled = cfg.wifi.enabled;
	out->ethernet.state = network_iface_state_of(DEVICE_CONFIG_INTERFACE_ETHERNET,
						     &out->ethernet, cfg.ethernet.enabled);
	out->wifi.state =
		network_iface_state_of(DEVICE_CONFIG_INTERFACE_WIFI, &out->wifi, cfg.wifi.enabled);

	/*
	 * Which interface carries traffic is the policy's choice, which
	 * process() sets as the default route; the plan is explicit that
	 * net_if_set_default() alone does not prove the routing is what anyone
	 * intended, so the choice is recomputed from what is observed.
	 */
	out->has_active = network_select_default(&cfg, out, &out->active);

	/*
	 * With automatic DNS the device holds whatever DHCP handed it, which
	 * only the adapter can report; without that call, only a manual list is
	 * known.
	 */
	if (nm.ops->get_dns != NULL) {
		size_t count = 0;

		if (nm.ops->get_dns(nm.ops->ctx, out->dns, ARRAY_SIZE(out->dns), &count) == 0) {
			out->dns_count = (uint8_t)MIN(count, ARRAY_SIZE(out->dns));
		}
	} else if (cfg.dns.mode == DEVICE_CONFIG_DNS_MANUAL) {
		out->dns_count = MIN(cfg.dns.server_count, (uint8_t)DEVICE_CONFIG_DNS_MAX_SERVERS);
		memcpy(out->dns, cfg.dns.servers, sizeof(cfg.dns.servers[0]) * out->dns_count);
	}

	return 0;
}

int network_get_status(struct network_status *out)
{
	if (out == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);

	int err = nm.initialised ? read_status_locked(out) : -EINVAL;

	k_mutex_unlock(&lock);

	return err;
}

/* --- transaction snapshots ---------------------------------------------- */

static void snapshot_locked(const struct transaction *t, struct network_transaction *out)
{
	memset(out, 0, sizeof(*out));

	strncpy(out->id, t->id, sizeof(out->id) - 1);
	out->state = t->state;
	out->base_revision = t->base_revision;
	out->candidate = t->candidate;
	out->has_job = t->has_job;
	if (t->has_job) {
		strncpy(out->job_id, t->job_id, sizeof(out->job_id) - 1);
	}
	out->error_code = t->error_code;
	out->has_error = t->has_error;

	if (t->deadline_ms > 0) {
		int64_t left = t->deadline_ms - now_ms();

		/* Rounded down, as the mock counts. */
		out->remaining_seconds = (left <= 0) ? 0 : (int32_t)(left / 1000);
	} else {
		out->remaining_seconds = -1;
	}
}

int network_transaction_get(const char *txn_id, struct network_transaction *out)
{
	if (out == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);

	int rc = -ENOENT;

	(void)expire_locked();
	if (txn_matches(txn_id)) {
		snapshot_locked(&nm.txn, out);
		rc = 0;
	}

	k_mutex_unlock(&lock);

	return rc;
}

int network_transaction_current(struct network_transaction *out)
{
	if (out == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);

	int rc = -ENOENT;

	(void)expire_locked();
	if (nm.txn.used) {
		snapshot_locked(&nm.txn, out);
		rc = 0;
	}

	k_mutex_unlock(&lock);

	return rc;
}

/* --- staging ------------------------------------------------------------ */

static void make_id(char *buf, size_t cap)
{
	nm.id_counter++;
	(void)snprintf(buf, cap, "txn_%08x", nm.id_counter);
}

int network_stage_config(const struct network_config_input *input, uint32_t base_revision,
			 struct api_error *err, struct network_transaction *out)
{
	if (input == NULL || err == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);

	int rc = -EINVAL;
	struct device_config committed;
	struct network_status status;

	if (!nm.initialised) {
		(void)api_error_init(err, API_ERR_SERVICE_NOT_READY, "Network manager not ready",
				     NULL);
		goto out;
	}

	(void)expire_locked();

	/*
	 * A transaction still in flight owns the interfaces. Replacing it
	 * would strand whoever is waiting on it, so a second one is refused
	 * with the code the contract reserves for exactly this.
	 */
	if (nm.txn.used && !network_transaction_state_is_terminal(nm.txn.state)) {
		(void)api_error_init(err, API_ERR_BUSY,
				     "Another network transaction is in progress; discard it first",
				     NULL);
		goto out;
	}

	if (device_config_get(DEVICE_CONFIG_COMMITTED, &committed) != 0) {
		(void)api_error_init(err, API_ERR_INTERNAL_ERROR,
				     "Configuration store unavailable", NULL);
		goto out;
	}

	if (base_revision != device_config_revision()) {
		/*
		 * Refused rather than merged. The candidate was built from a
		 * configuration that has since changed, and applying it would
		 * silently undo whatever the other change did.
		 */
		char message[API_ERROR_MESSAGE_MAX_LEN + 1];

		(void)snprintf(message, sizeof(message),
			       "The configuration is at revision %u; re-read it and stage again",
			       device_config_revision());
		(void)api_error_init(err, API_ERR_STALE_REVISION, message, NULL);
		goto out;
	}

	if (read_status_locked(&status) != 0) {
		(void)api_error_init(err, API_ERR_INTERNAL_ERROR,
				     "Interface state unavailable", NULL);
		goto out;
	}

	(void)api_error_init(err, API_ERR_VALIDATION_FAILED,
			     "The configuration cannot be applied", NULL);
	if (!network_validate_config(input, &committed, &status, err)) {
		goto out;
	}

	secure_wipe(&nm.txn, sizeof(nm.txn));
	nm.txn.used = true;
	nm.txn.state = NETWORK_TXN_STAGED;
	nm.txn.base_revision = base_revision;
	nm.txn.candidate = input->config;
	/* Derived by the store on write; never trusted from a caller. */
	nm.txn.candidate.wifi.password_set =
		(input->wifi_password.action == DEVICE_CONFIG_SECRET_REPLACE) ||
		(input->wifi_password.action == DEVICE_CONFIG_SECRET_KEEP &&
		 committed.wifi.password_set);
	nm.txn.candidate.admin_password_set = committed.admin_password_set;

	nm.txn.credential.action = input->wifi_password.action;
	if (input->wifi_password.action == DEVICE_CONFIG_SECRET_REPLACE) {
		nm.txn.credential_len = MIN(input->wifi_password.len,
					    (size_t)DEVICE_CONFIG_SECRET_MAX_LEN);
		memcpy(nm.txn.credential_value, input->wifi_password.value,
		       nm.txn.credential_len);
	}

	make_id(nm.txn.id, sizeof(nm.txn.id));
	nm.txn.deadline_ms =
		now_ms() + (int64_t)CONFIG_NETWORK_MANAGER_CANDIDATE_TTL_SECONDS * 1000;

	if (out != NULL) {
		snapshot_locked(&nm.txn, out);
	}
	LOG_INF("transaction %s staged against revision %u", nm.txn.id, base_revision);
	rc = 0;

out:
	k_mutex_unlock(&lock);

	return rc;
}

/* --- apply -------------------------------------------------------------- */

/* Build the update the store writes, resolving the credential into bytes. */
static void build_update(struct device_config_update *update)
{
	memset(update, 0, sizeof(*update));
	update->config = nm.txn.candidate;

	update->secrets[DEVICE_CONFIG_SECRET_WIFI_PASSWORD].action = nm.txn.credential.action;
	if (nm.txn.credential.action == DEVICE_CONFIG_SECRET_REPLACE) {
		update->secrets[DEVICE_CONFIG_SECRET_WIFI_PASSWORD].value =
			nm.txn.credential_value;
		update->secrets[DEVICE_CONFIG_SECRET_WIFI_PASSWORD].len = nm.txn.credential_len;
	}
	/*
	 * The admin credential is carried forward untouched. A network change
	 * has no business altering the password that guards the API, and KEEP
	 * is what makes that true rather than merely intended.
	 */
	update->secrets[DEVICE_CONFIG_SECRET_ADMIN_PASSWORD].action = DEVICE_CONFIG_SECRET_KEEP;
}

int network_apply(const char *txn_id, uint16_t confirmation_timeout_seconds,
		  const char *idempotency_key, uint32_t request_hash, struct api_error *err,
		  char job_id[JOB_ID_MAX_LEN + 1])
{
	if (err == NULL || job_id == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);

	int rc = -EINVAL;

	/* A retry of an apply already accepted gets its job back, whatever the
	 * transaction has done since. */
	if (answered_by_key(idempotency_key, request_hash, err, job_id, &rc)) {
		goto out;
	}

	(void)expire_locked();

	if (!txn_matches(txn_id)) {
		(void)api_error_init(err, API_ERR_NOT_FOUND, "No such network transaction", NULL);
		goto out;
	}
	if (nm.txn.state != NETWORK_TXN_STAGED) {
		reject_state(err, "applied");
		goto out;
	}

	uint16_t timeout = (confirmation_timeout_seconds == 0U)
				   ? CONFIG_NETWORK_MANAGER_CONFIRM_TIMEOUT_SECONDS
				   : confirmation_timeout_seconds;

	if (timeout < CONFIG_NETWORK_MANAGER_CONFIRM_TIMEOUT_MIN_SECONDS ||
	    timeout > CONFIG_NETWORK_MANAGER_CONFIRM_TIMEOUT_MAX_SECONDS) {
		(void)api_error_init(err, API_ERR_VALIDATION_FAILED,
				     "Confirmation timeout out of range", NULL);
		(void)api_error_add_field(err, "/confirmation_timeout_seconds",
					  API_FIELD_OUT_OF_RANGE);
		goto out;
	}

	const struct job_create_params params = {
		.kind = JOB_KIND_NETWORK_APPLY,
		/*
		 * Not cancellable through jobs/cancel: the contract routes
		 * cancellation of a network change through the transaction, so
		 * that undoing it also restores the interfaces rather than
		 * merely abandoning the job.
		 */
		.cancellable = false,
		.idempotency_key = idempotency_key,
		.request_hash = request_hash,
	};
	struct job_snapshot job;

	switch (job_create(&params, &job)) {
	case JOB_CREATE_NEW:
		break;
	case JOB_CREATE_EXISTING:
		copy_job_id(job_id, job.id);
		rc = 0;
		goto out;
	case JOB_CREATE_CONFLICT:
		(void)api_error_init(err, API_ERR_IDEMPOTENCY_CONFLICT,
				     "This Idempotency-Key was used with a different request",
				     NULL);
		goto out;
	default:
		reject_exhausted(err);
		goto out;
	}

	/*
	 * Step 2 of section 5: the journal is written and verified before the
	 * network is touched. If this fails nothing has changed, which is the
	 * only safe outcome — a change nothing recorded is a change nothing
	 * can undo.
	 */
	struct device_config_update update;

	build_update(&update);

	int store_rc = device_config_pending_begin(&update, nm.txn.base_revision, nm.txn.id);

	secure_wipe(&update, sizeof(update));

	if (store_rc != 0) {
		const enum api_error_code code = (store_rc == -ESTALE) ? API_ERR_STALE_REVISION
								       : API_ERR_INTERNAL_ERROR;

		fail_job(job.id, code);
		strncpy(nm.txn.job_id, job.id, sizeof(nm.txn.job_id) - 1);
		nm.txn.has_job = true;
		finish(NETWORK_TXN_FAILED, code, true);
		(void)api_error_init(err, code, "Could not record the pending configuration",
				     NULL);
		LOG_ERR("transaction %s: journal failed (%d)", nm.txn.id, store_rc);
		goto out;
	}

	(void)job_set_state(job.id, JOB_STATE_RUNNING);
	(void)job_set_phase(job.id, "applying");
	strncpy(nm.txn.job_id, job.id, sizeof(nm.txn.job_id) - 1);
	nm.txn.has_job = true;
	nm.txn.state = NETWORK_TXN_APPLYING;
	/*
	 * The confirmation deadline is armed here rather than after the
	 * interfaces change, so that a worker which never runs — or dies
	 * mid-apply — still ends in a rollback instead of waiting forever.
	 */
	nm.txn.deadline_ms = now_ms() + (int64_t)timeout * 1000;
	copy_job_id(job_id, job.id);

	LOG_INF("transaction %s journalled, job %s, %u s to confirm", nm.txn.id, job.id, timeout);
	rc = 0;

out:
	k_mutex_unlock(&lock);

	return rc;
}

/* --- confirm and rollback ----------------------------------------------- */

int network_confirm(const char *txn_id, struct api_error *err, char job_id[JOB_ID_MAX_LEN + 1])
{
	if (err == NULL || job_id == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);

	int rc = -EINVAL;
	struct network_status status;

	(void)expire_locked();

	if (!txn_matches(txn_id)) {
		(void)api_error_init(err, API_ERR_NOT_FOUND, "No such network transaction", NULL);
		goto out;
	}
	if (nm.txn.state == NETWORK_TXN_AWAITING_CONFIRMATION && nm.txn.commit_requested) {
		/* Asked again before the commit was written: the same job. */
		copy_job_id(job_id, nm.txn.job_id);
		rc = 0;
		goto out;
	}
	if (nm.txn.state != NETWORK_TXN_AWAITING_CONFIRMATION) {
		reject_state(err, "confirmed");
		goto out;
	}

	if (read_status_locked(&status) != 0) {
		(void)api_error_init(err, API_ERR_INTERNAL_ERROR, "Interface state unavailable",
				     NULL);
		goto out;
	}

	/*
	 * Step 5. The request arriving proves the client's own path works and
	 * nothing else: a browser on an unchanged Ethernet says nothing about
	 * whether Wi-Fi joined. So every enabled interface is checked, and a
	 * transaction that is not healthy stays awaiting rather than failing —
	 * DHCP may simply not have finished, and the deadline is what decides
	 * when waiting has gone on too long.
	 */
	if (!network_config_is_healthy(&nm.txn.candidate, &status)) {
		(void)api_error_init(err, API_ERR_INVALID_STATE,
				     "Not every enabled interface is working yet; confirm again shortly",
				     NULL);
		rc = -EAGAIN;
		goto out;
	}

	nm.txn.commit_requested = true;
	(void)job_set_state(nm.txn.job_id, JOB_STATE_RUNNING);
	(void)job_set_phase(nm.txn.job_id, "committing");
	copy_job_id(job_id, nm.txn.job_id);
	LOG_INF("transaction %s confirmed", nm.txn.id);
	rc = 0;

out:
	k_mutex_unlock(&lock);

	return rc;
}

int network_rollback(const char *txn_id, const char *idempotency_key, uint32_t request_hash,
		     struct api_error *err, char job_id[JOB_ID_MAX_LEN + 1])
{
	if (err == NULL || job_id == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);

	int rc = -EINVAL;

	if (answered_by_key(idempotency_key, request_hash, err, job_id, &rc)) {
		goto out;
	}

	(void)expire_locked();

	if (!txn_matches(txn_id)) {
		(void)api_error_init(err, API_ERR_NOT_FOUND, "No such network transaction", NULL);
		goto out;
	}

	switch (nm.txn.state) {
	case NETWORK_TXN_STAGED: {
		/* Never applied: no journal, no hardware — a short job of its own. */
		const struct job_create_params params = {
			.kind = JOB_KIND_NETWORK_DISCARD,
			.cancellable = false,
			.idempotency_key = idempotency_key,
			.request_hash = request_hash,
		};
		struct job_snapshot job;

		if (job_create(&params, &job) != JOB_CREATE_NEW) {
			reject_exhausted(err);
			goto out;
		}
		(void)job_set_state(job.id, JOB_STATE_RUNNING);
		(void)job_set_phase(job.id, "discarding");
		(void)job_set_state(job.id, JOB_STATE_SUCCEEDED);
		strncpy(nm.txn.job_id, job.id, sizeof(nm.txn.job_id) - 1);
		nm.txn.has_job = true;
		finish(NETWORK_TXN_ROLLED_BACK, API_ERR_COUNT, false);
		copy_job_id(job_id, job.id);
		LOG_INF("candidate %s discarded", nm.txn.id);
		rc = 0;
		break;
	}

	case NETWORK_TXN_APPLYING:
	case NETWORK_TXN_AWAITING_CONFIRMATION:
		if (nm.txn.commit_requested) {
			(void)api_error_init(err, API_ERR_INVALID_STATE,
					     "The change is being committed and can no longer be rolled back",
					     NULL);
			goto out;
		}
		/*
		 * While the candidate is still being pushed this only records
		 * the request; process() undoes what the push did as soon as
		 * the push returns, on the same job.
		 */
		begin_rollback_locked(ROLLBACK_REQUESTED);
		copy_job_id(job_id, nm.txn.job_id);
		LOG_INF("transaction %s rolling back on request", nm.txn.id);
		rc = 0;
		break;

	default:
		reject_state(err, "rolled back");
		break;
	}

out:
	k_mutex_unlock(&lock);

	return rc;
}

/* --- scan --------------------------------------------------------------- */

static bool change_in_progress_locked(void)
{
	return nm.txn.used && (nm.txn.state == NETWORK_TXN_APPLYING ||
			       nm.txn.state == NETWORK_TXN_AWAITING_CONFIRMATION ||
			       nm.txn.state == NETWORK_TXN_ROLLING_BACK);
}

int network_scan_begin(const char *idempotency_key, uint32_t request_hash, struct api_error *err,
		       char job_id[JOB_ID_MAX_LEN + 1])
{
	if (err == NULL || job_id == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);

	int rc = -EINVAL;
	struct network_status status;

	if (!nm.initialised) {
		(void)api_error_init(err, API_ERR_SERVICE_NOT_READY, "Network manager not ready",
				     NULL);
		goto out;
	}
	if (answered_by_key(idempotency_key, request_hash, err, job_id, &rc)) {
		goto out;
	}
	if (nm.ops->wifi_scan == NULL) {
		(void)api_error_init(err, API_ERR_CAPABILITY_UNAVAILABLE,
				     "This device has no Wi-Fi radio", NULL);
		rc = -ENOTSUP;
		goto out;
	}
	if (read_status_locked(&status) != 0 || !status.wifi.present) {
		(void)api_error_init(err, API_ERR_CAPABILITY_UNAVAILABLE,
				     "The Wi-Fi coprocessor is not available; scanning is unavailable",
				     NULL);
		rc = -ENOTSUP;
		goto out;
	}

	(void)expire_locked();

	/*
	 * Forbidden during an apply by the contract. A scan takes the radio
	 * off the channel it is associated on, which is the last thing to do
	 * to an interface whose new configuration is still unconfirmed.
	 */
	if (change_in_progress_locked()) {
		(void)api_error_init(err, API_ERR_BUSY,
				     "A network change is being applied; scan after it finishes",
				     NULL);
		rc = -EBUSY;
		goto out;
	}
	if (nm.scan.used && !nm.scan.complete) {
		(void)api_error_init(err, API_ERR_BUSY, "A Wi-Fi scan is already running", NULL);
		rc = -EBUSY;
		goto out;
	}

	const struct job_create_params params = {
		.kind = JOB_KIND_WIFI_SCAN,
		.cancellable = false,
		.idempotency_key = idempotency_key,
		.request_hash = request_hash,
	};
	struct job_snapshot job;

	switch (job_create(&params, &job)) {
	case JOB_CREATE_NEW:
		break;
	case JOB_CREATE_EXISTING:
		copy_job_id(job_id, job.id);
		rc = 0;
		goto out;
	case JOB_CREATE_CONFLICT:
		(void)api_error_init(err, API_ERR_IDEMPOTENCY_CONFLICT,
				     "This Idempotency-Key was used with a different request",
				     NULL);
		goto out;
	default:
		reject_exhausted(err);
		goto out;
	}

	memset(&nm.scan, 0, sizeof(nm.scan));
	nm.scan.used = true;
	nm.scan.requested = true;
	strncpy(nm.scan.job_id, job.id, sizeof(nm.scan.job_id) - 1);
	copy_job_id(job_id, job.id);
	rc = 0;

out:
	k_mutex_unlock(&lock);

	return rc;
}

int network_scan_results_get(const char *job_id, struct network_scan_results *out)
{
	if (out == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);

	int rc = -ENOENT;

	if (nm.scan.used && job_id != NULL &&
	    strncmp(nm.scan.job_id, job_id, JOB_ID_MAX_LEN) == 0) {
		if (!nm.scan.complete) {
			rc = -EAGAIN;
		} else if (nm.scan.failed) {
			rc = -EIO;
		} else {
			*out = nm.scan.results;
			rc = 0;
		}
	}

	k_mutex_unlock(&lock);

	return rc;
}

/* --- the worker's half -------------------------------------------------- */

/*
 * Push one configuration onto the hardware, without the mutex. With
 * @p stop_on_error the first refusal ends it, which is what an apply wants —
 * process() then restores the committed configuration; without, every part is
 * attempted, which is what a restore or the boot push wants: a Wi-Fi that will
 * not come up must not also leave Ethernet unconfigured.
 */
static int push_config(const struct device_config *cfg, enum device_config_generation secrets,
		       bool stop_on_error)
{
	const struct network_iface_ops *ops = nm.ops;
	int first = 0;
	int err = ops->configure(ops->ctx, DEVICE_CONFIG_INTERFACE_ETHERNET, &cfg->ethernet.ipv4,
				 cfg->ethernet.enabled);

	if (err != 0) {
		first = err;
		if (stop_on_error) {
			return err;
		}
	}

	if (!cfg->wifi.enabled) {
		err = ops->wifi_disconnect(ops->ctx);
		if (err == 0) {
			err = ops->configure(ops->ctx, DEVICE_CONFIG_INTERFACE_WIFI,
					     &cfg->wifi.ipv4, false);
		}
	} else {
		uint8_t password[DEVICE_CONFIG_SECRET_MAX_LEN];
		size_t password_len = 0;

		err = 0;
		if (cfg->wifi.security != DEVICE_CONFIG_WIFI_OPEN) {
			/*
			 * The pending generation during an apply, the committed
			 * one otherwise: the password that belongs to the
			 * network being joined is the one in the same
			 * generation as the SSID.
			 */
			if (device_config_secret_get(secrets, DEVICE_CONFIG_SECRET_WIFI_PASSWORD,
						     password, sizeof(password),
						     &password_len) != 0) {
				err = -EIO;
			}
		}
		if (err == 0) {
			err = ops->wifi_connect(ops->ctx, cfg->wifi.ssid, cfg->wifi.ssid_len,
						cfg->wifi.security, cfg->wifi.hidden,
						password_len > 0U ? password : NULL, password_len);
		}
		secure_wipe(password, sizeof(password));
		if (err == 0) {
			err = ops->configure(ops->ctx, DEVICE_CONFIG_INTERFACE_WIFI,
					     &cfg->wifi.ipv4, true);
		}
	}
	if (err != 0) {
		first = (first != 0) ? first : err;
		if (stop_on_error) {
			return err;
		}
	}

	/*
	 * DNS last, and always through this module. Section 5 requires one
	 * service to own DNS, DHCP ownership and Wi-Fi credentials together;
	 * an adapter that installed resolvers on its own would make manual DNS
	 * depend on which interface came up first.
	 */
	if (cfg->dns.mode == DEVICE_CONFIG_DNS_MANUAL) {
		err = ops->set_dns(ops->ctx, cfg->dns.servers, cfg->dns.server_count);
	} else {
		err = ops->set_dns(ops->ctx, NULL, 0);
	}

	return (first != 0) ? first : err;
}

/*
 * The next Wi-Fi join may be asked for @p wait_ms from now, and if that one
 * fails too, the one after waits twice as long. A push — a new configuration —
 * and a network that joined both start again from the first step.
 */
static void schedule_rejoin_locked(int32_t wait_ms)
{
	nm.reconnect_at_ms = now_ms() + wait_ms;
	nm.reconnect_backoff_ms = MIN(wait_ms * 2, RECONNECT_MAX_MS);
}

static int process_committed_push(void)
{
	struct device_config cfg;

	k_mutex_lock(&lock, K_FOREVER);
	if (!nm.push_committed) {
		k_mutex_unlock(&lock);
		return 0;
	}
	nm.push_committed = false;
	if (device_config_get(DEVICE_CONFIG_COMMITTED, &cfg) != 0) {
		device_config_defaults(&cfg);
	}
	schedule_rejoin_locked(RECONNECT_FIRST_MS);
	k_mutex_unlock(&lock);

	int err = push_config(&cfg, DEVICE_CONFIG_COMMITTED, false);

	if (err != 0) {
		LOG_ERR("configuring the interfaces from revision %u failed (%d)",
			device_config_revision(), err);
	}

	return 1;
}

static int process_apply(void)
{
	char id[NETWORK_TXN_ID_MAX_LEN + 1];
	struct device_config candidate;

	k_mutex_lock(&lock, K_FOREVER);
	if (!nm.txn.used || nm.txn.state != NETWORK_TXN_APPLYING || nm.txn.pushed) {
		k_mutex_unlock(&lock);
		return 0;
	}
	memcpy(id, nm.txn.id, sizeof(id));
	candidate = nm.txn.candidate;
	/* Whatever the push manages to change has to be undone from here on. */
	nm.txn.pushed = true;
	schedule_rejoin_locked(RECONNECT_FIRST_MS);
	k_mutex_unlock(&lock);

	int err = push_config(&candidate, DEVICE_CONFIG_PENDING, true);

	k_mutex_lock(&lock, K_FOREVER);
	if (txn_matches(id) && nm.txn.state == NETWORK_TXN_APPLYING) {
		if (err != 0) {
			LOG_ERR("transaction %s: applying failed (%d)", id, err);
			begin_rollback_locked(ROLLBACK_APPLY_FAILED);
		} else {
			nm.txn.state = NETWORK_TXN_AWAITING_CONFIRMATION;
			(void)job_set_phase(nm.txn.job_id, "awaiting_confirmation");
			(void)job_set_state(nm.txn.job_id, JOB_STATE_WAITING_CONFIRMATION);
			LOG_INF("transaction %s applied, awaiting confirmation", id);
		}
	}
	k_mutex_unlock(&lock);

	return 1;
}

static int process_commit(void)
{
	char id[NETWORK_TXN_ID_MAX_LEN + 1];

	k_mutex_lock(&lock, K_FOREVER);
	if (!nm.txn.used || nm.txn.state != NETWORK_TXN_AWAITING_CONFIRMATION ||
	    !nm.txn.commit_requested) {
		k_mutex_unlock(&lock);
		return 0;
	}
	memcpy(id, nm.txn.id, sizeof(id));
	k_mutex_unlock(&lock);

	int rc = device_config_pending_commit(id);

	k_mutex_lock(&lock, K_FOREVER);
	if (txn_matches(id) && nm.txn.state == NETWORK_TXN_AWAITING_CONFIRMATION) {
		if (rc == 0) {
			(void)job_set_state(nm.txn.job_id, JOB_STATE_SUCCEEDED);
			/* The candidate is the committed generation now. */
			nm.txn.pushed = false;
			finish(NETWORK_TXN_COMMITTED, API_ERR_COUNT, false);
			LOG_INF("transaction %s committed, revision %u", id,
				device_config_revision());
		} else {
			/*
			 * The generation that works could not be made durable.
			 * Keeping it would leave a device whose next reboot
			 * returns to something else, so it is undone now while
			 * the operator is watching.
			 */
			LOG_ERR("transaction %s: commit failed (%d); rolling back", id, rc);
			nm.txn.commit_requested = false;
			begin_rollback_locked(ROLLBACK_COMMIT_FAILED);
		}
	}
	k_mutex_unlock(&lock);

	return 1;
}

static int process_rollback(void)
{
	char id[NETWORK_TXN_ID_MAX_LEN + 1];
	struct device_config committed;
	bool pushed;

	k_mutex_lock(&lock, K_FOREVER);
	if (!nm.txn.used || nm.txn.state != NETWORK_TXN_ROLLING_BACK) {
		k_mutex_unlock(&lock);
		return 0;
	}
	memcpy(id, nm.txn.id, sizeof(id));
	pushed = nm.txn.pushed;
	if (device_config_get(DEVICE_CONFIG_COMMITTED, &committed) != 0) {
		device_config_defaults(&committed);
	}
	if (pushed) {
		schedule_rejoin_locked(RECONNECT_FIRST_MS);
	}
	k_mutex_unlock(&lock);

	/*
	 * Best effort by nature: if restoring fails there is nothing further to
	 * try, and the journal still has to go so the next boot does not
	 * resurrect the attempt. A candidate that never reached the interfaces
	 * has nothing on them to undo.
	 */
	if (pushed) {
		int err = push_config(&committed, DEVICE_CONFIG_COMMITTED, false);

		if (err != 0) {
			LOG_ERR("restoring the committed configuration failed (%d)", err);
		}
	}
	(void)device_config_pending_rollback(id);

	k_mutex_lock(&lock, K_FOREVER);
	if (txn_matches(id) && nm.txn.state == NETWORK_TXN_ROLLING_BACK) {
		nm.txn.pushed = false;
		switch (nm.txn.rollback_reason) {
		case ROLLBACK_REQUESTED:
			/* A rollback the administrator asked for is a success. */
			(void)job_set_state(nm.txn.job_id, JOB_STATE_SUCCEEDED);
			finish(NETWORK_TXN_ROLLED_BACK, API_ERR_COUNT, false);
			break;
		case ROLLBACK_TIMED_OUT:
			fail_job(nm.txn.job_id, API_ERR_RESOURCE_EXPIRED);
			finish(NETWORK_TXN_ROLLED_BACK, API_ERR_RESOURCE_EXPIRED, true);
			break;
		default:
			fail_job(nm.txn.job_id, API_ERR_INTERNAL_ERROR);
			finish(NETWORK_TXN_FAILED, API_ERR_INTERNAL_ERROR, true);
			break;
		}
		LOG_INF("transaction %s %s", id, network_transaction_state_str(nm.txn.state));
	}
	k_mutex_unlock(&lock);

	return 1;
}

static int process_scan(void)
{
	/* process() runs on one thread, and this is kilobytes. */
	static struct network_scan_results scratch;
	char id[JOB_ID_MAX_LEN + 1];

	k_mutex_lock(&lock, K_FOREVER);
	if (!nm.scan.used || !nm.scan.requested) {
		k_mutex_unlock(&lock);
		return 0;
	}
	nm.scan.requested = false;
	memcpy(id, nm.scan.job_id, sizeof(id));
	if (change_in_progress_locked()) {
		/* An apply was accepted after this scan was: it waits for no scan. */
		nm.scan.complete = true;
		nm.scan.failed = true;
		k_mutex_unlock(&lock);
		(void)job_set_state(id, JOB_STATE_RUNNING);
		fail_job(id, API_ERR_BUSY);
		return 1;
	}
	k_mutex_unlock(&lock);

	(void)job_set_state(id, JOB_STATE_RUNNING);
	(void)job_set_phase(id, "scanning");

	memset(&scratch, 0, sizeof(scratch));

	int err = nm.ops->wifi_scan(nm.ops->ctx, &scratch);

	/*
	 * The adapter is trusted to fill the array but not to count it: a
	 * count past the end would be read as valid entries by everything
	 * downstream. Over the cap the extra results are dropped and the
	 * client is told, which the contract requires rather than silently
	 * presenting a partial list as complete.
	 */
	if (scratch.count > ARRAY_SIZE(scratch.items)) {
		scratch.count = ARRAY_SIZE(scratch.items);
		scratch.truncated = true;
	}
	for (uint8_t i = 0; i < scratch.count; i++) {
		struct network_access_point *ap = &scratch.items[i];

		if ((unsigned int)ap->security >= NETWORK_AP_SECURITY_COUNT) {
			ap->security = NETWORK_AP_UNKNOWN;
		}
		ap->connect_supported = network_ap_security_connectable(ap->security);
	}

	k_mutex_lock(&lock, K_FOREVER);
	if (nm.scan.used && strncmp(nm.scan.job_id, id, JOB_ID_MAX_LEN) == 0) {
		nm.scan.complete = true;
		nm.scan.failed = (err != 0);
		if (err == 0) {
			nm.scan.results = scratch;
		}
	}
	k_mutex_unlock(&lock);

	if (err != 0) {
		LOG_WRN("scan failed (%d)", err);
		fail_job(id, API_ERR_SERVICE_NOT_READY);
	} else {
		(void)job_set_state(id, JOB_STATE_SUCCEEDED);
	}

	return 1;
}

/* Ask Wi-Fi to join the network in force again. Without the mutex. */
static int rejoin(const struct device_config *cfg, enum device_config_generation secrets)
{
	uint8_t password[DEVICE_CONFIG_SECRET_MAX_LEN];
	size_t password_len = 0;
	int err = 0;

	if (cfg->wifi.security != DEVICE_CONFIG_WIFI_OPEN &&
	    device_config_secret_get(secrets, DEVICE_CONFIG_SECRET_WIFI_PASSWORD, password,
				     sizeof(password), &password_len) != 0) {
		err = -EIO;
	}
	if (err == 0) {
		err = nm.ops->wifi_connect(nm.ops->ctx, cfg->wifi.ssid, cfg->wifi.ssid_len,
					   cfg->wifi.security, cfg->wifi.hidden,
					   password_len > 0U ? password : NULL, password_len);
	}
	secure_wipe(password, sizeof(password));

	return err;
}

/*
 * Keep the policies in force: the default route follows the selection, a
 * manual resolver list stays installed however often DHCP or a router
 * advertisement replaces it, and a Wi-Fi network that dropped is joined again.
 */
static int process_policy(void)
{
	static struct network_status status;
	struct device_config cfg;
	enum device_config_generation secrets = DEVICE_CONFIG_COMMITTED;
	bool set_route;
	bool set_dns;
	bool reconnect = false;

	k_mutex_lock(&lock, K_FOREVER);
	if (read_status_locked(&status) != 0) {
		k_mutex_unlock(&lock);
		return 0;
	}
	in_force_locked(&cfg);
	set_route = nm.ops->set_default != NULL && status.has_active &&
		    (!nm.has_default || nm.default_iface != status.active);
	set_dns = nm.ops->get_dns != NULL && cfg.dns.mode == DEVICE_CONFIG_DNS_MANUAL &&
		  !dns_matches(&status, &cfg.dns);

	if (status.wifi.wifi_associated) {
		/* Joined: a drop from here waits the first step, not the last. */
		schedule_rejoin_locked(RECONNECT_FIRST_MS);
	} else if (cfg.wifi.enabled && status.wifi.present && !status.wifi.wifi_connecting &&
		   !(nm.txn.used && (nm.txn.state == NETWORK_TXN_APPLYING ||
				     nm.txn.state == NETWORK_TXN_ROLLING_BACK)) &&
		   now_ms() >= nm.reconnect_at_ms) {
		/*
		 * Not while a change is about to reach the interfaces or to be
		 * undone: that push joins the network itself.
		 */
		reconnect = true;
		secrets = (nm.txn.used && nm.txn.pushed) ? DEVICE_CONFIG_PENDING
							 : DEVICE_CONFIG_COMMITTED;
		schedule_rejoin_locked(nm.reconnect_backoff_ms > 0 ? nm.reconnect_backoff_ms
								   : RECONNECT_FIRST_MS);
	}
	k_mutex_unlock(&lock);

	int steps = 0;

	if (reconnect) {
		int err = rejoin(&cfg, secrets);

		LOG_INF("Wi-Fi rejoin requested (%d)", err);
		steps++;
	}

	if (set_route && nm.ops->set_default(nm.ops->ctx, status.active) == 0) {
		k_mutex_lock(&lock, K_FOREVER);
		nm.has_default = true;
		nm.default_iface = status.active;
		k_mutex_unlock(&lock);
		LOG_INF("default route via %s", device_config_interface_str(status.active));
		steps++;
	}
	if (set_dns) {
		(void)nm.ops->set_dns(nm.ops->ctx, cfg.dns.servers, cfg.dns.server_count);
		steps++;
	}

	return steps;
}

int network_manager_process(void)
{
	int steps;

	k_mutex_lock(&lock, K_FOREVER);
	if (!nm.initialised) {
		k_mutex_unlock(&lock);
		return 0;
	}
	steps = expire_locked();
	k_mutex_unlock(&lock);

	/*
	 * The commit goes before the rollback, and both before the scan: a
	 * confirmation that arrived in time is honoured even when its deadline
	 * has passed since, and a queued scan finds out whether a change began
	 * in the meantime.
	 */
	steps += process_committed_push();
	steps += process_apply();
	steps += process_commit();
	steps += process_rollback();
	steps += process_scan();
	steps += process_policy();

	return steps;
}

/* --- lifecycle ---------------------------------------------------------- */

void network_manager_boot(const struct device_config_recovery_report *report)
{
	k_mutex_lock(&lock, K_FOREVER);
	nm.push_committed = true;
	memset(nm.lost_txn, 0, sizeof(nm.lost_txn));
	if (report != NULL && report->transaction_id[0] != '\0') {
		strncpy(nm.lost_txn, report->transaction_id, sizeof(nm.lost_txn) - 1);
	}
	k_mutex_unlock(&lock);
}

bool network_manager_lost_in_reboot(const char *txn_id)
{
	k_mutex_lock(&lock, K_FOREVER);

	bool lost = txn_id != NULL && nm.lost_txn[0] != '\0' &&
		    strncmp(nm.lost_txn, txn_id, NETWORK_TXN_ID_MAX_LEN) == 0;

	k_mutex_unlock(&lock);

	return lost;
}

int network_manager_restore_defaults(void)
{
	static const char id[] = "restore_defaults";
	struct device_config committed;
	struct device_config_update update;
	int rc;

	k_mutex_lock(&lock, K_FOREVER);

	if (nm.txn.used && !network_transaction_state_is_terminal(nm.txn.state)) {
		rc = -EBUSY;
		goto out;
	}
	rc = device_config_get(DEVICE_CONFIG_COMMITTED, &committed);
	if (rc != 0) {
		goto out;
	}

	memset(&update, 0, sizeof(update));
	device_config_defaults(&update.config);
	/*
	 * The profile is kept so the form still says which network it was;
	 * the password is not, because the reason for a physical recovery may
	 * well be that the network it belongs to is the one that failed.
	 */
	update.config.wifi.enabled = 0;
	update.config.wifi.security = committed.wifi.security;
	update.config.wifi.hidden = committed.wifi.hidden;
	update.config.wifi.ssid_len = committed.wifi.ssid_len;
	memcpy(update.config.wifi.ssid, committed.wifi.ssid, sizeof(update.config.wifi.ssid));
	update.secrets[DEVICE_CONFIG_SECRET_WIFI_PASSWORD].action = DEVICE_CONFIG_SECRET_CLEAR;
	update.secrets[DEVICE_CONFIG_SECRET_ADMIN_PASSWORD].action = DEVICE_CONFIG_SECRET_KEEP;

	rc = device_config_pending_begin(&update, device_config_revision(), id);
	if (rc == 0) {
		rc = device_config_pending_commit(id);
	}
	if (rc == 0) {
		nm.push_committed = true;
		LOG_WRN("network configuration restored to factory defaults, revision %u",
			device_config_revision());
	} else {
		LOG_ERR("restoring the factory network configuration failed (%d)", rc);
	}

out:
	k_mutex_unlock(&lock);

	return rc;
}

int network_manager_init(const struct network_iface_ops *ops)
{
	if (ops == NULL || ops->configure == NULL || ops->wifi_connect == NULL ||
	    ops->wifi_disconnect == NULL || ops->get_status == NULL || ops->set_dns == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);

	secure_wipe(&nm, sizeof(nm));
	nm.ops = ops;
	nm.initialised = true;

	k_mutex_unlock(&lock);

	return 0;
}
