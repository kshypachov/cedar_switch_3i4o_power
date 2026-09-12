/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Device configuration store: the durable, versioned, transactional home of
 * everything the device must remember across a reboot — network configuration
 * and the secrets that go with it.
 *
 * The contract this implements is section 5 of
 * docs/device-development/development-plan.md and the "Сеть" section of
 * docs/device-development/api-contract.md. Four requirements from there shape
 * the whole design:
 *
 *  - A configuration change is a transaction, not a field write. Candidate,
 *    apply, confirm and commit are distinct steps, and the device must survive
 *    losing power at any one of them with a configuration that is complete and
 *    self-consistent.
 *  - A reboot before the durable commit always restores the last committed
 *    configuration. A reboot after it keeps the new one. There is no third
 *    outcome, and in particular there is no state in which the device comes up
 *    with half of each.
 *  - Secrets never appear in anything the API serialises. The contract says
 *    the API returns only `password_set`, and that is enforced here
 *    structurally rather than by remembering to redact.
 *  - Omitting a credential in a mutation must not silently clear it.
 *
 * Design notes worth knowing before using this:
 *
 *  - Secrets are not members of struct device_config. They live in a separate
 *    region of the record, reachable only through device_config_secret_get().
 *    A snapshot handed to an HTTP handler therefore cannot leak one, however
 *    the handler is written — the bytes are not in the struct it holds.
 *  - The committed generation is stored in two slots written alternately, so a
 *    commit never overwrites the only good copy. The newest record that passes
 *    its CRC wins. The pending generation has a slot of its own, which is what
 *    makes a rollback after an unexpected reboot possible.
 *  - No dynamic allocation and no threads. Both generations are cached in RAM
 *    and storage is their durable mirror, so a GET never touches flash.
 *  - The storage backend is a struct of function pointers. That is the "pure
 *    core, thin adapter" split section 12 of the plan requires: power loss and
 *    write failures are tested by injecting them, not by staging them on a
 *    board.
 *  - The struct layout is itself the schema. A BUILD_ASSERT pins the size of
 *    every persisted struct, so changing one without bumping
 *    DEVICE_CONFIG_SCHEMA_VERSION fails the build rather than misreading old
 *    records at the customer's site.
 *
 * Threading: every entry point takes an internal mutex, and snapshots are
 * copied out under it. Not ISR-safe.
 *
 * What this module does not do: it does not validate business rules (a static
 * address inside its own subnet, at least one usable interface, whether a
 * recovery path exists) — that is network-manager's job, and it must happen
 * before a candidate is ever handed here. It does not open sockets, touch
 * net_if, or know that Wi-Fi exists beyond storing its name and password.
 */

#ifndef DEVICE_CONFIG_STORE_H_
#define DEVICE_CONFIG_STORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Version of the persisted layout.
 *
 * Bump this whenever any persisted struct below changes in any way — a field
 * added, removed, reordered, resized or given a new meaning. The build asserts
 * on the size of each one, so a change that forgets the bump does not compile.
 * Records written by an older version are migrated on load; records written by
 * a newer one are refused rather than reinterpreted, because a downgrade that
 * guesses at an unknown layout is how a device loses its network settings.
 */
#define DEVICE_CONFIG_SCHEMA_VERSION 1u

/** Longest SSID the 802.11 standard allows, in raw bytes. Not a C string. */
#define DEVICE_CONFIG_SSID_MAX_LEN 32
/** Longest secret the store will hold, in bytes. */
#define DEVICE_CONFIG_SECRET_MAX_LEN 96
/** Transaction id length, matching `^[A-Za-z0-9_-]{1,64}$` in openapi.json. */
#define DEVICE_CONFIG_TXN_ID_MAX_LEN 64
/** Manual DNS servers, capped at two by section 5 of the plan. */
#define DEVICE_CONFIG_DNS_MAX_SERVERS 2

/** Which interface is preferred when both are usable. */
enum device_config_interface {
	DEVICE_CONFIG_INTERFACE_ETHERNET = 0,
	DEVICE_CONFIG_INTERFACE_WIFI,

