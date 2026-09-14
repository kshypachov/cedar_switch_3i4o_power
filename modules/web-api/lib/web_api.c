/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * web-api core. The design is in include/web_api/web_api.h; comments here
 * cover only how it is carried out.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <job_manager/job_manager.h>
#include <web_api/web_api.h>

LOG_MODULE_REGISTER(web_api, CONFIG_WEB_API_LOG_LEVEL);

BUILD_ASSERT(sizeof(struct web_api_header) == 2 * sizeof(const char *),
	     "web_api_header must stay layout-compatible with struct http_header");
BUILD_ASSERT(JOB_IDEMPOTENCY_KEY_MAX_LEN >= WEB_API_SCOPED_KEY_MAX_LEN,
	     "job-manager must hold a scoped idempotency key");

#define FNV64_OFFSET 0xcbf29ce484222325ULL
#define FNV64_PRIME  0x100000001b3ULL

static const char *const method_names[] = {
	[WEB_API_GET] = "GET",
	[WEB_API_POST] = "POST",
	[WEB_API_PUT] = "PUT",
	[WEB_API_DELETE] = "DELETE",
};

/* -- pure helpers ------------------------------------------------------- */

uint64_t web_api_fnv1a64(uint64_t state, const void *data, size_t len)
{
	const uint8_t *p = data;

	for (size_t i = 0; i < len; i++) {
		state ^= p[i];
		state *= FNV64_PRIME;
	}

	return state;
}

enum web_api_method web_api_method_from_str(const char *name)
{
	for (size_t i = 0; i < ARRAY_SIZE(method_names); i++) {
		if (name != NULL && strcmp(name, method_names[i]) == 0) {
			return (enum web_api_method)i;
		}
	}

	return WEB_API_METHOD_OTHER;
}

static char lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool equal_nocase(const char *a, size_t na, const char *b, size_t nb)
{
	if (na != nb) {
		return false;
	}
	for (size_t i = 0; i < na; i++) {
		if (lower(a[i]) != lower(b[i])) {
			return false;
		}
	}

	return true;
}

static bool port_valid(const char *s, size_t n)
{
	uint32_t v = 0;

	if (n == 0U || n > 5U) {
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		if (s[i] < '0' || s[i] > '9') {
			return false;
		}
		v = v * 10U + (uint32_t)(s[i] - '0');
	}

	return v <= 65535U;
}

static bool name_listed(const char *name, size_t n)
{
	const char *list = CONFIG_WEB_API_EXTRA_HOSTS;

	if (equal_nocase(name, n, "localhost", 9)) {
		return true;
	}
	while (*list != '\0') {
		const char *comma = strchr(list, ',');
		size_t len = comma ? (size_t)(comma - list) : strlen(list);
		const char *start = list;

		while (len > 0U && *start == ' ') {
			start++;
			len--;
		}
		while (len > 0U && start[len - 1U] == ' ') {
			len--;
		}
		if (len > 0U && equal_nocase(name, n, start, len)) {
			return true;
		}
		if (comma == NULL) {
			break;
		}
		list = comma + 1;
	}

	return false;
}

bool web_api_host_allowed(const char *host)
{
	char buf[64];
	const char *end;
	size_t n;

	if (host == NULL) {
		/* No Host at all: not a browser, so not a rebinding page. */
		return true;
	}
	n = strlen(host);
	if (n == 0U || n >= sizeof(buf)) {
		return false;
	}

	if (host[0] == '[') {
		uint8_t addr[16];

		end = strchr(host, ']');
		if (end == NULL) {
			return false;
		}
		if (end[1] != '\0' && (end[1] != ':' || !port_valid(end + 2, strlen(end + 2)))) {
			return false;
		}
		n = (size_t)(end - host - 1);
		memcpy(buf, host + 1, n);
		buf[n] = '\0';
		return api_parse_ipv6(buf, addr) == 0;
	}

	end = strchr(host, ':');
	if (end != NULL) {
		if (!port_valid(end + 1, strlen(end + 1))) {
			return false;
		}
		n = (size_t)(end - host);
	}
	memcpy(buf, host, n);
	buf[n] = '\0';

	uint8_t v4[4];

	return api_parse_ipv4(buf, v4) == 0 || name_listed(buf, n);
}

