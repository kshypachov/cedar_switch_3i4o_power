/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * web-auth unit tests. Everything the module needs from outside is faked:
 * the clock moves only when a test moves it, randomness is a counter, the key
 * derivation is a deterministic mix that is cheap and still different for
 * every password, salt and count, and the store is a buffer whose failures a
 * test switches on. The concurrency cases run without threads: the fake key
 * derivation can call back into the module, which is exactly the window in
 * which a second request would arrive on the device.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include <web_auth/web_auth.h>

#define PASSWORD     "correct horse battery"
#define NEW_PASSWORD "a different staple"

/* -- fakes ------------------------------------------------------------- */

static int64_t fake_now;
static uint32_t random_counter;
static bool random_fails;

static int kdf_calls;
static uint32_t kdf_last_iterations;
static bool kdf_fails;
static void (*kdf_hook)(void);

static uint8_t store[128];
static size_t store_len;
static bool store_present;
static int load_rc;
static bool save_fails;
static int save_calls;

static int64_t fake_clock(void)
{
	return fake_now;
}

static int fake_random(uint8_t *buf, size_t len)
{
	if (random_fails) {
		return -EIO;
	}
	for (size_t i = 0; i < len; i++) {
		random_counter = random_counter * 1103515245U + 12345U;
		buf[i] = (uint8_t)(random_counter >> 16);
	}
	return 0;
}

static uint64_t mix(uint64_t h, const uint8_t *data, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		h ^= data[i];
		h *= 0x100000001b3ULL;
	}
	return h;
}

static void fake_kdf_bytes(const uint8_t *password, size_t password_len, const uint8_t *salt,
			   size_t salt_len, uint32_t iterations, uint8_t *out, size_t out_len)
{
	uint64_t h = 0xcbf29ce484222325ULL;
	uint8_t iter[4];

	sys_put_le32(iterations, iter);
	h = mix(h, password, password_len);
	h = mix(h, (const uint8_t *)"|", 1);
	h = mix(h, salt, salt_len);
	h = mix(h, iter, sizeof(iter));
	for (size_t i = 0; i < out_len; i++) {
		uint8_t idx = (uint8_t)i;

		h = mix(h, &idx, 1);
		out[i] = (uint8_t)(h >> 32);
	}
}

static int fake_kdf(const uint8_t *password, size_t password_len, const uint8_t *salt,
		    size_t salt_len, uint32_t iterations, uint8_t *out, size_t out_len)
{
	kdf_calls++;
	kdf_last_iterations = iterations;
	if (kdf_hook != NULL) {
		void (*hook)(void) = kdf_hook;

		/* One shot, so a hook that calls back into the module does not
		 * recurse through its own derivation. */
		kdf_hook = NULL;
		hook();
	}
	if (kdf_fails) {
		return -EIO;
	}
	fake_kdf_bytes(password, password_len, salt, salt_len, iterations, out, out_len);
	return 0;
}

static int fake_load(uint8_t *buf, size_t cap, size_t *out_len)
{
	if (load_rc != 0) {
		return load_rc;
	}
	if (!store_present) {
		return -ENOENT;
	}
	if (store_len > cap) {
		return -ENOSPC;
	}
	memcpy(buf, store, store_len);
	*out_len = store_len;
	return 0;
}

static int fake_save(const uint8_t *buf, size_t len)
{
	save_calls++;
	if (save_fails) {
		return -EIO;
	}
	memcpy(store, buf, len);
	store_len = len;
	store_present = true;
	return 0;
}

static const struct web_auth_platform platform = {
	.random = fake_random,
	.now_ms = fake_clock,
	.kdf = fake_kdf,
	.load_verifier = fake_load,
	.save_verifier = fake_save,
};

static void reset_fakes(void)
{
	fake_now = 1000;
	random_counter = 7;
	random_fails = false;
	kdf_calls = 0;
	kdf_last_iterations = 0;
	kdf_fails = false;
	kdf_hook = NULL;
	memset(store, 0, sizeof(store));
	store_len = 0;
	store_present = false;
	load_rc = 0;
	save_fails = false;
	save_calls = 0;
}

/* -- helpers ----------------------------------------------------------- */

static const struct web_auth_peer peer_a = {.family = 4, .addr = {192, 168, 88, 17}};
static const struct web_auth_peer peer_b = {.family = 4, .addr = {192, 168, 88, 18}};

static struct web_auth_peer peer_n(uint8_t n)
{
	struct web_auth_peer p = {.family = 6};

	p.addr[0] = 0xfe;
	p.addr[1] = 0x80;
	p.addr[15] = n;
	return p;
}

static void advance_ms(int64_t ms)
{
	fake_now += ms;
}

static void state(struct web_auth_state *s)
{
	web_auth_get_state(s);
}

static struct web_auth_result setup_with(const char *token, const char *password,
					 struct web_auth_session *out)
{
	return web_auth_setup(token, password, strlen(password), &peer_a, out);
}

static struct web_auth_result login(const char *password, const struct web_auth_peer *peer,
				    struct web_auth_session *out)
{
	return web_auth_login(password, strlen(password), peer, out);
}

/* Bring the module to "administrator exists, nobody signed in". */
static void make_admin(void)
{
	struct web_auth_state s;
	struct web_auth_session session;

	zassert_ok(web_auth_init(&platform));
	state(&s);
	zassert_equal(setup_with(s.setup_token, PASSWORD, &session).status, WEB_AUTH_OK);
	zassert_ok(web_auth_init(&platform));
	kdf_calls = 0;
	save_calls = 0;
}

static void store_verifier(uint32_t iterations, const char *password)
{
	uint8_t salt[WEB_AUTH_SALT_LEN];

	for (size_t i = 0; i < sizeof(salt); i++) {
		salt[i] = (uint8_t)(0xA0 + i);
	}
	memcpy(store, "CWA1", 4);
	store[4] = 1;
	store[5] = store[6] = store[7] = 0;
	sys_put_le32(iterations, &store[8]);
	memcpy(&store[12], salt, sizeof(salt));
	fake_kdf_bytes((const uint8_t *)password, strlen(password), salt, sizeof(salt), iterations,
		       &store[12 + WEB_AUTH_SALT_LEN], WEB_AUTH_DERIVED_LEN);
	store_len = WEB_AUTH_VERIFIER_LEN;
	store_present = true;
}

