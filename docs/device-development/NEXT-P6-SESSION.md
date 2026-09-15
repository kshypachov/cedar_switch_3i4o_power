# Промт: полное выполнение этапа P6 — ESP32 UART updater (плата на J-Link)

```
Выполняем этап P6 — ESP32 UART updater. Корень west workspace —
/Volumes/Programming/Zephyr/zephyr_latest

## ⚠️ Две платы. Работаем ТОЛЬКО с платой B на J-Link

К компьютеру подключены две одинаковые платы cedar_switch_3in4out_power_rev3.

**Плата A — на ST-Link — занята другой сессией (P3, настройка бэкендов settings;
на 2026-09-14 09:09 ещё шли попытки). Её тесты прерывать нельзя. Не трогать никак:**
- не прошивать, не сбрасывать, не открывать её порты, не входить в её веб, не слать ей
  запросы API, chip-tool, ping-флуд, e2e;
- ST-Link V3, серийник 002F002B3233510739363634; порты /dev/cu.usbmodem21103,
  /dev/cu.usbmodem21301, USB CDC 2037394C3543501200550045 (номера портов меняются —
  сверяй по серийнику); сеть 192.168.88.14, MAC 80:34:28:10:12:73;
- **STM32_Programmer_CLI не запускать вообще**;
- **никогда не назначать плате B адрес 192.168.88.14** и адреса других узлов;
- **C6 платы A не трогать** — в том числе не использовать её прошивку как «проверку»;
- скрипты, зашитые на плату A, как есть не запускать: reports/p2/hw/*.py,
  reports/p3/**/*.py, tests/bench/{smoke,boot_cycle,bench_console}.py. Шаблон для платы B —
  reports/p5/hw/board_b.py (константа `BOARD_A_REFUSED` отвергает адрес и серийники
  платы A до открытия портов) и run_image_b.py. Новый скрипт — в reports/p6/hw/, перед
  первым запуском `grep -n "192.168.88.14\|002F002B\|STM32_Programmer_CLI\|21103\|21301" *.py`
  находит только `BOARD_A_REFUSED`.
- NEXT-P3-SESSION.md (основное дерево) называет плату J-Link платой P3 — устарело:
  плата A — P3, плата B — P4, P5 и теперь P6. Основное дерево и незакоммиченные файлы
  других сессий (reports/p3/tuning, reports/crypto, storage_bench.c) не трогать.

**Плата B — на J-Link — наша:**
- J-Link V11 000941000024 (`-USB 000941000024` в каждой команде JLinkExe), STM32U585AI;
- консоль USART1: /dev/cu.usbmodem5AE60208891 (серийник 5AE6020889). Консоль теряет
  входные байты — команды шелла со стенда слать по telnet или с подтверждением и повтором;
- CDC приложения — USB serial **ровно** `2037394C3543501200210047` (сейчас
  /dev/cu.usbmodem11201); recovery стендового MCUboot — **ровно** `3543501200210047`
  (суффикс первого — сравнивай серийники целиком; macOS иногда показывает recovery
  дважды, 11201/11203);
- MAC 80:34:28:10:6a:1d, DHCP 192.168.88.13, веб-пароль `cedar-bench-P4-boardB`;
- образ на плате — P5 №4 (`2e2f30b5…`), оставлен режим `coproc dtr on` (DTR на CDC
  включает мост к USART3);
- прошивка STM32: `reports/p5/hw/run_image_b.py <workdir> <zephyr.signed.bin> <с>`
  (CDC recovery стендового MCUboot, ~6 мин, 5 с ожидания DFU на каждом старте). MCUboot
  не перешивать. `-Dmcuboot_EXTRA_CONF_FILE` начинается с `<repo>/sysbuild/mcuboot.conf;`;
- W5500 платы B иногда читает «одни единицы» (патч переоткрытия сокета в общем дереве
  zephyr; ≈20 % потерь ping, ложный link up). Гипотеза P4 — общий SPI2 с непрошитым C6;
- Matter-идентичность у плат общая — Matter-действия только по IP платы B.

Прочти до первой команды к плате: память проекта (board-b-jlink-bench, p5-logs-results,
p4-network-results, esp32-c6-uart-flashing, p0-bench-findings,
sysbuild-mcuboot-extra-conf-replaces, log-research-results), reports/p5/README.md,
reports/p5/hw/README.md, reports/p0/02-c6-rollback.md.

## ESP32-C6 платы B — главный факт этапа

**Загрузчик C6 платы B стёрт** (измерено): ROM печатает `invalid header: 0xffffffff`
и перезапускается сторожевым таймером ≈ раз в 0,65 с (reports/p5). Это доказывает
только пустой `0x0`: до таблицы разделов (`0x8000`), otadata (`0xd000`) и приложения
(`0x10000`) ROM не доходит, их состояние **не измерено**. Скорее всего пусто всё.

**Первое измерение этапа, до любых решений — только чтение:** тестовый образ на базе
`tests/esp_loader_integration` (на плате B уже проходил, без записи) дополнить
`esp_loader_flash_read` заголовков `0x0`, `0x8000`, `0xd000`, `0x10000`, `0x1d0000`.
Чтение через ROM loader разрешено решением P5, мост CDC не участвует (ограничение P0 на
чтение >8 КиБ через мост не относится). Результат — в reports/p6, он питает решения №1 и №3. **Решение владельца (2026-09-13): C6 этой платы не прошивать
никаким обходным путём** (esptool, мост CDC, тесты с записью) — первая прошивка должна
прийти **через веб**; это приёмочный случай P6.

А план (раздел 8) и контракт v1 записывают **только app `.bin`** в слот приложения;
загрузчик и таблица разделов — «сервисная процедура», merged full-flash образ
отклоняется. Значит, как этап сейчас сформулирован, первая прошивка через веб на плате
B **не загрузится** (тот же `invalid header`), health check провалится, задача закончится
`recovery_required`. Это решение №1 владельца, см. ниже. Без него не записывать в C6
ни байта.

### Проверено 2026-09-14: один файл с `0x0` без сервисных операций

Проект CP `SoC_and_w5500/esp32c6-hosted-cp` (ESP-Hosted 3.0.6, ESP-IDF 5.5.5,
`~/esp/idf5.5.5-matter1.5/esp-idf`) собран копией в scratchpad (как `build.sh`, вариант
`bt`, исходный каталог `build/` не тронут). Сборка даёт bootloader `0x0` 22 176 Б,
partition table `0x8000` 3 072 Б, `ota_data_initial.bin` `0xd000` 8 192 Б (все байты `0xFF`),
app `0x10000` 1 402 640 Б и `flash_args`. **`idf.py merge-bin`** (`esptool merge_bin -f raw
@flash_args`) делает `merged-binary.bin` — **1 468 176 Б (`0x166710`), пишется с `0x0`**.

Разбор файла:
| Участок | Содержимое |
|---|---|
| `0x00000–0x056A0` | bootloader, байт в байт как `bootloader.bin` (заголовок `E9 03 02 20`, bootloader_desc magic `0x50` на `0x20`) |
| `0x056A0–0x08000` | `0xFF` |
| `0x08000–0x08C00` | partition table, байт в байт; MD5-запись совпадает с расчётной; nvs `0x9000`+`0x4000`, otadata `0xd000`+`0x2000`, phy_init `0xf000`+`0x1000`, ota_0 `0x10000`+`0x1c0000`, ota_1 `0x1d0000`+`0x1c0000` |
| `0x08C00–0x0D000` | `0xFF` — **накрывает NVS `0x9000–0xD000`** |
| `0x0D000–0x0F000` | otadata, все `0xFF` (загрузчик выберет `ota_0`) |
| `0x0F000–0x10000` | `0xFF` — накрывает phy_init (в sdkconfig `ESP_PHY_INIT_DATA_IN_PARTITION` не задан — раздел не используется; калибровка PHY хранится в NVS) |
| `0x10000–0x166710` | app, байт в байт; до `ota_1` (`0x1d0000`) не доходит |

Следствия для P6:
- **Пустой C6 можно поднять одним файлом через веб**: bootloader + таблица + otadata + app
  за одну запись с `0x0`, без esptool и сервисных процедур. Проверка такого файла на
  устройстве: `0xE9` + bootloader_desc на `0x0`, таблица на `0x8000` с верным MD5 и
  разметкой из allowlist профиля, app на `0x10000` с app_desc и SHA-256, конец файла ≤ `0x1d0000`.
- **Цена**: запись с `0x0` одним куском стирает NVS C6 и сбрасывает otadata на `ota_0`.
  Что лежит в NVS C6: сама прошивка CP ничего своего не пишет (`main.c` — только
  `nvs_flash_init`/`nvs_flash_erase` при смене версии NVS); там кеш конфигурации Wi-Fi
  драйвера (`ESP_WIFI_NVS_ENABLED=y`) и калибровка PHY (первая загрузка после стирания
  сделает полную калибровку).
- **Владелец, 2026-09-14: «Wi-Fi пароли всё равно будут храниться на STM32 в settings».**
  Источник истины для Wi-Fi — device-config-store STM32 (P4), STM32 заново передаёт
  конфигурацию в C6. Значит, стирание NVS C6 ничего не теряет, и **единый файл с `0x0`
  годится и для обычного обновления**, не только для пустого чипа. Критерий раздела 10
  «сохранение settings и NVS C6» переформулировать при закрытии этапа: сохраняются
  settings STM32 (включая Wi-Fi), C6 после обновления переподключается к Wi-Fi по
  конфигурации STM32; NVS C6 может быть стёрт. Проверка на плате: калибровка PHY и
  переподключение после обновления — время записать.
- Необязательная оптимизация, если понадобится: писать из такого файла только непустые участки (`[0x0,0x8C00)`,
  `[0xD000,0xF000)`, `[0x10000, конец)`, промежутки не стирать — `esp_loader_flash_start`
  шлёт `FLASH_BEGIN` с `erase_size` записи, ROM/stub стирает сектора 4 КиБ под ней;
  что промежуток между участками действительно не стирается — проверить чтением NVS).
  После заявления владельца это не требуется.
- Контракт сейчас прямо запрещает merged full-flash как **app image**; `FirmwareImage.kind`
  уже знает `recovery_bundle`, `format` — только `raw_app`/`cedar_package`. Приём
  merged-файла — небольшая правка контракта (новый `format`, например `raw_full_flash`, с
  `kind=recovery_bundle`) — решение владельца.
- Сборки не воспроизводимы байт в байт (`APP_REPRODUCIBLE_BUILD` не задан): новый
  bootloader отличается от сборки 2026-09-06 только датой сборки в bootloader_desc и
  SHA-256 (40 байт), app — датой. Профиль совместимости по sha256 потребует фиксировать
  конкретный выпуск (решение №4).
- Размер 1,47 МБ ≤ `UPLOAD_MAX_BYTES` mock (2 МБ); на 115200 ≈ 2 мин плюс стирание.

Воспроизвести: скопировать проект без `build*`, `components/esp_hosted` → абсолютная ссылка
на `zephyr_latest/esp-hosted-3.0.6`, `sdkconfig` проекта → `<build>/sdkconfig`, затем
`idf.py -B <build> -DSDKCONFIG=<build>/sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;defaults.bt" build merge-bin`.

