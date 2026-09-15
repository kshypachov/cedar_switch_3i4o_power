# P6 — факты, прочитанные в коде до дизайна (2026-09-14)

Всё ниже — чтение исходников в дереве workspace на `4e5b8a4`; измерения на плате отмечены отдельно.

## ESP-Hosted: почему транспорт не переживает сброс C6

`zephyr/drivers/misc/esp_hosted_mcu/esp_hosted_mcu.c` и `.../esp_hosted_mcu_spi.c`, `zephyr/drivers/wifi/esp_hosted_mcu/esp_hosted_mcu.c`.

| Что | Где | Следствие для P6 |
|---|---|---|
| Ядро: `esp_hosted_mcu_core_init` (POST_KERNEL) создаёт поток TX, вызывает `transport->init`, создаёт поток событий, затем `check_fw_version` | `esp_hosted_mcu.c:862` | Всё выполняется один раз; API повторной инициализации нет |
| SPI-транспорт: `esp_hosted_mcu_spi_init` импульсом PA8 сбрасывает C6 (`GPIO_OUTPUT_ACTIVE` → 0 → 1, `RESET_PULSE_MS`, `SPI_RESET_SETTLE_MS`), настраивает handshake/data-ready и их прерывания, семафоры, mutex | `esp_hosted_mcu_spi.c:127–175` | Сброс EN есть только здесь |
| Версия прошивки: priv event (тег firmware version) записывается только при `fw_version == 0`; `check_fw_version` ждёт boot event, затем RPC `GetCoprocessorFwVersion` | `esp_hosted_mcu.c:380–410`, `807–858` | После перепрошивки версия не обновится, пока `fw_version` не обнулить |
| Wi-Fi: `esp_hosted_mcu_wifi_init` (NET_DEVICE, `WIFI_INIT_PRIORITY`) — регистрирует обработчик событий, читает MAC (`IfaceMacAddrSetGet`, или `GetMACAddress` у «legacy» при `fw_version == 0`), `WifiInit`, `SetWifiMode(STA)`, `WifiStart`. На плате B падает на `WifiInit` (C6 пуст) и возвращает `-EIO` — сетевое устройство Wi-Fi остаётся не готовым | `drivers/wifi/.../esp_hosted_mcu.c:947–1040` | Патч должен повторять и эту последовательность, и `device_is_ready` Wi-Fi после неё в Zephyr не меняется сам |
| Состояние приёма: `rx_stride_aligned` (из priv event SDIO), stash SPI-транспорта | `esp_hosted_mcu.c:414–433`, `esp_hosted_mcu_spi.c:380–392` | При переинициализации сбросить stash/выравнивание |

### Предел решения №2 (а) на плате B — Wi-Fi не оживает без перезагрузки STM32

- `do_device_init()` записывает ошибку init в `dev->state->init_res` и ставит `initialized=true`;
  `device_is_ready()` = `initialized && init_res == 0`; `device_init()` для уже
  инициализированного устройства возвращает `-EALREADY` (`zephyr/kernel/device.c:18–70,186`).
- `net_if` при старте пропускает интерфейс, чьё устройство не готово (`init_iface`: «device not
  ready», `api->init` не вызывается), и `net_if_up` для него — `-ENXIO`
  (`subsys/net/ip/net_if.c:447–460`, `6632–6640`).
- На плате B `esp_hosted_mcu_wifi_init` вернул `-EIO` (`coprocessor WifiInit failed`, P4/P5) —
  **сетевое устройство Wi-Fi останется не готовым до перезагрузки STM32, что бы ни сделал патч
  транспорта**, если не править состояние устройства в обход ядра.
- Что патч (а) даёт без перезагрузки: заново опросить C6 по ESP-Hosted (сбросить stash/выравнивание
  приёма и `fw_version`, запросить версию RPC, повторить `WifiInit`/`SetWifiMode`/`WifiStart` для
  устройства, которое уже было готово). Для обычного обновления (Wi-Fi работал) — полный health
  check; для первой прошивки пустого чипа — подтверждение «C6 жив, ESP-Hosted отвечает, версия X»,
  а Wi-Fi-интерфейс поднимется после перезагрузки STM32. Это остаток, записать в отчёт и владельцу.

## esp-serial-flasher: повторный init после deinit

- `esp_loader_deinit()` вызывает `port->ops->deinit` (выключает RX/TX IRQ USART3, EN и BOOT →
  `GPIO_DISCONNECTED`) и обнуляет `esp_loader_t` (`src/esp_loader.c`).
