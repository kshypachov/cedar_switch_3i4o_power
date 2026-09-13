/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * API v1 bindings: the operations of openapi.json that the device serves,
 * written against web-api's call helpers and the services behind them.
 *
 * Plan section 3 puts HTTP bindings in src/web/api/v1/ and keeps decisions out
 * of them: a handler reads its decoded body, calls a service, and writes the
 * schema's JSON. Everything with a rule in it - the session, CSRF, limits,
 * idempotency scope - happened in web-api before the handler runs.
 */

#ifndef WEB_API_V1_H_
#define WEB_API_V1_H_

#include <web_api/web_api.h>

#ifdef __cplusplus
extern "C" {
#endif

/** What SystemStatus reports about the device and its build. */
struct web_api_v1_identity {
	/** Matches ^[A-Za-z0-9_-]{1,64}$. */
	const char *device_id;
	const char *model;
	const char *firmware_version;
	const char *frontend_version;
	/** Random per boot, ^[A-Za-z0-9_-]{1,64}$; jobs carry it too. */
	const char *boot_id;
};

/** The route table built from routes.h. */
extern const struct web_api_router web_api_v1_router;

/**
 * @brief Start the bindings: remember the identity and start the worker that
 *        runs password changes.
 *
 * web-auth and job-manager must be initialised first. The strings must
 * outlive the program.
 */
int web_api_v1_init(const struct web_api_v1_identity *identity);

#ifdef __cplusplus
}
#endif

#endif /* WEB_API_V1_H_ */
