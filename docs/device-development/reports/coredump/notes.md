# Coredump при падении (2026-09-18/19)

Запрос владельца: «Включи сохранение стек трейса при падении, и добавь апи поддержку
в веб интерфейсе для выгрузки трейса». Решения владельца по ходу:

- только штатный Zephyr coredump (своя запись в BBRAM, сделанная сначала, удалена);
- уровень `THREADS` — все потоки и их стеки;
- сначала — SPI NOR, затем, после вопроса «кордамп может писаться только во
  внутреннюю память?», — **во внутреннюю флеш**;
- **любое изменение дерева Zephyr — только с разрешения**, каждый раз. Две правки,
  сделанные для SPI NOR без спроса (`drivers/flash/spi_nor.c`,
  `subsys/debug/coredump/coredump_backend_flash_partition.c`), откачены по просьбе
  владельца. Для coredump дерево Zephyr не изменено.

## Как устроено

- `prj.conf`: `DEBUG_COREDUMP`, `DEBUG_COREDUMP_BACKEND_FLASH_PARTITION`,
  `DEBUG_COREDUMP_MEMORY_DUMP_THREADS`, `DEBUG_COREDUMP_SHELL`,
  `EXTRA_EXCEPTION_INFO=y` (r4–r11 в дампе — без них GDB не раскрутит упавший поток;
  было `=n` с первого коммита), `ARCH_STACKWALK=n` (консольный call trace не нужен,
  его таблицы ~100 КБ), `RESET_ON_FATAL_ERROR=y` (сброс сразу после дампа, а не
  через ~20 с по IWDG), `DEBUG_THREAD_INFO=y` (его выбирает THREADS).
- Раздел: `coredump_partition` во **внутренней флеш** STM32U585, `0x08020000`,
  256 КБ, сразу после MCUboot (128 КБ); остальная внутренняя флеш не занята
  (`boards/…ext_flash_app.overlay`). Приложение исполняется с OSPI, во внутреннюю
  флеш при работе никто не пишет: запись из обработчика падения не трогает ни код,
  ни общую шину. У `flash0` в DT есть `write-block-size` (16) и
  `erase-block-size` (8 КБ) — штатный бэкенд берёт её как есть.
- `src/diagnostic/coredump_support.c`:
  - **`assert_post_action()` переопределён**: внутри обработчика исключения
    (IPSR 3–6, и 11 — SVC, через него идут `k_panic`/`k_oops`) сработавший assert
    сообщается и игнорируется, в остальных местах — `k_panic()` как у Zephyr.
    Причина: дамп пишется из обработчика исключения, а `flash_stm32` берёт свой
    семафор с `K_FOREVER`, и `k_sem_take()` проверяет assert'ом, что ISR не ждёт.
    При `CONFIG_ASSERT=y` без переопределения это вложенное падение и нет дампа.
    Драйвер ждёт флеш опросом, без `k_sleep`. Возврат из `assert_post_action()`
    допустим только при **`CONFIG_ASSERT_TEST=y`** (см. «Проверка на плате»); без него
    сборка падает на `#error`.
  - хуки для API (`web_api_v1_set_coredump()`): размер (`HAS_STORED_DUMP` проверяет
    контрольную сумму — оборванный дамп не отдаётся), чтение (`COPY_STORED_DUMP`),
    удаление (`INVALIDATE_STORED_DUMP`, одна страница 8 КБ);
  - при загрузке, если дамп есть, — `LOG_ERR "coredump stored: … bytes, reason …"`
    (попадает и в выгрузку логов).
- `CONFIG_STREAM_FLASH_ERASE=n` — обязательно, см. «Проверка на плате».
- API v1 (`openapi.json`, `routes.h`, `http_resources.h`, `system.c`):
  - `GET /system/coredump` → `{"coredump": null | {size_bytes, reason, reason_code}}`,
    причина из заголовка дампа (`ZE`, смещение 8);
  - `GET /system/coredump/data` → `application/octet-stream`, потоком,
    `cedar-coredump.bin`; 404 `not_found`, если дампа нет;
  - `DELETE /system/coredump` (CSRF) → 204;
  - без хуков (сборка без coredump) — 503 `capability_unavailable`.
- Веб-интерфейс: карточка «Дамп памяти при падении» на экране «Логи»
  (`src/web/frontend/src/features/logs/CoredumpCard.tsx`): размер, причина, «Скачать
  дамп» (ссылка на `/data`), «Удалить дамп».