/* memmem() is a GNU extension the sim tier's libc headers do not declare. */
static bool contains(const uint8_t *hay, size_t hay_len, const char *needle)
{
	size_t n = strlen(needle);

	for (size_t i = 0; n <= hay_len && i <= hay_len - n; i++) {
		if (memcmp(&hay[i], needle, n) == 0) {
			return true;
		}
	}
	return false;
}

static bool is_base64url(const char *s, size_t len)
{
	if (strlen(s) != len) {
		return false;
	}
	for (size_t i = 0; i < len; i++) {
		char c = s[i];

		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
		      c == '-' || c == '_')) {
			return false;
		}
	}
	return true;
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	reset_fakes();
}

ZTEST_SUITE(web_auth, NULL, NULL, before, NULL, NULL);

/* -- init --------------------------------------------------------------- */

ZTEST(web_auth, test_init_rejects_incomplete_platform)
{
	struct web_auth_platform partial = platform;

	zassert_equal(web_auth_init(NULL), -EINVAL);
	partial.kdf = NULL;
	zassert_equal(web_auth_init(&partial), -EINVAL);
	partial = platform;
	partial.save_verifier = NULL;
	zassert_equal(web_auth_init(&partial), -EINVAL);
}

ZTEST(web_auth, test_fresh_device_is_in_setup_with_a_random_token)
{
	struct web_auth_state s1, s2;

	zassert_ok(web_auth_init(&platform));
	state(&s1);
	zassert_true(s1.setup_required);
	zassert_true(s1.setup_allowed);
	zassert_true(s1.credential_usable);
	zassert_true(is_base64url(s1.setup_token, WEB_AUTH_SETUP_TOKEN_LEN));

	zassert_ok(web_auth_init(&platform));
	state(&s2);
	zassert_not_equal(strcmp(s1.setup_token, s2.setup_token), 0,
			  "a restart must draw a new setup token");
}

ZTEST(web_auth, test_stored_verifier_means_an_administrator)
{
	struct web_auth_state s;

	store_verifier(CONFIG_WEB_AUTH_PBKDF2_ITERATIONS, PASSWORD);
	zassert_ok(web_auth_init(&platform));
	state(&s);
	zassert_false(s.setup_required);
	zassert_false(s.setup_allowed);
	zassert_true(s.credential_usable);
	zassert_equal(s.setup_token[0], '\0', "no token is published once configured");
}

ZTEST(web_auth, test_store_failure_locks_rather_than_opening_setup)
{
	struct web_auth_state s;
	struct web_auth_session session;

	load_rc = -EIO;
	zassert_equal(web_auth_init(&platform), -EIO);
	state(&s);
	zassert_false(s.setup_required);
	zassert_false(s.setup_allowed);
	zassert_false(s.credential_usable);
	zassert_equal(login(PASSWORD, &peer_a, &session).status, WEB_AUTH_UNAVAILABLE);
	zassert_equal(setup_with("x", PASSWORD, &session).status, WEB_AUTH_UNAVAILABLE);
}

/* Each way a stored verifier can be damaged must lock, never reopen setup. */
ZTEST(web_auth, test_damaged_verifier_locks)
{
	struct web_auth_state s;
	struct web_auth_session session;
	static const struct {
		size_t offset;
		uint8_t value;
	} damage[] = {
		{0, 'X'},     /* magic */
		{4, 2},       /* unknown kdf */
		{5, 1},       /* reserved not zero */
		{8, 0},       /* with the rest zeroed below: zero iterations */
	};

	for (size_t i = 0; i < ARRAY_SIZE(damage); i++) {
		reset_fakes();
		store_verifier(CONFIG_WEB_AUTH_PBKDF2_ITERATIONS, PASSWORD);
		store[damage[i].offset] = damage[i].value;
		if (damage[i].offset == 8) {
			sys_put_le32(0, &store[8]);
		}
		zassert_equal(web_auth_init(&platform), -EBADMSG, "damage %zu", i);
		state(&s);
		zassert_false(s.setup_allowed, "damage %zu reopened setup", i);
		zassert_false(s.credential_usable, "damage %zu", i);
		zassert_equal(login(PASSWORD, &peer_a, &session).status, WEB_AUTH_UNAVAILABLE);
	}

	reset_fakes();
	store_verifier(CONFIG_WEB_AUTH_PBKDF2_MAX_ITERATIONS + 1, PASSWORD);
	zassert_equal(web_auth_init(&platform), -EBADMSG, "an absurd count is damage");

	reset_fakes();
	store_verifier(CONFIG_WEB_AUTH_PBKDF2_ITERATIONS, PASSWORD);
	store_len = WEB_AUTH_VERIFIER_LEN - 1;
	zassert_equal(web_auth_init(&platform), -EBADMSG, "short record");
}

ZTEST(web_auth, test_random_failure_at_init_locks)
{
	struct web_auth_state s;

	random_fails = true;
	zassert_equal(web_auth_init(&platform), -EFAULT);
	state(&s);
	zassert_false(s.setup_allowed);
}

ZTEST(web_auth, test_init_ends_sessions_like_a_reboot)
{
	struct web_auth_session session;

	make_admin();
	zassert_equal(login(PASSWORD, &peer_a, &session).status, WEB_AUTH_OK);
	zassert_ok(web_auth_init(&platform));
	zassert_equal(web_auth_resolve(session.token, NULL), WEB_AUTH_AUTHENTICATION_REQUIRED);
}

/* -- setup -------------------------------------------------------------- */