## Решения владельца — спросить в самом начале (до ответа — ядра и sim)

1. **Как пустой C6 получает загрузчик, таблицу разделов и otadata.** Варианты:
   - (А) разовая сервисная запись bootloader + partition table + otadata (esptool через
     мост CDC или тестовой прошивкой), приложение — через веб. Дёшево, но это «обходной
     путь», который владелец запретил; и путь CDC — тот, где в P5 голодали потоки;
   - (Б) **режим восстановления в устройстве**: updater читает `0x0`/`0x8000` через ROM
     loader (`esp_loader_flash_read`), при пустом или чужом загрузчике/таблице пишет
     проверенный набор для поддерживаемого профиля (bootloader 22 176 Б, partition table
     3 072 Б, otadata 8 192 Б — встроены в прошивку STM32 из той же сборки CP; эти
     ~33 КБ двоичных данных в репозитории — тоже вопрос решения №5) и затем
     приложение; включается только с `acknowledge_recovery=true` и отражается в
     контракте. Буквально выполняет «первая прошивка через веб» и даёт recovery для
     строки «повтор и recovery» матрицы. Цена — объём и правка контракта;
   - (В) `recovery_bundle`/`.cedarfw` сейчас — контракт относит это за v1.
   - (Г) **единый файл `merged-binary.bin` с `0x0` через веб** (см. «Проверено
     2026-09-14»): пользователь загружает один файл из сборки CP, устройство проверяет
     все три части и пишет его с `0x0`. Раз Wi-Fi-пароли хранятся на STM32 (владелец),
     стирание NVS C6 допустимо, и **один формат покрывает и первую прошивку пустого
     чипа, и обычное обновление, и восстановление** (ROM loader C6 в маске, так что
     обрыв посреди записи загрузчика лечится повтором через веб, пока жив STM32). Ничего
     не встраивается в прошивку STM32. Правка контракта: новый `format` (например
     `raw_full_flash`); нужен ли тогда `raw_app` в v1 и что значит `acknowledge_recovery`
     для такого файла — решить.
   **Рекомендация — (Г) как единственный формат v1**; решает владелец.
