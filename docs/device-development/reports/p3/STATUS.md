# P3 — состояние работы (для продолжения в новой сессии)

**Этап не закрыт. Коммитов P3 нет, всё в рабочем дереве.** Прочитай этот файл
вместе с `docs/device-development/NEXT-SESSION.md` (передача на P3, тоже не
закоммичена) и памятью проекта.

Дата: 2026-09-13. Дерево `modules/lib/matter` не тронуто (те же 4 чужие правки,
`hw/logs/00-matter-tree-at-start.txt`).

## Что сделано в коде (не закоммичено)

| Часть | Файлы | Проверено |
|---|---|---|
| `modules/matter-service` — ядро: снимок стека, окно, коды, fabrics, запросы через ScheduleWork; коды генерируются один раз при старте стека | `modules/matter-service/*`, `tests/matter_service` (31 sim-тест) | sim 31/31; мутации 32 + 3 (кеш кодов), все пойманы после добавления тестов |
| Адаптер к SDK: AppDelegate (перечитывание окна отдельной задачей после колбэка SDK), FabricTable::Delegate, id fabric = SHA-256(root pubkey)[:8] + fabric_id, лог медленных вызовов SDK ≥500 мс | `src/matter/matter_service_chip.{h,cpp}`, `matter_init.{h,cpp}` (убран `k_msleep(5000)` из net_mgmt-колбэка), `src/main.c` | плата |
| HTTP: 6 операций Matter, jobs `matter_open/close`, capabilities из снимка и пределов SDK | `src/web/api/v1/{matter.c,routes.h,http_resources.h,system.c,v1_internal.h}`, `tests/web_api/src/v1.c` (10 `test_matter_*`) | sim web_api 87/87; контракт 5/5, 15 из 35 операций |
| `prj.conf`: `CONFIG_CHIP_ETHERNET=y` (без него `chip_mdns="none"` — DNS-SD не было вовсе), `CONFIG_NET_IF_MCAST_IPV6_ADDR_COUNT=14`, `CONFIG_MATTER_SERVICE=y`, `CONFIG_WEB_AUTH_KDF_THREAD_PRIORITY=13` | `prj.conf`, `psram_sections.ld` (+libmatter_service) | плата: mDNS виден с хоста |
| Фронтенд: экран Matter (статус, окно+таймер, QR через `uqr` 0.1.3, ручной код и PIN раздельно, fabrics), маршрут `/matter` | `src/web/frontend/src/features/matter/*`, `components/{QrCode,Polled}.tsx`, `state/useSessionGuard.ts`, `api/matter.ts`, `ru.ts`, e2e `matter.spec.ts` | vitest 133, e2e mock 6/6; мутации 13/13 |
| Отладка: `storage_bench [rounds]`, `storage_wipe yes` | `src/diagnostic/storage_bench.c`, CMakeLists | плата |
| Документы: решения P3 в `api-contract.md`, README `matter-service`, `web-auth`, фронтенда, `tests/ci` | — | — |

Полный sim-прогон последний раз 316/316 до кеша кодов; после кеша прогнаны только
`matter_service` (31/31) и `web_api` (87/87) — **перед закрытием прогнать весь sim,
контракт и фронтенд заново.**

## На плате (P3)

- chip-tool (`modules/lib/matter/out/chip-tool/chip-tool`) вводил плату много раз;
  две независимые fabrics (alpha, beta) добавлены и пережили сброс (критерий P3).
- Исправление «окно закрыто после commissioning» подтверждено на плате (API и
  шелл SDK, `hw/logs` раунды alpha/beta 81–85 в scratchpad).
- Открыто: вход в веб иногда не укладывается в 10 с, пока Matter долго работает
  (ping и шелл при этом живы). Приоритет KDF 13 не помог. Причина не найдена;
  связана с долгими операциями хранилища на потоке Matter.
- Криптография P-256 программная (PSA без аппаратного драйвера): PASE ~10 с,
  аттестация 3–5 с, CSR ~4 с. Кандидат: `CONFIG_MBEDTLS_PSA_P256M_DRIVER_ENABLED`
  (в prj.conf закомментирован — спросить владельца, пробовал ли).

## Главная находка: бэкенд настроек

