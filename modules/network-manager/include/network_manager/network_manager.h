/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Network manager: the transaction that changes this device's network without
 * being able to lose it.
 *
 * The contract this implements is section 5 of
 * docs/device-development/development-plan.md and the "Сеть" section of
 * docs/device-development/api-contract.md. The problem it exists to solve is
 * narrow and unforgiving: the only way to manage this device is over the
 * network, so a change that goes wrong takes away the means of undoing it. A
 * wrong address, a Wi-Fi password that does not work, a gateway on another
 * subnet — each of those turns a five-second edit into a service visit.
 *
 * Everything here follows from that:
 *
 *  - Nothing is applied until it has been checked and journalled. Validation
 *    happens while the device is still reachable, and the durable record of
 *    what is being attempted is written before the first interface is touched.
 *  - A change is not kept until someone confirms from the other side that they
 *    can still reach the device. Silence is a rollback, not a success — the
 *    confirmation deadline exists precisely because the failure mode is a
 *    client that can no longer speak to us.
 *  - The health check is what the device can observe: link, address, route. No
 *    reachability test against a public host, which would make a working LAN
 *    look broken whenever the internet is down.
 *
 * Design notes worth knowing before using this:
 *
 *  - No threads and no I/O of its own. Interfaces are reached through an
 *    injected ops table, and time through an injected clock, so the whole
 *    transaction — including a confirmation that never arrives — is exercised
 *    in the sim tier without a board or a stopwatch.
 *  - Applying is deliberately two calls. network_apply() validates, journals
 *    and returns; network_apply_execute() is what the worker runs afterwards.
 *    The contract requires the journal to be durable and the HTTP response to
 *    be sent before the network changes, and one call could not do both.
 *  - The admin password is not reachable from here. The input type carries
 *    only the Wi-Fi credential, so a network transaction structurally cannot
 *    touch the credential that guards the API.
 *  - Candidates live in RAM and expire. Exactly one exists at a time; a second
 *    is refused rather than silently replacing a transaction another
 *    administrator is waiting on.
 *
 * Threading: every entry point takes an internal mutex and snapshots are
 * copied out under it. Not ISR-safe. The adapter callbacks are invoked with
 * that mutex held, so an adapter must not call back into this module.
 *
 * What this module does not do: no JSON, no HTTP, no knowledge of cookies or
 * request bodies. It reports failures as an api_error the web layer
 * serialises. It does not own storage — device-config-store holds the
 * generations — and it does not own job identity — job-manager does.
 */

#ifndef NETWORK_MANAGER_H_
#define NETWORK_MANAGER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <api_validation/api_validation.h>
#include <device_config_store/device_config_store.h>
#include <job_manager/job_manager.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Transaction id length, matching `^[A-Za-z0-9_-]{1,64}$` in openapi.json. */
#define NETWORK_TXN_ID_MAX_LEN DEVICE_CONFIG_TXN_ID_MAX_LEN

/**
 * @brief Transaction lifecycle.
 *
 * Mirrors the `state` enum of NetworkTransaction in openapi.json exactly.
 * Legal transitions:
 *
 *   STAGED                -> APPLYING | ROLLED_BACK | EXPIRED
 *   APPLYING              -> AWAITING_CONFIRMATION | FAILED
 *   AWAITING_CONFIRMATION -> COMMITTED | ROLLING_BACK
 *   ROLLING_BACK          -> ROLLED_BACK | FAILED
 *   COMMITTED, ROLLED_BACK, FAILED, EXPIRED -> nothing
 *
 * ROLLED_BACK is reached from STAGED as well, because discarding a candidate
 * that was never applied and undoing one that was are the same thing to a
 * client polling the transaction: the change did not happen.
 */
enum network_transaction_state {
	NETWORK_TXN_STAGED = 0,
	NETWORK_TXN_APPLYING,
	NETWORK_TXN_AWAITING_CONFIRMATION,
	NETWORK_TXN_COMMITTED,
	NETWORK_TXN_ROLLING_BACK,
	NETWORK_TXN_ROLLED_BACK,
	NETWORK_TXN_FAILED,
	NETWORK_TXN_EXPIRED,

