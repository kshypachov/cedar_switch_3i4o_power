/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * web-auth: the single local administrator of the web interface.
 *
 * Contract: "Сессия и устройство" in docs/device-development/api-contract.md,
 * the AuthState, SetupRequest, LoginRequest, Session and PasswordRequest
 * schemas in openapi.json, and section 9 of the development plan. The owner's
 * decisions this module follows are in section 13 of the plan.
 *
 * What it is, in one paragraph. One administrator named "admin". Its password
 * is never stored, only a PBKDF2-HMAC-SHA256 verifier with a random salt. A
 * device without a verifier is in setup: it shows a setup token in the web
 * interface, and the first request that presents that token together with a
 * password becomes the administrator. Sessions are opaque random tokens held
 * in RAM, each with its own CSRF token, an idle and an absolute lifetime, and
 * all of them end on reboot. Wrong passwords are counted per client address
 * and in total, and both counts slow guessing down.
 *
 * The decisions, and why:
 *
 * - **No HTTP here.** Results are an enum, not status codes. web-api maps them
 *   onto the contract's error table, where the mapping is visible next to all
 *   the others and cannot drift between two copies.
 *
 * - **Everything outside the state machine is injected** through
 *   struct web_auth_platform: randomness, the clock, the key derivation and
 *   the store. The sim tier runs the whole module with fakes and no crypto
 *   library; the production adapters (PSA Crypto, settings-registry) are thin
 *   and tested separately where the library exists. This is the testability
 *   rule of section 12: the core decides, the adapter does I/O.
 *
 * - **The key derivation runs on the caller's thread, and the caller is the
 *   HTTP server.** Zephyr's HTTP/1 server has no deferred response: once a
 *   POST body is complete it calls the resource callback in a loop until a
 *   final response is produced (subsys/net/lib/http/http_server_http1.c,
 *   dynamic_post_put_req). Handing the derivation to a worker and waiting for
 *   it blocks the same thread just as long. So the contract's "do not block
 *   the shared HTTP loop with a long KDF" is met by bounding the cost, not by
 *   moving it: the iteration count is a latency budget measured on the board
 *   (CONFIG_WEB_AUTH_PBKDF2_ITERATIONS), and the rate limit is checked before
 *   any derivation so that a refused guess costs nothing. The one exception is
 *   the new password of a password change, which the contract makes a job and
 *   which therefore runs on a worker (web_auth_password_change_run()).
 *
 * - **The setup token is shown in the web interface** (owner's decision,
 *   2026-09-13). It therefore proves nothing about physical access: whoever
 *   opens the page first on the network sets the password. What it still
 *   does is tie setup to a client that read this device's own
 *   GET /auth/state, which a cross-site form or a script firing blind at the
 *   address cannot do. Two more things keep that true and are not optional:
 *   web-api checks Host (so a DNS-rebinding page cannot read the token as
 *   same-origin) and Origin. The token is random per boot and never logged.
 *
 * - **Setup is atomic by claim.** The first setup request that passes the
 *   token check claims the device before deriving and storing anything. A
 *   second one arriving meanwhile is told BUSY - retryable, because if the
 *   first fails to store, setup is open again - and once the verifier is
 *   stored every later one is SETUP_NOT_ALLOWED. There is exactly one winner,
 *   and a failed write leaves no half-configured administrator behind.
 *
 * - **Sessions are RAM only.** A reboot signs everyone out, as the contract
 *   says. The number of sessions is fixed; when all are in use a new login
 *   ends the one used least recently rather than refusing the administrator
 *   who is standing in front of the device with the right password.
 *
 * - **Ended sessions are remembered for a while** (tombstones), so that a
 *   browser holding a logged-out, expired or revoked cookie is told
 *   SESSION_EXPIRED - retryable, "sign in again" - rather than
 *   AUTHENTICATION_REQUIRED. The mock makes the same distinction.
 *
 * - **Guessing is limited twice.** Per client address, a few free failures
 *   and then a doubling wait; and across all clients, a fixed number of
 *   failures per window. The per-address limit alone is defeated by a client
 *   that changes address, which on IPv6 costs nothing. The global limit also
 *   delays the real administrator during a flood; on a trusted network that
 *   denial of service was preferred to an unbounded guessing rate. A
 *   successful login clears its own address's count, not the global one.
 *
 * - **A damaged stored verifier locks the interface instead of reopening
 *   setup.** Reopening setup would hand the device to whoever reached the
 *   page first after a flash fault. Instead state reports neither setup nor a
 *   usable credential and every credential check answers UNAVAILABLE. There
 *   is no recovery path in the first version (factory reset is out of scope);
 *   that is recorded as an open question in the plan, not solved here.
 *
 * Threads: every function is thread-safe. An internal mutex guards the state;
 * the key derivation and the store are called with the mutex released, so a
 * worker running a password change does not stall a GET that only resolves a
 * session. Not ISR-safe.
 *
 * Secrets: passwords are copied only where a derivation needs them and wiped
 * afterwards. Nothing here logs a password, a token or a verifier.
 */