- `zephyr_port_init()` проверяет устройства, EN/BOOT → `GPIO_OUTPUT_INACTIVE`, `configure_tty()`:
  `tty_init()` ставит callback `tty_uart_isr` на USART3 и буферы 512/512 — повторяемо
  (`port/zephyr_port.c:58–110`, `subsys/console/tty.c`). Скорость USART3 порт не трогает, кроме
  `change_transmission_rate` (`uart_configure`).
- DT-инстанс: `DEVICE_DT_INST_DEFINE(... esp_loader_dev_init, POST_KERNEL,
  CONFIG_KERNEL_INIT_PRIORITY_DEVICE)` — при старте один раз `esp_loader_init_serial`.
- Следствие для адаптера: после `deinit` линии EN/BOOT висят — сразу вернуть их менеджеру
  (`GPIO_OUTPUT_INACTIVE`) и восстановить конфигурацию USART3 перед консолью.

## Крипто в сборке приложения

- `CONFIG_PSA_WANT_ALG_SHA_256=y`; **`PSA_WANT_ALG_MD5` не задан** — MD5 записи таблицы разделов
  считать своей реализацией (RFC 1321, проверка на векторах) или не проверять; запись во flash C6
  проверяет MD5 сам ROM/stub (`flash_finish`).

## Базовая проверка worktree (2026-09-14)

- Сборка `cedar_p6` sysbuild: FLASH 1 357 072 Б (32,4 %), RAM 84 600 Б (10,8 %), PSRAM 2 479 656 Б (29,6 %).
- sim: 623 из 623 в 15 конфигурациях (97 с).

## Кто управляет PA8 (EN) и PC0 (BOOT)

| Владелец | Узел / код | Полярность |
|---|---|---|
| Плата: `gpio-outputs` (`gpio-leds`) `wifi_reset`, `wifi_boot` | DTS платы `-common.dts:25–45` | ACTIVE_LOW |
| ESP-Hosted SPI (`reset-gpios`) | `esp-hosted-mcu@1`, `-common.dts:281` | **ACTIVE_HIGH** (`gpioa 8`) |
| Сервис копроцессора P5 (`op_reset`) | `src/services/coprocessor/coprocessor_service.c:349–370` через `wifi_reset`/`wifi_boot` | ACTIVE_LOW |
| esp-serial-flasher (`espressif,esp-loader`) | в приложении **узла пока нет** (только в `tests/esp_loader_integration` и `tests/esp_loader_flash_probe`) | ACTIVE_LOW |

После добавления узла `espressif,esp-loader` в overlay приложения его `DEVICE_DT_INST_DEFINE(... POST_KERNEL ...)` при старте выставит EN/BOOT неактивными и поставит свой callback на USART3 (факт 1 задачи); сервис копроцессора позже ставит свой callback консоли — порядок проверить на плате.

## web-api: тело запроса

- Один буфер тела на контекст: `uint8_t body[CONFIG_WEB_API_JSON_BODY_MAX]` (`web_api.h:276`); правило 8 — `Content-Type` не `application/json` → 415 (`web_api.c:936`), больше лимита → 413.
- Флаги маршрута: `PUBLIC`, `CSRF`, `IDEMPOTENT`, `ORIGIN`, `SETUP_TOKEN`, `BODY_REQUIRED` (`web_api.h:205–215`) — вида тела «октеты» нет.
- `peer` берётся `zsock_getpeername(client->fd)` (`web_api_http.c:141`); локальный адрес соединения тем же способом даёт `zsock_getsockname` — отсюда интерфейс приёма для `ethernet_required`.

## job-manager

- Виды `upload_chunk`, `firmware_verify`, `firmware_delete`, `coprocessor_update` уже есть (`job_manager.h:76`); `job_set_phase` сбрасывает прогресс; `job_cancel` → `-EPERM` для не отменяемых; `WAITING_CONFIRMATION` предусмотрен и для обновления копроцессора.
- `job_find_by_key` — проверить отказ до создания задачи (как смена пароля).

## Патчи общего дерева

`patches/patches.yml` + `west patch -sm <app> -b patches -l patches/patches.yml -dm zephyr apply`; README ведёт таблицу «что, где, когда убрать». Патч драйвера ESP-Hosted добавляется туда же (решение №2, применяется сразу после проверки).

## Образы CP, разобранные по форматам ESP-IDF 5.5.5 (только чтение `SoC_and_w5500/esp32c6-hosted-cp`)

