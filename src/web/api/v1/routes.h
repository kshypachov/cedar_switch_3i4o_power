/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The route table of API v1: one line per implemented operation.
 *
 * Included more than once with different definitions of WEB_API_V1_ROUTE
 * (router.c builds the table and checks body sizes from it), and read as text
 * by tools/api-contract, which checks every line against openapi.json: the
 * operationId must be declared, with the same method and path, and the flags
 * must match the document's security requirement and header parameters. Keep
 * one entry per line, and the arguments literal, so that check can parse it.
 *
 * WEB_API_V1_ROUTE(operationId, METHOD, "path", flags, body size, body schema,
 *                  query names, handler)
 *
 * Operations of the document that are not listed here are not served yet; the
 * contract check prints them as uncovered rather than failing, because each
 * belongs to a later stage (plan section 10).
 */

/* clang-format off */
WEB_API_V1_ROUTE(getAuthState, GET, "/auth/state", WEB_API_PUBLIC, V1_NO_BODY, NULL, NULL, v1_get_auth_state)
WEB_API_V1_ROUTE(setupAdmin, POST, "/auth/setup", WEB_API_PUBLIC | WEB_API_ORIGIN | WEB_API_SETUP_TOKEN | WEB_API_BODY_REQUIRED, V1_BODY(struct v1_setup_body), &v1_setup_schema, NULL, v1_setup_admin)
WEB_API_V1_ROUTE(login, POST, "/auth/session", WEB_API_PUBLIC | WEB_API_ORIGIN | WEB_API_BODY_REQUIRED, V1_BODY(struct v1_login_body), &v1_login_schema, NULL, v1_login)
WEB_API_V1_ROUTE(getSession, GET, "/auth/session", 0, V1_NO_BODY, NULL, NULL, v1_get_session)
WEB_API_V1_ROUTE(logout, DELETE, "/auth/session", WEB_API_CSRF, V1_NO_BODY, NULL, NULL, v1_logout)
WEB_API_V1_ROUTE(changePassword, PUT, "/auth/password", WEB_API_CSRF | WEB_API_IDEMPOTENT | WEB_API_BODY_REQUIRED, V1_BODY(struct v1_password_body), &v1_password_schema, NULL, v1_change_password)
WEB_API_V1_ROUTE(getSystemStatus, GET, "/system/status", 0, V1_NO_BODY, NULL, NULL, v1_get_system_status)
WEB_API_V1_ROUTE(getCapabilities, GET, "/capabilities", 0, V1_NO_BODY, NULL, NULL, v1_get_capabilities)
WEB_API_V1_ROUTE(getJob, GET, "/jobs/{job_id}", 0, V1_NO_BODY, NULL, NULL, v1_get_job)
/* clang-format on */
