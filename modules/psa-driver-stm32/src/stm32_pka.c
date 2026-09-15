/*
 * P-256 primitives on the STM32 PKA through the STM32Cube HAL (polling).
 *
 * Bring-up (STM32U5): the PKA needs the RNG clock running while it is enabled —
 * HAL_PKA_Init() waits for SR.INITOK after the PKA RAM erase, and that never
 * comes without rng_clk. The entropy driver gates the RNG clock between refills
 * unless the PKA is enabled (entropy_stm32.c, "PKA needs RNG clock"), so the
 * RNG clock is switched on here right before HAL_PKA_Init() and CR.EN is left
 * set for good.
 *
 * Curve constants and the coefficient convention (coefSign = 1, |a| = 3) follow
 * STM32CubeU5 Projects/B-U585I-IOT02A/Examples/PKA/PKA_ECDSA_Sign/Src/prime256v1.c.
 */

#define DT_DRV_COMPAT st_stm32_pka

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/stm32_clock_control.h>
#include <zephyr/logging/log.h>
#include <soc.h>
#include <stm32cube_hal.h>

#include <stm32_pka.h>

LOG_MODULE_REGISTER(stm32_pka, CONFIG_STM32_PKA_LOG_LEVEL);

#define SZ STM32_PKA_P256_SIZE

static const uint8_t p256_p[SZ] = {
	0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};
static const uint8_t p256_abs_a[SZ] = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
};
#define P256_A_SIGN 1U /* a = -3 */
static const uint8_t p256_b[SZ] = {
	0x5a, 0xc6, 0x35, 0xd8, 0xaa, 0x3a, 0x93, 0xe7, 0xb3, 0xeb, 0xbd, 0x55, 0x76, 0x98, 0x86, 0xbc,
	0x65, 0x1d, 0x06, 0xb0, 0xcc, 0x53, 0xb0, 0xf6, 0x3b, 0xce, 0x3c, 0x3e, 0x27, 0xd2, 0x60, 0x4b,
};
static const uint8_t p256_gx[SZ] = {
	0x6b, 0x17, 0xd1, 0xf2, 0xe1, 0x2c, 0x42, 0x47, 0xf8, 0xbc, 0xe6, 0xe5, 0x63, 0xa4, 0x40, 0xf2,
	0x77, 0x03, 0x7d, 0x81, 0x2d, 0xeb, 0x33, 0xa0, 0xf4, 0xa1, 0x39, 0x45, 0xd8, 0x98, 0xc2, 0x96,
};
static const uint8_t p256_gy[SZ] = {
	0x4f, 0xe3, 0x42, 0xe2, 0xfe, 0x1a, 0x7f, 0x9b, 0x8e, 0xe7, 0xeb, 0x4a, 0x7c, 0x0f, 0x9e, 0x16,
	0x2b, 0xce, 0x33, 0x57, 0x6b, 0x31, 0x5e, 0xce, 0xcb, 0xb6, 0x40, 0x68, 0x37, 0xbf, 0x51, 0xf5,
};
static const uint8_t p256_n[SZ] = {
	0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xbc, 0xe6, 0xfa, 0xad, 0xa7, 0x17, 0x9e, 0x84, 0xf3, 0xb9, 0xca, 0xc2, 0xfc, 0x63, 0x25, 0x51,
};

static const struct stm32_pclken pka_clk = STM32_DT_INST_CLOCK_INFO(0);
static const struct stm32_pclken rng_clk = STM32_CLOCK_INFO(0, DT_NODELABEL(rng));

static PKA_HandleTypeDef hpka;
static uint32_t p256_mont_r2[SZ / 4]; /* Montgomery parameter of p, for PointCheck */
static bool pka_ready;
static K_MUTEX_DEFINE(pka_lock);

static struct stm32_pka_op_stats op_stats[STM32_PKA_OP_COUNT];

const char *stm32_pka_op_name(enum stm32_pka_op op)
{
	static const char *const names[STM32_PKA_OP_COUNT] = {
		[STM32_PKA_OP_MUL] = "mul",
		[STM32_PKA_OP_POINT_CHECK] = "point_check",
		[STM32_PKA_OP_SIGN] = "sign",
		[STM32_PKA_OP_VERIFY] = "verify",
		[STM32_PKA_OP_MULADD] = "muladd",
	};

	return op < STM32_PKA_OP_COUNT ? names[op] : "?";
}

