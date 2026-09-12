//
// Created by Kirill Shypachov on 06.05.2026.
//
#include <zephyr/kernel.h>
#include <zephyr/net/http/service.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/logging/log.h>
#include <inttypes.h>
#include <string.h>
#include "../http_common.h"

LOG_MODULE_REGISTER(REST_API_device_info, LOG_LEVEL_DBG);

#define FW_VERSION   "1.0.0"
#define RESP_BUF_SZ  1024

static char resp_buf[RESP_BUF_SZ];

struct build_ctx {
    size_t pos;
    bool   first_iface;
    int    iface_idx;
};

static void append_iface(struct net_if *iface, void *ud)
{
    struct build_ctx *ctx = ud;

    if (ctx->pos >= RESP_BUF_SZ) {
        return;
    }

    if (!ctx->first_iface) {
        ctx->pos += snprintk(resp_buf + ctx->pos, RESP_BUF_SZ - ctx->pos, ",");
    }
    ctx->first_iface = false;

    /* MAC address */
    struct net_linkaddr *ll = net_if_get_link_addr(iface);
    char mac_str[18] = "N/A";
    if (ll && ll->len == 6) {
        snprintk(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                 ll->addr[0], ll->addr[1], ll->addr[2],
                 ll->addr[3], ll->addr[4], ll->addr[5]);
    }

    ctx->pos += snprintk(resp_buf + ctx->pos, RESP_BUF_SZ - ctx->pos,
                         "{\"index\":%d,\"mac\":\"%s\",\"ipv4\":[",
                         ctx->iface_idx++, mac_str);

    /* IPv4 addresses */
    bool first = true;
    char addr_str[NET_IPV6_ADDR_LEN];

#if defined(CONFIG_NET_IPV4)
    struct net_if_ipv4 *ipv4 = iface->config.ip.ipv4;
    if (ipv4) {
        for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
            if (!ipv4->unicast[i].ipv4.is_used) {
                continue;
            }
            net_addr_ntop(AF_INET,
                          &ipv4->unicast[i].ipv4.address.in_addr,
                          addr_str, sizeof(addr_str));
            ctx->pos += snprintk(resp_buf + ctx->pos, RESP_BUF_SZ - ctx->pos,
                                 "%s\"%s\"", first ? "" : ",", addr_str);
            first = false;
        }
    }
#endif

    ctx->pos += snprintk(resp_buf + ctx->pos, RESP_BUF_SZ - ctx->pos, "],\"ipv6\":[");

    /* IPv6 addresses */
    first = true;

#if defined(CONFIG_NET_IPV6)
    struct net_if_ipv6 *ipv6 = iface->config.ip.ipv6;
    if (ipv6) {
        for (int i = 0; i < NET_IF_MAX_IPV6_ADDR; i++) {
            if (!ipv6->unicast[i].is_used) {
                continue;
            }
            net_addr_ntop(AF_INET6,
                          &ipv6->unicast[i].address.in6_addr,
                          addr_str, sizeof(addr_str));
            ctx->pos += snprintk(resp_buf + ctx->pos, RESP_BUF_SZ - ctx->pos,
                                 "%s\"%s\"", first ? "" : ",", addr_str);
            first = false;
        }
    }
#endif

    ctx->pos += snprintk(resp_buf + ctx->pos, RESP_BUF_SZ - ctx->pos, "]}");
}

static int device_info_handler(struct http_client_ctx *client,
                               enum http_transaction_status status,
                               const struct http_request_ctx *request_ctx,
                               struct http_response_ctx *response_ctx,
                               void *user_data)
{
    ARG_UNUSED(status);
    ARG_UNUSED(request_ctx);
    ARG_UNUSED(user_data);

    if (client->method == HTTP_GET) {
        LOG_INF("GET /api/device/info");

        int64_t uptime_ms = k_uptime_get();

        struct build_ctx ctx = {
            .pos         = 0,
            .first_iface = true,
            .iface_idx   = 0,
        };

        ctx.pos += snprintk(resp_buf, RESP_BUF_SZ,
                            "{\"firmware_version\":\"%s\","
                            "\"uptime_ms\":%" PRId64 ","
                            "\"interfaces\":[",
                            FW_VERSION, uptime_ms);

        net_if_foreach(append_iface, &ctx);

        ctx.pos += snprintk(resp_buf + ctx.pos, RESP_BUF_SZ - ctx.pos, "]}");

        if (ctx.pos >= RESP_BUF_SZ) {
            response_ctx->status = HTTP_500_INTERNAL_SERVER_ERROR;
            static const char err[] = "{\"error\":\"buffer overflow\"}";
            response_ctx->body = (uint8_t *)err;
            response_ctx->body_len = sizeof(err) - 1;
            response_ctx->final_chunk = true;
            return 0;
        }

        LOG_DBG("GET /api/device/info: %zu bytes", ctx.pos);
        response_ctx->status = HTTP_200_OK;
        response_ctx->body = (uint8_t *)resp_buf;
        response_ctx->body_len = ctx.pos;
        response_ctx->final_chunk = true;
        return 0;

    } else if (client->method == HTTP_OPTIONS) {
        LOG_INF("OPTIONS /api/device/info");

        static struct http_header hdrs[] = {
            {.name = "Access-Control-Allow-Origin", .value = "*"},
        };
        response_ctx->status = HTTP_200_OK;
        response_ctx->headers = hdrs;
        response_ctx->header_count = ARRAY_SIZE(hdrs);
        response_ctx->final_chunk = true;
        return 0;

    } else {
        return -1;
    }
}

static struct http_resource_detail_dynamic device_info_api = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_GET) | BIT(HTTP_OPTIONS),
        .content_type = "application/json"
    },
    .cb = device_info_handler,
    .user_data = NULL,
};

HTTP_RESOURCE_DEFINE(api_device_info,
                     http_service,
                     "/api/device/info",
                     &device_info_api);