/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * web-api's adapter and web-assets, through Zephyr's HTTP server over
 * loopback. The service is defined the way src/web/web_server.c defines the
 * firmware's - [::] with IPv4 mapped, the fallback, and the resources of
 * src/web/api/v1/http_resources.h - so what passes here is the firmware's
 * request path minus the network driver.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/net/socket.h>
#include <zephyr/ztest.h>

#include <job_manager/job_manager.h>
#include <log_store/log_store.h>
#include <web_api/web_api_http.h>
#include <web_assets/web_assets.h>
#include <web_auth/web_auth.h>

#include "web_api_v1.h"

#define PORT 8080

/* -- the service, as the firmware defines it ----------------------------- */

static void answer_static(struct web_api_context *ctx)
{
	const struct web_assets_request req = {
		.is_get = ctx->req.method == WEB_API_GET,
		.path = ctx->req.path,
		.accept_encoding = ctx->req.headers.accept_encoding,
		.if_none_match = ctx->req.headers.if_none_match,
	};
	struct web_assets_response out;

	web_assets_respond(&web_assets, &req, &out);
	ctx->rsp.status = out.status;
	ctx->rsp.body = (const char *)out.body;
	ctx->rsp.body_len = out.body_len;
	for (size_t i = 0; i < out.header_count; i++) {
		ctx->rsp.headers[i].name = out.headers[i].name;
		ctx->rsp.headers[i].value = out.headers[i].value;
	}
	ctx->rsp.header_count = out.header_count;
	ctx->rsp.close_connection = out.close_connection;
}

static int web_callback(struct http_client_ctx *client, enum http_transaction_status status,
			const struct http_request_ctx *request_ctx,
			struct http_response_ctx *response_ctx, void *user_data)
{
	ARG_UNUSED(user_data);
	return web_api_http_callback(&web_api_v1_router, answer_static, client, status,
				     request_ctx, response_ctx);
}

static uint16_t port = PORT;
static struct http_resource_detail_dynamic fallback_detail = {
	.common = {.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		   .bitmask_of_supported_http_methods = WEB_API_HTTP_METHODS},
	.cb = web_callback,
};
HTTP_SERVICE_DEFINE(test_service, "::", &port, CONFIG_HTTP_SERVER_MAX_CLIENTS, 10, NULL,
		    &fallback_detail.common, NULL);

#define WEB_API_V1_RESOURCE(_name, _pattern)                                                       \
	static struct http_resource_detail_dynamic _name##_detail = {                              \
		.common = {.type = HTTP_RESOURCE_TYPE_DYNAMIC,                                     \
			   .bitmask_of_supported_http_methods = WEB_API_HTTP_METHODS,              \
			   .content_type = "application/json"},                                    \
		.cb = web_callback,                                                                \
	};                                                                                         \
	HTTP_RESOURCE_DEFINE(_name, test_service, _pattern, &_name##_detail);
#include "http_resources.h"

/* -- a web-auth platform that costs nothing ------------------------------ */

static uint32_t counter = 1;

static int rnd(uint8_t *b, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		counter = counter * 1103515245U + 12345U;
		b[i] = (uint8_t)(counter >> 16);
	}
	return 0;
}

static int64_t now(void)
{
	return k_uptime_get();
}

static int kdf(const uint8_t *p, size_t pl, const uint8_t *s, size_t sl, uint32_t it, uint8_t *o,
	       size_t ol)
{
	uint64_t h = 1469598103934665603ULL ^ it;

	for (size_t i = 0; i < pl; i++) {
		h = (h ^ p[i]) * 1099511628211ULL;
	}
	for (size_t i = 0; i < sl; i++) {
		h = (h ^ s[i]) * 1099511628211ULL;
	}
	for (size_t i = 0; i < ol; i++) {
		h = (h ^ i) * 1099511628211ULL;
		o[i] = (uint8_t)(h >> 30);
	}
	return 0;
}

static uint8_t stored[128];
static size_t stored_len;

static int load(uint8_t *b, size_t cap, size_t *len)
{
	if (stored_len == 0) {
		return -ENOENT;
	}
	memcpy(b, stored, stored_len);
	*len = stored_len;
	return 0;
}

static int save(const uint8_t *b, size_t len)
{
	memcpy(stored, b, len);
	stored_len = len;
	return 0;
}

static const struct web_auth_platform platform = {rnd, now, kdf, load, save};