bool web_api_cookie_session(const char *cookie_header, char *out, size_t cap)
{
	static const char name[] = WEB_API_SESSION_COOKIE;
	const char *p = cookie_header;
	const char *value = NULL;
	size_t value_len = 0;
	int found = 0;

	if (cookie_header == NULL || cap == 0U) {
		return false;
	}
	while (*p != '\0') {
		const char *semi = strchr(p, ';');
		size_t len = semi ? (size_t)(semi - p) : strlen(p);
		const char *eq;

		while (len > 0U && (*p == ' ' || *p == '\t')) {
			p++;
			len--;
		}
		eq = memchr(p, '=', len);
		if (eq != NULL && (size_t)(eq - p) == sizeof(name) - 1U &&
		    memcmp(p, name, sizeof(name) - 1U) == 0) {
			value = eq + 1;
			value_len = len - (sizeof(name) - 1U) - 1U;
			while (value_len > 0U &&
			       (value[value_len - 1U] == ' ' || value[value_len - 1U] == '\t')) {
				value_len--;
			}
			found++;
		}
		if (semi == NULL) {
			break;
		}
		p = semi + 1;
	}

	if (found != 1 || value_len == 0U || value_len >= cap) {
		return false;
	}
	memcpy(out, value, value_len);
	out[value_len] = '\0';

	return true;
}

/* -- responses ---------------------------------------------------------- */

static void add_header(struct web_api_response *rsp, const char *name, const char *value)
{
	if (rsp->header_count < ARRAY_SIZE(rsp->headers)) {
		rsp->headers[rsp->header_count].name = name;
		rsp->headers[rsp->header_count].value = value;
		rsp->header_count++;
	}
}

static void start_response(struct web_api_context *ctx)
{
	struct web_api_response *rsp = &ctx->rsp;
	char request_id[sizeof(rsp->request_id)];

	/* Keep the id across a reset: a rejection must report the id the
	 * request was given, not a second one. */
	memcpy(request_id, rsp->request_id, sizeof(request_id));
	memset(rsp, 0, sizeof(*rsp));
	memcpy(rsp->request_id, request_id, sizeof(request_id));
	add_header(rsp, "X-Request-ID", rsp->request_id);
	add_header(rsp, "Cache-Control", "no-store");
}

static void respond_error(struct web_api_context *ctx, struct api_error *err)
{
	struct web_api_response *rsp = &ctx->rsp;
	int len;

	memcpy(err->request_id, rsp->request_id, sizeof(err->request_id));
	start_response(ctx);
	rsp->status = api_error_status(err->code);
	len = api_error_to_json(err, ctx->response_body, sizeof(ctx->response_body));
	if (len < 0) {
		/* A rejection that does not fit its own buffer: say so briefly. */
		struct api_error fallback;

		(void)api_error_init(&fallback, API_ERR_INTERNAL_ERROR, "Response too large",
				     rsp->request_id);
		len = api_error_to_json(&fallback, ctx->response_body, sizeof(ctx->response_body));
		rsp->status = api_error_status(API_ERR_INTERNAL_ERROR);
	}
	rsp->body = ctx->response_body;
	rsp->body_len = len > 0 ? (size_t)len : 0U;
	add_header(rsp, "Content-Type", "application/json");
	if (err->retry_after_seconds > 0U) {
		(void)snprintf(rsp->retry_after, sizeof(rsp->retry_after), "%u",
			       (unsigned int)err->retry_after_seconds);
		add_header(rsp, "Retry-After", rsp->retry_after);
	}
}

