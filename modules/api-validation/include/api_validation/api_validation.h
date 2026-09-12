/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Validation and error codes for the web API: one place where a rejection gets
 * its code, its HTTP status, its field paths and its JSON shape, so that thirty
 * route handlers do not each invent their own.
 *
 * The contract this implements is the "Общие правила" section of
 * docs/device-development/api-contract.md — the error table and the ErrorDetail
 * example — together with the Error, ErrorDetail and ErrorField schemas in
 * openapi.json. Three rules from there shape the design:
 *
 *  - A code determines its HTTP status. The table pairs them, and a handler
 *    that could choose the status separately would eventually return
 *    `stale_revision` with a 422 on one route and a 409 on another. Here the
 *    status is a property of the code and there is no way to override it.
 *  - `retryable` is a promise to the client about whether repeating the request
 *    could succeed, so it follows from the code as well. `busy` is retryable;
 *    `validation_failed` never is, no matter how many times it is sent.
 *  - An error body never carries a stack trace or a secret. The message is a
 *    fixed-size buffer written by our own code, and there is no path by which
 *    request content reaches it.
 *
 * Design notes worth knowing before using this:
 *
 *  - No dynamic allocation, no threads, no I/O. An error is a value the caller
 *    owns; serialising it writes into a buffer the caller supplies.
 *  - A struct api_error is a few hundred bytes and belongs in a request
 *    context, not on a deep stack.
 *  - The field list is bounded. Overflowing it is recorded rather than hidden,
 *    because "there were more problems than we told you about" is itself
 *    information the client needs.
 *  - The parsers here are strict on purpose. `010.0.0.1`, `1.2.3.4 ` and
 *    `1.2.3.4.5` are all rejected. An address that a lenient parser silently
 *    reinterprets is an address the operator did not configure.
 *
 * What this module does not do: it holds no policy. Whether a static address
 * needs a gateway, which interface must stay reachable, whether a recovery path
 * exists — those need context this module does not have, and belong to
 * network-manager, which composes the primitives here with its own rules.
 */

#ifndef API_VALIDATION_H_
#define API_VALIDATION_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Longest error message, excluding the terminator. Ours, never the client's. */
#define API_ERROR_MESSAGE_MAX_LEN 127
/** Longest JSON Pointer in a field error, e.g. "/interfaces/ethernet/ipv4/address". */
#define API_ERROR_PATH_MAX_LEN 63
/** Request id length, matching `^[A-Za-z0-9_-]{1,64}$` in openapi.json. */
#define API_REQUEST_ID_MAX_LEN 64

/** Idempotency key bounds from the contract: 16-64 ASCII characters. */
#define API_IDEMPOTENCY_KEY_MIN_LEN 16
#define API_IDEMPOTENCY_KEY_MAX_LEN 64

/**
 * @brief Every error code in the contract's table, with its status attached.
 *
 * Grouped by the status each maps to. Adding a code means adding it to the
 * table in api-contract.md first: this enum is a transcription of that table,
 * not an independent list.
 */
enum api_error_code {
	/* 400 */
	API_ERR_INVALID_JSON = 0,
	API_ERR_INVALID_QUERY,
	API_ERR_INVALID_CURSOR,
	/* 401 */
	API_ERR_AUTHENTICATION_REQUIRED,
	API_ERR_INVALID_CREDENTIALS,
	API_ERR_SESSION_EXPIRED,
	/* 403 */
	API_ERR_CSRF_FAILED,
	API_ERR_ORIGIN_REJECTED,
	API_ERR_SETUP_NOT_ALLOWED,
	/* 404 */
	API_ERR_NOT_FOUND,
	/* 409 */
	API_ERR_BUSY,
	API_ERR_STALE_REVISION,
	API_ERR_INVALID_STATE,
	API_ERR_OFFSET_MISMATCH,
	API_ERR_IDEMPOTENCY_CONFLICT,
	API_ERR_ETHERNET_REQUIRED,
	/* 410 */
	API_ERR_RESOURCE_EXPIRED,
	API_ERR_BOOT_CHANGED,
	/* 413 */
	API_ERR_PAYLOAD_TOO_LARGE,
	/* 415 */
	API_ERR_UNSUPPORTED_MEDIA_TYPE,
	/* 422 */
	API_ERR_VALIDATION_FAILED,
	API_ERR_INVALID_IMAGE,
	API_ERR_UNSUPPORTED_TARGET,
	API_ERR_INCOMPATIBLE_FIRMWARE,
	API_ERR_SIGNATURE_INVALID,
	/* 429 */
	API_ERR_RATE_LIMITED,
	/* 500 */
	API_ERR_INTERNAL_ERROR,
	/* 503 */
	API_ERR_SERVICE_NOT_READY,
	API_ERR_CAPABILITY_UNAVAILABLE,
	/* 507 */
	API_ERR_STORAGE_FULL,

