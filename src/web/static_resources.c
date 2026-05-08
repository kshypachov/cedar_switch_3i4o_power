#include <zephyr/kernel.h>
#include <zephyr/net/http/service.h>
#include <zephyr/net/http/server.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(static_resources, LOG_LEVEL_INF);

/*
 * Generated from src/web/frontend/dist/* in CMakeLists.txt.
 * Produces HTTP_RESOURCE_DEFINE entries bound to upload_service.
 */
#include "web_resources.inc"
