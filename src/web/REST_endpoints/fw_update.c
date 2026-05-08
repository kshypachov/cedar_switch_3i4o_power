//
// Created by Kirill Shypachov on 03.05.2026.
//

#include <zephyr/kernel.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/storage/stream_flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>


#include "zephyr/dfu/mcuboot.h"

LOG_MODULE_REGISTER(http_flash_upload);

static uint16_t http_api_service_port = 8080;

extern int boot_set_pending(int permanent);

/* ───────── Flash partition ───────── */
/*
 * Укажите нужный узел из DTS, например:
 *   storage_partition, slot1_partition, и т.д.
 * Смотрите: west build -t guiconfig → Device Tree
 */
#define UPLOAD_FLASH_NODE   DT_NODELABEL(slot1_partition)
#define UPLOAD_FLASH_OFFSET DT_REG_ADDR(UPLOAD_FLASH_NODE)
#define UPLOAD_FLASH_SIZE   DT_REG_SIZE(UPLOAD_FLASH_NODE)

/* ───────── Stream Flash контекст ───────── */
static struct stream_flash_ctx sf_ctx;

/* Буфер должен быть кратен write-block-size устройства и ≤ размеру страницы */
static uint8_t sf_buf[4096];

static bool upload_active = false;

/* ───────── HTTP сервис ───────── */
HTTP_SERVICE_DEFINE(upload_service, "0.0.0.0", &http_api_service_port, 4, 10, NULL, NULL, NULL);

/* ───────── Обработчик POST /upload ───────── */
static int upload_handler(struct http_client_ctx *client,
                           enum http_data_status status,
                           const struct http_request_ctx *request_ctx,
                           struct http_response_ctx *response_ctx,
                           void *user_data)
{
    const struct device *flash_dev =
        DEVICE_DT_GET(DT_MTD_FROM_FIXED_PARTITION(UPLOAD_FLASH_NODE));

    int ret = 0;

    /* ── Запрос прерван клиентом ── */
    if (status == HTTP_SERVER_DATA_ABORTED) {
        LOG_WRN("Upload aborted");
        upload_active = false;
        return 0;
    }

    /* ── Первый чанк: инициализируем Stream Flash ── */
    if (!upload_active) {
        if (!device_is_ready(flash_dev)) {
            LOG_ERR("Flash device not ready");
            return -ENODEV;
        }

        ret = stream_flash_init(&sf_ctx,
                                flash_dev,
                                sf_buf,
                                sizeof(sf_buf),
                                UPLOAD_FLASH_OFFSET,  /* offset в байтах */
                                UPLOAD_FLASH_SIZE,    /* макс. размер */
                                NULL);                /* callback верификации */
        if (ret < 0) {
            LOG_ERR("stream_flash_init failed: %d", ret);
            return ret;
        }

        upload_active = true;
        LOG_INF("Upload started, flash offset=0x%08X size=%zu",
                UPLOAD_FLASH_OFFSET, UPLOAD_FLASH_SIZE);
    }

    /* ── Записываем пришедший чанк в Stream Flash ── */
    if (request_ctx->data_len > 0) {
        /*
         * flush=false — данные накапливаются в sf_buf,
         * физическая запись во Flash происходит только когда
         * буфер заполнен (размер страницы). Это экономит циклы.
         */
        ret = stream_flash_buffered_write(&sf_ctx,
                                          request_ctx->data,
                                          request_ctx->data_len,
                                          false);
        if (ret < 0) {
            LOG_ERR("stream_flash_buffered_write failed: %d", ret);
            upload_active = false;
            return ret;
        }

        LOG_DBG("Written chunk: %zu bytes (total: %zu)",
                request_ctx->data_len,
                stream_flash_bytes_written(&sf_ctx));
    }

    /* ── Последний чанк: сбрасываем остаток буфера во Flash ── */
    if (status == HTTP_SERVER_DATA_FINAL) {
        /*
         * flush=true — принудительно записывает оставшиеся
         * байты в буфере, даже если буфер заполнен не полностью.
         * Хвост дополняется 0xFF до выравнивания write-block-size.
         */
        ret = stream_flash_buffered_write(&sf_ctx, NULL, 0, true);
        if (ret < 0) {
            LOG_ERR("Final flush failed: %d", ret);
            upload_active = false;
            return ret;
        }

        size_t total = stream_flash_bytes_written(&sf_ctx);
        LOG_INF("Upload complete: %zu bytes written to flash", total);

        upload_active = false;

        /* Отправляем ответ клиенту */
        static const char ok_body[] = "{\"status\":\"ok\"}\n";
        response_ctx->body      = ok_body;
        response_ctx->body_len  = sizeof(ok_body) - 1;
        response_ctx->final_chunk = true;

        int rc = boot_set_pending(BOOT_UPGRADE_TEST);
        if (rc != 0) {
            printk("boot_set_pending failed: %d\n", rc);
            return rc;
        }
    }

    return 0;
}

/* ───────── Регистрация ресурса ───────── */
static struct http_resource_detail_dynamic upload_resource_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_POST),
    },
    .cb = upload_handler,
    .user_data = NULL,
};

HTTP_RESOURCE_DEFINE(upload_resource,
                     upload_service,
                     "/upload",
                     &upload_resource_detail);

/* ───────── Linker section (обязательно!) ───────── */
/*
 * В файле sections-rom.ld добавьте:
 *   ITERABLE_SECTION_ROM(http_resource_desc_upload_service, Z_LINK_ITERABLE_SUBALIGN)
 *
 * В CMakeLists.txt:
 *   zephyr_linker_sources(SECTIONS sections-rom.ld)
 *   zephyr_linker_section(NAME http_resource_desc_upload_service
 *                         KVMA RAM_REGION GROUP RODATA_REGION)
 */