2. **Health check после записи.** ESP-Hosted транспорт **не восстанавливается после
   сброса C6 без сброса STM32** (P0), а в драйвере `esp_hosted_mcu` нет пути повторной
   инициализации: только `DEVICE_DT_INST_DEFINE` с init, сброс EN — лишь в init, версия
   прошивки берётся из priv event один раз (`fw_version == 0`). Контракт: `succeeded` —
   только после reconnect и подтверждения версии. Варианты: (а) патч драйвера в `patches/`
   (повторная инициализация транспорта), момент применения — владелец (дерево общее);
   (б) последняя фаза — перезагрузка STM32, задача сохранена в `/lfs` как «ожидает
   проверки запуска» и завершается на следующем старте (проверка только читающая —
   продолжения разрушительной установки нет); (в) слабая проверка по UART-логу C6
   (баннер загрузчика IDF, `Loaded app from partition`, версия приложения — P5 уже
   разбирает строки), честно названная слабой. Рекомендация — (б) + (в) как ранний
   признак; решает владелец.
   Уточнение для платы B: `esp_hosted_mcu_priv_event()` записывает версию, пока
   `fw_version == 0`, а в P0 после сброса C6 работающий драйвер **получал** priv event 34.
   Сейчас на плате B версия 0 — значит, после **первой** прошивки пустого чипа версия
   может появиться через транспортный слой без перезагрузки STM32 и без патча (сильнее
   баннера UART, слабее полного reconnect; при повторной прошивке guard `== 0` её не
   обновит). Проверь, что на плате B с пустым C6 драйвер вообще дошёл до потока событий
   (в P4 был `coprocessor WifiInit failed`), и запиши.
