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
WEB_API_V1_ROUTE(cancelJob, POST, "/jobs/{job_id}/cancel", WEB_API_CSRF | WEB_API_IDEMPOTENT | WEB_API_BODY_REQUIRED, V1_BODY(struct v1_empty_body), &v1_empty_schema, NULL, v1_cancel_job)
WEB_API_V1_ROUTE(getMatterStatus, GET, "/matter/status", 0, V1_NO_BODY, NULL, NULL, v1_get_matter_status)
WEB_API_V1_ROUTE(getCommissioningWindow, GET, "/matter/commissioning", 0, V1_NO_BODY, NULL, NULL, v1_get_commissioning_window)
WEB_API_V1_ROUTE(openCommissioningWindow, POST, "/matter/commissioning", WEB_API_CSRF | WEB_API_IDEMPOTENT | WEB_API_BODY_REQUIRED, V1_BODY(struct v1_commissioning_body), &v1_commissioning_schema, NULL, v1_open_commissioning_window)
WEB_API_V1_ROUTE(closeCommissioningWindow, DELETE, "/matter/commissioning", WEB_API_CSRF | WEB_API_IDEMPOTENT, V1_NO_BODY, NULL, NULL, v1_close_commissioning_window)
WEB_API_V1_ROUTE(getOnboardingCodes, GET, "/matter/onboarding-codes", 0, V1_NO_BODY, NULL, NULL, v1_get_onboarding_codes)
WEB_API_V1_ROUTE(listFabrics, GET, "/matter/fabrics", 0, V1_NO_BODY, NULL, NULL, v1_list_fabrics)
WEB_API_V1_ROUTE(getNetworkStatus, GET, "/network/status", 0, V1_NO_BODY, NULL, NULL, v1_get_network_status)
WEB_API_V1_ROUTE(getNetworkConfig, GET, "/network/config", 0, V1_NO_BODY, NULL, NULL, v1_get_network_config)
WEB_API_V1_ROUTE(stageNetworkConfig, POST, "/network/transactions", WEB_API_CSRF | WEB_API_IDEMPOTENT | WEB_API_BODY_REQUIRED, V1_BODY(struct v1_network_transaction_body), &v1_network_transaction_schema, NULL, v1_stage_network_config)
WEB_API_V1_ROUTE(getNetworkTransaction, GET, "/network/transactions/{transaction_id}", 0, V1_NO_BODY, NULL, NULL, v1_get_network_transaction)
WEB_API_V1_ROUTE(rollbackNetworkTransaction, DELETE, "/network/transactions/{transaction_id}", WEB_API_CSRF | WEB_API_IDEMPOTENT, V1_NO_BODY, NULL, NULL, v1_rollback_network_transaction)
WEB_API_V1_ROUTE(applyNetworkTransaction, POST, "/network/transactions/{transaction_id}/apply", WEB_API_CSRF | WEB_API_IDEMPOTENT | WEB_API_BODY_REQUIRED, V1_BODY(struct v1_apply_body), &v1_apply_schema, NULL, v1_apply_network_transaction)
WEB_API_V1_ROUTE(confirmNetworkTransaction, POST, "/network/transactions/{transaction_id}/confirm", WEB_API_CSRF | WEB_API_IDEMPOTENT | WEB_API_BODY_REQUIRED, V1_BODY(struct v1_empty_body), &v1_empty_schema, NULL, v1_confirm_network_transaction)
WEB_API_V1_ROUTE(scanWiFi, POST, "/network/wifi/scans", WEB_API_CSRF | WEB_API_IDEMPOTENT | WEB_API_BODY_REQUIRED, V1_BODY(struct v1_empty_body), &v1_empty_schema, NULL, v1_scan_wifi)
WEB_API_V1_ROUTE(getWiFiScan, GET, "/network/wifi/scans/{job_id}", 0, V1_NO_BODY, NULL, NULL, v1_get_wifi_scan)
WEB_API_V1_ROUTE(getLogSources, GET, "/logs/sources", 0, V1_NO_BODY, NULL, NULL, v1_get_log_sources)
WEB_API_V1_ROUTE(getLogRecords, GET, "/logs/records", 0, V1_NO_BODY, NULL, v1_log_records_query, v1_get_log_records)
WEB_API_V1_ROUTE(exportLogs, GET, "/logs/export", 0, V1_NO_BODY, NULL, v1_log_export_query, v1_export_logs)
WEB_API_V1_ROUTE(getCoprocessorStatus, GET, "/coprocessor/status", 0, V1_NO_BODY, NULL, NULL, v1_get_coprocessor_status)
WEB_API_V1_ROUTE(startCoprocessorUpdate, POST, "/coprocessor/updates", WEB_API_CSRF | WEB_API_IDEMPOTENT | WEB_API_BODY_REQUIRED, V1_BODY(struct v1_update_body), &v1_update_schema, NULL, v1_start_coprocessor_update)
WEB_API_V1_ROUTE(listUploads, GET, "/firmware/uploads", 0, V1_NO_BODY, NULL, NULL, v1_list_uploads)
WEB_API_V1_ROUTE(createUpload, POST, "/firmware/uploads", WEB_API_CSRF | WEB_API_IDEMPOTENT | WEB_API_BODY_REQUIRED, V1_BODY(struct v1_upload_body), &v1_upload_schema, NULL, v1_create_upload)
WEB_API_V1_ROUTE(getUpload, GET, "/firmware/uploads/{upload_id}", 0, V1_NO_BODY, NULL, NULL, v1_get_upload)
WEB_API_V1_ROUTE(deleteUpload, DELETE, "/firmware/uploads/{upload_id}", WEB_API_CSRF | WEB_API_IDEMPOTENT, V1_NO_BODY, NULL, NULL, v1_delete_upload)
WEB_API_V1_ROUTE(writeUploadChunk, PUT, "/firmware/uploads/{upload_id}/data", WEB_API_CSRF | WEB_API_IDEMPOTENT | WEB_API_BODY_REQUIRED | WEB_API_BODY_OCTETS, V1_NO_BODY, NULL, v1_upload_chunk_query, v1_write_upload_chunk)
WEB_API_V1_ROUTE(verifyUpload, POST, "/firmware/uploads/{upload_id}/verify", WEB_API_CSRF | WEB_API_IDEMPOTENT | WEB_API_BODY_REQUIRED, V1_BODY(struct v1_empty_body), &v1_empty_schema, NULL, v1_verify_upload)
WEB_API_V1_ROUTE(getSystemFirmware, GET, "/system/firmware", 0, V1_NO_BODY, NULL, NULL, v1_get_system_firmware)
WEB_API_V1_ROUTE(startSystemUpdate, POST, "/system/updates", WEB_API_CSRF | WEB_API_IDEMPOTENT | WEB_API_BODY_REQUIRED, V1_BODY(struct v1_system_update_body), &v1_system_update_schema, NULL, v1_start_system_update)
WEB_API_V1_ROUTE(getCoredump, GET, "/system/coredump", 0, V1_NO_BODY, NULL, NULL, v1_get_coredump)
WEB_API_V1_ROUTE(downloadCoredump, GET, "/system/coredump/data", 0, V1_NO_BODY, NULL, NULL, v1_download_coredump)
WEB_API_V1_ROUTE(clearCoredump, DELETE, "/system/coredump", WEB_API_CSRF, V1_NO_BODY, NULL, NULL, v1_clear_coredump)
/* clang-format on */