	NETWORK_TXN_STATE_COUNT
};

/**
 * @brief What a caller proposes.
 *
 * Note what is absent: the admin credential. device_config_update carries both
 * secrets, and network-manager deliberately takes a narrower type so that no
 * path through a network transaction can reach the password that guards the
 * API. web-auth owns that one.
 */
struct network_config_input {
	/** Proposed configuration. The derived `*_set` flags are ignored. */
	struct device_config config;
	/** keep / replace / clear for the Wi-Fi password only. */
	struct device_config_secret_update wifi_password;
};

/** A transaction as a caller sees it. Contains no secret material. */
struct network_transaction {
	char id[NETWORK_TXN_ID_MAX_LEN + 1];
	enum network_transaction_state state;
	/** Committed revision this was staged against. */
	uint32_t base_revision;
	/** The proposal. `password_set` is truthful; the password is not here. */
	struct device_config candidate;
	/** The apply job, empty while the transaction is merely staged. */
	char job_id[JOB_ID_MAX_LEN + 1];
	bool has_job;
	/**
	 * Seconds left on whichever deadline applies — the candidate's time to
	 * live while STAGED, the confirmation deadline while
	 * AWAITING_CONFIRMATION. Negative when no deadline is running, which
	 * the web layer renders as null.
	 */
	int32_t remaining_seconds;
	/** Why it failed. Meaningful only when @ref has_error. */
	enum api_error_code error_code;
	bool has_error;
};

/** Runtime state of one interface, as the device can observe it. */
struct network_iface_status {
	/** False when the hardware is absent — no C6 fitted, no PHY. */
	bool present;
	bool enabled;
	bool link_up;
	bool has_ipv4;
	uint8_t ipv4[4];
	uint8_t prefix_length;
	bool has_gateway;
	uint8_t gateway[4];
	/** A default route exists through this interface. */
	bool has_route;
	/** Wi-Fi only: associated with an access point. */
	bool wifi_associated;
	uint8_t ssid[DEVICE_CONFIG_SSID_MAX_LEN];
	uint8_t ssid_len;
	int8_t rssi;
	bool rssi_valid;
};

/** Runtime state of the whole stack. No secrets, per the contract. */
struct network_status {
	struct network_iface_status ethernet;
	struct network_iface_status wifi;
	/** Which interface currently carries the default route. */
	enum device_config_interface active;
	bool has_active;
	/** Resolvers in force, however they were obtained. */
	struct device_config_addr dns[DEVICE_CONFIG_DNS_MAX_SERVERS];
	uint8_t dns_count;
};

/** One access point from a scan. */
struct network_access_point {
	uint8_t bssid[6];
	/** Raw bytes; an SSID need not be printable or valid UTF-8. */
	uint8_t ssid[DEVICE_CONFIG_SSID_MAX_LEN];
	uint8_t ssid_len;
	enum device_config_wifi_security security;
	/**
	 * False for an access point this build cannot join — enterprise, for
	 * instance. The contract says such networks stay visible but are not
	 * selectable, so they are reported rather than filtered out.
	 */
	bool connect_supported;
	int8_t rssi;
	uint8_t channel;
};

/** A completed scan. */
struct network_scan_results {
	struct network_access_point items[CONFIG_NETWORK_MANAGER_SCAN_MAX_RESULTS];
	uint8_t count;
	/** More access points were seen than fit. */
	bool truncated;
};

/**
 * @brief The interface adapter, injected.
 *
 * Everything that touches hardware lives behind this. The sim tier supplies a
 * fake; on the board, src/services supplies one over net_if, the W5500 and the
 * ESP-Hosted Wi-Fi driver.
 *
 * Two rules bind any implementation:
 *
 *  - It makes no policy decisions. In particular it must not start DHCP by
 *    itself — section 5 of the plan is explicit that one service owns DHCP,
 *    DNS and Wi-Fi credentials, and an adapter that starts DHCP whenever it
 *    associates makes static addressing impossible to express.
 *  - It must not call back into network-manager. These are invoked with the
 *    module's mutex held.
 *
 * Every call returns 0 or a negative errno.
 */