static void reject_ctx(struct web_api_context *ctx, enum api_error_code code, const char *message)
{
	(void)api_error_init(&ctx->error, code, message, ctx->rsp.request_id);
	respond_error(ctx, &ctx->error);
}

struct web_json_writer *web_api_json(struct web_api_call *call)
{
	if (!call->json_started) {
		web_json_writer_init(&call->json, call->ctx->response_body,
				     sizeof(call->ctx->response_body));
		call->json_started = true;
	}

	return &call->json;
}

void web_api_reply_json(struct web_api_call *call, uint16_t status)
{
	struct web_api_context *ctx = call->ctx;
	int len = web_json_writer_finish(web_api_json(call));

	if (len < 0) {
		LOG_ERR("%s: response does not fit (%zu bytes)", call->route->operation_id,
			sizeof(ctx->response_body));
		web_api_reject(call, API_ERR_INTERNAL_ERROR, "The response did not fit");
		return;
	}
	ctx->rsp.status = status;
	ctx->rsp.body = ctx->response_body;
	ctx->rsp.body_len = (size_t)len;
	add_header(&ctx->rsp, "Content-Type", "application/json");
	call->replied = true;
}

void web_api_reply_empty(struct web_api_call *call, uint16_t status)
{
	call->ctx->rsp.status = status;
	call->ctx->rsp.body = NULL;
	call->ctx->rsp.body_len = 0U;
	call->replied = true;
}

void web_api_reply_accepted(struct web_api_call *call, const char *job_id,
			    const char *resource_url)
{
	struct web_json_writer *w = web_api_json(call);
	char job_url[sizeof(WEB_API_BASE_PATH "/jobs/") + JOB_ID_MAX_LEN];

	(void)snprintf(job_url, sizeof(job_url), WEB_API_BASE_PATH "/jobs/%s", job_id);
	web_json_object_begin(w);
	web_json_key(w, "job_id");
	web_json_string(w, job_id);
	web_json_key(w, "job_url");
	web_json_string(w, job_url);
	web_json_key(w, "resource_url");
	web_json_string_or_null(w, resource_url);
	web_json_object_end(w);
	web_api_set_location(call, job_url);
	web_api_set_retry_after(call, 1U);
	web_api_reply_json(call, 202);
}

void web_api_reject(struct web_api_call *call, enum api_error_code code, const char *message)
{
	reject_ctx(call->ctx, code, message);
	call->replied = true;
}

struct api_error *web_api_error(struct web_api_call *call)
{
	return &call->ctx->error;
}

void web_api_reject_error(struct web_api_call *call)
{
	respond_error(call->ctx, &call->ctx->error);
	call->replied = true;
}

void web_api_set_retry_after(struct web_api_call *call, uint32_t seconds)
{
	struct web_api_response *rsp = &call->ctx->rsp;

	(void)snprintf(rsp->retry_after, sizeof(rsp->retry_after), "%u", (unsigned int)seconds);
	add_header(rsp, "Retry-After", rsp->retry_after);
}

void web_api_set_location(struct web_api_call *call, const char *url)
{
	struct web_api_response *rsp = &call->ctx->rsp;

	(void)snprintf(rsp->location, sizeof(rsp->location), "%s", url);
	add_header(rsp, "Location", rsp->location);
}

void web_api_set_session_cookie(struct web_api_call *call, const char *token,
				uint32_t max_age_seconds)
{
	struct web_api_response *rsp = &call->ctx->rsp;

	(void)snprintf(rsp->cookie, sizeof(rsp->cookie),
		       WEB_API_SESSION_COOKIE "=%s; HttpOnly; SameSite=Strict; Path=/; Max-Age=%u",
		       token, (unsigned int)max_age_seconds);
	add_header(rsp, "Set-Cookie", rsp->cookie);
}

