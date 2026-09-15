# Плата B (J-Link): прошивка и проверка связи с ESP32-C6

Дата: 2026-09-13. Вторая плата `cedar_switch_3in4out_power_rev3`, подключена
через J-Link. Задача владельца: проверить, есть ли связь с распаянным ESP32-C6
(раньше он был недоступен), и залить на плату эталонную прошивку.

## Итог

**Связь с ESP32-C6 есть, но флеш C6 пустая.**

- UART-линия и управление EN/BOOT исправны: `esp_loader_integration` — PASS,
  C6 опознан как ESP32-C6.
- При обычном старте C6 выдаёт только ROM `invalid header: 0xffffffff`: на флеш
  нет ни загрузчика, ни прошивки.
- Поэтому эталонное приложение не получает ответа по ESP-Hosted SPI:
  `coprocessor firmware version unknown`, `coprocessor WifiInit failed`.

**Решение владельца:** C6 на плате B не прошивать. Первая прошивка должна прийти
через веб, когда эта функция будет готова, — это и станет проверкой.

## Две платы на стенде

| | Плата A | Плата B |
|---|---|---|
| Отладчик | ST-Link V3, `002F002B3233510739363634` | J-Link V11, `000941000024` |
| Консоль (USART1) | VCP ST-Link | мост WCH, называет себя «ASIX», серийник `5AE6020889`, `/dev/cu.usbmodem5AE60208891` |
| Серийник USB CDC (hwinfo) | `2037394C3543501200550045` | `3543501200210047` |
| MAC (EEPROM) | `80:34:28:10:12:73` | `80:34:28:10:6a:1d` |
| IP (DHCP) | 192.168.88.14 | 192.168.88.13 |
| Веб-идентификатор | `cedar-2037394c3543501200550045` | `cedar-2037394c3543501200210047` |

VCP самого J-Link (`/dev/cu.usbmodem0009410000241`) — не консоль платы.
Matter-идентичность у плат общая (PIN 20202021, discriminator 0xF00), поэтому
commissioning — только по IP. Во время этой работы на плате A шёл перебор
настроек хранилища в другой сессии; плата A и ST-Link не затрагивались, окно
Matter на плате B не открывалось.

## Проверки

| # | Что | Результат | Лог |
|---|---|---|---|
| 1 | Консоль и состояние платы до работ | На плате был диагностический образ линий без шелла; MCUboot штатный | `logs/01-original-image-boot.log` |
| 2 | Внутренняя флеш до изменений | Первые 85 872 байта совпадают с MCUboot проекта, дальше `0xFF` | `logs/00-provenance.txt` |
| 3 | Эталонное приложение, первый старт | MAC из EEPROM, DHCP; C6 по SPI не отвечает; BUS FAULT в `w5500_rx` на 8,9 с | `logs/03-reference-first-boot-busfault.log` |
| 4 | `tests/esp_loader_integration` | PASS: `target reports: ESP32-C6` | `logs/04-esp-loader-integration.log` |
| 5 | `tests/coprocessor_link_check` | PC11 pull-up=1/pull-down=1, 28 528 байт при старте — только `invalid header: 0xffffffff` | `logs/05-coprocessor-link-check.log` |
| 6 | Возврат эталонного приложения | hash на плате совпадает, старт до DHCP; затем BusFault, система остановлена | `logs/06-reference-restore.log` |

Разбор проверки 5: при pull-up=1/pull-down=0 линия висела бы (C6 не подключён,
как на заменённой плате 2026-09-12). Здесь её держит сам C6. 28,5 КБ ROM-вывода —
это ROM, который циклически не находит загрузчик.

## Как прошивалась плата B

**J-Link не пишет внешнюю QSPI этой платы.** Разводка OCTOSPI1
(PB10/PA4/PB1/PB0/PA7/PA6) не входит в поддерживаемые загрузчиком J-Link;
чтение 0x90000000 через J-Link даёт нули. Внутреннюю флеш J-Link пишет
нормально, но только после reset-halt: при работающем образе с включённым
ICACHE отладочные чтения 0x08000000 и 0x02000000 искажаются.

Поэтому во внутреннюю флеш записан **стендовый MCUboot** — 110 384 из 131 072
байт. Лог у него идёт в консоль UART, образ принимается по USB CDC. Конфиг:
`bench-mcuboot/mcuboot-cdc-recovery.conf` и `.overlay`
(`zephyr,uart-mcumgr = &cdc_acm_uart0`), поверх `sysbuild/mcuboot.conf` проекта.

