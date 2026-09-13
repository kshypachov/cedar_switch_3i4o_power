/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * web-assets: the browser application, served from flash.
 *
 * Contract: "Общие правила" in docs/device-development/api-contract.md
 * ("Static hashed assets могут кешироваться; index revalidates. Неизвестный
 * API URL → JSON 404, никогда SPA redirect"), section 4 of the development plan
 * (works offline, no CDN), and section 12's web-assets row: MIME, ETag, gzip,
 * index cache policy, SPA fallback against JSON 404.
 *
 * The table this module serves is generated at build time from the frontend's
 * dist directory by tools/web-assets/gen_web_assets.py. The generator decides
 * everything that can be decided once - MIME type from a closed extension
 * list, gzip or identity, strong ETag, cache class - and emits the same facts
 * as a JSON manifest, which the mock server reads to serve the same bytes with
 * the same headers during frontend e2e tests. This module decides only what
 * depends on the request.
 *
 * The decisions:
 *
 * - **Not Zephyr's static resources.** HTTP_RESOURCE_TYPE_STATIC sends
 *   Content-Type, Content-Length and optionally Content-Encoding, and nothing
 *   else (subsys/net/lib/http/http_server_http1.c): no ETag, no Cache-Control,
 *   no conditional request, no security headers. Assets are therefore answered
 *   from the server's fallback resource through this module.
 *
 * - **One stored representation per file.** A text asset that gzip shrinks is
 *   stored gzipped only; everything else identity only. A client that does not
 *   accept gzip for a gzipped asset gets 406 rather than a second copy in
 *   flash. Every browser accepts gzip; the case exists for tools, and index.html
 *   - the page a tool is likely to fetch - is small enough that the generator
 *   keeps it identity (it gains too little).
 *
 * - **Cache classes.** A file under /assets/ whose name carries Vite's content
 *   hash is immutable for a year: a new build produces a new name. Everything
 *   else - index.html above all - is `no-cache`, which means "revalidate", and
 *   the ETag makes that revalidation a 304.
 *
 * - **304 closes the connection**, for the same reason 204 does in web-api:
 *   Zephyr frames the empty response as chunked and sends the terminating
 *   chunk, which a client must not read as a body.
 *
 * - **SPA fallback is for navigation only.** A GET for a path with no file
 *   extension outside /assets/ is a route of the browser application and is
 *   answered with index.html. A missing /assets/... file, or any path with an
 *   extension, is a plain 404: answering a missing script with an HTML page
 *   produces a MIME error in the browser instead of a clear failure. API paths
 *   never reach this module; the fallback dispatcher gives them to web-api.
 *
 * - **Security headers on the page.** index.html carries a Content-Security-
 *   Policy that allows only this origin - the plan's "no CDN, no external
 *   requests" enforced by the browser rather than trusted - plus frame
 *   denial and no referrer. Every asset carries X-Content-Type-Options: nosniff.
 */

#ifndef WEB_ASSETS_H_
#define WEB_ASSETS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum web_asset_cache {
	/** Cache-Control: no-cache (revalidate with the ETag). */
	WEB_ASSET_CACHE_REVALIDATE = 0,
	/** Cache-Control: public, max-age=31536000, immutable. */
	WEB_ASSET_CACHE_IMMUTABLE,
};

struct web_asset {
	/** URL path, e.g. "/index.html", "/assets/index-3f2a9c1b.js". */
	const char *path;
	const char *content_type;
	/** Strong validator including the quotes, e.g. "\"3f2a9c1b04e7d6a5\"". */
	const char *etag;
	const uint8_t *data;
	size_t len;
	/** @ref data is gzip-encoded. */
	bool gzip;
	enum web_asset_cache cache;
	/** This is the application's page: gets the CSP and friends. */
	bool is_page;
};

struct web_assets_table {
	/** Sorted by path, for binary search. */
	const struct web_asset *assets;
	size_t count;
	/** The page served for "/" and for navigation fallback. */
	const struct web_asset *index;
	/** The frontend version the build stamped, for SystemStatus. */
	const char *version;
};

/** What the generated source defines. */
extern const struct web_assets_table web_assets;

/** The request facts that matter to an asset. */
struct web_assets_request {
	/** "GET", or anything else. */
	bool is_get;
	/** Path without query. */
	const char *path;
	/** Header values, NULL when absent. */
	const char *accept_encoding;
	const char *if_none_match;
};

/* Content-Type, ETag, Cache-Control, nosniff, Content-Encoding, Vary, and on
 * the page CSP, Referrer-Policy, X-Frame-Options. */
#define WEB_ASSETS_MAX_HEADERS 9

struct web_assets_response {
	uint16_t status;
	const uint8_t *body;
	size_t body_len;
	struct {
		const char *name;
		const char *value;
	} headers[WEB_ASSETS_MAX_HEADERS];
	size_t header_count;
	bool close_connection;
};

/** The policy the page is served with. Exposed so tests and the mock agree. */
extern const char *const web_assets_page_csp;

/**
 * @brief Answer one non-API request from @p table.
 *
 * Statuses: 200 (asset, or index.html for navigation), 304, 404, 405 (not GET),
 * 406 (gzip-only asset, gzip not acceptable). Bodies of the errors are short
 * plain text.
 */
void web_assets_respond(const struct web_assets_table *table,
			const struct web_assets_request *req, struct web_assets_response *rsp);

/** @brief Look a path up; NULL when no file has it. */
const struct web_asset *web_assets_find(const struct web_assets_table *table, const char *path);

/**
 * @brief Does an Accept-Encoding value allow gzip?
 *
 * RFC 9110 section 12.5.3: `gzip` or `x-gzip` or `*` with a non-zero q, and no
 * explicit `gzip;q=0`. An absent header means any encoding is acceptable, but
 * the module does not rely on that for gzip: absent is treated as not allowed,
 * because a client that sends no Accept-Encoding is almost always a tool that
 * will not decode what it gets.
 */
bool web_assets_accepts_gzip(const char *accept_encoding);

/**
 * @brief Does an If-None-Match value match @p etag?
 *
 * Weak comparison, as RFC 9110 section 13.1.2 requires for If-None-Match:
 * `W/` prefixes are ignored, `*` matches anything.
 */
bool web_assets_etag_matches(const char *if_none_match, const char *etag);

#ifdef __cplusplus
}
#endif

#endif /* WEB_ASSETS_H_ */
