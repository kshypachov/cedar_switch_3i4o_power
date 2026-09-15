/*
 * PSA Crypto transparent driver on the STM32 PKA: P-256 ECDSA sign/verify, ECDH,
 * key generation and public key export.
 *
 * Structure and buffer formats as in p256-m_driver_entrypoints.c; from Nordic
 * CRACEN: return PSA_ERROR_NOT_SUPPORTED for anything outside the accelerated
 * set so the dispatch falls through to software, reserve the hardware per call
 * (the mutex inside stm32_pka.c) and wipe secrets.
 *
 * Differences from p256-m worth knowing:
 * - sign: the nonce k comes from psa_generate_random() with rejection sampling
 *   (0 < k < n); the PKA's "r or s is zero" answer is retried with a fresh k.
 * - key_agreement: the peer point is checked on the PKA (PointCheck) before the
 *   multiplication — builtin does mbedtls_ecp_check_pubkey(); skipping it would
 *   open ECDH to invalid-curve points.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>

#include <psa/crypto.h>
#include <mbedtls/platform_util.h>

#include <stm32_pka.h>
#include <stm32_pka_psa.h>

#define SZ               STM32_PKA_P256_SIZE
#define PUBKEY_SIZE      (1 + 2 * SZ)
#define PUBKEY_HEADER    0x04
#define SIGNATURE_SIZE   (2 * SZ)
#define SIGN_RETRIES     8

/*
 * Kept in .data whatever the default: a false initialiser would move it to .bss and
 * shift the RAM layout between the "on" and "off" images compared on the board.
 */
bool stm32_pka_psa_enabled __attribute__((section(".data.stm32_pka_psa_enabled"))) =
	IS_ENABLED(CONFIG_PSA_CRYPTO_DRIVER_STM32_PKA_DEFAULT_ON);

static const uint8_t p256_n[SZ] = {
	0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xbc, 0xe6, 0xfa, 0xad, 0xa7, 0x17, 0x9e, 0x84, 0xf3, 0xb9, 0xca, 0xc2, 0xfc, 0x63, 0x25, 0x51,
};

static bool is_p256(const psa_key_attributes_t *attributes)
{
	psa_key_type_t type = psa_get_key_type(attributes);

	return PSA_KEY_TYPE_IS_ECC(type) &&
	       PSA_KEY_TYPE_ECC_GET_FAMILY(type) == PSA_ECC_FAMILY_SECP_R1 &&
	       psa_get_key_bits(attributes) == 256;
}

static psa_status_t errno_to_psa(int rc)
{
	switch (rc) {
	case 0:
		return PSA_SUCCESS;
	case -EBADMSG:
		return PSA_ERROR_INVALID_SIGNATURE;
	case -EINVAL:
		return PSA_ERROR_INVALID_ARGUMENT;
	default:
		return PSA_ERROR_HARDWARE_FAILURE;
	}
}

/* Uniform scalar in [1, n-1] by rejection sampling. */
static psa_status_t random_scalar(uint8_t *out)
{
	for (int i = 0; i < 16; i++) {
		psa_status_t st = psa_generate_random(out, SZ);
		uint8_t acc = 0;

		if (st != PSA_SUCCESS) {
			return st;
		}
		for (int j = 0; j < SZ; j++) {
			acc |= out[j];
		}
		if (acc != 0 && memcmp(out, p256_n, SZ) < 0) {
			return PSA_SUCCESS;
		}
	}
	return PSA_ERROR_INSUFFICIENT_ENTROPY;
}

psa_status_t stm32_pka_transparent_sign_hash(const psa_key_attributes_t *attributes,
					     const uint8_t *key_buffer, size_t key_buffer_size,
					     psa_algorithm_t alg, const uint8_t *hash,
					     size_t hash_length, uint8_t *signature,
					     size_t signature_size, size_t *signature_length)
{
	uint8_t k[SZ];
	psa_status_t st = PSA_ERROR_HARDWARE_FAILURE;
	int rc = -EAGAIN;

	if (!stm32_pka_psa_enabled || !is_p256(attributes) ||
	    !PSA_KEY_TYPE_IS_ECC_KEY_PAIR(psa_get_key_type(attributes)) ||
	    !PSA_ALG_IS_RANDOMIZED_ECDSA(alg) || hash_length != SZ) {
		return PSA_ERROR_NOT_SUPPORTED;
	}
	if (key_buffer_size != SZ) {
		return PSA_ERROR_INVALID_ARGUMENT;
	}
	if (signature_size < SIGNATURE_SIZE) {
		return PSA_ERROR_BUFFER_TOO_SMALL;
	}

	for (int i = 0; i < SIGN_RETRIES && rc == -EAGAIN; i++) {
		st = random_scalar(k);
		if (st != PSA_SUCCESS) {
			break;
		}
		rc = stm32_pka_p256_ecdsa_sign(key_buffer, k, hash, signature, signature + SZ);
		st = errno_to_psa(rc);
	}
	mbedtls_platform_zeroize(k, sizeof(k));
	if (st == PSA_SUCCESS) {
		*signature_length = SIGNATURE_SIZE;
	} else {
		mbedtls_platform_zeroize(signature, signature_size);
	}
	return st;
}

