/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The v1 route table, built from routes.h.
 */

#include <zephyr/sys/util.h>

#include "v1_internal.h"

/* Every decoded body must fit the scratch space web-api reserves for it. */
#define WEB_API_V1_ROUTE(op, method, path, flags, size, schema, query, handler)                  \
	BUILD_ASSERT((size) <= WEB_API_DECODED_MAX, #op " body does not fit "               \
							     "CONFIG_WEB_API_DECODED_BODY_MAX");
#include "routes.h"
#undef WEB_API_V1_ROUTE

/* Parameter names must not collide with the member names they initialise. */
#define WEB_API_V1_ROUTE(op, m, p, f, size, schema, q, h)                                          \
	{                                                                                          \
		.operation_id = #op,                                                               \
		.method = WEB_API_##m,                                                             \
		.path = p,                                                                         \
		.flags = f,                                                                        \
		.body_schema = schema,                                                             \
		.body_size = size,                                                                 \
		.query = q,                                                                        \
		.handler = h,                                                                      \
	},

static const struct web_api_route routes[] = {
#include "routes.h"
};

#undef WEB_API_V1_ROUTE

const struct web_api_router web_api_v1_router = {
	.routes = routes,
	.route_count = ARRAY_SIZE(routes),
};

static const struct web_api_v1_identity *identity;

const struct web_api_v1_identity *v1_identity(void)
{
	return identity;
}

int web_api_v1_init(const struct web_api_v1_identity *id)
{
	identity = id;

	return v1_auth_worker_start();
}