	DEVICE_CONFIG_INTERFACE_COUNT
};

/** IPv4 addressing mode. Mirrors the `mode` enum of IPv4Config. */
enum device_config_ipv4_mode {
	DEVICE_CONFIG_IPV4_DHCP = 0,
	DEVICE_CONFIG_IPV4_STATIC,

	DEVICE_CONFIG_IPV4_MODE_COUNT
};

/** DNS server selection. Mirrors the `mode` enum of DNSConfig. */
enum device_config_dns_mode {
	DEVICE_CONFIG_DNS_AUTOMATIC = 0,
	DEVICE_CONFIG_DNS_MANUAL,

	DEVICE_CONFIG_DNS_MODE_COUNT
};

/** Wi-Fi security. Mirrors the `security` enum of WiFiConfigInput. */
enum device_config_wifi_security {
	DEVICE_CONFIG_WIFI_OPEN = 0,
	DEVICE_CONFIG_WIFI_WPA2_PSK,
	DEVICE_CONFIG_WIFI_WPA3_SAE,

	DEVICE_CONFIG_WIFI_SECURITY_COUNT
};

/** Address family of a stored IP address. */
enum device_config_af {
	DEVICE_CONFIG_AF_NONE = 0,
	DEVICE_CONFIG_AF_INET = 4,
	DEVICE_CONFIG_AF_INET6 = 6,
};

/**
 * An IP address of either family.
 *
 * DNS servers may be IPv4 or IPv6 per the contract, so they cannot be a bare
 * uint32_t. Bytes are in network order, the same order a text address is read
 * in, so nothing here depends on the host's endianness.
 */
struct device_config_addr {
	uint8_t family; /**< @ref device_config_af */
	uint8_t reserved[3];
	uint8_t bytes[16];
};

/** IPv4 settings of one interface. */
struct device_config_ipv4 {
	uint8_t mode; /**< @ref device_config_ipv4_mode */
	/** 1-30 when mode is static, 0 otherwise. */
	uint8_t prefix_length;
	/** True when @ref gateway carries an address; an isolated LAN has none. */
	uint8_t has_gateway;
	uint8_t reserved;
	/** Network order. Meaningful only when mode is static. */
	uint8_t address[4];
	uint8_t gateway[4];
};

/** Resolver settings, shared by both interfaces. */
struct device_config_dns {
	uint8_t mode; /**< @ref device_config_dns_mode */
	/** 0-2; always 0 when mode is automatic. */
	uint8_t server_count;
	uint8_t reserved[2];
	struct device_config_addr servers[DEVICE_CONFIG_DNS_MAX_SERVERS];
};

/** Wired interface. */
struct device_config_ethernet {
	uint8_t enabled;
	uint8_t reserved[3];
	struct device_config_ipv4 ipv4;
};

/**
 * Wireless interface.
 *
 * Note what is absent: the password. @ref password_set says whether one is
 * stored, which is exactly what WiFiConfigOutput exposes, and the bytes
 * themselves are reachable only through device_config_secret_get().
 */
struct device_config_wifi {
	uint8_t enabled;
	uint8_t security; /**< @ref device_config_wifi_security */
	uint8_t hidden;
	/** 0-32. An SSID is raw bytes and need not be valid UTF-8 or printable. */
	uint8_t ssid_len;
	uint8_t ssid[DEVICE_CONFIG_SSID_MAX_LEN];
	/**
	 * Derived by the store from the secret that survived the update, never
	 * taken from the caller. A caller cannot claim a password exists when
	 * none was stored, nor lose one by forgetting to set this.
	 */
	uint8_t password_set;
	uint8_t reserved[3];
	struct device_config_ipv4 ipv4;
};

/**
 * The whole persisted configuration of one generation, secrets excluded.
 *
 * Safe to copy, log and serialise: there is nothing sensitive in it. That is
 * the point of the type.
 */