psa_status_t stm32_pka_transparent_export_public_key(const psa_key_attributes_t *attributes,
						     const uint8_t *key_buffer,
						     size_t key_buffer_size, uint8_t *data,
						     size_t data_size, size_t *data_length)
{
	int rc;

	if (!stm32_pka_psa_enabled || !is_p256(attributes) ||
	    !PSA_KEY_TYPE_IS_ECC_KEY_PAIR(psa_get_key_type(attributes))) {
		return PSA_ERROR_NOT_SUPPORTED;
	}
	if (key_buffer_size != SZ) {
		return PSA_ERROR_INVALID_ARGUMENT;
	}
	if (data_size < PUBKEY_SIZE) {
		return PSA_ERROR_BUFFER_TOO_SMALL;
	}

	data[0] = PUBKEY_HEADER;
	rc = stm32_pka_p256_mul(key_buffer, NULL, NULL, data + 1, data + 1 + SZ);
	if (rc != 0) {
		return errno_to_psa(rc);
	}
	*data_length = PUBKEY_SIZE;
	return PSA_SUCCESS;
}

psa_status_t stm32_pka_transparent_verify_hash(const psa_key_attributes_t *attributes,
					       const uint8_t *key_buffer, size_t key_buffer_size,
					       psa_algorithm_t alg, const uint8_t *hash,
					       size_t hash_length, const uint8_t *signature,
					       size_t signature_length)
{
	uint8_t pub[PUBKEY_SIZE];
	const uint8_t *q = key_buffer;
	psa_key_type_t type = psa_get_key_type(attributes);

	if (!stm32_pka_psa_enabled || !is_p256(attributes) || !PSA_ALG_IS_ECDSA(alg) ||
	    hash_length != SZ) {
		return PSA_ERROR_NOT_SUPPORTED;
	}

	if (PSA_KEY_TYPE_IS_ECC_KEY_PAIR(type)) {
		size_t len;
		psa_status_t st = stm32_pka_transparent_export_public_key(
			attributes, key_buffer, key_buffer_size, pub, sizeof(pub), &len);

		if (st != PSA_SUCCESS) {
			return st;
		}
		q = pub;
	} else if (key_buffer_size != PUBKEY_SIZE || key_buffer[0] != PUBKEY_HEADER) {
		return PSA_ERROR_INVALID_ARGUMENT;
	}
	if (signature_length != SIGNATURE_SIZE) {
		return PSA_ERROR_INVALID_SIGNATURE;
	}

	return errno_to_psa(stm32_pka_p256_ecdsa_verify(q + 1, q + 1 + SZ, hash, signature,
							signature + SZ));
}

psa_status_t stm32_pka_transparent_generate_key(const psa_key_attributes_t *attributes,
						uint8_t *key_buffer, size_t key_buffer_size,
						size_t *key_buffer_length)
{
	psa_status_t st;

	if (!stm32_pka_psa_enabled || !is_p256(attributes) ||
	    psa_get_key_type(attributes) != PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1)) {
		return PSA_ERROR_NOT_SUPPORTED;
	}
	if (key_buffer_size < SZ) {
		return PSA_ERROR_BUFFER_TOO_SMALL;
	}

	/* A key pair is stored as the private scalar; the public key is derived on export. */
	st = random_scalar(key_buffer);
	if (st == PSA_SUCCESS) {
		*key_buffer_length = SZ;
	} else {
		mbedtls_platform_zeroize(key_buffer, key_buffer_size);
	}
	return st;
}

psa_status_t stm32_pka_transparent_key_agreement(const psa_key_attributes_t *attributes,
						 const uint8_t *key_buffer, size_t key_buffer_size,
						 psa_algorithm_t alg, const uint8_t *peer_key,
						 size_t peer_key_length, uint8_t *shared_secret,
						 size_t shared_secret_size,
						 size_t *shared_secret_length)
{
	uint8_t y[SZ];
	int rc;

	if (!stm32_pka_psa_enabled || !is_p256(attributes) || !PSA_ALG_IS_ECDH(alg) ||
	    !PSA_KEY_TYPE_IS_ECC_KEY_PAIR(psa_get_key_type(attributes))) {
		return PSA_ERROR_NOT_SUPPORTED;
	}
	if (key_buffer_size != SZ || peer_key_length != PUBKEY_SIZE ||
	    peer_key[0] != PUBKEY_HEADER) {
		return PSA_ERROR_INVALID_ARGUMENT;
	}
	if (shared_secret_size < SZ) {
		return PSA_ERROR_BUFFER_TOO_SMALL;
	}

	rc = stm32_pka_p256_point_check(peer_key + 1, peer_key + 1 + SZ);
	if (rc == 0) {
		rc = stm32_pka_p256_mul(key_buffer, peer_key + 1, peer_key + 1 + SZ, shared_secret,
					y);
	}
	mbedtls_platform_zeroize(y, sizeof(y));
	if (rc != 0) {
		mbedtls_platform_zeroize(shared_secret, shared_secret_size);
		return errno_to_psa(rc);
	}
	*shared_secret_length = SZ;
	return PSA_SUCCESS;
}
