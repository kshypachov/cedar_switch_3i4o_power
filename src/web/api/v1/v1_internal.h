/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Declarations shared by the v1 binding files and the route table.
 */

#ifndef WEB_API_V1_INTERNAL_H_
#define WEB_API_V1_INTERNAL_H_

#include <zephyr/kernel.h>

#include <job_manager/job_manager.h>
#include <web_api/web_api.h>
#include <web_auth/web_auth.h>

#include "web_api_v1.h"

#define V1_NO_BODY   0U
#define V1_BODY(type) sizeof(type)

/* SetupRequest */
struct v1_setup_body {
	char password[WEB_AUTH_PASSWORD_MAX_BYTES + 1];
};

/* LoginRequest */
struct v1_login_body {
	char password[WEB_AUTH_PASSWORD_MAX_BYTES + 1];
};

/* PasswordRequest */
struct v1_password_body {
	char current_password[WEB_AUTH_PASSWORD_MAX_BYTES + 1];
	char new_password[WEB_AUTH_PASSWORD_MAX_BYTES + 1];
};

/* CommissioningRequest; mode is checked against its enum, so 16 bytes hold
 * every value a client might send before it is refused. */
struct v1_commissioning_body {
	char mode[16];
	int64_t timeout_seconds;
};

/* IPv4Config. Buffers are wider than any address, so a long non-address is
 * refused by its format rather than as too long, as the mock refuses it. */
#define V1_IP_TEXT_MAX 46

struct v1_ipv4_body {
	char mode[16];
	char address[V1_IP_TEXT_MAX];
	bool address_null;
	int64_t prefix_length;
	bool prefix_length_null;
	char gateway[V1_IP_TEXT_MAX];
	bool gateway_null;
};

/* CredentialChange, both branches at once: 64 code points of up to four bytes. */
struct v1_credential_body {
	char action[16];
	char value[64 * 4 + 1];
	bool value_present;
};

/* EthernetConfig */
struct v1_ethernet_body {
	bool enabled;
	struct v1_ipv4_body ipv4;
};

/* WiFiConfigInput; ssid_base64 is at most 44 characters. */
struct v1_wifi_body {
	bool enabled;
	char ssid_base64[45];
	char security[32];
	bool hidden;
	struct v1_ipv4_body ipv4;
	struct v1_credential_body credential;
};

/* DNSConfig */
struct v1_dns_body {
	char mode[16];
	char servers[2][V1_IP_TEXT_MAX];
	size_t server_count;
};

struct v1_interfaces_body {
	struct v1_ethernet_body ethernet;
	struct v1_wifi_body wifi;
};

/* NetworkConfigInput */
struct v1_network_config_body {
	char preferred_interface[16];
	struct v1_dns_body dns;
	struct v1_interfaces_body interfaces;
};

/* NetworkTransactionRequest */
struct v1_network_transaction_body {
	int64_t base_revision;
	struct v1_network_config_body config;
};

/* NetworkApplyRequest */
struct v1_apply_body {
	int64_t confirmation_timeout_seconds;
};

/* Empty */
struct v1_empty_body {
	uint8_t unused;
};

/* UpdateRequest; upload_id wider than 64 so a longer id is too_long, not cut. */
struct v1_update_body {
	char upload_id[128 + 1];
	char method[16];
	bool acknowledge_recovery;
};

/* UploadRequest; 128 code points of up to four bytes. sha256 is wider than its
 * 64 digits so a longer value is refused by its format, as the mock refuses it. */
struct v1_upload_body {
	char filename[128 * 4 + 1];
	int64_t size_bytes;
	char sha256[128 + 1];
	/* Checked against its enum; empty when absent, which means esp32c6. */
	char target[16];
};

/* SystemUpdateRequest; upload_id wider than 64 so a longer id is too_long, not cut. */
struct v1_system_update_body {
	char upload_id[128 + 1];
	bool acknowledge_downgrade;
};

extern const struct web_json_object v1_setup_schema;
extern const struct web_json_object v1_login_schema;
extern const struct web_json_object v1_password_schema;
extern const struct web_json_object v1_commissioning_schema;
extern const struct web_json_object v1_network_transaction_schema;
extern const struct web_json_object v1_apply_schema;
extern const struct web_json_object v1_empty_schema;
extern const struct web_json_object v1_upload_schema;
extern const struct web_json_object v1_update_schema;
extern const struct web_json_object v1_system_update_schema;

