/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * web-api: routing, the middleware the contract orders, and the request and
 * response values that connect them.
 *
 * Contract: "Общие правила" and "Сессия и устройство" in
 * docs/device-development/api-contract.md, and openapi.json. The checks are
 * run in the order tools/api-contract's mock runs them (mock/app.py), because
 * the frontend is built against the mock and a different order is a different
 * answer to the same bad request.
 *
 * The shape, and why:
 *
 * - **The core is a function from a request value to a response value.**
 *   web_api_dispatch() reads a struct web_api_request and fills a struct
 *   web_api_response; it never touches a socket. The Zephyr HTTP server
 *   adapter (web_api_http.h) owns everything with timing in it: header
 *   capture, body accumulation, abort, releasing a client. So every rule of
 *   the contract is tested in the sim tier without a network, and the adapter
 *   is tested separately against the real server over loopback.
 *
 * - **The route table is data, in one X-macro file per API version**
 *   (src/web/api/v1/routes.h). Each entry names its operationId, method, path
 *   template, flags and body schema. The flags are the document's facts - no
 *   security requirement, an X-CSRF-Token parameter, an Idempotency-Key, a
 *   setup token - so tools/api-contract can read the same file and check it
 *   against openapi.json: every route is a declared operation with the same
 *   method and path and the same header requirements. That is the fifth
 *   contract check, which P1 deferred until this table existed.
 *
 * - **Middleware order** (each step runs only if the previous passed):
 *     1. Host is this device - an IP literal, `localhost`, or a name from
 *        CONFIG_WEB_API_EXTRA_HOSTS - else 403 origin_rejected. Not in the
 *        mock's list; added because the setup token is shown in the web
 *        interface, and a DNS-rebinding page whose name resolves to the
 *        device is same-origin with it in the browser's eyes. An IP literal
 *        in Host cannot be produced by such a page. The mock checks the same.
 *     2. Every header the server was asked to capture was captured, else 422
 *        validation_failed. Zephyr drops a header that does not fit its
 *        capture buffer and says only that something was dropped (measured
 *        in P2, reports/p2); acting on a request whose Cookie silently
 *        vanished would answer 401 to a signed-in browser.
 *     3. The path is a route under /api/v1, else 404 not_found - never a
 *        redirect. A known path with an undeclared method is also not_found,
 *        as the mock answers. Path parameters must be opaque ids, else 404.
 *     4. Session, unless the route is public: 401 authentication_required or
 *        session_expired.
 *     5. Origin, on the routes that run before a CSRF token exists (setup,
 *        login): absent is allowed, present must name this Host, else 403
 *        origin_rejected.
 *     6. CSRF token, on routes that declare it: 403 csrf_failed.
 *     7. Required headers: a missing X-Setup-Token is 403 setup_not_allowed;
 *        a missing or malformed Idempotency-Key, or a malformed setup token,
 *        is 422 validation_failed with no fields (the mock's DECISION: fields
 *        point into the body, not at headers).
 *     8. Body: absent where required 400 invalid_json; a body where none is
 *        declared 415; a Content-Type other than application/json 415; over
 *        the limit 413; malformed 400 invalid_json; not fitting the schema 422.
 *        On a route that keeps raw bytes (WEB_API_BODY_OCTETS, the upload
 *        chunk): absent where required 422 validation_failed, as the mock
 *        answers an empty chunk; a Content-Type other than
 *        application/octet-stream, or none, 415; over
 *        CONFIG_WEB_API_OCTET_BODY_MAX 413.
 *     9. Query: an undeclared or repeated parameter is 400 invalid_query.
 *    10. The handler. Replays of idempotent requests are resolved there,
 *        through job-manager, with the key scoped as the contract says.
 *
 * - **Idempotency scope** is "admin principal + method + canonical URL
 *   including significant query + body hash". The key given to job-manager is
 *   a 64-bit FNV-1a digest of principal, method, path and query in hex, a
 *   colon, and the client's key - so the same client key on two operations
 *   names two actions, as the mock treats it. The body hash is taken over the
 *   decoded request struct, not the raw bytes, so whitespace and member order
 *   do not turn a retry into a conflict (the mock hashes canonical JSON for
 *   the same reason).
 *
 * - **Every API response carries X-Request-ID and Cache-Control: no-store**,
 *   successes and rejections alike, and a rejection's request_id is that
 *   header's value.
 *
 * - **No response ever needs a body it cannot fit.** The response buffer is
 *   per client and fixed (CONFIG_WEB_API_RESPONSE_BODY_MAX); a handler whose
 *   JSON does not fit produces 500 internal_error, not a truncated document.
 */