void web_api_expire_session_cookie(struct web_api_call *call)
{
	struct web_api_response *rsp = &call->ctx->rsp;

	(void)snprintf(rsp->cookie, sizeof(rsp->cookie),
		       WEB_API_SESSION_COOKIE "=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0");
	add_header(rsp, "Set-Cookie", rsp->cookie);
}

void web_api_reject_auth(struct web_api_call *call, const struct web_auth_result *result)
{
	struct web_api_context *ctx = call->ctx;
	enum api_error_code code;
	const char *message;

	switch (result->status) {
	case WEB_AUTH_SETUP_NOT_ALLOWED:
		code = API_ERR_SETUP_NOT_ALLOWED;
		message = "Setup is not allowed: the device is configured or the token is wrong";
		break;
	case WEB_AUTH_BUSY:
		code = API_ERR_BUSY;
		message = "Another setup or password change is in progress; retry";
		break;
	case WEB_AUTH_INVALID_CREDENTIALS:
		code = API_ERR_INVALID_CREDENTIALS;
		message = "The password is not correct";
		break;
	case WEB_AUTH_AUTHENTICATION_REQUIRED:
		code = API_ERR_AUTHENTICATION_REQUIRED;
		message = "This resource requires an administrator session";
		break;
	case WEB_AUTH_SESSION_EXPIRED:
		code = API_ERR_SESSION_EXPIRED;
		message = "The session has ended; sign in again";
		break;
	case WEB_AUTH_CSRF_FAILED:
		code = API_ERR_CSRF_FAILED;
		message = "The CSRF token is missing or does not match the session";
		break;
	case WEB_AUTH_RATE_LIMITED:
		code = API_ERR_RATE_LIMITED;
		message = "Too many failed attempts; wait before trying again";
		break;
	case WEB_AUTH_INVALID_PASSWORD:
		code = API_ERR_VALIDATION_FAILED;
		message = "The password does not meet the policy";
		break;
	case WEB_AUTH_UNAVAILABLE:
		code = API_ERR_SERVICE_NOT_READY;
		message = "Sign-in is unavailable on this device";
		break;
	case WEB_AUTH_STORAGE_FAILED:
		code = API_ERR_INTERNAL_ERROR;
		message = "The credential could not be stored";
		break;
	default:
		code = API_ERR_INTERNAL_ERROR;
		message = "Authentication failed internally";
		break;
	}

	(void)api_error_init(&ctx->error, code, message, ctx->rsp.request_id);
	if (result->status == WEB_AUTH_RATE_LIMITED) {
		(void)api_error_set_retry_after(&ctx->error,
						(uint16_t)MIN(result->retry_after_seconds,
							      (uint32_t)UINT16_MAX));
	}
	respond_error(ctx, &ctx->error);
	call->replied = true;
}

void web_api_add_header(struct web_api_call *call, const char *name, const char *value)
{
	add_header(&call->ctx->rsp, name, value);
}

/* -- streamed responses --------------------------------------------------- */

void *web_api_reply_stream(struct web_api_call *call, uint16_t status, const char *content_type,
			   size_t state_size, web_api_stream_next_t next, web_api_stream_end_t end)
{
	struct web_api_context *ctx = call->ctx;

	if (state_size > CONFIG_WEB_API_STREAM_STATE_MAX || next == NULL) {
		LOG_ERR("%s: stream state %zu does not fit %d", call->route->operation_id,
			state_size, CONFIG_WEB_API_STREAM_STATE_MAX);
		web_api_reject(call, API_ERR_INTERNAL_ERROR, "The response could not be streamed");
		return NULL;
	}

	memset(ctx->stream_state, 0, sizeof(ctx->stream_state));
	ctx->stream.next = next;
	ctx->stream.end = end;
	ctx->stream.active = true;
	ctx->rsp.status = status;
	ctx->rsp.body = NULL;
	ctx->rsp.body_len = 0U;
	add_header(&ctx->rsp, "Content-Type", content_type);
	call->replied = true;

	return ctx->stream_state;
}

