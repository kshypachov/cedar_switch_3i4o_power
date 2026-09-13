/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * device-config-store's three slots in settings-registry (owner's decision,
 * P4: the network configuration lives in settings, like every other setting
 * of this application).
 *
 * A slot is one BYTES key of NETCFG_RECORD_BYTES: two bytes of length, little
 * endian, then the record, then zeros. The registry stores a BYTES value as
 * the whole fixed-size blob and reads a key that was never written as zeros,
 * so a length of zero is "empty" and an erase writes zeros. A length that does
 * not fit is reported as a read error, which device-config-store treats as a
 * torn slot.
 *
 * Every write goes through settings_save_one() and is therefore as durable as
 * the settings backend in the image; device-config-store reads each write back
 * before it counts.
 */

#ifndef NETCFG_STORE_H_
#define NETCFG_STORE_H_

#include <stddef.h>
#include <stdint.h>

#include <device_config_store/device_config_store.h>

/** Bytes of one stored slot: the record (412 bytes in schema 1) and its length, with room to grow. */
#define NETCFG_RECORD_BYTES 512

/** Fixed-size values by key. The sim tier supplies a fake. */
struct netcfg_kv {
	/** Read exactly @p cap bytes of @p key; zeros when it was never written. */
	int (*get)(const char *key, uint8_t *buf, size_t cap);
	/** Replace @p key with @p len bytes, durably. */
	int (*set)(const char *key, const uint8_t *buf, size_t len);
};

/** Keys of the three slots, in enum device_config_slot order. */
extern const char *const netcfg_slot_keys[DEVICE_CONFIG_SLOT_COUNT];

/** @brief Make @p out a device-config-store backend over @p kv. @p kv must outlive it. */
void netcfg_store_bind(const struct netcfg_kv *kv, struct device_config_backend *out);

#if defined(CONFIG_SETTINGS_REGISTRY)
/** The slots in settings-registry. */
extern const struct netcfg_kv netcfg_registry_kv;
#endif

#endif /* NETCFG_STORE_H_ */