#ifndef WEB_AUTH_H_
#define WEB_AUTH_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Session and CSRF tokens: 32 random bytes as unpadded base64url. */
#define WEB_AUTH_TOKEN_LEN 43

/** Setup token: 24 random bytes as unpadded base64url. */
#define WEB_AUTH_SETUP_TOKEN_LEN 32

/**
 * Password policy, in Unicode code points, as the schemas count it
 * (SetupRequest and PasswordRequest.new_password: 12-128; the current password
 * of a login or a change: 1-128).
 */
#define WEB_AUTH_PASSWORD_MIN_CHARS 12
#define WEB_AUTH_PASSWORD_MAX_CHARS 128
/** Worst case of WEB_AUTH_PASSWORD_MAX_CHARS in UTF-8. */
#define WEB_AUTH_PASSWORD_MAX_BYTES (WEB_AUTH_PASSWORD_MAX_CHARS * 4)

#define WEB_AUTH_SALT_LEN 16
#define WEB_AUTH_DERIVED_LEN 32

/**
 * Size of the stored verifier:
 * "CWA1" | kdf id (1) | reserved (3) | iterations u32 LE | salt | derived key.
 * The parameters travel with the verifier, so raising the iteration count
 * later does not invalidate passwords already set.
 */
#define WEB_AUTH_VERIFIER_LEN (4 + 1 + 3 + 4 + WEB_AUTH_SALT_LEN + WEB_AUTH_DERIVED_LEN)

/** What a call concluded. web-api maps each onto one row of the error table. */
enum web_auth_status {
	WEB_AUTH_OK = 0,
	/** Setup is closed, or the setup token is not this device's. */
	WEB_AUTH_SETUP_NOT_ALLOWED,
	/** Setup or a password change is already in flight. Retryable. */
	WEB_AUTH_BUSY,
	/** The password does not match. */
	WEB_AUTH_INVALID_CREDENTIALS,
	/** No session token, or one this device never issued. */
	WEB_AUTH_AUTHENTICATION_REQUIRED,
	/** A token that was valid and is not any more. Retryable: sign in again. */
	WEB_AUTH_SESSION_EXPIRED,
	/** The CSRF token is missing or belongs to another session. */
	WEB_AUTH_CSRF_FAILED,
	/** Too many failures; see web_auth_result.retry_after_seconds. */
	WEB_AUTH_RATE_LIMITED,
	/** A new password outside the length policy, or not valid UTF-8. */
	WEB_AUTH_INVALID_PASSWORD,
	/** Not initialised, or the stored verifier is unreadable. */
	WEB_AUTH_UNAVAILABLE,
	/** The store refused the new verifier; nothing changed. */
	WEB_AUTH_STORAGE_FAILED,
	/** Randomness or the key derivation failed. */
	WEB_AUTH_INTERNAL,
};

/** A status plus the wait that goes with WEB_AUTH_RATE_LIMITED. */
struct web_auth_result {
	enum web_auth_status status;
	/** Seconds until an attempt can succeed; 0 unless RATE_LIMITED. */
	uint32_t retry_after_seconds;
};

