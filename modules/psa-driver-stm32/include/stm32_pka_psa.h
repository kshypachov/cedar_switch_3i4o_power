/*
 * PSA Crypto transparent driver entry points on the STM32 PKA (P-256 only).
 *
 * Included by the dispatch copy in modules/psa-driver-stm32/dispatch. The entry
 * points follow tf-psa-crypto/drivers/p256-m/p256-m_driver_entrypoints.h (same
 * key buffer formats: key pair = 32-byte private scalar, public key = 0x04||X||Y,
 * signature = r||s). Anything else returns PSA_ERROR_NOT_SUPPORTED, so the
 * dispatch falls through to p256-m / builtin, as in Nordic CRACEN.
 */

#ifndef STM32_PKA_PSA_H_
#define STM32_PKA_PSA_H_

#if defined(CONFIG_PSA_CRYPTO_DRIVER_STM32_PKA)

#ifndef PSA_CRYPTO_DRIVER_PRESENT
#define PSA_CRYPTO_DRIVER_PRESENT
#endif
#ifndef PSA_CRYPTO_ACCELERATOR_DRIVER_PRESENT
#define PSA_CRYPTO_ACCELERATOR_DRIVER_PRESENT
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <psa/crypto.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Runtime switch (shell 'pka psa on|off'); when false every entry point returns NOT_SUPPORTED. */
extern bool stm32_pka_psa_enabled;

psa_status_t stm32_pka_transparent_sign_hash(const psa_key_attributes_t *attributes,
					     const uint8_t *key_buffer, size_t key_buffer_size,
					     psa_algorithm_t alg, const uint8_t *hash,
					     size_t hash_length, uint8_t *signature,
					     size_t signature_size, size_t *signature_length);

psa_status_t stm32_pka_transparent_verify_hash(const psa_key_attributes_t *attributes,
					       const uint8_t *key_buffer, size_t key_buffer_size,
					       psa_algorithm_t alg, const uint8_t *hash,
					       size_t hash_length, const uint8_t *signature,
					       size_t signature_length);

psa_status_t stm32_pka_transparent_generate_key(const psa_key_attributes_t *attributes,
						uint8_t *key_buffer, size_t key_buffer_size,
						size_t *key_buffer_length);

psa_status_t stm32_pka_transparent_export_public_key(const psa_key_attributes_t *attributes,
						     const uint8_t *key_buffer,
						     size_t key_buffer_size, uint8_t *data,
						     size_t data_size, size_t *data_length);

psa_status_t stm32_pka_transparent_key_agreement(const psa_key_attributes_t *attributes,
						 const uint8_t *key_buffer, size_t key_buffer_size,
						 psa_algorithm_t alg, const uint8_t *peer_key,
						 size_t peer_key_length, uint8_t *shared_secret,
						 size_t shared_secret_size,
						 size_t *shared_secret_length);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_PSA_CRYPTO_DRIVER_STM32_PKA */
#endif /* STM32_PKA_PSA_H_ */
