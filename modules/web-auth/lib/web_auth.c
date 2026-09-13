/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * web-auth core. The design is documented in include/web_auth/web_auth.h;
 * comments here cover only how it is carried out.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <web_auth/web_auth.h>

LOG_MODULE_REGISTER(web_auth, CONFIG_WEB_AUTH_LOG_LEVEL);

#define VERIFIER_MAGIC  "CWA1"
#define KDF_PBKDF2_SHA256 1U

#define SESSION_BYTES     32U
#define SETUP_TOKEN_BYTES 24U

#define TOMBSTONE_SLOTS MAX(CONFIG_WEB_AUTH_TOMBSTONES, 1)

enum phase {
	/** web_auth_init() has not succeeded. */
	PHASE_UNINITIALISED = 0,
	/** No administrator; setup is open. */
	PHASE_SETUP,
	/** A setup request is deriving and storing; setup is closed to others. */
	PHASE_CLAIMED,
	/** An administrator exists. */
	PHASE_ADMIN,
	/** The store failed or holds a damaged verifier. */
	PHASE_LOCKED,
};

enum change {
	CHANGE_IDLE = 0,
	/** Reserved by a begin() that is verifying the current password. */
	CHANGE_VERIFYING,
	/** A new password waits for run(). */
	CHANGE_PENDING,
	/** run() is deriving and storing it. */
	CHANGE_RUNNING,
};

struct session_slot {
	bool active;
	char token[WEB_AUTH_TOKEN_LEN + 1];
	char csrf[WEB_AUTH_TOKEN_LEN + 1];
	int64_t created_ms;
	int64_t last_used_ms;
};

struct tombstone {
	bool used;
	char token[WEB_AUTH_TOKEN_LEN + 1];
};

struct peer_slot {
	bool used;
	struct web_auth_peer peer;
	uint32_t failures;
	int64_t blocked_until_ms;
	int64_t last_ms;
};

static struct {
	const struct web_auth_platform *platform;
	enum phase phase;
	uint8_t verifier[WEB_AUTH_VERIFIER_LEN];
	/** Bumped whenever the verifier changes; a derivation that started
	 *  against an older one is stale. */
	uint32_t generation;
	char setup_token[WEB_AUTH_SETUP_TOKEN_LEN + 1];

	struct session_slot sessions[CONFIG_WEB_AUTH_MAX_SESSIONS];
	struct tombstone tombstones[TOMBSTONE_SLOTS];
	size_t tombstone_next;

	struct peer_slot peers[CONFIG_WEB_AUTH_PEER_SLOTS];
	int64_t global_failures[CONFIG_WEB_AUTH_GLOBAL_FAILURES];
	size_t global_next;
	size_t global_count;

	enum change change;
	char pending[WEB_AUTH_PASSWORD_MAX_BYTES];
	size_t pending_len;
} st;

static K_MUTEX_DEFINE(lock);

/* -- small primitives ------------------------------------------------- */

static void wipe(void *buf, size_t len)
{
	volatile uint8_t *p = buf;

	while (len-- > 0U) {
		*p++ = 0U;
	}
}

/* Constant time in the length, which is fixed for every secret compared here. */
static bool equal_ct(const uint8_t *a, const uint8_t *b, size_t len)
{
	uint8_t diff = 0U;

	for (size_t i = 0; i < len; i++) {
		diff |= a[i] ^ b[i];
	}

	return diff == 0U;
}

/* strnlen() is POSIX, and the sim tier's libc headers do not declare it. */
static size_t bounded_len(const char *s, size_t max)
{
	size_t n = 0;

	while (n < max && s[n] != '\0') {
		n++;
	}

	return n;
}

/* A presented token against a stored one of known length. */
static bool token_equal(const char *presented, const char *stored, size_t len)
{
	if (presented == NULL || bounded_len(presented, len + 1U) != len) {
		return false;
	}

	return equal_ct((const uint8_t *)presented, (const uint8_t *)stored, len);
}