void stm32_pka_stats_get(struct stm32_pka_op_stats out[STM32_PKA_OP_COUNT])
{
	k_mutex_lock(&pka_lock, K_FOREVER);
	memcpy(out, op_stats, sizeof(op_stats));
	k_mutex_unlock(&pka_lock);
}

void stm32_pka_stats_reset(void)
{
	k_mutex_lock(&pka_lock, K_FOREVER);
	memset(op_stats, 0, sizeof(op_stats));
	k_mutex_unlock(&pka_lock);
}

static void stats_add(enum stm32_pka_op op, int64_t start_ticks, int rc)
{
	uint64_t us = k_ticks_to_us_near64(k_uptime_ticks() - start_ticks);
	struct stm32_pka_op_stats *st = &op_stats[op];

	st->calls++;
	st->total_us += us;
	if (us > st->max_us) {
		st->max_us = (uint32_t)us;
	}
	/* An invalid signature or point is an answer, not a failure of the block. */
	if (rc != 0 && rc != -EBADMSG && rc != -EINVAL) {
		st->errors++;
	}
}

static int hal_to_errno(HAL_StatusTypeDef st)
{
	if (st == HAL_OK) {
		return 0;
	}
	uint32_t err = HAL_PKA_GetError(&hpka);

	if (err & HAL_PKA_ERROR_TIMEOUT) {
		return -ETIMEDOUT;
	}
	if (err & HAL_PKA_ERROR_OPERATION) {
		return -EAGAIN;
	}
	LOG_ERR("PKA error 0x%x (HAL %d)", err, st);
	return -EIO;
}

/* Called with pka_lock held. */
static int pka_bring_up(void)
{
	const struct device *clk = DEVICE_DT_GET(STM32_CLOCK_CONTROL_NODE);
	HAL_StatusTypeDef st = HAL_ERROR;
	int rc;

	if (pka_ready) {
		return 0;
	}
	if (!device_is_ready(clk)) {
		return -ENODEV;
	}

	hpka.Instance = (PKA_TypeDef *)DT_INST_REG_ADDR(0);

	for (int attempt = 0; attempt < 3; attempt++) {
		/*
		 * The entropy driver may gate the RNG clock between our clock_control_on()
		 * and CR.EN; once CR.EN is set it leaves the clock alone. Retry covers
		 * that window.
		 */
		rc = clock_control_on(clk, (clock_control_subsys_t)&rng_clk);
		if (rc == 0) {
			rc = clock_control_on(clk, (clock_control_subsys_t)&pka_clk);
		}
		if (rc != 0) {
			LOG_ERR("clock on failed: %d", rc);
			return -EIO;
		}
		hpka.State = HAL_PKA_STATE_RESET;
		st = HAL_PKA_Init(&hpka);
		if (st == HAL_OK) {
			break;
		}
		LOG_WRN("HAL_PKA_Init attempt %d: %d", attempt, st);
	}
	if (st != HAL_OK) {
		return -EIO;
	}

	PKA_MontgomeryParamInTypeDef mont = { .size = SZ, .pOp1 = p256_p };

	st = HAL_PKA_MontgomeryParam(&hpka, &mont, CONFIG_STM32_PKA_TIMEOUT_MS);
	if (st != HAL_OK) {
		LOG_ERR("Montgomery parameter: %d", st);
		return hal_to_errno(st);
	}
	HAL_PKA_MontgomeryParam_GetResult(&hpka, p256_mont_r2);

	pka_ready = true;
	LOG_INF("PKA ready");
	return 0;
}

int stm32_pka_init(void)
{
	int rc;

	k_mutex_lock(&pka_lock, K_FOREVER);
	rc = pka_bring_up();
	k_mutex_unlock(&pka_lock);
	return rc;
}

/* Big-endian a < b */
static bool be_less(const uint8_t *a, const uint8_t *b)
{
	return memcmp(a, b, SZ) < 0;
}

