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
 *  - No threads of its own. Every request is split in two: a call that decides
 *    and records (stage, apply, confirm, rollback, scan), which never touches an
 *    interface and is what an HTTP handler calls, and network_manager_process(),
 *    which a worker runs afterwards and which does all the interface I/O. The
 *    contract requires the journal to be durable and the response sent before
 *    the network changes, and the HTTP server's one thread is cooperative, so a
 *    Wi-Fi association or a scan run from a handler would stop every other
 *    request for seconds. (P4 moved confirm, rollback and scan to this split;
 *    apply had it from P1.)
 *  - Interfaces are reached through an injected ops table and time through an
 *    injected clock, so the whole transaction — including a confirmation that
 *    never arrives — is exercised in the sim tier without a board or a
 *    stopwatch.
 *  - The admin password is not reachable from here. The input type carries
 *    only the Wi-Fi credential, so a network transaction structurally cannot
 *    touch the credential that guards the API.
 *  - Candidates live in RAM and expire. Exactly one exists at a time; a second
 *    is refused rather than silently replacing a transaction another
 *    administrator is waiting on.
 *  - The rules and their field codes are the mock server's
 *    (tools/api-contract/cedar_contract/mock/network.py), field for field and
 *    in the same order, because the frontend is built against the mock and a
 *    different code — or a different six fields surviving truncation — is a
 *    different screen. Where the device knows something the mock cannot, the
 *    rule is marked device-only below.
 *
 * Threading: every entry point takes an internal mutex and snapshots are
 * copied out under it. Not ISR-safe. See struct network_iface_ops for which
 * adapter calls happen with the mutex held.
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

/** Addresses one interface reports, both families together. */
#define NETWORK_IFACE_MAX_ADDRS 6

/**
 * @brief Transaction lifecycle.
 *
 * Mirrors the `state` enum of NetworkTransaction in openapi.json exactly.
 * Legal transitions:
 *
 *   STAGED                -> APPLYING | ROLLED_BACK | EXPIRED | FAILED
 *   APPLYING              -> AWAITING_CONFIRMATION | ROLLING_BACK
 *   AWAITING_CONFIRMATION -> COMMITTED | ROLLING_BACK
 *   ROLLING_BACK          -> ROLLED_BACK | FAILED
 *   COMMITTED, ROLLED_BACK, FAILED, EXPIRED -> nothing
 *
 * ROLLED_BACK is reached from STAGED as well, because discarding a candidate
 * that was never applied and undoing one that was are the same thing to a
 * client polling the transaction: the change did not happen. FAILED is reached
 * from STAGED when the journal cannot be written, and from ROLLING_BACK when
 * the interfaces refused the candidate or the commit could not be written: in
 * both of the latter the committed configuration has been put back first.
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
 *
 * The flags below carry what struct device_config cannot: it is the persisted
 * schema, so it has no room for "the request said null" or "the request was not
 * base64". A caller that builds a configuration in code leaves them false.
 */
struct network_config_input {
	/** Proposed configuration. The derived `*_set` flags are ignored. */
	struct device_config config;
	/** keep / replace / clear for the Wi-Fi password only. */
	struct device_config_secret_update wifi_password;
	/**
	 * The request gave an address for this interface, even 0.0.0.0. An
	 * all-zero address with this false reads as null.
	 */
	bool ethernet_address_given;
	bool wifi_address_given;
	/** `ssid_base64` did not decode; `ssid_len` is meaningless. */
	bool ssid_invalid;
};

/** A transaction as a caller sees it. Contains no secret material. */
struct network_transaction {
	char id[NETWORK_TXN_ID_MAX_LEN + 1];
	enum network_transaction_state state;
	/** Committed revision this was staged against. */
	uint32_t base_revision;
	/** The proposal. `password_set` is truthful; the password is not here. */
	struct device_config candidate;
	/** The job, empty while the transaction is merely staged. */
	char job_id[JOB_ID_MAX_LEN + 1];
	bool has_job;
	/**
	 * Whole seconds left on whichever deadline applies, rounded down — the
	 * candidate's time to live while STAGED, the confirmation deadline while
	 * APPLYING or AWAITING_CONFIRMATION. Negative when no deadline is
	 * running, which the web layer renders as null.
	 */
	int32_t remaining_seconds;
	/** Why it failed, expired or was rolled back. Meaningful when @ref has_error. */
	enum api_error_code error_code;
	bool has_error;
};