static void base64url(const uint8_t *in, size_t len, char *out)
{
	static const char alphabet[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
	size_t o = 0;
	size_t i = 0;

	for (; i + 3U <= len; i += 3U) {
		uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];

		out[o++] = alphabet[(v >> 18) & 0x3F];
		out[o++] = alphabet[(v >> 12) & 0x3F];
		out[o++] = alphabet[(v >> 6) & 0x3F];
		out[o++] = alphabet[v & 0x3F];
	}
	if (len - i == 1U) {
		uint32_t v = (uint32_t)in[i] << 16;

		out[o++] = alphabet[(v >> 18) & 0x3F];
		out[o++] = alphabet[(v >> 12) & 0x3F];
	} else if (len - i == 2U) {
		uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);

		out[o++] = alphabet[(v >> 18) & 0x3F];
		out[o++] = alphabet[(v >> 12) & 0x3F];
		out[o++] = alphabet[(v >> 6) & 0x3F];
	}
	out[o] = '\0';
}

/* Code points in well-formed UTF-8, or -1. Overlong forms, surrogates and
 * values past U+10FFFF are malformed. */
static int utf8_chars(const char *text, size_t len)
{
	const uint8_t *s = (const uint8_t *)text;
	int count = 0;
	size_t i = 0;

	while (i < len) {
		uint8_t c = s[i];
		uint32_t cp;
		size_t extra;

		if (c < 0x80U) {
			if (c == 0U) {
				return -1;
			}
			i++;
			count++;
			continue;
		} else if ((c & 0xE0U) == 0xC0U) {
			cp = c & 0x1FU;
			extra = 1U;
		} else if ((c & 0xF0U) == 0xE0U) {
			cp = c & 0x0FU;
			extra = 2U;
		} else if ((c & 0xF8U) == 0xF0U) {
			cp = c & 0x07U;
			extra = 3U;
		} else {
			return -1;
		}
		/* The continuation bytes must all be inside the buffer. */
		if (i + extra >= len) {
			return -1;
		}
		for (size_t k = 1; k <= extra; k++) {
			if ((s[i + k] & 0xC0U) != 0x80U) {
				return -1;
			}
			cp = (cp << 6) | (s[i + k] & 0x3FU);
		}
		if ((extra == 1U && cp < 0x80U) || (extra == 2U && cp < 0x800U) ||
		    (extra == 3U && cp < 0x10000U) || cp > 0x10FFFFU ||
		    (cp >= 0xD800U && cp <= 0xDFFFU)) {
			return -1;
		}
		i += extra + 1U;
		count++;
	}

	return count;
}

static bool password_fits(const char *password, size_t len, int min_chars)
{
	int chars;

	if (password == NULL || len > WEB_AUTH_PASSWORD_MAX_BYTES) {
		return false;
	}
	chars = utf8_chars(password, len);

	return chars >= min_chars && chars <= WEB_AUTH_PASSWORD_MAX_CHARS;
}

static int64_t now_ms(void)
{
	return st.platform->now_ms();
}

static uint32_t seconds_until(int64_t deadline_ms, int64_t now)
{
	int64_t ms = deadline_ms - now;

	if (ms <= 0) {
		return 1U;
	}

	return (uint32_t)((ms + 999) / 1000);
}

/* -- verifier --------------------------------------------------------- */

static void verifier_encode(uint32_t iterations, const uint8_t *salt, const uint8_t *derived,
			    uint8_t *out)
{
	memcpy(out, VERIFIER_MAGIC, 4);
	out[4] = KDF_PBKDF2_SHA256;
	out[5] = 0U;
	out[6] = 0U;
	out[7] = 0U;
	sys_put_le32(iterations, &out[8]);
	memcpy(&out[12], salt, WEB_AUTH_SALT_LEN);
	memcpy(&out[12 + WEB_AUTH_SALT_LEN], derived, WEB_AUTH_DERIVED_LEN);
}

static bool verifier_decode(const uint8_t *in, size_t len, uint32_t *iterations,
			    const uint8_t **salt, const uint8_t **derived)
{
	if (len != WEB_AUTH_VERIFIER_LEN || memcmp(in, VERIFIER_MAGIC, 4) != 0 ||
	    in[4] != KDF_PBKDF2_SHA256 || in[5] != 0U || in[6] != 0U || in[7] != 0U) {
		return false;
	}
	*iterations = sys_get_le32(&in[8]);
	if (*iterations == 0U || *iterations > CONFIG_WEB_AUTH_PBKDF2_MAX_ITERATIONS) {
		return false;
	}
	*salt = &in[12];
	*derived = &in[12 + WEB_AUTH_SALT_LEN];

	return true;
}

