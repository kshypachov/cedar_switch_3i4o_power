/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * web-api adapter to Zephyr's HTTP server. Design in
 * include/web_api/web_api_http.h.
 */

#include <errno.h>
#include <string.h>
#include <strings.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>

#include <web_api/web_api_http.h>

LOG_MODULE_DECLARE(web_api, CONFIG_WEB_API_LOG_LEVEL);

BUILD_ASSERT(sizeof(struct web_api_header) == sizeof(struct http_header) &&
		     offsetof(struct web_api_header, name) == offsetof(struct http_header, name) &&
		     offsetof(struct web_api_header, value) == offsetof(struct http_header, value),
	     "response headers are handed to the server without copying");

/* The request headers the API and the static application read. */
HTTP_SERVER_REGISTER_HEADER_CAPTURE(web_api_hdr_host, "Host");
HTTP_SERVER_REGISTER_HEADER_CAPTURE(web_api_hdr_origin, "Origin");
HTTP_SERVER_REGISTER_HEADER_CAPTURE(web_api_hdr_cookie, "Cookie");
HTTP_SERVER_REGISTER_HEADER_CAPTURE(web_api_hdr_csrf, "X-CSRF-Token");
HTTP_SERVER_REGISTER_HEADER_CAPTURE(web_api_hdr_idem, "Idempotency-Key");
HTTP_SERVER_REGISTER_HEADER_CAPTURE(web_api_hdr_setup, "X-Setup-Token");
HTTP_SERVER_REGISTER_HEADER_CAPTURE(web_api_hdr_ae, "Accept-Encoding");
HTTP_SERVER_REGISTER_HEADER_CAPTURE(web_api_hdr_inm, "If-None-Match");

/* Room for every captured value, NUL-terminated, plus the path and query. */
#define HEADER_STORE_SIZE (CONFIG_HTTP_SERVER_CAPTURE_HEADER_BUFFER_SIZE + 16)

struct slot {
	struct http_client_ctx *client;
	bool active;
	bool responded;
	/** A streamed body has pieces left; the server's next call asks for one. */
	bool streaming;
	size_t limit;
	char url[CONFIG_HTTP_SERVER_MAX_URL_LENGTH + 1];
	char content_type[CONFIG_HTTP_SERVER_MAX_CONTENT_TYPE_LENGTH + 1];
	char headers[HEADER_STORE_SIZE];
	struct web_api_context ctx;
	struct http_header rsp_headers[WEB_API_MAX_RESPONSE_HEADERS + 1];
};

static struct slot slots[CONFIG_HTTP_SERVER_MAX_CLIENTS];
static K_MUTEX_DEFINE(slots_lock);

size_t web_api_http_contexts_in_use(void)
{
	size_t n = 0;

	k_mutex_lock(&slots_lock, K_FOREVER);
	for (size_t i = 0; i < ARRAY_SIZE(slots); i++) {
		n += slots[i].active ? 1U : 0U;
	}
	k_mutex_unlock(&slots_lock);

	return n;
}

static struct slot *slot_find(struct http_client_ctx *client)
{
	for (size_t i = 0; i < ARRAY_SIZE(slots); i++) {
		if (slots[i].active && slots[i].client == client) {
			return &slots[i];
		}
	}
	return NULL;
}

static struct slot *slot_take(struct http_client_ctx *client)
{
	struct slot *s;

	k_mutex_lock(&slots_lock, K_FOREVER);
	s = slot_find(client);
	if (s == NULL) {
		for (size_t i = 0; i < ARRAY_SIZE(slots); i++) {
			if (!slots[i].active) {
				s = &slots[i];
				break;
			}
		}
	}
	if (s != NULL) {
		/* A context still marked active for this client is a request the
		 * server never finished telling us about; start over. */
		s->active = true;
		s->client = client;
		s->responded = false;
		s->streaming = false;
		web_api_stream_end(&s->ctx);
	}
	k_mutex_unlock(&slots_lock);

	return s;
}

static void slot_release(struct http_client_ctx *client)
{
	k_mutex_lock(&slots_lock, K_FOREVER);
	struct slot *s = slot_find(client);

	if (s != NULL) {
		/* A client that went away mid-export: the stream's end runs here,
		 * so nothing it pinned outlives the connection. */
		web_api_stream_end(&s->ctx);
		s->streaming = false;
		s->active = false;
		s->client = NULL;
		/* The body may have carried a password. */
		memset(s->ctx.body, 0, sizeof(s->ctx.body));
		memset(s->ctx.decoded, 0, sizeof(s->ctx.decoded));
	}
	k_mutex_unlock(&slots_lock);
}