Сборка (только MCUboot, каталог вне репозитория):

    west build -p always -b cedar_switch_3in4out_power_rev3/stm32u585xx/ext_flash_app --sysbuild \
      -d <build> -t mcuboot cedar_switch_3in4out_power -- \
      "-Dmcuboot_EXTRA_CONF_FILE=<repo>/sysbuild/mcuboot.conf;<repo>/docs/device-development/reports/board-b/bench-mcuboot/mcuboot-cdc-recovery.conf" \
      -Dmcuboot_EXTRA_DTC_OVERLAY_FILE=<repo>/docs/device-development/reports/board-b/bench-mcuboot/mcuboot-cdc-recovery.overlay

Запись: `JLinkExe -USB 000941000024 -device STM32U585AI -if SWD -speed 4000
-autoconnect 1 -CommanderScript scripts/flash-bench-mcuboot.jlink.template`
(подставить `<build>`). Лог: `logs/02-jlink-flash-bench-mcuboot.log`.

Загрузка образа: сброс через J-Link, в течение 5 с найти CDC по серийнику
`3543501200210047` (`resolve_port` из `tests/bench/bench_console.py`), затем

    ~/go/bin/mcumgr --conntype serial --connstring "dev=<порт>,baud=115200,mtu=512" image upload <zephyr.signed.bin>

Всё это делает `scripts/run_test_image_b.py <workdir> <signed.bin> <сек>`.
Приложение 1,23 МиБ грузится за 5 мин 36 с (3,7 КиБ/с), тест — за 10 с.
Пустой ответ `Images:` — нормальный ответ recovery без образа.
`scripts/boot_and_c6_check_b.py` сбрасывает плату, снимает лог старта и шлёт
только читающие команды; `wifi_ctrl reset` не шлёт.

### Что не сработало

1. **Recovery по UART-консоли** (`bench-mcuboot/rejected-mcuboot-uart-recovery.conf`):
   загрузка шла со скоростью около 75 байт/с (часы на образ) и требовала
   выключить консоль MCUboot. Прервано.
2. **`-Dmcuboot_EXTRA_CONF_FILE` без `sysbuild/mcuboot.conf`.** В sysbuild явный
   `<image>_EXTRA_CONF_FILE` заменяет `sysbuild/<image>.conf`
   (`share/sysbuild/cmake/modules/sysbuild_extensions.cmake:281`), а overlay,
   наоборот, добавляется (строка 394). Из MCUboot пропали `BOOT_GO_HOOKS`, PSRAM,
   BBRAM, memory-mapped QSPI, а `BOOT_DISABLE_CACHES` стал `y`. MCUboot работал,
   но на переходе в приложение уходил в HardFault. Перед записью проверять в
   `.config` MCUboot: `BOOT_GO_HOOKS=y`, `BOOT_DISABLE_CACHES` не задан.
3. **`CONFIG_USB_DEVICE_SN`** в старом USB-стеке игнорируется: серийник берётся из
   hwinfo.

## Состояние платы B после работ

- Внутренняя флеш: стендовый MCUboot. Каждый старт на 5 с дольше (ожидание
  recovery). Штатный MCUboot проекта вернуть можно той же командой J-Link из
  обычной sysbuild-сборки.
- slot0: эталонное приложение, HEAD `0cb7f60` плюс незакоммиченная правка
  `src/diagnostic/storage_bench.c` соседней сессии (`logs/00-worktree.diff`),
  hash образа `0e5eb6ee…`.
- Приложение останавливается BusFault через несколько секунд после старта
  (см. ниже), в сети плата не отвечает.
- C6: флеш пустая, по решению владельца не прошивается.

## Открытые вопросы

- **BUS FAULT в `w5500_rx`** (`eth_w5500.c:256/268`, BFAR 0xc, поток `eth_w5500`):
  на плате B сработал в обоих наблюдавшихся стартах эталонного приложения
  (на 8,9 с в первом; во втором — позже 15 с, лог отказа не снят), система
  останавливается. Причина та же, что в P0 (`reports/p0/fault-c6-reset/`):
  драйвер не сверяет длину кадра из заголовка W5500 с размером буфера,
  исправление — PR #115626. На плате A отказ был редким, на плате B он
  мешает любой работе с приложением.
- **FRAM** (`mb85rsxx`) не определяется, RDID 00 00 00 00 — как на плате A.
- Оставить стендовый MCUboot на плате B или вернуть штатный — решение владельца.