#ifndef WEB_API_H_
#define WEB_API_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

#include <api_validation/api_validation.h>
#include <web_api/json_reader.h>
#include <web_api/json_writer.h>
#include <web_auth/web_auth.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Where version 1 lives. */
#define WEB_API_BASE_PATH "/api/v1"

/** Most path parameters one route template uses (upload id, then nothing). */
#define WEB_API_MAX_PATH_PARAMS 2

/** Longest path parameter value; the schemas' opaque id pattern allows 64. */
#define WEB_API_PARAM_MAX_LEN 64

/** 16 hex digits of scope digest, a colon, and the client's key. */
#define WEB_API_SCOPED_KEY_MAX_LEN (16 + 1 + API_IDEMPOTENCY_KEY_MAX_LEN)

/** Name of the session cookie. */
#define WEB_API_SESSION_COOKIE "cedar_session"

enum web_api_method {
	WEB_API_GET = 0,
	WEB_API_POST,
	WEB_API_PUT,
	WEB_API_DELETE,
	/** Anything else. Never matches a route; answered 404 like an unknown path. */
	WEB_API_METHOD_OTHER,
};

/** The request headers web-api reads. NULL when the header was absent. */
struct web_api_headers {
	const char *host;
	const char *origin;
	const char *cookie;
	const char *csrf_token;
	const char *idempotency_key;
	const char *setup_token;
	const char *content_type;
	/** For the static application: gzip negotiation and revalidation. */
	const char *accept_encoding;
	const char *if_none_match;
	/**
	 * The server dropped at least one captured header for lack of room, or
	 * a header the API reads arrived twice.
	 */
	bool dropped;
};

/** A complete request, as the adapter assembled it. */
struct web_api_request {
	enum web_api_method method;
	/** Path without the query, NUL-terminated. */
	const char *path;
	/** Everything after '?', or "" when there was none. */
	const char *query;
	struct web_api_headers headers;
	/** The body, or NULL when none arrived or it exceeded the limit. */
	const uint8_t *body;
	size_t body_len;
	/** Bytes that arrived, counted even past the limit. */
	size_t body_received;
	/** Who sent it, for the password guessing limit. */
	struct web_auth_peer peer;
	/**
	 * The device's own address the request arrived on (family 0 when the
	 * adapter could not tell): which interface it came through, for the
	 * coprocessor update's "requested over Ethernet" rule.
	 */
	struct web_auth_peer local;
};

/** One response header. Same layout as Zephyr's struct http_header. */
struct web_api_header {
	const char *name;
	const char *value;
};

/**
 * Most response headers any answer uses: the application page with gzip
 * carries nine (web_assets.h), and the adapter may add Connection.
 */
#define WEB_API_MAX_RESPONSE_HEADERS 10

/** A complete response. Every pointer is into the owning context. */
struct web_api_response {
	uint16_t status;
	/** NULL for a response without a body. */
	const char *body;
	size_t body_len;
	struct web_api_header headers[WEB_API_MAX_RESPONSE_HEADERS];
	size_t header_count;
	/**
	 * Close the connection after this response. Set for statuses that must
	 * not carry a body (204, 304): Zephyr's HTTP/1 server frames every
	 * dynamic response as chunked and sends the zero-length terminating
	 * chunk even then (measured in P2, reports/p2), and a client that
	 * correctly reads no body leaves those five bytes in front of the next
	 * response on a kept-alive connection. Closing makes the stray bytes
	 * unreadable instead of misparsed.
	 */
	bool close_connection;