ZTEST(web_auth, test_setup_creates_administrator_and_session)
{
	struct web_auth_state s;
	struct web_auth_session session;
	struct web_auth_session resolved;

	zassert_ok(web_auth_init(&platform));
	state(&s);
	zassert_equal(setup_with(s.setup_token, PASSWORD, &session).status, WEB_AUTH_OK);

	zassert_true(is_base64url(session.token, WEB_AUTH_TOKEN_LEN));
	zassert_true(is_base64url(session.csrf_token, WEB_AUTH_TOKEN_LEN));
	zassert_not_equal(strcmp(session.token, session.csrf_token), 0);
	zassert_equal(session.idle_timeout_seconds, CONFIG_WEB_AUTH_IDLE_TIMEOUT_SECONDS);
	zassert_equal(session.absolute_remaining_seconds, CONFIG_WEB_AUTH_ABSOLUTE_TIMEOUT_SECONDS);
	zassert_equal(save_calls, 1);

	zassert_equal(web_auth_resolve(session.token, &resolved), WEB_AUTH_OK);
	zassert_str_equal(resolved.csrf_token, session.csrf_token);

	state(&s);
	zassert_false(s.setup_required);
	zassert_false(s.setup_allowed);
	zassert_equal(s.setup_token[0], '\0');
}

ZTEST(web_auth, test_verifier_layout)
{
	struct web_auth_state s;
	struct web_auth_session session;
	uint8_t expected[WEB_AUTH_DERIVED_LEN];

	zassert_ok(web_auth_init(&platform));
	state(&s);
	zassert_equal(setup_with(s.setup_token, PASSWORD, &session).status, WEB_AUTH_OK);

	zassert_equal(store_len, WEB_AUTH_VERIFIER_LEN);
	zassert_mem_equal(store, "CWA1", 4);
	zassert_equal(store[4], 1, "kdf id");
	zassert_equal(store[5] | store[6] | store[7], 0, "reserved");
	zassert_equal(sys_get_le32(&store[8]), CONFIG_WEB_AUTH_PBKDF2_ITERATIONS);
	zassert_equal(kdf_last_iterations, CONFIG_WEB_AUTH_PBKDF2_ITERATIONS);
	fake_kdf_bytes((const uint8_t *)PASSWORD, strlen(PASSWORD), &store[12], WEB_AUTH_SALT_LEN,
		       CONFIG_WEB_AUTH_PBKDF2_ITERATIONS, expected, sizeof(expected));
	zassert_mem_equal(&store[12 + WEB_AUTH_SALT_LEN], expected, sizeof(expected),
			  "derived key over the stored salt");
	zassert_false(contains(store, store_len, PASSWORD),
		      "the password itself is never stored");
}

ZTEST(web_auth, test_two_setups_draw_different_salts)
{
	struct web_auth_state s;
	struct web_auth_session session;
	uint8_t first_salt[WEB_AUTH_SALT_LEN];

	zassert_ok(web_auth_init(&platform));
	state(&s);
	zassert_equal(setup_with(s.setup_token, PASSWORD, &session).status, WEB_AUTH_OK);
	memcpy(first_salt, &store[12], sizeof(first_salt));

	store_present = false;
	zassert_ok(web_auth_init(&platform));
	state(&s);
	zassert_equal(setup_with(s.setup_token, PASSWORD, &session).status, WEB_AUTH_OK);
	zassert_true(memcmp(first_salt, &store[12], sizeof(first_salt)) != 0);
}

ZTEST(web_auth, test_setup_refuses_wrong_or_missing_token)
{
	struct web_auth_state s;
	struct web_auth_session session;
	char near[WEB_AUTH_SETUP_TOKEN_LEN + 1];

	zassert_ok(web_auth_init(&platform));
	state(&s);
	memcpy(near, s.setup_token, sizeof(near));
	near[0] = (near[0] == 'A') ? 'B' : 'A';

	zassert_equal(setup_with(near, PASSWORD, &session).status, WEB_AUTH_SETUP_NOT_ALLOWED);
	zassert_equal(setup_with(NULL, PASSWORD, &session).status, WEB_AUTH_SETUP_NOT_ALLOWED);
	near[WEB_AUTH_SETUP_TOKEN_LEN - 1] = '\0';
	memcpy(near, s.setup_token, WEB_AUTH_SETUP_TOKEN_LEN - 1);
	zassert_equal(setup_with(near, PASSWORD, &session).status, WEB_AUTH_SETUP_NOT_ALLOWED,
		      "a prefix of the token is not the token");
	zassert_equal(kdf_calls, 0, "no derivation for a refused token");
	zassert_equal(save_calls, 0);

	state(&s);
	zassert_true(s.setup_allowed, "a wrong token does not close setup");
}

ZTEST(web_auth, test_wrong_setup_tokens_are_rate_limited)
{
	struct web_auth_state s;
	struct web_auth_session session;
	struct web_auth_result r;

	zassert_ok(web_auth_init(&platform));
	state(&s);
	for (int i = 0; i < CONFIG_WEB_AUTH_FREE_FAILURES; i++) {
		zassert_equal(setup_with("wrong-token-wrong-token-wrong-tk", PASSWORD, &session)
				      .status,
			      WEB_AUTH_SETUP_NOT_ALLOWED);
	}
	r = setup_with(s.setup_token, PASSWORD, &session);
	zassert_equal(r.status, WEB_AUTH_RATE_LIMITED, "even the right token waits");
	zassert_equal(r.retry_after_seconds, 1);
}

ZTEST(web_auth, test_setup_closes_after_administrator_exists)
{
	struct web_auth_state s;
	struct web_auth_session session;
	char old_token[WEB_AUTH_SETUP_TOKEN_LEN + 1];

	zassert_ok(web_auth_init(&platform));
	state(&s);
	memcpy(old_token, s.setup_token, sizeof(old_token));
	zassert_equal(setup_with(old_token, PASSWORD, &session).status, WEB_AUTH_OK);

	for (int i = 0; i < CONFIG_WEB_AUTH_FREE_FAILURES + 2; i++) {
		zassert_equal(setup_with(old_token, NEW_PASSWORD, &session).status,
			      WEB_AUTH_SETUP_NOT_ALLOWED);
	}
	zassert_equal(login(PASSWORD, &peer_a, &session).status, WEB_AUTH_OK,
		      "setup attempts on a configured device are not guesses");
	zassert_equal(save_calls, 1, "the administrator was never replaced");
}

