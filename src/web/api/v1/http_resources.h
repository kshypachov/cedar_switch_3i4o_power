/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The HTTP server resources API v1 is registered under: one per distinct path
 * of routes.h, a path parameter written as `*`.
 *
 * Included by the firmware's service (src/web/web_server.c) and by the adapter
 * suite (tests/web_api_http), each with its own WEB_API_V1_RESOURCE, so the
 * suite exercises this exact list through the server's real matching.
 *
 * Order matters and is by name. Zephyr tries exact paths first, then wildcard
 * patterns in section order, which is name order (measured in P2), and the
 * first match wins; FNM_LEADING_DIR makes the jobs pattern also match
 * .../jobs/x/cancel. A more specific wildcard must therefore get a smaller
 * number than the general one it overlaps.
 *
 * WEB_API_V1_RESOURCE(name, "pattern")
 */

/* clang-format off */
WEB_API_V1_RESOURCE(web_api_00_auth_state, "/api/v1/auth/state")
WEB_API_V1_RESOURCE(web_api_01_auth_setup, "/api/v1/auth/setup")
WEB_API_V1_RESOURCE(web_api_02_auth_session, "/api/v1/auth/session")
WEB_API_V1_RESOURCE(web_api_03_auth_password, "/api/v1/auth/password")
WEB_API_V1_RESOURCE(web_api_04_system_status, "/api/v1/system/status")
WEB_API_V1_RESOURCE(web_api_05_capabilities, "/api/v1/capabilities")
WEB_API_V1_RESOURCE(web_api_06_jobs, "/api/v1/jobs/*")
/* clang-format on */
