# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

#set(BOARD cedar_switch_3in4out_power_rev3/stm32u585xx)
#set(DTC_OVERLAY_FILE /Volumes/LocalData/Programming/CLionProjects/cedar_switch_3i3o_power/boards/cedar_switch_3in4out_power_rev3.overlay)
#add_compile_definitions(CHIP_PROJECT_CONFIG_INCLUDE=</Volumes/LocalData/Programming/CLionProjects/cedar_switch_3i3o_power/src/CHIPProjectConfig.h>)


#list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

#list(APPEND CHIP_GN_ARGS "chip_skip_app_specific_generated_header_includes=true")
#list(APPEND CHIP_GN_ARGS "chip_device_config_dynamic_endpoint_count=8")

# 2. Добавляем флаги компиляции для твоего приложения (как в твоем примере для ESP)
#add_compile_definitions(CHIP_CONFIG_SKIP_APP_SPECIFIC_GENERATED_HEADER_INCLUDES=1)

# Matter (connectedhomeip) берём из west-воркспейса: <workspace>/modules/lib/matter.
# Корень воркспейса вычисляется из ZEPHYR_BASE (<workspace>/zephyr), без жёстких путей.
# Переопределить можно через -DCHIP_ROOT=<path> или переменную окружения CHIP_ROOT.
# Должно выполняться до find_package(Zephyr), иначе chip-module не подхватится.
if(NOT DEFINED CHIP_ROOT)
    if(DEFINED ENV{CHIP_ROOT})
        set(CHIP_ROOT $ENV{CHIP_ROOT})
    elseif(DEFINED ENV{ZEPHYR_BASE})
        get_filename_component(WEST_TOPDIR $ENV{ZEPHYR_BASE} DIRECTORY)
        set(CHIP_ROOT ${WEST_TOPDIR}/modules/lib/matter)
    else()
        message(FATAL_ERROR "Не задан ни CHIP_ROOT, ни ZEPHYR_BASE. Собирайте через `west build`.")
    endif()
endif()
get_filename_component(CHIP_ROOT "${CHIP_ROOT}" ABSOLUTE)
if(NOT EXISTS ${CHIP_ROOT}/config/zephyr/chip-module/zephyr/module.yml)
    message(FATAL_ERROR "Matter не найден в CHIP_ROOT=${CHIP_ROOT}. Выполните `west update`.")
endif()

list(APPEND ZEPHYR_EXTRA_MODULES ${CHIP_ROOT}/config/zephyr/chip-module)

# Модули приложения внутри репозитория (modules/<name>, у каждого zephyr/module.yml).
# Zephyr сам находит только модули из west-манифеста, поэтому регистрируем явно.
# Должно выполняться до find_package(Zephyr).
list(APPEND ZEPHYR_EXTRA_MODULES
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/settings-registry # Generic per-key settings store over Zephyr Settings (CONFIG_SETTINGS_REGISTRY)
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/job-manager       # Lifecycle and idempotency for long-running web API operations (CONFIG_JOB_MANAGER)
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/device-config-store # Durable, transactional configuration with committed/pending generations (CONFIG_DEVICE_CONFIG_STORE)
)

# CHIP (src/platform/Zephyr/CHIPDevicePlatformConfig.h) делает #include "autoconf.h",
# а новый Zephyr генерирует его как generated/zephyr/autoconf.h. Отдаём этот каталог
# только GN-сборке CHIP (MATTER_CFLAGS наследуется chip-module через add_subdirectory),
# чтобы не включать глобальный CONFIG_LEGACY_GENERATED_INCLUDE_PATH.
list(APPEND MATTER_CFLAGS "-isystem${CMAKE_BINARY_DIR}/zephyr/include/generated/zephyr")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(cedar_switch_3in4out_power)

include(${CHIP_ROOT}/src/app/chip_data_model.cmake)