bool web_api_stream_active(const struct web_api_context *ctx)
{
	return ctx->stream.active;
}

void web_api_stream_end(struct web_api_context *ctx)
{
	if (!ctx->stream.active) {
		return;
	}
	ctx->stream.active = false;
	if (ctx->stream.end != NULL) {
		ctx->stream.end(ctx->stream_state);
	}
}

int web_api_stream_pull(struct web_api_context *ctx, bool *final)
{
	int n;

	*final = true;
	ctx->rsp.body = NULL;
	ctx->rsp.body_len = 0U;
	if (!ctx->stream.active) {
		return 0;
	}

	n = ctx->stream.next(ctx->stream_state, ctx->response_body, sizeof(ctx->response_body));
	if (n <= 0) {
		web_api_stream_end(ctx);
		return n;
	}
	ctx->rsp.body = ctx->response_body;
	ctx->rsp.body_len = MIN((size_t)n, sizeof(ctx->response_body));
	*final = false;

	return 0;
}

/* -- query values ---------------------------------------------------------- */

static int hex_digit(char c);

int web_api_query_get(const struct web_api_request *req, const char *name, char *out, size_t cap)
{
	const char *p = req->query;
	const size_t want = strlen(name);

	while (p != NULL && *p != '\0') {
		const char *amp = strchr(p, '&');
		size_t len = amp ? (size_t)(amp - p) : strlen(p);
		const char *eq = memchr(p, '=', len);
		size_t name_len = eq ? (size_t)(eq - p) : len;

		/* Names the route declares are plain ASCII, so they are compared
		 * as written; query_ok() has already refused anything else. */
		if (len > 0U && name_len == want && memcmp(p, name, want) == 0) {
			const char *v = eq ? eq + 1 : p + len;
			size_t vn = eq ? len - name_len - 1U : 0U;
			size_t n = 0;

			for (size_t i = 0; i < vn; i++) {
				char c = v[i];

				if (c == '+') {
					c = ' ';
				} else if (c == '%' && i + 2U < vn && hex_digit(v[i + 1]) >= 0 &&
					   hex_digit(v[i + 2]) >= 0) {
					c = (char)(hex_digit(v[i + 1]) * 16 + hex_digit(v[i + 2]));
					i += 2U;
					if (c == '\0') {
						return -EINVAL;
					}
				}
				if (n + 1U >= cap) {
					return -ENOSPC;
				}
				out[n++] = c;
			}
			if (cap == 0U) {
				return -ENOSPC;
			}
			out[n] = '\0';
			return (int)n;
		}
		if (amp == NULL) {
			break;
		}
		p = amp + 1;
	}

	return -ENOENT;
}

/* -- routing ------------------------------------------------------------ */

/* Match @p tail against @p tmpl, capturing {parameters}. */
static bool path_matches(const char *tmpl, const char *tail,
			 char params[WEB_API_MAX_PATH_PARAMS][WEB_API_PARAM_MAX_LEN + 1],
			 bool *params_valid)
{
	size_t param = 0;

	*params_valid = true;
	while (*tmpl != '\0' && *tail != '\0') {
		if (*tmpl != '/' || *tail != '/') {
			return false;
		}
		tmpl++;
		tail++;
		const char *te = strchr(tmpl, '/');
		const char *se = strchr(tail, '/');
		size_t tn = te ? (size_t)(te - tmpl) : strlen(tmpl);
		size_t sn = se ? (size_t)(se - tail) : strlen(tail);

		if (tn >= 2U && tmpl[0] == '{' && tmpl[tn - 1U] == '}') {
			if (sn == 0U) {
				return false;
			}
			if (param < WEB_API_MAX_PATH_PARAMS) {
				if (sn > WEB_API_PARAM_MAX_LEN) {
					*params_valid = false;
				} else {
					memcpy(params[param], tail, sn);
					params[param][sn] = '\0';
					if (!api_validate_opaque_id(params[param])) {
						*params_valid = false;
					}
				}
			}
			param++;
		} else if (tn != sn || memcmp(tmpl, tail, tn) != 0) {
			return false;
		}
		tmpl += tn;
		tail += sn;
	}

	return *tmpl == '\0' && *tail == '\0';
}

