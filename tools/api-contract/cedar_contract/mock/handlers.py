# SPDX-License-Identifier: Apache-2.0
"""One handler per declared operation, grouped the way the API is.

A handler is deliberately thin. Everything that could be got wrong twice —
authentication, the CSRF token, required headers, the request schema, the
idempotency key — has already happened in `app.py`, driven by the document. What
is left here is the call into the state machine and the shape of the success
response, which is the part that differs per operation.

Registration is by `operationId`. That is the only name in the system that the
document, the generated client and this file all agree on, and binding to it
means a renamed path changes nothing here while a renamed operation fails loudly
at construction.
"""

from __future__ import annotations

from ..errors import error
from .wire import Context, Response, accepted, handles, json_response

# -- session and device ----------------------------------------------------


@handles("getAuthState")
def get_auth_state(app, ctx: Context) -> Response:
    return json_response(200, app.state.auth.state_json())


@handles("setupAdmin")
def setup_admin(app, ctx: Context) -> Response:
    session = app.state.auth.setup(ctx.body["password"], ctx.request.headers.get("x-setup-token"))
    return json_response(
        201,
        app.state.auth.session_json(session),
        {"Location": "/api/v1/auth/session", "Set-Cookie": app.state.auth.cookie(session)},
    )


@handles("login")
def login(app, ctx: Context) -> Response:
    session = app.state.auth.login(ctx.body["password"])
    return json_response(
        200,
        app.state.auth.session_json(session),
        {"Set-Cookie": app.state.auth.cookie(session)},
    )


@handles("getSession")
def get_session(app, ctx: Context) -> Response:
    return json_response(200, app.state.auth.session_json(ctx.session))


@handles("logout")
def logout(app, ctx: Context) -> Response:
    app.state.auth.logout(ctx.session)
    # 204: no body at all, and the cookie is expired rather than left to rot.
    return Response(204, b"", {"Set-Cookie": app.state.auth.expiring_cookie()})


@handles("changePassword")
def change_password(app, ctx: Context) -> Response:
    auth = app.state.auth
    auth.change_password(ctx.body["current_password"], ctx.body["new_password"])
    from .constants import PASSWORD_CHANGE_MS
    from .jobs import Step

    job = app.state.jobs.create(
        "password_change",
        [Step("hashing", PASSWORD_CHANGE_MS, on_done=auth.commit_password)],
        resource_url="/api/v1/auth/session",
    )
    return accepted(job.id, "/api/v1/auth/session")


@handles("getSystemStatus")
def get_system_status(app, ctx: Context) -> Response:
    return json_response(200, app.state.system_status_json())


@handles("getCapabilities")
def get_capabilities(app, ctx: Context) -> Response:
    return json_response(200, app.state.capabilities_json())


@handles("getCoprocessorStatus")
def get_coprocessor_status(app, ctx: Context) -> Response:
    return json_response(200, app.state.coprocessor.status_json())


# -- jobs ------------------------------------------------------------------


@handles("getJob")
def get_job(app, ctx: Context) -> Response:
    job = app.state.jobs.get(ctx.params["job_id"])
    if job is None:
        raise error("not_found", "No such job")
    return json_response(200, job.to_json(app.state.boot_ms))


@handles("cancelJob")
def cancel_job(app, ctx: Context) -> Response:
    job = app.state.jobs.get(ctx.params["job_id"])
    if job is None:
        raise error("not_found", "No such job")
    if job.kind == "network_apply":
        # The contract routes this one through the transaction, so that a
        # cancellation cannot become a second network operation racing the first.
        raise error(
            "invalid_state",
            "Cancel a network apply through DELETE on its transaction",
        )
    if not job.cancellable:
        raise error(
            "invalid_state", f"A {job.kind} job in state {job.state!r} cannot be cancelled"
        )
    job.finish(
        app.clock.now_ms(),
        "cancelled",
        error("invalid_state", "Cancelled by the administrator"),
    )
    if job.kind == "coprocessor_update":
        app.state.coprocessor.cancelled(job)
    elif job.kind == "firmware_verify":
        app.state.firmware.verify_cancelled(job)
    return accepted(job.id, job.resource_url)


# -- Matter ----------------------------------------------------------------


@handles("getMatterStatus")
def get_matter_status(app, ctx: Context) -> Response:
    return json_response(200, app.state.matter.status_json())


@handles("getCommissioningWindow")
def get_commissioning_window(app, ctx: Context) -> Response:
    return json_response(200, app.state.matter.window_json())


@handles("openCommissioningWindow")
def open_commissioning_window(app, ctx: Context) -> Response:
    job_id = app.state.matter.open_window(ctx.body["mode"], ctx.body["timeout_seconds"])
    return accepted(job_id, "/api/v1/matter/commissioning")


@handles("closeCommissioningWindow")
def close_commissioning_window(app, ctx: Context) -> Response:
    return accepted(app.state.matter.close_window(), "/api/v1/matter/commissioning")


@handles("getOnboardingCodes")
def get_onboarding_codes(app, ctx: Context) -> Response:
    return json_response(200, app.state.matter.codes_json())


