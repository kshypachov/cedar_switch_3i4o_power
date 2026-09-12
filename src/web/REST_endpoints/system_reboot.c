//
// Created by Kirill Shypachov on 17.09.2025.
//

#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/service.h>
#include <zephyr/net/http/server.h>

LOG_MODULE_REGISTER(system_reboot);

static void reboot(void) {
    LOG_INF("Reboot device by REST API");
    k_sleep(K_SECONDS(5));
    sys_reboot(SYS_REBOOT_COLD);
}

static int reboot_handler(struct http_client_ctx *client,
                            enum http_transaction_status status,
                            const struct http_request_ctx *request_ctx,
                            struct http_response_ctx *response_ctx,
                            void *user_data) {

    ARG_UNUSED(request_ctx);
    ARG_UNUSED(user_data);

    if (client->method == HTTP_POST) {
        reboot();
    }
    return 0;
}

static struct http_resource_detail_dynamic reboot_resource_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_POST),
        //.content_type = "application/json"
    },
    .cb = reboot_handler,
    .user_data = NULL,
};

/* === Register path for HTTP service only === */
HTTP_RESOURCE_DEFINE(api_system_reboot,
                     http_service,
                     "/api/system/reboot",
                     &reboot_resource_detail);