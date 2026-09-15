/*
 * Shell 'pka': known-answer tests and a benchmark of the PKA P-256 primitives.
 *
 * Vectors: RFC 6979 A.2.5 (P-256, SHA-256, messages "sample" and "test"; the
 * nonces k are given there, so r and s are bit-exact). The ECDH peer and shared
 * secret were generated and cross-checked on the host with python
 * cryptography 49.0.0 (reports/crypto/pka/README.md).
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include <stm32_pka.h>

/* RFC 6979 A.2.5: private key x, public key U = xG */
static const uint8_t kat_d1[32] = {
	0xc9, 0xaf, 0xa9, 0xd8, 0x45, 0xba, 0x75, 0x16, 0x6b, 0x5c, 0x21, 0x57, 0x67, 0xb1, 0xd6, 0x93,
	0x4e, 0x50, 0xc3, 0xdb, 0x36, 0xe8, 0x9b, 0x12, 0x7b, 0x8a, 0x62, 0x2b, 0x12, 0x0f, 0x67, 0x21,
};
static const uint8_t kat_ux[32] = {
	0x60, 0xfe, 0xd4, 0xba, 0x25, 0x5a, 0x9d, 0x31, 0xc9, 0x61, 0xeb, 0x74, 0xc6, 0x35, 0x6d, 0x68,
	0xc0, 0x49, 0xb8, 0x92, 0x3b, 0x61, 0xfa, 0x6c, 0xe6, 0x69, 0x62, 0x2e, 0x60, 0xf2, 0x9f, 0xb6,
};
static const uint8_t kat_uy[32] = {
	0x79, 0x03, 0xfe, 0x10, 0x08, 0xb8, 0xbc, 0x99, 0xa4, 0x1a, 0xe9, 0xe9, 0x56, 0x28, 0xbc, 0x64,
	0xf2, 0xf1, 0xb2, 0x0c, 0x2d, 0x7e, 0x9f, 0x51, 0x77, 0xa3, 0xc2, 0x94, 0xd4, 0x46, 0x22, 0x99,
};

/* SHA-256("sample"), RFC 6979 k, r, s */
static const uint8_t kat_hash_sample[32] = {
	0xaf, 0x2b, 0xdb, 0xe1, 0xaa, 0x9b, 0x6e, 0xc1, 0xe2, 0xad, 0xe1, 0xd6, 0x94, 0xf4, 0x1f, 0xc7,
	0x1a, 0x83, 0x1d, 0x02, 0x68, 0xe9, 0x89, 0x15, 0x62, 0x11, 0x3d, 0x8a, 0x62, 0xad, 0xd1, 0xbf,
};
static const uint8_t kat_k_sample[32] = {
	0xa6, 0xe3, 0xc5, 0x7d, 0xd0, 0x1a, 0xbe, 0x90, 0x08, 0x65, 0x38, 0x39, 0x83, 0x55, 0xdd, 0x4c,
	0x3b, 0x17, 0xaa, 0x87, 0x33, 0x82, 0xb0, 0xf2, 0x4d, 0x61, 0x29, 0x49, 0x3d, 0x8a, 0xad, 0x60,
};
static const uint8_t kat_r_sample[32] = {
	0xef, 0xd4, 0x8b, 0x2a, 0xac, 0xb6, 0xa8, 0xfd, 0x11, 0x40, 0xdd, 0x9c, 0xd4, 0x5e, 0x81, 0xd6,
	0x9d, 0x2c, 0x87, 0x7b, 0x56, 0xaa, 0xf9, 0x91, 0xc3, 0x4d, 0x0e, 0xa8, 0x4e, 0xaf, 0x37, 0x16,
};
static const uint8_t kat_s_sample[32] = {
	0xf7, 0xcb, 0x1c, 0x94, 0x2d, 0x65, 0x7c, 0x41, 0xd4, 0x36, 0xc7, 0xa1, 0xb6, 0xe2, 0x9f, 0x65,
	0xf3, 0xe9, 0x00, 0xdb, 0xb9, 0xaf, 0xf4, 0x06, 0x4d, 0xc4, 0xab, 0x2f, 0x84, 0x3a, 0xcd, 0xa8,
};

