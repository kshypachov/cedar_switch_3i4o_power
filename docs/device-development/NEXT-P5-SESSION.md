# Промт: полное выполнение этапа P5 — Логи и владелец UART (плата на J-Link)

```
Выполняем этап P5 — Логи и владелец UART. Корень west workspace —
/Volumes/Programming/Zephyr/zephyr_latest

## ⚠️ Две платы. Работаем ТОЛЬКО с платой B на J-Link

К компьютеру подключены две одинаковые платы cedar_switch_3in4out_power_rev3.

**Плата A — на ST-Link — занята другой сессией (P3 / настройки хранения).
Её тесты прерывать нельзя. Не трогать никак:**
- не прошивать, не сбрасывать, не открывать её порты, не входить в её веб,
  не слать ей запросы API, chip-tool, ping-флуд, e2e;
- ST-Link V3, серийник 002F002B3233510739363634; порты /dev/cu.usbmodem21103,
  /dev/cu.usbmodem21301, USB CDC с серийником 2037394C3543501200550045
  (номера портов меняются — сверяй по серийнику);
- сеть: 192.168.88.14, MAC 80:34:28:10:12:73, web-id cedar-2037394c3543501200550045;
- **STM32_Programmer_CLI не запускать вообще** — он работает только через ST-Link;
- **никогда не назначать плате B адрес 192.168.88.14** и адреса других узлов.
- **Скрипты стенда, зашитые на плату A, как есть не запускать:**
  reports/p3/hw/*.py, reports/p3/settings-backends/run_backend.py,
  reports/p3/lfs-cache/compare.py, reports/p3/tuning/run_trial.py,
  reports/p2/hw/*.py, tests/bench/{smoke,boot_cycle,bench_console}.py.
  Шаблоны, безопасные для платы B, — reports/p4/hw/*_b.py (они есть только на ветке
  `p4-network`: worktree `zephyr_latest/cedar_p4/docs/device-development/reports/p4/hw/`;
  `net_api_b.py` уже
  отвергает адрес платы A до любого запроса — переиспользуй эту защиту). Новый
  скрипт — в reports/p5/hw/, перед первым запуском
  `grep -n "192.168.88.14\|002F002B\|STM32_Programmer_CLI\|21103\|21301" <скрипт>` — пусто
  (кроме константы отказа).
- NEXT-P3-SESSION.md (основное дерево) пишет, что P3 идёт на J-Link, — устарело:
  владелец назначил плату A — P3, плату B — P4 и теперь P5.

**Плата B — на J-Link — наша:**
- J-Link V11, серийник 000941000024, JLinkExe V9.76 (/usr/local/bin), устройство
  STM32U585AI; `-USB 000941000024` в каждой команде JLinkExe;
- консоль (USART1): мост WCH «ASIX», серийник 5AE6020889,
  /dev/cu.usbmodem5AE60208891. VCP самого J-Link (…0009410000241) — не консоль;
- USB CDC платы (hwinfo): 3543501200210047 — и recovery стендового MCUboot, и CDC
  приложения. **У приложения два CDC ACM с этим серийником**: `cdc_acm_uart0` (пир
  `uart-bridge0` ↔ USART3) и `cdc_acm_esp` («ESP32 Bridge», в коде не используется).
  `resolve_port()` из tests/bench/bench_console.py возвращает первый найденный порт —
  для моста определи нужный интерфейс по индексу/label (ioreg) и запиши способ;
  реши, `cdc_acm_esp` — мёртвый узел DT или задуманная точка passthrough;
- MAC 80:34:28:10:6a:1d, DHCP 192.168.88.13; администратор веба —
  пароль `cedar-bench-P4-boardB`;
- на плате образ P4 №4 (`0d32d454…`, reports/p4/README.md), бэкенд settings —
  файловый `/lfs/settings`;
- патч W5500 (переоткрытие сокета 0) уже применён в общем дереве zephyr, BUS FAULT
  `w5500_rx` больше не останавливает плату. Остались: чтения W5500 «одни единицы»
  (≈20 % потерь ping, до 200 мс ожидания в потоке RX на каждое переоткрытие) и
  ложный link up каждые ~2,9 с без кабеля — учитывай в замерах; для строки «burst»
  это даже полезный фон;
- Matter-идентичность у плат общая (PIN 20202021, discriminator 0xF00): любые
  Matter-действия — только по IP платы B.

Прочти до первой команды к плате: память проекта (board-b-jlink-bench,
p4-network-results, sysbuild-mcuboot-extra-conf-replaces, esp32-c6-uart-flashing,
p0-bench-findings), reports/p4/README.md и reports/p4/hw/README.md.

### Как прошивается плата B

J-Link не пишет внешнюю QSPI этой платы. Стендовый MCUboot с recovery по USB CDC
(5 с ожидания на каждом старте — учитывай в замерах):

    docs/device-development/reports/p4/hw/run_test_image_b.py <workdir> <zephyr.signed.bin> <сек прослушивания>

(в worktree от `p4-network`; иначе — из `cedar_p4`). Скрипт импортирует
`resolve_port` из tests/bench по абсолютному пути основного дерева — это нормально,
порт платы B он ищет по её серийнику. ~350–360 с на образ — копи изменения. MCUboot не перешивать. Если нужен другой
MCUboot: `-Dmcuboot_EXTRA_CONF_FILE` начинается с `<repo>/sysbuild/mcuboot.conf;`.
Сброс без прошивки — `reports/p4/hw/capture_boot_b.py <log> <с>`.

## ESP32-C6 платы B: что можно и что нельзя

**Flash C6 на плате B пустой, и прошивать его нельзя никаким путём** (решение
владельца: первая прошивка C6 этой платы должна прийти через веб — приёмка P6).
Никаких esptool, записи/стирания через esp-serial-flasher, `tests/esp_loader_integration`
с записью, передачи образа через USB-мост.

Следствия для P5:
- Реальный вывод C6 на USART3 — это только ROM: баннер `ESP-ROM:esp32c6-20220919`,
  `rst:0x1 (POWERON),boot:0xc` и бесконечный поток `invalid header: 0xffffffff`
  (запись — `zephyr_latest/cedar_switch_3in4out_power/docs/device-development/reports/board-b/logs/05-coprocessor-link-check.log`,
  28,9 КБ; файл не закоммичен и есть только в рабочей копии основного дерева —
  скопируй в фикстуры). Это и есть строка матрицы «binary UART ROM data», а
  непрерывный поток на 115200 — естественная нагрузка для wrap кольца и медленного
  клиента.
- Строк ESP-IDF (`I (412) tag: …`, ANSI-цвета, уровни, модуль) на плате B нет.
  Их разбор проверяется в sim на синтетических фикстурах; строку «реальный вывод
  C6 на 115200 с уровнями» оставь открытой на железе до P6.
- Сброс C6 через EN (normal boot) и чтение ответа ROM без записи — **спроси
  владельца в начале** (см. решения ниже). Сброс C6 при работающем STM32 в P0 давал
  BUS FAULT `w5500_rx`; патч должен это закрывать — проверь и запиши.
- Режим `flashing` в P5 — только передача владения UART фейковому «прошивальщику» в
  sim и, на плате, захват/возврат USART3 без обмена с ROM loader. Настоящая
  библиотека — P6.

## Решения владельца — спросить в самом начале

1. **База ветки.** P5 правит те же файлы, что незалитый P4 (routes.h,
   http_resources.h, system.c capabilities, фронтенд router/ru.ts, psram_sections.ld),
   а на плате B стоит образ P4. **Рекомендация:** worktree
   `git worktree add ../cedar_p5 -b p5-logs p4-network` (или от `main`, если владелец
   уже влил `p4-network`; CI P4 зелёный, run 34785218644). От `main` без P4 плата B
   откатится на образ до P4, слияние даст конфликты, и в worktree не будет стендовых
   скриптов платы B (reports/p4/hw есть только на `p4-network`). Ветку и worktree создавать
   только с разрешения; коммиты — только по просьбе владельца.
2. **Сброс C6 через EN** из тестов и из веба (normal boot, без записи) — разрешён?
3. **Как включается режим `usb_bridge`.** В контракте `uart_mode` только читается
   (`GET /coprocessor/status`), операции переключения нет. Варианты: команда шелла
   (отладка), автоматически по DTR на CDC, Kconfig-режим по умолчанию. Предложи, реши
   с владельцем; контракт без его решения не расширять.
4. **`GET /coprocessor/status` в P5 или в P6.** Рекомендация — P5: `uart_mode`,
   `generation`, `transport_ready`, `state` принадлежат coprocessor-manager; поля
   обновления — `available=false`, `reason="not_implemented"`, `last_update=null`.
5. **Ранние сообщения до старта кольца** («по возможности», раздел 7): делать ли
   захват до инициализации PSRAM-кольца или честно начинать с его старта.

До ответов делай ядра и sim — они от решений не зависят.

## Работа рядом с другими сессиями

- Основное дерево `cedar_switch_3in4out_power` занято P3 (незакоммиченные
  storage_bench.c, reports/p3/tuning и др.). Ничего в него не пиши; оттуда только
  читай (например, фикстуру reports/board-b/logs/05-…). Worktree `cedar_p4` —
  результат P4, в нём не работать.
- В worktree проверь сборку прошивки и sim: `tests/ci/run-sim-tests.sh` монтирует
  родителя приложения и берёт имя каталога через basename — worktree должен лежать
  в zephyr_latest рядом с остальными. `-T tests` берёт все сюиты каталога: новые сюиты
  с тегом sim подхватятся сами, аппаратные — не должны ломать discovery (см. раздел 12
  про `platform_allow`).
- Дерево zephyr общее: **не править** без решения владельца, патчи — только через
  `patches/` по patches/README.md; момент применения назначает владелец.
- Мутационное тестирование — только в копии в scratchpad, не в worktree.
- Сборки — только в scratchpad, не build/ в репозитории.

## Сначала прочти

- docs/device-development/development-plan.md: раздел 3 (модули `log-store`,
  `zephyr-log-source`, `esp32-log-source`, `coprocessor-manager`, правила интеграции:
  «UART RX callback один», «Zephyr log backend не пишет в HTTP и не логирует своё
  переполнение через тот же backend»), раздел 4 (экран «Логи», polling), раздел 7
  (Логи — бюджеты и правила), раздел 8 «Запись по UART» (владение USART3 и EN/BOOT),
  раздел 9 (Log page ≤100 records, ≤64 КиБ, 512 байт текста; журнал 512 КиБ PSRAM),
  раздел 10 строка P5, раздел 11 строка Logs, раздел 12 строки модулей.
- docs/device-development/api-contract.md: «Общие правила», «Логи» (cursor, gap,
  boot change, export), конфликт операций в «Upload и ESP32 update».
- openapi.json: `LogSource`, `LogSources`, `LogRecord` (`kind`: message/reset/paused/gap,
  `source_generation`, `truncated`), `LogPage`, операции `getLogSources`,
  `getLogRecords`, `exportLogs`, `getCoprocessorStatus`, `Capabilities.features.esp32_logs`,
  `esp32_uart`, `limits.log_page_records`.
- tools/api-contract/cedar_contract/mock/logs.py и state.py — поведение mock
  (cursor привязан к фильтрам и boot, `unknown`/null переживают `min_level`).
- modules/web-api/include/web_api/web_api.h и web_api_http.h, src/web/api/v1/network.c
  (как P4 отдаёт scan), system.c (boot_id, capabilities: сейчас `esp32_logs` и
  `esp32_uart` — `not_implemented`).
- Zephyr: drivers/serial/uart_bridge.c, include/zephyr/logging/log_backend.h,
  drivers/misc/esp_hosted_mcu/esp_hosted_mcu_spi.c (сброс C6 на init).
- src/plugin_wifi/{wifi.c,wifi_shell.c}, tests/coprocessor_link_check.

## Установленные факты, от которых зависит дизайн

1. **Ответ web-api — фиксированный буфер на клиента,
   `CONFIG_WEB_API_RESPONSE_BODY_MAX=16384`; не влезло → 500.** Страница из 100 записей
   по 512 байт с JSON-экранированием — до ~60 КБ, экспорт 2000 записей — ~1 МБ. Значит:
   (а) страница ограничивается и числом записей, и байтами (`has_more=true`,
   cursor на следующую непоказанную запись) — либо буфер увеличивается с обоснованием
   по памяти; (б) экспорт — потоковая отдача кусками из кольца под зафиксированной
   верхней seq, с явной gap-записью, если часть съедена кольцом, а не копия snapshot в
   RAM. **Сначала измерь**, умеет ли текущий адаптер web-api отдавать тело кусками,
   `text/plain`/`application/x-ndjson` и `Content-Disposition`, и сколько стоит
   генерация на единственном кооперативном потоке HTTP-сервера (он стоит, пока
   обработчик работает — P2/P4). Решение и цифры — в reports/p5 и README модуля.
2. **USART3 делят три потребителя.** Узел платы `uart-bridge0` (`zephyr,uart-bridge`,
   peers `cdc_acm_uart0` ↔ `usart3`) включён по DT (`CONFIG_UART_BRIDGE` default y) и на
   init забирает IRQ-callback USART3. Выключить мост на лету можно только
   `pm_device_action_run(SUSPEND)` — его PM-обработчик снимает callbacks и RX IRQ
   обоих портов; для этого нужен `CONFIG_PM_DEVICE` — в prj.conf и defconfig платы он
   явно не включён, сверь `.config` сборки. Альтернатива —
   `status = "disabled"` у `uart-bridge0` в overlay приложения
   (`boards/cedar_switch_3in4out_power_rev3*.overlay`) и собственная реализация
   passthrough в coprocessor-manager. Выбери, проверь на плате, запиши. Следствие PM:
   после RESUME мост ставит свой callback обратно — источник логов в этот момент
   обязан быть отключён. В P6 третьим придёт `tty_serial` из esp-serial-flasher.
3. **EN C6 (PA8) управляют трое.** `esp_hosted_mcu_spi.c` на init конфигурирует
   `reset-gpios = <&gpioa 8 GPIO_ACTIVE_HIGH>` и дёргает его; `gpio-leds` `wifi_reset`
   (тот же PA8, `GPIO_ACTIVE_LOW`) и `wifi_boot` (PC0) после этого приводит к неактивному
   уровню; `src/plugin_wifi/wifi.c` дёргает их из шелла. P5 переносит EN/BOOT под
   coprocessor-manager: опиши владение с учётом драйвера (драйвер не править), сохрани
   команды `wifi_ctrl` (отладочный код не удалять — перенаправь их через менеджер).
4. **Логирование сейчас:** `CONFIG_LOG_MODE_DEFERRED=y`, поток обработки приоритет 13,
   `CONFIG_LOG_BUFFER_SIZE=64384`, backends — shell на USART1 и telnet
   (`SHELL_BACKEND_TELNET=y`), `CONFIG_LOG_CMDS=y` (`log enable dbg <модуль>` — готовый
   генератор burst), таймстемпы backend выключены. Часов реального времени нет (SNTP
   закомментирован) → `wall_time=null`.
5. **boot_id** уже есть: `v1_identity()->boot_id` (system.c, network.c) — записи и
   cursor используют его же.
6. **Фейка UART в tests/fakes нет** (там fake_iface, fake_storage) — P5 добавляет.

## Задача P5

Критерий раздела 10: «Два источника, overflow и gaps, залипший браузер не блокирует
логгер, переключение владельца UART». Каждый модуль — чистое ядро + тонкий адаптер
(раздел 12 «Условие тестируемости»), README с жизненным циклом, потоками, владением и
ошибками.

1. **`log-store`** (`modules/log-store`): два кольца в PSRAM, 256 КиБ STM32 + 256 КиБ
   ESP32 (Kconfig, настроить по измерениям); глобальный seq, присваиваемый при захвате;
   запись: source, boot_id, source_generation, seq, uptime_ms, level/module nullable,
   текст ≤512 байт + truncated, kind; счётчики потерь раздельно: UART, backend,
   перезапись кольца. Чтение: без cursor — последние `limit` подходящих, от старых к
   новым; cursor opaque, связан с фильтрами и boot, указывает позицию сканирования
   даже без совпадений; wrap → `gap=true`; cursor прежнего boot → хвост, новый boot_id,
   gap; повреждённый/чужой cursor → 400 `invalid_cursor`. Фильтры не пересматривают
   пройденное. Производитель никогда не ждёт читателя: короткая критическая секция
   на запись, копирование для чтения — без блокировки producer (seqlock/поколение или
   копия под коротким lock — обоснуй и измерь). Экспорт — по пункту «Факты» 1.
2. **`zephyr-log-source`**: `LOG_BACKEND_DEFINE` в deferred-режиме → log-store; уровень и
   модуль из сообщения, текст без таймстемпа; счётчик отброшенного; никакой рекурсии
   (свои ошибки — счётчиками, не `LOG_*`); без flash I/O и долгого форматирования.
   Существующие backends (shell/UART, telnet) не ломать.
3. **`esp32-log-source`**: RX USART3 (interrupt-driven или async — выбери по тому, что
   совместимо с мостом и P6), в ISR только копирование байтов в кольцевой буфер; сборка
   строк на потоке: 512 байт + truncated, сброс частичной строки по timeout, ANSI strip,
   некорректный UTF-8 → U+FFFD, бинарные байты ROM не ломают разбор; разбор уровня и
   модуля ESP-IDF (`E/W/I/D/V (ms) tag:`), иначе level `unknown`/null; маркеры `reset`
   (сброс C6, новая generation) и `paused` (UART отдан); переполнение RX — счётчик.
4. **`coprocessor-manager`** (ядро владения UART и EN/BOOT, без updater): автомат
   `console ↔ usb_bridge ↔ flashing ↔ unavailable`, единственный владелец USART3;
   переключение: остановить текущего владельца, дождаться остановки RX (с таймаутом и
   явной ошибкой «отказ остановки RX»), передать; взаимоисключение с network apply/scan
   и будущим update (раздел 3); `generation` растёт на каждом сбросе C6 и возврате UART;
   `transport_ready` — из ESP-Hosted. Залипший USB-хост (CDC никто не читает) не должен
   останавливать источник логов или систему.
5. **HTTP API v1:** `GET /logs/sources`, `GET /logs/records`, `GET /logs/export`
   (NDJSON и text, `Content-Disposition`), `GET /coprocessor/status` (если владелец
   согласен, см. решения); capabilities `esp32_logs`/`esp32_uart` — из реального
   состояния, `limits.log_page_records` — фактический. **`esp32_logs.available`
   следует владению UART** (`uart_mode=console`, RX работает), `reason` называет
   настоящую причину (UART занят мостом/прошивкой и т. п.); готовность ESP-Hosted —
   это `CoprocessorStatus.transport_ready`, не доступность логов. Mock сейчас
   (`state.py:99`) связывает `esp32_logs` с транспортом — на плате B транспорт не
   поднимется никогда, а ROM-поток реален; mock выровнять под устройство. Маршруты в routes.h и
   http_resources.h; контрактная проверка маршрутов должна пройти.
6. **Экран «Логи»** (`src/web/frontend/src/features/logs`, раздел 4): источник
   STM32/ESP32/оба, уровень, модуль, текст; пауза автопрокрутки; экспорт; dropped/gap
   видны; boot_id/смена загрузки; polling 500–1000 мс, следующий запрос только после
   завершения предыдущего, в фоне реже, отмена при уходе; строки — текст, не HTML;
   строки UI — в словарь ru.ts. Экран ESP32 — P6, но статус `uart_mode` можно показать.
7. **Mock** подтянуть к устройству там, где устройство строже (как в P4), README mock —
   таблица правил. `psram_sections.ld` — новые библиотеки (проверь по map, что кольца и
   данные модулей действительно в PSRAM).

## Тесты

- **sim** (строки раздела 12): log-store — wrap, gap, устаревший cursor, смена boot,
  фильтры, глобальный seq, байтовый лимит страницы, bounded export, медленный читатель;
  zephyr-log-source — форматирование, учёт потерь, отсутствие рекурсии;
  esp32-log-source — фрагменты, timeout частичной строки, ANSI, невалидный UTF-8,
  реальный ROM-поток из фикстуры, уровни ESP-IDF; coprocessor-manager — автомат
  владения, отказ остановки RX, взаимоисключения; web_api — 4 операции, коды ошибок,
  сверка со схемой; web_api_http — большая страница и экспорт на настоящем сервере.
- **Контракт:** 5 проверок + mock pytest; число обслуживаемых операций вырастет с 24.
- **Фронтенд:** vitest на фильтры/cursor/паузу/gap, компонентные на фикстурах OpenAPI,
  e2e против mock; `device.spec.ts` против платы B (`E2E_DEVICE_URL=http://192.168.88.13`).
