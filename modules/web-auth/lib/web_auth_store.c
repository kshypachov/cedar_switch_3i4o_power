/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The administrator verifier in settings-registry.
 *
 * Why here rather than in device-config-store, which has a slot named
 * DEVICE_CONFIG_SECRET_ADMIN_PASSWORD: that store versions the whole network
 * configuration as one generation with a revision, and allows one journalled
 * transaction at a time. A password change stored there would bump the network
 * revision - turning an unrelated staged network change into 409
 * stale_revision - and would be refused outright while a network change awaits
 * confirmation. The two have nothing to do with each other, so they are not
 * stored together. The owner's rule that application code reaches settings
 * only through settings-registry is kept.
 *
 * One quirk of the registry shapes this file: a DIRECT BYTES key that has
 * never been written reads back as its default, max_len bytes of zeros, with
 * nothing to tell it from a stored value (settings_registry.c,
 * direct_get_default). A real verifier starts with "CWA1", so all zeros is
 * taken to mean "never stored".
 */

#include <errno.h>
#include <string.h>

#include <settings_registry/settings_registry.h>

#include <web_auth/web_auth.h>
#include <web_auth/web_auth_adapters.h>

#define VERIFIER_KEY "auth/admin_verifier"

SETTING_REGISTRY_DEFINE(web_auth_verifier_setting, .key = VERIFIER_KEY,
			.type = SETTING_TYPE_BYTES, .storage = SETTING_STORAGE_OWNED,
			.persistence = SETTING_PERSISTENT, .mirror = SETTING_MIRROR_DIRECT,
			.max_len = WEB_AUTH_VERIFIER_LEN, .default_value = NULL);

int web_auth_store_load(uint8_t *buf, size_t cap, size_t *out_len)
{
	struct setting_value value = {
		.type = SETTING_TYPE_BYTES,
		.buf = {.data = buf, .len = cap},
	};
	uint8_t any = 0U;
	int rc;

	if (cap < WEB_AUTH_VERIFIER_LEN) {
		return -ENOSPC;
	}
	rc = setting_get(VERIFIER_KEY, &value);
	if (rc != 0) {
		return rc;
	}
	for (size_t i = 0; i < value.buf.len; i++) {
		any |= buf[i];
	}
	if (any == 0U) {
		return -ENOENT;
	}
	*out_len = value.buf.len;

	return 0;
}

int web_auth_store_save(const uint8_t *buf, size_t len)
{
	struct setting_value value = {
		.type = SETTING_TYPE_BYTES,
		/* The registry does not write through this pointer. */
		.buf = {.data = (uint8_t *)buf, .len = len},
	};

	return setting_set(VERIFIER_KEY, &value);
}