/* Derive with the stored parameters and compare. Called without the lock. */
static enum web_auth_status verify_against(const uint8_t *verifier, const char *password,
					   size_t len)
{
	uint32_t iterations;
	const uint8_t *salt;
	const uint8_t *expected;
	uint8_t derived[WEB_AUTH_DERIVED_LEN];
	bool match;

	if (!verifier_decode(verifier, WEB_AUTH_VERIFIER_LEN, &iterations, &salt, &expected)) {
		return WEB_AUTH_UNAVAILABLE;
	}
	if (st.platform->kdf((const uint8_t *)password, len, salt, WEB_AUTH_SALT_LEN, iterations,
			     derived, sizeof(derived)) != 0) {
		wipe(derived, sizeof(derived));
		return WEB_AUTH_INTERNAL;
	}
	match = equal_ct(derived, expected, sizeof(derived));
	wipe(derived, sizeof(derived));

	return match ? WEB_AUTH_OK : WEB_AUTH_INVALID_CREDENTIALS;
}

/* Draw a salt, derive, encode. Called without the lock. */
static enum web_auth_status make_verifier(const char *password, size_t len, uint8_t *out)
{
	uint8_t salt[WEB_AUTH_SALT_LEN];
	uint8_t derived[WEB_AUTH_DERIVED_LEN];
	enum web_auth_status status = WEB_AUTH_OK;

	if (st.platform->random(salt, sizeof(salt)) != 0 ||
	    st.platform->kdf((const uint8_t *)password, len, salt, sizeof(salt),
			     CONFIG_WEB_AUTH_PBKDF2_ITERATIONS, derived, sizeof(derived)) != 0) {
		status = WEB_AUTH_INTERNAL;
	} else {
		verifier_encode(CONFIG_WEB_AUTH_PBKDF2_ITERATIONS, salt, derived, out);
	}
	wipe(derived, sizeof(derived));

	return status;
}

/* -- rate limiting (lock held) ------------------------------------------ */

static bool peer_equal(const struct web_auth_peer *a, const struct web_auth_peer *b)
{
	size_t n = (a->family == 4U) ? 4U : (a->family == 6U) ? 16U : 0U;

	return a->family == b->family && memcmp(a->addr, b->addr, n) == 0;
}

static struct web_auth_peer normalised(const struct web_auth_peer *peer)
{
	struct web_auth_peer p = {0};

	if (peer != NULL && (peer->family == 4U || peer->family == 6U)) {
		p.family = peer->family;
		memcpy(p.addr, peer->addr, peer->family == 4U ? 4U : 16U);
	}

	return p;
}

static struct peer_slot *peer_find(const struct web_auth_peer *peer)
{
	for (size_t i = 0; i < ARRAY_SIZE(st.peers); i++) {
		if (st.peers[i].used && peer_equal(&st.peers[i].peer, peer)) {
			return &st.peers[i];
		}
	}

	return NULL;
}

static void global_prune(int64_t now)
{
	int64_t window = (int64_t)CONFIG_WEB_AUTH_GLOBAL_WINDOW_SECONDS * 1000;

	/* The ring holds timestamps oldest first, starting global_count back
	 * from global_next. Drop the ones that left the window. */
	while (st.global_count > 0U) {
		size_t oldest = (st.global_next + ARRAY_SIZE(st.global_failures) - st.global_count) %
				ARRAY_SIZE(st.global_failures);

		if (now - st.global_failures[oldest] < window) {
			break;
		}
		st.global_count--;
	}
}

/* true when an attempt may proceed; otherwise *retry is set. */
static bool rate_allows(const struct web_auth_peer *peer, int64_t now, uint32_t *retry)
{
	struct peer_slot *slot;

	global_prune(now);
	if (st.global_count >= ARRAY_SIZE(st.global_failures)) {
		size_t oldest = st.global_next; /* the ring is full, so next is the oldest */

		*retry = seconds_until(st.global_failures[oldest] +
					       (int64_t)CONFIG_WEB_AUTH_GLOBAL_WINDOW_SECONDS * 1000,
				       now);
		return false;
	}

	slot = peer_find(peer);
	if (slot != NULL && slot->blocked_until_ms > now) {
		*retry = seconds_until(slot->blocked_until_ms, now);
		return false;
	}

	return true;
}

