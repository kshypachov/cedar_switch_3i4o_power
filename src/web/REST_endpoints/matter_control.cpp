//
// Created by Kirill Shypachov on 06.05.2026.
//

/* Zephyr headers must NOT be wrapped in extern "C" — they have their own
 * __cplusplus guards. Wrapping pulls cbprintf_cxx.h (C++ templates) into
 * C linkage and breaks the build. */
#include <zephyr/kernel.h>
#include <zephyr/net/http/service.h>
#include <zephyr/net/http/server.h>
#include <zephyr/logging/log.h>

#include <app/server/Server.h>
#include <platform/CHIPDeviceLayer.h>
#include <setup_payload/OnboardingCodesUtil.h>
#include <setup_payload/QRCodeSetupPayloadGenerator.h>

#include <string.h>
#include <zephyr/data/json.h>

LOG_MODULE_REGISTER(REST_API_matter_control, LOG_LEVEL_DBG);

using namespace chip;
using namespace chip::DeviceLayer;

#define RESP_BUF_SZ 512

#define JSON_COMMAN_OPEN_COMMISSIONING_WINDOW "open_commissioning_window"
#define JSON_COMMAND_FACTORY_RESET "factory_reset"

struct matter_post_cmd {
    const char *action;
};

static const struct json_obj_descr matter_post_cmd_descr[] = {
    JSON_OBJ_DESCR_PRIM(struct matter_post_cmd, action, JSON_TOK_STRING),
};

static char resp_buf[RESP_BUF_SZ];
static char post_buf[256];
static size_t post_cursor;

/* Executed inside the Matter event loop via ScheduleWork */
static void do_open_commissioning_window(intptr_t)
{
    CHIP_ERROR err = Server::GetInstance()
                         .GetCommissioningWindowManager()
                         .OpenBasicCommissioningWindow();
    if (err != CHIP_NO_ERROR) {
        LOG_ERR("OpenBasicCommissioningWindow failed: %" CHIP_ERROR_FORMAT, err.Format());
    } else {
        LOG_INF("Commissioning window opened via REST");
    }
}

static void do_factory_reset(intptr_t)
{
    Server::GetInstance().ScheduleFactoryReset();
}