void v1_get_auth_state(struct web_api_call *call);
void v1_setup_admin(struct web_api_call *call);
void v1_login(struct web_api_call *call);
void v1_get_session(struct web_api_call *call);
void v1_logout(struct web_api_call *call);
void v1_change_password(struct web_api_call *call);
void v1_get_system_status(struct web_api_call *call);
void v1_get_capabilities(struct web_api_call *call);
void v1_get_job(struct web_api_call *call);
void v1_get_matter_status(struct web_api_call *call);
void v1_get_commissioning_window(struct web_api_call *call);
void v1_open_commissioning_window(struct web_api_call *call);
void v1_close_commissioning_window(struct web_api_call *call);
void v1_get_onboarding_codes(struct web_api_call *call);
void v1_list_fabrics(struct web_api_call *call);
void v1_get_network_status(struct web_api_call *call);
void v1_get_network_config(struct web_api_call *call);
void v1_stage_network_config(struct web_api_call *call);
void v1_get_network_transaction(struct web_api_call *call);
void v1_apply_network_transaction(struct web_api_call *call);
void v1_confirm_network_transaction(struct web_api_call *call);
void v1_rollback_network_transaction(struct web_api_call *call);
void v1_scan_wifi(struct web_api_call *call);
void v1_get_wifi_scan(struct web_api_call *call);
void v1_get_log_sources(struct web_api_call *call);
void v1_get_log_records(struct web_api_call *call);
void v1_export_logs(struct web_api_call *call);
void v1_get_coprocessor_status(struct web_api_call *call);
void v1_create_upload(struct web_api_call *call);
void v1_get_upload(struct web_api_call *call);
void v1_write_upload_chunk(struct web_api_call *call);
void v1_verify_upload(struct web_api_call *call);
void v1_delete_upload(struct web_api_call *call);
void v1_cancel_job(struct web_api_call *call);
void v1_start_coprocessor_update(struct web_api_call *call);
void v1_get_system_firmware(struct web_api_call *call);
void v1_start_system_update(struct web_api_call *call);
void v1_get_coredump(struct web_api_call *call);
void v1_download_coredump(struct web_api_call *call);
void v1_clear_coredump(struct web_api_call *call);

/** The board's STM32 update hooks, or NULL until it opened both modules (system_update.c). */
const struct web_api_v1_system *v1_system(void);

/**
 * Why an STM32 install could not be accepted now - `firmware_unconfirmed`,
 * `update_running`, `service_not_ready`, `not_implemented` - or NULL (system_update.c).
 */
const char *v1_system_update_unavailable_reason(void);

/** capabilities.limits.system_upload_max_bytes: the board's, or the slot's design size (system_update.c). */
uint32_t v1_system_upload_max_bytes(void);

/** An install of the STM32 or of the C6 is accepted or running (system_update.c). */
bool v1_install_running(void);

/** Ids of system-image-store start with this; firmware-store's with "upload_". */
#define V1_SYSTEM_UPLOAD_PREFIX "sysimg_"

/** The board says @p req arrived on the Ethernet interface; false without its hook (coprocessor.c). */
bool v1_request_over_ethernet(const struct web_api_request *req);

/* Query parameters of the log operations, for routes.h (logs.c). */
extern const char *const v1_log_records_query[];
extern const char *const v1_log_export_query[];
/* writeUploadChunk's `offset` (firmware.c). */
extern const char *const v1_upload_chunk_query[];

/**
 * Why a UART install could not start now - the UART's owner is not the console
 * - or NULL (coprocessor.c). The C6's state never enters into it: an install is
 * how a C6 with no firmware gets some.
 */
const char *v1_uart_update_unavailable_reason(void);

/** The upload an upload_chunk or firmware_verify job worked on, as a URL in @p buf, or NULL (firmware.c). */
const char *v1_upload_job_resource_url(const struct job_snapshot *job, char *buf, size_t cap);

/** Run @p work on the v1 job worker, the one thread of firmware-store's file I/O (auth.c). */
int v1_worker_submit(struct k_work *work);

/**
 * Why ESP32 logs are unavailable - the UART's owner is not the console, or
 * the store has not started - or NULL; @p generation gets the C6's (logs.c).
 */
const char *v1_esp32_logs_unavailable_reason(uint32_t *generation);

/** The identity given to web_api_v1_init(). */
const struct web_api_v1_identity *v1_identity(void);

/** Start the worker that runs password changes (auth.c). */
int v1_auth_worker_start(void);

/**
 * The resource @p job acts on, or NULL. A scan's is its own results, so the
 * URL is built into @p buf.
 */
const char *v1_job_resource_url(const struct job_snapshot *job, char *buf, size_t cap);

/** Wi-Fi security modes to publish: BIT(enum device_config_wifi_security) (network.c). */
uint32_t v1_wifi_security_modes(void);

#endif /* WEB_API_V1_INTERNAL_H_ */