/** Where an address came from. Mirrors the `source` enum of Address. */
enum network_addr_source {
	NETWORK_ADDR_DHCP = 0,
	NETWORK_ADDR_STATIC,
	NETWORK_ADDR_SLAAC,
	NETWORK_ADDR_LINK_LOCAL,

	NETWORK_ADDR_SOURCE_COUNT
};

/** One address an interface holds. */
struct network_addr {
	uint8_t family; /**< DEVICE_CONFIG_AF_INET or DEVICE_CONFIG_AF_INET6 */
	uint8_t prefix_length;
	uint8_t source; /**< @ref network_addr_source */
	uint8_t reserved;
	/** Network order; the first four bytes for IPv4. */
	uint8_t bytes[16];
};

/** What an interface is doing. Mirrors the `state` enum of InterfaceStatus. */
enum network_iface_state {
	NETWORK_IFACE_DISABLED = 0,
	NETWORK_IFACE_DOWN,
	NETWORK_IFACE_CONNECTING,
	NETWORK_IFACE_CONNECTED,
	NETWORK_IFACE_ADDRESSING,
	NETWORK_IFACE_READY,
	NETWORK_IFACE_FAILED,

	NETWORK_IFACE_STATE_COUNT
};

/**
 * @brief Runtime state of one interface, as the device can observe it.
 *
 * The adapter fills everything except @ref enabled and @ref state, which
 * network-manager derives from the configuration in force.
 */
struct network_iface_status {
	/**
	 * False when the hardware is absent or does not answer — no PHY, or a
	 * Wi-Fi coprocessor with no firmware that never completed its
	 * initialisation.
	 */
	bool present;
	/** Enabled in the configuration in force. Filled by network-manager. */
	bool enabled;
	bool link_up;
	bool has_ipv4;
	uint8_t ipv4[4];
	uint8_t prefix_length;
	bool has_gateway;
	uint8_t gateway[4];
	/** A default route exists through this interface. */
	bool has_route;
	/** Link-layer address; all zero when unknown. */
	uint8_t mac[6];
	/** Every address the stack holds on this interface, IPv4 first. */
	struct network_addr addrs[NETWORK_IFACE_MAX_ADDRS];
	uint8_t addr_count;
	/** Wi-Fi only: associated with an access point. */
	bool wifi_associated;
	/** Wi-Fi only: an association was requested and has no result yet. */
	bool wifi_connecting;
	/** Wi-Fi only: the last association attempt failed. */
	bool wifi_failed;
	uint8_t ssid[DEVICE_CONFIG_SSID_MAX_LEN];
	uint8_t ssid_len;
	int8_t rssi;
	bool rssi_valid;
	/** Derived by network-manager; see network_iface_state_str(). */
	enum network_iface_state state;
};

/** Runtime state of the whole stack. No secrets, per the contract. */
struct network_status {
	struct network_iface_status ethernet;
	struct network_iface_status wifi;
	/**
	 * Which interface carries traffic with no route of its own: the
	 * preferred one while it has a link and an address, else the other one
	 * if that does. This is the policy the service applies, and so the
	 * interface it has set as the default.
	 */
	enum device_config_interface active;
	bool has_active;
	/** Resolvers in force, however they were obtained. */
	struct device_config_addr dns[DEVICE_CONFIG_DNS_MAX_SERVERS];
	uint8_t dns_count;
};

/** Security of an access point seen in a scan. Mirrors AccessPoint.security. */
enum network_ap_security {
	NETWORK_AP_OPEN = 0,
	NETWORK_AP_WPA2_PSK,
	NETWORK_AP_WPA3_SAE,
	NETWORK_AP_WPA2_WPA3_TRANSITION,
	NETWORK_AP_ENTERPRISE,
	NETWORK_AP_UNKNOWN,

	NETWORK_AP_SECURITY_COUNT
};