static enum web_api_method method_of(enum http_method m)
{
	switch (m) {
	case HTTP_GET:
		return WEB_API_GET;
	case HTTP_POST:
		return WEB_API_POST;
	case HTTP_PUT:
		return WEB_API_PUT;
	case HTTP_DELETE:
		return WEB_API_DELETE;
	default:
		return WEB_API_METHOD_OTHER;
	}
}

static void peer_of(struct http_client_ctx *client, struct web_auth_peer *peer)
{
	struct net_sockaddr_storage addr;
	net_socklen_t len = sizeof(addr);

	memset(peer, 0, sizeof(*peer));
	if (zsock_getpeername(client->fd, (struct net_sockaddr *)&addr, &len) != 0) {
		return;
	}
	if (addr.ss_family == NET_AF_INET) {
		struct net_sockaddr_in *in = (struct net_sockaddr_in *)&addr;

		peer->family = 4;
		memcpy(peer->addr, &in->sin_addr, 4);
	} else if (addr.ss_family == NET_AF_INET6) {
		struct net_sockaddr_in6 *in6 = (struct net_sockaddr_in6 *)&addr;
		static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};

		if (memcmp(&in6->sin6_addr, mapped, sizeof(mapped)) == 0) {
			/* An IPv4 client on the dual-stack socket is still that
			 * IPv4 address for the guessing limit. */
			peer->family = 4;
			memcpy(peer->addr, (uint8_t *)&in6->sin6_addr + 12, 4);
		} else {
			peer->family = 6;
			memcpy(peer->addr, &in6->sin6_addr, 16);
		}
	}
}

static const char *store(struct slot *s, size_t *used, const char *value)
{
	size_t n = strlen(value) + 1U;
	const char *dst;

	if (*used + n > sizeof(s->headers)) {
		s->ctx.req.headers.dropped = true;
		return NULL;
	}
	memcpy(&s->headers[*used], value, n);
	dst = &s->headers[*used];
	*used += n;

	return dst;
}

static void begin_request(struct slot *s, const struct web_api_router *router,
			  struct http_client_ctx *client, const struct http_request_ctx *rq)
{
	struct web_api_request *req = &s->ctx.req;
	struct web_api_headers *h = &req->headers;
	size_t used = 0;
	char *q;

	memset(req, 0, sizeof(*req));
	strncpy(s->url, (const char *)client->url_buffer, sizeof(s->url) - 1U);
	s->url[sizeof(s->url) - 1U] = '\0';
	q = strchr(s->url, '?');
	if (q != NULL) {
		*q = '\0';
		req->query = q + 1;
	} else {
		req->query = "";
	}
	req->path = s->url;
	req->method = method_of(client->method);

	strncpy(s->content_type, (const char *)client->content_type, sizeof(s->content_type) - 1U);
	s->content_type[sizeof(s->content_type) - 1U] = '\0';
	h->content_type = s->content_type[0] != '\0' ? s->content_type : NULL;

	h->dropped = rq->headers_status == HTTP_HEADER_STATUS_DROPPED;
	for (size_t i = 0; i < rq->header_count; i++) {
		const char *name = rq->headers[i].name;
		const char *value = rq->headers[i].value;
		const char **dst = NULL;

		if (strcasecmp(name, "Host") == 0) {
			dst = &h->host;
		} else if (strcasecmp(name, "Origin") == 0) {
			dst = &h->origin;
		} else if (strcasecmp(name, "Cookie") == 0) {
			dst = &h->cookie;
		} else if (strcasecmp(name, "X-CSRF-Token") == 0) {
			dst = &h->csrf_token;
		} else if (strcasecmp(name, "Idempotency-Key") == 0) {
			dst = &h->idempotency_key;
		} else if (strcasecmp(name, "X-Setup-Token") == 0) {
			dst = &h->setup_token;
		} else if (strcasecmp(name, "Accept-Encoding") == 0) {
			dst = &h->accept_encoding;
		} else if (strcasecmp(name, "If-None-Match") == 0) {
			dst = &h->if_none_match;
		}
		if (dst != NULL) {
			if (*dst != NULL) {
				/* The same header twice: which one counts cannot be
				 * known, so the request is refused as unreadable. */
				h->dropped = true;
			} else {
				*dst = store(s, &used, value);
			}
		}
	}

	peer_of(client, &req->peer);
	s->limit = (strncmp(req->path, "/api/", 5) == 0)
			   ? web_api_body_limit(router, req->method, req->path)
			   : 0U;
}

