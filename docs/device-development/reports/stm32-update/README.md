# Отчёт этапа «Обновление STM32 через веб»

**Статус: начат 2026-09-15.** Worktree `zephyr_latest/cedar_p7`, ветка `p7-stm32-update` от `main`
`df852ea` (слит P6, PR #3). Номер P7 в разделе 10 плана занят SoftAP, поэтому этап называется
по содержанию, а отчёт лежит в `reports/stm32-update/`.

Исходный проект и шаг 0 на плате B — `reports/p6/stm32-ota-proposal.md` и
`reports/p6/hw/README.md` строка 23: обмен через scratch 64 КБ работает на текущей разметке,
~34 с на образ 1,42 МБ, возврат неподтверждённого образа и продолжение прерванного обмена
проверены.

## Плата

Работа на железе — **плата A** (ST-Link V3SET `002F002B3233510739363634`, 192.168.88.14,
USB CDC `2037394C3543501200550045`). На 2026-09-15 17:10 других сессий Claude Code нет, порты
платы никем не открыты, HTTP отвечает (`setup_required=false`). На плате A — рабочий MCUboot
проекта (`sysbuild/mcuboot.conf`, без recovery), тот же swap-scratch и те же хуки, что у
стендового MCUboot платы B; результат шага 0 к нему переносится. MCUboot не перешивается.

Плата B (J-Link) 2026-09-15 17:10 не отвечала на ping `192.168.88.13` — не разбиралось.

## Итог на 2026-09-15 19:00

**Проверено на плате A через API v1** (приёмка, `hw/logs/03-web-acceptance`, `04-downgrade-rollback`):
загрузка образа MCUboot сразу в слот 2 → проверка → установка → обмен MCUboot (≈36 с, ответ через
55–65 с) → **самоподтверждение через 20 минут без перезапуска** → подтверждение сохранилось после
сброса; понижение версии без флажка — 422, с флажком — установлено; **сброс до подтверждения —
откат, `rolled_back`**; отказ по содержимому (`invalid_image`); одна загрузка на оба target (409 с
id). Шагом 0 (шелл, `01-shell-step0`, `02-iwdg-and-revert`): запись подтверждения через XIP,
**зависание → IWDG → откат**, IWDG не режет обмен после программного сброса.

**Тесты на итоговом дереве:** sim 21 из 22 конфигураций, 1010 из 1011 (единственный отказ —
`web_api` `test_a_chunk_the_volume_cannot_take_fails_its_job`, падает и на `df852ea`); контракт 5/5,
37 из 37 операций, pytest 464 + 14; фронтенд (форк D) vitest 455, e2e против mock 31 + 1 пропущен.
Мутации: system-image-store 149/161, system-updater 155/158, mock 90/93, экран 49+ (60), привязки
52/56 — выжившие обоснованы в `notes-*.md`, кроме 2 пробелов привязок (ниже).

**На плате A оставлен** образ `31628617…` (`1.0.0+0`, итоговый код worktree), подтверждён; в журнале
— итог `rolled_back` последней проверки; администратор `cedar-bench-P2-changed`; в обоих слотах — один и тот же образ `1.0.0+0` (после
отката слот 2 держал `31628617`, затем тот же образ записан в слот 1 через ST-Link; `sysupd status`
19:10 — хеши слотов совпадают, загрузок STM32 и ESP32 нет). Плата отдаёт тот же бандл, что в
`dist` (`index-C13QFMkj.js`, в нём `stm32u585` и `/system/updates`). Файл `flash.bin` (8 МиБ)
симулятора flash, оставленный прямым запуском форка A в корне west, удалён.

**Открыто:**
1. Скорость: приём 7,1–7,5 КиБ/с (1,4 МБ ≈ 3,3 мин: `PUT` 16 КиБ ≈ 400 мс, задача куска ≈ 1,7 с),
   проверка 62,6 с (≈ 22,8 КиБ/с чтения SPI NOR + SHA-256 программно). Не разбиралось, где время.
   **Zephyr `stream_flash`** (вопрос владельца 2026-09-15, прочитан `subsys/storage/stream/stream_flash.c`
   дерева; `CONFIG_STREAM_FLASH=y` уже есть в сборке): делает те же операции, что `write_chunk` —
   стирание страницы перед дописыванием, запись, чтение обратно в callback, — поэтому скорость не
   изменит. Не покрывает нашу модель: буфер не больше страницы (4 КиБ, `STREAM_FLASH_INSPECT`),
   куски только подряд без повтора/отката, а после перезагрузки `stream_flash_progress_load`
   восстанавливает только `bytes_written`, `erased_up_to` начинается с 0 — первая же запись стирает
   страницу в **начале** области, то есть уже принятый заголовок образа; продолжение загрузки после
   сброса без правки внутренних полей невозможно. Прогресс он хранит в settings (ZMS) на каждый
   сохранённый кусок. Кандидаты на ускорение — сначала замер фаз куска на плате: `write_meta` на
   каждый кусок (open/write/sync/close/rename LittleFS на той же SPI NOR), стирание по 4 КиБ со
   `SPI_NOR_SLEEP_ERASE_MS=50` (≥ 50 мс на сектор, 4 сектора на кусок), `PUT` ≈ 400 мс по сети.
2. Обрыв питания и сброс **посреди приёма и посреди обмена** на плате A не проверялись (на плате B
   обмен продолжался с места — P6 строка 23); сброс во время `requesting`/`rebooting` — тоже.
3. Экран «Прошивка» против настоящей платы не открывался (e2e только против mock; `device.spec.ts`
   его не открывает).
4. Установка ESP32, блокирующая установку STM32 (`update_running`), в sim не покрыта (2 мутанта).
5. Унаследованный тест `web_api` про заполненный том — решить, как его заполнять после правки P6.
6. На плате A удалена чужая загрузка ESP32 `upload_fe778808ba86d326` (`wifi-cp-merged.bin`).
7. Решения владельца, принятые по рекомендации (понижение с флажком, установка через Wi-Fi, без
   веб-кнопки подтверждения) — подтвердить.
8. Настройки: образ worktree (`df852ea`) использует File-бэкенд; переход на ZMS в основном дереве не
   закоммичен — при слиянии этапа это надо свести.
9. `createUpload` для STM32 стирает 64 КиБ слота 2 прямо в кооперативном потоке HTTP (759–1063 мс
   на плате) — всё остальное HTTP стоит. Кандидаты: задача или ленивое стирание в первом куске.
10. Журнал `sysupd.journal` и `sysimg.meta` открываются на каждом старте, даже если их нет, — новые
    строки `fs: file open error (-2)` в логе (тот же класс, что находка P6 про unlink). Сначала `fs_stat`.
11. Порядок отказов: если своя загрузка STM32 уже есть **и** прошивка не подтверждена,
    `createUpload` отвечает `invalid_state` (проверка слота раньше `-EBUSY` хранилища), а контракт
    ставит `busy` первым. Достижимо только через шелл `sysupd request`; sim проверяет порядок лишь
    для загрузки ESP32. Правка кода отложена, чтобы образ на плате совпадал с деревом.
12. **Комиссионинг на плате A стал медленным, сессия падает** (владелец, 2026-09-15 после
    приёмки). Причина — база worktree, не модуль обновления: `cedar_p7` сделан от `df852ea`, а
    ускорение комиссионинга лежит в основном дереве незакоммиченным (`reports/p3/tuning/sram`,
    `reports/crypto/pka`). В образе `31628617…` по сравнению с прежним образом платы A нет:
    арены malloc в SRAM (`MALLOC_ARENA_SIZE=-1`; здесь 1 МиБ в PSRAM вместе с `malloc.c`),
    `.bss/.noinit` libCHIP и libtfpsacrypto в SRAM (здесь в `psram_sections.ld`), горячего кода
    криптографии в SRAM (`CODE_DATA_RELOCATION_SRAM` и блок в CMakeLists), PKA и p256-m, ZMS
    (здесь File). По замерам P3 это состояние c0: chip-tool 56 с против 18 с, одна проверка
    ECDSA 3,4 с против 0,3 с, вторая фабрика Apple Home не укладывается в fail-safe 30 с. Свои
    правки этапа на пути сопряжения — IWDG-поток и тик установщика раз в секунду — на плате не
    измерялись. Для слияния этапа с основным деревом: потоки `src/services/system` в SRAM
    уменьшают арену malloc (SRAM в s3 распределён полностью), подтверждение требует стек в SRAM.

    **Сведено 2026-09-15 20:08 (решение владельца: вариант 2).** Незакоммиченные правки основного
    дерева перенесены в worktree трёхсторонним слиянием от `df852ea` (`git merge-file`, база —
    `df852ea`): `prj.conf` (ZMS, арена −1, `CODE_DATA_RELOCATION_SRAM`, p256-m, PKA), `CMakeLists.txt`
    (горячий код криптографии в SRAM, malloc.c вне PSRAM, модуль `psa-driver-stm32`),
    `psram_sections.ld` (libCHIP и libtfpsacrypto остаются в SRAM), `Kconfig`
    (`TF_PSA_CRYPTO_DISPATCH_DIR`), overlay (узел PKA, chosen `settings-partition`), `patches/`
    (`tf-psa-crypto/ecp-p256-stm32-pka.patch`, в дереве tf-psa-crypto уже применён), исправление
    `network_manager.c` `rejoin` с тестом; `modules/psa-driver-stm32` скопирован. Конфликты — только
    соседние добавления (оба варианта оставлены). Не перенесены правки отчётов P3 и
    `tests/storage_report`: в образ они не входят. `VERSION` → `1.1.0`.
    Образ `zephyr.signed.bin` `1b359f34…` (1 449 988 Б, заголовок `1.1.0+0`): SRAM `_end`
    `0x200415e8` (≈268 КБ статики, арена malloc ≈510 КБ до `0x200c0000`), PSRAM до `0x7016b2b0`;
    `mbedtls_ecp_mul` и `mbedtls_mpi_mul_mpi` в SRAM, стек Matter `0x20035ce0`, стеки `app_iwdg` и
    `sysupd_tick` в SRAM (`0x2003d4e0`, `0x2003c4e0`) — подтверждение разрешено. Пик malloc при
    сопряжении по-прежнему не измерен. Вклад этапа в SRAM ≈9 КБ: стек `sysupd_tick` 4 КБ,
    `app_iwdg` 1 КБ, `copy_buf` 4 КБ (`sysupd selfcopy`). Sim после слияния: 21 из 22 конфигураций,
    1011 из 1012 (на один больше — тест `rejoin` в `network_manager`; падает прежняя конфигурация
    `web_api`). Комиссионинг на этом образе ещё не проверялся — владелец шьёт его по сети.

    **Комиссионинг на образе `1b359f34…` (1.1.0, установлен владельцем по сети) — всё ещё срывается**
    (лог владельца 2026-09-15 ~20:40): PASE быстрый (Pake1 → Pake2 100 мс, `stm32_pka: PKA ready`),
    но поток Matter (`CHIP`, приоритет 14 — `src/CHIPProjectConfig.h`, так же в основном дереве)
    получает процессор с задержками 650–900 мс примерно каждую секунду: ответ через ~700 мс после
    запроса, повторы на 250–500 мс позже срока, `Long dispatch time: 695 ms`, mDNS-публикация 550 мс;
    ответ на AttestationRequest не подтверждён после 4 повторов, fail-safe 60 с истёк
    (`Commissioning failed (attempt 1): 32`). Пинг во время простоя ровный (6–7 мс), то есть длинной
    маскировки прерываний нет. Чтение OSPI правкой XIP не затронуто (перехвачены только запись и
    стирание); SPI в режиме опроса — как в образе основного дерева. Фон без комиссионинга
    (`kernel thread list` через telnet каждые 5 с, uptime 506–606 с): idle 55–65 %, `tx_q` 8–10 %,
    `rx_q` 6–8 %, `shell_telnet` 5–6 % (сам сбор), `eth_w5500` 3–5 %, `http_server` ≤ 3 %.
    Срез во время попытки потерян: принудительно закрытое подключение `nc` заняло telnet-шелл
    (один клиент), новые подключения без ответа до перезагрузки. **Пауза по просьбе владельца**;
    следующий шаг — срезы потоков через консоль UART во время попытки.
13. **Полярность реле** (владелец 2026-09-15 ~21:15: «на включение выключаются и наоборот»; находка
    P0 №4). В общем DTS платы (`zephyr/boards/arm/cedar_switch_3in4out_power_rev3`, вне git
    zephyr) реле `GPIO_ACTIVE_LOW`, а ULN2003 включает катушку высоким уровнем. Исправлено в
    overlay приложения, дерево zephyr не тронуто: `&relay_ac1..4` → `GPIO_ACTIVE_HIGH` (PA5, PC4,
    PC5, PB2) в обоих overlay платы; `VERSION` → `1.1.1`. Правильное место — DTS платы; overlay
    можно убрать, когда поправят его.

## Решения владельца

Приняты раньше (2026-09-14, `stm32-ota-proposal.md`):

| № | Решение |
|---|---|
| Д1 | Отдельный этап, новый worktree после P6 |
| Д2/Д5 | Новая прошивка подтверждает себя сама, **проработав 20 минут без падения**; плюс консольная команда немедленного подтверждения |
| Д3 | Пока без подписи (`BOOT_SIGNATURE_TYPE_NONE`) |

Приняты 2026-09-15 (начало этапа):

| Вопрос | Решение |
|---|---|
| Где вести работу | **Новый worktree** `../cedar_p7`, ветка `p7-stm32-update` от `df852ea`. Незакоммиченные правки PKA/SRAM основного дерева в этот образ не входят |
| Способ записи подтверждения | **Запись в XIP через драйвер**: `boot_write_img_confirmed()` пишет `image_ok` в трейлер слота 1 на OCTOSPI, откуда выполняется код, через XIP-драйвер владельца (`FLASH_STM32_OSPI_XIP`). Без второго перезапуска |
| Куда идут байты загрузки | **Сразу в слот 2** (`slot1_partition`, SPI NOR `0x600000`), без промежуточного файла в `/lfs` |
| IWDG | **Включить сразу**: зависшая новая прошивка откатывается через сброс сторожем |

Принято по рекомендации проекта, отдельно не спрашивалось (владелец может переиграть):

| № | Решение | Почему так |
|---|---|---|
| Д6 | Понижение версии разрешено только с явным `acknowledge_downgrade=true` | Рекомендация проекта; откат на известную прошивку — законный сценарий |
| Д7 | Установка STM32 разрешена и через Wi-Fi | C6 не затрагивается; обмен делает MCUboot без сети |
| — | Веб-кнопки немедленного подтверждения нет | Владелец назвал только консольную команду |

## Что известно до кода (проверено 2026-09-15)

- **XIP-драйвер** — работа владельца (6 сентября), лежит в общем дереве zephyr незакоммиченным и
  не записан в `patches/`: `drivers/flash/flash_stm32_ospi_xip.{c,h}` (новые),
  `flash_stm32_ospi.c` (+176), `Kconfig.stm32_ospi` (+43), `CMakeLists.txt` (+1). Он же требует
  ещё одну незаписанную правку: `sys_clock_systick_wraps_observed()` в
  `drivers/timer/cortex_m_systick.c` и `include/zephyr/drivers/timer/system_timer.h`. Включается
  одной строкой Kconfig без правки дерева; записать в `patches/` — вместе с правкой SysTick.
- Охраняемый участок драйвера (`XIP_RAM_FUNC`) обращается к `ctx` в данных устройства и к буферу
  отскока на стеке вызывающего. Правило в самом файле: «никакой внешней памяти, кроме регистров
  контроллера». В этом приложении `.data/.bss` многих библиотек и стеки лежат в PSRAM
  (OCTOSPI2). Данные драйвера flash (`drivers__flash`) в список перемещаемых библиотек не входят —
  **проверить по map**; стек рабочего потока v1 — **проверить**. PSRAM сидит на другом
  контроллере, и OCTOSPI1 вне memory-mapped её, вероятно, не трогает, но это надо измерить.
- Приложение собирается с `FLASH_STM32_OSPI=y` в режиме `NOR_MEMMAP` — чтение слота 1 через
  flash area работает штатно (bootutil: `boot_is_img_confirmed`, `boot_read_bank_header`).
- `boot_write_img_confirmed()` → `boot_set_next(active)`: при `magic=good` и `image_ok` не
  выставлен пишет **только флаг `image_ok`** (16 байт с выравниванием), без стирания.
- `prj.conf:274 CONFIG_FLASH_LOG_LEVEL_DBG=y` перекрывает `CONFIG_FLASH_LOG_LEVEL_OFF=y`
  (строка 13); комментарий у строки 13 и заметка `aliro_reader` — логи драйвера flash из
  кода в QSPI дают hard-fault. Для записи через XIP **убрать DBG**.
- IWDG: узел `iwdg` в `stm32u5.dtsi` выключен; драйвер `wdt_iwdg_stm32` стартует только по
  `wdt_setup()` после init устройства (POST_KERNEL), старта на загрузке нет.
- Версия образа у всех сборок `0.0.0+0`: `MCUBOOT_IMGTOOL_SIGN_VERSION` берётся из
  `APP_VERSION_TWEAK_STRING`, если в каталоге приложения есть файл `VERSION`, — его нет.
- Трейлер swap-scratch в разметке MCUboot (секторы SPI NOR 64 КБ) занимает последний сектор
  слота 2; в разметке приложения (4 КБ) это последние 16 секторов. Образ не должен заходить в
  последние 64 КиБ слота: предел файла `4 МиБ − 64 КиБ = 4 128 768 Б`.

## Дизайн

### Поток

1. `POST /firmware/uploads {filename, size_bytes, sha256, target:"stm32u585"}` → `201 Upload`.
   Сервер **стирает последние 64 КиБ слота 2** (там magic и флаги прошлого запроса обмена) и
   пишет метаданные в `/lfs/firmware/sysimg.meta`. Имя не начинается с `upload` — firmware-store
   чистит только своё пространство имён.
2. `PUT …/data?offset=N` → задача `upload_chunk`: рабочий поток стирает секторы, **начинающиеся**
   внутри куска (сектор, в середине которого стоит `N`, уже содержит принятые байты и не
   стирается), пишет кусок, читает обратно и сравнивает, затем метаданные с новым
   `received_bytes`. Перезапуск между записью и метаданными: клиент шлёт тот же кусок, запись
   тех же байтов поверх частично записанных NOR допускает (только 1→0 к тому же образцу).
3. `POST …/verify` → задача `firmware_verify`: потоковый SHA-256 всего файла по слоту против
   объявленного; разбор MCUboot — magic `0x96f3b83d`, `ih_hdr_size` равен `ROM_START_OFFSET`
   этой сборки, запрещённые флаги (шифрование, RAM load, non-bootable, сжатие), размер
   `hdr + img + protected TLV + TLV` ≤ предела и **ровно** размер файла, TLV info magic, TLV
   SHA-256 совпадает с посчитанным по заголовку, телу и защищённым TLV (ту же проверку сделает
   MCUboot), таблица векторов: начальный SP в ОЗУ, reset-вектор в окне приложения с битом Thumb.
   Чужой вектор или размер заголовка — `unsupported_target`; остальное — `invalid_image`.
4. `POST /system/updates {upload_id, acknowledge_downgrade}` → `202`, задача `system_update`,
   фазы `preparing → requesting → rebooting`. `requesting`: журнал в
   `/lfs/firmware/sysupd.journal` («ожидает обмена»: версия и хеш TLV старой и новой прошивки),
   `boot_request_upgrade(BOOT_UPGRADE_TEST)`; `rebooting`: перезапуск через 2 с (ответ и опрос
   успевают). Отменяема до `requesting`.
5. MCUboot: проверка хеша слота 2 → обмен ~34 с → новое приложение.
6. Новое приложение на старте сверяет журнал с тем, что работает (хеш TLV слота 1):
   - работает новая, не подтверждена → `awaiting_confirmation`, срок = старт +
     `CONFIG_SYSTEM_UPDATER_CONFIRM_SECONDS` (1200). По сроку (или команде шелла) —
     `boot_write_img_confirmed()` через XIP → проверка `boot_is_img_confirmed()` → `succeeded`;
   - работает новая, подтверждена → `succeeded`;
   - работает старая, журнал видел новую запущенной → `rolled_back`;
   - работает старая, новая до записи журнала не дошла → `failed` («новая прошивка не
     запустилась или MCUboot её отклонил»);
   - журнал остановился до `requesting` → `interrupted`, ничего не менялось.
7. Итог — `GET /system/firmware` (`last_update`); задача после перезапуска — 404, как у ESP32.

### Отказы

- `createUpload` STM32: образ больше предела — 413; идёт загрузка любого target — 409 `busy`;
  **работающая прошивка не подтверждена** или обмен уже запрошен — 409 `invalid_state` (в слоте 2
  лежит прежняя прошивка, без неё откат невозможен).
- `startSystemUpdate`: неизвестный upload — 404; upload не STM32 — 422 `unsupported_target`;
  не `ready` — 409 `invalid_state`; установка STM32 или ESP32 идёт, сетевая транзакция в
  `applying`/`awaiting_confirmation` — 409 `busy`; работающая прошивка не подтверждена — 409
  `invalid_state`; понижение без `acknowledge_downgrade` — 422 `validation_failed`.

### IWDG

`SYS_INIT` на POST_KERNEL после драйверов: `wdt_install_timeout` +
`wdt_setup(WDT_OPT_PAUSE_HALTED_BY_DBG)`, срок `CONFIG_APP_IWDG_TIMEOUT_MS` (20 000); кормит
отдельный поток `K_PRIO_PREEMPT(0)` — сброс только при зависании, IRQ lock или голодании
кооперативными потоками (принятый риск W5500 превращается в сброс, а в первые 20 минут — в
откат; записано). Причина сброса (`RCC_CSR`, в том числе `IWDGRSTF`) читается до того, как её
очистит Matter, и выводится в лог и шелл. **Проверить на плате**: не продолжает ли IWDG
считать после программного сброса (обмен 34 с длиннее срока).

### Модули и файлы

| Что | Где |
|---|---|
| Приём в слот 2 и проверка MCUboot | `modules/system-image-store` (ядро над таблицей функций flash и файлов; sim на flash simulator) |
| Установка, журнал, подтверждение, итог | `modules/system-updater` (ядро над платформой; sim на фейке) |
| Связка платы: bootutil, XIP, IWDG, шелл `sysupd` | `src/services/system` |
| API | `src/web/api/v1/firmware.c` (диспетчер по `target`), `system_update.c`, `routes.h`, `http_resources.h` |
| Контракт | `openapi.json`, `api-contract.md` («Обновление STM32»), mock, проверки |
| Экран «Прошивка» | `src/web/frontend/src/features/system-update` (переиспользует `uploader.ts`, `sha256.ts`, `remember.ts`) |
| Версия образа | файл `VERSION` в корне приложения |
| Патч XIP-драйвера и SysTick | `patches/zephyr/` + `patches.yml` (дерево не меняется — правка там уже есть) |

### Изменения контракта (v1, только добавления)

- `UploadRequest.target` (необязательно, `esp32c6` | `stm32u585`, по умолчанию `esp32c6`);
  `Upload.target` (обязательно).
- `FirmwareImage`: `target` + `stm32u585`, `format` + `mcuboot_image`; `partition_layout_id` и
  `host_protocol` — `null` у STM32; `allowed_methods=["ota"]`.
- `Capabilities`: `features.stm32_update`, `limits.system_upload_max_bytes`,
  `firmware_formats` + `mcuboot_image`.
- `GET /system/firmware` → `SystemFirmware`; `POST /system/updates` → `JobAccepted`;
  `Job.kind` + `system_update`.

## План проверки на плате A

0. **Без API, только шелл** (самый рискованный путь — запись через XIP): образ с
   `FLASH_STM32_OSPI_XIP`, без DBG-логов flash, с IWDG и командами `sysupd status|selfcopy|
   request|confirm|wdt`. `selfcopy` копирует работающий образ из слота 1 в слот 2 — обмен
   одинаковых образов проверяет весь путь, кроме смены версии. Проверки: init XIP-драйвера;
   `status` читает оба слота; `selfcopy` → `request` → сброс → обмен → старт неподтверждённым →
   `confirm` через XIP → сброс → отката нет; `request` → сброс → без подтверждения сброс → откат;
   IWDG: зависание → сброс; зависание при запрошенном обмене — обмен доходит до конца.
1. API + mock + экран + тесты.
2. Приёмка через веб: загрузка → проверка → установка → обмен → подтверждение по сроку (для
   стенда срок укорачивается Kconfig) → версия сменилась; образ, который зависает → IWDG → откат;
   сброс на приёме, на обмене и до подтверждения; понижение версии.

## Ход работ

### 2026-09-15, связка платы для шага 0

- Контракт: `openapi.json` (+2 операции, +3 схемы, поля `target`, `stm32_update`,
  `system_upload_max_bytes`, вид задачи `system_update`), `api-contract.md` раздел «Обновление STM32».
  Проверки документа: 5/5, устройство обслуживает 35 из 37 операций.
- `VERSION` → образ `1.0.0+0` (было `0.0.0+0` у всех сборок).
- `src/services/system`: причина сброса на PRE_KERNEL_1, IWDG (узел включён в оверлее, кормящий
  поток `K_PRIO_COOP(0)`, 20 с, пауза под отладчиком), шелл `sysupd status|selfcopy|erase-trailer|
  request|confirm|hang`. `prj.conf`: `FLASH_STM32_OSPI_XIP=y`, `HWINFO`, `WATCHDOG`; строка
  `FLASH_LOG_LEVEL_DBG=y` убрана.
- **Первая сборка не слинковала сервис вовсе**: объект статической библиотеки, до которого
  доходят только iterable sections (SYS_INIT, поток, шелл), линкер не берёт. Вызов
  `system_service_start()` из `main()` это исправляет.
- **Карта (`zephyr.elf`)**: охраняемые функции XIP в `.ramfunc` `0x20000000–0x200004a0`, данные
  устройства `flash_stm32_ospi_dev_data` `0x2000457c`, стек шелла — SRAM. **Стек рабочего потока v1
  (`v1_worker_stack` `0x70026d20`) — в PSRAM**: запись подтверждения через XIP с этого потока
  положила бы буфер отскока драйвера и буфер `boot_write_trailer_flag` во внешнюю память внутри
  охраняемого участка. Подтверждение делается потоком с SRAM-стеком (сервис платы), не рабочим
  потоком API.
- Порты: у ST-Link платы A только VCP `usbmodem21103`; `usbmodem21301` — постороннее устройство
  «USB JTAG/serial debug unit» `3C:0F:02:C7:78:70` (ESP32), не плата A, хотя промпт P6 приписывал
  его ей. USB CDC приложения платы A — `usbmodem101` (`2037394C3543501200550045`).
- Вторая сборка не слинковалась: `boot_request_upgrade`, `boot_write_img_confirmed`,
  `boot_is_img_confirmed`, `mcuboot_swap_type` живут в `zephyr/subsys/dfu/boot/mcuboot.c`, который
  собирается только с `CONFIG_MCUBOOT_IMG_MANAGER` (внутри меню `IMG_MANAGER`);
  `MCUBOOT_BOOTUTIL_LIB` даёт лишь заголовки и `bootutil_public.c`. Включены оба.
- **С `IMG_MANAGER` сборка не компилируется**: `mcuboot.c` берёт активный слот из chosen
  `zephyr,code-partition` (`DT_PARTITION_ID`), а у платы это регион памяти `ext_flash_mem`
  (`memory@2000000`), не раздел. Менять chosen нельзя — от него зависят адрес линковки и
  `FLASH_LOAD_OFFSET` приложения. `IMG_MANAGER` снова выключен; сервис работает прямо через
  `bootutil_public.c`: `boot_set_pending()`, `boot_set_confirmed()`, `boot_swap_type()`,
  `boot_read_swap_state_by_id()` — они открывают `slot0_partition`/`slot1_partition` по метке.
  Подтверждение идёт тем же `boot_set_next(active)`, что и у `boot_write_img_confirmed()`.
  «Подтверждён» считается как у zephyr: magic трейлера не `good` (образ записан напрямую) или
  `image_ok` выставлен.

### 2026-09-15, шаг 0 на плате A (только шелл, `hw/logs/01-shell-step0/`)

Образ `53b6af18…` (1 396 628 Б, `1.0.0+0`), прошит в слот 1 по ST-Link (`hw/sysupd_a.py flash`).

| Шаг | Результат |
|---|---|
| Первый старт | MCUboot `Swap type: none`, `v1.0.0`; `IWDG running, timeout 20000 ms` на 1,556 с; причина сброса `0x3` (pin, software); образ подтверждён (трейлер слота 1 от прежней истории: `magic good, swap_type 4, copy_done 1, image_ok 1`); слот 2 пуст, трейлер стёрт. DHCP `.14` |
| `sysupd selfcopy` | 1 396 628 Б в слот 2 за **10,97 с**, rc 0 (стирание 4 КБ секторами + запись 4 КБ кусками по SPI NOR) |
| `sysupd request test` | rc 0, следующий обмен `test` (2) |
| `sysupd confirm` (после обмена) | **`boot_set_confirmed()` через XIP-драйвер: rc 0 за 1 мс**, повторное чтение трейлера — подтверждён; плата продолжила работать (консоль, сеть) |
| Сброс по ST-Link | **подтверждение на флеш**: `Primary image: magic=good, swap_type=0x2, copy_done=0x1, image_ok=0x1` → `Swap type: none`, приложение `confirmed`, обмена нет. Путь «запись `image_ok` в слот 1, пока код выполняется из него» работает на рабочем MCUboot |
| Сброс по ST-Link (перед подтверждением) | **рабочий MCUboot выполнил обмен**: `Swap type: test` 0,61 с → `Starting swap using scratch` 3,45 с → `Jumping` 39,27 с — **обмен ≈36 с**; приложение: `NOT confirmed, next swap type 4` (revert). IWDG прошлого запуска обмен не прервал (сброс аппаратный — программный проверить отдельно) |

Сторож и откат (`hw/logs/02-iwdg-and-revert/`):

| Шаг | Результат |
|---|---|
| `selfcopy` + `request test` | 10,78 с, rc 0; следующий обмен `test` |
| `kernel reboot cold` (программный сброс, IWDG запущен приложением, срок 20 с) | **обмен не прерван**: `Starting swap` 3,49 с → `Jumping` 39,73 с (≈36 с > 20 с); приложение `NOT confirmed`, следующий обмен `revert`. IWDG, запущенный приложением, программный сброс не переживает — MCUboot перепрошивать ради сторожа не нужно |
| `sysupd hang` на неподтверждённом образе (IRQ заблокированы, бесконечный цикл) | **сброс сторожем через 17,4 с** (срок 20 с, кормление раз в 5 с); MCUboot: `Primary image: … image_ok=0x3` → `Swap type: revert`, обмен 20,54 → 56,54 с (36 с); приложение `confirmed`, `reset cause 0x00000011 (watchdog)`. **Цепочка «зависание → IWDG → откат» работает** |

Итог шага 0: на рабочем MCUboot платы A проверены обмен, подтверждение записью через XIP, откат без
подтверждения по сбросу сторожа и то, что сторож не режет обмен после программного сброса.
Не проверено: обрыв питания посреди обмена на этой плате (на плате B — продолжение с места,
`reports/p6/hw` строка 23); запись через XIP во время активной нагрузки (сеть, Matter, приём по
SPI NOR) — будет в приёмке через веб.

### 2026-09-15, модули, mock, экран и связка

Форки (подробности — `notes-*.md` рядом):

| Часть | Итог |
|---|---|
| `modules/system-updater` (форк B) | фазы `preparing → requesting → rebooting`, журнал со стадиями `preparing/requesting/rebooting/booted`, сверка после старта, самоподтверждение по сроку с повтором, `confirm_now`; sim 64/64, мутаций 158 — 155 убиты, 3 эквивалентны. Добавил `upload_consumed` в платформу: после обмена в слоте 2 прежняя прошивка |
| `modules/system-image-store` (форк A) | приём в слот 2, стирание секторов, начинающихся в куске, чтение обратно, ремонт сектора при повторе другого куска, проверка MCUboot потоково; мутации ещё идут |
| mock (форк C) | загрузки с `target`, одна загрузка на оба target, проверка MCUboot по байтам, `getSystemFirmware`/`startSystemUpdate`, перезапуск с молчанием, самоподтверждение, откат; контракт 5/5, pytest 464, мутаций 93 — 90 убиты, 3 эквивалентны. 8 вопросов записаны в `api-contract.md` («Решения этапа») |
| экран «Прошивка» (форк D) | `/firmware`, загрузка с SHA-256 в браузере и продолжением, проверка с версией (понижение — флажок), установка, ожидание смены `boot_id` до 180 с, обратный отсчёт и предупреждение; vitest 455, e2e против mock 31 (+7 новых), gzip 113 650 Б; мутаций 60 — найдена и исправлена ошибка отправки после того, как установка стала недоступна |

Связка (моя часть):

- `src/web/api/v1/system_update.c`: `getSystemFirmware`, `startSystemUpdate` — отказы в порядке
  контракта до создания задачи, понижение версии проверяется до `job_create` (отказ не оставляет
  отменённую задачу под ключом), установка на рабочем потоке v1. Причины недоступности —
  `firmware_unconfirmed`, `update_running`, `service_not_ready`, `not_implemented`.
- `firmware.c`: `target` в `UploadRequest`, выбор хранилища по `target` при создании и по префиксу
  id (`sysimg_`) дальше, одна загрузка на оба target через новую `fw_store_current()` (+1 тест в
  `tests/firmware_store`) и `sys_img_current()`, `Upload.target`, образ STM32 в ответе,
  422 `unsupported_target` при попытке поставить загрузку не на тот процессор. Найдено при чтении
  своего кода: общий статический буфер `find_upload()` использовался бы и с рабочего потока
  (`run_verify`) — переписано на локальную копию.
- `system.c`: `features.stm32_update`, `limits.system_upload_max_bytes`, `mcuboot_image` в
  `firmware_formats`, URL задачи `system_update`; `web_server.c`: `firmware_version` —
  `APP_VERSION_TWEAK_STRING` (git-ревизия осталась в стартовом логе).
- Маршруты и ресурсы: **устройство обслуживает 37 из 37 операций**, проверки контракта 5/5.
- `src/services/system`: платформа хранилища (flash area `slot1_partition`, `slot_locked` = образ
  не подтверждён или запрошен обмен, параметры образа из DT: SRAM, окно PSRAM, окно
  `ext_flash_mem` + `ROM_START_OFFSET`), платформа установщика (bootutil, журнал
  `/lfs/firmware/sysupd.journal` через временный файл, `fs_sync`, переименование, под мьютексом),
  поток `sysupd_tick` (SRAM-стек 4096) — `system_updater_tick()` раз в секунду и
  `sys_img_tick()` раз в минуту. Подтверждение отказывает `-EPERM`, если стек вызывающего не в SRAM.
  Шелл: `sysupd confirm` теперь через установщик (`last_update` следует), прежний — `confirm-raw`.

### 2026-09-15, приёмка через API на плате A (`hw/logs/03-web-acceptance/`)

Образ `06f9bb45…` (1 427 212 Б, `1.0.0+0`, вся связка), прошит по ST-Link. Старт: `IWDG running`,
`running image 1.0.0+0 … confirmed`, `web interface … firmware 1.0.0+0 (df852ea3a0fa-dirty)`.
Сюита `system-image-store` форка A: sim 76/76, мутаций 161 — 149 убиты, 12 эквивалентны.

- Setup на плате A — пароль стенда `cedar-bench-P2-changed` (тот же, что записан раньше).
- `GET /capabilities`: `stm32_update` доступен, `system_upload_max_bytes` 4 128 768,
  `firmware_formats` `["raw_full_flash","mcuboot_image"]`.
- `GET /system/status`: `firmware_version` `1.0.0+0`.
- `GET /system/firmware`: `running` `1.0.0+0`, `image_hash` `021cb39e…151c`, `confirmed` true,
  `confirm_remaining_seconds` null, `swap_pending` false, `update` доступен, `last_update` null.

- **Загрузка STM32 отказала 409 `busy`**: на плате A `firmware-store` (ESP32) держит незавершённую
  загрузку от прежних сессий, а правило «одна загрузка на оба target» его учитывает. id этой
  загрузки неизвестен, файлового шелла в прошивке нет — удалить её через API нельзя, только ждать
  24 ч. Это и дыра в интерфейсе: экран одного процессора не знает загрузку другого (она могла прийти
  из другого браузера). Исправлено: сообщение 409 называет id мешающей загрузки
  («Upload upload_… of the ESP32 is staged; delete it first»), `sysupd status` печатает текущие
  загрузки обоих хранилищ.
- Мешающей оказалась `upload_fe778808ba86d326` — `ready`, 1 468 176 Б, `wifi-cp-merged.bin`
  (проверенный merged-образ C6 от прежних тестов на плате A; `sysupd status` образа `d2ea8f6e…`).
  **Удалена через `DELETE /firmware/uploads/{id}`** (задача `job_00000001`): LittleFS тестовой платы
  владелец разрешил стирать, образ воспроизводится из сборки CP. Для установки ESP32 на плате A его
  нужно загрузить заново.
- **Отказ по содержимому на железе** (образ `d2ea8f6e…`): 64 КиБ случайных байтов с
  `target=stm32u585` — создание, 4 куска в слот 2, проверка; итог `failed`, `invalid_image`,
  «This is not an MCUboot image: the header magic is missing». Весь путь приёма и проверки на плате
  работает. **Скорость — 7,3 КиБ/с**: `PUT` куска 16 КиБ ~400 мс (p50 398, max 418), задача записи
  куска 1,7 с p50 / 2,3 с max (стирание 4 секторов 4 КБ, запись, чтение обратно, метаданные в
  `/lfs`). На образ 1,4 МБ это ~3 мин — не разбиралось, где время.
- **sim `web_api`: 2 из 95 тестов `v1` падали.** `test_capabilities` сравнивает тело целиком —
  ожидание дополнено `stm32_update` (`not_implemented`: сюита собирается без модулей STM32) и
  `system_upload_max_bytes`. `test_a_chunk_the_volume_cannot_take_fails_its_job`
  (`v1_firmware.c:645`, задача куска не `failed` при заполненном томе) **падает и в основном дереве
  на `df852ea`** (прогон той же сюиты из `cedar_switch_3in4out_power`) — не от этапа. Вероятная
  причина — правка P6 после приёмки: проверку места при коммите куска убрали по решению владельца,
  `tests/firmware_store` переписали, а этот тест `web_api` — нет. Не чинился: вопрос к тому, как
  тест должен заполнять том теперь.
- **Приёмка установки через API** (образ `2598fec3…`, `1.0.1+0`, поверх `d2ea8f6e…` `1.0.0+0`;
  `hw/web_update_a.py full`):

  | Шаг | Результат |
  |---|---|
  | `createUpload` (`target=stm32u585`) | 201 за 759 мс (стирание трейлера 64 КиБ на HTTP-потоке) |
  | 88 кусков | 1 427 792 Б за **185 с, 7,5 КиБ/с**; `PUT` p50 393 мс, задача p50 1,7 с / max 2,6 с |
  | `verifyUpload` | **62,7 с** (SHA-256 и разбор по слоту, ≈22,8 КиБ/с чтения SPI NOR), `ready`, `mcuboot_image` `1.0.1+0`, `kind=app`, `allowed_methods ["ota"]` |
  | `startSystemUpdate` | 202 за 503 мс; фазы `preparing` 0,4 с → `requesting` 1,4 с → `rebooting` 2,0 с; консоль: `restarting into the swap` через 2,6 с |
  | Обмен | MCUboot `Swap type: test` → `Starting swap` → `Image version: v1.0.1` → `Jumping` |
  | Возврат | первый ответ HTTP через **64,8 с** после запроса, новый `boot_id`; сессия прежней загрузки недействительна, вход заново |
  | После старта | консоль: `running image 1.0.1+0 … NOT confirmed`, `update job_00000061: the new firmware runs, awaiting confirmation`, `confirms itself in 1200 s` |
  | `GET /system/firmware` | `running` `1.0.1+0` (`64b0098d…`), `confirmed` false, `confirm_remaining_seconds` 1180, `last_update` `awaiting_confirmation` `1.0.0+0 → 1.0.1+0`, `update` недоступен `firmware_unconfirmed`; capabilities `stm32_update` тоже |

  **Самоподтверждение по сроку** (18:49:40, uptime 1 289 с, тот же `boot_id` — без перезапуска):
  `running.confirmed` true, `confirm_remaining_seconds` null, `last_update` **`succeeded`**
  `1.0.0+0 → 1.0.1+0`, `update` и `stm32_update` снова доступны. Подтверждение записал поток
  `sysupd_tick` через XIP-драйвер при работающих сети, Matter и HTTP. Консоль в момент подтверждения
  не записана: фоновая запись консоли была снята системой по нехватке памяти хоста.
  **Подтверждение на флеш**: сброс по ST-Link в 18:50 — MCUboot `Primary image: magic=good,
  swap_type=0x2, copy_done=0x1, image_ok=0x1` → `Swap type: none`, `v1.0.1`; приложение
  `running image 1.0.1+0 … confirmed, next swap type 1`. Полный путь владельца — загрузка через веб,
  обмен, 20 минут работы, подтверждение без перезапуска, сохранение после сброса — пройден.

  **Расхождение: `SystemStatus.firmware_version` новой прошивки — `1.0.0+0`** (и в стартовом логе
  `web interface … firmware 1.0.0+0`), хотя заголовок и MCUboot — `1.0.1+0`. Причина — сборка
  второго образа: инкрементальная сборка после смены `VERSION` не перекомпилировала
  `web_server.c` (`web_server.c.obj` 18:18:25 старше `app_version.h` 18:20:03, в его файле
  зависимостей `app_version.h` нет), в `zephyr.elf` оба вхождения — `1.0.0+0`. Строка версии,
  вшитая при компиляции, может разойтись с тем, что запущено; исправление — `firmware_version`
  берётся из заголовка слота 1 при старте (`system_service_running_version()`), вшитая строка —
  только запасная.
- **sim после связки** (18:30): `web_api_http` — все проходят; `web_api` — 188 из 189 (единственный
  отказ — унаследованный `test_a_chunk_the_volume_cannot_take_fails_its_job`); `firmware_store`
  79/79; `system_updater` 64/64 и `job_manager` — проходят; `system_image_store` 76/76 (форк A).
  Ветки STM32 привязок в sim не выполнялись: сюиты `web_api` собираются без модулей STM32.
- **Контракт на итоговом дереве** (18:57, `tests/ci/run-contract-tests.sh`): 5/5 проверок, устройство
  обслуживает 37 из 37 операций, mock pytest 464, web-assets 14.
- **`tests/web_api_system` (форк E, `notes-bindings-sim.md`)**: настоящие firmware-store и
  system-image-store на общем LittleFS, слот 2 на симуляторе flash 4 МиБ, system-updater на фейке
  с настоящим хранилищем за `staged_image`/`set_upload_in_use`/`upload_consumed`. **21/21**:
  создание и повтор, `target` по умолчанию, одна загрузка на оба target в обе стороны (сообщение с
  id), порядок отказов создания, куски/проверка/удаление через слот, кусок при заблокированном слоте,
  отказы проверки, порядок отказов `startSystemUpdate`, понижение без флажка (задачи под ключом не
  остаётся) и с ним, переустановка, установка до перезапуска на рабочем потоке, сетевая транзакция
  → busy, отмена до `requesting`, `startCoprocessorUpdate` с `sysimg_` → 422, `getSystemFirmware`
  через установку, обмен и откат, capabilities, 503 без `web_api_v1_set_system`. **Дефектов в
  привязках не найдено.** Мутации: 56 — 52 убиты, 2 эквивалентны, **2 выжили — пробел**: не
  проверено, что идущая установка ESP32 блокирует установку STM32 и даёт `update_running`
  (нужен coprocessor-updater посреди установки на фейке).
- **Понижение версии** (`hw/logs/04-downgrade-rollback/`): на `1.0.1+0` (подтверждён) загружен
  `31628617…` `1.0.0+0` — 197 с (7,1 КиБ/с), проверка 62,6 с, `ready`. `startSystemUpdate` без
  `acknowledge_downgrade` → **422 `validation_failed`** «The image is older than the running
  firmware; set acknowledge_downgrade to true» за 93 мс, без перезапуска. С `acknowledge_downgrade=true`
  → 202, фазы `preparing` 0,4 с → `requesting` 0,6 с → `rebooting` 1,4 с; первый ответ через **55,5 с**;
  `running` `1.0.0+0` (`ce51b613…`), не подтверждён, `confirm_remaining_seconds` 1189,
  `last_update` `awaiting_confirmation` `1.0.1+0 → 1.0.0+0`; **`SystemStatus.firmware_version`
  `1.0.0+0` — совпадает с заголовком** (образ с версией из заголовка слота 1).
- **Откат по сбросу до подтверждения** (18:57, сброс по ST-Link на 13-й секунде работы `1.0.0+0`):
  MCUboot `Primary image: … image_ok=0x3` → `Swap type: revert`, обмен 3,48 → 39,18 с; приложение
  `running image 1.0.1+0 … confirmed`, `system_updater: update job_0000005a: rolled_back`.
  `GET /system/firmware`: `running` `1.0.1+0` (`64b0098d…`, прежний хеш), `confirmed` true,
  `last_update` **`rolled_back`** `1.0.1+0 → 1.0.0+0`, `error` `boot_changed` «The new firmware was
  reset before it was confirmed; MCUboot restored the previous one», `update` снова доступен.
  (`firmware_version` `1.0.0+0` здесь — тот же артефакт сборки `2598fec3…`, что выше.)
- **Второй образ с другой версией так не собрать**: `-Dcedar_p7_VERSION_FILE=…` sysbuild в образ не
  передаёт (`version.cmake` читает обычную переменную) — образ вышел `1.0.0+0`. Передавать версию
  только в imgtool (`-Dcedar_p7_CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION`) нельзя: заголовок и
  `SystemStatus.firmware_version` разойдутся. Второй образ собирается с временно изменённым
  `VERSION` в отдельном каталоге сборки.

Попутно: после прошивки образом worktree на плате A веб пишет `no administrator yet: setup is open`. У
`main` `df852ea` бэкенд настроек File (`/lfs/settings`), переход на ZMS лежит в основном дереве
незакоммиченным; прежний образ платы A хранил администратора в ZMS. К обновлению отношения не
имеет, но на стенде `.14` сейчас открыт setup.
- **XIP-драйвер и правка SysTick записаны в `patches/`**:
  `zephyr/flash_stm32_ospi-xip-safe-program-erase.patch` (sha256 `57dba3e1…2dc1`, 7 файлов, +895),
  снят `git diff HEAD` в zephyr `b6a5e6e8aa9`; обратный `git apply --check` по дереву проходит,
  **дерево не менялось** — правка там уже лежала. Правки таймера ESP32 в том же каталоге — чужие,
  в запись не входят.