	/* Storage for header values. */
	char request_id[API_REQUEST_ID_MAX_LEN + 1];
	/* cedar_session=<43>; HttpOnly; SameSite=Strict; Path=/; Max-Age=<10> */
	char cookie[128];
	char location[128];
	char retry_after[12];
};

struct web_api_call;

/** Implements one operation. Must leave a response in @p call. */
typedef void (*web_api_handler_t)(struct web_api_call *call);

/** No security requirement in the document (`security: []`). */
#define WEB_API_PUBLIC BIT(0)
/** Declares X-CSRF-Token. */
#define WEB_API_CSRF BIT(1)
/** Declares Idempotency-Key. */
#define WEB_API_IDEMPOTENT BIT(2)
/** Checks Origin instead of a CSRF token (setup and login). */
#define WEB_API_ORIGIN BIT(3)
/** Declares X-Setup-Token. */
#define WEB_API_SETUP_TOKEN BIT(4)
/** The request body is required. */
#define WEB_API_BODY_REQUIRED BIT(5)
/**
 * The body is raw bytes, `application/octet-stream`, up to
 * CONFIG_WEB_API_OCTET_BODY_MAX - the firmware upload chunk. Such a route has
 * no body schema; its handler reads web_api_call.octets.
 */
#define WEB_API_BODY_OCTETS BIT(6)

struct web_api_route {
	/** The operationId, as the document spells it. */
	const char *operation_id;
	enum web_api_method method;
	/** Template relative to WEB_API_BASE_PATH, e.g. "/jobs/{job_id}/cancel". */
	const char *path;
	uint32_t flags;
	/** NULL when the operation takes no body. */
	const struct web_json_object *body_schema;
	/** sizeof the struct the body decodes into; 0 with no schema. */
	size_t body_size;
	/** NULL-terminated names of accepted query parameters, or NULL for none. */
	const char *const *query;
	web_api_handler_t handler;
};

struct web_api_router {
	const struct web_api_route *routes;
	size_t route_count;
};

/** Size of the scratch space a body decodes into; checked per route at build time. */
#define WEB_API_DECODED_MAX CONFIG_WEB_API_DECODED_BODY_MAX

/**
 * @brief Produce the next piece of a streamed body.
 *
 * @param state  the handler's state, kept in the request's context
 * @param buf    where to write, @p cap bytes (the response buffer)
 * @return the bytes written (> 0), 0 when the body is complete, or a negative
 *         errno: the connection is closed mid-body, so the client can tell the
 *         body is incomplete. Never 0 for "nothing yet": the server reads an
 *         empty piece as the end.
 */
typedef int (*web_api_stream_next_t)(void *state, char *buf, size_t cap);

/**
 * @brief Called exactly once for every stream that was started: after the
 *        last piece, after a failed one, or when the client went away.
 */
typedef void (*web_api_stream_end_t)(void *state);

/** A response whose body is produced piece by piece. Private to web-api. */
struct web_api_stream {
	web_api_stream_next_t next;
	web_api_stream_end_t end;
	bool active;
};

/**
 * Everything one in-flight request needs, owned by the adapter, one per HTTP
 * client. Large on purpose - it holds the body, the decoded body and the
 * response - and therefore statically allocated and linked into PSRAM, never
 * on a stack.
 */
struct web_api_context {
	struct web_api_request req;
	struct web_api_response rsp;
	struct api_error error;
	/** A JSON body or an upload chunk, whichever the route takes. */
	uint8_t body[MAX(CONFIG_WEB_API_JSON_BODY_MAX, CONFIG_WEB_API_OCTET_BODY_MAX)];
	char response_body[CONFIG_WEB_API_RESPONSE_BODY_MAX];
	/** Aligned for any struct a body decodes into. */
	uint64_t decoded[DIV_ROUND_UP(WEB_API_DECODED_MAX, sizeof(uint64_t))];
	struct web_api_stream stream;
	/** A streamed response's state between pieces (web_api_reply_stream()). */
	uint64_t stream_state[DIV_ROUND_UP(CONFIG_WEB_API_STREAM_STATE_MAX, sizeof(uint64_t)) + 1];
};