3. **Слот записи** (варианты P0, раздел 8): (А) `ota_0` на месте, otadata не трогать —
   отката нет; (Б) писать `ota_1`, после проверки переключать otadata (seq, CRC, две
   копии); (В) Б + rollback в CP (нужна сервисная перепрошивка загрузчика C6 — за v1).
   Rollback в sdkconfig CP выключен (`# CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is not set`).
   На пустом C6 при варианте Б первый образ всё равно придётся связать с otadata.
   **При варианте (Г) решения №1 выбор уже сделан**: единый файл пишет приложение в
   `ota_0` и otadata `0xFF` → всегда вариант (А), `ota_1` не используется. Вариант (Б)
   с единым файлом возможен только если устройство перекладывает приложение в `ota_1` и
   само пишет otadata — это уже не «записать файл как есть».
4. **Профиль совместимости.** В app `.bin` нет ничего, что отличает Wi-Fi CP от OT CP:
   обе сборки — project `eh_cp_c6_cedar`, chip_id 13. Отличаются только содержимым
   (sha256 `6fcadfd5…` Wi-Fi, 1 402 640 Б; `b39cf8a4…` OT, 803 520 Б). Откуда
   `host_protocol` и `partition_layout_id`: allowlist sha256 известных сборок (release
   manifest), правило по app_desc (project + IDF + версия), маркер в сборке CP (правка
   соседнего проекта)? OT-образ — естественная негативная фикстура «incompatible»,
   если правило его отличает.
5. **Бинарные фикстуры**: класть ли 1,4 МБ образы в репозиторий. Рекомендация: в репо —
   синтетические фикстуры с настоящими заголовками (обрезанные, чужой chip_id, merged,
   без app_desc), настоящие образы — из `SoC_and_w5500/esp32c6-hosted-cp/build{,-ot}`
   только для hw, путь и sha256 в отчёте.
6. **Потеря питания во время erase/write/verify**: руками владельца или сбросом STM32
   через J-Link (как заменяли в P4/P5)? Это разные отказы: сброс STM32 через J-Link
   заново запускает `esp_hosted_mcu_spi_init`, которая дёргает PA8, — C6 сбрасывается
   по EN посреди записи, питание flash C6 остаётся; снятие питания — C6 теряет питание
   записи. Оба — отдельные строки проверки `recovery_required`.
7. **`higher-baudrate`**: разрешено ли пробовать скорость выше 115200 на плате B
   (не проверялась ни разу).

## Работа рядом с другими сессиями