## Разбор дампа

С ELF **той же** сборки, что упала:

    python3 zephyr/scripts/coredump/coredump_gdbserver.py zephyr.elf cedar-coredump.bin
    arm-zephyr-eabi-gdb zephyr.elf -ex "target remote localhost:1234"
    (gdb) bt
    (gdb) info threads

ELF и образ сборок этой сессии сохранены в `~/Downloads/cedar-firmware/`
(`1.1.1-41df4595/` — стоит на плате, без coredump; `1.1.1-7ddc4847/` — с coredump и командой `crash`).

## Чего дамп не покрывает

- Дамп больше раздела (256 КБ) бэкенд не усекает, а бракует (`-ERANGE` → ошибка в
  заголовке → «not found»). Сейчас 99 КБ на 28 потоков.
- Зависание, которое сбрасывает IWDG (прерывания заблокированы, голодание
  кооперативного потока), не вызывает обработчик падения — дампа нет. Теперь это
  различимо: падение даёт дамп и причину сброса «software», зависание — причину
  «watchdog» и без нового дампа.
- «Жива, но без сети» (плата 192.168.88.10, 22:37, 15 минут до сброса питания) —
  не падение: IWDG кормится, дампа не будет.
- Падение во время записи во флеш с XIP (сама флеш OSPI в косвенном режиме) —
  обработчик исполняется с этой флеш и может не отработать.

## Проверки

- Контракт: `tests/ci/run-contract-tests.sh` — 469 + 14 passed; `undocumented_routes`
  ок (маршруты сходятся с документом). В mock: coredump переживает перезагрузку,
  `POST /__mock/crash` сохраняет дамп и перезапускает, сценарий `coredump_stored`.
- Sim: `cedar.web_api` — 9 тестов coredump (без хуков 503, без сессии 401, нет дампа,
  описание, заголовок без `ZE`, выгрузка кусками побайтно, обрыв чтения рвёт
  выгрузку, ошибка хранилища 500, удаление/CSRF/идемпотентность) проходят; в наборе
  падает только давний `test_a_chunk_the_volume_cannot_take_fails_its_job` (падает с
  `df852ea`).
- Весь sim-уровень (`tests/ci/run-sim-tests.sh --tag sim`): 1020 из 1021 тестов, 21 из 22
  конфигураций; единственный провал — тот же давний тест в `cedar.web_api`.
- Фронтенд: `SKIP_E2E=1 tests/ci/run-frontend-tests.sh` — 26 файлов, 462 теста;
  бюджет 114 517 Б gzip из 524 288.
- Прошивка собрана (чистая сборка в scratch, `ZAP_INSTALL_PATH` на cipd zap):
  образ 1467180 Б, sha256 `7ddc4847…`, `assert_post_action` приложения
  заменяет слабый Zephyr, раздел в DTS на `0x20000` внутренней флеш.
- Плата — см. следующий раздел.

## Проверка на плате (2026-09-19, тестовая плата `.21`, ST-Link)

Всё, что было до этого дня, **дампа не писало**: `crash fault` → нет дампа, сброс по IWDG.
Две ошибки, обе найдены отладчиком (pyocd attach + GDB, стек и регистры в момент
зависания) и исправлены только в приложении, дерево Zephyr не менялось.

1. **Возврат из `assert_post_action()`.** `__ASSERT()` ставит после вызова
   `CODE_UNREACHABLE`, если нет `CONFIG_ASSERT_TEST`. Компилятор считает, что вызов не
   возвращается, и кладёт за ним что угодно. Переопределение возвращалось → в
   `k_sem_take()` (семафор `flash_stm32`, `count 1`) исполнение провалилось из ветки
   assert'а в ветку «ждать» → `z_pend_curr()` → опять assert'ы → литеральный пул
   (`0x0208feb0`, данные как код) → UNDEFINSTR внутри обработчика UsageFault →
   HardFault (HFSR FORCED) → снова `coredump()` → тот же путь → остановка до IWDG. На
   стеке поэтому было два `flash_stm32_erase`, а семафор был свободен.
   Исправление: `CONFIG_ASSERT_TEST=y` (Kconfig Zephyr ровно для «хук может
   вернуться»), в `coredump_support.c` — `#error` без него.
   Ловушка сборки: после смены `prj.conf` инкрементальная сборка **не пересобрала**
   ядро (`sem.c.obj` остался старым, код тот же) — нужна чистая сборка (`-p always`).
