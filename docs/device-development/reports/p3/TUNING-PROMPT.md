Настраиваем бэкенды settings ZMS и File на максимальную скорость чтения и записи
для Matter. Рабочий каталог —
/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power

Сначала прочти:
- docs/device-development/reports/p3/STATUS.md — состояние P3, почему хранилище
  стало узким местом, сравнение четырёх бэкендов
- docs/device-development/reports/p3/settings-backends/ — `run_backend.py`
  (методика на плате), `summarize.py`, `phases.py`, `configs/`, `summary.md`,
  `phases.md`, данные прошлых прогонов
- docs/device-development/reports/p3/lfs-cache/ — прошлый замер кешей LittleFS
- src/diagnostic/storage_bench.c — отладочные `storage_bench` и `storage_wipe`
- в дереве Zephyr: subsys/settings/Kconfig, subsys/settings/src/settings_{zms,file,line}.c,
  subsys/kvss/zms/Kconfig, subsys/fs/littlefs_fs.c, drivers/spi/spi_stm32.c
- modules/lib/matter/src/platform/Zephyr/KeyValueStoreManagerImpl.cpp — как
  Matter читает ключи (`settings_load_one`, `settings_get_val_len`)

Память проекта подхватится сама.

## Что уже известно

- Хранилище — SPI NOR на `spi1` (80 МГц в DTS, опрос без DMA), та же шина, что FRAM.
  File: LittleFS на `storage_partition` (0xA00000, 5 МБ), `/lfs/settings`.
  ZMS в сравнении: 32 сектора на `storage_lfs` (0x0, не смонтирован прошивкой),
  через `chosen zephyr,settings-partition`.
- File не имеет `csi_load_one`: каждое чтение ключа = полная загрузка с поиском
  дубликатов (N²). ZMS читает ключ по хешу.
- Базовые цифры (10 циклов, `settings-backends/summary.md`): ZMS старт Matter
  0,6 → 6,2 с, удаление fabrics 2,0 / 6,7 с; File 3,2 → 19,0 с пилой до ~50 с,
  удаление 29,7 / 60,8 с. У ZMS рост линейный по числу удалённых записей.
- Commissioning на ~70% — программная криптография P-256 (PASE ~10 с, аттестация
  3–5 с, CSR ~4 с); от бэкенда не зависит. Сравнивать по фазам хранилища.
- Владелец ранее пробовал ZMS без ускорения; кеш LittleFS в прошлом замере
  выигрыша не дал. Не повторять выводы без нового замера.

## Задача

Для каждого из двух бэкендов, ZMS и File, найти комбинацию настроек, дающую
наибольшую скорость. **Лимит — до 200 попыток на бэкенд.**

Алгоритм владельца — по одному фактору:

1. Базовая конфигурация бэкенда (для ZMS — `configs/zms.conf`, для File —
   прошивка как есть, `configs/file.conf`).
2. Меняем **одну** настройку (например, размер кеша), собираем образ, прогоняем
   **5 commissioning Matter** по методике ниже, сравниваем с базой.
3. **Откатываем** эту настройку назад и пробуем следующую (или следующее значение
   той же настройки).
4. Когда перебор закончен — собираем комбинацию лучших значений и подтверждаем её
   тем же прогоном, а затем удлинённым (10 циклов), чтобы увидеть рост со временем.

Одна попытка = один образ = один прогон из 5 циклов. Попытки, упавшие из-за
сборки, скрипта или консоли, в лимит не входят, но записываются.

### Методика одной попытки (как в сравнении бэкендов)

1. Прошить образ, `storage_wipe yes` — полностью стереть раздел бэкенда.
2. Старт на чистом разделе, дать бэкенду создать структуру.
3. Перезагрузка, проверить, что структура прочитана (`mt/ctr/reboot-count` вырос на 1,
   `mt/cfg/unique-id` тот же).
4. 5 циклов: перезагрузка → commissioning (chip-tool) → перезагрузка → удаление fabrics.
5. В конце `storage_bench 3` и проверка сохранности (счётчик перезагрузок = число загрузок).