Файловый бэкенд (`/lfs/settings`) не имеет `csi_load_one`; каждое чтение ключа
Matter (`settings_load_one`/`settings_get_val_len`, Zephyr ≥4.4) = полная загрузка
с поиском дубликатов для каждой строки (N²), чтения по 32 байта через LittleFS.
Кеш LittleFS не помогает (`lfs-cache/`: скан 138–237 мс при любых read/cache size).
Живых записей: 17 при 0 fabrics, 37 при 2 (9 ключей на fabric; +5 мёртвых
`/settings/relays/default/*`). Оценка MAX_LINES для 5 fabrics: ~58 живых → 90–128.

### Сравнение бэкендов по методике владельца

`settings-backends/`: стирание раздела → старт на чистом → перезагрузка и
проверка чтения → 10 циклов (перезагрузка, commissioning, перезагрузка, удаление
fabrics). NVS/ZMS/FCB — 32 сектора на `storage_lfs` (SPI-флеш 0x0), File — как в
прошивке. Сводка: `python3 summarize.py`, фазы: `python3 phases.py`.

| Бэкенд | Commissioning | Старт Matter без fabric, цикл 1 → 10 (макс) | с fabric 1 → 10 (макс) | Удаление fabrics медиана/макс | Сохранность |
|---|---|---|---|---|---|
| File | 9/10* | 3,2 → 19,0 (49,4) пилой | 8,2 → 48,0 (50,8) | 29,7 / 60,8 | 23/23 |
| ZMS | 9/10* | 0,6 → 6,2 | 0,7 → 7,4 | 2,0 / 6,7 | 23/23 |
| NVS | 10/10 | 0,7 → 10,4 | 2,0 → 12,6 | 8,3 / 17,6 | 23/23 |
| FCB | 1/10 | 6,2 → 123,3 | 49,6 → 127,2 | 14,9 / 71,3 | 24 учтено / 23 измерено** |

\* сбой — потерянная команда UART (старая версия run_backend.py), не хранилище.
FCB: со 2-го цикла chip-tool падает на PASE (поток Matter занят чтением настроек).
\*\* у FCB счётчик Matter на одну загрузку больше измеренных уже на шаге проверки чтения (2→3 вместо 1→2): лишняя загрузка до первого замера, вероятно повторная отправка `kernel reboot` в новой версии скрипта; не проверено.
`storage_bench` FCB: 2,4 с на любое чтение ключа (16 записей). Полные таблицы: `settings-backends/summary.md`, фазы: `phases.md`.
Фазы (медианы): PASE 10,2–10,8 с у всех (крипто); запись fabric File 2,8 / ZMS 0,7 / NVS 1,4 с.
NVS `storage_bench`: 176 мс на любое чтение (нет load_one). ZMS и NVS растут линейно
с числом удалённых записей — GC за 10 циклов не наступил; долгий прогон не делался.

Предварительный вывод: ZMS лучший из штатных, NVS второй, File нестабилен, FCB
непригоден. **Решение о бэкенде — за владельцем** (он ранее пробовал ZMS без
ускорения; вероятная разница — быстрый путь `USE_SETTINGS_LOAD_ONE` в Matter SDK
для Zephyr ≥4.4). Переход требует переноса ключей (пароль админа, MQTT, fabrics)
или сброса.

## Что осталось

1. Решение владельца по бэкенду настроек; при переходе — раздел, миграция, тесты.
2. После этого: повторить на плате commissioning 2 fabrics + сброс, e2e против
   платы (`E2E_DEVICE_URL=http://192.168.88.14 E2E_DEVICE_PASSWORD=cedar-bench-P2-changed`).
3. Разобрать зависание входа во время долгой работы Matter.
4. Полные прогоны sim/контракт/фронтенд; отчёт `reports/p3/README.md`; план
   (разделы 3, 10, 11, 12, 13); раскладка коммитов.
5. Вопросы владельцу: p256-m; мёртвые ключи реле; четыре правки в `modules/lib/matter`.

## Состояние платы

Прошит образ сравнения FCB (`settings-backends`), настройки на `storage_lfs`.
Веб-пароля там нет (режим setup) — для работы прошить нужный образ; для
первичной настройки `hw/setup_admin.py`. Сборки лежали в scratchpad прошлой
сессии (`/private/tmp/claude-501/.../scratchpad/p3/`), могут исчезнуть —
пересобрать: `west build ... -- "-Dcedar_switch_3in4out_power_EXTRA_CONF_FILE=<common.conf>;<backend>.conf"
-Dcedar_switch_3in4out_power_EXTRA_DTC_OVERLAY_FILE=settings-on-storage-lfs.overlay`.
Скрипты в `settings-backends/` и `hw/` ссылаются на пути scratchpad — поправить `HERE`/`P3`.