/** The client a credential attempt came from, for the per-address limit. */
struct web_auth_peer {
	/** 4, 6, or 0 when the address is unknown (all unknown share one slot). */
	uint8_t family;
	/** IPv4 in the first four bytes, IPv6 in all sixteen. */
	uint8_t addr[16];
};

/**
 * Everything the core needs from outside. All five are required.
 *
 * The functions are called without the module's mutex held and may block.
 */
struct web_auth_platform {
	/** Fill @p buf with cryptographically secure random bytes. 0 or -errno. */
	int (*random)(uint8_t *buf, size_t len);
	/** Monotonic milliseconds; lifetimes and rate limits use nothing else. */
	int64_t (*now_ms)(void);
	/**
	 * PBKDF2-HMAC-SHA256. 0 or -errno. The core never calls it with more than
	 * CONFIG_WEB_AUTH_PBKDF2_MAX_ITERATIONS.
	 */
	int (*kdf)(const uint8_t *password, size_t password_len, const uint8_t *salt,
		   size_t salt_len, uint32_t iterations, uint8_t *out, size_t out_len);
	/**
	 * Read the stored verifier. 0 with @p out_len set, -ENOENT if none has
	 * ever been stored, any other -errno if the store failed.
	 */
	int (*load_verifier)(uint8_t *buf, size_t cap, size_t *out_len);
	/** Replace the stored verifier, durably, before returning 0. */
	int (*save_verifier)(const uint8_t *buf, size_t len);
};

/** GET /auth/state, plus what the device itself needs to know. */
struct web_auth_state {
	/** No administrator exists yet. */
	bool setup_required;
	/** Setup would be accepted now (required, and nobody is mid-setup). */
	bool setup_allowed;
	/** False when the stored verifier is damaged; see the header comment. */
	bool credential_usable;
	/** The token to present to setup; empty unless @ref setup_allowed. */
	char setup_token[WEB_AUTH_SETUP_TOKEN_LEN + 1];
};

/** A session as the API shows it (the Session schema) plus its cookie value. */
struct web_auth_session {
	/** The cookie value. Never put in a response body or a log. */
	char token[WEB_AUTH_TOKEN_LEN + 1];
	char csrf_token[WEB_AUTH_TOKEN_LEN + 1];
	uint32_t idle_timeout_seconds;
	uint32_t absolute_remaining_seconds;
};

/**
 * @brief Start, or restart, the module.
 *
 * Reads the stored verifier. With none, the device is in setup and a fresh
 * setup token is drawn. With a damaged one the interface is locked (see the
 * header comment). Ends every session and forgets every failure count, which
 * is what a reboot does; the sim tier calls it between cases for the same
 * reason.
 *
 * @retval 0        ready (in setup, or with an administrator)
 * @retval -EINVAL  @p platform is NULL or incomplete
 * @retval -EIO     the store failed (not merely empty); credential checks
 *                  answer UNAVAILABLE until a later init succeeds
 * @retval -EBADMSG the stored verifier is damaged; the interface is locked
 * @retval -EFAULT  the random source failed while drawing the setup token
 */
int web_auth_init(const struct web_auth_platform *platform);

/** @brief Snapshot of setup state. @p out must not be NULL. */
void web_auth_get_state(struct web_auth_state *out);

/**
 * @brief POST /auth/setup: create the administrator and sign it in.
 *
 * Order: availability, rate limit, setup still open, token, claim, password
 * policy, derive, store, session. A wrong token counts as a failure for the
 * rate limit, like a wrong password does. A request to an already configured
 * device is SETUP_NOT_ALLOWED without counting: it is not a guess. The password
 * policy is checked after the claim so that a losing concurrent request learns
 * BUSY rather than a policy complaint about a setup that is not going to be its
 * own.
 *
 * @param setup_token  X-Setup-Token as received; NULL counts as wrong.
 * @param password     UTF-8, not NUL-terminated.
 * @param out          The new session on WEB_AUTH_OK.
 * @return OK, SETUP_NOT_ALLOWED, BUSY, RATE_LIMITED, INVALID_PASSWORD,
 *         STORAGE_FAILED (setup reopens), INTERNAL (setup reopens),
 *         UNAVAILABLE
 */