ZTEST(web_auth, test_setup_password_policy_counts_code_points)
{
	struct web_auth_state s;
	struct web_auth_session session;
	char buf[WEB_AUTH_PASSWORD_MAX_BYTES + 8];
	/* 12 Cyrillic letters: 24 bytes, 12 code points. */
	static const char cyrillic12[] = "пароль-админ";
	static const char eleven[] = "12345678901";
	static const char emoji[] = "\xF0\x9F\x94\x91"; /* U+1F511, four bytes */

	zassert_ok(web_auth_init(&platform));
	state(&s);

	zassert_equal(setup_with(s.setup_token, eleven, &session).status,
		      WEB_AUTH_INVALID_PASSWORD);
	state(&s);
	zassert_true(s.setup_allowed, "a policy refusal releases the claim");

	/* 128 four-byte characters: the largest valid password, 512 bytes. */
	for (int i = 0; i < WEB_AUTH_PASSWORD_MAX_CHARS; i++) {
		memcpy(&buf[i * 4], emoji, 4);
	}
	zassert_equal(web_auth_setup(s.setup_token, buf, WEB_AUTH_PASSWORD_MAX_BYTES, &peer_a,
				     &session)
			      .status,
		      WEB_AUTH_OK);

	store_present = false;
	zassert_ok(web_auth_init(&platform));
	state(&s);
	zassert_equal(setup_with(s.setup_token, cyrillic12, &session).status, WEB_AUTH_OK);
}

ZTEST(web_auth, test_setup_password_policy_refusals)
{
	struct web_auth_state s;
	struct web_auth_session session;
	char buf[WEB_AUTH_PASSWORD_MAX_BYTES + 8];

	zassert_ok(web_auth_init(&platform));
	state(&s);

	memset(buf, 'a', WEB_AUTH_PASSWORD_MAX_CHARS + 1);
	zassert_equal(web_auth_setup(s.setup_token, buf, WEB_AUTH_PASSWORD_MAX_CHARS + 1, &peer_a,
				     &session)
			      .status,
		      WEB_AUTH_INVALID_PASSWORD, "129 characters");

	memcpy(buf, "twelve chars\xFF", 13);
	zassert_equal(web_auth_setup(s.setup_token, buf, 13, &peer_a, &session).status,
		      WEB_AUTH_INVALID_PASSWORD, "not UTF-8");

	memcpy(buf, "twelve\0chars", 12);
	zassert_equal(web_auth_setup(s.setup_token, buf, 12, &peer_a, &session).status,
		      WEB_AUTH_INVALID_PASSWORD, "embedded NUL");

	memcpy(buf, "overlong \xC0\xAF abc", 15);
	zassert_equal(web_auth_setup(s.setup_token, buf, 15, &peer_a, &session).status,
		      WEB_AUTH_INVALID_PASSWORD, "overlong encoding");

	memcpy(buf, "surrogate \xED\xA0\x80 ab", 16);
	zassert_equal(web_auth_setup(s.setup_token, buf, 16, &peer_a, &session).status,
		      WEB_AUTH_INVALID_PASSWORD, "encoded surrogate");

	memcpy(buf, "truncated sequence \xE2\x82", 21);
	zassert_equal(web_auth_setup(s.setup_token, buf, 21, &peer_a, &session).status,
		      WEB_AUTH_INVALID_PASSWORD, "sequence cut by the length");

	zassert_equal(kdf_calls, 0);
}

ZTEST(web_auth, test_setup_storage_failure_reopens_setup)
{
	struct web_auth_state s;
	struct web_auth_session session;
	char token[WEB_AUTH_SETUP_TOKEN_LEN + 1];

	zassert_ok(web_auth_init(&platform));
	state(&s);
	memcpy(token, s.setup_token, sizeof(token));

	save_fails = true;
	zassert_equal(setup_with(token, PASSWORD, &session).status, WEB_AUTH_STORAGE_FAILED);
	state(&s);
	zassert_true(s.setup_allowed);
	zassert_str_equal(s.setup_token, token, "the page the operator has open still works");
	zassert_equal(login(PASSWORD, &peer_a, &session).status, WEB_AUTH_SETUP_NOT_ALLOWED,
		      "no half-configured administrator");

	save_fails = false;
	zassert_equal(setup_with(token, PASSWORD, &session).status, WEB_AUTH_OK);
}

ZTEST(web_auth, test_setup_kdf_failure_reopens_setup)
{
	struct web_auth_state s;
	struct web_auth_session session;

	zassert_ok(web_auth_init(&platform));
	state(&s);
	kdf_fails = true;
	zassert_equal(setup_with(s.setup_token, PASSWORD, &session).status, WEB_AUTH_INTERNAL);
	zassert_equal(save_calls, 0);
	state(&s);
	zassert_true(s.setup_allowed);
}

/* The concurrency case: a second setup arrives while the first derives. */
static struct web_auth_result racing_result;
static struct web_auth_state racing_state;
static char racing_token[WEB_AUTH_SETUP_TOKEN_LEN + 1];
static struct web_auth_result racing_login;

static void second_setup_during_derivation(void)
{
	struct web_auth_session session;

	web_auth_get_state(&racing_state);
	racing_result = web_auth_setup(racing_token, NEW_PASSWORD, strlen(NEW_PASSWORD), &peer_b,
				       &session);
	racing_login = web_auth_login(NEW_PASSWORD, strlen(NEW_PASSWORD), &peer_b, &session);
}

ZTEST(web_auth, test_concurrent_setup_has_one_winner)
{
	struct web_auth_state s;
	struct web_auth_session session;

	zassert_ok(web_auth_init(&platform));
	state(&s);
	memcpy(racing_token, s.setup_token, sizeof(racing_token));

	kdf_hook = second_setup_during_derivation;
	zassert_equal(setup_with(racing_token, PASSWORD, &session).status, WEB_AUTH_OK);

	zassert_true(racing_state.setup_required, "claimed, not yet configured");
	zassert_false(racing_state.setup_allowed, "claimed setup is closed to others");
	zassert_equal(racing_state.setup_token[0], '\0');
	zassert_equal(racing_result.status, WEB_AUTH_BUSY, "the loser is told to retry");
	zassert_equal(racing_login.status, WEB_AUTH_SETUP_NOT_ALLOWED);
	zassert_equal(save_calls, 1, "exactly one verifier stored");

	zassert_equal(login(PASSWORD, &peer_a, &session).status, WEB_AUTH_OK);
	zassert_equal(login(NEW_PASSWORD, &peer_b, &session).status,
		      WEB_AUTH_INVALID_CREDENTIALS);
}

