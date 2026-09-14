/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared by the web-api suites: a fake web-auth platform, and a way to send a
 * request value through a router and read the response back.
 */

#ifndef WEB_API_TEST_HARNESS_H_
#define WEB_API_TEST_HARNESS_H_

#include <stdbool.h>

#include <web_api/web_api.h>
#include <web_auth/web_auth.h>

extern struct web_api_context ctx;
/* The response body, NUL-terminated. */
extern char body[CONFIG_WEB_API_RESPONSE_BODY_MAX + 1];

extern int64_t fake_now;
/* While set, the fake key derivation on any thread but the test's blocks on
 * kdf_gate: a password change job can be held running. */
extern bool kdf_gate_enabled;
extern struct k_sem kdf_gate;
extern int kdf_calls;

extern const struct web_auth_platform fake_platform;

void harness_reset(void);

/* Start a request: Host is a LAN address, everything else empty. */
void request(enum web_api_method method, const char *path);
void request_body(const char *json);
void dispatch(const struct web_api_router *router);

const char *response_header(const char *name);
/* The value of "name": in the body, as raw JSON text up to , or } */
bool body_has(const char *fragment);

/* Empty log-store, and coprocessor-manager on a fake platform with the console
 * owning the UART and no transport (src/v1_logs.c). */
void v1_coprocessor_fake_init(void);

#endif