@handles("listFabrics")
def list_fabrics(app, ctx: Context) -> Response:
    return json_response(200, app.state.matter.fabrics_json())


# -- network ---------------------------------------------------------------


@handles("getNetworkStatus")
def get_network_status(app, ctx: Context) -> Response:
    return json_response(200, app.state.network.status_json())


@handles("getNetworkConfig")
def get_network_config(app, ctx: Context) -> Response:
    return json_response(200, app.state.network.config_json())


@handles("stageNetworkConfig")
def stage_network_config(app, ctx: Context) -> Response:
    tx = app.state.network.stage(ctx.body)
    body = app.state.network.transaction_json(tx)
    return json_response(201, body, {"Location": f"/api/v1/network/transactions/{tx.id}"})


@handles("getNetworkTransaction")
def get_network_transaction(app, ctx: Context) -> Response:
    tx = app.state.network.find(ctx.params["transaction_id"])
    return json_response(200, app.state.network.transaction_json(tx))


@handles("applyNetworkTransaction")
def apply_network_transaction(app, ctx: Context) -> Response:
    tx = app.state.network.find(ctx.params["transaction_id"])
    job_id = app.state.network.apply(tx, ctx.body["confirmation_timeout_seconds"])
    return accepted(job_id, "/api/v1/network/config")


@handles("confirmNetworkTransaction")
def confirm_network_transaction(app, ctx: Context) -> Response:
    tx = app.state.network.find(ctx.params["transaction_id"])
    return accepted(app.state.network.confirm(tx), "/api/v1/network/config")


@handles("rollbackNetworkTransaction")
def rollback_network_transaction(app, ctx: Context) -> Response:
    tx = app.state.network.find(ctx.params["transaction_id"])
    return accepted(app.state.network.discard(tx), "/api/v1/network/config")


@handles("scanWiFi")
def scan_wifi(app, ctx: Context) -> Response:
    job_id = app.state.wifi.start()
    return accepted(job_id, f"/api/v1/network/wifi/scans/{job_id}")


@handles("getWiFiScan")
def get_wifi_scan(app, ctx: Context) -> Response:
    return json_response(200, app.state.wifi.results_json(ctx.params["job_id"]))


# -- logs ------------------------------------------------------------------


@handles("getLogSources")
def get_log_sources(app, ctx: Context) -> Response:
    return json_response(200, app.state.logs.sources_json())


@handles("getLogRecords")
def get_log_records(app, ctx: Context) -> Response:
    return json_response(200, app.state.logs.page(_query(ctx)))


@handles("exportLogs")
def export_logs(app, ctx: Context) -> Response:
    content_type, body = app.state.logs.export(_query(ctx))
    extension = "txt" if ctx.query_one("format", "ndjson") == "text" else "ndjson"
    return Response(
        200,
        body,
        {
            "Content-Type": content_type,
            # An attachment, because the contract calls the export a snapshot to
            # download rather than a page to render.
            "Content-Disposition": f'attachment; filename="cedar-logs.{extension}"',
        },
    )


def _query(ctx: Context) -> dict[str, object]:
    """Declared query parameters only, already validated by the middleware."""
    out: dict[str, object] = {}
    for param in ctx.operation.query_parameters:
        value = ctx.query_one(param.name)
        if value is None:
            continue
        out[param.name] = int(value) if param.schema.get("type") == "integer" else value
    return out


# -- firmware and the coprocessor -----------------------------------------


@handles("createUpload")
def create_upload(app, ctx: Context) -> Response:
    upload = app.state.firmware.create(ctx.body)
    return json_response(
        201,
        upload.to_json(),
        {"Location": f"/api/v1/firmware/uploads/{upload.id}"},
    )


@handles("getUpload")
def get_upload(app, ctx: Context) -> Response:
    upload = app.state.firmware.find(ctx.params["upload_id"])
    return json_response(200, upload.to_json())


@handles("writeUploadChunk")
def write_upload_chunk(app, ctx: Context) -> Response:
    upload = app.state.firmware.find(ctx.params["upload_id"])
    offset = int(ctx.query_one("offset", "0") or 0)
    job_id = app.state.firmware.write_chunk(upload, offset, ctx.body or b"")
    return accepted(job_id, f"/api/v1/firmware/uploads/{upload.id}")


@handles("verifyUpload")
def verify_upload(app, ctx: Context) -> Response:
    upload = app.state.firmware.find(ctx.params["upload_id"])
    return accepted(app.state.firmware.verify(upload), f"/api/v1/firmware/uploads/{upload.id}")


@handles("deleteUpload")
def delete_upload(app, ctx: Context) -> Response:
    upload = app.state.firmware.find(ctx.params["upload_id"])
    return accepted(app.state.firmware.delete(upload), None)


@handles("startCoprocessorUpdate")
def start_coprocessor_update(app, ctx: Context) -> Response:
    job_id = app.state.coprocessor.start_update(ctx.body)
    return accepted(job_id, "/api/v1/coprocessor/status")