struct network_iface_ops {
	/**
	 * Bring an interface to the given IPv4 settings, or take it down when
	 * @p enabled is false. For DHCP the adapter starts the client; for
	 * static it assigns the address and, if present, the gateway route.
	 */
	int (*configure)(void *ctx, enum device_config_interface iface,
			 const struct device_config_ipv4 *ipv4, bool enabled);
	/**
	 * Join a Wi-Fi network. @p password is NULL for an open network. The
	 * bytes are not retained by the caller after this returns.
	 */
	int (*wifi_connect)(void *ctx, const uint8_t *ssid, size_t ssid_len,
			    enum device_config_wifi_security security, bool hidden,
			    const uint8_t *password, size_t password_len);
	/** Leave the current network. Succeeds when not associated. */
	int (*wifi_disconnect)(void *ctx);
	/** Read one interface's runtime state. */
	int (*get_status)(void *ctx, enum device_config_interface iface,
			  struct network_iface_status *out);
	/** Install resolvers. @p count may be zero, meaning "clear manual DNS". */
	int (*set_dns)(void *ctx, const struct device_config_addr *servers, size_t count);
	/**
	 * Scan for access points, synchronously, filling @p out. Called from
	 * the worker, never from an HTTP handler. Optional: an adapter with no
	 * radio leaves it NULL and scans are refused as unavailable.
	 */
	int (*wifi_scan)(void *ctx, struct network_scan_results *out);
	void *ctx;
};

/**
 * @brief Replace the clock used for deadlines.
 *
 * Intended for tests. Pass NULL to restore k_uptime_get(). Deadlines already
 * running keep the instants they were given.
 */
void network_manager_set_clock(int64_t (*clock_fn)(void));

/**
 * @brief Bind the adapter and drop any transaction in progress.
 *
 * device_config_init() and job_manager_init() must already have run: this
 * module is a policy layer over both. Safe to call again, which is how tests
 * restart it.
 *
 * @retval 0        ready
 * @retval -EINVAL  @p ops is missing a required callback
 */
int network_manager_init(const struct network_iface_ops *ops);

/**
 * @brief Runtime state of both interfaces and the resolvers.
 *
 * Reads through the adapter every time rather than caching: a link that went
 * down thirty seconds ago must not be reported as up.
 */
int network_get_status(struct network_status *out);

/**
 * @brief Validate a proposal and hold it as the candidate.
 *
 * Every rule the contract states is checked here, while the device is still
 * reachable and nothing has been changed. Failures come back as field-level
 * errors against JSON Pointers into the request body, so a form can highlight
 * what is wrong rather than showing one sentence.
 *
 * What is checked, beyond the shapes the JSON schema already fixes:
 *
 *  - Static IPv4 needs an address and a prefix of 1-30, and the address must
 *    be one a host can actually hold on that prefix — not the subnet address,
 *    not its broadcast, not loopback or link-local.
 *  - A gateway, if given, must sit on the same subnet and not be the address
 *    itself. DHCP mode must carry none of those fields.
 *  - Manual DNS means one or two resolvers of either family; automatic means
 *    none.
 *  - Both interfaces disabled is refused. So is naming a disabled interface as
 *    preferred.
 *  - Enabled Wi-Fi needs an SSID, and a protected network needs a password —
 *    either supplied now or already stored. Changing SSID or security while
 *    keeping the old password is refused, because the stored password belongs
 *    to the old network.
 *  - At least one enabled interface must have a link right now. This is the
 *    recovery path section 5 requires: without it the device would be applying
 *    a change it has no way to be told to undo.
 *
 * @param input          The proposal. Copied; not retained.
 * @param base_revision  Committed revision the caller believes is in force.
 * @param err            Receives the rejection. Must not be NULL.
 * @param out            Receives the staged transaction. May be NULL.
 * @retval 0        staged
 * @retval -EINVAL  rejected; @p err carries the code and the fields
 */
int network_stage_config(const struct network_config_input *input, uint32_t base_revision,
			 struct api_error *err, struct network_transaction *out);

/** @brief Copy out a transaction by id. @retval -ENOENT unknown or forgotten. */
int network_transaction_get(const char *txn_id, struct network_transaction *out);