/* SHA-256("test"), RFC 6979 k, r, s */
static const uint8_t kat_hash_test[32] = {
	0x9f, 0x86, 0xd0, 0x81, 0x88, 0x4c, 0x7d, 0x65, 0x9a, 0x2f, 0xea, 0xa0, 0xc5, 0x5a, 0xd0, 0x15,
	0xa3, 0xbf, 0x4f, 0x1b, 0x2b, 0x0b, 0x82, 0x2c, 0xd1, 0x5d, 0x6c, 0x15, 0xb0, 0xf0, 0x0a, 0x08,
};
static const uint8_t kat_k_test[32] = {
	0xd1, 0x6b, 0x6a, 0xe8, 0x27, 0xf1, 0x71, 0x75, 0xe0, 0x40, 0x87, 0x1a, 0x1c, 0x7e, 0xc3, 0x50,
	0x01, 0x92, 0xc4, 0xc9, 0x26, 0x77, 0x33, 0x6e, 0xc2, 0x53, 0x7a, 0xca, 0xee, 0x00, 0x08, 0xe0,
};
static const uint8_t kat_r_test[32] = {
	0xf1, 0xab, 0xb0, 0x23, 0x51, 0x83, 0x51, 0xcd, 0x71, 0xd8, 0x81, 0x56, 0x7b, 0x1e, 0xa6, 0x63,
	0xed, 0x3e, 0xfc, 0xf6, 0xc5, 0x13, 0x2b, 0x35, 0x4f, 0x28, 0xd3, 0xb0, 0xb7, 0xd3, 0x83, 0x67,
};
static const uint8_t kat_s_test[32] = {
	0x01, 0x9f, 0x41, 0x13, 0x74, 0x2a, 0x2b, 0x14, 0xbd, 0x25, 0x92, 0x6b, 0x49, 0xc6, 0x49, 0x15,
	0x5f, 0x26, 0x7e, 0x60, 0xd3, 0x81, 0x4b, 0x4c, 0x0c, 0xc8, 0x42, 0x50, 0xe4, 0x6f, 0x00, 0x83,
};

/* ECDH: d2 = SHA-256("cedar pka ecdh peer") mod n, Q2 = d2 * G, shared = x(d1 * Q2) */
static const uint8_t kat_ecdh_d2[32] = {
	0x05, 0x53, 0xa1, 0x4f, 0x93, 0x09, 0x91, 0xfb, 0xb3, 0x7f, 0x7d, 0xfe, 0xbd, 0x54, 0x17, 0x23,
	0x5a, 0x59, 0xb9, 0xfc, 0xb1, 0xae, 0xac, 0x33, 0x55, 0x54, 0xdb, 0x29, 0x3f, 0x82, 0xe1, 0x43,
};
static const uint8_t kat_ecdh_q2x[32] = {
	0x4d, 0xce, 0x55, 0x41, 0xf0, 0xb9, 0x09, 0xc6, 0xcd, 0xa9, 0x69, 0x14, 0xd0, 0x9a, 0x98, 0xde,
	0x6d, 0x48, 0xb4, 0xc1, 0xf5, 0x41, 0x1a, 0x77, 0xc8, 0x6a, 0x33, 0x45, 0x25, 0xbb, 0xa2, 0xc3,
};
static const uint8_t kat_ecdh_q2y[32] = {
	0xef, 0x51, 0x0f, 0x91, 0x17, 0x93, 0x02, 0xcf, 0x89, 0xfb, 0xc9, 0xf8, 0x15, 0x53, 0x0b, 0xb9,
	0x51, 0xfd, 0x8f, 0xdd, 0x54, 0x0b, 0x5b, 0x5c, 0xea, 0xe0, 0xf8, 0x04, 0xc8, 0x0e, 0xa7, 0xc9,
};
static const uint8_t kat_ecdh_shared[32] = {
	0xa6, 0x65, 0xe7, 0xec, 0x5f, 0xfd, 0x5c, 0x40, 0xe7, 0x6e, 0x5f, 0x1a, 0x12, 0x7e, 0x0f, 0xe2,
	0xa1, 0xcb, 0xd4, 0x59, 0xfe, 0xe7, 0x73, 0x76, 0x07, 0x3b, 0xa9, 0x7e, 0x24, 0x94, 0xf0, 0xa5,
};

