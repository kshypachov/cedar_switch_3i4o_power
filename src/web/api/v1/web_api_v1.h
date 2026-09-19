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
	/**
	 * The request whose device-side address is @p local arrived on the
	 * Ethernet interface (the board: net_adapter_is_ethernet_address()). NULL,
	 * or no hooks at all, means the check cannot be made, and an install is
	 * refused 409 ethernet_required.
	 */
	bool (*request_over_ethernet)(const struct web_auth_peer *local);
};

/**
 * @brief Connect the coprocessor bindings to their service.
 *
 * Without it the status still answers from coprocessor-manager, with the
 * version unknown and a chip that has never spoken. @p coprocessor must
 * outlive the program.
 */
void web_api_v1_set_coprocessor(const struct web_api_v1_coprocessor *coprocessor);

/** What the firmware upload bindings need from the board. */
struct web_api_v1_firmware {
	/** Uptime for the store's expiry and activity; NULL: k_uptime_get(). */
	int64_t (*now_ms)(void);
};

/**
 * @brief Tell the upload bindings that firmware-store is open.
 *
 * Call after fw_store_init() on the store's directory (/lfs/firmware) has
 * succeeded. Until then createUpload, getUpload, writeUploadChunk,
 * verifyUpload and deleteUpload answer 503 service_not_ready. The store's
 * expiry of an untouched upload also needs fw_store_tick() from a periodic
 * caller - the board's coprocessor service worker. @p firmware must outlive the
 * program; NULL closes the bindings again.
 */
void web_api_v1_set_firmware(const struct web_api_v1_firmware *firmware);

/** What the STM32 update bindings need from the board (reports/stm32-update). */
struct web_api_v1_system {
	/** Uptime for the slot store's expiry and activity; NULL: k_uptime_get(). */
	int64_t (*now_ms)(void);
	/** Largest stm32u585 upload: MCUboot's slot 2 less its trailer sector. */
	uint32_t upload_max_bytes;
};

/**
 * @brief Tell the STM32 update bindings that system-image-store and
 *        system-updater are open.
 *
 * Until then a createUpload with target stm32u585 and startSystemUpdate answer
 * 503 service_not_ready, getSystemFirmware too, and capabilities report
 * stm32_update unavailable. @p system must outlive the program.
 */
void web_api_v1_set_system(const struct web_api_v1_system *system);

/** The stored Zephyr coredump, as the board keeps it (src/diagnostic/coredump_support.c). */
struct web_api_v1_coredump {
	/** Bytes of the stored dump; 0 when there is none; a negative errno when unreadable. */
	int (*size)(void);
	/** Copy up to @p len bytes from @p offset: the count copied, or a negative errno. */
	int (*read)(size_t offset, uint8_t *buf, size_t len);
	/** Forget the stored dump: 0, or a negative errno. */
	int (*clear)(void);
};

/**
 * @brief Tell the coredump bindings where the stored dump is.
 *
 * Until then getCoredump, downloadCoredump and clearCoredump answer 503
 * capability_unavailable. @p coredump must outlive the program; NULL closes
 * them again.
 */
void web_api_v1_set_coredump(const struct web_api_v1_coredump *coredump);

#ifdef __cplusplus
}
#endif

#endif /* WEB_API_V1_H_ */
