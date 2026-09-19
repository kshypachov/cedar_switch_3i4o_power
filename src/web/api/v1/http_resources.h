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
 * number than the general one it overlaps: the apply and confirm paths of a
 * transaction come before the transaction itself.
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
WEB_API_V1_RESOURCE(web_api_06_a_job_cancel, "/api/v1/jobs/*/cancel")
WEB_API_V1_RESOURCE(web_api_06_jobs, "/api/v1/jobs/*")
WEB_API_V1_RESOURCE(web_api_07_matter_status, "/api/v1/matter/status")
WEB_API_V1_RESOURCE(web_api_08_matter_commissioning, "/api/v1/matter/commissioning")
WEB_API_V1_RESOURCE(web_api_09_matter_onboarding_codes, "/api/v1/matter/onboarding-codes")
WEB_API_V1_RESOURCE(web_api_10_matter_fabrics, "/api/v1/matter/fabrics")
WEB_API_V1_RESOURCE(web_api_11_network_status, "/api/v1/network/status")
WEB_API_V1_RESOURCE(web_api_12_network_config, "/api/v1/network/config")
WEB_API_V1_RESOURCE(web_api_13_network_transactions, "/api/v1/network/transactions")
WEB_API_V1_RESOURCE(web_api_14_network_transaction_apply, "/api/v1/network/transactions/*/apply")
WEB_API_V1_RESOURCE(web_api_15_network_transaction_confirm, "/api/v1/network/transactions/*/confirm")
WEB_API_V1_RESOURCE(web_api_16_network_transaction, "/api/v1/network/transactions/*")
WEB_API_V1_RESOURCE(web_api_17_network_wifi_scans, "/api/v1/network/wifi/scans")
WEB_API_V1_RESOURCE(web_api_18_network_wifi_scan, "/api/v1/network/wifi/scans/*")
WEB_API_V1_RESOURCE(web_api_19_logs_sources, "/api/v1/logs/sources")
WEB_API_V1_RESOURCE(web_api_20_logs_records, "/api/v1/logs/records")
WEB_API_V1_RESOURCE(web_api_21_logs_export, "/api/v1/logs/export")
WEB_API_V1_RESOURCE(web_api_22_coprocessor_status, "/api/v1/coprocessor/status")
WEB_API_V1_RESOURCE(web_api_23_firmware_uploads, "/api/v1/firmware/uploads")
WEB_API_V1_RESOURCE(web_api_24_firmware_upload_data, "/api/v1/firmware/uploads/*/data")
WEB_API_V1_RESOURCE(web_api_25_firmware_upload_verify, "/api/v1/firmware/uploads/*/verify")
WEB_API_V1_RESOURCE(web_api_26_firmware_upload, "/api/v1/firmware/uploads/*")
WEB_API_V1_RESOURCE(web_api_27_coprocessor_updates, "/api/v1/coprocessor/updates")
WEB_API_V1_RESOURCE(web_api_28_system_firmware, "/api/v1/system/firmware")
WEB_API_V1_RESOURCE(web_api_29_system_updates, "/api/v1/system/updates")
WEB_API_V1_RESOURCE(web_api_30_a_system_coredump_data, "/api/v1/system/coredump/data")
WEB_API_V1_RESOURCE(web_api_30_system_coredump, "/api/v1/system/coredump")
/* clang-format on */
