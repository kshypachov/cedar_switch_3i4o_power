# P0, шаг 1 — чистый sysbuild вне CLion

Дата: 2026-09-12. Этот образ прошит на плату и на нём измерялись старты и smoke.

## Как собрано

```sh
cd /Volumes/Programming/Zephyr/zephyr_latest
export ZEPHYR_SDK_INSTALL_DIR=$HOME/zephyr-sdk-1.0.1 ZEPHYR_TOOLCHAIN_VARIANT=zephyr
.venv/bin/west build -p always \
  -b cedar_switch_3in4out_power_rev3/stm32u585xx/ext_flash_app \
  --sysbuild -d <scratchpad>/p0/build cedar_switch_3in4out_power
```

Каталог сборки — scratchpad сессии, не `build/` репозитория: CLion его не
видит и не перегенерирует. `-p always` удаляет каталог целиком; GN-сборка
Matter лежит внутри него (`modules/connectedhomeip/args.gn`), так что Matter тоже
собран заново. Настенное время 18:54:52–18:56:32 UTC, 100 с; код возврата 0.
Полный лог — `logs/build.log`, конфиги — `logs/app.config`, `logs/mcuboot.config`.

## Что именно собрано

| Что | Ревизия | Состояние дерева |
|---|---|---|
| приложение | `ec880df18c48` | изменён 1 файл (`NEXT-SESSION.md`, в сборку не входит) |
| zephyr | `b6a5e6e8aa90` (`v4.4.0-12983`) | **87 изменённых/неотслеживаемых путей**, sha256 от `git diff HEAD` начинается с `49963d7f68c17658`; из них наш патч W5500 MMB (2 файла, +11/−2), остальное — чужая работа в общем дереве |
| плата `cedar_board_zephyr` | `07968194b5f9` | **изменены `board.cmake` и `…-common.dts`**: узел `esp-hosted-mcu@1`, полярность `wifi_reset`/`wifi_boot` — в репозитории платы не закоммичено |
| matter (`connectedhomeip`) | `250a9e6c50ee` | изменены 4 файла (`chip-module/CMakeLists.txt`, TCP/Inet) |
| mcuboot | `7ad67106c325` (`v2.4.0-118`) | чистое |
| esp-serial-flasher | `ca7edfcef903` (`v2.0.0`) | чистое |

Образ воспроизводим только вместе с рабочими деревьями zephyr, платы и matter —
ревизий недостаточно. Для платы это прямо касается решения «CI клонирует
`cedar_board_zephyr`»: клон получит определение без узла ESP-Hosted.

| Артефакт | sha256 |
|---|---|
| `cedar_switch_3in4out_power/zephyr/zephyr.signed.bin` (1 136 480 байт) | `a59bdebd47def93d4b03cf8938e62f373ade7543e30896d54aaf3938dfa9f67d` |
| `mcuboot/zephyr/zephyr.hex` | `505fbcffb264e18bb084413d20f03b70297ff767f9391a0db92eb35f7848557e` |
| `cedar_switch_3in4out_power/zephyr/zephyr.elf` | `c4c2f18453746de073137bee2e1f1ab57f1a4de64910395d7bc05cd8aed4750b` |

MCUboot во внутренней флеш **не перешивался**: на плате остался ранее прошитый
загрузчик, `zephyr.hex` из этой сборки записан только для сверки.

## Память

| Регион | Приложение | MCUboot |
|---|---|---|
| FLASH | 1 136 440 Б из 4 191 144 (27,12 %) | 85 872 Б из 128 КБ (65,52 %) |
| RAM (SRAM) | 54 216 Б из 768 КБ (6,89 %) | 44 736 Б (5,69 %) |
| PSRAM | 1 746 984 Б из 8 МБ (20,83 %) | 0 |

Цель раздела 9 — запас SRAM ≥ 25 % — по статическому размеру выполняется с
большим запасом (93 % свободно). Стеки на плате — в отчёте smoke.

Кто занимает SRAM (.data/.bss/noinit по map, 50 648 Б): `libzephyr` 13 800,
`libkernel` 9 597, USB device_next 8 766 + UDC 4 396, zvfs 3 020,
`drivers__ethernet` 2 936, POSIX 1 724, serial 1 592, input 1 440; остальное
меньше 600 Б каждое.

## Модули P1 в linker map

Обязательство раздела 9: проверить, куда легли .data/.bss библиотек,
выделенных из `app`.

**Сейчас — никуда.** Приложение не вызывает ни одну функцию `job_manager`,
`device_config_store`, `api_validation`, `network_manager`, и `--gc-sections`
выбрасывает их целиком: символов в ELF нет, в map их секции — только в
«Discarded input sections». В образе остались лишь записи регистрации
логгера у `device_config_store` и `network_manager`: `log_const` по 8 Б во
флеш и `log_dynamic` по 4 Б в SRAM (`0x20002370`, `0x200024cc`).

**Когда их начнут вызывать (P2), они лягут в SRAM, и это не задумано.**
Правило relocation в `CMakeLists.txt` перечисляет библиотеки явно
(`app`, `subsys__net__ip`, …) — новых там нет. Размеры из выброшенных секций:

| Библиотека | .bss/.data | Крупнейшее |
|---|---:|---|
| `job_manager` | 44 424 Б | `compacts` 40 960, `records` 3 456 |
| `network_manager` | 3 164 Б | `nm` 3 160 |
| `device_config_store` | 836 Б | `store` |
| `api_validation` | 5 Б | |

Одна `job_manager` подняла бы SRAM с 6,9 % до ~12,5 %. В цель ≥ 25 % запаса
это укладывается, но задумано было держать данные приложения в PSRAM.

Добавить их в `foreach(lib …)` скорее всего **не сработает молча**: все четыре
исходника лежат в каталоге `lib/`, объектники — в `CMakeFiles/<имя>.dir/`, а
`gen_relocate_app.py` сопоставляет по вхождению имени родительского каталога
исходника в путь объектника. `lib` не входит в `job_manager.dir` — ровно так
уже пропускается `settings_registry`, которая поэтому идёт через
`src/helpers/psram_sections.ld`. Решение при интеграции в P2: дописать
библиотеки в этот фрагмент (он переносит только .bss/.noinit; .data у модулей
по 4 Б) и проверить размещение по map, а не по «сборка прошла».

## Предупреждения

33 предупреждения вне Matter. Kconfig: значения USB_DEVICE_*, TIMESLICE_*,
MCUBOOT_UPDATE_FOOTER_SIZE, FS_LITTLEFS_HEAP_PER_ALLOC_OVERHEAD_SIZE не
применились, устаревший MBEDTLS_SSL_MAX_CONTENT_LEN. Код приложения: мёртвые
функции и переменные (`write_relays_once` в `io.c`, `ipv4_addr_add_handler`,
`warmup_settings_cb`, `net_diag_thread_fn` и др.), 4 проигнорированных
`CHIP_ERROR` в `matter_control.cpp`, устаревший макрос в
`littlefs_mount.c` и `firmware_updater.c`. Одно в драйвере
`esp_hosted_mcu.c` (неиспользуемая `esp_hosted_mcu_ap_authmode`).
Новых ошибок нет; предупреждения не разбирались — это не цель P0.

`write_relays_once` оказался не косметикой: см. находку о состоянии реле по
умолчанию в отчёте стартов.
