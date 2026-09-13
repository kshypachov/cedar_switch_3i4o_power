Начинаем этап P3 — Matter. Рабочий каталог —
/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power

Сначала прочти:
- docs/device-development/development-plan.md — план; раздел 6 (Matter),
  раздел 3 (модуль `matter-service`), раздел 10 (этапы), раздел 11 (строка
  Matter матрицы приёмки), раздел 12 (строка `matter-service`), раздел 13
  (решения владельца — там новые, из P2)
- docs/device-development/reports/p2/README.md — итог P2: что измерено на
  плате, что осталось открытым
- docs/device-development/api-contract.md — разделы «Задачи» и «Matter»
- docs/device-development/openapi.json — схемы `MatterStatus`,
  `CommissioningRequest`, `CommissioningWindow`, `OnboardingCodes`, `Fabric`,
  `Capabilities`
- modules/web-api/README.md — конвейер запросов и то, что измерено у
  HTTP-сервера Zephyr; от этого зависит форма Matter-обработчиков
- modules/web-auth/README.md — раздел о потоках: как долгую работу вынесли с
  потока HTTP-сервера
- src/web/frontend/README.md — стек, планировщик опросов, словарь
- tools/api-contract/README.md — mock, его автомат Matter и общие правила JSON
- tests/ci/README.md — три раннера и CI

Память проекта подхватится сама.

## Состояние на 2026-09-13

| Этап | Статус |
|---|---|
| P0. Стабильная основа | 🟡 не закрыт: владелец пропустил проверку существующего I/O; CI с платой (`BOARD_ROOT`) не сделан |
| P1. Сервисы и контракт | ✅ закрыт |
| P2. Web shell и доступ | ✅ закрыт — локально, на плате и в CI на GitHub (run 34745868566) |

### Что сделал P2

- **`modules/web-auth`** — один администратор, PBKDF2 (3000 итераций,
  подобрано измерением), сессии с CSRF, два ограничения попыток, атомарный
  setup, смена пароля как задача. Setup-token показывается в веб-интерфейсе.
- **`modules/web-api`** — ядро без сокетов (запрос → ответ), строгий JSON,
  проверка Host, адаптер к HTTP-серверу Zephyr. Сервис один, на `[::]:80`,
  IPv4 через mapping.
- **`modules/web-assets`** и `tools/web-assets` — фронтенд вшит в прошивку
  таблицей: MIME, ETag, gzip, возврат страницы для путей приложения.
- **`src/web/api/v1`** — 9 из 35 операций: auth, system status,
  capabilities, jobs. Matter там нет.
- **`src/web/frontend`** — React 19, TypeScript, Vite; экраны setup, входа,
  доступа и обзора; строки в одном русском словаре.
- **Весь legacy HTTP удалён** (решение владельца): старые `/api/*`,
  `/api/matter/control`, сервис на 8080. URL отвечают JSON 404.
- Пятая контрактная проверка работает: `routes.h` сверяется с документом.

Тесты: sim 277, контракт 5/5 проверок + 321 + 14 pytest, фронтенд 109 vitest
+ 5 e2e против mock + 1 против платы. Мутации: C 63 (62 пойманы,
1 эквивалентная), фронтенд и Python 24 (23 и 1).

На плате образ P2 (`a77dee0c…`): интерфейс поднимается через 2,9 с после
старта, вход 1,1 с, 2 клиента p95 157 мс, 4 клиента p95 313 мс (цель 250 —
открыто).

## Что P2 оставил для P3

Это прямо ложится на P3:

- **Поток HTTP-сервера Zephyr кооперативный и единственный, отложенного ответа
  нет.** Обработчик, который ждёт, останавливает всю систему: PBKDF2 в нём
  замораживал плату больше чем на 18 с. Matter-обработчики не должны ни ждать
  CHIP lock, ни ждать `ScheduleWork`. Открытие и закрытие окна — задачи
  (`JOB_KIND_MATTER_OPEN`/`CLOSE` уже есть в `job-manager`), GET — из снимка,
  который собирает поток Matter. Образец — смена пароля в
  `src/web/api/v1/auth.c`: `job_find_by_key` → проверка → `job_create` →
  work queue.