struct device_config {
	uint8_t preferred_interface; /**< @ref device_config_interface */
	/** Derived, like device_config_wifi.password_set. */
	uint8_t admin_password_set;
	uint8_t reserved[2];
	struct device_config_dns dns;
	struct device_config_ethernet ethernet;
	struct device_config_wifi wifi;
};

/** Which secret, for the secret accessors. */
enum device_config_secret_id {
	/** Wi-Fi PSK or SAE password, 8-63 characters, stored as given. */
	DEVICE_CONFIG_SECRET_WIFI_PASSWORD = 0,
	/**
	 * Admin credential verifier. Opaque bytes: the KDF, its parameters and
	 * the salt are web-auth's business, and the store deliberately cannot
	 * tell a hash from a password.
	 */
	DEVICE_CONFIG_SECRET_ADMIN_PASSWORD,

	DEVICE_CONFIG_SECRET_COUNT
};

/**
 * What an update does to one secret. Mirrors CredentialChange.action, and
 * KEEP is zero so that a zero-initialised update changes no secret at all —
 * the contract's rule that an absent field must not clear one.
 */
enum device_config_secret_action {
	DEVICE_CONFIG_SECRET_KEEP = 0,
	DEVICE_CONFIG_SECRET_REPLACE,
	DEVICE_CONFIG_SECRET_CLEAR,
};

/** One secret's part of an update. */
struct device_config_secret_update {
	enum device_config_secret_action action;
	/** Required for REPLACE, ignored otherwise. Copied, not retained. */
	const uint8_t *value;
	size_t len;
};

/** A complete proposed generation: configuration plus what to do with secrets. */
struct device_config_update {
	struct device_config config;
	struct device_config_secret_update secrets[DEVICE_CONFIG_SECRET_COUNT];
};

/** Which generation an accessor refers to. */
enum device_config_generation {
	/** The configuration in force, and the one a reboot returns to. */
	DEVICE_CONFIG_COMMITTED = 0,
	/** Journalled, applied to the hardware, not yet confirmed. */
	DEVICE_CONFIG_PENDING,
};

/**
 * @brief What init() found on storage and what it did about it.
 *
 * The store resolves an interrupted transaction by itself, because correctness
 * cannot wait for a caller. This is the report, so the caller can tell the
 * client what became of a transaction it may still be polling.
 */
enum device_config_recovery {
	/** Committed configuration loaded, nothing was in flight. */
	DEVICE_CONFIG_RECOVERY_CLEAN = 0,
	/** Storage held nothing usable; factory defaults are in force at revision 0. */
	DEVICE_CONFIG_RECOVERY_DEFAULTS,
	/**
	 * A pending generation was found that had never been committed. It was
	 * discarded and the committed configuration stands. This is the reboot
	 * branch of the confirmation timeout, and the transaction named in
	 * @ref device_config_recovery_report.transaction_id is rolled_back.
	 */
	DEVICE_CONFIG_RECOVERY_ROLLED_BACK,
	/**
	 * Power was lost between writing the new committed record and erasing
	 * the pending one. The commit had already succeeded, so the new
	 * configuration stands and only the leftover was cleaned up.
	 */
	DEVICE_CONFIG_RECOVERY_COMMIT_COMPLETED,
	/**
	 * A pending record existed but could not be read — a torn write, most
	 * likely. Discarded; the committed configuration stands. The
	 * transaction id is not recoverable in this case.
	 */
	DEVICE_CONFIG_RECOVERY_PENDING_CORRUPT,
	/**
	 * Storage held committed records but none could be used — a newer
	 * schema this build cannot read, or both slots damaged. Factory
	 * defaults are in force at revision 0, which is not the same event as
	 * @ref DEVICE_CONFIG_RECOVERY_DEFAULTS on a device that never had a
	 * configuration, and wants reporting rather than silence.
	 */
	DEVICE_CONFIG_RECOVERY_COMMITTED_LOST,
	/**
	 * A committed record was unreadable and an older one was used instead.
	 * The configuration is valid but not the newest that was written; worth
	 * reporting because it means storage is failing.
	 */
	DEVICE_CONFIG_RECOVERY_COMMITTED_DEGRADED,
};

