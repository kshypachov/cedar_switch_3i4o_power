/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Device configuration store implementation. See
 * include/device_config_store/device_config_store.h for the contract; this
 * file documents only how it is achieved.
 *
 * The record on storage is a fixed-size header followed by the configuration
 * and the secrets. Two checksums rather than one: the header's own covers the
 * header, so a torn write is recognised without trusting a length field read
 * out of the same damaged block, and the payload's covers the rest.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include <device_config_store/device_config_store.h>

LOG_MODULE_REGISTER(device_config_store, CONFIG_DEVICE_CONFIG_STORE_LOG_LEVEL);

/* "CDRC", little-endian. Cheap first rejection of an empty or foreign slot. */
#define RECORD_MAGIC 0x43524443u

/*
 * The persisted image. Field order and sizes are the schema: anything that
 * changes them changes DEVICE_CONFIG_SCHEMA_VERSION, and the BUILD_ASSERTs
 * below make forgetting that a compile error rather than a field misread on a
 * device already in service.
 */
struct record_header {
	uint32_t magic;
	uint16_t schema_version;
	uint16_t flags; /* reserved, written as zero */
	uint32_t revision;
	uint32_t base_revision;
	uint32_t payload_len;
	uint32_t payload_crc;
	char transaction_id[DEVICE_CONFIG_TXN_ID_MAX_LEN + 1];
	uint8_t reserved[3];
	/* Covers every byte of this struct before it. Must stay last. */
	uint32_t header_crc;
};

struct secret_blob {
	uint16_t len;
	uint8_t reserved[2];
	uint8_t data[DEVICE_CONFIG_SECRET_MAX_LEN];
};

struct record_payload {
	struct device_config config;
	struct secret_blob secrets[DEVICE_CONFIG_SECRET_COUNT];
};

struct record {
	struct record_header header;
	struct record_payload payload;
};

/*
 * Layout is the schema, so it is asserted rather than assumed. A field added
 * to any of these without bumping DEVICE_CONFIG_SCHEMA_VERSION stops the build
 * here, which is the only moment at which the mistake is still free.
 */
BUILD_ASSERT(sizeof(struct device_config_addr) == 20, "schema: device_config_addr");
BUILD_ASSERT(sizeof(struct device_config_ipv4) == 12, "schema: device_config_ipv4");
BUILD_ASSERT(sizeof(struct device_config_dns) == 44, "schema: device_config_dns");
BUILD_ASSERT(sizeof(struct device_config_ethernet) == 16, "schema: device_config_ethernet");
BUILD_ASSERT(sizeof(struct device_config_wifi) == 52, "schema: device_config_wifi");
BUILD_ASSERT(sizeof(struct device_config) == 116, "schema: device_config");
BUILD_ASSERT(sizeof(struct secret_blob) == 100, "schema: secret_blob");
BUILD_ASSERT(sizeof(struct record_payload) == 316, "schema: record_payload");
BUILD_ASSERT(sizeof(struct record_header) == 96, "schema: record_header");
BUILD_ASSERT(DEVICE_CONFIG_SCHEMA_VERSION == 1u,
	     "schema version changed: review the sizes asserted above and the migration table");

/*
 * Migrations from an older schema to the current one, applied in order. Empty
 * while version 1 is the only version that has ever shipped; the dispatcher
 * exists and is tested so the first real migration lands on rails rather than
 * being invented in a hurry. Each entry converts `from` to `from + 1` in
 * place, given the bytes as they were read.
 */
struct migration {
	uint16_t from;
	int (*fn)(struct record_payload *payload, uint32_t *payload_len);
};

static const struct migration migrations[] = {
	/* { .from = 1, .fn = migrate_v1_to_v2 }, */
};

static struct {
	const struct device_config_backend *backend;
	struct record committed;
	bool pending_present;
	struct record pending;
	bool initialised;
} store;

static K_MUTEX_DEFINE(lock);

/*
 * memset() on a buffer that is never read again is a documented target for
 * removal by the optimiser. Going through a volatile pointer keeps it.
 */
static void secure_wipe(void *buf, size_t len)
{
	volatile uint8_t *p = buf;

	while (len-- > 0U) {
		*p++ = 0U;
	}
}

static uint32_t header_checksum(const struct record_header *h)
{
	return crc32_ieee((const uint8_t *)h, offsetof(struct record_header, header_crc));
}