- **Плата B** (логи в reports/p5/hw/logs, в каждом — sha256 образа):
  - два источника одновременно: STM32 (обычный лог + burst через `log enable dbg`
    по telnet) и ESP32 (ROM-поток после сброса C6, если владелец разрешил сброс);
  - wrap кольца, `gap`, `dropped_count`, устаревший cursor, cursor через сброс STM32
    (новый boot_id);
  - медленный/зависший клиент (читает раз в минуту, обрыв посреди экспорта) — логгер,
    шелл, Matter и сеть живут; время остановки HTTP-потока на странице и экспорте;
  - экспорт максимального размера: время, память, куски;
  - переключение `console → usb_bridge → console` с наблюдением со стороны хоста через
    CDC платы B (порт по серийнику 3543501200210047), хост не читает CDC — система жива;
    `console → flashing(заглушка) → console` — маркеры `paused`/`reset`, новая generation;
  - сброс C6 при работающем STM32 — нет BUS FAULT (патч W5500);
  - запас стеков и памяти новых потоков (как в P4 — из консоли).
- **Мутации** нового кода — в копии в scratchpad, docker, выжившие закрыть тестами или
  обосновать эквивалентность.

## Как проверять

    colima start --cpu 4 --memory 8 --disk 60 --mount /Volumes/Programming:w
    tests/ci/run-sim-tests.sh
    tests/ci/run-contract-tests.sh
    tests/ci/run-frontend-tests.sh            # E2E_DEVICE_URL=http://192.168.88.13 — только плата B

