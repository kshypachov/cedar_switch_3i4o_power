# Промт: полное выполнение этапа P4 — Сеть (плата на J-Link)

```
Выполняем этап P4 — Сеть. Рабочий каталог —
/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power

## ⚠️ Две платы. Работаем ТОЛЬКО с платой B на J-Link

К компьютеру подключены две одинаковые платы cedar_switch_3in4out_power_rev3.

**Плата A — на ST-Link — занята другой сессией (незакрытые задачи P3, Matter).
Её тесты прерывать нельзя. Не трогать никак:**
- не прошивать, не сбрасывать, не открывать её порты, не входить в её веб,
  не слать ей запросы API, chip-tool, ping-флуд, e2e;
- ST-Link V3, серийник 002F002B3233510739363634; порты /dev/cu.usbmodem21103,
  /dev/cu.usbmodem21301, USB CDC с серийником 2037394C3543501200550045
  (номера портов меняются — сверяй по серийнику);
- сеть: 192.168.88.14, MAC 80:34:28:10:12:73, fe80::8234:28ff:fe10:1273,
  web-id cedar-2037394c3543501200550045;
- **STM32_Programmer_CLI не запускать вообще** — он работает только через ST-Link;
- `tests/bench/*.py` по умолчанию ищут консоль по серийнику ST-Link платы A —
  без явного порта платы B их не запускать;
- **никогда не назначать плате B адрес 192.168.88.14** и вообще адреса, занятые
  другими узлами сети: статический IP проверяй на свободность (arping/ping)
  до apply. Конфликт адреса сломает тесты P3.
- **Скрипты стенда зашиты на плату A** (IP 192.168.88.14, серийник ST-Link,
  STM32_Programmer_CLI, консоль платы A). Запуск без правки войдёт в веб платы A,
  перезагрузит её или сбросит её fabric. Как есть **не запускать**:
  reports/p3/hw/*.py (matter_api, login_probe, probes, stall_capture,
  telnet_shell, setup_admin, flash_and_boot), reports/p3/settings-backends/run_backend.py,
  reports/p3/lfs-cache/compare.py, reports/p3/tuning/run_trial.py,
  reports/p2/hw/*.py (latency, diag, kdf_timing, kdf_board, api_bench,
  reboot_check), tests/bench/{smoke,boot_cycle,bench_console}.py.
  Нужен скрипт — копируй в reports/p4/hw/, меняй IP/серийник/порт/прошивальщик и
  перед первым запуском проверь:
  `grep -n "192.168.88.14\|002F002B\|STM32_Programmer_CLI\|21103\|21301" <скрипт>` — пусто.
- **docs/device-development/NEXT-P3-SESSION.md** (из другой сессии) пишет, что P3
  идёт на плате с J-Link. Это устарело: сейчас владелец назначил плату A (ST-Link) —
  P3, плату B (J-Link) — P4. Раздел о платах в том файле не действует.

**Плата B — на J-Link — наша:**
- J-Link V11, серийник 000941000024, JLinkExe V9.76 (/usr/local/bin), устройство
  STM32U585AI. `-USB 000941000024` в каждой команде JLinkExe;
- консоль (USART1): мост WCH, называет себя «ASIX», серийник 5AE6020889,
  /dev/cu.usbmodem5AE60208891. VCP самого J-Link (…0009410000241) — не консоль;
- USB CDC платы (hwinfo): 3543501200210047;
- MAC 80:34:28:10:6a:1d, DHCP 192.168.88.13, web-id
  cedar-2037394c3543501200210047;
- Matter-идентичность у плат общая (PIN 20202021, discriminator 0xF00): любые
  Matter-действия — только по IP платы B.

Всё о плате B — docs/device-development/reports/board-b/README.md и память
проекта (board-b-jlink-bench, sysbuild-mcuboot-extra-conf-replaces). Прочти до
первой команды к плате.

### Как прошивается плата B

J-Link не пишет внешнюю QSPI этой платы. Во внутренней флеш стоит стендовый
MCUboot с recovery по USB CDC (5 с ожидания на каждом старте — учитывай в
замерах времени). Образ приложения:

    docs/device-development/reports/board-b/scripts/run_test_image_b.py <workdir> <zephyr.signed.bin> <сек прослушивания>

(сброс через J-Link → CDC по серийнику 3543501200210047 → `~/go/bin/mcumgr image upload`).
1,23 МиБ грузится ~5,5 мин — копи изменения, не прошивай на каждую мелочь.
Поправь пути в скриптах board-b, если они указывают на scratchpad прошлой сессии.
run_test_image_b.py импортирует `resolve_port` из tests/bench по абсолютному пути
основного дерева (не worktree) — порт платы B он передаёт явно, это нормально.
Если нужен другой MCUboot: `-Dmcuboot_EXTRA_CONF_FILE` обязательно начинается с
`<repo>/sysbuild/mcuboot.conf;` — иначе MCUboot падает на переходе в приложение;
перед записью проверь в его .config `BOOT_GO_HOOKS=y` и отсутствие `BOOT_DISABLE_CACHES`.

## ⛔ Блокеры, которые надо снять до аппаратной части (решает владелец)

1. **BUS FAULT в `w5500_rx` на плате B — в каждом наблюдавшемся старте** (8,9 с и
   позже 15 с), система останавливается, в сети плата не отвечает. Причина —
   драйвер не ограничивает длину кадра из заголовка W5500 (P0,
   reports/p0/fault-c6-reset/; исправление в upstream PR #115626). Владелец
   ранее принял это как риск до P8, но на плате B без исправления P4 на
   железе невозможен. Исправление — это правка общего дерева zephyr, из
   которого собирает и сессия P3 для платы A. **Не правь дерево zephyr сам.**
   Сначала подтверди отказ на текущей сборке (лог в reports/p4), затем принеси
   владельцу варианты: патч в `patches/zephyr/` по процедуре patches/README.md с
   условием удаления, или другое решение. Применение патча меняет дерево, из
   которого прямо сейчас собирает сессия P3, — **момент применения назначает
   владелец**, не сессия P4. До ответа делай sim, фронтенд и код адаптеров.
2. **ESP32-C6 на плате B не прошит, и прошивать его нельзя** (решение владельца:
   первая прошивка C6 на плате B должна прийти через веб, это приёмка P6).
   Значит Wi-Fi на плате B не работает: scan, connect, WPA2/WPA3/open, скрытый
   SSID, reconnect, Wi-Fi-only и «достижимость Matter при Wi-Fi-only» на железе
   проверить нельзя. Не обходи это (esptool, esp_loader-тесты с записью).
   Wi-Fi-адаптер и всё Wi-Fi делай и проверяй в sim/на моке; на плате проверь
   корректное поведение при недоступном C6 (capabilities, статус, отказ scan и
   apply Wi-Fi с понятной ошибкой). Строки критерия, требующие живого Wi-Fi,
   оставь открытыми и спроси владельца, как их закрывать (ждать P6, плата A
   после P3, иное).
3. **Бэкенд настроек не выбран** (исследуется в P3, reports/p3/STATUS.md): файловый
   бэкенд медленный. P4 пишет pending/committed конфигурацию через
   device-config-store. Не меняй бэкенд settings в P4; замеряй время записи и
   apply и отмечай, какой бэкенд был в образе.
4. **Физические действия** (снятие кабеля, пропадание питания, пять циклов
   питания для восстановления конфигурации) — только руками владельца. Готовь
   сценарий, проси его выполнить, записывай по времени консоли.

## Работа рядом с сессией P3 в одном репозитории

Сессия P3 правит файлы и собирает прошивки в этом же рабочем дереве (сейчас там
её незакоммиченные изменения: src/diagnostic/storage_bench.c,
reports/p3/tuning/). Правки одного каталога двумя сессиями и мутационное
тестирование, временно портящее исходники, ломают чужие сборки (в P3 так
прошивка собралась с мутированным кодом).

- Предпочтительно работать в отдельном git worktree внутри west workspace, от
  main, например `git worktree add ../cedar_p4 -b p4-network main` — **только
  если владелец разрешил создать ветку и worktree** (спроси первым делом; коммиты
  в любом случае не делать). Сначала проверь, что в worktree собираются прошивка
  и sim (`tests/ci/run-sim-tests.sh` монтирует родительский каталог приложения и
  берёт имя каталога через basename — worktree должен лежать рядом с
  cedar_switch_3in4out_power в zephyr_latest; sysbuild-префикс образа
  `-D<имя-каталога>_EXTRA_CONF_FILE` станет именем worktree).
  Если worktree не разрешён или не взлетает — работай в общем дереве, не трогая
  файлы P3 (src/matter, modules/matter-service, storage_bench, reports/p3), и
  мутационные прогоны делай только в отдельной копии.
- Общие файлы (prj.conf, CMakeLists.txt, routes.h, http_resources.h, system.c,
  фронтенд router/Layout/ru.ts) перечитывай перед каждой правкой, чужие
  изменения не откатывай.
- Не запускай сборки из каталога, где в этот момент идут мутации.

## Сначала прочти

- docs/device-development/development-plan.md: раздел 5 (сеть и применение),
  раздел 3 (network-manager, правила интеграции), раздел 4 (экран «Сеть»),
  раздел 10 (строка P4), раздел 11 (Ethernet, Wi-Fi, Apply), раздел 12
  (network-manager), раздел 13 (решения владельца)
- docs/device-development/api-contract.md — «Сеть»; openapi.json — 9 операций
  getNetworkStatus, getNetworkConfig, stageNetworkConfig, getNetworkTransaction,
  rollbackNetworkTransaction, applyNetworkTransaction, confirmNetworkTransaction,
  scanWiFi, getWiFiScan и их схемы
- modules/network-manager/README.md и заголовок (`struct network_iface_ops`,
  разделы The adapter, Not covered in sim), modules/device-config-store/README.md
- modules/web-api/README.md (конвейер, измеренное поведение сервера Zephyr),
  src/web/api/v1 (образец: auth.c — задача через job-manager; matter.c —
  проверки до создания задачи, снимок без ожидания)
- docs/device-development/reports/p2/README.md, reports/p3/STATUS.md
- src/lib-init/net_init.c, src/plugin_wifi/, драйвер
  zephyr/drivers/wifi/esp_hosted_mcu (DHCP по `CONFIG_WIFI_STA_AUTO_DHCPV4`)
- tools/api-contract/README.md — mock уже реализует сетевые транзакции и scan;
  src/web/frontend/README.md

## Задача P4

Раздел 10: «Ethernet/Wi-Fi adapters, scan, reconnect, DHCP/static/DNS, откат
транзакции, web-экран». Критерий: «Все сценарии apply/confirm/timeout/power-loss,
достижимость Matter при Wi-Fi-only». Ядро (`network-manager`: транзакция,
валидация, таймер, health policy) сделано в P1 и проверено только в sim.

1. **Адаптеры `network_iface_ops`** в приложении (src/services/ или рядом с
   net_init): Ethernet на W5500 (net_if, DHCPv4 start/stop, статический адрес,
   маршрут, link) и Wi-Fi на esp_hosted_mcu (connect/disconnect, scan, статус).
   Адаптер не принимает политических решений и не стартует DHCP сам; перенести
   сюда DHCP из net_init.c и выключить автоматический DHCP драйвера
   (`CONFIG_WIFI_STA_AUTO_DHCPV4=n`, проверь в Kconfig этого дерева). Stable IDs
   `ethernet`/`wifi`, не индекс net_if. Callbacks net_mgmt только копируют событие
   и сразу возвращаются; работа — в network worker.
2. **Жизненный цикл**: первый старт без конфигурации — Ethernet DHCP; старт с
   pending/незакоммиченной транзакцией — откат к последней committed; события
   адресов не пересоздают Matter Server и не стирают fabrics (раздел 3).
3. **DNS**: automatic от выбранного маршрута или manual 1–2 сервера, согласовать с
   resolver (сейчас «DHCP server provided more DNS servers than can be saved»).
   Политика Ethernet preferred, Wi-Fi fallback; health по link/address/route, без
   публичного ping. Два интерфейса в одной подсети — исследовать на железе до
   правила (здесь — только Ethernet, отметь как открытое).
4. **HTTP API v1**: 9 сетевых операций по контракту через job-manager: проверки
   до создания задачи, повтор с тем же Idempotency-Key, 409 при чужой revision,
   секреты не отражаются (`password_set`), reconnect hints без паролей и токенов.
   Ответ 202 уходит до применения сети. Строки routes.h и ресурсы
   http_resources.h — пятая контрактная проверка должна пройти.
5. **Wi-Fi scan**: задача, до 64 AP, `truncated`, BSSID, `ssid_base64`,
   security, `connect_supported`; запрет scan во время apply/прошивки C6.
6. **Web-экран «Сеть»** (раздел 4): отдельные формы Ethernet/Wi-Fi, список AP с
   RSSI/security, скрытый SSID, пароль write-only, IPv4 DHCP/static, DNS, текущий
   и будущий адрес, подтверждение/откат с таймером по данным сервера, переход на
   новый адрес (cookie не переносятся — повторный вход и подтверждение известной
   транзакции). Строки в словарь, запросы через общий планировщик, SSID — текстом.
   Писать и гонять e2e против mock до устройства.
7. **Capabilities**: wifi_security_modes и сетевые лимиты — из прошивки и
   состояния C6, не константы.
8. **Физическое восстановление сетевой конфигурации** с сохранением fabrics —
   решение владельца «пять циклов питания подряд» (раздел 13): реализовать и
   проверить с владельцем.

## Тесты

- sim: адаптеры на фейковом net_if/драйвере где возможно; сетевые v1-привязки в
  tests/web_api (как test_matter_*); интеграция network-manager + адаптеры на
  фейках; все сценарии Apply из раздела 11 (два клиента с разными revision,
  retry тем же ключом, смена обоих интерфейсов, confirm по новому IP, timeout
  rollback, reboot в каждом состоянии, ошибка записи settings).
- Контракт: mock и устройство совпадают по правилам; `undocumented_routes` зелёная.
- Фронтенд: vitest + e2e против mock (формы, скан, apply→confirm, timeout, откат,
  смена адреса).
- Плата B (после снятия блокера 1): DHCP↔static на W5500, неверные маска/шлюз,
  DNS, снятие кабеля и возврат (владелец), confirm по новому IP, timeout rollback
  (120 с), reboot и пропадание питания в каждом состоянии транзакции (владелец),
  IPv6/mDNS после смены IPv4 (Matter виден по `_matter._tcp` через dns-sd по IP
  платы B), e2e экрана против платы B. Wi-Fi — см. блокер 2.
- Мутационное тестирование нового кода (не в общем дереве, см. выше).

## Как проверять

    colima start --cpu 4 --memory 8 --disk 60 --mount /Volumes/Programming:w
    tests/ci/run-sim-tests.sh                 # весь sim
    tests/ci/run-contract-tests.sh
    tests/ci/run-frontend-tests.sh            # E2E_DEVICE_URL=http://192.168.88.13 — только плата B

Сборка (сначала фронтенд: `cd src/web/frontend && npm ci && npm run build`),
каталог сборки — только scratchpad, не build/ в репозитории:

    cd /Volumes/Programming/Zephyr/zephyr_latest
    .venv/bin/west build -p always -b cedar_switch_3in4out_power_rev3/stm32u585xx/ext_flash_app \
      --sysbuild -d <scratchpad>/build cedar_switch_3in4out_power

Плата B, по последним данным, в режиме setup (администратор не создан), состояние
её раздела настроек неизвестно. Пароль платы A из памяти к ней не относится.
Первым запросом проверь `GET http://192.168.88.13/api/v1/auth/state` (если плата
вообще в сети — см. блокер 1). Для создания администратора — копия
reports/p3/hw/setup_admin.py в reports/p4/hw/ с `HOST` (константа в файле),
заменённым на 192.168.88.13; пароль запиши в отчёт P4.

## Как работать

- Сначала публичный интерфейс с обоснованием, потом реализация, потом тесты — всё
  вместе (раздел 12). Набор, прошедший с первого раза, ничего не доказывает —
  проверяй мутациями.
- Не полагайся на документацию без проверки; важное — измеряй. Перед починкой
  убедись, что дефект настоящий (устаревший бинарник, голодание потока W5500,
  потерянные байты консоли уже вводили в заблуждение).
- Команды шелла по UART отправляй с ожиданием подтверждения и повтором: консоль
  теряет входные байты.
- **Не правь вендорский Matter** (modules/lib/matter) и **не правь дерево zephyr**
  без решения владельца (патчи — только через patches/).
- **Не убирай отладочный код** (команды шелла, storage_bench, отладочный вывод).
- **Коммиты не делай сам.** Доведи работу, прогони тесты, оставь всё в рабочем
  дереве (или worktree) и скажи, что во что складывается.
- Когда закончишь — обнови строку P4 в разделе 10, строки Ethernet/Wi-Fi/Apply в
  разделе 11 и network-manager в разделе 12; отчёт — docs/device-development/reports/p4/
  (логи платы B с указанием образа и бэкенда settings). Незакрытое из-за блокеров
  перечисли явно.
```