/** Detail accompanying @ref device_config_recovery. */
struct device_config_recovery_report {
	enum device_config_recovery result;
	/** Revision now in force. */
	uint32_t revision;
	/** The discarded transaction, empty when there was none or it is unknown. */
	char transaction_id[DEVICE_CONFIG_TXN_ID_MAX_LEN + 1];
};

/** Storage slots. The backend maps these onto files, NVS entries or sectors. */
enum device_config_slot {
	DEVICE_CONFIG_SLOT_COMMITTED_A = 0,
	DEVICE_CONFIG_SLOT_COMMITTED_B,
	DEVICE_CONFIG_SLOT_PENDING,

	DEVICE_CONFIG_SLOT_COUNT
};

/**
 * @brief Durable storage, injected.
 *
 * Three whole-slot operations, deliberately not byte-addressed: the store
 * needs no partial updates, and a narrow interface is one a fake can implement
 * exactly. None of them has to be atomic — a write may tear and leave the slot
 * unreadable, which is what the two committed slots and the CRC exist for.
 *
 * Every call must be synchronous and durable on return. A backend that buffers
 * must flush before returning, or the whole transaction model is a fiction.
 */
struct device_config_backend {
	/**
	 * Read a slot.
	 *
	 * @return bytes read, -ENOENT when the slot is empty, or another
	 *         negative errno. Reading more than @p cap bytes is an error;
	 *         returning fewer is normal.
	 */
	int (*read)(void *ctx, enum device_config_slot slot, void *buf, size_t cap);
	/** Replace a slot's contents. @return 0 or a negative errno. */
	int (*write)(void *ctx, enum device_config_slot slot, const void *buf, size_t len);
	/** Make a slot empty. @return 0 or a negative errno. */
	int (*erase)(void *ctx, enum device_config_slot slot);
	void *ctx;
};

/**
 * @brief Load the store from a backend and resolve anything left in flight.
 *
 * Must be called before anything else. Safe to call again, which is how tests
 * simulate a reboot: the RAM state is rebuilt from what the backend holds.
 *
 * @param backend  Storage. Retained by the store; must outlive it.
 * @param report   Receives what was found and done. May be NULL.
 * @retval 0        loaded, including the defaults and degraded cases
 * @retval -EINVAL  backend missing a callback
 * @retval -EIO     storage failed in a way that left nothing usable
 */
int device_config_init(const struct device_config_backend *backend,
		       struct device_config_recovery_report *report);

/** @brief Revision of the committed generation. Zero means factory defaults. */
uint32_t device_config_revision(void);

/**
 * @brief Copy out a generation's configuration. Never includes a secret.
 *
 * @retval 0        copied
 * @retval -ENOENT  @p generation is PENDING and none exists
 * @retval -EINVAL  bad arguments
 */
int device_config_get(enum device_config_generation generation, struct device_config *out);

/**
 * @brief Copy out a secret's bytes.
 *
 * The one way to reach secret material, kept separate from the configuration
 * so that a caller has to ask for it on purpose. Callers wipe the buffer when
 * done; the store wipes its own copies.
 *
 * During an apply the Wi-Fi adapter needs the PENDING password, since that is
 * the network being joined, while everything else wants COMMITTED.
 *
 * @param out_len  Receives the length. Set to 0 when no secret is stored.
 * @retval 0        copied, or none stored and @p out_len is 0
 * @retval -ENOENT  @p generation is PENDING and none exists
 * @retval -ENOMEM  @p cap is too small for the stored secret
 * @retval -EINVAL  bad arguments
 */
int device_config_secret_get(enum device_config_generation generation,
			     enum device_config_secret_id id, uint8_t *buf, size_t cap,
			     size_t *out_len);

/** Identity of a journalled transaction. */
struct device_config_pending_info {
	bool present;
	char transaction_id[DEVICE_CONFIG_TXN_ID_MAX_LEN + 1];
	/** Revision this transaction will produce if it commits. */
	uint32_t revision;
	/** Revision it was staged against, i.e. the committed one. */
	uint32_t base_revision;
};