- **Одна операция — одна строка в `routes.h` и один ресурс в
  `http_resources.h`.** Zephyr отдаёт голый 409 второму клиенту на тот же
  ресурс, пока первый шлёт body; фронтенд это повторяет. Порядок имён ресурсов
  — порядок сопоставления wildcard. Контрактная проверка `undocumented_routes`
  сверит флаги маршрута с документом (public, CSRF, Idempotency-Key, body,
  query).
- **Capabilities.** `system.c` отвечает `matter: available=false,
  not_implemented`, а `commissioning_min/max_seconds` — проектные 180/900 с
  пометкой «SDK limits not yet read». В P3 заменить реальными значениями.
- **Модуль приложения в `modules/` попадёт в SRAM**, если не добавить его
  архив в `src/helpers/psram_sections.ld`. Стек потока, который должен жить в
  SRAM, туда не класть (так сделано с `web-auth`). Проверять по map.
- **Прошивка не собирается без собранного фронтенда.** Нужно
  `cd src/web/frontend && npm ci && npm run build` или
  `-DAPP_WEB_UI_PLACEHOLDER=ON` (под sysbuild —
  `-Dcedar_switch_3in4out_power_APP_WEB_UI_PLACEHOLDER=ON`).
- **Mock уже изображает Matter**: все шесть операций, автомат окна
  (`MATTER_OPEN_MS`, `MATTER_CLOSE_MS`), fabrics. Экран Matter можно писать и
  гонять e2e до устройства.

## Что известно о текущем Matter в коде

Прочитано в коде, на плате в P2 не проверялось:

- Matter SDK — `modules/lib/matter`, `v1.6.0.0` (`250a9e6c50`, 2026-08-19).
  **Дерево не чистое**: четыре изменённых файла, нигде не записанных в
  `patches/` — `config/zephyr/chip-module/CMakeLists.txt`,
  `src/inet/TCPEndPointImplSockets.cpp`,
  `src/platform/Zephyr/InetPlatformConfig.h`, `src/transport/raw/TCP.cpp`. По
  комментариям в коде это правки проекта Atios от 2026-08-30 (ветка RISC-V в
  chip-module и исправления TCP в порте Zephyr). Зависит ли от них сборка
  Cedar — не проверено. Не чистить; куда их деть — вопрос владельцу.
- `start_matter()` — callback `NET_EVENT_IPV6_ADDR_ADD` (`src/lib-init/net_init.c`)
  и в нём `k_msleep(5000)`, то есть поток net_mgmt спит 5 с. Раздел 2 плана
  требует убрать задержку.
- `NetworkCommissioningCluster.cpp` всегда сообщает Ethernet `connected=true`.
- DAC — `Examples::GetExampleDACProvider()`, тестовые credentials. Для
  разработки допустимо (раздел 6); продуктовый provisioning — отдельный этап.
- Окно commissioning на старте не открывается (код закомментирован).
  `src/matter/shell.cpp` — **отладочные команды**: открыть basic window с
  печатью QR и ручного кода в лог, показать число fabrics, `DeleteAllFabrics`.
  **Отладочный код не убирать** — ни эти команды, ни закомментированный вывод
  кодов в `matter_init.cpp` (решение владельца 2026-09-13).
- В каждом старте P0 и P2: `DNS-SD advertising not available`,
  `Failed to initialize advertiser: 2d`. В `prj.conf` закомментирован
  `CONFIG_MDNS_RESPONDER`. Без advertising discovery и commissioning не
  заработают; причину не разбирали.
- Настройки Matter лежат в том же `/lfs/settings`, что и реестр. Файл растёт с
  каждым стартом Matter; Matter init в серии P0 рос с 20,5 до 29,9 с
  (находка 8 отчёта P0).