static void rate_fail(const struct web_auth_peer *peer, int64_t now)
{
	struct peer_slot *slot = peer_find(peer);

	st.global_failures[st.global_next] = now;
	st.global_next = (st.global_next + 1U) % ARRAY_SIZE(st.global_failures);
	if (st.global_count < ARRAY_SIZE(st.global_failures)) {
		st.global_count++;
	}

	if (slot == NULL) {
		/* A free slot, else the one that failed least recently. */
		slot = &st.peers[0];
		for (size_t i = 0; i < ARRAY_SIZE(st.peers); i++) {
			if (!st.peers[i].used) {
				slot = &st.peers[i];
				break;
			}
			if (st.peers[i].last_ms < slot->last_ms) {
				slot = &st.peers[i];
			}
		}
		memset(slot, 0, sizeof(*slot));
		slot->used = true;
		slot->peer = *peer;
	}

	slot->failures++;
	slot->last_ms = now;
	if (slot->failures >= CONFIG_WEB_AUTH_FREE_FAILURES) {
		uint32_t steps = slot->failures - CONFIG_WEB_AUTH_FREE_FAILURES;
		uint64_t delay_s = (steps >= 31U) ? UINT32_MAX : (1ULL << steps);

		delay_s = MIN(delay_s, (uint64_t)CONFIG_WEB_AUTH_MAX_DELAY_SECONDS);
		slot->blocked_until_ms = now + (int64_t)delay_s * 1000;
	}
}

static void rate_success(const struct web_auth_peer *peer)
{
	struct peer_slot *slot = peer_find(peer);

	if (slot != NULL) {
		memset(slot, 0, sizeof(*slot));
	}
}

/* -- sessions (lock held) ----------------------------------------------- */

static void tombstone_add(const char *token)
{
	if (CONFIG_WEB_AUTH_TOMBSTONES == 0) {
		return;
	}
	st.tombstones[st.tombstone_next].used = true;
	memcpy(st.tombstones[st.tombstone_next].token, token, WEB_AUTH_TOKEN_LEN + 1U);
	st.tombstone_next = (st.tombstone_next + 1U) % CONFIG_WEB_AUTH_TOMBSTONES;
}

static bool tombstone_has(const char *token)
{
	bool found = false;

	for (size_t i = 0; i < (size_t)CONFIG_WEB_AUTH_TOMBSTONES; i++) {
		if (st.tombstones[i].used &&
		    token_equal(token, st.tombstones[i].token, WEB_AUTH_TOKEN_LEN)) {
			found = true;
		}
	}

	return found;
}

static void session_end(struct session_slot *slot)
{
	tombstone_add(slot->token);
	wipe(slot, sizeof(*slot));
}

static void sessions_end_all(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(st.sessions); i++) {
		if (st.sessions[i].active) {
			session_end(&st.sessions[i]);
		}
	}
}

static bool session_expired(const struct session_slot *slot, int64_t now)
{
	return now - slot->last_used_ms > (int64_t)CONFIG_WEB_AUTH_IDLE_TIMEOUT_SECONDS * 1000 ||
	       now - slot->created_ms > (int64_t)CONFIG_WEB_AUTH_ABSOLUTE_TIMEOUT_SECONDS * 1000;
}

static void session_describe(const struct session_slot *slot, int64_t now,
			     struct web_auth_session *out)
{
	int64_t remaining =
		(int64_t)CONFIG_WEB_AUTH_ABSOLUTE_TIMEOUT_SECONDS * 1000 - (now - slot->created_ms);

	memcpy(out->token, slot->token, sizeof(out->token));
	memcpy(out->csrf_token, slot->csrf, sizeof(out->csrf_token));
	out->idle_timeout_seconds = CONFIG_WEB_AUTH_IDLE_TIMEOUT_SECONDS;
	out->absolute_remaining_seconds = remaining > 0 ? (uint32_t)(remaining / 1000) : 0U;
}