`run_backend.py <backend> <cycles>` уже делает это — доработай его под попытки
(имя попытки, путь к образу, каталог результатов) и поправь пути: скрипты
писались для scratchpad прошлой сессии (`HERE`, `P3`).

### Метрики и оценка

На каждой попытке:
- время старта Matter (`Init CHIP stack` → `Matter stack initialized`) до и после
  commissioning, по каждому циклу;
- удаление fabrics (`matter fabric reset` → `Matter fabrics after delete`);
- фазы commissioning (`phases.py`): **`commit`** и **`noc_to_done`** — фазы хранилища;
  `pase`, `attestation`, `csr` — контроль, должны быть стабильны;
- время открытия окна;
- `storage_bench`: `load_one`/`val_len` hit и miss, скан;
- успешность commissioning и сохранность.

Основная оценка попытки (зафиксируй до начала и не меняй по ходу): медиана
старта Matter с fabric + медиана удаления fabrics + медиана фазы `commit` по 5
циклам; дополнительно — наклон роста старта Matter от цикла 1 к 5. Попытка
лучше базы, только если выигрыш больше шума.

**Шум измерь до перебора:** 3 прогона базы подряд для каждого бэкенда. Разброс
базы — порог значимости. Повторяй базу каждые ~20 попыток и при подозрительном
результате — дрейф платы (износ, температура, состояние C6) не должен выглядеть
как эффект настройки.

### Что перебирать (сверь каждое имя с Kconfig/DTS этого дерева до использования)

ZMS:
- `CONFIG_ZMS_LOOKUP_CACHE` вкл/выкл, `CONFIG_ZMS_LOOKUP_CACHE_SIZE`
- `CONFIG_ZMS_LOOKUP_CACHE_FOR_SETTINGS`
- `CONFIG_SETTINGS_ZMS_LL_CACHE`, `CONFIG_SETTINGS_ZMS_LL_CACHE_SIZE`
- `CONFIG_SETTINGS_ZMS_LOAD_SUBTREE_PATH`
- `CONFIG_SETTINGS_ZMS_SECTOR_COUNT`, `CONFIG_SETTINGS_ZMS_SECTOR_SIZE_MULT`
- `CONFIG_SETTINGS_ZMS_MAX_COLLISIONS_BITS`, `CONFIG_SETTINGS_ZMS_NO_LL_DELETE`
- размер буфера ZMS (`CONFIG_ZMS_CUSTOMIZE_BLOCK_SIZE` и связанный размер), `CONFIG_ZMS_NO_DOUBLE_WRITE`
- расположение раздела (другой адрес/размер на SPI NOR)

File:
- `CONFIG_SETTINGS_FILE_MAX_LINES` (живых записей ~17 без fabrics, ~37 при 2, оценка ~58 при 5)
- LittleFS в fstab (`boards/..._ext_flash_app.overlay`, узел `lfs0`): `read-size`,
  `prog-size`, `cache-size`, `lookahead-size`, `block-cycles` — после смены
  `prog-size`/геометрии раздел обязательно стирать (методика это делает)
- `CONFIG_FS_LITTLEFS_FC_HEAP_SIZE`, `CONFIG_FS_LITTLEFS_NUM_FILES`
- размер раздела LittleFS

Общее для обоих (путь чтения/записи на флеш):
- `spi-max-frequency` `spi_flash` (через overlay), `use-fast-read`
- `CONFIG_SPI_STM32_INTERRUPT`; DMA (`CONFIG_SPI_STM32_DMA` + `dmas` у `spi1` в
  overlay) — если каналы DMA для SPI1 на STM32U585 свободны
- `CONFIG_SPI_NOR_SLEEP_WHILE_WAITING_UNTIL_READY`, `CONFIG_SPI_NOR_SLEEP_ERASE_MS`

Для числовых настроек начинай с грубой сетки (×2/÷2 от базы), уточняй вокруг
лучшего. Не перебирай значения, про которые код дерева уже говорит, что они не
влияют на путь Matter, — сначала прочитай, где настройка используется.

### Ведение результатов

