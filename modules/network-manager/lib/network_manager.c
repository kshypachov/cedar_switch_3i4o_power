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
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "network_internal.h"

LOG_MODULE_REGISTER(network_manager, CONFIG_NETWORK_MANAGER_LOG_LEVEL);

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
};

struct scan_record {
	bool used;
	char job_id[JOB_ID_MAX_LEN + 1];
	bool complete;
	struct network_scan_results results;
};

static struct {
	const struct network_iface_ops *ops;
	struct transaction txn;
	struct scan_record scan;
	uint32_t id_counter;
	bool initialised;
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

/* --- status ------------------------------------------------------------- */

static int read_status_locked(struct network_status *out)
{
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

	/*
	 * Which interface is actually carrying traffic is decided by the route
	 * the device holds, not by the preference in the configuration. The
	 * plan is explicit that net_if_set_default() alone does not prove the
	 * routing is what anyone intended, so the observed route is what gets
	 * reported.
	 */
	if (out->ethernet.has_route) {
		out->active = DEVICE_CONFIG_INTERFACE_ETHERNET;
		out->has_active = true;
	} else if (out->wifi.has_route) {
		out->active = DEVICE_CONFIG_INTERFACE_WIFI;
		out->has_active = true;
	}

	return 0;
}

int network_get_status(struct network_status *out)
{
	if (out == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);

	int err;

	if (!nm.initialised) {
		err = -EINVAL;
	} else {
		err = read_status_locked(out);
		if (err == 0) {
			struct device_config cfg;

			/*
			 * Resolvers come from the committed configuration only
			 * when they were set manually; with automatic DNS the
			 * device holds whatever DHCP handed it, which the
			 * adapter reports and this module does not invent.
			 */
			if (device_config_get(DEVICE_CONFIG_COMMITTED, &cfg) == 0 &&
			    cfg.dns.mode == DEVICE_CONFIG_DNS_MANUAL) {
				out->dns_count = MIN(cfg.dns.server_count,
						     (uint8_t)DEVICE_CONFIG_DNS_MAX_SERVERS);
				memcpy(out->dns, cfg.dns.servers,
				       sizeof(cfg.dns.servers[0]) * out->dns_count);
			}
		}
	}

	k_mutex_unlock(&lock);

	return err;
}

/* --- transaction helpers ------------------------------------------------ */

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

		out->remaining_seconds = (left <= 0) ? 0 : (int32_t)((left + 999) / 1000);
	} else {
		out->remaining_seconds = -1;
	}
}

static void make_id(char *buf, size_t cap)
{
	nm.id_counter++;
	(void)snprintf(buf, cap, "txn_%08x", nm.id_counter);
}

static bool txn_matches(const char *id)
{
	return nm.txn.used && id != NULL &&
	       strncmp(nm.txn.id, id, NETWORK_TXN_ID_MAX_LEN) == 0;
}

static void clear_transaction(void)
{
	secure_wipe(&nm.txn, sizeof(nm.txn));
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
	nm.txn.error_code = code;
	nm.txn.has_error = has_error;
	secure_wipe(nm.txn.credential_value, sizeof(nm.txn.credential_value));
	nm.txn.credential_len = 0;
	nm.txn.credential.action = DEVICE_CONFIG_SECRET_KEEP;
	nm.txn.credential.value = NULL;
	nm.txn.credential.len = 0;
}

/* --- staging ------------------------------------------------------------ */

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

	/*
	 * A transaction still in flight owns the interfaces. Replacing it
	 * would strand whoever is waiting on it, so a second one is refused
	 * with the code the contract reserves for exactly this.
	 */
	if (nm.txn.used && !network_transaction_state_is_terminal(nm.txn.state)) {
		(void)api_error_init(err, API_ERR_BUSY,
				     "Another network transaction is in progress", NULL);
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
		(void)api_error_init(err, API_ERR_STALE_REVISION,
				     "Configuration changed since it was read", NULL);
		goto out;
	}

	if (read_status_locked(&status) != 0) {
		(void)api_error_init(err, API_ERR_INTERNAL_ERROR,
				     "Interface state unavailable", NULL);
		goto out;
	}

	(void)api_error_init(err, API_ERR_VALIDATION_FAILED, "Network configuration is not valid",
			     NULL);
	if (!network_validate_config(input, &committed, &status, err)) {
		goto out;
	}

	clear_transaction();
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

int network_transaction_get(const char *txn_id, struct network_transaction *out)
{
	if (out == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);

	int rc = -ENOENT;

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

	if (nm.txn.used) {
		snapshot_locked(&nm.txn, out);
		rc = 0;
	}

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
		  struct api_error *err)
{
	if (err == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);

	int rc = -EINVAL;