static bool be_is_zero(const uint8_t *a)
{
	uint8_t acc = 0;

	for (int i = 0; i < SZ; i++) {
		acc |= a[i];
	}
	return acc == 0;
}

int stm32_pka_p256_mul(const uint8_t *k, const uint8_t *px, const uint8_t *py,
		       uint8_t *rx, uint8_t *ry)
{
	int64_t t0;
	int rc;

	if ((px == NULL) != (py == NULL)) {
		return -EINVAL;
	}

	k_mutex_lock(&pka_lock, K_FOREVER);
	t0 = k_uptime_ticks();
	rc = pka_bring_up();
	if (rc == 0) {
		PKA_ECCMulInTypeDef in = {
			.scalarMulSize = SZ,
			.modulusSize = SZ,
			.coefSign = P256_A_SIGN,
			.coefA = p256_abs_a,
			.coefB = p256_b,
			.modulus = p256_p,
			.pointX = px != NULL ? px : p256_gx,
			.pointY = py != NULL ? py : p256_gy,
			.scalarMul = k,
			.primeOrder = p256_n,
		};
		PKA_ECCMulOutTypeDef out = { .ptX = rx, .ptY = ry };

		rc = hal_to_errno(HAL_PKA_ECCMul(&hpka, &in, CONFIG_STM32_PKA_TIMEOUT_MS));
		if (rc == 0) {
			HAL_PKA_ECCMul_GetResult(&hpka, &out);
		} else if (rc == -EAGAIN) {
			/* The PKA flags the input (point at infinity / invalid). */
			rc = -EINVAL;
		}
		HAL_PKA_RAMReset(&hpka); /* the scalar may be a private key */
	}
	stats_add(STM32_PKA_OP_MUL, t0, rc);
	k_mutex_unlock(&pka_lock);
	return rc;
}

int stm32_pka_p256_point_check(const uint8_t *x, const uint8_t *y)
{
	int64_t t0;
	int rc;

	k_mutex_lock(&pka_lock, K_FOREVER);
	t0 = k_uptime_ticks();
	rc = pka_bring_up();
	if (rc == 0 && !(be_less(x, p256_p) && be_less(y, p256_p))) {
		rc = -EINVAL;
	} else if (rc == 0) {
		PKA_PointCheckInTypeDef in = {
			.modulusSize = SZ,
			.coefSign = P256_A_SIGN,
			.coefA = p256_abs_a,
			.coefB = p256_b,
			.modulus = p256_p,
			.pointX = x,
			.pointY = y,
			.pMontgomeryParam = p256_mont_r2,
		};

		rc = hal_to_errno(HAL_PKA_PointCheck(&hpka, &in, CONFIG_STM32_PKA_TIMEOUT_MS));
		if (rc == 0 && HAL_PKA_PointCheck_IsOnCurve(&hpka) != 1U) {
			rc = -EINVAL;
		}
	}
	stats_add(STM32_PKA_OP_POINT_CHECK, t0, rc);
	k_mutex_unlock(&pka_lock);
	return rc;
}

int stm32_pka_p256_ecdsa_sign(const uint8_t *d, const uint8_t *k, const uint8_t *hash,
			      uint8_t *r, uint8_t *s)
{
	int64_t t0;
	int rc;

	k_mutex_lock(&pka_lock, K_FOREVER);
	t0 = k_uptime_ticks();
	rc = pka_bring_up();
	if (rc == 0) {
		PKA_ECDSASignInTypeDef in = {
			.primeOrderSize = SZ,
			.modulusSize = SZ,
			.coefSign = P256_A_SIGN,
			.coef = p256_abs_a,
			.coefB = p256_b,
			.modulus = p256_p,
			.integer = k,
			.basePointX = p256_gx,
			.basePointY = p256_gy,
			.hash = hash,
			.privateKey = d,
			.primeOrder = p256_n,
		};
		PKA_ECDSASignOutTypeDef out = { .RSign = r, .SSign = s };

		rc = hal_to_errno(HAL_PKA_ECDSASign(&hpka, &in, CONFIG_STM32_PKA_TIMEOUT_MS));
		if (rc == 0) {
			HAL_PKA_ECDSASign_GetResult(&hpka, &out, NULL);
		}
		HAL_PKA_RAMReset(&hpka); /* d and k were loaded */
	}
	stats_add(STM32_PKA_OP_SIGN, t0, rc);
	k_mutex_unlock(&pka_lock);
	return rc;
}