/** What a handler is given. */
struct web_api_call {
	const struct web_api_request *req;
	const struct web_api_route *route;
	/** Path parameters in template order, validated as opaque ids. */
	char params[WEB_API_MAX_PATH_PARAMS][WEB_API_PARAM_MAX_LEN + 1];
	/** The resolved session; valid when @ref has_session. */
	struct web_auth_session session;
	bool has_session;
	/** The decoded body, of the route's body struct type; NULL without one. */
	const void *body;
	/**
	 * For WEB_API_BODY_OCTETS routes: the body as it arrived, NULL when none
	 * was sent. It lives in the request's context and is gone once the
	 * handler returns - copy what must outlive the call.
	 */
	const uint8_t *octets;
	size_t octets_len;
	/**
	 * For WEB_API_IDEMPOTENT routes: the key scoped as the contract requires,
	 * and a 32-bit hash of the decoded body. Hand both to job_create().
	 */
	char scoped_key[WEB_API_SCOPED_KEY_MAX_LEN + 1];
	uint32_t request_hash;

	/* Private to the implementation. */
	struct web_api_context *ctx;
	struct web_json_writer json;
	bool json_started;
	bool replied;
};

/**
 * @brief Answer one request.
 *
 * Always leaves a complete response in @p ctx->rsp: an answer from a handler,
 * or a rejection from the middleware. A handler that returns without replying
 * is a bug, answered 500 internal_error rather than an empty 200.
 */
void web_api_dispatch(const struct web_api_router *router, struct web_api_context *ctx);

/**
 * @brief Answer a request outside any route with the contract's JSON 404.
 *
 * For the server's fallback resource: a URL under /api/ that no resource
 * matched is still an API URL and is never handed to the static assets.
 */
void web_api_not_found(struct web_api_context *ctx);

/**
 * @brief The body limit for a request, decided before its body is read.
 *
 * The adapter calls this on the first callback of a request so that it knows
 * how many bytes to keep. JSON requests get CONFIG_WEB_API_JSON_BODY_MAX, routes
 * that keep raw bytes CONFIG_WEB_API_OCTET_BODY_MAX; a route with no body gets
 * 0, and anything that arrives past the limit is counted, not kept.
 */
size_t web_api_body_limit(const struct web_api_router *router, enum web_api_method method,
			  const char *path);

/* -- for handlers ------------------------------------------------------- */

/** @brief The JSON writer on the response buffer; starts the document. */
struct web_json_writer *web_api_json(struct web_api_call *call);

/** @brief Finish the JSON written so far and answer with @p status. */
void web_api_reply_json(struct web_api_call *call, uint16_t status);

/** @brief Answer with @p status and no body (204). */
void web_api_reply_empty(struct web_api_call *call, uint16_t status);

/**
 * @brief Answer 202 with a JobAccepted body, Location at the job and
 *        Retry-After: 1, as every asynchronous operation does.
 *
 * @param resource_url  The resource the job acts on, or NULL.
 */
void web_api_reply_accepted(struct web_api_call *call, const char *job_id,
			    const char *resource_url);

/** @brief Reject with one of the contract's codes. */
void web_api_reject(struct web_api_call *call, enum api_error_code code, const char *message);

/**
 * @brief Reject with the error in call->ctx->error, which the caller built
 *        with api_error_init() and field detail.
 */
void web_api_reject_error(struct web_api_call *call);

/** @brief The error a handler builds field detail into before rejecting. */
struct api_error *web_api_error(struct web_api_call *call);

/** @brief Set Retry-After; used with 429 and 202. */
void web_api_set_retry_after(struct web_api_call *call, uint32_t seconds);

/** @brief Set Location. @p url must start with WEB_API_BASE_PATH. */
void web_api_set_location(struct web_api_call *call, const char *url);