Форматы: `esp_image_header_t` 24 Б (magic `0xE9`, `segment_count`, `chip_id` u16 на смещении 12,
`hash_appended` на 23), сегмент — `load_addr`, `data_len`; после сегментов выравнивание до 16 с байтом
контрольной суммы (XOR данных сегментов с `0xEF`) последним, затем SHA-256 32 Б при `hash_appended=1`.
`esp_app_desc_t` на `0x20` (magic `0xABCD5432`, `version[32]` на 16, `project_name[32]` на 48, `idf_ver[32]`
на 112); `esp_bootloader_desc_t` на `0x20` (magic byte 80, `version` u32 на 4, `idf_ver[32]` на 8,
`date_time[24]` на 40). Таблица: записи 32 Б (`0x50AA`, type, subtype, offset, size, label[16], flags),
MD5-запись `0xEBEB` + MD5 предыдущих байт на 16..31, конец — `0xFF…`, не больше `0xC00`
(`bootloader_support/include/esp_app_format.h`, `esp_app_desc.h`, `esp_bootloader_desc.h`, `esp_flash_partitions.h`).

| Файл | Размер | Сегменты | Длина по сегментам + SHA | Описание | sha256 |
|---|---:|---:|---|---|---|
| `build/eh_cp_c6_cedar.bin` (Wi-Fi CP) | 1 402 640 | 5 | = файлу, checksum и SHA верны | app `1`, `eh_cp_c6_cedar`, 14:03:44 Sep 6 2026, IDF `b774170f` | `6fcadfd5…6741` |
| `build-ot/eh_cp_c6_cedar.bin` (OT CP) | 803 520 | 5 | = файлу, checksum и SHA верны | app `1`, `eh_cp_c6_cedar`, 14:39:54, IDF `b774170f` | `b39cf8a4…60f2` |
| `build/bootloader/bootloader.bin` | 22 176 | 3 | = файлу, checksum и SHA верны | bootloader_desc v1, IDF `b774170f`, `Sep  6 2026 14:03:49` | `f5564a2f…30d1` |
| `build/partition_table/partition-table.bin` | 3 072 | — | 5 записей, MD5 на 160 = расчётному `b233c05e…`, конец на 192 | nvs 1/2 `0x9000`+`0x4000`, otadata 1/0 `0xd000`+`0x2000`, phy_init 1/1 `0xf000`+`0x1000`, ota_0 0/16 `0x10000`+`0x1c0000`, ota_1 0/17 `0x1d0000`+`0x1c0000`, flags 0 | `73e7f5c6…a57d` |
| `build/ota_data_initial.bin` | 8 192 | — | все `0xFF` | — | — |

OT и Wi-Fi app по заголовкам различаются только содержимым (одинаковые project, version, IDF, chip) —
решение №4 отложено, и валидатор их не различает. `build-ot` содержит только app: merged-файл OT для
отказной фикстуры собирается из OT app и загрузчика/таблицы Wi-Fi-сборки (разметка одна).

### Настоящие merged-файлы для платы (вне репозитория)

Склейка частей `build/` с заполнением `0xFF` (как `esptool merge_bin -f raw`), каждая часть сверена
байт в байт после склейки; файлы — только в scratchpad сессии, в репозиторий не кладутся (решение №5).

| Файл | Из чего | Размер | sha256 |
|---|---|---:|---|
| `wifi-cp-merged.bin` | bootloader + таблица + otadata Wi-Fi-сборки + `build/eh_cp_c6_cedar.bin` | 1 468 176 (`0x166710`, как `idf.py merge-bin` 2026-09-14) | `750cb58e2c3692cae0d8b6abd6d89346765b735316d75d5392e73cae7f82cb77` |
| `ot-cp-merged.bin` | те же bootloader, таблица, otadata + `build-ot/eh_cp_c6_cedar.bin` | 869 056 (`0xd42c0`) | `5bbe0d278996b19ec779d6e4ff2dcd96b83cf1cfc1e6f555bf722a766dac7a16` |

Оба кончаются до `0x1d0000`. OT-файл структурно верен — без профиля совместимости (№4 отложено)
устройство его примет; прошивать его в C6 платы B не нужно и не планируется.

## mock сейчас

- `start_update` отказывает `service_not_ready`, если `coprocessor_state != ready` — **первая прошивка пустого C6 так невозможна**; при решении №1 (Г) обновление должно разрешаться и при `offline`/`failed`.
- `_image_json()` — `raw_app`; при решении «только merged» меняется на новый format.