	if (!txn_matches(txn_id)) {
		(void)api_error_init(err, API_ERR_NOT_FOUND, "No such transaction", NULL);
		goto out;
	}
	if (nm.txn.state != NETWORK_TXN_STAGED) {
		(void)api_error_init(err, API_ERR_INVALID_STATE,
				     "Transaction is not staged", NULL);
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
		.idempotency_key = NULL,
	};
	struct job_snapshot job;

	if (job_create(&params, &job) != JOB_CREATE_NEW) {
		(void)api_error_init(err, API_ERR_BUSY, "No capacity to track this operation",
				     NULL);
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
		(void)job_fail(job.id, "internal_error", true);
		finish(NETWORK_TXN_FAILED,
		       (store_rc == -ESTALE) ? API_ERR_STALE_REVISION : API_ERR_INTERNAL_ERROR,
		       true);
		(void)api_error_init(err,
				     (store_rc == -ESTALE) ? API_ERR_STALE_REVISION
							   : API_ERR_INTERNAL_ERROR,
				     "Could not record the pending configuration", NULL);
		LOG_ERR("transaction %s: journal failed (%d)", nm.txn.id, store_rc);
		goto out;
	}

	strncpy(nm.txn.job_id, job.id, sizeof(nm.txn.job_id) - 1);
	nm.txn.has_job = true;
	nm.txn.state = NETWORK_TXN_APPLYING;
	/*
	 * The confirmation deadline is armed here rather than after the
	 * interfaces change, so that a worker which never runs — or dies
	 * mid-apply — still ends in a rollback instead of waiting forever.
	 */
	nm.txn.deadline_ms = now_ms() + (int64_t)timeout * 1000;

	LOG_INF("transaction %s journalled, job %s, %u s to confirm", nm.txn.id, job.id, timeout);
	rc = 0;

out:
	k_mutex_unlock(&lock);

	return rc;
}

/* Push one configuration onto the hardware. Returns 0 or a negative errno. */
static int push_config(const struct device_config *cfg, bool wifi_credential_from_pending)
{
	int err = nm.ops->configure(nm.ops->ctx, DEVICE_CONFIG_INTERFACE_ETHERNET,
				    &cfg->ethernet.ipv4, cfg->ethernet.enabled);

	if (err != 0) {
		return err;
	}

	if (!cfg->wifi.enabled) {
		err = nm.ops->wifi_disconnect(nm.ops->ctx);
		if (err != 0) {
			return err;
		}
	} else {
		uint8_t password[DEVICE_CONFIG_SECRET_MAX_LEN];
		size_t password_len = 0;

		if (cfg->wifi.security != DEVICE_CONFIG_WIFI_OPEN) {
			/*
			 * The pending generation during an apply, the committed
			 * one when restoring: the password that belongs to the
			 * network being joined is the one in the same
			 * generation as the SSID.
			 */
			const enum device_config_generation gen =
				wifi_credential_from_pending ? DEVICE_CONFIG_PENDING
							     : DEVICE_CONFIG_COMMITTED;

			if (device_config_secret_get(gen, DEVICE_CONFIG_SECRET_WIFI_PASSWORD,
						     password, sizeof(password),
						     &password_len) != 0) {
				return -EIO;
			}
		}

		err = nm.ops->wifi_connect(nm.ops->ctx, cfg->wifi.ssid, cfg->wifi.ssid_len,
					   cfg->wifi.security, cfg->wifi.hidden,
					   password_len > 0U ? password : NULL, password_len);
		secure_wipe(password, sizeof(password));
		if (err != 0) {
			return err;
		}

		err = nm.ops->configure(nm.ops->ctx, DEVICE_CONFIG_INTERFACE_WIFI,
					&cfg->wifi.ipv4, true);
		if (err != 0) {
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
		return nm.ops->set_dns(nm.ops->ctx, cfg->dns.servers, cfg->dns.server_count);
	}

	return nm.ops->set_dns(nm.ops->ctx, NULL, 0);
}

/* Put the interfaces back to the committed generation. Best effort by nature:
 * if this fails there is nothing further to try, and the journal still has to
 * go so the next boot does not resurrect the attempt.
 */
static void restore_committed(void)
{
	struct device_config committed;

	if (device_config_get(DEVICE_CONFIG_COMMITTED, &committed) == 0) {
		int err = push_config(&committed, false);

		if (err != 0) {
			LOG_ERR("restoring the committed configuration failed (%d)", err);
		}
	}
}

int network_apply_execute(const char *txn_id)
{
	k_mutex_lock(&lock, K_FOREVER);

	int rc = -EINVAL;

	if (!txn_matches(txn_id) || nm.txn.state != NETWORK_TXN_APPLYING) {
		goto out;
	}

	(void)job_set_state(nm.txn.job_id, JOB_STATE_RUNNING);
	(void)job_set_phase(nm.txn.job_id, "applying");

	int err = push_config(&nm.txn.candidate, true);

	if (err != 0) {
		LOG_ERR("transaction %s: applying failed (%d)", nm.txn.id, err);
		restore_committed();
		(void)device_config_pending_rollback(nm.txn.id);
		(void)job_fail(nm.txn.job_id, "internal_error", true);
		finish(NETWORK_TXN_FAILED, API_ERR_INTERNAL_ERROR, true);
		rc = -EIO;
		goto out;
	}

	nm.txn.state = NETWORK_TXN_AWAITING_CONFIRMATION;
	(void)job_set_phase(nm.txn.job_id, "awaiting_confirmation");
	(void)job_set_state(nm.txn.job_id, JOB_STATE_WAITING_CONFIRMATION);
	LOG_INF("transaction %s applied, awaiting confirmation", nm.txn.id);
	rc = 0;

out:
	k_mutex_unlock(&lock);

	return rc;
}

/* --- confirm and rollback ----------------------------------------------- */

int network_confirm(const char *txn_id, struct api_error *err)
{
	if (err == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);

	int rc = -EINVAL;

	if (!txn_matches(txn_id)) {
		(void)api_error_init(err, API_ERR_NOT_FOUND, "No such transaction", NULL);
		goto out;
	}
	if (nm.txn.state != NETWORK_TXN_AWAITING_CONFIRMATION) {
		(void)api_error_init(err, API_ERR_INVALID_STATE,
				     "Transaction is not awaiting confirmation", NULL);
		goto out;
	}

	struct network_status status;

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
				     "Not every enabled interface is working yet", NULL);
		rc = -EAGAIN;
		goto out;
	}

	if (device_config_pending_commit(nm.txn.id) != 0) {
		(void)api_error_init(err, API_ERR_INTERNAL_ERROR,
				     "Could not commit the configuration", NULL);
		(void)job_fail(nm.txn.job_id, "internal_error", true);
		finish(NETWORK_TXN_FAILED, API_ERR_INTERNAL_ERROR, true);
		goto out;
	}

	(void)job_set_state(nm.txn.job_id, JOB_STATE_SUCCEEDED);
	finish(NETWORK_TXN_COMMITTED, API_ERR_COUNT, false);
	LOG_INF("transaction %s committed, revision %u", nm.txn.id, device_config_revision());
	rc = 0;