- `CHIP_DEVICE_CONFIG_DISCOVERY_TIMEOUT_SECS` в SDK по умолчанию 15 минут —
  совпадает с верхней границей контракта 900. Нижнюю границу SDK не искали.
- В дереве Matter есть собранный `out/chip-tool/chip-tool` (2026-08-30);
  работает ли — не проверялось.
- Сколько fabrics сейчас на стендовой плате, неизвестно: проверка I/O и
  fabrics после перезагрузки в P0 пропущена.

## Задача P3

Критерий из раздела 10: «Commissioning на двух независимых fabrics,
окно/timeout/close, сохранение после перезагрузки». Контроллеры по решению
владельца: Apple Home в эксплуатации, chip-tool для тестов.

1. **`matter-service`** (раздел 3): состояние стека, окно commissioning,
   onboarding codes, снимки FabricTable. Без HTTP и HTML, без управления Wi-Fi.
   Вызовы в Matter — на его потоке через `ScheduleWork`, снимки собираются
   там же, CHIP lock не держится во время HTTP, flash или сетевого I/O.
   Платформу — функциями, как у `web-auth`, чтобы логика гонялась в sim над
   фейковой FabricTable.
2. **Lifecycle** (раздел 2): убрать сон из callback, привязать состояние к
   реальным интерфейсам, `state` = `not_ready`/`starting`/`ready`/`failed`
   по факту. Mutations при `not_ready` — 503.
3. **Discovery**: разобраться с `DNS-SD advertising not available`, проверить
   mDNS по IPv6 на Ethernet. Wi-Fi-only — строка P4, не P3. Выбор реализации
   mDNS (`chip_mdns`) задаётся в `config/zephyr/chip-module/CMakeLists.txt`
   вендорского дерева — сначала ищи путь через Kconfig приложения и
   `src/CHIPProjectConfig.h`.
4. **Окно**: только `basic` из веба; повтор ключа возвращает исходную задачу;
   новый запрос при открытом окне — `409 invalid_state`; закрытие закрытого
   окна идемпотентно; окно, открытое контроллером (enhanced), видно как
   `source=controller`, `codes_available=false`. Ограничения basic window на
   уже commissioned устройстве проверить на закреплённом SDK.
5. **Коды**: QR строит SDK, картинку — браузер; ручной код и PIN — разные поля,
   ведущие нули сохраняются; при закрытом окне все три поля `null`;
   заводские коды вместо действующих не показывать. Сервис и API коды в лог не
   пишут; отладочные команды шелла, которые их печатают, остаются.
6. **Fabrics** из FabricTable: `id` включает root identity, `fabric_index`,
   `fabric_id`/`node_id` hex, `vendor_id`, `label`. Название экосистемы по
   vendor ID не угадывать. Только показ, удаления нет (решение владельца).
7. **Экран Matter** (раздел 4): статус, открыть/закрыть окно, таймер по данным
   сервера, QR, ручной код и PIN, таблица fabrics; empty/loading/error/
   unavailable. Строки — в словарь, запросы — через общий планировщик.
8. **Capabilities**: `matter` доступен по факту, пределы окна — из SDK.

Тесты по разделу 12: sim — логика над фейковой FabricTable (открытие, таймаут,
повторное открытие, формат кодов, ведущие нули, `codes_available=false`);
плата — commissioning на двух fabrics, mDNS, сохранение после перезагрузки.
Строка Matter матрицы раздела 11: пустая FabricTable, две fabrics, reboot,
отказ SDK, окно истекло/закрылось, внешнее enhanced окно без известного PIN,
ведущие нули, unavailable до init.

## Как проверять свою работу

Sim-уровень (Linux-контейнер, native_sim не собирается на macOS):

    colima start --cpu 4 --memory 8 --disk 60 --mount /Volumes/Programming:w
    tests/ci/run-sim-tests.sh                        # весь набор, сейчас 277
    tests/ci/run-sim-tests.sh -s cedar.web_api       # одна сюита

Twister не перезаписывает каталог вывода, а добавляет суффикс — не оставляй
за собой .twister-tmp.N.