/* Random material for a session, drawn before taking the lock. */
struct session_seed {
	uint8_t token[SESSION_BYTES];
	uint8_t csrf[SESSION_BYTES];
};

static int session_seed_draw(struct session_seed *seed)
{
	return st.platform->random((uint8_t *)seed, sizeof(*seed));
}

static void session_issue(const struct session_seed *seed, int64_t now,
			  struct web_auth_session *out)
{
	struct session_slot *slot = NULL;

	for (size_t i = 0; i < ARRAY_SIZE(st.sessions); i++) {
		if (!st.sessions[i].active) {
			slot = &st.sessions[i];
			break;
		}
	}
	if (slot == NULL) {
		slot = &st.sessions[0];
		for (size_t i = 1; i < ARRAY_SIZE(st.sessions); i++) {
			if (st.sessions[i].last_used_ms < slot->last_used_ms) {
				slot = &st.sessions[i];
			}
		}
		session_end(slot);
	}

	slot->active = true;
	base64url(seed->token, sizeof(seed->token), slot->token);
	base64url(seed->csrf, sizeof(seed->csrf), slot->csrf);
	slot->created_ms = now;
	slot->last_used_ms = now;
	if (out != NULL) {
		session_describe(slot, now, out);
	}
}

/* -- public ------------------------------------------------------------- */

int web_auth_init(const struct web_auth_platform *platform)
{
	uint8_t stored[WEB_AUTH_VERIFIER_LEN + 1];
	uint8_t raw_token[SETUP_TOKEN_BYTES];
	size_t len = 0;
	int rc;
	int result = 0;

	if (platform == NULL || platform->random == NULL || platform->now_ms == NULL ||
	    platform->kdf == NULL || platform->load_verifier == NULL ||
	    platform->save_verifier == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);
	wipe(&st, sizeof(st));
	st.platform = platform;
	st.phase = PHASE_UNINITIALISED;
	k_mutex_unlock(&lock);

	rc = platform->load_verifier(stored, sizeof(stored), &len);

	k_mutex_lock(&lock, K_FOREVER);
	if (rc == -ENOENT) {
		if (platform->random(raw_token, sizeof(raw_token)) != 0) {
			st.phase = PHASE_LOCKED;
			result = -EFAULT;
		} else {
			base64url(raw_token, sizeof(raw_token), st.setup_token);
			st.phase = PHASE_SETUP;
		}
	} else if (rc != 0) {
		LOG_ERR("credential store failed: %d", rc);
		st.phase = PHASE_LOCKED;
		result = -EIO;
	} else {
		uint32_t iterations;
		const uint8_t *salt;
		const uint8_t *derived;

		if (len == WEB_AUTH_VERIFIER_LEN &&
		    verifier_decode(stored, len, &iterations, &salt, &derived)) {
			memcpy(st.verifier, stored, WEB_AUTH_VERIFIER_LEN);
			st.phase = PHASE_ADMIN;
		} else {
			LOG_ERR("stored credential is damaged; web sign-in is locked");
			st.phase = PHASE_LOCKED;
			result = -EBADMSG;
		}
	}
	k_mutex_unlock(&lock);

	wipe(stored, sizeof(stored));
	wipe(raw_token, sizeof(raw_token));

	return result;
}

void web_auth_get_state(struct web_auth_state *out)
{
	k_mutex_lock(&lock, K_FOREVER);
	memset(out, 0, sizeof(*out));
	out->setup_required = st.phase == PHASE_SETUP || st.phase == PHASE_CLAIMED;
	out->setup_allowed = st.phase == PHASE_SETUP;
	out->credential_usable = st.phase == PHASE_SETUP || st.phase == PHASE_CLAIMED ||
				 st.phase == PHASE_ADMIN;
	if (out->setup_allowed) {
		memcpy(out->setup_token, st.setup_token, sizeof(out->setup_token));
	}
	k_mutex_unlock(&lock);
}

struct web_auth_result web_auth_setup(const char *setup_token, const char *password,
				      size_t password_len, const struct web_auth_peer *peer_in,
				      struct web_auth_session *out)
{
	struct web_auth_result r = {.status = WEB_AUTH_OK};
	struct web_auth_peer peer = normalised(peer_in);
	uint8_t verifier[WEB_AUTH_VERIFIER_LEN];
	struct session_seed seed;
	int64_t now = 0;

