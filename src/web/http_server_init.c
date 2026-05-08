//
// Created by Kirill Shypachov on 14.09.2025.
//
#include "http_server_init.h"
#include <zephyr/net/http/service.h>
#include <zephyr/net/http/server.h>
#include <zephyr/logging/log.h>
#include <autoconf.h>
#include "authentication.h"

static uint16_t http_static_service_port = 80;
// static uint16_t http_api_service_port = 8080;
//
LOG_MODULE_REGISTER(http_server_init);
//
// HTTP_SERVER_REGISTER_HEADER_CAPTURE(capture_user_agent, "User-Agent");
// HTTP_SERVER_REGISTER_HEADER_CAPTURE(capture_authorization, "Authorization");
// HTTP_SERVER_REGISTER_HEADER_CAPTURE(capture_cookie, "Cookie");
//
HTTP_SERVICE_DEFINE(http_service, "0.0.0.0", &http_static_service_port, 2, 10, NULL, NULL, NULL);
//
// HTTP_SERVICE_DEFINE(http_api_service, "0.0.0.0", &http_api_service_port, 1, 10, NULL, NULL, NULL);

void app_http_server_init(void) {
    //authentication_init();
    http_server_start();
}