/* muladd: m = SHA-256("cedar pka muladd m") mod n, k = SHA-256("cedar pka muladd n") mod n,
 * R = m*U + k*Q2 = (m*d1 + k*d2)*G (host, pure-Python affine arithmetic, cross-checked with
 * python cryptography 49.0.0). */
static const uint8_t kat_muladd_m[32] = {
	0xe1, 0x8f, 0xde, 0xc9, 0xd7, 0x2e, 0x02, 0xca, 0xae, 0x6f, 0x72, 0x66, 0xd9, 0xfa, 0xde, 0x4f,
	0x32, 0x26, 0x58, 0x88, 0x5a, 0x5f, 0xbf, 0xdd, 0x7b, 0x57, 0x87, 0x1c, 0xfc, 0x74, 0x9b, 0x44,
};
static const uint8_t kat_muladd_k[32] = {
	0x94, 0xd3, 0xce, 0x3e, 0xb6, 0x5c, 0x51, 0x54, 0xdc, 0xae, 0x7d, 0x96, 0xe9, 0x7c, 0xaa, 0xca,
	0x73, 0xf8, 0x02, 0x5e, 0xc4, 0x3b, 0x5e, 0x9f, 0xbf, 0xaa, 0x7f, 0xdc, 0x31, 0x1f, 0xaa, 0xfa,
};
static const uint8_t kat_muladd_rx[32] = {
	0x66, 0x40, 0x07, 0xb0, 0x03, 0xe0, 0xf8, 0xcf, 0x57, 0x62, 0x32, 0x29, 0x23, 0x75, 0xdf, 0x56,
	0x01, 0xd2, 0xb7, 0x59, 0x3e, 0x79, 0x1e, 0xd2, 0xa4, 0x52, 0xac, 0xf1, 0x23, 0xca, 0x4a, 0x1c,
};
static const uint8_t kat_muladd_ry[32] = {
	0x01, 0xd5, 0xa6, 0x1d, 0xeb, 0x23, 0x46, 0xd4, 0x2d, 0x21, 0x1b, 0x70, 0xdb, 0xc5, 0xaa, 0x07,
	0x61, 0x94, 0x30, 0x7e, 0xad, 0x5a, 0x0b, 0x78, 0x71, 0x96, 0xfa, 0xfc, 0xd6, 0x68, 0x02, 0xdc,
};

struct kat_ctx {
	const struct shell *sh;
	int failed;
};

static void kat_check(struct kat_ctx *c, const char *name, bool ok, int rc, int64_t us)
{
	shell_print(c->sh, "  %-40s %s (rc %d, %lld us)", name, ok ? "PASS" : "FAIL", rc, us);
	if (!ok) {
		c->failed++;
	}
}

static int64_t now_us(void)
{
	return k_ticks_to_us_near64(k_uptime_ticks());
}

static void kat_sign(struct kat_ctx *c, const char *name, const uint8_t *hash, const uint8_t *k,
		     const uint8_t *r_exp, const uint8_t *s_exp)
{
	uint8_t r[32], s[32];
	uint8_t bad[32];
	int64_t t0 = now_us();
	int rc = stm32_pka_p256_ecdsa_sign(kat_d1, k, hash, r, s);
	int64_t t1 = now_us();

	kat_check(c, name, rc == 0 && memcmp(r, r_exp, 32) == 0 && memcmp(s, s_exp, 32) == 0, rc,
		  t1 - t0);

	t0 = now_us();
	rc = stm32_pka_p256_ecdsa_verify(kat_ux, kat_uy, hash, r_exp, s_exp);
	kat_check(c, "  verify (valid)", rc == 0, rc, now_us() - t0);

	memcpy(bad, s_exp, sizeof(bad));
	bad[31] ^= 0x01;
	rc = stm32_pka_p256_ecdsa_verify(kat_ux, kat_uy, hash, r_exp, bad);
	kat_check(c, "  verify (s tampered) -> -EBADMSG", rc == -EBADMSG, rc, 0);
}