	API_ERR_COUNT
};

/**
 * @brief Why one particular field was rejected.
 *
 * The contract fixes the shape of ErrorField but leaves its `code` a free
 * string, and shows only `required`. This is our set: small, closed, and
 * stable, so a client can branch on it. Extending it is a contract change like
 * any other.
 */
enum api_field_code {
	/** Absent, or null where a value is required. */
	API_FIELD_REQUIRED = 0,
	/** Present but not the right shape: a malformed address, a bad enum. */
	API_FIELD_INVALID_FORMAT,
	/** A number outside its documented bounds. */
	API_FIELD_OUT_OF_RANGE,
	/** A string or array longer than the schema permits. */
	API_FIELD_TOO_LONG,
	/** A field the schema does not define; the contract rejects these. */
	API_FIELD_UNKNOWN,
	/** Valid alone, but contradicts another field of the same request. */
	API_FIELD_CONFLICTING,
	/** Well-formed and understood, but not permitted in the current state. */
	API_FIELD_NOT_ALLOWED,

	API_FIELD_CODE_COUNT
};

/** One entry of ErrorDetail.fields. */
struct api_error_field {
	char path[API_ERROR_PATH_MAX_LEN + 1];
	enum api_field_code code;
};

/**
 * @brief A complete rejection, ready to serialise.
 *
 * Build one with api_error_init(), add field detail, then render it with
 * api_error_to_json(). The HTTP status and the retryable flag come from the
 * code and are not separately settable.
 */
struct api_error {
	enum api_error_code code;
	char message[API_ERROR_MESSAGE_MAX_LEN + 1];
	char request_id[API_REQUEST_ID_MAX_LEN + 1];
	struct api_error_field fields[CONFIG_API_VALIDATION_MAX_FIELDS];
	uint8_t field_count;
	/**
	 * Set when more fields were reported than fit. api_error_to_json()
	 * appends a note to the rendered message when it is set, since
	 * ErrorDetail is additionalProperties false and there is nowhere else
	 * to put one. Silently dropping half the problems makes a form
	 * impossible to fix.
	 */
	bool fields_truncated;
	/** Seconds for the Retry-After header. Meaningful when @ref code is rate limited. */
	uint16_t retry_after_seconds;
};

/** @brief HTTP status for a code, per the table in api-contract.md. */
uint16_t api_error_status(enum api_error_code code);

/** @brief Stable wire name, e.g. "idempotency_conflict". NULL if out of range. */
const char *api_error_str(enum api_error_code code);

/**
 * @brief Whether repeating the identical request could succeed.
 *
 * A property of the code, not of the moment: `busy` and `service_not_ready`
 * are true because the condition is transient, while `validation_failed` and
 * `csrf_failed` are false because nothing changes by asking again.
 */
bool api_error_is_retryable(enum api_error_code code);

/** @brief Stable wire name for a field code, e.g. "invalid_format". */
const char *api_field_code_str(enum api_field_code code);

/**
 * @brief Start an error.
 *
 * @param err        Receives the error. Fully overwritten.
 * @param code       What went wrong.
 * @param message    Human-readable, truncated to fit. Must describe the
 *                   problem without quoting the request: this string reaches
 *                   the client, and a message that echoes input is how a
 *                   password ends up in a log.
 * @param request_id Correlates with the X-Request-ID response header. NULL
 *                   generates one.
 * @retval 0        built
 * @retval -EINVAL  bad arguments
 */
int api_error_init(struct api_error *err, enum api_error_code code, const char *message,
		   const char *request_id);