- Каждая попытка: `docs/device-development/reports/p3/tuning/<backend>/<NNN>-<настройка>-<значение>/`
  с `*.jsonl`, сжатым логом консоли, конфигом/overlay и хешем образа.
- Сводная таблица `tuning/<backend>/results.md`, обновлять после каждой попытки:
  номер, изменение, оценка, метрики, решение (лучше/хуже/в пределах шума), причина сбоя.
- Итог `tuning/README.md`: лучшая комбинация для каждого бэкенда, подтверждающие
  прогоны, сравнение с базой и между ZMS и File, что не помогло и почему.

## Как работать

- **Сначала проверь, что база проходит** (и сборка, и методика) — попытка против
  сломанной базы ничего не значит.
- **Не запускай сборку или правку исходников параллельно с прогоном на плате или
  sim-прогоном из того же рабочего дерева** — в P3 так прошивка собралась с
  мутированным кодом. Сборки пробных образов — в scratchpad, изменения настроек —
  через `EXTRA_CONF_FILE`/`EXTRA_DTC_OVERLAY_FILE`, репозиторий не менять ради попытки.
- Сборки можно готовить пачкой заранее (разные каталоги), но на плату — по одной.
- Консоль платы теряет входные байты: команды шелла отправлять с ожиданием
  подтверждения и повтором (`send_acked` в `run_backend.py`); `storage_wipe` и
  `storage_bench` молчат до конца — ждать их финальную строку.
- Не делай выводов из одного прогона; сверяй с шумом базы.
- **Не правь вендорский Matter** (`modules/lib/matter`). Если нужна правка Zephyr —
  останавливайся и приноси варианты владельцу.
- **Не убирай отладочный код** (команды шелла Matter, `storage_bench`/`storage_wipe`).
- **Коммиты не делай сам.** Оставь всё в рабочем дереве и скажи, что во что складывается.
- Время: попытка ≈ 12–15 мин (прошивка ~50 с, 5 циклов по ~2 мин, стирание 15–30 с).
  400 попыток — это дни. Сообщай владельцу прогресс и спрашивай, продолжать ли,
  когда перебор по очевидным настройкам исчерпан раньше лимита.

## Плата

ST-LINK V3SET, серийник 002F002B3233510739363634; консоль ищется по серийнику
(`tests/bench/bench_console.py`), если порт занят — CLion (`lsof /dev/cu.usbmodem*`).
Плата 192.168.88.14 (en7), telnet-шелл на 23. Прошивка только приложения, MCUboot не
трогать:

    CLI="/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/Resources/bin/STM32_Programmer_CLI"
    EL=$PWD/CLIVEONE-W25Q128_STM32U545-PB10-PA4-PB1-PB0-PA7-PA6.stldr
    "$CLI" -c port=swd sn=002F002B3233510739363634 mode=NORMAL -el "$EL" -w <signed.bin> 0x90000000 -v
    "$CLI" -c port=swd sn=002F002B3233510739363634 mode=NORMAL -hardRst

Сборка (фронтенд должен быть собран: `cd src/web/frontend && npm ci && npm run build`):

    cd /Volumes/Programming/Zephyr/zephyr_latest
    .venv/bin/west build -p always -b cedar_switch_3in4out_power_rev3/stm32u585xx/ext_flash_app \
      --sysbuild -d <scratchpad-dir> cedar_switch_3in4out_power -- \
      "-Dcedar_switch_3in4out_power_EXTRA_CONF_FILE=<common.conf>;<trial.conf>" \
      -Dcedar_switch_3in4out_power_EXTRA_DTC_OVERLAY_FILE=<trial.overlay>

chip-tool: `modules/lib/matter/out/chip-tool/chip-tool` (`pairing onnetwork-long <node> 20202021 3840`,
отдельный `--storage-directory` на бэкенд). На плате нет важных данных, всё можно стирать
(решение владельца). Веб-пароль после стирания отсутствует; `reports/p3/hw/setup_admin.py`
задаёт `cedar-bench-P2-changed`. Сбросы и прошивки платы в рамках этой задачи разрешены.

Не используй каталог build/ в репозитории — только scratchpad.