static const struct web_api_v1_identity identity = {
	"cedar-test", "cedar_switch_3in4out_power", "fw-test", "fixture-1", "boot_http",
};

/* -- a client -------------------------------------------------------------- */

static char rsp[16384];
static int rsp_len;
static bool rsp_closed;

static int connect_to(int family)
{
	struct net_sockaddr_storage ss = {0};
	net_socklen_t len;
	struct timeval tv = {.tv_sec = 0, .tv_usec = 400000};
	int fd = zsock_socket(family, NET_SOCK_STREAM, NET_IPPROTO_TCP);

	zassert_true(fd >= 0, "socket: %d", errno);
	zsock_setsockopt(fd, ZSOCK_SOL_SOCKET, ZSOCK_SO_RCVTIMEO, &tv, sizeof(tv));
	if (family == NET_AF_INET) {
		struct net_sockaddr_in *in = (struct net_sockaddr_in *)&ss;

		in->sin_family = NET_AF_INET;
		in->sin_port = net_htons(PORT);
		zsock_inet_pton(NET_AF_INET, "127.0.0.1", &in->sin_addr);
		len = sizeof(*in);
	} else {
		struct net_sockaddr_in6 *in6 = (struct net_sockaddr_in6 *)&ss;

		in6->sin6_family = NET_AF_INET6;
		in6->sin6_port = net_htons(PORT);
		zsock_inet_pton(NET_AF_INET6, "::1", &in6->sin6_addr);
		len = sizeof(*in6);
	}
	zassert_ok(zsock_connect(fd, (struct net_sockaddr *)&ss, len), "connect: %d", errno);
	return fd;
}

static int client(void)
{
	return connect_to(NET_AF_INET);
}

static void send_all(int fd, const char *data, size_t n)
{
	while (n > 0) {
		int sent = zsock_send(fd, data, n, 0);

		zassert_true(sent > 0, "send: %d", errno);
		data += sent;
		n -= (size_t)sent;
	}
}

static void send_str(int fd, const char *s)
{
	send_all(fd, s, strlen(s));
}

/* Read until the peer closes or goes quiet. */
static void receive(int fd)
{
	rsp_len = 0;
	rsp_closed = false;
	for (;;) {
		int n = zsock_recv(fd, rsp + rsp_len, sizeof(rsp) - 1 - rsp_len, 0);

		if (n == 0) {
			rsp_closed = true;
			break;
		}
		if (n < 0) {
			break;
		}
		rsp_len += n;
	}
	rsp[rsp_len] = '\0';
}

static int status(void)
{
	int code = 0;

	sscanf(rsp, "HTTP/1.1 %d", &code);
	return code;
}

static bool has(const char *text)
{
	return strstr(rsp, text) != NULL;
}

/* One complete exchange on a fresh connection. */
static void exchange(const char *request)
{
	int fd = client();

	send_str(fd, request);
	receive(fd);
	zsock_close(fd);
}

static void wait_contexts_released(void)
{
	for (int i = 0; i < 100 && web_api_http_contexts_in_use() > 0; i++) {
		k_msleep(20);
	}
}

/* -- suite ----------------------------------------------------------------- */

static void *suite_setup(void)
{
	zassert_ok(web_api_v1_init(&identity));
	return NULL;
}

static void before(void *f)
{
	ARG_UNUSED(f);
	stored_len = 0;
	job_manager_init();
	zassert_ok(web_auth_init(&platform));
	zassert_ok(http_server_start());
	k_msleep(20);
}

static void after(void *f)
{
	ARG_UNUSED(f);
	http_server_stop();
	k_msleep(50);
	zassert_equal(web_api_http_contexts_in_use(), 0, "every context released");
}

ZTEST_SUITE(web_api_http, NULL, suite_setup, before, after, NULL);

ZTEST(web_api_http, test_api_answer_over_ipv4_and_ipv6)
{
	static const char req[] = "GET /api/v1/auth/state HTTP/1.1\r\nHost: 192.168.88.14\r\n\r\n";
	int families[] = {NET_AF_INET, NET_AF_INET6};

	for (size_t i = 0; i < ARRAY_SIZE(families); i++) {
		int fd = connect_to(families[i]);

		send_str(fd, req);
		receive(fd);
		zsock_close(fd);
		zassert_true(has("HTTP/1.1 200 OK\r\n"), "complete status line: %s", rsp);
		zassert_true(has("X-Request-ID: req_"));
		zassert_true(has("Cache-Control: no-store\r\n"));
		zassert_true(has("Content-Type: application/json\r\n"));
		zassert_true(has("\"setup_required\":true"));
	}
}

