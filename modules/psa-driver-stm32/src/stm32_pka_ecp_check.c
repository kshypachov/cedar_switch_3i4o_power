/*
 * 'pka ecp check N': the ecp.c hook (patches/tf-psa-crypto/ecp-p256-stm32-pka.patch)
 * against the software path, through the same mbedtls_ecp_mul()/mbedtls_ecp_muladd()
 * calls Matter's SPAKE2+ makes. Random scalars and points; each result computed with
 * stm32_pka_ecp_enabled off (software) and on (PKA) and compared point by point.
 */

/*
 * mbedTLS 4.x: ECP and bignum declarations are private (as in Matter's CHIPCryptoPALPSA.cpp).
 * Must precede every include: psa/crypto.h already pulls in mbedtls/private/ecp.h.
 */
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS

#include <errno.h>
#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include <psa/crypto.h>
#include <mbedtls/private/bignum.h>
#include <mbedtls/private/ecp.h>

#include <stm32_pka.h>

static int check_rng(void *ctx, unsigned char *out, size_t len)
{
	ARG_UNUSED(ctx);
	return psa_generate_random(out, len) == PSA_SUCCESS ? 0 : -1;
}

static int64_t now_us(void)
{
	return k_ticks_to_us_near64(k_uptime_ticks());
}

int stm32_pka_ecp_check(const struct shell *sh, uint32_t n)
{
	mbedtls_ecp_group grp;
	mbedtls_ecp_point P, Q, R_sw, R_hw, S_sw, S_hw;
	mbedtls_mpi a, b, m, k;
	bool saved = stm32_pka_ecp_enabled;
	int64_t t_mul_sw = 0, t_mul_hw = 0, t_add_sw = 0, t_add_hw = 0, t0;
	int mismatches = 0, ret = 0;
	uint32_t i;

	mbedtls_ecp_group_init(&grp);
	mbedtls_ecp_point_init(&P);
	mbedtls_ecp_point_init(&Q);
	mbedtls_ecp_point_init(&R_sw);
	mbedtls_ecp_point_init(&R_hw);
	mbedtls_ecp_point_init(&S_sw);
	mbedtls_ecp_point_init(&S_hw);
	mbedtls_mpi_init(&a);
	mbedtls_mpi_init(&b);
	mbedtls_mpi_init(&m);
	mbedtls_mpi_init(&k);

	ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
	for (i = 0; ret == 0 && i < n; i++) {
		/* Fresh points P = a*G, Q = b*G and scalars m, k every round (software). */
		stm32_pka_ecp_enabled = false;
		ret = mbedtls_ecp_gen_keypair(&grp, &a, &P, check_rng, NULL);
		ret = ret ? ret : mbedtls_ecp_gen_keypair(&grp, &b, &Q, check_rng, NULL);
		ret = ret ? ret : mbedtls_ecp_gen_privkey(&grp, &m, check_rng, NULL);
		ret = ret ? ret : mbedtls_ecp_gen_privkey(&grp, &k, check_rng, NULL);
		if (ret != 0) {
			break;
		}

		t0 = now_us();
		ret = mbedtls_ecp_mul(&grp, &R_sw, &m, &P, check_rng, NULL);
		t_mul_sw += now_us() - t0;
		t0 = now_us();
		ret = ret ? ret : mbedtls_ecp_muladd(&grp, &S_sw, &m, &P, &k, &Q);
		t_add_sw += now_us() - t0;

		stm32_pka_ecp_enabled = true;
		t0 = now_us();
		ret = ret ? ret : mbedtls_ecp_mul(&grp, &R_hw, &m, &P, check_rng, NULL);
		t_mul_hw += now_us() - t0;
		t0 = now_us();
		ret = ret ? ret : mbedtls_ecp_muladd(&grp, &S_hw, &m, &P, &k, &Q);
		t_add_hw += now_us() - t0;
		if (ret != 0) {
			break;
		}

		if (mbedtls_ecp_point_cmp(&R_sw, &R_hw) != 0 || mbedtls_ecp_point_cmp(&S_sw, &S_hw) != 0) {
			mismatches++;
		}
	}
	stm32_pka_ecp_enabled = saved;

	if (ret != 0) {
		shell_error(sh, "pka ecp check: mbedtls error -0x%04x at round %u", (unsigned)-ret, i);
	} else {
		shell_print(sh,
			    "pka ecp check n=%u mismatches=%d avg us: mul sw=%lld hw=%lld muladd sw=%lld hw=%lld",
			    n, mismatches, t_mul_sw / n, t_mul_hw / n, t_add_sw / n, t_add_hw / n);
		shell_print(sh, "pka ecp check: %s", mismatches == 0 ? "PASS" : "FAIL");
	}

	mbedtls_mpi_free(&k);
	mbedtls_mpi_free(&m);
	mbedtls_mpi_free(&b);
	mbedtls_mpi_free(&a);
	mbedtls_ecp_point_free(&S_hw);
	mbedtls_ecp_point_free(&S_sw);
	mbedtls_ecp_point_free(&R_hw);
	mbedtls_ecp_point_free(&R_sw);
	mbedtls_ecp_point_free(&Q);
	mbedtls_ecp_point_free(&P);
	mbedtls_ecp_group_free(&grp);
	return (ret != 0 || mismatches != 0) ? -EIO : 0;
}