2. **`CONFIG_STREAM_FLASH_ERASE` портит дамп.** После исправления 1 сброс стал
   «software», дамп записан (99 029 Б, ошибок нет), но `coredump find` — «not found»:
   не сходится контрольная сумма. В разделе первые 16 байт каждой страницы 8 КБ, кроме
   первой, — `0xFF` (12 страниц, 192 байта). Причина в `stream_flash`
   (`subsys/storage/stream/stream_flash.c`, `stream_flash_erase_to_append`):
   `erased_up_to` считается от `ctx->offset`, а стирается страница от своей границы;
   бэкенд начинает запись с `0x20010` (раздел + заголовок 16 Б), поэтому каждая
   следующая страница стирается, когда её первые 16 байт уже записаны. Бэкенд и так
   стирает раздел целиком в начале, стирание по ходу ему не нужно. Опцию включал
   `prj.conf` (с первого коммита, пользователей не было) и `select` в
   `SYSTEM_IMAGE_STORE_STREAM_FLASH`. Исправление: `CONFIG_STREAM_FLASH_ERASE=n`,
   хранилище образа стирает секторы куска само (`p->erase`) перед `stream_flash`.
   В апстриме Zephyr (последний коммит файла в нашем дереве, `fc08b0535ed`) не
   исправлено, задач на GitHub не нашёл. Своё хранилище не задето: оно начинает
   `stream_flash` только с границы сектора.

Проверено на `.21` (сборка `build-tm`: код приложения + `SYSTEM_IMAGE_STORE_TIMING`):
- `crash fault` в telnet → сброс «pin software» через несколько секунд, не IWDG;
  `coredump find` — found, `coredump verify` — verified;
- `GET /system/coredump` → `{"size_bytes":99029,"reason":"cpu_exception","reason_code":36}`;
  `/data` → 99 029 Б, `application/octet-stream`, `cedar-coredump.bin`;
- `coredump_gdbserver.py` + GDB с ELF этой сборки: `fault_now()`
  (`coredump_support.c:155`) ← `cmd_crash` ← shell в потоке `shell_telnet`; `info
  threads` — 28 потоков с именами;
- `crash oops` и `crash panic` (через SVC, IPSR 11) — тоже дамп, `verified`, сброс
  «software»; от команды до ответа платы 19–20 с при аптайме после загрузки ~16 с, то
  есть запись дампа (~99 КБ) и сброс — 3–4 с, до IWDG (20 с) запас большой;
- `DELETE /system/coredump` → 204, затем `{"coredump":null}` и `/data` 404;
- `coredump erase` из потока (256 КБ внутренней флеш) — плата работает дальше; то есть
  выключение ICACHE драйвером на время стирания исполнению с `0x02…` не мешает
  (версия про ICACHE в `reports/upload-speed` была неверной и убрана).

Сборки (ELF рядом с образом, для разбора дампов): `.21` — `~/Downloads/cedar-firmware/1.1.1-aec0917b-timing-board21/`;
рабочая без `TIMING`, для `.10` (не ставилась) — `1.1.1-6932137c/`, 1 473 040 Б.

Хранилище образа после смены (стирает само, `STREAM_FLASH_ERASE=n`):
- sim `cedar.system_image_store` — 79 из 79 (два теста stream-пути ждали «платформа не
  стирает», исправлены на «стирает секторы куска одним вызовом»); весь sim-уровень —
  провал только давнего `test_a_chunk_the_volume_cannot_take_fails_its_job` в
  `cedar.web_api`;
- на `.21` по Ethernet: 1 467 468 Б кусками 4096 без паузы за 645 с (2,2 КиБ/с), 0
  сбросов, проверка образа 71 с, `ready`. На кусок p50: стирание 52 мс + запись 74 мс
  (вместе — как прежние 127 мс у `stream_flash` со стиранием), проверка 38 мс,
  метаданные 893 мс.

Следствие для прошлых выводов: пока дамп не писался, «сброс по IWDG и нет дампа» не
отличал зависание от падения. Выводы «все сбросы — зависания» в `reports/upload-runs`
этим не подтверждены.