static bool record_is_valid(const struct record *rec, size_t read_len)
{
	if (read_len < sizeof(struct record_header)) {
		return false;
	}
	if (rec->header.magic != RECORD_MAGIC) {
		return false;
	}
	if (rec->header.header_crc != header_checksum(&rec->header)) {
		return false;
	}
	/* Only now is payload_len trustworthy enough to bound a read. */
	if (rec->header.payload_len > sizeof(struct record_payload)) {
		return false;
	}
	if (read_len < sizeof(struct record_header) + rec->header.payload_len) {
		return false;
	}

	return rec->header.payload_crc ==
	       crc32_ieee((const uint8_t *)&rec->payload, rec->header.payload_len);
}

/*
 * Bring a record read from storage up to the current schema. A newer record is
 * refused outright: guessing at a layout this build has never seen is how a
 * downgrade silently corrupts a working configuration.
 */
static int migrate_record(struct record *rec)
{
	uint16_t version = rec->header.schema_version;

	if (version > DEVICE_CONFIG_SCHEMA_VERSION) {
		return -ENOTSUP;
	}

	while (version < DEVICE_CONFIG_SCHEMA_VERSION) {
		const struct migration *step = NULL;

		for (size_t i = 0; i < ARRAY_SIZE(migrations); i++) {
			if (migrations[i].from == version) {
				step = &migrations[i];
				break;
			}
		}

		if (step == NULL) {
			/* No path from a version we can no longer read. */
			return -ENOTSUP;
		}

		int err = step->fn(&rec->payload, &rec->header.payload_len);

		if (err != 0) {
			return err;
		}

		version++;
	}

	rec->header.schema_version = DEVICE_CONFIG_SCHEMA_VERSION;
	return 0;
}

static int read_slot(enum device_config_slot slot, struct record *out)
{
	int n = store.backend->read(store.backend->ctx, slot, out, sizeof(*out));

	if (n < 0) {
		return n;
	}
	if (!record_is_valid(out, (size_t)n)) {
		return -EBADMSG;
	}

	return migrate_record(out);
}

static int write_slot(enum device_config_slot slot, const struct record *rec)
{
	const size_t len = sizeof(struct record_header) + rec->header.payload_len;
	int err = store.backend->write(store.backend->ctx, slot, rec, len);

	if (err != 0) {
		return err;
	}

	/*
	 * Read back before treating the write as done. Section 5 of the plan
	 * requires the pending snapshot to be verified before the network is
	 * touched, and the same check on commit is what stops a silently failed
	 * write from being reported to the client as a committed configuration.
	 */
	struct record verify;

	memset(&verify, 0, sizeof(verify));
	err = read_slot(slot, &verify);
	if (err != 0) {
		return -EIO;
	}
	if (memcmp(&verify.header, &rec->header, sizeof(verify.header)) != 0 ||
	    memcmp(&verify.payload, &rec->payload, rec->header.payload_len) != 0) {
		return -EIO;
	}

	return 0;
}

/*
 * Which committed slot to write next: the one the current generation does not
 * occupy, so a commit never overwrites the only good copy.
 */
static enum device_config_slot inactive_committed_slot(void)
{
	/*
	 * Revisions alternate slots, and revision 0 is the unwritten default,
	 * so the first real commit lands in A.
	 */
	return (store.committed.header.revision % 2U == 0U) ? DEVICE_CONFIG_SLOT_COMMITTED_A
							    : DEVICE_CONFIG_SLOT_COMMITTED_B;
}

void device_config_defaults(struct device_config *out)
{
	if (out == NULL) {
		return;
	}

	memset(out, 0, sizeof(*out));

	/*
	 * Section 5: "При первом запуске без сети базовый доступ — Ethernet
	 * DHCP." Wi-Fi is left off because there is nothing to join yet, and an
	 * enabled interface with no SSID is a worse default than an honest
	 * disabled one.
	 */
	out->preferred_interface = DEVICE_CONFIG_INTERFACE_ETHERNET;
	out->dns.mode = DEVICE_CONFIG_DNS_AUTOMATIC;
	out->ethernet.enabled = 1U;
	out->ethernet.ipv4.mode = DEVICE_CONFIG_IPV4_DHCP;
	out->wifi.enabled = 0U;
	out->wifi.security = DEVICE_CONFIG_WIFI_OPEN;
	out->wifi.ipv4.mode = DEVICE_CONFIG_IPV4_DHCP;
}