static const struct web_api_route *find_route(const struct web_api_router *router,
					      enum web_api_method method, const char *path,
					      struct web_api_call *call, bool *path_known,
					      bool *params_valid)
{
	const char *tail;

	*path_known = false;
	*params_valid = true;
	if (strncmp(path, WEB_API_BASE_PATH, sizeof(WEB_API_BASE_PATH) - 1U) != 0 ||
	    path[sizeof(WEB_API_BASE_PATH) - 1U] != '/') {
		return NULL;
	}
	tail = path + sizeof(WEB_API_BASE_PATH) - 1U;

	for (size_t i = 0; i < router->route_count; i++) {
		const struct web_api_route *route = &router->routes[i];
		char params[WEB_API_MAX_PATH_PARAMS][WEB_API_PARAM_MAX_LEN + 1];
		bool valid;

		if (!path_matches(route->path, tail, params, &valid)) {
			continue;
		}
		*path_known = true;
		if (route->method != method) {
			continue;
		}
		*params_valid = valid;
		if (call != NULL) {
			memcpy(call->params, params, sizeof(params));
		}
		return route;
	}

	return NULL;
}

size_t web_api_body_limit(const struct web_api_router *router, enum web_api_method method,
			  const char *path)
{
	bool known;
	bool valid;
	const struct web_api_route *route = find_route(router, method, path, NULL, &known, &valid);

	if (route == NULL || route->body_schema == NULL) {
		return 0U;
	}

	return CONFIG_WEB_API_JSON_BODY_MAX;
}

/* -- middleware steps ---------------------------------------------------- */

static bool origin_ok(const struct web_api_headers *h)
{
	const char *netloc;
	const char *slash;
	size_t n;

	if (h->origin == NULL) {
		/* A browser always sends Origin on a cross-site POST, which is the
		 * case defended against; refusing its absence only breaks curl. */
		return true;
	}
	netloc = strstr(h->origin, "://");
	if (netloc == NULL) {
		return false; /* "null", or not a URL */
	}
	netloc += 3;
	slash = strchr(netloc, '/');
	n = slash ? (size_t)(slash - netloc) : strlen(netloc);

	return h->host != NULL && strlen(h->host) == n && memcmp(netloc, h->host, n) == 0;
}

static bool is_json_type(const char *content_type)
{
	static const char json[] = "application/json";
	const char *p = content_type;
	size_t n;

	while (*p == ' ') {
		p++;
	}
	n = strcspn(p, "; ");

	return equal_nocase(p, n, json, sizeof(json) - 1U);
}

static int hex_digit(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	c = lower(c);
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	return -1;
}

/* Every query parameter must be declared, and none may repeat. */
static bool query_ok(const struct web_api_route *route, const char *query, const char **why)
{
	const char *p = query;
	uint32_t seen = 0;

	while (*p != '\0') {
		const char *amp = strchr(p, '&');
		size_t len = amp ? (size_t)(amp - p) : strlen(p);
		const char *eq = memchr(p, '=', len);
		size_t name_len = eq ? (size_t)(eq - p) : len;
		char name[32];
		size_t n = 0;
		bool declared = false;

		for (size_t i = 0; i < name_len; i++) {
			char c = p[i];

			if (c == '+') {
				c = ' ';
			} else if (c == '%' && i + 2U < name_len && hex_digit(p[i + 1]) >= 0 &&
				   hex_digit(p[i + 2]) >= 0) {
				c = (char)(hex_digit(p[i + 1]) * 16 + hex_digit(p[i + 2]));
				i += 2U;
			}
			if (n + 1U < sizeof(name)) {
				name[n++] = c;
			}
		}
		name[n] = '\0';

		if (len > 0U) {
			if (route->query != NULL) {
				for (size_t k = 0; route->query[k] != NULL && k < 32U; k++) {
					if (strcmp(route->query[k], name) == 0) {
						if (seen & BIT(k)) {
							*why = "A query parameter was given more than once";
							return false;
						}
						seen |= BIT(k);
						declared = true;
					}
				}
			}
			if (!declared) {
				*why = "A query parameter is not defined for this operation";
				return false;
			}
		}
		if (amp == NULL) {
			break;
		}
		p = amp + 1;
	}

	return true;
}