Контракт и mock — обычный Python, без контейнера:

    tests/ci/run-contract-tests.sh                   # 5 проверок, 321 + 14 pytest

Фронтенд:

    tests/ci/run-frontend-tests.sh                   # npm ci, vitest, build, e2e
    SKIP_E2E=1 tests/ci/run-frontend-tests.sh

E2E против платы (в `src/web/frontend`, гоняет только `e2e/device.spec.ts`):

    E2E_DEVICE_URL=http://192.168.88.14 E2E_DEVICE_PASSWORD=cedar-bench-P2-changed npx playwright test

Сборка прошивки (сначала собери фронтенд):

    cd /Volumes/Programming/Zephyr/zephyr_latest
    .venv/bin/west build -p always -b cedar_switch_3in4out_power_rev3/stm32u585xx/ext_flash_app \
      --sysbuild -d <build-dir> cedar_switch_3in4out_power

Не используй каталог build/ в репозитории: CLion перегенерирует его
параллельно, и получаются испорченные артефакты. Бери отдельный каталог в
scratchpad.

## Как работать

Набор тестов, который проходит с первого раза, ничего не доказывает. Проверяй
его мутациями реализации: ломай по одному решению и смотри, что набор это
ловит. Мутация, после которой код не собирается, не считается пойманной.

Не полагайся на описания в документации без проверки. Если вывод важен,
измеряй. В P2 так выяснилось, что KDF на потоке HTTP-сервера останавливает
всю плату, а p95 1,09 с был артефактом `Connection: close` в скрипте замера.

Перед тем как чинить, убедись, что дефект настоящий.

**Не правь вендорский Matter** (`modules/lib/matter`: исходники, chip-module,
BUILD.gn — ничего). Работа идёт через то, что SDK уже даёт: Kconfig в
`prj.conf`, `src/CHIPProjectConfig.h`, код в `src/matter`, публичные API и
delegates. Если без правки SDK не обойтись — остановись и принеси владельцу
варианты. Одобренная правка записывается в `patches/` с условием удаления,
никогда не остаётся неотслеживаемой. Запиши
`git -C ../modules/lib/matter status --short` в начале и в конце работы,
чтобы было видно, что дерево не тронуто (сейчас там четыре чужие правки).

**Не убирай отладочный код** — команды шелла, отладочный вывод,
закомментированные отладочные блоки. Решение владельца; если отладочный код
мешает, спроси.

**Коммиты не делай сам.** Доведи работу, прогони тесты, оставь всё в рабочем
дереве и скажи, что во что складывается. Коммитит владелец.

## Порядок

Сначала публичный интерфейс с обоснованием решений, потом реализация, потом
тесты — всё вместе. Так требует раздел 12.

Когда закончишь — обнови статус P3 в разделе 10 плана и соответствующие строки
в разделах 3, 11 и 12, отчёт — в `docs/device-development/reports/p3/`.

---

# Справочное: контекст, который дальше не меняется

## Работа с платой

Для P3 плата нужна рано: DNS-SD и commissioning в sim не проверить.
Сбросы — только согласованные. Commissioning в Apple Home делает владелец.

ST-LINK V3SET, серийник 002F002B3233510739363634. **Номер порта консоли
меняется между сессиями**: ищи по серийнику, `tests/bench/bench_console.py`
делает это сам. USB CDC платы с VID `0x2fe3` — мост к UART C6. Проводной
интерфейс хоста к плате — en7 (192.168.88.17), Wi-Fi en0 не используй для
IPv6. Плата — 192.168.88.14, fe80::8234:28ff:fe10:1273.

**Пароль администратора веб-интерфейса на стендовой плате —
`cedar-bench-P2-changed`.** Восстановления пароля в прошивке нет.

Прошивка только приложения, MCUboot уже во внутренней флеш и его трогать не
надо:

    CLI="/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/Resources/bin/STM32_Programmer_CLI"
    EL=$PWD/CLIVEONE-W25Q128_STM32U545-PB10-PA4-PB1-PB0-PA7-PA6.stldr
    "$CLI" -c port=swd sn=002F002B3233510739363634 mode=NORMAL -el "$EL" \
       -w <build>/cedar_switch_3in4out_power/zephyr/zephyr.signed.bin 0x90000000 -v
    "$CLI" -c port=swd sn=002F002B3233510739363634 mode=NORMAL -hardRst