static void record_set_defaults(struct record *rec)
{
	memset(rec, 0, sizeof(*rec));

	rec->header.magic = RECORD_MAGIC;
	rec->header.schema_version = DEVICE_CONFIG_SCHEMA_VERSION;
	rec->header.revision = 0U;
	rec->header.payload_len = sizeof(struct record_payload);

	device_config_defaults(&rec->payload.config);
}

static void record_seal(struct record *rec)
{
	rec->header.magic = RECORD_MAGIC;
	rec->header.schema_version = DEVICE_CONFIG_SCHEMA_VERSION;
	rec->header.flags = 0U;
	rec->header.payload_len = sizeof(struct record_payload);
	rec->header.payload_crc =
		crc32_ieee((const uint8_t *)&rec->payload, rec->header.payload_len);
	rec->header.header_crc = header_checksum(&rec->header);
}

/* The two derived flags, recomputed from the secrets that actually survived. */
static void record_derive_flags(struct record *rec)
{
	rec->payload.config.wifi.password_set =
		rec->payload.secrets[DEVICE_CONFIG_SECRET_WIFI_PASSWORD].len > 0U;
	rec->payload.config.admin_password_set =
		rec->payload.secrets[DEVICE_CONFIG_SECRET_ADMIN_PASSWORD].len > 0U;
}

static bool txn_id_is_valid(const char *id)
{
	if (id == NULL) {
		return false;
	}

	/*
	 * ^[A-Za-z0-9_-]{1,64}$, so an id can go straight into a URL. Length is
	 * measured here rather than with strnlen(), which the minimal libc does
	 * not declare, and bounding the scan means an unterminated string is
	 * rejected rather than read past its buffer.
	 */
	size_t len = 0;

	while (len <= DEVICE_CONFIG_TXN_ID_MAX_LEN && id[len] != '\0') {
		char c = id[len];

		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
		      c == '_' || c == '-')) {
			return false;
		}
		len++;
	}

	return len > 0U && len <= DEVICE_CONFIG_TXN_ID_MAX_LEN;
}

/*
 * Resolve whatever was in flight when power was lost. Called only from init().
 *
 * The two branches turn on one comparison. A journal for a revision beyond the
 * committed one is an apply that never confirmed, and the contract says a
 * reboot before the durable commit restores the committed configuration. A
 * journal at or below the committed revision is the leftover of a commit that
 * had already succeeded before it could erase the journal, and discarding it
 * changes nothing.
 */
static void resolve_pending(struct device_config_recovery_report *report)
{
	struct record pending;
	int err = read_slot(DEVICE_CONFIG_SLOT_PENDING, &pending);

	if (err == -ENOENT) {
		return; /* Nothing was in flight. */
	}

	if (err != 0) {
		report->result = DEVICE_CONFIG_RECOVERY_PENDING_CORRUPT;
		(void)store.backend->erase(store.backend->ctx, DEVICE_CONFIG_SLOT_PENDING);
		return;
	}

	if (pending.header.revision > store.committed.header.revision) {
		report->result = DEVICE_CONFIG_RECOVERY_ROLLED_BACK;
		strncpy(report->transaction_id, pending.header.transaction_id,
			sizeof(report->transaction_id) - 1);
	} else {
		report->result = DEVICE_CONFIG_RECOVERY_COMMIT_COMPLETED;
	}

	(void)store.backend->erase(store.backend->ctx, DEVICE_CONFIG_SLOT_PENDING);
	secure_wipe(&pending, sizeof(pending));
}

int device_config_init(const struct device_config_backend *backend,
		       struct device_config_recovery_report *report)
{
	struct device_config_recovery_report local = {
		.result = DEVICE_CONFIG_RECOVERY_CLEAN,
	};

	if (backend == NULL || backend->read == NULL || backend->write == NULL ||
	    backend->erase == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);

	secure_wipe(&store, sizeof(store));
	store.backend = backend;

	struct record a, b;
	int err_a = read_slot(DEVICE_CONFIG_SLOT_COMMITTED_A, &a);
	int err_b = read_slot(DEVICE_CONFIG_SLOT_COMMITTED_B, &b);
	const struct record *winner = NULL;

	if (err_a == 0 && err_b == 0) {
		winner = (a.header.revision >= b.header.revision) ? &a : &b;
	} else if (err_a == 0) {
		winner = &a;
	} else if (err_b == 0) {
		winner = &b;
	}