/* -- login -------------------------------------------------------------- */

ZTEST(web_auth, test_login)
{
	struct web_auth_session a, b;

	make_admin();
	zassert_equal(login("not the password", &peer_a, &a).status, WEB_AUTH_INVALID_CREDENTIALS);
	zassert_equal(login(PASSWORD, &peer_a, &a).status, WEB_AUTH_OK);
	zassert_equal(login(PASSWORD, &peer_b, &b).status, WEB_AUTH_OK);
	zassert_not_equal(strcmp(a.token, b.token), 0);
	zassert_not_equal(strcmp(a.csrf_token, b.csrf_token), 0);
	zassert_equal(web_auth_resolve(a.token, NULL), WEB_AUTH_OK);
	zassert_equal(web_auth_resolve(b.token, NULL), WEB_AUTH_OK);
}

ZTEST(web_auth, test_login_before_setup)
{
	struct web_auth_session session;

	zassert_ok(web_auth_init(&platform));
	zassert_equal(login(PASSWORD, &peer_a, &session).status, WEB_AUTH_SETUP_NOT_ALLOWED);
	zassert_equal(kdf_calls, 0);
}

ZTEST(web_auth, test_empty_password_needs_no_derivation_but_counts)
{
	struct web_auth_session session;

	make_admin();
	for (int i = 0; i < CONFIG_WEB_AUTH_FREE_FAILURES; i++) {
		zassert_equal(web_auth_login("", 0, &peer_a, &session).status,
			      WEB_AUTH_INVALID_CREDENTIALS);
	}
	zassert_equal(kdf_calls, 0);
	zassert_equal(login(PASSWORD, &peer_a, &session).status, WEB_AUTH_RATE_LIMITED);
}

ZTEST(web_auth, test_login_uses_stored_parameters)
{
	struct web_auth_session session;

	store_verifier(4321, PASSWORD);
	zassert_ok(web_auth_init(&platform));
	zassert_equal(login(PASSWORD, &peer_a, &session).status, WEB_AUTH_OK);
	zassert_equal(kdf_last_iterations, 4321,
		      "a password set under an older count still verifies");
}

ZTEST(web_auth, test_login_kdf_failure_is_internal_and_not_a_guess)
{
	struct web_auth_session session;

	make_admin();
	kdf_fails = true;
	for (int i = 0; i < CONFIG_WEB_AUTH_FREE_FAILURES + 1; i++) {
		zassert_equal(login(PASSWORD, &peer_a, &session).status, WEB_AUTH_INTERNAL);
	}
	kdf_fails = false;
	zassert_equal(login(PASSWORD, &peer_a, &session).status, WEB_AUTH_OK);
}

static void finish_password_change(void)
{
	zassert_equal(web_auth_password_change_run(), WEB_AUTH_OK);
}

ZTEST(web_auth, test_login_racing_a_password_change_is_busy)
{
	struct web_auth_session session;

	make_admin();
	zassert_equal(web_auth_password_change_begin(PASSWORD, strlen(PASSWORD), NEW_PASSWORD,
						     strlen(NEW_PASSWORD), &peer_b)
			      .status,
		      WEB_AUTH_OK);
	kdf_hook = finish_password_change;
	zassert_equal(login(PASSWORD, &peer_a, &session).status, WEB_AUTH_BUSY,
		      "no session for a password that stopped existing mid-login");
	zassert_equal(web_auth_resolve(session.token, NULL), WEB_AUTH_AUTHENTICATION_REQUIRED);
	zassert_equal(login(NEW_PASSWORD, &peer_a, &session).status, WEB_AUTH_OK);
}

/* -- rate limiting ------------------------------------------------------ */

ZTEST(web_auth, test_per_peer_delay_doubles_and_caps)
{
	struct web_auth_session session;
	struct web_auth_result r;
	uint32_t expected = 1;

	make_admin();
	for (int i = 0; i < CONFIG_WEB_AUTH_FREE_FAILURES - 1; i++) {
		zassert_equal(login("wrong", &peer_a, &session).status,
			      WEB_AUTH_INVALID_CREDENTIALS, "free failure %d", i);
	}
	zassert_equal(login("wrong", &peer_a, &session).status, WEB_AUTH_INVALID_CREDENTIALS);

	for (int round = 0; round < 6; round++) {
		int calls = kdf_calls;

		r = login(PASSWORD, &peer_a, &session);
		zassert_equal(r.status, WEB_AUTH_RATE_LIMITED, "round %d", round);
		zassert_equal(r.retry_after_seconds, expected, "round %d", round);
		zassert_equal(kdf_calls, calls, "a refused attempt costs no derivation");

		advance_ms((int64_t)expected * 1000);
		zassert_equal(login("wrong", &peer_a, &session).status,
			      WEB_AUTH_INVALID_CREDENTIALS);
		expected = MIN(expected * 2, CONFIG_WEB_AUTH_MAX_DELAY_SECONDS);
	}
}

ZTEST(web_auth, test_blocked_peer_does_not_block_another)
{
	struct web_auth_session session;

	make_admin();
	for (int i = 0; i < CONFIG_WEB_AUTH_FREE_FAILURES; i++) {
		login("wrong", &peer_a, &session);
	}
	zassert_equal(login(PASSWORD, &peer_a, &session).status, WEB_AUTH_RATE_LIMITED);
	zassert_equal(login(PASSWORD, &peer_b, &session).status, WEB_AUTH_OK);
}

ZTEST(web_auth, test_success_clears_the_peer_count)
{
	struct web_auth_session session;

	make_admin();
	for (int i = 0; i < CONFIG_WEB_AUTH_FREE_FAILURES - 1; i++) {
		login("wrong", &peer_a, &session);
	}
	zassert_equal(login(PASSWORD, &peer_a, &session).status, WEB_AUTH_OK);
	for (int i = 0; i < CONFIG_WEB_AUTH_FREE_FAILURES - 1; i++) {
		zassert_equal(login("wrong", &peer_a, &session).status,
			      WEB_AUTH_INVALID_CREDENTIALS);
	}
	zassert_equal(login(PASSWORD, &peer_a, &session).status, WEB_AUTH_OK,
		      "the count started again after the success");
}

