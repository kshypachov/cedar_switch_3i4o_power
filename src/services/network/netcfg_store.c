/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * See netcfg_store.h.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/sys/util.h>

#if defined(CONFIG_SETTINGS_REGISTRY)
#include <settings_registry/settings_registry.h>
#endif

#include "netcfg_store.h"

#define LENGTH_BYTES 2U

const char *const netcfg_slot_keys[DEVICE_CONFIG_SLOT_COUNT] = {
	[DEVICE_CONFIG_SLOT_COMMITTED_A] = "net/config_a",
	[DEVICE_CONFIG_SLOT_COMMITTED_B] = "net/config_b",
	[DEVICE_CONFIG_SLOT_PENDING] = "net/config_pending",
};

/* The blob holds the Wi-Fi password: nothing of it stays on a stack. */
static void wipe(void *buf, size_t len)
{
	volatile uint8_t *p = buf;

	while (len-- > 0U) {
		*p++ = 0U;
	}
}

static int slot_read(void *ctx, enum device_config_slot slot, void *buf, size_t cap)
{
	const struct netcfg_kv *kv = ctx;
	uint8_t blob[NETCFG_RECORD_BYTES];
	size_t len;
	int rc;

	if ((unsigned int)slot >= DEVICE_CONFIG_SLOT_COUNT) {
		return -EINVAL;
	}
	rc = kv->get(netcfg_slot_keys[slot], blob, sizeof(blob));
	if (rc != 0) {
		wipe(blob, sizeof(blob));
		return rc;
	}

	len = (size_t)blob[0] | ((size_t)blob[1] << 8);
	if (len == 0U) {
		rc = -ENOENT;
	} else if (len > sizeof(blob) - LENGTH_BYTES || len > cap) {
		rc = -EIO;
	} else {
		memcpy(buf, &blob[LENGTH_BYTES], len);
		rc = (int)len;
	}
	wipe(blob, sizeof(blob));

	return rc;
}

static int slot_write(void *ctx, enum device_config_slot slot, const void *buf, size_t len)
{
	const struct netcfg_kv *kv = ctx;
	uint8_t blob[NETCFG_RECORD_BYTES];
	int rc;

	if ((unsigned int)slot >= DEVICE_CONFIG_SLOT_COUNT || len == 0U) {
		return -EINVAL;
	}
	if (len > sizeof(blob) - LENGTH_BYTES) {
		return -ENOSPC;
	}

	memset(blob, 0, sizeof(blob));
	blob[0] = (uint8_t)(len & 0xFFU);
	blob[1] = (uint8_t)(len >> 8);
	memcpy(&blob[LENGTH_BYTES], buf, len);
	rc = kv->set(netcfg_slot_keys[slot], blob, sizeof(blob));
	wipe(blob, sizeof(blob));

	return rc;
}

static int slot_erase(void *ctx, enum device_config_slot slot)
{
	const struct netcfg_kv *kv = ctx;
	const uint8_t zeros[NETCFG_RECORD_BYTES] = {0};

	if ((unsigned int)slot >= DEVICE_CONFIG_SLOT_COUNT) {
		return -EINVAL;
	}
	return kv->set(netcfg_slot_keys[slot], zeros, sizeof(zeros));
}

void netcfg_store_bind(const struct netcfg_kv *kv, struct device_config_backend *out)
{
	out->read = slot_read;
	out->write = slot_write;
	out->erase = slot_erase;
	out->ctx = (void *)kv;
}

#if defined(CONFIG_SETTINGS_REGISTRY)

/*
 * DIRECT, not mirrored: the store keeps both generations in RAM itself, so a
 * registry copy would be a third, and the keys are read once at boot.
 */
#define NETCFG_SLOT_SETTING(_name, _slot)                                                          \
	SETTING_REGISTRY_DEFINE(_name, .key = "net/config_" _slot, .type = SETTING_TYPE_BYTES,     \
				.storage = SETTING_STORAGE_OWNED,                                  \
				.persistence = SETTING_PERSISTENT,                                 \
				.mirror = SETTING_MIRROR_DIRECT, .max_len = NETCFG_RECORD_BYTES,   \
				.default_value = NULL)

NETCFG_SLOT_SETTING(netcfg_slot_a, "a");
NETCFG_SLOT_SETTING(netcfg_slot_b, "b");
NETCFG_SLOT_SETTING(netcfg_slot_pending, "pending");

static int registry_get(const char *key, uint8_t *buf, size_t cap)
{
	struct setting_value value = {
		.type = SETTING_TYPE_BYTES,
		.buf = {.data = buf, .len = cap},
	};

	return setting_get(key, &value);
}

static int registry_set(const char *key, const uint8_t *buf, size_t len)
{
	struct setting_value value = {
		.type = SETTING_TYPE_BYTES,
		/* The registry does not write through this pointer. */
		.buf = {.data = (uint8_t *)buf, .len = len},
	};

	return setting_set(key, &value);
}

const struct netcfg_kv netcfg_registry_kv = {
	.get = registry_get,
	.set = registry_set,
};

#endif /* CONFIG_SETTINGS_REGISTRY */