	if (winner == NULL) {
		record_set_defaults(&store.committed);
		record_seal(&store.committed);
		/*
		 * A blank device and a device whose records have become
		 * unreadable both end up on defaults, but they are very
		 * different situations to be told about, so they are reported
		 * apart. Defaults rather than a refusal to boot is deliberate
		 * even for a record from a newer schema: Ethernet DHCP keeps
		 * the device reachable for someone to fix, and an unreachable
		 * device cannot be fixed at all.
		 */
		local.result = (err_a == -ENOENT && err_b == -ENOENT)
				       ? DEVICE_CONFIG_RECOVERY_DEFAULTS
				       : DEVICE_CONFIG_RECOVERY_COMMITTED_LOST;
	} else {
		store.committed = *winner;
		/*
		 * One readable slot and one unreadable one is not fatal — the
		 * scheme is built for it — but it means storage is failing and
		 * the newest generation may be the lost one, so it is reported
		 * rather than passed over in silence. A slot that is merely
		 * empty is the normal state before the second commit.
		 */
		if ((err_a != 0 && err_a != -ENOENT) || (err_b != 0 && err_b != -ENOENT)) {
			local.result = DEVICE_CONFIG_RECOVERY_COMMITTED_DEGRADED;
		}
	}

	resolve_pending(&local);

	local.revision = store.committed.header.revision;
	store.initialised = true;

	/*
	 * Logged at boot because a device that quietly rolled back a network
	 * change, or quietly came up on defaults, looks identical to a healthy
	 * one from the outside. Nothing here prints a secret, an SSID or an
	 * address; the transaction id is a generated opaque string.
	 */
	switch (local.result) {
	case DEVICE_CONFIG_RECOVERY_CLEAN:
		LOG_INF("configuration loaded, revision %u", local.revision);
		break;
	case DEVICE_CONFIG_RECOVERY_DEFAULTS:
		LOG_INF("no stored configuration, factory defaults in force");
		break;
	case DEVICE_CONFIG_RECOVERY_ROLLED_BACK:
		LOG_WRN("transaction %s was not confirmed before reboot; rolled back to "
			"revision %u",
			local.transaction_id, local.revision);
		break;
	case DEVICE_CONFIG_RECOVERY_COMMIT_COMPLETED:
		LOG_INF("commit to revision %u had completed; stale journal cleaned up",
			local.revision);
		break;
	case DEVICE_CONFIG_RECOVERY_PENDING_CORRUPT:
		LOG_WRN("pending journal unreadable and discarded; revision %u stands",
			local.revision);
		break;
	case DEVICE_CONFIG_RECOVERY_COMMITTED_LOST:
		LOG_ERR("stored configuration unusable; factory defaults in force");
		break;
	case DEVICE_CONFIG_RECOVERY_COMMITTED_DEGRADED:
		LOG_ERR("one committed slot unreadable; running from revision %u",
			local.revision);
		break;
	}

	secure_wipe(&a, sizeof(a));
	secure_wipe(&b, sizeof(b));
	k_mutex_unlock(&lock);

	if (report != NULL) {
		*report = local;
	}

	return 0;
}

uint32_t device_config_revision(void)
{
	k_mutex_lock(&lock, K_FOREVER);
	uint32_t revision = store.initialised ? store.committed.header.revision : 0U;

	k_mutex_unlock(&lock);

	return revision;
}

static const struct record *generation_record(enum device_config_generation generation)
{
	switch (generation) {
	case DEVICE_CONFIG_COMMITTED:
		return &store.committed;
	case DEVICE_CONFIG_PENDING:
		return store.pending_present ? &store.pending : NULL;
	default:
		return NULL;
	}
}

int device_config_get(enum device_config_generation generation, struct device_config *out)
{
	if (out == NULL || (generation != DEVICE_CONFIG_COMMITTED &&
			    generation != DEVICE_CONFIG_PENDING)) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);

	int err = 0;

	if (!store.initialised) {
		err = -EINVAL;
	} else {
		const struct record *rec = generation_record(generation);

		if (rec == NULL) {
			err = -ENOENT;
		} else {
			*out = rec->payload.config;
		}
	}

	k_mutex_unlock(&lock);

	return err;
}