/** @brief Report the journalled transaction, if any. @p out must not be NULL. */
int device_config_pending_info_get(struct device_config_pending_info *out);

/**
 * @brief Journal a new generation durably, without putting it in force.
 *
 * This is step 2 of the apply sequence in section 5 of the plan: the pending
 * snapshot is written and read back before the network is touched, so that a
 * failure to record the intent is discovered while the device is still
 * reachable. On return the caller may change the hardware; on any error it
 * must not, and nothing has been modified.
 *
 * Secrets marked KEEP are carried forward from the committed generation here.
 * That is the whole reason this takes an update rather than a configuration:
 * the new generation must be complete, and "unchanged password" has to be
 * resolved into actual bytes at this point rather than left as a reference to
 * a generation that is about to be replaced.
 *
 * `password_set` and `admin_password_set` in @p update->config are ignored and
 * recomputed from the secrets that result.
 *
 * @param update          Proposed generation. Copied; not retained.
 * @param base_revision   Revision the caller believes is committed.
 * @param transaction_id  Caller's id, matching `^[A-Za-z0-9_-]{1,64}$`.
 * @retval 0          journalled and verified
 * @retval -ESTALE    @p base_revision is not the committed revision; answer 409
 * @retval -EBUSY     a transaction is already journalled; answer 409
 * @retval -EINVAL    bad arguments, or a REPLACE with no value
 * @retval -EOVERFLOW the revision counter is exhausted
 * @retval -EIO       storage failed; nothing was changed
 */
int device_config_pending_begin(const struct device_config_update *update, uint32_t base_revision,
				const char *transaction_id);

/**
 * @brief Put the journalled generation in force, durably.
 *
 * Step 6 of the apply sequence. The caller must already have satisfied the
 * health conditions; the store does not and cannot judge whether the network
 * works.
 *
 * Ordering is what makes this survive power loss: the new record reaches a
 * committed slot and is read back before the pending slot is erased. Losing
 * power in between leaves a committed record that already wins and a stale
 * journal that init() recognises and cleans up.
 *
 * @param transaction_id  Must match the journalled one, so a confirmation for
 *                        a transaction that has already been replaced cannot
 *                        commit the wrong generation.
 * @retval 0        committed; device_config_revision() has advanced
 * @retval -ENOENT  nothing is journalled
 * @retval -EINVAL  id does not match the journalled transaction
 * @retval -EIO     storage failed; the committed generation is unchanged
 */
int device_config_pending_commit(const char *transaction_id);

/**
 * @brief Discard the journalled generation.
 *
 * The explicit form of what init() does after an unexpected reboot, used for
 * the confirmation timeout and for an operator cancelling. The committed
 * generation is untouched; restoring the hardware to it is the caller's job.
 *
 * @retval 0        discarded
 * @retval -ENOENT  nothing is journalled
 * @retval -EINVAL  id does not match
 * @retval -EIO     storage failed; the journal may still be present
 */
int device_config_pending_rollback(const char *transaction_id);

/** @brief Fill @p out with the factory defaults: Ethernet DHCP, no secrets. */
void device_config_defaults(struct device_config *out);

/** @brief Stable lowercase wire name, e.g. "wpa3_sae". NULL if out of range. */
const char *device_config_wifi_security_str(enum device_config_wifi_security security);
/** @brief Stable lowercase wire name: "dhcp" or "static". */
const char *device_config_ipv4_mode_str(enum device_config_ipv4_mode mode);
/** @brief Stable lowercase wire name: "automatic" or "manual". */
const char *device_config_dns_mode_str(enum device_config_dns_mode mode);
/** @brief Stable lowercase wire name: "ethernet" or "wifi". */
const char *device_config_interface_str(enum device_config_interface iface);

#ifdef __cplusplus
}
#endif

#endif /* DEVICE_CONFIG_STORE_H_ */