/**
 * @brief Set the session cookie: HttpOnly, SameSite=Strict, Path=/, Max-Age.
 *
 * No Secure flag: the device serves plain HTTP on a trusted network (owner's
 * decision, plan section 9), and a Secure cookie would simply never be stored.
 */
void web_api_set_session_cookie(struct web_api_call *call, const char *token,
				uint32_t max_age_seconds);

/** @brief Expire the session cookie (Max-Age=0). */
void web_api_expire_session_cookie(struct web_api_call *call);

/** @brief Map a web-auth verdict onto the error table and reject with it. */
void web_api_reject_auth(struct web_api_call *call, const struct web_auth_result *result);

/**
 * @brief Add a response header. @p value must outlive the response: a literal,
 *        or storage in the context.
 */
void web_api_add_header(struct web_api_call *call, const char *name, const char *value);

/**
 * @brief The value of query parameter @p name, decoded: `+` is a space and
 *        `%XX` a byte, as a browser's form encoding writes them.
 *
 * The middleware has already refused undeclared and repeated parameters.
 *
 * @return the value's length in bytes (the value may be empty), -ENOENT when
 *         the parameter is absent, -ENOSPC when it does not fit @p cap with its
 *         NUL, -EINVAL when it decodes to a NUL byte.
 */
int web_api_query_get(const struct web_api_request *req, const char *name, char *out, size_t cap);

/**
 * @brief Answer with @p status and a body produced piece by piece.
 *
 * For a body too large for the response buffer (the log export). The handler
 * validates first and replies last: after this call it may only fill the
 * returned state and add headers. Content-Type is @p content_type; X-Request-ID
 * and Cache-Control are sent as with every answer. The pieces are pulled by
 * the adapter (web_api_stream_pull()) into the response buffer, one per call
 * of the server, so RAM holds one piece at a time.
 *
 * @return zeroed state of @p state_size bytes that the context keeps until
 *         @p end runs, or NULL - when it exceeds CONFIG_WEB_API_STREAM_STATE_MAX,
 *         in which case the request has been answered 500.
 */
void *web_api_reply_stream(struct web_api_call *call, uint16_t status, const char *content_type,
			   size_t state_size, web_api_stream_next_t next, web_api_stream_end_t end);

/**
 * @brief Pull the next piece of @p ctx's stream into its response buffer
 *        (ctx->rsp.body, body_len).
 *
 * @param[out] final  true when there is nothing after this piece; the stream
 *                    has then ended. Also true when no stream is active.
 * @return 0, or the negative errno the handler's piece returned (the stream has
 *         ended; the connection must be closed).
 */
int web_api_stream_pull(struct web_api_context *ctx, bool *final);

/** @brief End @p ctx's stream if one is active: the client has gone. */
void web_api_stream_end(struct web_api_context *ctx);

/** @brief Whether @p ctx is in the middle of a streamed response. */
bool web_api_stream_active(const struct web_api_context *ctx);

/* -- pure helpers, exposed for the adapter and for tests ---------------- */

/**
 * @brief Is @p host (a Host header value) this device?
 *
 * An IPv4 literal, a bracketed IPv6 literal, `localhost`, or one of the
 * comma-separated names in CONFIG_WEB_API_EXTRA_HOSTS, each optionally with
 * `:port`. Case-insensitive for names.
 */
bool web_api_host_allowed(const char *host);

/**
 * @brief Extract the session token from a Cookie header.
 *
 * Copies the value of WEB_API_SESSION_COOKIE into @p out. Other cookies are
 * ignored; a cookie that appears twice is treated as absent, because which of
 * the two a browser meant cannot be known.
 *
 * @return true when exactly one was found and it fit.
 */
bool web_api_cookie_session(const char *cookie_header, char *out, size_t cap);

/** @brief 64-bit FNV-1a, the digest behind the idempotency scope. */
uint64_t web_api_fnv1a64(uint64_t state, const void *data, size_t len);

/** @brief Parse a method name; anything unknown is WEB_API_METHOD_OTHER. */
enum web_api_method web_api_method_from_str(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* WEB_API_H_ */