Сборка (сначала фронтенд: `cd src/web/frontend && npm ci && npm run build`), каталог
сборки — только scratchpad:

    cd /Volumes/Programming/Zephyr/zephyr_latest
    .venv/bin/west build -p always -b cedar_switch_3in4out_power_rev3/stm32u585xx/ext_flash_app \
      --sysbuild -d <scratchpad>/build cedar_p5

(имя образа sysbuild = имя каталога: `-Dcedar_p5_EXTRA_CONF_FILE`, если понадобится.)

## Как работать

- Сначала публичный интерфейс с обоснованием, потом реализация, потом тесты — всё
  вместе (раздел 12). Набор, прошедший с первого раза, ничего не доказывает — мутации.
- Не полагайся на документацию без проверки; важное — измеряй. Перед починкой убедись,
  что дефект настоящий (устаревший бинарник, голодание потока W5500, потерянные байты
  консоли уже вводили в заблуждение).
- **Записывай результаты исследований в reports/p5 сразу по мере получения**, а не в
  конце: сессии обрываются, выводы в чате и scratchpad теряются.
- Команды шелла по UART — с ожиданием подтверждения и повтором: консоль теряет байты.
- **Не правь вендорский Matter** (modules/lib/matter) и **дерево zephyr** без решения
  владельца (патчи — через patches/).
- **Не убирай отладочный код** (команды шелла `wifi_ctrl`, Matter shell, storage_bench,
  отладочный вывод) — перенаправляй, а не удаляй.
- **Не меняй бэкенд settings** — решается в другом треде (логи его не используют).
- **Коммиты не делай сам.** Доведи работу, прогони тесты, оставь всё в worktree и скажи,
  что во что складывается.
- Когда закончишь — обнови строку P5 в разделе 10, строку Logs в разделе 11, строки
  `log-store`, `zephyr-log-source`, `esp32-log-source`, `coprocessor-manager` в разделах 3
  и 12, решения владельца — в раздел 13; отчёт — docs/device-development/reports/p5/
  (README, hw/README, логи с образом). Незакрытое из-за пустого C6 перечисли явно.
```