static int cmd_pka_kat(const struct shell *sh, size_t argc, char **argv)
{
	struct kat_ctx c = { .sh = sh };
	uint8_t x[32], y[32], bad[32];
	int64_t t0;
	int rc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	t0 = now_us();
	rc = stm32_pka_init();
	kat_check(&c, "init", rc == 0, rc, now_us() - t0);
	if (rc != 0) {
		return -EIO;
	}

	t0 = now_us();
	rc = stm32_pka_p256_mul(kat_d1, NULL, NULL, x, y);
	kat_check(&c, "mul d1*G == U (RFC 6979)",
		  rc == 0 && memcmp(x, kat_ux, 32) == 0 && memcmp(y, kat_uy, 32) == 0, rc,
		  now_us() - t0);

	t0 = now_us();
	rc = stm32_pka_p256_point_check(kat_ux, kat_uy);
	kat_check(&c, "point check U on curve", rc == 0, rc, now_us() - t0);

	memcpy(bad, kat_uy, sizeof(bad));
	bad[31] ^= 0x01;
	rc = stm32_pka_p256_point_check(kat_ux, bad);
	kat_check(&c, "point check U' off curve -> -EINVAL", rc == -EINVAL, rc, 0);

	kat_sign(&c, "sign \"sample\" == RFC 6979 r,s", kat_hash_sample, kat_k_sample,
		 kat_r_sample, kat_s_sample);
	kat_sign(&c, "sign \"test\" == RFC 6979 r,s", kat_hash_test, kat_k_test, kat_r_test,
		 kat_s_test);

	t0 = now_us();
	rc = stm32_pka_p256_mul(kat_d1, kat_ecdh_q2x, kat_ecdh_q2y, x, y);
	kat_check(&c, "ECDH x(d1*Q2) == shared", rc == 0 && memcmp(x, kat_ecdh_shared, 32) == 0,
		  rc, now_us() - t0);

	t0 = now_us();
	rc = stm32_pka_p256_mul(kat_ecdh_d2, kat_ux, kat_uy, x, y);
	kat_check(&c, "ECDH x(d2*U) == shared", rc == 0 && memcmp(x, kat_ecdh_shared, 32) == 0,
		  rc, now_us() - t0);


	t0 = now_us();
	rc = stm32_pka_p256_muladd(kat_muladd_m, kat_ux, kat_uy, kat_muladd_k, kat_ecdh_q2x,
				   kat_ecdh_q2y, x, y);
	kat_check(&c, "muladd m*U + k*Q2 == R",
		  rc == 0 && memcmp(x, kat_muladd_rx, 32) == 0 && memcmp(y, kat_muladd_ry, 32) == 0,
		  rc, now_us() - t0);

	shell_print(sh, "pka kat: %s (%d failed)", c.failed ? "FAIL" : "PASS", c.failed);
	return c.failed ? -EIO : 0;
}

static int cmd_pka_bench(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t n = argc > 1 ? strtoul(argv[1], NULL, 10) : 10;
	uint8_t x[32], y[32], r[32], s[32];
	int64_t t0, t_mulg = 0, t_mulp = 0, t_sign = 0, t_verify = 0, t_muladd = 0;
	int err = 0;

	if (n == 0) {
		n = 1;
	}
	for (uint32_t i = 0; i < n; i++) {
		t0 = now_us();
		err |= stm32_pka_p256_mul(kat_d1, NULL, NULL, x, y);
		t_mulg += now_us() - t0;

		t0 = now_us();
		err |= stm32_pka_p256_mul(kat_ecdh_d2, kat_ux, kat_uy, x, y);
		t_mulp += now_us() - t0;

		t0 = now_us();
		err |= stm32_pka_p256_ecdsa_sign(kat_d1, kat_k_sample, kat_hash_sample, r, s);
		t_sign += now_us() - t0;

		t0 = now_us();
		err |= stm32_pka_p256_ecdsa_verify(kat_ux, kat_uy, kat_hash_sample, r, s);
		t_verify += now_us() - t0;

		t0 = now_us();
		err |= stm32_pka_p256_muladd(kat_muladd_m, kat_ux, kat_uy, kat_muladd_k,
					     kat_ecdh_q2x, kat_ecdh_q2y, x, y);
		t_muladd += now_us() - t0;
	}
	shell_print(sh, "pka bench n=%u err=%d avg us: mul_G=%lld mul_P=%lld sign=%lld verify=%lld muladd=%lld",
		    n, err, t_mulg / n, t_mulp / n, t_sign / n, t_verify / n, t_muladd / n);
	return err ? -EIO : 0;
}