out:
	k_mutex_unlock(&lock);

	return rc;
}

/* Shared by the explicit rollback and the deadline. */
static void roll_back_locked(bool timed_out)
{
	if (nm.txn.state == NETWORK_TXN_STAGED) {
		/* Never applied: there is no journal and no hardware to undo. */
		finish(NETWORK_TXN_ROLLED_BACK, API_ERR_COUNT, false);
		return;
	}

	nm.txn.state = NETWORK_TXN_ROLLING_BACK;
	if (nm.txn.has_job) {
		(void)job_set_phase(nm.txn.job_id, "rolling_back");
	}

	restore_committed();
	(void)device_config_pending_rollback(nm.txn.id);

	if (nm.txn.has_job) {
		if (timed_out) {
			(void)job_fail(nm.txn.job_id, "invalid_state", true);
		} else {
			(void)job_set_state(nm.txn.job_id, JOB_STATE_CANCELLED);
		}
	}

	finish(NETWORK_TXN_ROLLED_BACK, timed_out ? API_ERR_INVALID_STATE : API_ERR_COUNT,
	       timed_out);
}

int network_rollback(const char *txn_id, struct api_error *err)
{
	if (err == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);

	int rc = -EINVAL;

	if (!txn_matches(txn_id)) {
		(void)api_error_init(err, API_ERR_NOT_FOUND, "No such transaction", NULL);
		goto out;
	}
	if (network_transaction_state_is_terminal(nm.txn.state)) {
		(void)api_error_init(err, API_ERR_INVALID_STATE, "Transaction already finished",
				     NULL);
		goto out;
	}
	if (nm.txn.state == NETWORK_TXN_APPLYING) {
		/*
		 * The worker is between the journal and the interfaces.
		 * Interrupting it there would leave nobody able to say whether
		 * the hardware had been touched, so the caller waits; the
		 * deadline still covers the case where the worker never
		 * finishes.
		 */
		(void)api_error_init(err, API_ERR_BUSY, "Change is being applied", NULL);
		goto out;
	}

	roll_back_locked(false);
	LOG_INF("transaction %s rolled back on request", nm.txn.id);
	rc = 0;

out:
	k_mutex_unlock(&lock);

	return rc;
}