struct web_auth_result web_auth_setup(const char *setup_token, const char *password,
				      size_t password_len, const struct web_auth_peer *peer,
				      struct web_auth_session *out);

/**
 * @brief POST /auth/session: sign in with the administrator password.
 *
 * A password change that completes while this login is deriving makes the
 * verdict stale; the login then answers BUSY, retryable, instead of issuing a
 * session for a password that no longer exists.
 *
 * @return OK, INVALID_CREDENTIALS, RATE_LIMITED, SETUP_NOT_ALLOWED (there is
 *         no administrator yet - the mock answers the same), BUSY,
 *         UNAVAILABLE, INTERNAL
 */
struct web_auth_result web_auth_login(const char *password, size_t password_len,
				      const struct web_auth_peer *peer,
				      struct web_auth_session *out);

/**
 * @brief Find the session behind a cookie and mark it used.
 *
 * Expiry is decided here, on use, against the injected clock: past its idle or
 * absolute lifetime a session ends and its holder is told SESSION_EXPIRED.
 * Using a session extends only the idle lifetime.
 *
 * @param token  The cookie value; NULL or empty when there was none.
 * @param out    The session, with remaining lifetimes, on WEB_AUTH_OK. May be
 *               NULL when only the verdict matters.
 * @return OK, AUTHENTICATION_REQUIRED, SESSION_EXPIRED
 */
enum web_auth_status web_auth_resolve(const char *token, struct web_auth_session *out);

/**
 * @brief Compare a request's X-CSRF-Token with its session's, in constant time.
 *
 * @param session      As returned by web_auth_resolve().
 * @param csrf_header  The header value; NULL when absent.
 * @return OK or CSRF_FAILED
 */
enum web_auth_status web_auth_check_csrf(const struct web_auth_session *session,
					 const char *csrf_header);

/**
 * @brief DELETE /auth/session: end one session. Ending an ended one is OK.
 *
 * @return OK, AUTHENTICATION_REQUIRED (a token never issued)
 */
enum web_auth_status web_auth_logout(const char *token);

/**
 * @brief PUT /auth/password, the synchronous half: check the current
 *        password and hold the new one for the job.
 *
 * The contract makes a password change a job, and the mock decided (recorded
 * in tools/api-contract/README.md) that a wrong current password is answered
 * on the request itself as 401 invalid_credentials. So the current password is
 * verified here, on the HTTP thread, and only a request that passes becomes a
 * job. The new password is checked against the policy first - before any
 * derivation, and matching the mock, which rejects a short new password as a
 * schema violation before its handler runs - and copied into a single pending
 * slot; web_auth_password_change_run() consumes it.
 *
 * @return OK (a change is now pending), INVALID_CREDENTIALS, INVALID_PASSWORD,
 *         RATE_LIMITED, BUSY (a change is already pending or running),
 *         UNAVAILABLE
 */
struct web_auth_result web_auth_password_change_begin(const char *current_password,
						      size_t current_len,
						      const char *new_password, size_t new_len,
						      const struct web_auth_peer *peer);

/**
 * @brief The job half: derive and store the pending password, then end every
 *        session.
 *
 * Runs on a worker. On success every session ends - including the one that
 * asked - so every browser signs in again with the new password, as the
 * contract requires. On failure nothing changes, the old password still works,
 * and the pending slot is cleared either way.
 *
 * @return OK, STORAGE_FAILED, INTERNAL, or UNAVAILABLE when nothing is pending
 */
enum web_auth_status web_auth_password_change_run(void);

/**
 * @brief Drop a pending change that will never run, e.g. because the job
 *        could not be created. Wipes the held password.
 */
void web_auth_password_change_abandon(void);

/** @brief Stable lower-case name of a status, for logs and tests. */
const char *web_auth_status_str(enum web_auth_status status);

#ifdef __cplusplus
}
#endif

#endif /* WEB_AUTH_H_ */