static int cmd_pka_stats(const struct shell *sh, size_t argc, char **argv)
{
	struct stm32_pka_op_stats st[STM32_PKA_OP_COUNT];

	if (argc > 1 && strcmp(argv[1], "reset") == 0) {
		stm32_pka_stats_reset();
		shell_print(sh, "pka stats reset");
		return 0;
	}
	stm32_pka_stats_get(st);
	for (int i = 0; i < STM32_PKA_OP_COUNT; i++) {
		shell_print(sh, "pka stat %-12s calls=%u errors=%u total_us=%llu avg_us=%llu max_us=%u",
			    stm32_pka_op_name(i), st[i].calls, st[i].errors, st[i].total_us,
			    st[i].calls ? st[i].total_us / st[i].calls : 0, st[i].max_us);
	}
	return 0;
}

#if defined(CONFIG_PSA_CRYPTO_DRIVER_STM32_PKA)
#include <stm32_pka_psa.h>

static int cmd_pka_psa(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		if (strcmp(argv[1], "on") == 0) {
			stm32_pka_psa_enabled = true;
		} else if (strcmp(argv[1], "off") == 0) {
			stm32_pka_psa_enabled = false;
		} else {
			shell_error(sh, "usage: pka psa [on|off]");
			return -EINVAL;
		}
	}
	shell_print(sh, "pka psa: %s", stm32_pka_psa_enabled ? "on" : "off");
	return 0;
}
#else
/* SHELL_COND_CMD_ARG still names the handler when the option is off. */
#define cmd_pka_psa NULL
#endif

#if defined(CONFIG_STM32_PKA_ECP_P256)
int stm32_pka_ecp_check(const struct shell *sh, uint32_t n);

static int cmd_pka_ecp(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "check") == 0) {
		uint32_t n = argc > 2 ? strtoul(argv[2], NULL, 10) : 10;

		return stm32_pka_ecp_check(sh, n ? n : 1);
	}
	if (argc > 1) {
		if (strcmp(argv[1], "on") == 0) {
			stm32_pka_ecp_enabled = true;
		} else if (strcmp(argv[1], "off") == 0) {
			stm32_pka_ecp_enabled = false;
		} else {
			shell_error(sh, "usage: pka ecp [on|off|check [n]]");
			return -EINVAL;
		}
	}
	shell_print(sh, "pka ecp: %s", stm32_pka_ecp_enabled ? "on" : "off");
	return 0;
}
#else
#define cmd_pka_ecp NULL
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(sub_pka,
	SHELL_CMD(kat, NULL, "Known-answer tests (RFC 6979 P-256, ECDH)", cmd_pka_kat),
	SHELL_COND_CMD_ARG(CONFIG_PSA_CRYPTO_DRIVER_STM32_PKA, psa, NULL,
			   "psa [on|off]: route PSA P-256 calls to the PKA", cmd_pka_psa, 1, 1),
	SHELL_COND_CMD_ARG(CONFIG_STM32_PKA_ECP_P256, ecp, NULL,
			   "ecp [on|off|check [n]]: builtin ECP P-256 (SPAKE2+) on the PKA", cmd_pka_ecp, 1, 2),
	SHELL_CMD_ARG(bench, NULL, "bench [n]: average time of mul/sign/verify", cmd_pka_bench, 1, 1),
	SHELL_CMD_ARG(stats, NULL, "stats [reset]: per-operation counters", cmd_pka_stats, 1, 1),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(pka, &sub_pka, "STM32 PKA (public key accelerator)", NULL);