/** One access point from a scan. */
struct network_access_point {
	uint8_t bssid[6];
	/** Raw bytes; an SSID need not be printable or valid UTF-8. */
	uint8_t ssid[DEVICE_CONFIG_SSID_MAX_LEN];
	uint8_t ssid_len;
	enum network_ap_security security;
	/**
	 * False for an access point this build cannot join — enterprise, or a
	 * security it does not recognise. The contract says such networks stay
	 * visible but are not selectable, so they are reported rather than
	 * filtered out. Set by network-manager from @ref security; an adapter's
	 * value is ignored.
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
 * Network operations another owner of the Wi-Fi coprocessor may have to
 * exclude. On the board the ESP32-C6's UART can be handed to a USB bridge or a
 * flasher, and plan section 3 makes those exclusive with an apply, and a
 * flasher with a scan too.
 */
enum network_exclusive {
	/** A transaction from its apply until it is committed, rolled back or failed. */
	NETWORK_EXCLUSIVE_APPLY = 0,
	/** A scan from its acceptance until its results are in. */
	NETWORK_EXCLUSIVE_SCAN,

	NETWORK_EXCLUSIVE_COUNT
};

/**
 * @brief The interface adapter, injected.
 *
 * Everything that touches hardware lives behind this. The sim tier supplies a
 * fake; on the board, src/services/network supplies one over net_if, the W5500
 * and the ESP-Hosted Wi-Fi driver.
 *
 * Three rules bind any implementation:
 *
 *  - It makes no policy decisions. In particular it must not start DHCP by
 *    itself — section 5 of the plan is explicit that one service owns DHCP,
 *    DNS and Wi-Fi credentials, and an adapter that starts DHCP whenever it
 *    associates makes static addressing impossible to express.
 *  - It must not call back into network-manager.
 *  - @ref get_status and @ref get_dns are called with the module's mutex held,
 *    from whichever thread asked — an HTTP handler, usually. They must answer
 *    from state the adapter keeps and must not block: no RPC to a coprocessor.
 *    Every other call is made only from network_manager_process(), without
 *    the mutex, and may block for as long as the hardware needs.
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
	 * bytes are not retained by the caller after this returns. Success means
	 * the request was accepted; association is reported through get_status().
	 */
	int (*wifi_connect)(void *ctx, const uint8_t *ssid, size_t ssid_len,
			    enum device_config_wifi_security security, bool hidden,
			    const uint8_t *password, size_t password_len);
	/** Leave the current network. Succeeds when not associated. */
	int (*wifi_disconnect)(void *ctx);
	/** Read one interface's runtime state. Non-blocking; mutex held. */
	int (*get_status)(void *ctx, enum device_config_interface iface,
			  struct network_iface_status *out);
	/**
	 * Install resolvers. @p count zero means "no manual resolvers": the ones
	 * DHCP and router advertisements provide are used.
	 */
	int (*set_dns)(void *ctx, const struct device_config_addr *servers, size_t count);
	/**
	 * Optional. The resolvers actually in force, whoever installed them.
	 * Non-blocking; mutex held. Without it, status reports the manual
	 * resolvers of the configuration and nothing for automatic DNS, and a
	 * manual list is installed once rather than kept in force.
	 */
	int (*get_dns)(void *ctx, struct device_config_addr *out, size_t cap, size_t *count);
	/**
	 * Optional. Route traffic that has no route of its own through @p iface.
	 * Called only when the choice changes.
	 */
	int (*set_default)(void *ctx, enum device_config_interface iface);
	/**
	 * Scan for access points, synchronously, filling @p out. Optional: an
	 * adapter with no radio leaves it NULL and scans are refused as
	 * unavailable.
	 */
	int (*wifi_scan)(void *ctx, struct network_scan_results *out);
	/**
	 * Optional, together with @ref exclusive_release. Claim the coprocessor
	 * for @p what before it starts: asked once an apply or a scan is
	 * otherwise acceptable, and before its job or journal exists. A negative
	 * return refuses the request with 409 busy and leaves nothing behind.
	 * Called with the module's mutex held, from the requesting thread: must
	 * not block and must not call back. On the board it maps to
	 * coprocessor-manager's claims.
	 */
	int (*exclusive_claim)(void *ctx, enum network_exclusive what);
	/**
	 * End a granted claim: an apply's when its transaction reaches committed,
	 * rolled_back or failed, a scan's when its results are in or it failed.
	 * Also for whatever is held when network_manager_init() starts over.
	 * Same calling rules as @ref exclusive_claim.
	 */
	void (*exclusive_release)(void *ctx, enum network_exclusive what);
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
 * @brief Start from what the store found at boot.
 *
 * Asks the next network_manager_process() to put the committed configuration
 * on the interfaces — the device's first network configuration of every boot
 * comes from here, not from whatever the drivers default to — and remembers a
 * transaction the store discarded, so a client still polling it is told the
 * change did not survive the reboot rather than that it never existed.
 *
 * @param report  From device_config_init(). May be NULL.
 */
void network_manager_boot(const struct device_config_recovery_report *report);

/**
 * @brief Whether @p txn_id is the transaction a reboot discarded.
 *
 * Its RAM record is gone with the boot, which is the contract's 410
 * boot_changed; any other unknown id is 404.
 */
bool network_manager_lost_in_reboot(const char *txn_id);

/**
 * @brief Put the factory network configuration in force, keeping what is not network.
 *
 * The owner's physical recovery (five power cycles in a row, plan section 13):
 * Ethernet on DHCP, automatic DNS, Wi-Fi disabled with its stored password
 * cleared. The SSID, security and hidden flag stay, so the form shows which
 * network it was. Matter fabrics and the administrator are untouched — neither
 * lives in this store's network configuration.
 *
 * Written as one committed generation, then pushed on the next
 * network_manager_process().
 *
 * @retval 0       committed
 * @retval -EBUSY  a transaction is in progress
 * @retval <0      the store refused; the previous configuration stands
 */
int network_manager_restore_defaults(void);

/**
 * @brief Do the interface work every other call has only recorded.
 *
 * Run from one worker thread: after any call below that returns 0, and at
 * least once a second, because the deadlines are checked here too. In order:
 * expire candidates and time out confirmations; push the configuration at
 * boot; push an applied candidate; commit a confirmed one; restore the
 * committed configuration for a rollback; run a requested scan; keep the
 * default route and a manual resolver list in force; and join an enabled Wi-Fi
 * network again once it has dropped, five seconds after the drop and then
 * twice as late each time up to a minute. Adapter calls are made without the
 * mutex, so requests are answered while it works.
 *
 * @return the number of steps it took; 0 when there was nothing to do.
 */
int network_manager_process(void);

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
 * errors against JSON Pointers into the request body (`/config/...`), in the
 * mock's order, so a form can highlight what is wrong rather than showing one
 * sentence.
 *
 * What is checked, beyond the shapes the JSON schema already fixes:
 *
 *  - Both interfaces disabled is refused. So is enabled Wi-Fi while the radio
 *    is not present (device-only: confirm waits for every enabled interface,
 *    so such a change could never be confirmed).
 *  - Static IPv4 needs an address and a prefix, and the address must be one a
 *    host can actually hold on that prefix — not the subnet address, not its
 *    broadcast, not 0/8, loopback, multicast or link-local. DHCP mode must
 *    carry none of those fields.
 *  - A gateway, if given, must be such a host on the same subnet and not the
 *    address itself; it is not judged against an address that is already
 *    wrong.
 *  - Manual DNS means one or two resolvers of either family and none of them
 *    unspecified; automatic means none.
 *  - Naming a disabled interface as preferred is refused.
 *  - Enabled Wi-Fi needs an SSID of 1-32 bytes, and a protected network needs
 *    a password — either supplied now or already stored. Changing SSID or
 *    security while keeping the old password is refused, because the stored
 *    password belongs to the old network. An open network takes none.
 *  - At least one enabled interface must have a link right now (device-only).
 *    This is the recovery path section 5 requires: without it the device
 *    would be applying a change it has no way to be told to undo.
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
 * caller can send its 202. **The network is not touched here**; the next
 * network_manager_process() does that. If the journal cannot be written,
 * nothing has changed and the transaction fails — a change that cannot be
 * recorded is one that cannot be undone.
 *
 * The confirmation deadline starts now, not when the interfaces have changed:
 * a worker that never runs, or stops half way, still ends in a rollback.
 *
 * @param confirmation_timeout_seconds  Bounded by
 *        CONFIG_NETWORK_MANAGER_CONFIRM_TIMEOUT_{MIN,MAX}_SECONDS; zero means
 *        the configured default.
 * @param idempotency_key  Given to the job; a retry with the same key and
 *        @p request_hash gets the same job back whatever state the transaction
 *        has reached since. May be NULL.
 * @param job_id  Receives the apply job's id.
 * @retval 0        journalled, or a replay of a request already accepted
 * @retval -EINVAL  rejected; @p err says why
 */
int network_apply(const char *txn_id, uint16_t confirmation_timeout_seconds,
		  const char *idempotency_key, uint32_t request_hash, struct api_error *err,
		  char job_id[JOB_ID_MAX_LEN + 1]);

/**
 * @brief Ask for the change to be kept, if the device can see that it works.
 *
 * Step 5. Every enabled interface must have a link and an address, and a route
 * if the configuration asked for a gateway. A client reaching this endpoint
 * proves only its own path works — the contract is explicit that a browser on
 * an unchanged Ethernet says nothing about whether Wi-Fi actually joined — so
 * the check covers both. The commit itself is written by the next
 * network_manager_process() and completes the apply job; asking again before
 * that answers with the same job.
 *
 * @param job_id  Receives the apply job's id.
 * @retval 0        accepted; the commit follows
 * @retval -EAGAIN  not healthy yet; the transaction stays awaiting (invalid_state)
 * @retval -EINVAL  wrong state or unknown id
 */
int network_confirm(const char *txn_id, struct api_error *err, char job_id[JOB_ID_MAX_LEN + 1]);

/**
 * @brief Undo, whatever stage the transaction has reached.
 *
 * Discards a candidate that was only staged, on a short `network_discard` job
 * of its own that is finished before this returns. For one that was applied —
 * applying or awaiting confirmation — the next network_manager_process()
 * restores the interfaces and drops the journal, on the apply job, which then
 * succeeds: a rollback somebody asked for is not a failure. This is the
 * endpoint behind `DELETE /network/transactions/{id}`, which the contract makes
 * the only way to cancel network work — `jobs/cancel` does not apply to it.
 *
 * Refused once a confirm has been accepted: the commit is already under way.
 *
 * @param idempotency_key  Given to a discard job; see network_apply().
 * @param job_id  Receives the discard job's or the apply job's id.
 */
int network_rollback(const char *txn_id, const char *idempotency_key, uint32_t request_hash,
		     struct api_error *err, char job_id[JOB_ID_MAX_LEN + 1]);

/**
 * @brief Start a Wi-Fi scan.
 *
 * @param err     Receives the rejection.
 * @param job_id  Receives the job id to poll.
 * @retval 0        accepted, or a replay; the next network_manager_process() scans
 * @retval -EBUSY   a scan is running, or a network change is being applied —
 *                  scanning during an apply is forbidden by the contract
 * @retval -ENOTSUP no radio, or the radio is not present
 * @retval -EINVAL  rejected; @p err says why
 */
int network_scan_begin(const char *idempotency_key, uint32_t request_hash, struct api_error *err,
		       char job_id[JOB_ID_MAX_LEN + 1]);

/**
 * @brief Read the results of the latest scan.
 *
 * Only the latest scan keeps its records: 64 of them are kilobytes of RAM.
 *
 * @retval 0        finished; @p out holds the results
 * @retval -EAGAIN  still queued or running
 * @retval -EIO     the scan failed; the job carries why
 * @retval -ENOENT  @p job_id is not the latest scan
 */
int network_scan_results_get(const char *job_id, struct network_scan_results *out);

/** @brief Stable lowercase wire name, e.g. "awaiting_confirmation". */
const char *network_transaction_state_str(enum network_transaction_state state);

/** @brief True when the transaction can no longer change by itself. */
bool network_transaction_state_is_terminal(enum network_transaction_state state);

/** @brief Wire name of an interface state, e.g. "addressing". NULL if out of range. */
const char *network_iface_state_str(enum network_iface_state state);

/** @brief Wire name of an access point's security, e.g. "wpa2_wpa3_transition". */
const char *network_ap_security_str(enum network_ap_security security);

/** @brief Whether this build can join an access point of @p security. */
bool network_ap_security_connectable(enum network_ap_security security);

/** @brief Wire name of an address source, e.g. "link_local". */
const char *network_addr_source_str(enum network_addr_source source);

#ifdef __cplusplus
}
#endif

#endif /* NETWORK_MANAGER_H_ */