int device_config_secret_get(enum device_config_generation generation,
			     enum device_config_secret_id id, uint8_t *buf, size_t cap,
			     size_t *out_len)
{
	if (buf == NULL || out_len == NULL || id >= DEVICE_CONFIG_SECRET_COUNT ||
	    (generation != DEVICE_CONFIG_COMMITTED && generation != DEVICE_CONFIG_PENDING)) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);

	int err = 0;

	if (!store.initialised) {
		err = -EINVAL;
	} else {
		const struct record *rec = generation_record(generation);

		if (rec == NULL) {
			err = -ENOENT;
		} else {
			const struct secret_blob *blob = &rec->payload.secrets[id];

			if (blob->len > cap) {
				err = -ENOMEM;
			} else {
				memcpy(buf, blob->data, blob->len);
				*out_len = blob->len;
			}
		}
	}

	k_mutex_unlock(&lock);

	return err;
}

int device_config_pending_info_get(struct device_config_pending_info *out)
{
	if (out == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);

	memset(out, 0, sizeof(*out));
	if (store.pending_present) {
		out->present = true;
		out->revision = store.pending.header.revision;
		out->base_revision = store.pending.header.base_revision;
		strncpy(out->transaction_id, store.pending.header.transaction_id,
			sizeof(out->transaction_id) - 1);
	}

	k_mutex_unlock(&lock);

	return 0;
}

/* Resolve one secret's KEEP/REPLACE/CLEAR against the committed generation. */
static int apply_secret_update(struct record *next, enum device_config_secret_id id,
			       const struct device_config_secret_update *update)
{
	struct secret_blob *dst = &next->payload.secrets[id];

	switch (update->action) {
	case DEVICE_CONFIG_SECRET_KEEP:
		/*
		 * Carried forward here rather than left as a reference to the
		 * committed generation, which is about to be replaced. This is
		 * the contract's rule that an absent field must not clear a
		 * secret, and it is resolved into bytes while the old
		 * generation still exists.
		 */
		*dst = store.committed.payload.secrets[id];
		return 0;

	case DEVICE_CONFIG_SECRET_CLEAR:
		secure_wipe(dst, sizeof(*dst));
		return 0;

	case DEVICE_CONFIG_SECRET_REPLACE:
		if (update->value == NULL || update->len == 0U ||
		    update->len > DEVICE_CONFIG_SECRET_MAX_LEN) {
			return -EINVAL;
		}
		secure_wipe(dst, sizeof(*dst));
		memcpy(dst->data, update->value, update->len);
		dst->len = (uint16_t)update->len;
		return 0;

	default:
		return -EINVAL;
	}
}

int device_config_pending_begin(const struct device_config_update *update, uint32_t base_revision,
				const char *transaction_id)
{
	if (update == NULL || !txn_id_is_valid(transaction_id)) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);

	int err = 0;
	struct record next;

	if (!store.initialised) {
		err = -EINVAL;
		goto out;
	}
	if (store.pending_present) {
		err = -EBUSY;
		goto out;
	}
	if (base_revision != store.committed.header.revision) {
		err = -ESTALE;
		goto out;
	}
	if (store.committed.header.revision == UINT32_MAX) {
		/*
		 * Refused rather than wrapped: the newest-revision-wins rule
		 * between the two committed slots stops being true the moment
		 * the counter wraps. Unreachable in practice at one commit per
		 * configuration change, and cheap to make impossible.
		 */
		err = -EOVERFLOW;
		goto out;
	}

	memset(&next, 0, sizeof(next));
	next.payload.config = update->config;
	next.header.revision = store.committed.header.revision + 1U;
	next.header.base_revision = base_revision;
	strncpy(next.header.transaction_id, transaction_id,
		sizeof(next.header.transaction_id) - 1);

	for (size_t i = 0; i < DEVICE_CONFIG_SECRET_COUNT; i++) {
		err = apply_secret_update(&next, (enum device_config_secret_id)i,
					  &update->secrets[i]);
		if (err != 0) {
			goto out;
		}
	}

	/*
	 * Derived, never taken from the caller: a handler that forgot to set
	 * password_set cannot make a stored password invisible, and one that
	 * set it wrongly cannot advertise a password that is not there.
	 */
	record_derive_flags(&next);
	record_seal(&next);

	err = write_slot(DEVICE_CONFIG_SLOT_PENDING, &next);
	if (err != 0) {
		/*
		 * The journal is the promise that this transaction can be
		 * undone. Without it the caller must not touch the network, so
		 * the slot is cleared and the failure reported rather than
		 * proceeding with a transaction nothing can roll back.
		 */
		(void)store.backend->erase(store.backend->ctx, DEVICE_CONFIG_SLOT_PENDING);
		err = -EIO;
		goto out;
	}

	store.pending = next;
	store.pending_present = true;

