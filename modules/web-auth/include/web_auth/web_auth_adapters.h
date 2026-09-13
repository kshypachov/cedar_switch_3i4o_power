/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The production implementations of struct web_auth_platform's functions.
 *
 * Kept out of web_auth.h because the core does not need them and the sim
 * tier's core suite builds without the libraries they wrap. Each is a thin
 * translation with no decision of its own - the testability rule of section 12
 * of the plan - and each has its own test where its library exists.
 */

#ifndef WEB_AUTH_ADAPTERS_H_
#define WEB_AUTH_ADAPTERS_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief PBKDF2-HMAC-SHA256 through PSA Crypto (CONFIG_WEB_AUTH_PSA_KDF).
 *
 * psa_key_derivation with PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256), inputs in the
 * order the algorithm requires: cost, salt, password.
 *
 * The derivation runs on a dedicated low-priority preemptive thread and the
 * caller waits for it, because the caller is usually the HTTP server's
 * cooperative thread, and a derivation computed there stops every other
 * thread of the system for its duration (measured; lib/web_auth_psa.c).
 * Calls are serialised.
 *
 * @retval 0      @p out holds @p out_len derived bytes
 * @retval -EIO   PSA refused; @p out is wiped
 */
int web_auth_psa_pbkdf2(const uint8_t *password, size_t password_len, const uint8_t *salt,
			size_t salt_len, uint32_t iterations, uint8_t *out, size_t out_len);

/**
 * @brief Cryptographically secure random bytes from Zephyr's CSPRNG.
 *
 * sys_csrand_get(), which on the board is the STM32 RNG through the entropy
 * driver - the same source PSA Crypto is configured to use
 * (CONFIG_MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG).
 */
int web_auth_csrand(uint8_t *buf, size_t len);

/** @brief Monotonic milliseconds, k_uptime_get(). */
int64_t web_auth_uptime_ms(void);

/**
 * @brief Read the verifier from settings-registry (CONFIG_WEB_AUTH_REGISTRY_STORE).
 *
 * The key is "auth/admin_verifier": persistent BYTES, read on demand rather
 * than mirrored in RAM. A key that has never been written is reported as
 * -ENOENT, which is what puts a fresh device into setup.
 */
int web_auth_store_load(uint8_t *buf, size_t cap, size_t *out_len);

/** @brief Write the verifier to settings-registry, durably. */
int web_auth_store_save(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* WEB_AUTH_ADAPTERS_H_ */
