/*
 * P-256 primitives on the STM32 PKA (public key accelerator).
 *
 * All integers and coordinates are big-endian, exactly 32 bytes. Calls are
 * serialised by a mutex and block the caller while the PKA computes (polling);
 * do not call from an ISR. The first call brings the block up (PKA clock, RNG
 * clock, PKA RAM erase).
 */

#ifndef STM32_PKA_H_
#define STM32_PKA_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STM32_PKA_P256_SIZE 32

/* Bring the PKA up now (also done lazily by the first operation). */
int stm32_pka_init(void);

/*
 * R = k * P. With px == NULL and py == NULL, P is the generator G.
 * Returns 0, -EINVAL if the PKA rejects the input (point not on the curve,
 * scalar out of range), -ETIMEDOUT or -EIO.
 */
int stm32_pka_p256_mul(const uint8_t *k, const uint8_t *px, const uint8_t *py,
		       uint8_t *rx, uint8_t *ry);

/* Returns 0 if (x, y) is on the curve, -EINVAL if not, -ETIMEDOUT or -EIO. */
int stm32_pka_p256_point_check(const uint8_t *x, const uint8_t *y);

/*
 * ECDSA signature of a 32-byte hash with private key d and nonce k.
 * Returns 0, -EAGAIN if the PKA reports r == 0 or s == 0 (retry with another
 * k), -ETIMEDOUT or -EIO. The PKA RAM is cleared afterwards.
 */
int stm32_pka_p256_ecdsa_sign(const uint8_t *d, const uint8_t *k, const uint8_t *hash,
			      uint8_t *r, uint8_t *s);

/*
 * ECDSA verification against public key (qx, qy).
 * Returns 0 if valid, -EBADMSG if the signature is invalid (including r or s
 * out of [1, n-1]), -ETIMEDOUT or -EIO.
 */
int stm32_pka_p256_ecdsa_verify(const uint8_t *qx, const uint8_t *qy, const uint8_t *hash,
				const uint8_t *r, const uint8_t *s);

/*
 * R = k * P + m * Q on affine points (PKA double base ladder, affine result).
 * Returns 0, -EINVAL if the PKA rejects the input or the sum is the point at
 * infinity, -ETIMEDOUT or -EIO. The PKA RAM is cleared afterwards.
 */
int stm32_pka_p256_muladd(const uint8_t *k, const uint8_t *px, const uint8_t *py,
			  const uint8_t *m, const uint8_t *qx, const uint8_t *qy,
			  uint8_t *rx, uint8_t *ry);

/*
 * Runtime switch of the builtin ECP hook (CONFIG_STM32_PKA_ECP_P256,
 * patches/tf-psa-crypto/ecp-p256-stm32-pka.patch): mbedtls_ecp_mul()/muladd() on P-256.
 */
extern bool stm32_pka_ecp_enabled;

/* Per-operation counters, for the shell and for commissioning measurements. */
enum stm32_pka_op {
	STM32_PKA_OP_MUL,
	STM32_PKA_OP_POINT_CHECK,
	STM32_PKA_OP_SIGN,
	STM32_PKA_OP_VERIFY,
	STM32_PKA_OP_MULADD,
	STM32_PKA_OP_COUNT,
};

struct stm32_pka_op_stats {
	uint32_t calls;
	uint32_t errors;
	uint64_t total_us;
	uint32_t max_us;
};

void stm32_pka_stats_get(struct stm32_pka_op_stats out[STM32_PKA_OP_COUNT]);
void stm32_pka_stats_reset(void);
const char *stm32_pka_op_name(enum stm32_pka_op op);

#ifdef __cplusplus
}
#endif

#endif /* STM32_PKA_H_ */