# target for build frondend files
set(FRONTEND_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/web/frontend)
find_program(NPM_EXECUTABLE npm)
if(NPM_EXECUTABLE)
    add_custom_target(frontend_build
            COMMAND ${NPM_EXECUTABLE} install
            COMMAND ${NPM_EXECUTABLE} run build
            WORKING_DIRECTORY ${FRONTEND_DIR}
            COMMENT "Run npm install and npm run build for frontend"
            USES_TERMINAL
    )
else()
    message(WARNING "npm not found: target frontend_build is unavailable")
endif()

zephyr_include_directories(
        src
#        src/matter
#        src/app-firmware-updater/*.c
)

# CHIP-заголовки, подключаемые из src/matter/*.cpp, делают #include "autoconf.h"
# (см. MATTER_CFLAGS выше — там то же самое для GN-сборки libCHIP). Только для app.
target_include_directories(app SYSTEM PRIVATE ${ZEPHYR_BINARY_DIR}/include/generated/zephyr)

# add al files by mask
file(GLOB app_sources
        src/lib-init/*.c
        src/config/*.c
        src/web/REST_endpoints/*.c
        src/web/REST_endpoints/*.cpp
    )

target_sources(app PRIVATE
        src/main.c
        src/test_functions.c # to delete
        src/littlefs/filesystem_helpers.c
        src/zbus_topics.c
        src/msgq_topiks.c
        src/web/http_common.c
#        src/web/REST_endpoints/settings_status.c
#        src/web/REST_endpoints/default_relay_state.c
#        src/web/REST_endpoints/mqtt_settings.c
#        src/web/REST_endpoints/relays_state.c
#        src/web/REST_endpoints/system_reboot.c
#        src/web/REST_endpoints/webui_delete.c
#        src/web/REST_endpoints/webui_mkdir.c
#        src/web/REST_endpoints/webui_upload.c
#        src/web/REST_endpoints/login.c
#        src/web/REST_endpoints/web_user_mgmt.c
#        src/web/REST_endpoints/fw_update.c
        src/web/static_resources.c
        src/web/http_server_init.c
        src/mqtt/ha_mqtt.c
        src/mqtt/ha_mqtt_callback.c
        src/mqtt/ha_mqtt_helpers.c
        src/mqtt/ha_mqtt_adapter.c
        src/mqtt/ha_task.c
        src/littlefs/littlefs_mount.c
        src/io/io.c
        src/energy_monitoring/energy_monitoring.c
        src/energy_monitoring/HLW8032.c
        src/web/authentication.c
        src/diagnostic/diag_report.c
        src/plugin_wifi/wifi.c
        src/plugin_wifi/wifi_shell.c
        src/app-firmware-updater/firmware_updater.c
        src/app-firmware-updater/firmware_updater_shell.c
        src/helpers/memory.c
        src/events/events.c
        src/matter/matter_init.cpp
        src/matter/matter_event_loop.cpp
        src/matter/shell.cpp
        src/matter/crypto_bench.cpp
        src/matter/clusters/IdentifyCluster.cpp
        src/matter/clusters/NetworkCommissioningCluster.cpp
#        src/web/REST_endpoints/matter_control.cpp
#        src/lib-init/net_init.c
        ${app_sources}
#        /Users/kiro/zephyrproject/modules/hal/stm32/stm32cube/stm32u5xx/drivers/src/stm32u5xx_hal_icache.c
)

#chip_configure_data_model(app
#        ZAP_FILE ${CHIP_ROOT}/examples/chef/devices/rootnode_onoffpluginunit_Wtf8ss5EBY.zap
#)

chip_configure_data_model(app
        ZAP_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/matter/cedar_switch_4relay.zap
        IDL ${CMAKE_CURRENT_SOURCE_DIR}/src/matter/cedar_switch_4relay.matter
        ZCL_PATH ${CHIP_ROOT}/src/app/zap-templates/zcl/zcl.json
)
# Include dir with generated files
set(gen_dir ${ZEPHYR_BINARY_DIR}/include/generated)

# ------------------ Generate files/resources for web ------------------
file(GLOB_RECURSE WEB_DIST_FILES
        RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
        src/web/frontend/dist/*
)
#define name of HTTP service for static files
set(HTTP_STATIC_SERVICE http_service)
set(WEB_RESOURCES_INC ${gen_dir}/web_resources.inc)
file(WRITE ${WEB_RESOURCES_INC} "/* Auto-generated by CMake. Do not edit. */\n")

foreach(rel_file IN LISTS WEB_DIST_FILES)
    set(src_file ${CMAKE_CURRENT_SOURCE_DIR}/${rel_file})
    if(IS_DIRECTORY "${src_file}")
        continue()
    endif()

    # Safe C identifier suffix from path
    string(REPLACE "/" "_" symbol_suffix "${rel_file}")
    string(REPLACE "." "_" symbol_suffix "${symbol_suffix}")
    string(MAKE_C_IDENTIFIER "${symbol_suffix}" symbol_suffix)

    # URL path served by HTTP server (strip dist prefix)
    string(REGEX REPLACE "^src/web/frontend/dist" "" url_path "${rel_file}")
    if(url_path STREQUAL "")
        continue()
    endif()

    # Content-Type by extension
    set(content_type "application/octet-stream")
    if(rel_file MATCHES "\\.html$")
        set(content_type "text/html")
    elseif(rel_file MATCHES "\\.css$")
        set(content_type "text/css")
    elseif(rel_file MATCHES "\\.js$")
        set(content_type "application/javascript")
    elseif(rel_file MATCHES "\\.json$")
        set(content_type "application/json")
    elseif(rel_file MATCHES "\\.svg$")
        set(content_type "image/svg+xml")
    elseif(rel_file MATCHES "\\.png$")
        set(content_type "image/png")
    elseif(rel_file MATCHES "\\.jpe?g$")
        set(content_type "image/jpeg")
    elseif(rel_file MATCHES "\\.woff2$")
        set(content_type "font/woff2")
    elseif(rel_file MATCHES "\\.woff$")
        set(content_type "font/woff")
    endif()

    # Binary include file per resource
    set(bin_inc ${gen_dir}/${symbol_suffix}.inc)
    generate_inc_file_for_target(app "${rel_file}" "${bin_inc}")

    file(APPEND ${WEB_RESOURCES_INC}
"static const uint8_t web_data_${symbol_suffix}[] = {\n#include \"${symbol_suffix}.inc\"\n};\n"
    )
    file(APPEND ${WEB_RESOURCES_INC}
"static struct http_resource_detail_static web_detail_${symbol_suffix} = {\n"
"    .common = {\n"
"        .type = HTTP_RESOURCE_TYPE_STATIC,\n"
"        .bitmask_of_supported_http_methods = BIT(HTTP_GET),\n"
"        .content_type = \"${content_type}\",\n"
"    },\n"
"    .static_data = web_data_${symbol_suffix},\n"
"    .static_data_len = sizeof(web_data_${symbol_suffix}),\n"
"};\n"
    )
    file(APPEND ${WEB_RESOURCES_INC}
"HTTP_RESOURCE_DEFINE(web_res_${symbol_suffix}, ${HTTP_STATIC_SERVICE}, \"${url_path}\", &web_detail_${symbol_suffix});\n\n"
    )

    if(url_path STREQUAL "/index.html")
        file(APPEND ${WEB_RESOURCES_INC}
"HTTP_RESOURCE_DEFINE(web_res_root_${symbol_suffix}, ${HTTP_STATIC_SERVICE}, \"/\", &web_detail_${symbol_suffix});\n\n"
        )
    endif()
endforeach()
# ------------------ Generate files/resources for web END --------------

generate_inc_file_for_target(
        app
        src/app-firmware-updater/globalsign_r1.der
        ${gen_dir}/globalsign_r1.der.inc
)

zephyr_linker_sources(SECTIONS sections-rom.ld)

# ------------------ PSRAM (OCTOSPI2, окно 0x70000000, регион линкера PSRAM) ------------------
# MCUboot поднимает PSRAM и оставляет её в memory-mapped режиме до прыжка в приложение,
# поэтому .data/.bss/.noinit можно линковать туда штатным zephyr_code_relocate():
# копирование .data и обнуление .bss выполняются в z_prep_c() (arch/common/xip.c, init.c),
# когда окно уже доступно. В SRAM остаются ядро (_kernel, стеки ISR/main/idle, sysworkq),
# arch, драйверы прерываний/таймера/UART (async UART работает через DMA), USB.
if(CONFIG_CODE_DATA_RELOCATION)
    foreach(lib
            app                     # приложение + Matter-исходники из chip_data_model
            subsys__net__ip         # пулы net_pkt/net_buf, очередь net_mgmt, стеки RX/TX
            subsys__net             # сокеты, TLS-контексты
            subsys__net__conn_mgr
            subsys__net__lib__dns
            subsys__net__lib__http
            subsys__fs              # littlefs: кеши файлов, read/prog-буферы
            subsys__zbus)
        if(TARGET ${lib})
            zephyr_code_relocate(LIBRARY ${lib} LOCATION PSRAM_DATA_BSS_NOINIT NOKEEP)
        endif()
    endforeach()
    zephyr_code_relocate(FILES
            ${ZEPHYR_BASE}/subsys/logging/log_core.c                # буфер deferred-логов
            ${ZEPHYR_BASE}/lib/libc/common/source/stdlib/malloc.c   # арена malloc (Matter, mbedTLS)
            ${ZEPHYR_BASE}/kernel/mempool.c                         # куча k_malloc
            ${ZEPHYR_BASE}/drivers/flash/flash_shell.c
            LOCATION PSRAM_DATA_BSS_NOINIT NOKEEP)
endif()
# .bss/.noinit библиотек, объектники которых gen_relocate_app.py не находит или путает:
# libCHIP.a (GN), libtfpsacrypto.a, settings_registry, esp_hosted_mcu (+_core, SPI без DMA).
zephyr_linker_sources(SECTIONS src/helpers/psram_sections.ld)

zephyr_linker_section(NAME http_resource_desc_http_service
        KVMA RAM_REGION GROUP RODATA_REGION)

zephyr_linker_section(NAME http_resource_desc_http_api_service
        KVMA RAM_REGION GROUP RODATA_REGION)

zephyr_linker_section(NAME http_resource_desc_upload_service
        KVMA RAM_REGION GROUP RODATA_REGION)

# Линкуем Matter (CHIP) к твоему приложению
target_link_libraries(app PUBLIC chip)

# Перенос драйвера прерываний и работы с UART в оперативную память (SRAM)
#zephyr_code_relocate(FILES ${ZEPHYR_BASE}/drivers/serial/uart_stm32.c LOCATION RAM)
#zephyr_code_relocate(FILES ${ZEPHYR_BASE}/subsys/shell/shell.c LOCATION RAM)

# XIP write/erase support.
# Relocate the OSPI flash driver and STM32 HAL OSPI to SRAM so they can
# execute when the QSPI controller is in indirect (non-memory-mapped) mode.
# The ARM MPU automatically grants RX permissions to relocated text
# (CONFIG_CODE_DATA_RELOCATION_SRAM in arm_core_mpu.c).
#
# IMPORTANT: ISR handlers that reside in QSPI will hard-fault if they fire
# during a write/erase operation (irq_lock() is held for the duration).
# For erase operations (up to ~400 ms per sector), stop all interrupt-driven
# peripherals (networking, USB, etc.) before calling flash_erase().
#zephyr_code_relocate(
#    FILES ${ZEPHYR_BASE}/drivers/flash/flash_stm32_ospi.c
#    LOCATION RAM)
#zephyr_code_relocate(
#    FILES ${ZEPHYR_HAL_STM32_MODULE_DIR}/stm32cube/stm32u5xx/drivers/src/stm32u5xx_hal_ospi.c
#    LOCATION RAM)
#
#zephyr_code_relocate(
#        FILES ${ZEPHYR_HAL_STM32_MODULE_DIR}/stm32cube/stm32u5xx/drivers/src/stm32u5xx_hal_ospi.c
#        LOCATION RAM)
