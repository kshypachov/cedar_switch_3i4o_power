/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * PSA Crypto key derivation and Zephyr randomness for web-auth.
 *
 * Why the derivation runs on its own thread. web-auth calls the platform's
 * kdf() on the thread of the request, and for setup, login and the check of a
 * current password that thread is the HTTP server's. That thread is
 * cooperative - http_server_core.c creates it at K_PRIO_COOP(NUM_COOP - 1), with
 * no option to change it - and a cooperative thread is never preempted by
 * another thread. Measured on the board in P2 (reports/p2): a 10000-iteration
 * PBKDF2 run directly on it took longer than 18 s, and for all of that time
 * the network stack's own threads, Matter and every probe from a second client
 * stood still.
 *
 * So web_auth_psa_pbkdf2() hands the work to a preemptive thread of low
 * priority and waits for it on a semaphore. Waiting blocks the HTTP server's
 * thread, which lets the scheduler run everything else; the derivation takes
 * the CPU only when nothing more urgent is ready. The HTTP server itself still
 * answers no other request until the derivation finishes - its one thread is
 * waiting - so the iteration count stays a latency budget
 * (CONFIG_WEB_AUTH_PBKDF2_ITERATIONS, measured with `web_auth kdf`).
 *
 * The worker's stack is in SRAM, not PSRAM: libweb_auth.a is deliberately not
 * in src/helpers/psram_sections.ld, so the SHA-256 state the derivation churns
 * through lives in the faster memory.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <psa/crypto.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/shell/shell.h>

#include <web_auth/web_auth.h>
#include <web_auth/web_auth_adapters.h>

/* -- the derivation itself ------------------------------------------------- */

static int derive(const uint8_t *password, size_t password_len, const uint8_t *salt,
		  size_t salt_len, uint32_t iterations, uint8_t *out, size_t out_len)
{
	psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
	psa_status_t status = psa_crypto_init();

	if (status == PSA_SUCCESS) {
		status = psa_key_derivation_setup(&op, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256));
	}
	if (status == PSA_SUCCESS) {
		status = psa_key_derivation_input_integer(&op, PSA_KEY_DERIVATION_INPUT_COST,
							  iterations);
	}
	if (status == PSA_SUCCESS) {
		status = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, salt,
							salt_len);
	}
	if (status == PSA_SUCCESS) {
		status = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_PASSWORD,
							password, password_len);
	}
	if (status == PSA_SUCCESS) {
		status = psa_key_derivation_output_bytes(&op, out, out_len);
	}
	(void)psa_key_derivation_abort(&op);

	if (status != PSA_SUCCESS) {
		memset(out, 0, out_len);
		return -EIO;
	}

	return 0;
}

/* -- the worker ------------------------------------------------------------ */

struct job {
	const uint8_t *password;
	size_t password_len;
	const uint8_t *salt;
	size_t salt_len;
	uint32_t iterations;
	uint8_t *out;
	size_t out_len;
	int result;
};

K_THREAD_STACK_DEFINE(kdf_stack, CONFIG_WEB_AUTH_KDF_STACK_SIZE);
static struct k_thread kdf_thread;
static K_SEM_DEFINE(kdf_request, 0, 1);
static K_SEM_DEFINE(kdf_done, 0, 1);
/* One derivation at a time; callers queue here, not in the worker. */
static K_MUTEX_DEFINE(kdf_lock);
static struct job *pending;
static bool started;

static void kdf_main(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		k_sem_take(&kdf_request, K_FOREVER);
		pending->result = derive(pending->password, pending->password_len, pending->salt,
					 pending->salt_len, pending->iterations, pending->out,
					 pending->out_len);
		k_sem_give(&kdf_done);
	}
}

static void start_worker(void)
{
	if (started) {
		return;
	}
	k_thread_create(&kdf_thread, kdf_stack, K_THREAD_STACK_SIZEOF(kdf_stack), kdf_main, NULL,
			NULL, NULL, K_PRIO_PREEMPT(CONFIG_WEB_AUTH_KDF_THREAD_PRIORITY), 0, K_NO_WAIT);
	k_thread_name_set(&kdf_thread, "web_auth_kdf");
	started = true;
}

int web_auth_psa_pbkdf2(const uint8_t *password, size_t password_len, const uint8_t *salt,
			size_t salt_len, uint32_t iterations, uint8_t *out, size_t out_len)
{
	struct job job = {
		.password = password,
		.password_len = password_len,
		.salt = salt,
		.salt_len = salt_len,
		.iterations = iterations,
		.out = out,
		.out_len = out_len,
		.result = -EIO,
	};

	k_mutex_lock(&kdf_lock, K_FOREVER);
	start_worker();
	pending = &job;
	k_sem_give(&kdf_request);
	/* Pends, so a cooperative caller yields the CPU for the whole wait. */
	k_sem_take(&kdf_done, K_FOREVER);
	pending = NULL;
	k_mutex_unlock(&kdf_lock);

	return job.result;
}

int web_auth_csrand(uint8_t *buf, size_t len)
{
	return sys_csrand_get(buf, len);
}

int64_t web_auth_uptime_ms(void)
{
	return k_uptime_get();
}

/* -- measurement ------------------------------------------------------------ */

#if defined(CONFIG_WEB_AUTH_SHELL)
static int cmd_kdf(const struct shell *sh, size_t argc, char **argv)
{
	static const uint8_t password[] = "measure-only-password";
	static const uint8_t salt[WEB_AUTH_SALT_LEN] = {1, 2, 3, 4, 5, 6, 7, 8};
	uint8_t out[WEB_AUTH_DERIVED_LEN];
	uint32_t iterations = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 1000U;
	int64_t start;
	int64_t elapsed;
	int rc;

	if (iterations == 0U || iterations > CONFIG_WEB_AUTH_PBKDF2_MAX_ITERATIONS) {
		shell_error(sh, "iterations: 1..%d", CONFIG_WEB_AUTH_PBKDF2_MAX_ITERATIONS);
		return -EINVAL;
	}
	start = k_uptime_get();
	rc = web_auth_psa_pbkdf2(password, sizeof(password) - 1, salt, sizeof(salt), iterations,
				 out, sizeof(out));
	elapsed = k_uptime_get() - start;
	shell_print(sh, "pbkdf2-hmac-sha256 %u iterations: %lld ms (rc %d, configured %d)",
		    iterations, (long long)elapsed, rc, CONFIG_WEB_AUTH_PBKDF2_ITERATIONS);

	return rc;
}

SHELL_STATIC_SUBCMD_SET_CREATE(web_auth_cmds,
			       SHELL_CMD_ARG(kdf, NULL,
					     "Time one derivation on the production path: kdf [iterations]",
					     cmd_kdf, 1, 1),
			       SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(web_auth, &web_auth_cmds, "web-auth diagnostics", NULL);
#endif