ZTEST(web_auth, test_global_limit_across_addresses)
{
	struct web_auth_session session;
	struct web_auth_result r;
	struct web_auth_peer p;

	make_admin();
	/* One failure per address, so no per-address delay ever applies. */
	for (int i = 0; i < CONFIG_WEB_AUTH_GLOBAL_FAILURES; i++) {
		p = peer_n((uint8_t)i);
		zassert_equal(login("wrong", &p, &session).status, WEB_AUTH_INVALID_CREDENTIALS);
		advance_ms(1000);
	}
	p = peer_n(200);
	r = login(PASSWORD, &p, &session);
	zassert_equal(r.status, WEB_AUTH_RATE_LIMITED, "the total guessing rate is bounded");
	/* The first failure was at t0; the window ends at t0 + window. We are at
	 * t0 + GLOBAL_FAILURES seconds. */
	zassert_equal(r.retry_after_seconds,
		      CONFIG_WEB_AUTH_GLOBAL_WINDOW_SECONDS - CONFIG_WEB_AUTH_GLOBAL_FAILURES);

	advance_ms((int64_t)r.retry_after_seconds * 1000);
	zassert_equal(login(PASSWORD, &p, &session).status, WEB_AUTH_OK,
		      "the oldest failure left the window");
}

ZTEST(web_auth, test_peer_slots_evict_least_recent)
{
	struct web_auth_session session;
	struct web_auth_peer p;

	make_admin();
	/* peer 0 is blocked... */
	p = peer_n(0);
	for (int i = 0; i < CONFIG_WEB_AUTH_FREE_FAILURES; i++) {
		login("wrong", &p, &session);
	}
	zassert_equal(login(PASSWORD, &p, &session).status, WEB_AUTH_RATE_LIMITED);
	/* ...until enough newer addresses push it out of the table. */
	for (int i = 1; i <= CONFIG_WEB_AUTH_PEER_SLOTS; i++) {
		struct web_auth_peer other = peer_n((uint8_t)i);

		advance_ms(1);
		login("wrong", &other, &session);
	}
	zassert_equal(login(PASSWORD, &p, &session).status, WEB_AUTH_OK,
		      "an evicted address starts over (the global limit is the backstop)");
}

ZTEST(web_auth, test_unknown_and_ipv4_peers_compare_sensibly)
{
	struct web_auth_session session;
	struct web_auth_peer v4_junk = peer_a;
	struct web_auth_peer unknown = {.family = 0, .addr = {1, 2, 3}};

	make_admin();
	/* Bytes past the fourth are not part of an IPv4 address. */
	memset(&v4_junk.addr[4], 0xEE, 12);
	for (int i = 0; i < CONFIG_WEB_AUTH_FREE_FAILURES; i++) {
		login("wrong", (i % 2) ? &v4_junk : &peer_a, &session);
	}
	zassert_equal(login(PASSWORD, &peer_a, &session).status, WEB_AUTH_RATE_LIMITED);

	/* Every unknown address, and none at all, is one client. */
	for (int i = 0; i < CONFIG_WEB_AUTH_FREE_FAILURES; i++) {
		web_auth_login("wrong", 5, (i % 2) ? &unknown : NULL, &session);
	}
	zassert_equal(web_auth_login(PASSWORD, strlen(PASSWORD), NULL, &session).status,
		      WEB_AUTH_RATE_LIMITED);
}

/* -- sessions ------------------------------------------------------------ */

ZTEST(web_auth, test_resolve_refusals)
{
	make_admin();
	zassert_equal(web_auth_resolve(NULL, NULL), WEB_AUTH_AUTHENTICATION_REQUIRED);
	zassert_equal(web_auth_resolve("", NULL), WEB_AUTH_AUTHENTICATION_REQUIRED);
	zassert_equal(web_auth_resolve("short", NULL), WEB_AUTH_AUTHENTICATION_REQUIRED);
	zassert_equal(web_auth_resolve("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", NULL),
		      WEB_AUTH_AUTHENTICATION_REQUIRED, "well-formed but never issued");
}

ZTEST(web_auth, test_idle_expiry_is_strictly_after_the_limit)
{
	struct web_auth_session session;

	make_admin();
	zassert_equal(login(PASSWORD, &peer_a, &session).status, WEB_AUTH_OK);
	advance_ms((int64_t)CONFIG_WEB_AUTH_IDLE_TIMEOUT_SECONDS * 1000);
	zassert_equal(web_auth_resolve(session.token, NULL), WEB_AUTH_OK, "exactly at the limit");
	advance_ms((int64_t)CONFIG_WEB_AUTH_IDLE_TIMEOUT_SECONDS * 1000 + 1);
	zassert_equal(web_auth_resolve(session.token, NULL), WEB_AUTH_SESSION_EXPIRED);
	zassert_equal(web_auth_resolve(session.token, NULL), WEB_AUTH_SESSION_EXPIRED,
		      "and it stays expired, not unknown");
}

ZTEST(web_auth, test_use_extends_idle_but_not_absolute)
{
	struct web_auth_session session, now;
	int64_t step = (int64_t)CONFIG_WEB_AUTH_IDLE_TIMEOUT_SECONDS * 1000 - 1000;
	int64_t elapsed = 0;

	make_admin();
	zassert_equal(login(PASSWORD, &peer_a, &session).status, WEB_AUTH_OK);
	while (elapsed + step <= (int64_t)CONFIG_WEB_AUTH_ABSOLUTE_TIMEOUT_SECONDS * 1000) {
		advance_ms(step);
		elapsed += step;
		zassert_equal(web_auth_resolve(session.token, &now), WEB_AUTH_OK);
		zassert_equal(now.absolute_remaining_seconds,
			      CONFIG_WEB_AUTH_ABSOLUTE_TIMEOUT_SECONDS - elapsed / 1000);
	}
	advance_ms((int64_t)CONFIG_WEB_AUTH_ABSOLUTE_TIMEOUT_SECONDS * 1000 - elapsed + 1);
	zassert_equal(web_auth_resolve(session.token, NULL), WEB_AUTH_SESSION_EXPIRED,
		      "kept busy, it still ends at the absolute lifetime");
}

