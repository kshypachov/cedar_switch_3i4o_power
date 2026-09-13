/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * web-api's adapter to Zephyr's HTTP server.
 *
 * The core decides; this file does the I/O the core must not see. Every
 * behaviour here was measured against the server in this tree before it was
 * designed (P2, docs/device-development/reports/p2):
 *
 * - **Headers are captured by the server** (CONFIG_HTTP_SERVER_CAPTURE_HEADERS)
 *   and are available only in the first callback of a request. They are
 *   copied into the client's context there. A header longer than
 *   CONFIG_HTTP_SERVER_MAX_HEADER_LEN, or one that does not fit the capture
 *   buffer, is dropped by the server with nothing but a status flag; the flag
 *   is carried into the request and the core refuses it (web_api.h, step 2).
 *
 * - **One context per client, released by the server's own notifications.**
 *   A context is taken on the first callback of a request and given back on
 *   HTTP_SERVER_TRANSACTION_COMPLETE or HTTP_SERVER_TRANSACTION_ABORTED. The
 *   server delivers ABORTED when a client closes or times out mid-request
 *   (measured: under 300 ms after a close on loopback; at the inactivity
 *   timeout for a client that simply stops sending), so an abandoned upload
 *   leaves nothing behind. That is the plan's "correct cleanup on abort".
 *
 * - **The body is kept only up to the route's limit** and counted past it,
 *   so a large body is never buffered and the core answers 413.
 *
 * - **Zephyr serialises a dynamic resource across clients.** While one client
 *   is in the middle of a body on a resource, any other client asking for the
 *   same resource gets a bare `409 Conflict` with no body and a closed
 *   connection, produced by the server before any callback (measured,
 *   http_server_http1.c). The application registers one resource per path
 *   template to keep that to requests on the same path; the frontend treats a
 *   409 without a JSON body as "busy, retry". A client that stalls mid-body
 *   holds its resource until the inactivity timeout.
 *
 * - **GET and DELETE callbacks carry the query string in the data**, not a
 *   body; the adapter reads the path and query from the client's URL buffer
 *   for every method instead.
 */

#ifndef WEB_API_HTTP_H_
#define WEB_API_HTTP_H_

#include <zephyr/net/http/server.h>

#include <web_api/web_api.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Answers a request whose path is not under /api/: fills @p ctx->rsp (status,
 * body, headers) for the static application. @p ctx->req is complete, and its
 * headers include Accept-Encoding and If-None-Match.
 */
typedef void (*web_api_http_other_t)(struct web_api_context *ctx);

/**
 * @brief The whole callback of a dynamic resource.
 *
 * Call this from each resource's http_resource_dynamic_cb_t. Paths under
 * /api/ go to @p router (and are a JSON 404 when nothing matches); anything
 * else goes to @p other, or is a plain 404 when @p other is NULL.
 *
 * @return what the server expects from the callback: 0, or a negative errno
 *         to close the connection (only when no context could be found for a
 *         new request, which the pool size makes impossible in practice).
 */
int web_api_http_callback(const struct web_api_router *router, web_api_http_other_t other,
			  struct http_client_ctx *client, enum http_transaction_status status,
			  const struct http_request_ctx *request_ctx,
			  struct http_response_ctx *response_ctx);

/** @brief Contexts currently held by a request in flight; for tests. */
size_t web_api_http_contexts_in_use(void);

/** Methods an API resource declares, so that none is refused by the server
 *  with a bare 405 before the core can answer it with a JSON 404. HEAD is left
 *  out: the server answers HEAD on a dynamic resource by itself. */
#define WEB_API_HTTP_METHODS                                                                       \
	(BIT(HTTP_GET) | BIT(HTTP_POST) | BIT(HTTP_PUT) | BIT(HTTP_DELETE) | BIT(HTTP_PATCH))

#ifdef __cplusplus
}
#endif

#endif /* WEB_API_HTTP_H_ */