out:
	secure_wipe(&next, sizeof(next));
	k_mutex_unlock(&lock);

	return err;
}

int device_config_pending_commit(const char *transaction_id)
{
	if (transaction_id == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);

	int err = 0;

	if (!store.initialised || !store.pending_present) {
		err = -ENOENT;
		goto out;
	}
	if (strncmp(store.pending.header.transaction_id, transaction_id,
		    DEVICE_CONFIG_TXN_ID_MAX_LEN) != 0) {
		err = -EINVAL;
		goto out;
	}

	/*
	 * Order matters and is the whole of the power-loss argument:
	 *
	 *  1. the new record reaches the committed slot the current generation
	 *     does not occupy, and is read back;
	 *  2. only then is the journal erased.
	 *
	 * Power lost during (1) leaves a torn slot that fails its CRC, so the
	 * old generation still wins and the journal still describes an
	 * uncommitted transaction — init() rolls it back. Power lost between
	 * (1) and (2) leaves the new record winning on revision and a stale
	 * journal that init() recognises by its revision and simply cleans up.
	 */
	err = write_slot(inactive_committed_slot(), &store.pending);
	if (err != 0) {
		err = -EIO;
		goto out;
	}

	store.committed = store.pending;

	/*
	 * A failure to erase the journal is not a failed commit — the new
	 * generation is durable and in force. init() resolves the leftover, so
	 * the caller is told the truth: this succeeded.
	 */
	(void)store.backend->erase(store.backend->ctx, DEVICE_CONFIG_SLOT_PENDING);
	secure_wipe(&store.pending, sizeof(store.pending));
	store.pending_present = false;

out:
	k_mutex_unlock(&lock);

	return err;
}

int device_config_pending_rollback(const char *transaction_id)
{
	if (transaction_id == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);

	int err = 0;

	if (!store.initialised || !store.pending_present) {
		err = -ENOENT;
		goto out;
	}
	if (strncmp(store.pending.header.transaction_id, transaction_id,
		    DEVICE_CONFIG_TXN_ID_MAX_LEN) != 0) {
		err = -EINVAL;
		goto out;
	}

	err = store.backend->erase(store.backend->ctx, DEVICE_CONFIG_SLOT_PENDING);
	if (err != 0) {
		/*
		 * Kept in RAM as still present. The journal may survive on
		 * storage, and claiming a rollback that did not reach storage
		 * would let a later reboot resurrect a transaction the caller
		 * believes is gone.
		 */
		err = -EIO;
		goto out;
	}

	secure_wipe(&store.pending, sizeof(store.pending));
	store.pending_present = false;

out:
	k_mutex_unlock(&lock);

	return err;
}

const char *device_config_wifi_security_str(enum device_config_wifi_security security)
{
	switch (security) {
	case DEVICE_CONFIG_WIFI_OPEN:
		return "open";
	case DEVICE_CONFIG_WIFI_WPA2_PSK:
		return "wpa2_psk";
	case DEVICE_CONFIG_WIFI_WPA3_SAE:
		return "wpa3_sae";
	default:
		return NULL;
	}
}

const char *device_config_ipv4_mode_str(enum device_config_ipv4_mode mode)
{
	switch (mode) {
	case DEVICE_CONFIG_IPV4_DHCP:
		return "dhcp";
	case DEVICE_CONFIG_IPV4_STATIC:
		return "static";
	default:
		return NULL;
	}
}

const char *device_config_dns_mode_str(enum device_config_dns_mode mode)
{
	switch (mode) {
	case DEVICE_CONFIG_DNS_AUTOMATIC:
		return "automatic";
	case DEVICE_CONFIG_DNS_MANUAL:
		return "manual";
	default:
		return NULL;
	}
}

const char *device_config_interface_str(enum device_config_interface iface)
{
	switch (iface) {
	case DEVICE_CONFIG_INTERFACE_ETHERNET:
		return "ethernet";
	case DEVICE_CONFIG_INTERFACE_WIFI:
		return "wifi";
	default:
		return NULL;
	}
}
