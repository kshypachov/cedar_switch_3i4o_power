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

/** What the network bindings need from the service that runs network-manager. */
struct web_api_v1_network {
	/**
	 * Wake the worker that runs network_manager_process(): a request was
	 * accepted and has interface work waiting. Called after the decision is
	 * recorded and before the response is written, so the worker must not
	 * pre-empt the HTTP thread before the response is sent (it cannot: the
	 * server's thread is cooperative).
	 */
	void (*kick)(void);
	/**
	 * Wi-Fi security modes this build can join, as a bitmask of
	 * BIT(enum device_config_wifi_security); NULL or 0 publishes all of them.
	 */
	uint32_t (*wifi_security_modes)(void);
};

/**
 * @brief Connect the network bindings to their service.
 *
 * Without it the handlers still answer — from network-manager's state — but
 * nothing wakes the worker; the sim tier drives network_manager_process()
 * itself. @p network must outlive the program.
 */
void web_api_v1_set_network(const struct web_api_v1_network *network);

/** What the coprocessor bindings need from the board's coprocessor service. */
struct web_api_v1_coprocessor {
	/**
	 * The firmware version ESP-Hosted reported, as text ("v1.4.1"), into
	 * @p buf; false when unknown.
	 */
	bool (*firmware_version)(char *buf, size_t cap);
	/** The C6 has sent at least one byte on its UART since boot. */
	bool (*rx_seen)(void);
};

/**
 * @brief Connect the coprocessor bindings to their service.
 *
 * Without it the status still answers from coprocessor-manager, with the
 * version unknown and a chip that has never spoken. @p coprocessor must
 * outlive the program.
 */
void web_api_v1_set_coprocessor(const struct web_api_v1_coprocessor *coprocessor);

#ifdef __cplusplus
}
#endif

#endif /* WEB_API_V1_H_ */