ZTEST(web_api_http, test_unknown_api_urls_are_json_404_for_every_method)
{
	static const char *const reqs[] = {
		"GET /api/v1/nope HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n",
		"POST /api/v1/nope HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 2\r\n\r\n{}",
		"DELETE /api/v2/x HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n",
		"PATCH /api/v1/auth/state HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n",
		"PUT /api/v1/jobs/x/cancel HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n",
		"GET /api HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n",
	};

	for (size_t i = 0; i < ARRAY_SIZE(reqs); i++) {
		exchange(reqs[i]);
		zassert_equal(status(), 404, "%s -> %s", reqs[i], rsp);
		zassert_true(has("\"code\":\"not_found\""), "%s", rsp);
		zassert_false(has("Location:"), "never a redirect");
	}
}

ZTEST(web_api_http, test_spa_fallback_and_static_files)
{
	exchange("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
	zassert_equal(status(), 200);
	zassert_true(has("Content-Type: text/html; charset=utf-8"));
	zassert_true(has("Content-Security-Policy: default-src 'self'"));
	zassert_true(has("Cache-Control: no-cache"));
	zassert_true(has("<div id=\"root\">"));

	exchange("GET /network/wifi HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
	zassert_equal(status(), 200, "a route of the application gets the page");
	zassert_true(has("<div id=\"root\">"));

	exchange("GET /assets/missing-AbCdEf12.js HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
	zassert_equal(status(), 404, "a missing file is not the page");
	zassert_false(has("<div id=\"root\">"));

	exchange("GET /robots.txt HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
	zassert_equal(status(), 404);

	exchange("POST /settings HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n");
	zassert_equal(status(), 405);
}

ZTEST(web_api_http, test_gzip_asset_negotiation_and_revalidation)
{
	char etag[32] = {0};
	char req[256];
	const char *p;

	exchange("GET /assets/app-AbCdEf12.js HTTP/1.1\r\nHost: 127.0.0.1\r\n"
		 "Accept-Encoding: gzip, deflate, br\r\n\r\n");
	zassert_equal(status(), 200, "%s", rsp);
	zassert_true(has("Content-Encoding: gzip\r\n"));
	zassert_true(has("Vary: Accept-Encoding\r\n"));
	zassert_true(has("Cache-Control: public, max-age=31536000, immutable\r\n"));
	zassert_true(has("Content-Type: text/javascript; charset=utf-8\r\n"));
	p = strstr(rsp, "ETag: ");
	zassert_not_null(p);
	sscanf(p, "ETag: %31s", etag);

	exchange("GET /assets/app-AbCdEf12.js HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
	zassert_equal(status(), 406, "gzip-only file, gzip not accepted");

	snprintf(req, sizeof(req),
		 "GET /assets/app-AbCdEf12.js HTTP/1.1\r\nHost: 127.0.0.1\r\n"
		 "Accept-Encoding: gzip\r\nIf-None-Match: W/%s\r\n\r\n",
		 etag);
	exchange(req);
	zassert_equal(status(), 304, "%s", rsp);
	zassert_true(has("Connection: close\r\n"), "the stray chunk terminator must not be read");
}

ZTEST(web_api_http, test_fragmented_body)
{
	int fd = client();
	static const char head[] = "POST /api/v1/auth/session HTTP/1.1\r\nHost: 127.0.0.1\r\n"
				   "Content-Type: application/json\r\nContent-Length: 32\r\n\r\n";
	static const char body[] = "{\"password\":\"a fine password\"}  ";

	send_str(fd, head);
	k_msleep(30);
	send_all(fd, body, 10);
	k_msleep(30);
	send_all(fd, body + 10, 12);
	k_msleep(30);
	send_all(fd, body + 22, 10);
	receive(fd);
	zsock_close(fd);
	zassert_equal(status(), 403, "the whole body arrived: %s", rsp);
	zassert_true(has("\"code\":\"setup_not_allowed\""));
}

ZTEST(web_api_http, test_oversized_body_is_413_and_not_kept)
{
	static char req[10000];
	int n = snprintf(req, sizeof(req),
			 "POST /api/v1/auth/session HTTP/1.1\r\nHost: 127.0.0.1\r\n"
			 "Content-Length: 9000\r\n\r\n{\"password\":\"");
	int fd = client();

	/* The body is exactly Content-Length: the 13 bytes of {"password":" that
	 * are already in req, 8985 of padding and the closing "}. One byte more
	 * would be parsed by the server as the start of a second request. */
	memset(req + n, 'a', 9000 - 13 - 2);
	memcpy(req + n + 9000 - 13 - 2, "\"}", 2);
	send_all(fd, req, n + 9000 - 13);
	receive(fd);
	zsock_close(fd);
	zassert_equal(status(), 413, "%s", rsp);
	zassert_true(has("\"code\":\"payload_too_large\""));
	wait_contexts_released();
	zassert_equal(web_api_http_contexts_in_use(), 0);
}

ZTEST(web_api_http, test_abort_mid_body_releases_the_context)
{
	int fd = client();

	send_str(fd, "POST /api/v1/auth/session HTTP/1.1\r\nHost: 127.0.0.1\r\n"
		     "Content-Length: 100\r\n\r\n{\"password\":");
	k_msleep(100);
	zassert_equal(web_api_http_contexts_in_use(), 1, "held while the body is incomplete");
	zsock_close(fd);
	wait_contexts_released();
	zassert_equal(web_api_http_contexts_in_use(), 0, "released by the server's abort");

	exchange("GET /api/v1/auth/state HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
	zassert_equal(status(), 200, "and the resource is free again");
}

ZTEST(web_api_http, test_stalled_client_is_released_by_the_timeout)
{
	int fd = client();
	int64_t start = k_uptime_get();

	send_str(fd, "POST /api/v1/auth/session HTTP/1.1\r\nHost: 127.0.0.1\r\n"
		     "Content-Length: 100\r\n\r\n{");
	k_msleep(100);
	zassert_equal(web_api_http_contexts_in_use(), 1);
	while (web_api_http_contexts_in_use() > 0 && k_uptime_get() - start < 5000) {
		k_msleep(50);
	}
	zassert_equal(web_api_http_contexts_in_use(), 0);
	zassert_true(k_uptime_get() - start >= CONFIG_HTTP_SERVER_CLIENT_INACTIVITY_TIMEOUT * 1000 - 200,
		     "released by the inactivity timeout, not earlier");
	zsock_close(fd);
}

ZTEST(web_api_http, test_two_clients)
{
	int writer = client();

	/* One client is in the middle of a body on /auth/session... */
	send_str(writer, "POST /api/v1/auth/session HTTP/1.1\r\nHost: 127.0.0.1\r\n"
			 "Content-Length: 30\r\n\r\n{\"password\":");
	k_msleep(50);

	/* ...another is served on a different resource meanwhile... */
	exchange("GET /api/v1/auth/state HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
	zassert_equal(status(), 200, "%s", rsp);
	exchange("GET /assets/app-AbCdEf12.js HTTP/1.1\r\nHost: 127.0.0.1\r\nAccept-Encoding: gzip\r\n\r\n");
	zassert_equal(status(), 200);

	/* ...and on the same resource gets the server's bare 409, which the
	 * frontend treats as busy. Pinned so a change in Zephyr is noticed. */
	exchange("GET /api/v1/auth/session HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
	zassert_equal(status(), 409, "%s", rsp);
	zassert_false(has("\"error\""), "no JSON: the server answers before any callback");

	send_str(writer, "\"a fine password\"}");
	receive(writer);
	zsock_close(writer);
	zassert_equal(status(), 403, "the first client's request completed normally: %s", rsp);
}

ZTEST(web_api_http, test_headers_the_server_could_not_keep)
{
	static char req[1024];
	char cookie[300];

	memset(cookie, 'c', sizeof(cookie) - 1);
	cookie[sizeof(cookie) - 1] = '\0';
	snprintf(req, sizeof(req),
		 "GET /api/v1/auth/session HTTP/1.1\r\nHost: 127.0.0.1\r\nCookie: cedar_session=%s\r\n\r\n",
		 cookie);
	exchange(req);
	zassert_equal(status(), 422, "a dropped Cookie is not a missing session: %s", rsp);

	exchange("GET /api/v1/auth/state HTTP/1.1\r\nHost: 127.0.0.1\r\nHost: evil.example\r\n\r\n");
	zassert_equal(status(), 422, "two Host headers: which one counts cannot be known");
}

ZTEST(web_api_http, test_foreign_host)
{
	exchange("GET /api/v1/auth/state HTTP/1.1\r\nHost: rebound.example\r\n\r\n");
	zassert_equal(status(), 403);
	zassert_true(has("\"code\":\"origin_rejected\""));
	zassert_false(has("setup_token"));
}

static bool header_value(const char *name, char *out, size_t cap)
{
	const char *p = strstr(rsp, name);
	const char *end;

	if (p == NULL) {
		return false;
	}
	p += strlen(name);
	end = strpbrk(p, ";\r");
	if (end == NULL || (size_t)(end - p) >= cap) {
		return false;
	}
	memcpy(out, p, end - p);
	out[end - p] = '\0';
	return true;
}

static bool json_value(const char *name, char *out, size_t cap)
{
	char key[40];
	const char *p;
	const char *end;

	snprintf(key, sizeof(key), "\"%s\":\"", name);
	p = strstr(rsp, key);
	if (p == NULL) {
		return false;
	}
	p += strlen(key);
	end = strchr(p, '"');
	if (end == NULL || (size_t)(end - p) >= cap) {
		return false;
	}
	memcpy(out, p, end - p);
	out[end - p] = '\0';
	return true;
}

ZTEST(web_api_http, test_setup_login_logout_over_the_wire)
{
	char token[64], cookie[64], csrf[64], req[512];

	exchange("GET /api/v1/auth/state HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
	zassert_true(json_value("setup_token", token, sizeof(token)));

	snprintf(req, sizeof(req),
		 "POST /api/v1/auth/setup HTTP/1.1\r\nHost: 127.0.0.1:8080\r\n"
		 "Origin: http://127.0.0.1:8080\r\nX-Setup-Token: %s\r\n"
		 "Content-Type: application/json\r\nContent-Length: 32\r\n\r\n"
		 "{\"password\":\"a fine password\"}  ",
		 token);
	exchange(req);
	zassert_equal(status(), 201, "%s", rsp);
	zassert_true(has("Set-Cookie: cedar_session="));
	zassert_true(has("; HttpOnly; SameSite=Strict; Path=/; Max-Age="));
	zassert_false(has("Secure"), "plain HTTP: a Secure cookie would never be stored");
	zassert_true(header_value("Set-Cookie: cedar_session=", cookie, sizeof(cookie)));
	zassert_true(json_value("csrf_token", csrf, sizeof(csrf)));

	snprintf(req, sizeof(req),
		 "GET /api/v1/auth/session HTTP/1.1\r\nHost: 127.0.0.1\r\nCookie: cedar_session=%s\r\n\r\n",
		 cookie);
	exchange(req);
	zassert_equal(status(), 200, "%s", rsp);

	snprintf(req, sizeof(req),
		 "DELETE /api/v1/auth/session HTTP/1.1\r\nHost: 127.0.0.1\r\n"
		 "Cookie: cedar_session=%s\r\nX-CSRF-Token: %s\r\n\r\n",
		 cookie, csrf);
	exchange(req);
	zassert_equal(status(), 204, "%s", rsp);
	zassert_true(has("Connection: close\r\n"));
	zassert_true(has("Max-Age=0"));

	snprintf(req, sizeof(req),
		 "GET /api/v1/auth/session HTTP/1.1\r\nHost: 127.0.0.1\r\nCookie: cedar_session=%s\r\n\r\n",
		 cookie);
	exchange(req);
	zassert_equal(status(), 401);
	zassert_true(has("\"code\":\"session_expired\""));
}

ZTEST(web_api_http, test_every_route_reaches_its_operation_through_the_resources)
{
	for (size_t i = 0; i < web_api_v1_router.route_count; i++) {
		const struct web_api_route *route = &web_api_v1_router.routes[i];
		static const char *const methods[] = {"GET", "POST", "PUT", "DELETE"};
		char path[128] = "/api/v1";
		char req[256];
		const char *t = route->path;
		size_t n = strlen(path);

		while (*t != '\0') {
			if (*t == '{') {
				memcpy(&path[n], "x1", 2);
				n += 2;
				t = strchr(t, '}') + 1;
			} else {
				path[n++] = *t++;
			}
		}
		path[n] = '\0';
		snprintf(req, sizeof(req), "%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n",
			 methods[route->method], path);
		exchange(req);
		zassert_true(status() != 0, "%s: no answer", route->operation_id);
		zassert_false(status() == 404 && has("Not an API resource"), "%s %s not routed: %s",
			      route->operation_id, path, rsp);
		zassert_false(status() == 405, "%s: the server refused the method itself", route->operation_id);
	}
}

ZTEST(web_api_http, test_query_is_not_part_of_the_path)
{
	exchange("GET /api/v1/auth/state? HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
	zassert_equal(status(), 200, "an empty query is no query: %s", rsp);

	exchange("GET /api/v1/auth/state?x=1 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
	zassert_equal(status(), 400, "routed, then refused for the query: %s", rsp);
	zassert_true(has("\"code\":\"invalid_query\""));

	exchange("GET /network/wifi?tab=scan HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
	zassert_equal(status(), 200, "navigation with a query still gets the page");
	zassert_true(has("<div id=\"root\">"));
}

/* -- the log export, streamed through the real server --------------------- */

static char export_cookie[64];

static void sign_in_for_logs(void)
{
	char token[64], req[512];

	exchange("GET /api/v1/auth/state HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
	zassert_true(json_value("setup_token", token, sizeof(token)), "%s", rsp);
	snprintf(req, sizeof(req),
		 "POST /api/v1/auth/setup HTTP/1.1\r\nHost: 127.0.0.1:8080\r\n"
		 "Origin: http://127.0.0.1:8080\r\nX-Setup-Token: %s\r\n"
		 "Content-Type: application/json\r\nContent-Length: 32\r\n\r\n"
		 "{\"password\":\"a fine password\"}  ",
		 token);
	exchange(req);
	zassert_equal(status(), 201, "%s", rsp);
	zassert_true(header_value("Set-Cookie: cedar_session=", export_cookie,
				  sizeof(export_cookie)));
}

/* @p n records of @p len bytes of text in the STM32 ring. */
static void fill_logs(int n, size_t len)
{
	static char text[LOG_STORE_TEXT_MAX + 1];

	zassert_ok(log_store_init());
	memset(text, 'e', len);
	text[len] = '\0';
	for (int i = 0; i < n; i++) {
		const struct log_store_entry e = {
			.source = LOG_STORE_STM32,
			.level = LOG_STORE_LEVEL_INFO,
			.module = "burst",
			.module_len = 5,
			.text = text,
			.text_len = len,
		};

		zassert_ok(log_store_append(&e));
	}
}

static void send_export(int fd, const char *query)
{
	char req[256];

	snprintf(req, sizeof(req),
		 "GET /api/v1/logs/export%s HTTP/1.1\r\nHost: 127.0.0.1\r\nCookie: cedar_session=%s\r\n\r\n",
		 query, export_cookie);
	send_str(fd, req);
}

/* Read a whole streamed response, counting instead of keeping it. */
struct streamed {
	size_t bytes;
	int records;
	bool terminated;
	char head[512];
};

static void read_stream(int fd, struct streamed *out, int64_t timeout_ms)
{
	char buf[1024];
	char tail[16] = {0};
	/* A record is counted by the part a chunk boundary cannot split twice. */
	const char needle[] = "\"kind\":\"message\"";
	char carry[sizeof(needle)] = {0};
	int64_t start = k_uptime_get();

	memset(out, 0, sizeof(*out));
	while (k_uptime_get() - start < timeout_ms) {
		int n = zsock_recv(fd, buf, sizeof(buf) - 1, 0);

		if (n == 0) {
			break;
		}
		if (n < 0) {
			if (out->terminated) {
				break;
			}
			continue;
		}
		buf[n] = '\0';
		if (out->bytes < sizeof(out->head) - 1) {
			size_t k = MIN((size_t)n, sizeof(out->head) - 1 - out->bytes);

			memcpy(&out->head[out->bytes], buf, k);
		}
		/* Join the previous read's end to this one's start. */
		char joined[sizeof(carry) + sizeof(buf)];

		snprintf(joined, sizeof(joined), "%s%s", carry, buf);
		for (const char *p = strstr(joined, needle); p != NULL; p = strstr(p + 1, needle)) {
			if ((size_t)(p - joined) + sizeof(needle) - 1 > strlen(carry)) {
				out->records++;
			}
		}
		size_t jl = strlen(joined);
		size_t keep = MIN(jl, sizeof(carry) - 1);

		memcpy(carry, &joined[jl - keep], keep);
		carry[keep] = '\0';
		keep = MIN(jl, sizeof(tail) - 1);
		memcpy(tail, &joined[jl - keep], keep);
		tail[keep] = '\0';
		out->bytes += (size_t)n;
		out->terminated = strstr(tail, "0\r\n\r\n") != NULL;
	}
}

ZTEST(web_api_http, test_log_export_is_streamed_in_chunks)
{
	struct streamed s;
	struct timeval tv = {.tv_sec = 1};
	int fd;

	sign_in_for_logs();
	fill_logs(300, 300);

	fd = client();
	zsock_setsockopt(fd, ZSOCK_SOL_SOCKET, ZSOCK_SO_RCVTIMEO, &tv, sizeof(tv));
	send_export(fd, "?max_records=300");
	read_stream(fd, &s, 10000);
	zsock_close(fd);

	zassert_true(strstr(s.head, "HTTP/1.1 200") == s.head, "%s", s.head);
	zassert_not_null(strstr(s.head, "Transfer-Encoding: chunked"), "%s", s.head);
	zassert_not_null(strstr(s.head, "Content-Type: application/x-ndjson"), "%s", s.head);
	zassert_not_null(strstr(s.head, "Content-Disposition: attachment; filename=\"cedar-logs.ndjson\""));
	zassert_not_null(strstr(s.head, "X-Request-ID: "));
	zassert_equal(s.records, 300, "every record once: %d", s.records);
	zassert_true(s.bytes > 3 * CONFIG_WEB_API_RESPONSE_BODY_MAX, "%zu", s.bytes);
	zassert_true(s.terminated, "the terminating chunk came");
	wait_contexts_released();
	zassert_equal(web_api_http_contexts_in_use(), 0);
}

ZTEST(web_api_http, test_log_export_abandoned_by_its_client_releases_the_context)
{
	char buf[512];
	int fd;

	sign_in_for_logs();
	fill_logs(400, 400);

	fd = client();
	send_export(fd, "");
	zassert_true(zsock_recv(fd, buf, sizeof(buf), 0) > 0);
	zsock_close(fd);

	wait_contexts_released();
	zassert_equal(web_api_http_contexts_in_use(), 0, "the stream ended with its connection");
	exchange("GET /api/v1/auth/state HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
	zassert_equal(status(), 200, "%s", rsp);
}

/*
 * A browser tab frozen mid-export stops reading. The server writes from its
 * one thread with blocking sends, so without a send timeout every other client
 * would wait on that tab. Measures how long another client waits.
 */
ZTEST(web_api_http, test_stalled_export_client_does_not_hold_the_server)
{
	struct timeval tv = {.tv_sec = 5};
	int64_t start;
	int64_t waited;
	int stalled;
	int other;

	sign_in_for_logs();
	fill_logs(600, 400);

	stalled = client();
	send_export(stalled, "");
	/* Long enough for the server to fill the stalled client's window. */
	k_msleep(300);

	other = client();
	zsock_setsockopt(other, ZSOCK_SOL_SOCKET, ZSOCK_SO_RCVTIMEO, &tv, sizeof(tv));
	start = k_uptime_get();
	send_str(other, "GET /api/v1/auth/state HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
	rsp_len = zsock_recv(other, rsp, sizeof(rsp) - 1, 0);
	waited = k_uptime_get() - start;
	rsp[MAX(rsp_len, 0)] = '\0';
	zsock_close(other);

	TC_PRINT("another client waited %lld ms behind the stalled export\n", waited);
	zassert_equal(status(), 200, "served at all: %s", rsp);
	zassert_true(waited < CONFIG_WEB_API_HTTP_SEND_TIMEOUT_MS + 1500,
		     "within the send timeout: %lld ms", waited);

	zsock_close(stalled);
	wait_contexts_released();
	zassert_equal(web_api_http_contexts_in_use(), 0);
}

ZTEST(web_api_http, test_log_page_fits_the_response_buffer)
{
	char req[256];

	sign_in_for_logs();
	fill_logs(100, 500);
	snprintf(req, sizeof(req),
		 "GET /api/v1/logs/records?limit=100 HTTP/1.1\r\nHost: 127.0.0.1\r\nCookie: cedar_session=%s\r\n\r\n",
		 export_cookie);
	exchange(req);
	zassert_equal(status(), 200, "%s", rsp);
	zassert_true(has("\"has_more\":true"), "cut by bytes, not by limit");
	zassert_true(rsp_closed || rsp_len < (int)sizeof(rsp) - 1, "the page arrived whole");
}