	k_mutex_lock(&lock, K_FOREVER);
	if (st.phase == PHASE_UNINITIALISED || st.phase == PHASE_LOCKED) {
		r.status = WEB_AUTH_UNAVAILABLE;
		goto unlock;
	}
	now = now_ms();
	if (!rate_allows(&peer, now, &r.retry_after_seconds)) {
		r.status = WEB_AUTH_RATE_LIMITED;
		goto unlock;
	}
	if (st.phase == PHASE_ADMIN) {
		r.status = WEB_AUTH_SETUP_NOT_ALLOWED;
		goto unlock;
	}
	if (!token_equal(setup_token, st.setup_token, WEB_AUTH_SETUP_TOKEN_LEN)) {
		rate_fail(&peer, now);
		r.status = WEB_AUTH_SETUP_NOT_ALLOWED;
		goto unlock;
	}
	if (st.phase == PHASE_CLAIMED) {
		r.status = WEB_AUTH_BUSY;
		goto unlock;
	}
	if (!password_fits(password, password_len, WEB_AUTH_PASSWORD_MIN_CHARS)) {
		r.status = WEB_AUTH_INVALID_PASSWORD;
		goto unlock;
	}
	st.phase = PHASE_CLAIMED;
	k_mutex_unlock(&lock);

	r.status = make_verifier(password, password_len, verifier);
	if (r.status == WEB_AUTH_OK && session_seed_draw(&seed) != 0) {
		r.status = WEB_AUTH_INTERNAL;
	}
	if (r.status == WEB_AUTH_OK && st.platform->save_verifier(verifier, sizeof(verifier)) != 0) {
		r.status = WEB_AUTH_STORAGE_FAILED;
	}

	k_mutex_lock(&lock, K_FOREVER);
	if (r.status == WEB_AUTH_OK) {
		memcpy(st.verifier, verifier, sizeof(verifier));
		st.generation++;
		wipe(st.setup_token, sizeof(st.setup_token));
		st.phase = PHASE_ADMIN;
		rate_success(&peer);
		session_issue(&seed, now_ms(), out);
	} else {
		/* Nothing was stored, or storing failed: setup is open again, with
		 * the same token, so the page the operator is looking at still
		 * works. */
		st.phase = PHASE_SETUP;
	}
unlock:
	k_mutex_unlock(&lock);
	wipe(verifier, sizeof(verifier));
	wipe(&seed, sizeof(seed));

	return r;
}

struct web_auth_result web_auth_login(const char *password, size_t password_len,
				      const struct web_auth_peer *peer_in,
				      struct web_auth_session *out)
{
	struct web_auth_result r = {.status = WEB_AUTH_OK};
	struct web_auth_peer peer = normalised(peer_in);
	uint8_t verifier[WEB_AUTH_VERIFIER_LEN];
	struct session_seed seed;
	uint32_t generation = 0U;
	int64_t now = 0;

	k_mutex_lock(&lock, K_FOREVER);
	if (st.phase == PHASE_UNINITIALISED || st.phase == PHASE_LOCKED) {
		r.status = WEB_AUTH_UNAVAILABLE;
		goto unlock;
	}
	if (st.phase != PHASE_ADMIN) {
		r.status = WEB_AUTH_SETUP_NOT_ALLOWED;
		goto unlock;
	}
	now = now_ms();
	if (!rate_allows(&peer, now, &r.retry_after_seconds)) {
		r.status = WEB_AUTH_RATE_LIMITED;
		goto unlock;
	}
	if (!password_fits(password, password_len, 1)) {
		/* It cannot be the password; no derivation needed to say so, but
		 * it is still a wrong guess. */
		rate_fail(&peer, now);
		r.status = WEB_AUTH_INVALID_CREDENTIALS;
		goto unlock;
	}
	memcpy(verifier, st.verifier, sizeof(verifier));
	generation = st.generation;
	k_mutex_unlock(&lock);