- База: `cedar_board/main` = `4e5b8a4` (слиты P4 и P5, PR #1 и #2). Локальный `main`
  основного дерева отстаёт — он checked out в дереве сессии P3, не двигать. Worktree:
  `git worktree add ../cedar_p6 -b p6-updater cedar_board/main` — **только с разрешения
  владельца**; коммиты и push — только по его просьбе.
- Проверь в worktree сборку прошивки и sim (`tests/ci/run-sim-tests.sh` берёт имя
  каталога через basename, `-T tests`; имя образа sysbuild = `cedar_p6`).
- Дерево zephyr, `modules/lib/esp-serial-flasher`, `modules/lib/matter` — **не править**;
  патчи — только через `patches/` с решением владельца.
- Мутации — в копии в scratchpad (colima монтирует только /Volumes/Programming: копию
  заносить в контейнер `docker cp`). Сборки — только в scratchpad.

## Сначала прочти

- development-plan.md: раздел 3 (модули `firmware-store`, `esp-loader-adapter`,
  `coprocessor-updater`, `coprocessor-manager`; правила интеграции: владение UART и
  EN/BOOT, взаимоисключение update ↔ apply/scan/reset/мост), раздел 4 (экран ESP32),
  раздел 8 целиком, раздел 9 (upload chunk, staging), раздел 10 строка P6, раздел 11
  строки Upload и UART update, раздел 12 строки трёх модулей.
- api-contract.md: «Upload и ESP32 update» (весь HTTP flow, состояния upload, фазы
  install, `ethernet_required`, повтор после таймаута, `interrupted`), решения P5 про
  `CoprocessorStatus.state` и `esp32_uart`.
- openapi.json: `createUpload`, `getUpload`, `writeUploadChunk`, `verifyUpload`,
  `deleteUpload`, `startCoprocessorUpdate`, `cancelJob`; схемы `UploadRequest`, `Upload`,
  `FirmwareImage`, `UpdateRequest`, `UpdateSummary`, `Job` (kinds `upload_chunk`,
  `firmware_verify`, `firmware_delete`, `coprocessor_update`), `CoprocessorStatus`,
  `Capabilities` (`esp32_uart`, `limits.upload_*`, `firmware_formats`,
  `update_requires_ethernet`).
- tools/api-contract/cedar_contract/mock/firmware.py (offset растёт только по завершении
  задачи записи; OTA → 503), constants.py (`UPLOAD_CHUNK_BYTES` 16 384,
  `UPLOAD_MAX_BYTES` 2 097 152).
- modules/coprocessor-manager (README, заголовок: `struct coprocessor_platform`, режим
  `FLASHING` — «flasher opens the UART itself»), src/services/coprocessor,
  reports/p5/notes-coprocessor-manager.md.
- modules/lib/esp-serial-flasher: include/esp_loader.h, port/zephyr_port.{h,c},
  zephyr/Kconfig, dts/bindings; tests/esp_loader_integration (README, overlay).
- zephyr/drivers/misc/esp_hosted_mcu/{esp_hosted_mcu.c,esp_hosted_mcu_spi.c}.
- modules/web-api (правило 8: не-JSON тело → 415; `CONFIG_WEB_API_JSON_BODY_MAX`),
  modules/job-manager (`job_cancel`, `cancellable`).
- tests/storage_report, SoC_and_w5500/esp32c6-hosted-cp/build/flasher_args.json.

## Установленные факты, от которых зависит дизайн

1. **esp-serial-flasher захватывает USART3 и EN/BOOT уже на старте.** С DT-узлом
   `espressif,esp-loader` `DEVICE_DT_INST_DEFINE(... POST_KERNEL ...)` вызывает
   `esp_loader_init_serial()` → `zephyr_port_init()`: EN и BOOT — `GPIO_OUTPUT_INACTIVE`,
   `tty_init()` ставит **свой** IRQ-callback на USART3 (`tty.c:238`). Конфигурация
   (`esp_loader_config_t`: uart, enable/boot `gpio_dt_spec`, скорости) берётся только из
   DT, таблица ops порта `static` — экземпляр без DT-узла не создать. `esp_loader_deinit()`
   выключает RX/TX IRQ и переводит EN и BOOT в **`GPIO_DISCONNECTED`** (линия EN C6
   повиснет) и обнуляет loader. Следствия: coprocessor-manager должен после init
   вернуть себе callback USART3 и линии; перед записью — снова `esp_loader_init_serial()`
   на loader устройства (или `configure_tty`), после — deinit и немедленное
   переконфигурирование EN/BOOT и возврат callback консоли. Проверь порядок init на плате.
2. **Страпы**: `reset_target` — BOOT=0, EN активен `SERIAL_FLASHER_RESET_HOLD_TIME_MS`,
   отпущен; `enter_bootloader` — BOOT и EN активны, EN отпущен, через
   `SERIAL_FLASHER_BOOT_HOLD_TIME_MS` отпущен BOOT. На плате проверено 200 мс (P0);
   в DT-узле обе линии `GPIO_ACTIVE_LOW`, у `esp-hosted-mcu` тот же PA8 — `ACTIVE_HIGH`.
3. **API библиотеки v2.0.0**: `connect`, `connect_with_stub`, `flash_start/write/finish`
   (MD5 по окончании, `skip_verify`), `flash_deflate_*`, `flash_read`, `flash_erase(_region)`,
   `flash_detect_size`, `flash_verify_known_md5`, `read_mac`, `get_security_info`,
   `change_transmission_rate`, `reset_target`. Stub и deflate на плате не пробовались.
4. **Образы CP** (`SoC_and_w5500/esp32c6-hosted-cp/build`, flasher_args.json):
   bootloader `0x0` 22 176 Б, partition table `0x8000` 3 072 Б, otadata `0xd000` 8 192 Б
   (`ota_data_initial.bin`), app `0x10000` 1 402 640 Б; DIO, 4 МБ, 80 МГц. App: magic
   `0xE9`, 5 сегментов, chip_id 13, `hash_appended=1` (SHA-256 в конце), app_desc на
   `0x20` — magic `0xABCD5432`, project `eh_cp_c6_cedar`, version `1`, 14:03:44 Sep 6 2026,
   IDF `b774170f`. bootloader.bin — тоже `0xE9`, без app_desc. Таблица разделов на C6 платы A
   (P0): nvs `0x9000`, otadata `0xd000`, phy_init `0xf000`, ota_0 `0x10000`+`0x1c0000`,
   ota_1 `0x1d0000`+`0x1c0000`.
5. **Время**: 115200 8N1 ≈ 11,5 КБ/с → 1,4 МБ без сжатия ≈ 2 мин плюс стирание; upload
   через W5500 в P5 шёл ≈16–28 КБ/с → ≈1–1,5 мин плюс `fsync` каждого куска. В P4 запись
   в `/lfs` (файловый бэкенд settings там же) останавливала HTTP-поток на секунды —
   **первое измерение этапа — время `write+fsync` куска 16 КиБ**.
6. **`/lfs`**: 5120 КиБ, на свежей плате 4992 КиБ свободно, блок 64 КиБ (P0); сейчас там же
   растёт `/lfs/settings`. Квота удерживается сервером, не берётся из свободного места.
7. **web-api**: тело не `application/json` → 415 (правило 8), тело JSON ≤
   `CONFIG_WEB_API_JSON_BODY_MAX`; ответ — буфер 16 КиБ или поток (`web_api_reply_stream`,
   P5); `SO_SNDTIMEO` 5 с. Для `PUT …/data` нужен новый вид тела: `application/octet-stream`
   до `limits.upload_chunk_bytes`, копия в буфер задачи (указатель на тело после callback
   держать нельзя), `Idempotency-Key` + offset.
8. **`ethernet_required`**: запрос install должен прийти через Ethernet. Сейчас в
   `web_api_call` есть `peer`, но не интерфейс приёма — найди, как получить локальный
   адрес/интерфейс соединения из HTTP-сервера Zephyr, без правки дерева.
9. **Взаимоисключение уже есть**: `coprocessor_manager_claim()` (apply, scan). Update
   добавляет свой claim; мост (DTR, `coproc mode bridge`) во время update — отказ `409 busy`
   или вытеснение моста — решить и записать; при `coproc dtr on` на стенде это случится
   на первом же прогоне.
10. **Фронтенд**: `crypto.subtle` недоступен вне secure context, а `http://192.168.88.13`
    не secure context. SHA-256 файла — чистая JS-реализация (своя или маленькая
    зависимость), проверить на `File` 1,4 МБ.
11. P5: `CoprocessorStatus.state=updating` при `uart_mode=flashing`; `esp32_uart` —
    `not_implemented`; фазовые маркеры `paused`/`reset` пишет менеджер. Голодание потоков
    после потока через **мост CDC** (P5, причина не найдена) — путь записи из веба CDC не
    использует, но при варианте (А) решения №1 — использует.
12. Фейки: `tests/fakes/fake_uart.{c,h}` уже есть (P5); фейка интерфейса библиотеки
    esp-serial-flasher и fake flash для LittleFS — нет (раздел 12 требует оба).

## Задача P6

Критерий раздела 10: «Обновление и повтор после обрыва через Ethernet, отклонение чужого
target, сохранение settings и NVS C6» (NVS C6 — см. заявление владельца от 2026-09-14:
Wi-Fi-пароли хранятся на STM32, стирание NVS C6 допустимо; формулировку поправить при
закрытии). Модули — ядро + тонкий адаптер, README
(жизненный цикл, потоки, владение, ошибки).

1. **`firmware-store`** (`modules/firmware-store`): один upload; файл в `/lfs/firmware/` с
   именем от сервера (клиентское имя — только отображение, path traversal невозможен);
   квота до первого байта и на всё время; кусок → запись + `fsync` → `received_bytes`;
   метаданные переживают перезагрузку, offset восстанавливается по размеру файла,
   сверенному с метаданными; остатки прошлых попыток удаляются по метаданным; expire
   24 ч uptime без активности, активная установка блокирует expire и delete (`409`).
   SHA-256 потоково по файлу при verify.
2. **Валидатор образа** (часть firmware-store, чистые функции): заголовок `0xE9`,
   chip_id 13, границы и число сегментов, SHA-256 appended, app_desc magic, размер ≤ слота
   профиля, профиль совместимости по решению №4; merged full-flash и bootloader, поданные
   как app, — `invalid_image` (merged-файл принимается только как формат восстановления,
   если владелец выберет вариант (Г) решения №1); чужой chip — `unsupported_target`; неизвестный профиль —
   `incompatible_firmware`. Корпус фикстур: верные, чужой target, обрезанные, merged,
   без app_desc, OT-сборка.
3. **`esp-loader-adapter`**: захват UART и линий у coprocessor-manager (режим `flashing`) →
   init порта → connect (попытки, stub — после измерения) → проверка ESP32-C6 → чтение
   состояния (`flash_read` загрузчика/таблицы/otadata для решений №1 и №3) → erase/write
   (deflate — после измерения) → MD5 verify → normal boot → deinit и **гарантированный**
   возврат GPIO и callback консоли при любом исходе, включая отказ посреди записи.
   Коды библиотеки → `ErrorDetail`; прогресс по фазам. Адреса — только из профиля.
   Не стирать NVS и phy_init, не трогать eFuse, Secure Download Mode не включать.
4. **`coprocessor-updater`**: автомат фаз `preflight → entering_bootloader → begin →
   writing → verifying → (activating) → reconnecting → health_check → (confirming) →
   complete` по контракту; `ethernet_required` до сброса; отказ и таймаут каждой фазы;
   `recovery_required`; после перезапуска STM32 — `interrupted`, никакого автоматического
   продолжения, сверка и явный повтор; health check — по решению №2; `last_update` в
   статусе; claim против apply/scan/reset/моста.
5. **HTTP API v1**: 6 операций upload/update + `cancelJob` (если отмена разумна для фаз
   upload; install после `begin` — не отменяемый, `cancellable=false`); capabilities:
   `esp32_uart` из реального состояния, `limits.upload_max_bytes` = квота, `firmware_formats`
   `["raw_app"]`, `update_requires_ethernet=true`; `CoprocessorStatus.uart_update`,
   `last_update`, `firmware_version`, `host_protocol`, `partition_layout_id`. Маршруты в
   routes.h/http_resources.h; контрактная проверка маршрутов — 35 из 35 (или объяснить
   недостающие).
6. **Экран ESP32** (`src/web/frontend/src/features/coprocessor-update`, раздел 4): версия,
   transport health, `uart_mode`; выбор файла, локальный SHA-256, загрузка кусками —
   следующий `PUT` только после завершения задачи предыдущего; возобновление после
   перезагрузки страницы по `GET /firmware/uploads/{id}`; результат verify; прогресс по
   фазам; reconnect/recovery; причины недоступности; OTA — недоступная возможность с
   причиной, без выключенного переключателя; строки в ru.ts.
7. **Mock** выровнять под устройство (как в P4/P5), README mock — таблица правил;
   `api-contract.md` — решения P6 (особенно решение №1, если меняет контракт);
   `psram_sections.ld` — новые библиотеки, проверка по map.

## Тесты

- **sim** (раздел 12): firmware-store на LittleFS поверх fake flash — chunk/offset/SHA-256,
  повтор и неверный offset, `storage_full`, уборка остатков, восстановление после
  «перезагрузки», expire; валидатор на корпусе фикстур; esp-loader-adapter на
  подменённом интерфейсе библиотеки — трансляция ошибок, прогресс, порядок захвата и
  возврата UART, уборка GPIO и normal boot при аварийном выходе; coprocessor-updater на
  фейковом loader — отказ и таймаут каждой фазы, `recovery_required`, запрет продолжения
  после reboot, `ethernet_required`, claims; web_api и web_api_http — octet-stream тело,
  413/415, повтор ключа, обрыв посреди куска на настоящем сервере.
- **Контракт**: 5 проверок + mock pytest.
- **Фронтенд**: vitest (SHA-256 на эталонных векторах и 1,4 МБ, цикл кусков, возобновление,
  фазы), компонентные на фикстурах OpenAPI, e2e против mock с `setInputFiles`;
  `device.spec.ts` против платы B.
- **Плата B** (строки Upload и UART update раздела 11; каждый лог — с sha256 образа STM32 и
  образа C6), в таком порядке:
  1. без записи в C6: upload, verify, отказы (чужой target, обрезанный, merged, OT по
     решению №4, неверный offset, повтор, logout посреди upload, перезагрузка STM32
     посреди upload — offset после `fsync`, заполненный `/lfs` — `storage_full` заранее,
     остатки прошлых попыток); время `write+fsync` куска и остановки HTTP-потока;
  2. без записи в C6: `entering_bootloader` → connect → идентификация → `flash_read`
     загрузчика/таблицы → normal boot → консоль вернулась, линии EN/BOOT в исходном
     состоянии, маркеры `paused`/`reset`; отказ sync (C6 держим в сбросе через EN);
     мост включён (DTR) во время update;
  3. **только после решений №1–№3: первая настоящая прошивка C6 платы B через веб**
     (Wi-Fi CP `6fcadfd5…`) — это приёмка. Затем: C6 загрузился, строки ESP-IDF в логах
     (закрывает открытую строку P5), транспорт/версия по решению №2, settings STM32 (в том
     числе Wi-Fi) сохранились, C6 получил конфигурацию Wi-Fi от STM32 (если у платы B есть
     сеть), время первой загрузки с полной калибровкой PHY, Matter-fabrics на месте;
  4. повтор и recovery: обрыв (сброс STM32 через J-Link / питание по решению №6) на
     erase, write, verify → `interrupted`, Ethernet и STM32 доступны, явный повтор доводит
     до успеха; повтор той же прошивки;
  5. запрос install через Wi-Fi → `ethernet_required` (станет возможен только после
     шага 3 и подключения Wi-Fi платы B — если сети нет, оставить открытым).
- После шага 3 **перемерить W5500** (потери ping, ложный link up) — проверка гипотезы P4
  об общем SPI2; записать, что строки Wi-Fi P4 и уровни ESP-IDF P5 стали проверяемыми на
  плате B (продолжение — вне P6, если владелец не скажет иначе).
- **Мутации** нового кода — в копии в scratchpad, выжившие закрыть тестами или обосновать.

## Как проверять

    colima start --cpu 4 --memory 8 --disk 60 --mount /Volumes/Programming:w
    tests/ci/run-sim-tests.sh
    tests/ci/run-contract-tests.sh
    tests/ci/run-frontend-tests.sh            # E2E_DEVICE_URL=http://192.168.88.13 — только плата B

Сборка (сначала `cd src/web/frontend && npm ci && npm run build`), каталог — scratchpad:

    cd /Volumes/Programming/Zephyr/zephyr_latest
    .venv/bin/west build -p always -b cedar_switch_3in4out_power_rev3/stm32u585xx/ext_flash_app \
      --sysbuild -d <scratchpad>/build cedar_p6

## Как работать

- Сначала публичный интерфейс с обоснованием, потом реализация, потом тесты — вместе
  (раздел 12). Набор, прошедший с первого раза, ничего не доказывает — мутации.
- Не полагайся на документацию без проверки; важное — измеряй. Перед починкой убедись,
  что дефект настоящий (устаревший бинарник, голодание потока W5500, потерянные байты
  консоли, чужой порт с похожим серийником уже вводили в заблуждение).
- **Записывай результаты исследований в reports/p6 сразу**, не в конце.
- **Ни одного байта записи/стирания в C6 до решений №1–№3.** Чтение через ROM loader и
  сброс через EN разрешены (решение P5).
- **Не правь** дерево zephyr, esp-serial-flasher, вендорский Matter; патчи — через
  `patches/` с решением владельца.
- **Не убирай отладочный код** (`coproc`, `wifi_ctrl`, Matter shell, storage_bench) —
  добавляй отладочные команды updater рядом.
- **Не меняй бэкенд settings** — решается в другом треде; только измеряй влияние на upload.
- **Коммиты не делай сам.** Доведи, прогони тесты, оставь в worktree, скажи, что во что
  складывается.
- Когда закончишь — обнови строку P6 в разделе 10, строки Upload и UART update в разделе 11,
  строки `firmware-store`, `esp-loader-adapter`, `coprocessor-updater` в разделах 3 и 12,
  решения владельца — в раздел 13 (и раздел 8, если меняется слот/recovery); отчёт —
  docs/device-development/reports/p6/ (README, hw/README, логи). Незакрытое перечисли явно.
```
