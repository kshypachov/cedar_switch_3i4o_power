/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * web-assets. The policy is documented in include/web_assets/web_assets.h.
 */

#include <string.h>

#include <zephyr/sys/util.h>

#include <web_assets/web_assets.h>

const char *const web_assets_page_csp =
	"default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; "
	"font-src 'self'; connect-src 'self'; object-src 'none'; base-uri 'none'; "
	"form-action 'self'; frame-ancestors 'none'";

static const char cache_revalidate[] = "no-cache";
static const char cache_immutable[] = "public, max-age=31536000, immutable";

const struct web_asset *web_assets_find(const struct web_assets_table *table, const char *path)
{
	size_t lo = 0;
	size_t hi = table->count;

	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2U;
		int c = strcmp(path, table->assets[mid].path);

		if (c == 0) {
			return &table->assets[mid];
		}
		if (c < 0) {
			hi = mid;
		} else {
			lo = mid + 1U;
		}
	}

	return NULL;
}

static char lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool token_is(const char *s, size_t n, const char *word)
{
	size_t wn = strlen(word);

	if (n != wn) {
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		if (lower(s[i]) != word[i]) {
			return false;
		}
	}

	return true;
}

static const char *trim(const char *s, size_t *n)
{
	while (*n > 0U && (*s == ' ' || *s == '\t')) {
		s++;
		(*n)--;
	}
	while (*n > 0U && (s[*n - 1U] == ' ' || s[*n - 1U] == '\t')) {
		(*n)--;
	}

	return s;
}

/* q=0, q=0., q=0.0... are zero; any other value is not. Absent q is 1. */
static bool q_is_zero(const char *params, size_t n)
{
	while (n > 0U) {
		const char *semi = memchr(params, ';', n);
		size_t len = semi ? (size_t)(semi - params) : n;
		size_t tn = len;
		const char *t = trim(params, &tn);

		if (tn >= 2U && lower(t[0]) == 'q' && t[1] == '=') {
			const char *v = t + 2;
			size_t vn = tn - 2U;

			if (vn == 0U || v[0] != '0') {
				return false;
			}
			for (size_t i = 1; i < vn; i++) {
				if (v[i] != '.' && v[i] != '0') {
					return false;
				}
			}
			return true;
		}
		if (semi == NULL) {
			break;
		}
		n -= len + 1U;
		params = semi + 1;
	}

	return false;
}

bool web_assets_accepts_gzip(const char *accept_encoding)
{
	const char *p = accept_encoding;
	int gzip = -1; /* -1 not mentioned, 0 refused, 1 accepted */
	int star = -1;

	if (p == NULL) {
		return false;
	}
	while (*p != '\0') {
		const char *comma = strchr(p, ',');
		size_t len = comma ? (size_t)(comma - p) : strlen(p);
		const char *semi = memchr(p, ';', len);
		size_t tn = semi ? (size_t)(semi - p) : len;
		const char *token = trim(p, &tn);
		bool zero = semi != NULL && q_is_zero(semi + 1, len - (size_t)(semi - p) - 1U);

		if (token_is(token, tn, "gzip") || token_is(token, tn, "x-gzip")) {
			gzip = zero ? 0 : 1;
		} else if (token_is(token, tn, "*")) {
			star = zero ? 0 : 1;
		}
		if (comma == NULL) {
			break;
		}
		p = comma + 1;
	}

	if (gzip >= 0) {
		return gzip == 1;
	}

	return star == 1;
}

static const char *strip_weak(const char *s, size_t *n)
{
	if (*n >= 2U && s[0] == 'W' && s[1] == '/') {
		*n -= 2U;
		return s + 2;
	}

	return s;
}

bool web_assets_etag_matches(const char *if_none_match, const char *etag)
{
	const char *p = if_none_match;
	size_t en = strlen(etag);
	const char *e = strip_weak(etag, &en);

	if (p == NULL) {
		return false;
	}
	while (*p != '\0') {
		const char *comma = strchr(p, ',');
		size_t n = comma ? (size_t)(comma - p) : strlen(p);
		const char *t = trim(p, &n);

		if (n == 1U && t[0] == '*') {
			return true;
		}
		t = strip_weak(t, &n);
		if (n == en && memcmp(t, e, n) == 0) {
			return true;
		}
		if (comma == NULL) {
			break;
		}
		p = comma + 1;
	}

	return false;
}

static void header(struct web_assets_response *rsp, const char *name, const char *value)
{
	if (rsp->header_count < ARRAY_SIZE(rsp->headers)) {
		rsp->headers[rsp->header_count].name = name;
		rsp->headers[rsp->header_count].value = value;
		rsp->header_count++;
	}
}

static void plain(struct web_assets_response *rsp, uint16_t status, const char *text)
{
	rsp->status = status;
	rsp->body = (const uint8_t *)text;
	rsp->body_len = strlen(text);
	header(rsp, "Content-Type", "text/plain; charset=utf-8");
	header(rsp, "Cache-Control", "no-store");
	header(rsp, "X-Content-Type-Options", "nosniff");
}

/* A route of the browser application, as opposed to a file. */
static bool is_navigation(const char *path)
{
	const char *last = strrchr(path, '/');

	if (strncmp(path, "/assets/", 8) == 0) {
		return false;
	}

	return strchr(last != NULL ? last : path, '.') == NULL;
}

void web_assets_respond(const struct web_assets_table *table,
			const struct web_assets_request *req, struct web_assets_response *rsp)
{
	const struct web_asset *asset;

	memset(rsp, 0, sizeof(*rsp));

	if (!req->is_get) {
		plain(rsp, 405, "Method not allowed\n");
		return;
	}

	asset = (strcmp(req->path, "/") == 0) ? table->index : web_assets_find(table, req->path);
	if (asset == NULL && is_navigation(req->path)) {
		asset = table->index;
	}
	if (asset == NULL) {
		plain(rsp, 404, "Not found\n");
		return;
	}

	if (asset->gzip && !web_assets_accepts_gzip(req->accept_encoding)) {
		plain(rsp, 406, "This file is only available gzip-encoded\n");
		header(rsp, "Vary", "Accept-Encoding");
		return;
	}

	header(rsp, "ETag", asset->etag);
	header(rsp, "Cache-Control",
	       asset->cache == WEB_ASSET_CACHE_IMMUTABLE ? cache_immutable : cache_revalidate);
	if (asset->gzip) {
		header(rsp, "Vary", "Accept-Encoding");
	}

	if (web_assets_etag_matches(req->if_none_match, asset->etag)) {
		rsp->status = 304;
		rsp->close_connection = true;
		return;
	}

	rsp->status = 200;
	rsp->body = asset->data;
	rsp->body_len = asset->len;
	header(rsp, "Content-Type", asset->content_type);
	header(rsp, "X-Content-Type-Options", "nosniff");
	if (asset->gzip) {
		header(rsp, "Content-Encoding", "gzip");
	}
	if (asset->is_page) {
		header(rsp, "Content-Security-Policy", web_assets_page_csp);
		header(rsp, "Referrer-Policy", "no-referrer");
		header(rsp, "X-Frame-Options", "DENY");
	}
}
