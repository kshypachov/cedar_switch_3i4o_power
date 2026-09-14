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
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/api-validation  # Shared error codes, rejections and strict parsers for the web API (CONFIG_API_VALIDATION)
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/network-manager  # Validate/journal/apply/confirm transaction for the network configuration (CONFIG_NETWORK_MANAGER)
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/web-auth         # Local administrator: setup, sessions, CSRF, guessing limit (CONFIG_WEB_AUTH)
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/web-api          # API v1 request pipeline, strict JSON, HTTP server adapter (CONFIG_WEB_API)
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/web-assets       # Browser application from flash: MIME, ETag, gzip, cache (CONFIG_WEB_ASSETS)
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/matter-service   # Matter snapshot and commissioning window requests for the web API (CONFIG_MATTER_SERVICE)
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/log-store        # Bounded STM32 and ESP32 log rings, cursor, filters, export (CONFIG_LOG_STORE)
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/zephyr-log-source # Zephyr log backend into log-store (CONFIG_ZEPHYR_LOG_SOURCE)
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/esp32-log-source # C6 UART bytes into log lines (CONFIG_ESP32_LOG_SOURCE)
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/coprocessor-manager # Owner of USART3 and EN/BOOT, UART modes and exclusion (CONFIG_COPROCESSOR_MANAGER)
)

# CHIP (src/platform/Zephyr/CHIPDevicePlatformConfig.h) делает #include "autoconf.h",
# а новый Zephyr генерирует его как generated/zephyr/autoconf.h. Отдаём этот каталог
# только GN-сборке CHIP (MATTER_CFLAGS наследуется chip-module через add_subdirectory),
# чтобы не включать глобальный CONFIG_LEGACY_GENERATED_INCLUDE_PATH.
list(APPEND MATTER_CFLAGS "-isystem${CMAKE_BINARY_DIR}/zephyr/include/generated/zephyr")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(cedar_switch_3in4out_power)

include(${CHIP_ROOT}/src/app/chip_data_model.cmake)


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
    )

target_sources(app PRIVATE
        src/main.c
        src/test_functions.c # to delete
        src/littlefs/filesystem_helpers.c
        src/zbus_topics.c
        src/msgq_topiks.c
        src/web/web_server.c
        src/web/api/v1/router.c
        src/web/api/v1/auth.c
        src/web/api/v1/system.c
        src/web/api/v1/matter.c
        src/web/api/v1/network.c
        src/web/api/v1/logs.c
        src/web/api/v1/coprocessor.c
        src/mqtt/ha_mqtt.c
        src/mqtt/ha_mqtt_callback.c
        src/mqtt/ha_mqtt_helpers.c
        src/mqtt/ha_mqtt_adapter.c
        src/mqtt/ha_task.c
        src/littlefs/littlefs_mount.c
        src/io/io.c
        src/energy_monitoring/energy_monitoring.c
        src/energy_monitoring/HLW8032.c
        src/mqtt/mqtt_settings_keys.c
        src/diagnostic/diag_report.c
        src/diagnostic/storage_bench.c
        src/plugin_wifi/wifi.c
        src/plugin_wifi/wifi_shell.c
        src/app-firmware-updater/firmware_updater.c
        src/app-firmware-updater/firmware_updater_shell.c
        src/helpers/memory.c
        src/events/events.c
        src/matter/matter_init.cpp
        src/matter/matter_service_chip.cpp
        src/matter/matter_event_loop.cpp
        src/matter/shell.cpp
        src/matter/crypto_bench.cpp
        src/matter/clusters/IdentifyCluster.cpp
        src/matter/clusters/NetworkCommissioningCluster.cpp
#        src/lib-init/net_init.c
        ${app_sources}
#        /Users/kiro/zephyrproject/modules/hal/stm32/stm32cube/stm32u5xx/drivers/src/stm32u5xx_hal_icache.c
)

# Network service (P4): adapter, store, recovery rule and worker.
add_subdirectory(src/services/network)
# Coprocessor service (P5): USART3 owner's platform, CDC passthrough, ESP32 logs.
add_subdirectory(src/services/coprocessor)

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

# ------------------ Browser application and revision ------------------
# The frontend is built by npm (src/web/frontend, `npm ci && npm run build`)
# and embedded here: tools/web-assets/gen_web_assets.py turns dist/ into a C
# table at build time - the file list is read when the command runs, not when
# CMake configures - and fails the build over the plan's 512 KiB gzip budget.
# A firmware build does not run npm itself: without a built frontend it stops
# and says how to get one, or embeds a placeholder page when asked to.
#
# Both knobs are read with zephyr_get(), so they can be given as -D, as an
# environment variable, or through sysbuild as -D<image>_<name> - sysbuild copies
# its cache into the image's with the prefix intact, and plain option() would
# not see it:
#   APP_WEB_UI_PLACEHOLDER  embed a placeholder page instead of the frontend
#   APP_WEB_DIST            a built frontend somewhere other than the default
zephyr_get(APP_WEB_UI_PLACEHOLDER SYSBUILD LOCAL)
zephyr_get(APP_WEB_DIST SYSBUILD LOCAL)
if(NOT APP_WEB_DIST)
    set(APP_WEB_DIST ${CMAKE_CURRENT_SOURCE_DIR}/src/web/frontend/dist)
endif()
if(APP_WEB_UI_PLACEHOLDER)
    set(web_dist ${CMAKE_CURRENT_SOURCE_DIR}/tools/web-assets/placeholder)
elseif(EXISTS ${APP_WEB_DIST}/index.html)
    set(web_dist ${APP_WEB_DIST})
else()
    message(FATAL_ERROR
        "No built frontend in ${APP_WEB_DIST}.\n"
        "Build it:  cd src/web/frontend && npm ci && npm run build\n"
        "or configure with -DAPP_WEB_UI_PLACEHOLDER=ON for firmware without the interface.")
endif()

set(app_gen_dir ${CMAKE_BINARY_DIR}/generated/app)
set(web_assets_c ${app_gen_dir}/web_assets_data.c)
set(web_assets_manifest ${app_gen_dir}/web_assets_manifest.json)
set(web_assets_gen ${CMAKE_CURRENT_SOURCE_DIR}/tools/web-assets/gen_web_assets.py)
set(web_assets_depends ${web_assets_gen} ${web_dist}/index.html)
if(EXISTS ${web_dist}/.cedar-build.json)
    list(APPEND web_assets_depends ${web_dist}/.cedar-build.json)
endif()
add_custom_command(
    OUTPUT ${web_assets_c} ${web_assets_manifest}
    COMMAND ${PYTHON_EXECUTABLE} ${web_assets_gen}
            --dist ${web_dist}
            --out-c ${web_assets_c}
            --out-manifest ${web_assets_manifest}
            --budget-kib 512
    DEPENDS ${web_assets_depends}
    COMMENT "Embedding the browser application from ${web_dist}"
    VERBATIM
)
target_sources(app PRIVATE ${web_assets_c})

set(cedar_version_h ${app_gen_dir}/cedar_version.h)
add_custom_target(cedar_version
    COMMAND ${CMAKE_COMMAND} -DAPP_DIR=${CMAKE_CURRENT_SOURCE_DIR} -DOUT=${cedar_version_h}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/cedar_version.cmake
    BYPRODUCTS ${cedar_version_h}
    COMMENT "Recording the application revision"
)
add_dependencies(app cedar_version)
target_include_directories(app PRIVATE ${app_gen_dir})
# ------------------ Browser application and revision END ------------------

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