/** @brief Copy out the current transaction, if any. @retval -ENOENT none. */
int network_transaction_current(struct network_transaction *out);

/**
 * @brief Journal the candidate durably and accept the change.
 *
 * Steps 2 and 3 of section 5. Creates the apply job, writes the pending
 * generation through device-config-store and verifies it, then returns so the
 * caller can send its 202. **The network is not touched here.** If the journal
 * cannot be written, nothing has changed and the transaction fails — a change
 * that cannot be recorded is one that cannot be undone.
 *
 * @param confirmation_timeout_seconds  How long the client has to confirm once
 *        the change is live. Bounded by CONFIG_NETWORK_MANAGER_CONFIRM_*; zero
 *        means the configured default.
 * @retval 0        journalled; call network_apply_execute() next
 * @retval -EINVAL  rejected; @p err says why
 */
int network_apply(const char *txn_id, uint16_t confirmation_timeout_seconds,
		  struct api_error *err);

/**
 * @brief Change the interfaces. Run this from the worker, after the response.
 *
 * Step 3's second half. On success the transaction is awaiting confirmation
 * and the deadline is running. On failure the interfaces are put back to the
 * committed configuration and the journal is discarded, so a candidate that
 * could not be applied leaves nothing behind.
 *
 * @retval 0        applied; awaiting confirmation
 * @retval -EIO     the adapter failed; the transaction is FAILED and the
 *                  committed configuration has been restored
 * @retval -EINVAL  not in APPLYING
 */
int network_apply_execute(const char *txn_id);

/**
 * @brief Keep the change, if the device can see that it works.
 *
 * Step 5. Every enabled interface must have a link and an address, and a route
 * if the configuration asked for a gateway. A client reaching this endpoint
 * proves only its own path works — the contract is explicit that a browser on
 * an unchanged Ethernet says nothing about whether Wi-Fi actually joined — so
 * the check covers both.
 *
 * @retval 0        committed; the revision has advanced
 * @retval -EAGAIN  not healthy yet; the transaction stays awaiting
 * @retval -EINVAL  wrong state or unknown id
 */
int network_confirm(const char *txn_id, struct api_error *err);

/**
 * @brief Undo, whatever stage the transaction has reached.
 *
 * Discards a candidate that was only staged; restores the interfaces and drops
 * the journal for one that was applied. This is the endpoint behind
 * `DELETE /network/transactions/{id}`, which the contract makes the only way
 * to cancel network work — `jobs/cancel` does not apply to it.
 */
int network_rollback(const char *txn_id, struct api_error *err);

/**
 * @brief Advance the deadlines.
 *
 * Call periodically from the worker, at least once a second. Expires a
 * candidate nobody applied, and rolls back a change nobody confirmed — the
 * latter being the whole reason this module can be trusted with the only
 * interface the device has.
 *
 * @return the number of transactions whose state changed.
 */
int network_manager_tick(void);

/**
 * @brief Start a Wi-Fi scan.
 *
 * @param err     Receives the rejection.
 * @param job_id  Receives the job id to poll.
 * @retval 0        accepted; call network_scan_execute() from the worker
 * @retval -EBUSY   a network change is in progress; scanning during an apply
 *                  is forbidden by the contract
 * @retval -ENOTSUP no radio
 * @retval -EINVAL  rejected; @p err says why
 */
int network_scan_begin(const char *idempotency_key, uint32_t request_hash, struct api_error *err,
		       char job_id[JOB_ID_MAX_LEN + 1]);

/** @brief Perform the scan. Run from the worker; blocks for its duration. */
int network_scan_execute(const char *job_id);

/** @brief Read a finished scan. @retval -EAGAIN still running. */
int network_scan_results_get(const char *job_id, struct network_scan_results *out);

/** @brief Stable lowercase wire name, e.g. "awaiting_confirmation". */
const char *network_transaction_state_str(enum network_transaction_state state);

/** @brief True when the transaction can no longer change by itself. */
bool network_transaction_state_is_terminal(enum network_transaction_state state);

#ifdef __cplusplus
}
#endif

#endif /* NETWORK_MANAGER_H_ */