static int matter_control_cb(struct http_client_ctx *client,
                             enum http_transaction_status status,
                             const struct http_request_ctx *request_ctx,
                             struct http_response_ctx *response_ctx,
                             void *user_data)
{
    ARG_UNUSED(user_data);

    if (client->method == HTTP_GET) {
        LOG_INF("GET /api/matter/control");

        PlatformMgr().LockChipStack();

        bool window_open = Server::GetInstance()
                               .GetCommissioningWindowManager()
                               .IsCommissioningWindowOpen();
        uint8_t fabric_count =
            (uint8_t)Server::GetInstance().GetFabricTable().FabricCount();

        char qr_buf[QRCodeBasicSetupPayloadGenerator::kMaxQRCodeBase38RepresentationLength + 1] = {};
        char pin_buf[kManualSetupLongCodeCharLength + 1] = {};

        if (window_open) {
            MutableCharSpan qr_span(qr_buf);
            MutableCharSpan pin_span(pin_buf);
            GetQRCode(qr_span,
                      RendezvousInformationFlags(RendezvousInformationFlag::kOnNetwork));
            GetManualPairingCode(
                pin_span,
                RendezvousInformationFlags(RendezvousInformationFlag::kOnNetwork));
        }

        PlatformMgr().UnlockChipStack();

        int n;
        if (window_open) {
            n = snprintk(resp_buf, RESP_BUF_SZ,
                         "{\"commissioning_window_open\":true,"
                         "\"fabrics\":%u,"
                         "\"qr_code\":\"%s\","
                         "\"pin_code\":\"%s\"}",
                         fabric_count, qr_buf, pin_buf);
        } else {
            n = snprintk(resp_buf, RESP_BUF_SZ,
                         "{\"commissioning_window_open\":false,"
                         "\"fabrics\":%u,"
                         "\"message\":\"Commissioning window is closed\"}",
                         fabric_count);
        }

        if (n < 0 || n >= (int)RESP_BUF_SZ) {
            response_ctx->status = HTTP_500_INTERNAL_SERVER_ERROR;
            static const char err[] = "{\"error\":\"format\"}";
            response_ctx->body     = (uint8_t *)err;
            response_ctx->body_len = sizeof(err) - 1;
            response_ctx->final_chunk = true;
            return 0;
        }

        response_ctx->status      = HTTP_200_OK;
        response_ctx->body        = (uint8_t *)resp_buf;
        response_ctx->body_len    = (size_t)n;
        response_ctx->final_chunk = true;
        return 0;

    } else if (client->method == HTTP_POST) {
        LOG_INF("POST /api/matter/control");

        if (status == HTTP_SERVER_TRANSACTION_ABORTED) {
            post_cursor = 0;
            return 0;
        }

        if (request_ctx->data_len + post_cursor > sizeof(post_buf) - 1) {
            post_cursor = 0;
            return -ENOMEM;
        }

        memcpy(post_buf + post_cursor, request_ctx->data, request_ctx->data_len);
        post_cursor += request_ctx->data_len;

        if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
            return 0;
        }

        post_buf[post_cursor] = '\0';
        size_t body_len_raw = post_cursor;
        post_cursor = 0;

        LOG_DBG("POST /api/matter/control body: %s", post_buf);

        struct matter_post_cmd cmd = {};
        int64_t parsed = json_obj_parse(post_buf, body_len_raw,
                                    matter_post_cmd_descr,
                                    ARRAY_SIZE(matter_post_cmd_descr),
                                    &cmd);

        const uint8_t *body;
        size_t body_len;

        if (parsed < 0 || !(parsed & BIT(0)) || !cmd.action) {
            LOG_ERR("POST /api/matter/control: missing or invalid JSON action field");
            static const char err[] = "{\"error\":\"missing or invalid action field\"}";
            body     = (uint8_t *)err;
            body_len = sizeof(err) - 1;
            response_ctx->status = HTTP_400_BAD_REQUEST;
        } else if (strcmp(cmd.action, JSON_COMMAN_OPEN_COMMISSIONING_WINDOW) == 0) {
            PlatformMgr().ScheduleWork(do_open_commissioning_window, 0);
            static const char ok[] =
                "{\"result\":\"commissioning window opening scheduled\"}";
            body     = (uint8_t *)ok;
            body_len = sizeof(ok) - 1;
            response_ctx->status = HTTP_200_OK;
        } else if (strcmp(cmd.action, JSON_COMMAND_FACTORY_RESET) == 0) {
            PlatformMgr().ScheduleWork(do_factory_reset, 0);
            static const char ok[] = "{\"result\":\"factory reset scheduled\"}";
            body     = (uint8_t *)ok;
            body_len = sizeof(ok) - 1;
            response_ctx->status = HTTP_200_OK;
        } else {
            LOG_ERR("POST /api/matter/control: unknown action: %s", cmd.action);
            static const char err[] = "{\"error\":\"unknown action\"}";
            body     = (uint8_t *)err;
            body_len = sizeof(err) - 1;
            response_ctx->status = HTTP_400_BAD_REQUEST;
        }

        response_ctx->body        = body;
        response_ctx->body_len    = body_len;
        response_ctx->final_chunk = true;
        return 0;

    } else if (client->method == HTTP_OPTIONS) {
        LOG_INF("OPTIONS /api/matter/control");
        static struct http_header hdrs[] = {
            {.name = "Access-Control-Allow-Origin", .value = "*"},
        };
        response_ctx->status      = HTTP_200_OK;
        response_ctx->headers     = hdrs;
        response_ctx->header_count = ARRAY_SIZE(hdrs);
        response_ctx->final_chunk = true;
        return 0;
    }

    return -1;
}

/* Field order must match struct http_resource_detail declaration:
 * bitmask_of_supported_http_methods → type → content_type            */
static struct http_resource_detail_dynamic matter_control_handler = {
    .common = {
        .bitmask_of_supported_http_methods =
            BIT(HTTP_GET) | BIT(HTTP_POST) | BIT(HTTP_OPTIONS),
        .type         = HTTP_RESOURCE_TYPE_DYNAMIC,
        .content_type = "application/json",
    },
    .cb        = matter_control_cb,
    .user_data = NULL,
};

HTTP_RESOURCE_DEFINE(matter_control,
                     http_service,
                     "/api/matter/control",
                     &matter_control_handler);