int network_manager_tick(void)
{
	k_mutex_lock(&lock, K_FOREVER);

	int changed = 0;

	if (nm.initialised && nm.txn.used && nm.txn.deadline_ms > 0 &&
	    now_ms() >= nm.txn.deadline_ms) {
		switch (nm.txn.state) {
		case NETWORK_TXN_STAGED:
			/* Nobody applied it. Nothing was changed, so it simply
			 * goes away — a candidate kept indefinitely would block
			 * the next one.
			 */
			finish(NETWORK_TXN_EXPIRED, API_ERR_RESOURCE_EXPIRED, true);
			LOG_INF("candidate %s expired", nm.txn.id);
			changed++;
			break;

		case NETWORK_TXN_APPLYING:
		case NETWORK_TXN_AWAITING_CONFIRMATION:
			/*
			 * The case this module exists for: the change went
			 * live and nobody could confirm it. Silence is a
			 * rollback.
			 */
			LOG_WRN("transaction %s not confirmed in time; rolling back", nm.txn.id);
			roll_back_locked(true);
			changed++;
			break;

		default:
			break;
		}
	}

	k_mutex_unlock(&lock);

	return changed;
}

/* --- scan --------------------------------------------------------------- */

int network_scan_begin(const char *idempotency_key, uint32_t request_hash, struct api_error *err,
		       char job_id[JOB_ID_MAX_LEN + 1])
{
	if (err == NULL || job_id == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);

	int rc = -EINVAL;

	if (!nm.initialised) {
		(void)api_error_init(err, API_ERR_SERVICE_NOT_READY, "Network manager not ready",
				     NULL);
		goto out;
	}
	if (nm.ops->wifi_scan == NULL) {
		(void)api_error_init(err, API_ERR_CAPABILITY_UNAVAILABLE,
				     "This device cannot scan for networks", NULL);
		rc = -ENOTSUP;
		goto out;
	}
	/*
	 * Forbidden during an apply by the contract. A scan takes the radio
	 * off the channel it is associated on, which is the last thing to do
	 * to an interface whose new configuration is still unconfirmed.
	 */
	if (nm.txn.used && !network_transaction_state_is_terminal(nm.txn.state) &&
	    nm.txn.state != NETWORK_TXN_STAGED) {
		(void)api_error_init(err, API_ERR_BUSY, "A network change is in progress", NULL);
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
	enum job_create_result res = job_create(&params, &job);

	if (res == JOB_CREATE_EXISTING) {
		/* A retry of the same request gets the same scan, not a second one. */
		strncpy(job_id, job.id, JOB_ID_MAX_LEN);
		job_id[JOB_ID_MAX_LEN] = '\0';
		rc = 0;
		goto out;
	}
	if (res == JOB_CREATE_CONFLICT) {
		(void)api_error_init(err, API_ERR_IDEMPOTENCY_CONFLICT,
				     "This key was used for a different request", NULL);
		goto out;
	}
	if (res != JOB_CREATE_NEW) {
		(void)api_error_init(err, API_ERR_RATE_LIMITED,
				     "No capacity to track this operation", NULL);
		(void)api_error_set_retry_after(err, 5);
		goto out;
	}

	memset(&nm.scan, 0, sizeof(nm.scan));
	nm.scan.used = true;
	strncpy(nm.scan.job_id, job.id, sizeof(nm.scan.job_id) - 1);

	strncpy(job_id, job.id, JOB_ID_MAX_LEN);
	job_id[JOB_ID_MAX_LEN] = '\0';
	rc = 0;

out:
	k_mutex_unlock(&lock);

	return rc;
}

int network_scan_execute(const char *job_id)
{
	k_mutex_lock(&lock, K_FOREVER);

	int rc = -ENOENT;

	if (!nm.scan.used || job_id == NULL ||
	    strncmp(nm.scan.job_id, job_id, JOB_ID_MAX_LEN) != 0) {
		goto out;
	}

	(void)job_set_state(nm.scan.job_id, JOB_STATE_RUNNING);

	struct network_scan_results results;

	memset(&results, 0, sizeof(results));

	int err = nm.ops->wifi_scan(nm.ops->ctx, &results);

	if (err != 0) {
		(void)job_fail(nm.scan.job_id, "internal_error", true);
		rc = err;
		goto out;
	}

	/*
	 * The adapter is trusted to fill the array but not to count it: a
	 * count past the end would be read as valid entries by everything
	 * downstream. Over the cap the extra results are dropped and the
	 * client is told, which the contract requires rather than silently
	 * presenting a partial list as complete.
	 */
	if (results.count > ARRAY_SIZE(results.items)) {
		results.count = ARRAY_SIZE(results.items);
		results.truncated = true;
	}

	nm.scan.results = results;
	nm.scan.complete = true;
	(void)job_set_state(nm.scan.job_id, JOB_STATE_SUCCEEDED);
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
		if (nm.scan.complete) {
			*out = nm.scan.results;
			rc = 0;
		} else {
			rc = -EAGAIN;
		}
	}

	k_mutex_unlock(&lock);

	return rc;
}

/* --- init --------------------------------------------------------------- */

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