ZTEST(web_auth, test_logout)
{
	struct web_auth_session session;

	make_admin();
	zassert_equal(login(PASSWORD, &peer_a, &session).status, WEB_AUTH_OK);
	zassert_equal(web_auth_logout(session.token), WEB_AUTH_OK);
	zassert_equal(web_auth_resolve(session.token, NULL), WEB_AUTH_SESSION_EXPIRED);
	zassert_equal(web_auth_logout(session.token), WEB_AUTH_OK, "ending an ended one is fine");
	zassert_equal(web_auth_logout("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"),
		      WEB_AUTH_AUTHENTICATION_REQUIRED);
	zassert_equal(web_auth_logout(NULL), WEB_AUTH_AUTHENTICATION_REQUIRED);
}

ZTEST(web_auth, test_full_table_ends_least_recently_used)
{
	struct web_auth_session s1, s2, s3;

	make_admin();
	zassert_equal(login(PASSWORD, &peer_a, &s1).status, WEB_AUTH_OK);
	advance_ms(10);
	zassert_equal(login(PASSWORD, &peer_a, &s2).status, WEB_AUTH_OK);
	advance_ms(10);
	/* Touch the older one, so the newer is now least recently used. */
	zassert_equal(web_auth_resolve(s1.token, NULL), WEB_AUTH_OK);
	advance_ms(10);
	zassert_equal(login(PASSWORD, &peer_a, &s3).status, WEB_AUTH_OK,
		      "a full table never refuses the administrator");

	zassert_equal(web_auth_resolve(s1.token, NULL), WEB_AUTH_OK);
	zassert_equal(web_auth_resolve(s2.token, NULL), WEB_AUTH_SESSION_EXPIRED);
	zassert_equal(web_auth_resolve(s3.token, NULL), WEB_AUTH_OK);
}

ZTEST(web_auth, test_tombstones_are_bounded)
{
	struct web_auth_session s[CONFIG_WEB_AUTH_TOMBSTONES + 1];

	make_admin();
	for (int i = 0; i <= CONFIG_WEB_AUTH_TOMBSTONES; i++) {
		zassert_equal(login(PASSWORD, &peer_a, &s[i]).status, WEB_AUTH_OK);
		zassert_equal(web_auth_logout(s[i].token), WEB_AUTH_OK);
	}
	zassert_equal(web_auth_resolve(s[0].token, NULL), WEB_AUTH_AUTHENTICATION_REQUIRED,
		      "the oldest ended session was forgotten");
	for (int i = 1; i <= CONFIG_WEB_AUTH_TOMBSTONES; i++) {
		zassert_equal(web_auth_resolve(s[i].token, NULL), WEB_AUTH_SESSION_EXPIRED);
	}
}

ZTEST(web_auth, test_csrf)
{
	struct web_auth_session a, b;
	char tampered[WEB_AUTH_TOKEN_LEN + 1];

	make_admin();
	zassert_equal(login(PASSWORD, &peer_a, &a).status, WEB_AUTH_OK);
	zassert_equal(login(PASSWORD, &peer_a, &b).status, WEB_AUTH_OK);

	zassert_equal(web_auth_check_csrf(&a, a.csrf_token), WEB_AUTH_OK);
	zassert_equal(web_auth_check_csrf(&a, NULL), WEB_AUTH_CSRF_FAILED);
	zassert_equal(web_auth_check_csrf(&a, ""), WEB_AUTH_CSRF_FAILED);
	zassert_equal(web_auth_check_csrf(&a, b.csrf_token), WEB_AUTH_CSRF_FAILED,
		      "another session's token");
	zassert_equal(web_auth_check_csrf(&a, a.token), WEB_AUTH_CSRF_FAILED,
		      "the cookie is not the CSRF token");
	memcpy(tampered, a.csrf_token, sizeof(tampered));
	tampered[WEB_AUTH_TOKEN_LEN - 1] = (tampered[WEB_AUTH_TOKEN_LEN - 1] == 'A') ? 'B' : 'A';
	zassert_equal(web_auth_check_csrf(&a, tampered), WEB_AUTH_CSRF_FAILED);
	tampered[WEB_AUTH_TOKEN_LEN - 1] = '\0';
	memcpy(tampered, a.csrf_token, WEB_AUTH_TOKEN_LEN - 1);
	zassert_equal(web_auth_check_csrf(&a, tampered), WEB_AUTH_CSRF_FAILED, "a prefix");
	zassert_equal(web_auth_check_csrf(NULL, a.csrf_token), WEB_AUTH_CSRF_FAILED);
}

/* -- password change ---------------------------------------------------- */

static struct web_auth_result change(const char *current, const char *next)
{
	return web_auth_password_change_begin(current, strlen(current), next, strlen(next),
					      &peer_a);
}

ZTEST(web_auth, test_password_change_ends_every_session)
{
	struct web_auth_session a, b;

	make_admin();
	zassert_equal(login(PASSWORD, &peer_a, &a).status, WEB_AUTH_OK);
	zassert_equal(login(PASSWORD, &peer_b, &b).status, WEB_AUTH_OK);

	zassert_equal(change(PASSWORD, NEW_PASSWORD).status, WEB_AUTH_OK);
	zassert_equal(web_auth_resolve(a.token, NULL), WEB_AUTH_OK, "nothing ends before the job");
	zassert_equal(web_auth_password_change_run(), WEB_AUTH_OK);
	zassert_equal(save_calls, 1);

	zassert_equal(web_auth_resolve(a.token, NULL), WEB_AUTH_SESSION_EXPIRED);
	zassert_equal(web_auth_resolve(b.token, NULL), WEB_AUTH_SESSION_EXPIRED);
	zassert_equal(login(PASSWORD, &peer_a, &a).status, WEB_AUTH_INVALID_CREDENTIALS);
	zassert_equal(login(NEW_PASSWORD, &peer_a, &a).status, WEB_AUTH_OK);

	/* And it survives a restart, because it was stored. */
	zassert_ok(web_auth_init(&platform));
	zassert_equal(login(NEW_PASSWORD, &peer_a, &a).status, WEB_AUTH_OK);
}