/**
 * @brief Add one field-level detail.
 *
 * @param path  JSON Pointer into the request body, e.g.
 *              "/interfaces/ethernet/ipv4/address". Leading slash required.
 * @retval 0        added
 * @retval -ENOSPC  the list is full; the error is marked truncated and this
 *                  detail is dropped. Not fatal, and not worth aborting for.
 * @retval -EINVAL  bad arguments, or a path that is not a JSON Pointer
 */
int api_error_add_field(struct api_error *err, const char *path, enum api_field_code code);

/** @brief Set the Retry-After value, in seconds. */
int api_error_set_retry_after(struct api_error *err, uint16_t seconds);

/**
 * @brief Render the error as the contract's JSON body.
 *
 * Produces `{"error":{...}}` exactly as the Error schema defines it. Every
 * string is escaped, so a path or message containing a quote cannot break the
 * document.
 *
 * @return bytes written excluding the terminator, or -ENOMEM when @p cap is
 *         too small. On -ENOMEM nothing usable is left in @p buf: a truncated
 *         JSON body is worse than none, because the client cannot tell it
 *         apart from a complete one.
 */
int api_error_to_json(const struct api_error *err, char *buf, size_t cap);

/**
 * @brief Seed the request id generator.
 *
 * Ids are unique within a boot, and two boots start from different points so
 * that logs from either cannot be confused. They are not secrets: a request id
 * is a correlation handle that is never accepted as input, so it needs no
 * entropy. Tests seed explicitly; production seeds from the cycle counter at
 * first use.
 */
void api_request_id_seed(uint32_t seed);

/**
 * @brief Generate the next request id, e.g. "req_3f8a91c2".
 *
 * @param cap  Must be at least 13 bytes.
 * @retval 0        written
 * @retval -ENOMEM  @p cap too small
 */
int api_request_id_generate(char *buf, size_t cap);

/**
 * @brief Check an Idempotency-Key header value.
 *
 * The contract requires 16-64 ASCII characters. Printable ASCII only: the
 * value is echoed in logs and used as a map key, and a control character in
 * either is a problem with no upside.
 */
bool api_validate_idempotency_key(const char *key);

/**
 * @brief Check a request id against `^[A-Za-z0-9_-]{1,64}$`.
 *
 * The same pattern guards every opaque identifier in the contract — job ids,
 * transaction ids, upload ids — because all of them end up in a URL path.
 */
bool api_validate_opaque_id(const char *id);

/**
 * @brief Parse dotted-quad IPv4 into four bytes in network order.
 *
 * Strict: exactly four decimal octets of 0-255, no leading zeros, no spaces,
 * nothing trailing. `010.0.0.1` is rejected rather than read as octal, which
 * is a real divergence between parsers and not a theoretical one.
 *
 * @retval 0        parsed
 * @retval -EINVAL  not a valid address
 */
int api_parse_ipv4(const char *text, uint8_t out[4]);

/**
 * @brief Parse IPv6 into sixteen bytes in network order.
 *
 * Supports `::` compression and a trailing embedded IPv4 (`::ffff:192.0.2.1`).
 * Zone identifiers (`%eth0`) are rejected: a DNS server address with a scope
 * is not something this product can act on, and accepting one would mean
 * storing a string the resolver cannot use.
 *
 * @retval 0        parsed
 * @retval -EINVAL  not a valid address
 */
int api_parse_ipv6(const char *text, uint8_t out[16]);

/** @brief Whether an IPv4 prefix length is one the contract permits, i.e. 1-30. */
bool api_ipv4_prefix_is_valid(uint8_t prefix_length);

/** @brief Whether two IPv4 addresses share a prefix. */
bool api_ipv4_same_subnet(const uint8_t a[4], const uint8_t b[4], uint8_t prefix_length);

/**
 * @brief Whether an address can belong to a host on its own subnet.
 *
 * False for the network and broadcast addresses of the prefix, and for 0.0.0.0
 * and the loopback, multicast and reserved ranges. A configuration that
 * assigns one of these is accepted by the kernel and then does not work, which
 * is the worst way for it to fail.
 */
bool api_ipv4_is_usable_host(const uint8_t addr[4], uint8_t prefix_length);

#ifdef __cplusplus
}
#endif

#endif /* API_VALIDATION_H_ */