static void accumulate(struct slot *s, const struct http_request_ctx *rq)
{
	struct web_api_request *req = &s->ctx.req;

	if (rq->data_len == 0U) {
		return;
	}
	if (req->body_received + rq->data_len <= s->limit) {
		memcpy(&s->ctx.body[req->body_received], rq->data, rq->data_len);
	}
	req->body_received += rq->data_len;
}

/*
 * The next piece of a streamed body. The server sends status and headers with
 * the first piece only and ignores them afterwards, and it reads a piece with
 * no status, no headers and no body as the end of the response - which is why
 * web_api_stream_next_t never returns an empty piece that is not the last.
 */
static int pull(struct slot *s, struct http_response_ctx *rsp)
{
	bool final;
	int rc = web_api_stream_pull(&s->ctx, &final);

	s->streaming = rc == 0 && !final;
	if (rc < 0) {
		/* Closing the connection mid-body is how the client learns the
		 * body is incomplete: the terminating chunk never comes. */
		return rc;
	}
	rsp->body = (const uint8_t *)s->ctx.rsp.body;
	rsp->body_len = s->ctx.rsp.body_len;
	rsp->final_chunk = final;

	return 0;
}

static int answer(struct slot *s, struct http_response_ctx *rsp)
{
	const struct web_api_response *r = &s->ctx.rsp;
	size_t n = MIN(r->header_count, (size_t)WEB_API_MAX_RESPONSE_HEADERS);

	memcpy(s->rsp_headers, r->headers, n * sizeof(struct http_header));
	if (r->close_connection) {
		s->rsp_headers[n].name = "Connection";
		s->rsp_headers[n].value = "close";
		n++;
	}
	rsp->status = r->status;
	rsp->headers = s->rsp_headers;
	rsp->header_count = n;
	s->responded = true;
	if (web_api_stream_active(&s->ctx)) {
		return pull(s, rsp);
	}
	rsp->body = (const uint8_t *)r->body;
	rsp->body_len = r->body_len;
	rsp->final_chunk = true;

	return 0;
}

/*
 * The server writes with blocking send() from its only thread; see
 * CONFIG_WEB_API_HTTP_SEND_TIMEOUT_MS.
 */
static void limit_send_time(struct http_client_ctx *client)
{
#if defined(CONFIG_NET_CONTEXT_SNDTIMEO)
	if (CONFIG_WEB_API_HTTP_SEND_TIMEOUT_MS > 0) {
		const struct zsock_timeval tv = {
			.tv_sec = CONFIG_WEB_API_HTTP_SEND_TIMEOUT_MS / 1000,
			.tv_usec = (CONFIG_WEB_API_HTTP_SEND_TIMEOUT_MS % 1000) * 1000,
		};

		(void)zsock_setsockopt(client->fd, ZSOCK_SOL_SOCKET, ZSOCK_SO_SNDTIMEO, &tv,
				       sizeof(tv));
	}
#else
	ARG_UNUSED(client);
#endif
}

int web_api_http_callback(const struct web_api_router *router, web_api_http_other_t other,
			  struct http_client_ctx *client, enum http_transaction_status status,
			  const struct http_request_ctx *request_ctx,
			  struct http_response_ctx *response_ctx)
{
	struct slot *s;

	if (status == HTTP_SERVER_TRANSACTION_ABORTED ||
	    status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		slot_release(client);
		return 0;
	}

	k_mutex_lock(&slots_lock, K_FOREVER);
	s = slot_find(client);
	k_mutex_unlock(&slots_lock);

	if (s != NULL && s->streaming) {
		/* The server calls again for the next piece of the same response. */
		return pull(s, response_ctx);
	}

	if (s == NULL || s->responded) {
		s = slot_take(client);
		if (s == NULL) {
			LOG_ERR("no request context free for a new request");
			return -ENOMEM;
		}
		begin_request(s, router, client, request_ctx);
		limit_send_time(client);
	}

	if (client->method == HTTP_POST || client->method == HTTP_PUT ||
	    client->method == HTTP_PATCH) {
		accumulate(s, request_ctx);
	}

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	struct web_api_request *req = &s->ctx.req;

	if (req->body_received > 0U && req->body_received <= s->limit) {
		req->body = s->ctx.body;
		req->body_len = req->body_received;
	}

	if (strncmp(req->path, "/api/", 5) == 0 || strcmp(req->path, "/api") == 0) {
		web_api_dispatch(router, &s->ctx);
	} else if (other != NULL) {
		memset(&s->ctx.rsp, 0, sizeof(s->ctx.rsp));
		other(&s->ctx);
	} else {
		memset(&s->ctx.rsp, 0, sizeof(s->ctx.rsp));
		s->ctx.rsp.status = 404;
	}

	return answer(s, response_ctx);
}