int stm32_pka_p256_ecdsa_verify(const uint8_t *qx, const uint8_t *qy, const uint8_t *hash,
				const uint8_t *r, const uint8_t *s)
{
	int64_t t0;
	int rc;

	k_mutex_lock(&pka_lock, K_FOREVER);
	t0 = k_uptime_ticks();
	rc = pka_bring_up();
	if (rc == 0 && (be_is_zero(r) || be_is_zero(s) || !be_less(r, p256_n) ||
			!be_less(s, p256_n))) {
		rc = -EBADMSG;
	} else if (rc == 0) {
		PKA_ECDSAVerifInTypeDef in = {
			.primeOrderSize = SZ,
			.modulusSize = SZ,
			.coefSign = P256_A_SIGN,
			.coef = p256_abs_a,
			.modulus = p256_p,
			.basePointX = p256_gx,
			.basePointY = p256_gy,
			.pPubKeyCurvePtX = qx,
			.pPubKeyCurvePtY = qy,
			.RSign = r,
			.SSign = s,
			.hash = hash,
			.primeOrder = p256_n,
		};

		rc = hal_to_errno(HAL_PKA_ECDSAVerif(&hpka, &in, CONFIG_STM32_PKA_TIMEOUT_MS));
		if (rc == 0 && HAL_PKA_ECDSAVerif_IsValidSignature(&hpka) != 1U) {
			rc = -EBADMSG;
		}
	}
	stats_add(STM32_PKA_OP_VERIFY, t0, rc);
	k_mutex_unlock(&pka_lock);
	return rc;
}

#if defined(CONFIG_STM32_PKA_ECP_P256)
/* Runtime switch of the ecp.c hook; kept in .data so on/off images share one layout. */
bool stm32_pka_ecp_enabled __attribute__((section(".data.stm32_pka_ecp_enabled"))) =
	IS_ENABLED(CONFIG_STM32_PKA_ECP_P256_DEFAULT_ON);
#endif

int stm32_pka_p256_muladd(const uint8_t *k, const uint8_t *px, const uint8_t *py,
			  const uint8_t *m, const uint8_t *qx, const uint8_t *qy,
			  uint8_t *rx, uint8_t *ry)
{
	/* Z = 1: the ladder takes projective inputs, the points here are affine. */
	static const uint8_t one[SZ] = { [SZ - 1] = 1 };
	int64_t t0;
	int rc;

	k_mutex_lock(&pka_lock, K_FOREVER);
	t0 = k_uptime_ticks();
	rc = pka_bring_up();
	if (rc == 0) {
		PKA_ECCDoubleBaseLadderInTypeDef in = {
			.primeOrderSize = SZ,
			.modulusSize = SZ,
			.coefSign = P256_A_SIGN,
			.coefA = p256_abs_a,
			.modulus = p256_p,
			.integerK = k,
			.integerM = m,
			.basePointX1 = px,
			.basePointY1 = py,
			.basePointZ1 = one,
			.basePointX2 = qx,
			.basePointY2 = qy,
			.basePointZ2 = one,
		};
		/* Output slots are affine (PKA_ECC_DOUBLE_LADDER_OUT_RESULT_X/Y, stm32u585xx.h). */
		PKA_ECCDoubleBaseLadderOutTypeDef out = { .ptX = rx, .ptY = ry };

		rc = hal_to_errno(HAL_PKA_ECCDoubleBaseLadder(&hpka, &in, CONFIG_STM32_PKA_TIMEOUT_MS));
		if (rc == 0) {
			HAL_PKA_ECCDoubleBaseLadder_GetResult(&hpka, &out);
		} else if (rc == -EAGAIN) {
			rc = -EINVAL; /* e.g. the sum is the point at infinity */
		}
		HAL_PKA_RAMReset(&hpka); /* SPAKE2+ scalars are session secrets */
	}
	stats_add(STM32_PKA_OP_MULADD, t0, rc);
	k_mutex_unlock(&pka_lock);
	return rc;
}