ZTEST(web_auth, test_password_change_wrong_current)
{
	make_admin();
	zassert_equal(change("not it", NEW_PASSWORD).status, WEB_AUTH_INVALID_CREDENTIALS);
	zassert_equal(web_auth_password_change_run(), WEB_AUTH_UNAVAILABLE, "nothing pending");
	zassert_equal(change(PASSWORD, NEW_PASSWORD).status, WEB_AUTH_OK,
		      "a failed begin does not leave the slot reserved");
}

ZTEST(web_auth, test_password_change_policy_before_derivation)
{
	make_admin();
	zassert_equal(change("not it", "short").status, WEB_AUTH_INVALID_PASSWORD);
	zassert_equal(kdf_calls, 0);
}

ZTEST(web_auth, test_password_change_one_at_a_time)
{
	make_admin();
	zassert_equal(change(PASSWORD, NEW_PASSWORD).status, WEB_AUTH_OK);
	zassert_equal(change(PASSWORD, "yet another password").status, WEB_AUTH_BUSY);
	zassert_equal(web_auth_password_change_run(), WEB_AUTH_OK);
	zassert_equal(web_auth_password_change_run(), WEB_AUTH_UNAVAILABLE);
}

ZTEST(web_auth, test_password_change_storage_failure_changes_nothing)
{
	struct web_auth_session a;

	make_admin();
	zassert_equal(login(PASSWORD, &peer_a, &a).status, WEB_AUTH_OK);
	zassert_equal(change(PASSWORD, NEW_PASSWORD).status, WEB_AUTH_OK);
	save_fails = true;
	zassert_equal(web_auth_password_change_run(), WEB_AUTH_STORAGE_FAILED);
	save_fails = false;

	zassert_equal(web_auth_resolve(a.token, NULL), WEB_AUTH_OK, "sessions untouched");
	zassert_equal(web_auth_password_change_run(), WEB_AUTH_UNAVAILABLE, "pending cleared");
	zassert_equal(login(PASSWORD, &peer_b, &a).status, WEB_AUTH_OK, "old password still works");
	zassert_equal(change(PASSWORD, NEW_PASSWORD).status, WEB_AUTH_OK, "and a retry may begin");
}

ZTEST(web_auth, test_password_change_kdf_failure_changes_nothing)
{
	struct web_auth_session a;

	make_admin();
	zassert_equal(change(PASSWORD, NEW_PASSWORD).status, WEB_AUTH_OK);
	kdf_fails = true;
	zassert_equal(web_auth_password_change_run(), WEB_AUTH_INTERNAL);
	kdf_fails = false;
	zassert_equal(save_calls, 0);
	zassert_equal(login(PASSWORD, &peer_a, &a).status, WEB_AUTH_OK);
}

ZTEST(web_auth, test_password_change_abandon)
{
	make_admin();
	zassert_equal(change(PASSWORD, NEW_PASSWORD).status, WEB_AUTH_OK);
	web_auth_password_change_abandon();
	zassert_equal(web_auth_password_change_run(), WEB_AUTH_UNAVAILABLE);
	zassert_equal(change(PASSWORD, NEW_PASSWORD).status, WEB_AUTH_OK);
}

ZTEST(web_auth, test_password_change_rate_limited)
{
	make_admin();
	for (int i = 0; i < CONFIG_WEB_AUTH_FREE_FAILURES; i++) {
		zassert_equal(change("wrong", NEW_PASSWORD).status, WEB_AUTH_INVALID_CREDENTIALS);
	}
	int calls = kdf_calls;
	struct web_auth_result r = change(PASSWORD, NEW_PASSWORD);

	zassert_equal(r.status, WEB_AUTH_RATE_LIMITED);
	zassert_true(r.retry_after_seconds > 0);
	zassert_equal(kdf_calls, calls);
}

ZTEST(web_auth, test_password_change_before_setup)
{
	zassert_ok(web_auth_init(&platform));
	zassert_equal(change(PASSWORD, NEW_PASSWORD).status, WEB_AUTH_UNAVAILABLE);
}

ZTEST(web_auth, test_status_names)
{
	zassert_str_equal(web_auth_status_str(WEB_AUTH_OK), "ok");
	zassert_str_equal(web_auth_status_str(WEB_AUTH_SESSION_EXPIRED), "session_expired");
	zassert_str_equal(web_auth_status_str(WEB_AUTH_INTERNAL), "internal");
	zassert_str_equal(web_auth_status_str((enum web_auth_status)99), "unknown");
}

/* -- a token is exactly the token -------------------------------------------- */

ZTEST(web_auth, test_tokens_with_anything_appended_are_refused)
{
	struct web_auth_state s;
	struct web_auth_session session;
	char longer[WEB_AUTH_TOKEN_LEN + 8];

	zassert_ok(web_auth_init(&platform));
	state(&s);
	snprintf(longer, sizeof(longer), "%sx", s.setup_token);
	zassert_equal(setup_with(longer, PASSWORD, &session).status, WEB_AUTH_SETUP_NOT_ALLOWED,
		      "the setup token with a character appended");
	zassert_equal(setup_with(s.setup_token, PASSWORD, &session).status, WEB_AUTH_OK);

	snprintf(longer, sizeof(longer), "%sA", session.token);
	zassert_equal(web_auth_resolve(longer, NULL), WEB_AUTH_AUTHENTICATION_REQUIRED,
		      "the session token with a character appended");
	zassert_equal(web_auth_logout(longer), WEB_AUTH_AUTHENTICATION_REQUIRED);
	zassert_equal(web_auth_resolve(session.token, NULL), WEB_AUTH_OK,
		      "and the real session was not ended by it");

	snprintf(longer, sizeof(longer), "%s-", session.csrf_token);
	zassert_equal(web_auth_check_csrf(&session, longer), WEB_AUTH_CSRF_FAILED,
		      "the CSRF token with a character appended");
}