	r.status = verify_against(verifier, password, password_len);
	if (r.status == WEB_AUTH_OK && session_seed_draw(&seed) != 0) {
		r.status = WEB_AUTH_INTERNAL;
	}

	k_mutex_lock(&lock, K_FOREVER);
	now = now_ms();
	if (generation != st.generation) {
		r.status = WEB_AUTH_BUSY;
	} else if (r.status == WEB_AUTH_INVALID_CREDENTIALS) {
		rate_fail(&peer, now);
	} else if (r.status == WEB_AUTH_OK) {
		rate_success(&peer);
		session_issue(&seed, now, out);
	}
unlock:
	k_mutex_unlock(&lock);
	wipe(verifier, sizeof(verifier));
	wipe(&seed, sizeof(seed));

	return r;
}

enum web_auth_status web_auth_resolve(const char *token, struct web_auth_session *out)
{
	enum web_auth_status status = WEB_AUTH_AUTHENTICATION_REQUIRED;
	struct session_slot *found = NULL;
	int64_t now;

	if (token == NULL || token[0] == '\0') {
		return WEB_AUTH_AUTHENTICATION_REQUIRED;
	}

	k_mutex_lock(&lock, K_FOREVER);
	if (st.platform == NULL) {
		goto unlock;
	}
	now = now_ms();
	/* Compare against every slot, not just until the first match. */
	for (size_t i = 0; i < ARRAY_SIZE(st.sessions); i++) {
		if (st.sessions[i].active &&
		    token_equal(token, st.sessions[i].token, WEB_AUTH_TOKEN_LEN)) {
			found = &st.sessions[i];
		}
	}
	if (found != NULL) {
		if (session_expired(found, now)) {
			session_end(found);
			status = WEB_AUTH_SESSION_EXPIRED;
		} else {
			found->last_used_ms = now;
			if (out != NULL) {
				session_describe(found, now, out);
			}
			status = WEB_AUTH_OK;
		}
	} else if (tombstone_has(token)) {
		status = WEB_AUTH_SESSION_EXPIRED;
	}
unlock:
	k_mutex_unlock(&lock);

	return status;
}

enum web_auth_status web_auth_check_csrf(const struct web_auth_session *session,
					 const char *csrf_header)
{
	if (session == NULL || !token_equal(csrf_header, session->csrf_token, WEB_AUTH_TOKEN_LEN)) {
		return WEB_AUTH_CSRF_FAILED;
	}

	return WEB_AUTH_OK;
}

enum web_auth_status web_auth_logout(const char *token)
{
	enum web_auth_status status = WEB_AUTH_AUTHENTICATION_REQUIRED;

	if (token == NULL || token[0] == '\0') {
		return status;
	}

	k_mutex_lock(&lock, K_FOREVER);
	for (size_t i = 0; i < ARRAY_SIZE(st.sessions); i++) {
		if (st.sessions[i].active &&
		    token_equal(token, st.sessions[i].token, WEB_AUTH_TOKEN_LEN)) {
			session_end(&st.sessions[i]);
			status = WEB_AUTH_OK;
		}
	}
	if (status != WEB_AUTH_OK && tombstone_has(token)) {
		status = WEB_AUTH_OK;
	}
	k_mutex_unlock(&lock);

	return status;
}