static void scope_idempotency(struct web_api_call *call)
{
	const struct web_api_request *req = call->req;
	uint64_t h = FNV64_OFFSET;
	static const char zero = '\0';
	const char *principal = call->has_session ? "admin" : "-";

	h = web_api_fnv1a64(h, principal, strlen(principal));
	h = web_api_fnv1a64(h, &zero, 1);
	h = web_api_fnv1a64(h, method_names[req->method], strlen(method_names[req->method]));
	h = web_api_fnv1a64(h, &zero, 1);
	h = web_api_fnv1a64(h, req->path, strlen(req->path));
	h = web_api_fnv1a64(h, &zero, 1);
	h = web_api_fnv1a64(h, req->query, strlen(req->query));
	(void)snprintf(call->scoped_key, sizeof(call->scoped_key), "%016llx:%s",
		       (unsigned long long)h, req->headers.idempotency_key);

	if (call->body != NULL) {
		call->request_hash =
			(uint32_t)web_api_fnv1a64(FNV64_OFFSET, call->body, call->route->body_size);
	} else {
		call->request_hash = 0U;
	}
}

/* -- dispatch ------------------------------------------------------------ */

void web_api_not_found(struct web_api_context *ctx)
{
	(void)api_request_id_generate(ctx->rsp.request_id, sizeof(ctx->rsp.request_id));
	reject_ctx(ctx, API_ERR_NOT_FOUND, "Not an API resource");
}