Именно mode=NORMAL: UR и HOTPLUG отваливаются с "Unable to get core ID".
Замеры старта и smoke — `tests/bench/boot_cycle.py` и `smoke.py`; после P2
они опрашивают `/api/v1/auth/state`, и `GET /` уже не сравним с базой P0.
Скрипты аппаратных проверок P2 — `reports/p2/hw/`.

Если порт занят — это CLion держит его своим serial monitor: `lsof /dev/cu.usbmodem*`.

**Реле:** OUTPUT1..4 = PA5/PC4/PC5/PB2. В DTS они `ACTIVE_LOW`, а катушки
через ULN2003: при логическом «выключено» катушки под током, каждый сброс
отпускает и снова притягивает все четыре. Решение о полярности — за
владельцем. Состояние реле на старте задаёт Matter (StartUpOnOff).

## Что известно и не надо перепроверять

Поток W5500 создаётся как K_PRIO_COOP(2) и внутри while(int_pin) делает
синхронный опросный SPI, не уступая процессор. Любой достаточный поток в
сегменте снова задушит логи и шелл. **Владелец принял это как известный риск
до P8.** В апстриме — открытый PR #115626; он же добавляет проверку длины
кадра, отсутствие которой дало BUS FAULT в `w5500_rx` после сброса C6.

Поток HTTP-сервера Zephyr — `K_PRIO_COOP(NUM_COOP_PRIORITIES - 1)`, зашито в
`http_server_core.c`. Динамический ответ HTTP/1 всегда chunked, включая 204 и
304. Заголовок длиннее `MAX_HEADER_LEN` молча отбрасывается. Всё измерено в
`reports/p2/http-server-behaviour`.

Транспорт ESP-Hosted после сброса C6 не восстанавливается без сброса STM32.

Биты Sn_MR: MMB режет только IPv4-мультикаст, MIP6B — только IPv6-мультикаст.
**MIP6B на этом изделии применять нельзя**: ломает Neighbor Discovery и SLAAC,
а Matter живёт на IPv6-мультикасте.

Патч W5500 и другие правки в чужом дереве zephyr сохранены в patches/,
процедура восстановления — в patches/README.md. **`west patch clean` в этом
workspace запускать нельзя**: его `git clean -d -f -x` снесёт чужую
неотслеживаемую работу в общем дереве zephyr.

Вне west workspace Zephyr игнорирует `ZEPHYR_EXTRA_MODULES` целиком, и сборка
падает с «undefined symbol». `west init -l` создаёт workspace, ничего не
выкачивая.

Sysbuild не передаёт `-D<image>_VAR` в `option()` образа; читать через
`zephyr_get(VAR SYSBUILD LOCAL)`.

Флаги сброса RCC к моменту шелла уже очищены Matter — причину старта с платы
не прочитать.

## Открытые вопросы, которые ждут решения владельца

- закрыть P0 без проверки I/O или провести её
- полярность реле в DTS платы
- вариант записи C6 для P6 (раздел 8) и rollback
- BUS FAULT в `w5500_rx`: принять риск, патч в `patches/`, ждать #115626;
  отдельно — `CONFIG_RESET_ON_FATAL_ERROR`
- FRAM `mb85rsxx` отказывает на каждом старте: сверить чип или убрать узел
- бэкенд хранения настроек (файл растёт с каждым стартом Matter)
- восстановление доступа при забытом пароле
- 4 клиента p95 313 мс при цели 250; цена заголовков ответа не подтверждена
- слот `DEVICE_CONFIG_SECRET_ADMIN_PASSWORD` не используется
- четыре незаписанные правки в `modules/lib/matter` (Atios, 2026-08-30):
  перенести в `patches/`, оставить или откатить