struct web_auth_result web_auth_password_change_begin(const char *current_password,
						      size_t current_len,
						      const char *new_password, size_t new_len,
						      const struct web_auth_peer *peer_in)
{
	struct web_auth_result r = {.status = WEB_AUTH_OK};
	struct web_auth_peer peer = normalised(peer_in);
	uint8_t verifier[WEB_AUTH_VERIFIER_LEN];
	uint32_t generation = 0U;
	int64_t now = 0;

	k_mutex_lock(&lock, K_FOREVER);
	if (st.phase != PHASE_ADMIN) {
		r.status = WEB_AUTH_UNAVAILABLE;
		goto unlock;
	}
	if (!password_fits(new_password, new_len, WEB_AUTH_PASSWORD_MIN_CHARS)) {
		r.status = WEB_AUTH_INVALID_PASSWORD;
		goto unlock;
	}
	if (st.change != CHANGE_IDLE) {
		r.status = WEB_AUTH_BUSY;
		goto unlock;
	}
	now = now_ms();
	if (!rate_allows(&peer, now, &r.retry_after_seconds)) {
		r.status = WEB_AUTH_RATE_LIMITED;
		goto unlock;
	}
	if (!password_fits(current_password, current_len, 1)) {
		rate_fail(&peer, now);
		r.status = WEB_AUTH_INVALID_CREDENTIALS;
		goto unlock;
	}
	st.change = CHANGE_VERIFYING;
	memcpy(verifier, st.verifier, sizeof(verifier));
	generation = st.generation;
	k_mutex_unlock(&lock);

	r.status = verify_against(verifier, current_password, current_len);

	k_mutex_lock(&lock, K_FOREVER);
	now = now_ms();
	if (generation != st.generation) {
		r.status = WEB_AUTH_BUSY;
	} else if (r.status == WEB_AUTH_INVALID_CREDENTIALS) {
		rate_fail(&peer, now);
	} else if (r.status == WEB_AUTH_OK) {
		rate_success(&peer);
		memcpy(st.pending, new_password, new_len);
		st.pending_len = new_len;
		st.change = CHANGE_PENDING;
	}
	if (r.status != WEB_AUTH_OK) {
		st.change = CHANGE_IDLE;
	}
unlock:
	k_mutex_unlock(&lock);
	wipe(verifier, sizeof(verifier));

	return r;
}

enum web_auth_status web_auth_password_change_run(void)
{
	char password[WEB_AUTH_PASSWORD_MAX_BYTES];
	uint8_t verifier[WEB_AUTH_VERIFIER_LEN];
	enum web_auth_status status;
	size_t len;

	k_mutex_lock(&lock, K_FOREVER);
	if (st.phase != PHASE_ADMIN || st.change != CHANGE_PENDING) {
		k_mutex_unlock(&lock);
		return WEB_AUTH_UNAVAILABLE;
	}
	st.change = CHANGE_RUNNING;
	len = st.pending_len;
	memcpy(password, st.pending, len);
	wipe(st.pending, sizeof(st.pending));
	st.pending_len = 0U;
	k_mutex_unlock(&lock);

	status = make_verifier(password, len, verifier);
	wipe(password, sizeof(password));
	if (status == WEB_AUTH_OK && st.platform->save_verifier(verifier, sizeof(verifier)) != 0) {
		status = WEB_AUTH_STORAGE_FAILED;
	}

	k_mutex_lock(&lock, K_FOREVER);
	if (status == WEB_AUTH_OK) {
		memcpy(st.verifier, verifier, sizeof(verifier));
		st.generation++;
		sessions_end_all();
	}
	st.change = CHANGE_IDLE;
	k_mutex_unlock(&lock);
	wipe(verifier, sizeof(verifier));

	return status;
}

void web_auth_password_change_abandon(void)
{
	k_mutex_lock(&lock, K_FOREVER);
	if (st.change == CHANGE_PENDING) {
		wipe(st.pending, sizeof(st.pending));
		st.pending_len = 0U;
		st.change = CHANGE_IDLE;
	}
	k_mutex_unlock(&lock);
}

const char *web_auth_status_str(enum web_auth_status status)
{
	static const char *const names[] = {
		[WEB_AUTH_OK] = "ok",
		[WEB_AUTH_SETUP_NOT_ALLOWED] = "setup_not_allowed",
		[WEB_AUTH_BUSY] = "busy",
		[WEB_AUTH_INVALID_CREDENTIALS] = "invalid_credentials",
		[WEB_AUTH_AUTHENTICATION_REQUIRED] = "authentication_required",
		[WEB_AUTH_SESSION_EXPIRED] = "session_expired",
		[WEB_AUTH_CSRF_FAILED] = "csrf_failed",
		[WEB_AUTH_RATE_LIMITED] = "rate_limited",
		[WEB_AUTH_INVALID_PASSWORD] = "invalid_password",
		[WEB_AUTH_UNAVAILABLE] = "unavailable",
		[WEB_AUTH_STORAGE_FAILED] = "storage_failed",
		[WEB_AUTH_INTERNAL] = "internal",
	};

	if ((size_t)status >= ARRAY_SIZE(names) || names[status] == NULL) {
		return "unknown";
	}

	return names[status];
}