void web_api_dispatch(const struct web_api_router *router, struct web_api_context *ctx)
{
	const struct web_api_request *req = &ctx->req;
	const struct web_api_headers *h = &req->headers;
	struct web_api_call call;
	const struct web_api_route *route;
	bool path_known;
	bool params_valid;
	const char *why = NULL;

	/* A context reused before its last stream was told to end (the adapter
	 * always tells it; a test might not) must not leak that stream's state. */
	web_api_stream_end(ctx);
	memset(&call, 0, sizeof(call));
	call.ctx = ctx;
	call.req = req;
	ctx->rsp.request_id[0] = '\0';
	(void)api_request_id_generate(ctx->rsp.request_id, sizeof(ctx->rsp.request_id));
	start_response(ctx);

	/* 1. Host */
	if (!web_api_host_allowed(h->host)) {
		reject_ctx(ctx, API_ERR_ORIGIN_REJECTED, "Host is not this device");
		return;
	}

	/* 2. Captured headers */
	if (h->dropped) {
		reject_ctx(ctx, API_ERR_VALIDATION_FAILED,
			   "Request headers exceed what the device can read");
		return;
	}

	/* 3. Route */
	route = find_route(router, req->method, req->path, &call, &path_known, &params_valid);
	if (route == NULL) {
		reject_ctx(ctx, API_ERR_NOT_FOUND,
			   path_known ? "This method is not defined for the resource"
				      : "Not an API resource");
		return;
	}
	if (!params_valid) {
		reject_ctx(ctx, API_ERR_NOT_FOUND, "A path parameter is not a valid identifier");
		return;
	}
	call.route = route;

	/* 4. Session */
	if (!(route->flags & WEB_API_PUBLIC)) {
		char token[WEB_AUTH_TOKEN_LEN + 1];
		struct web_auth_result r = {.status = WEB_AUTH_AUTHENTICATION_REQUIRED};

		if (web_api_cookie_session(h->cookie, token, sizeof(token))) {
			r.status = web_auth_resolve(token, &call.session);
		}
		if (r.status != WEB_AUTH_OK) {
			web_api_reject_auth(&call, &r);
			return;
		}
		call.has_session = true;
	}

	/* 5. Origin */
	if ((route->flags & WEB_API_ORIGIN) && !origin_ok(h)) {
		reject_ctx(ctx, API_ERR_ORIGIN_REJECTED, "Origin is not this device");
		return;
	}

	/* 6. CSRF */
	if (route->flags & WEB_API_CSRF) {
		struct web_auth_result r = {
			.status = web_auth_check_csrf(call.has_session ? &call.session : NULL,
						      h->csrf_token),
		};

		if (r.status != WEB_AUTH_OK) {
			web_api_reject_auth(&call, &r);
			return;
		}
	}

	/* 7. Required headers */
	if (route->flags & WEB_API_SETUP_TOKEN) {
		size_t n = h->setup_token ? strlen(h->setup_token) : 0U;

		if (h->setup_token == NULL) {
			reject_ctx(ctx, API_ERR_SETUP_NOT_ALLOWED, "X-Setup-Token is required for setup");
			return;
		}
		if (n < 16U || n > 128U) {
			reject_ctx(ctx, API_ERR_VALIDATION_FAILED, "X-Setup-Token is not acceptable");
			return;
		}
	}
	if (route->flags & WEB_API_IDEMPOTENT) {
		if (h->idempotency_key == NULL) {
			reject_ctx(ctx, API_ERR_VALIDATION_FAILED,
				   "The Idempotency-Key header is required");
			return;
		}
		if (!api_validate_idempotency_key(h->idempotency_key)) {
			reject_ctx(ctx, API_ERR_VALIDATION_FAILED, "Idempotency-Key is not acceptable");
			return;
		}
	}

	/* 8. Body */
	if (route->body_schema == NULL) {
		if (req->body_received > 0U) {
			reject_ctx(ctx, API_ERR_UNSUPPORTED_MEDIA_TYPE,
				   "This operation takes no request body");
			return;
		}
	} else if (req->body_received == 0U) {
		if (route->flags & WEB_API_BODY_REQUIRED) {
			reject_ctx(ctx, API_ERR_INVALID_JSON, "A JSON body is required");
			return;
		}
	} else {
		if (h->content_type != NULL && h->content_type[0] != '\0' &&
		    !is_json_type(h->content_type)) {
			reject_ctx(ctx, API_ERR_UNSUPPORTED_MEDIA_TYPE, "The body must be application/json");
			return;
		}
		if (req->body == NULL || req->body_received > CONFIG_WEB_API_JSON_BODY_MAX) {
			reject_ctx(ctx, API_ERR_PAYLOAD_TOO_LARGE, "The JSON body is too large");
			return;
		}
		__ASSERT(route->body_size <= sizeof(ctx->decoded), "route body struct too large");
		if (web_json_decode((const char *)req->body, req->body_len, route->body_schema,
				    ctx->decoded, route->body_size, &ctx->error) != 0) {
			respond_error(ctx, &ctx->error);
			return;
		}
		call.body = ctx->decoded;
	}

	/* 9. Query */
	if (!query_ok(route, req->query, &why)) {
		reject_ctx(ctx, API_ERR_INVALID_QUERY, why);
		return;
	}

	/* 10. Handler */
	if (route->flags & WEB_API_IDEMPOTENT) {
		scope_idempotency(&call);
	}
	route->handler(&call);
	if (!call.replied) {
		LOG_ERR("%s returned without a response", route->operation_id);
		reject_ctx(ctx, API_ERR_INTERNAL_ERROR, "The operation produced no response");
	}
	if (ctx->rsp.status == 204 || ctx->rsp.status == 304) {
		ctx->rsp.close_connection = true;
	}
}
