# Устройство драйвера `memc_mspi_qspi_psram`

Описание по коду ветки `driver/ospi_psram` на коммите **`869e497cf70`**
(02.09.2026), репозиторий `/Users/kiro/CLionProjects/zephyr`. Номера строк —
`drivers/memc/memc_mspi_qspi_psram.c` на этом коммите.

## Файлы PR

| Файл | Строк | Назначение |
|---|---|---|
| `drivers/memc/memc_mspi_qspi_psram.c` | 1007 | драйвер; отдельного `.h` нет (влит в `.c` 17.08) |
| `drivers/memc/Kconfig.mspi` | +11 | `CONFIG_MEMC_MSPI_QSPI_PSRAM` |
| `drivers/memc/CMakeLists.txt` | +1 | `zephyr_library_sources_ifdef(...)` |
| `dts/bindings/mtd/qspi-psram-mspi.yaml` | 218 | биндинг `compatible: "qspi-psram"`, `include: [mspi-device.yaml]`, `title:`, `examples:` |
| `samples/drivers/memc/boards/b_u585i_iot02a_mspi_qspi_psram.overlay` | 68 | CI: `chip-variant = "ESP64H"` |
| `…_mspi_qspi_psram_generic.overlay` | 24 | CI: `#include` первого + `/delete-property/ chip-variant` + параметры |
| `…_mspi_qspi_psram_auto.overlay` | 69 | CI: `chip-variant = "AUTO"` |
| `…_mspi_qspi_psram.conf` | 7 | LOG, MSPI_MEMMAP |
| `samples/drivers/memc/tests.yaml` | +30 | `sample.drivers.memc.stm32_mspi.qspi_psram{,.generic,.auto}`, `build_only` |

CI-сборки идут на `b_u585i_iot02a/stm32u585xx`, где физически стоит октальный
APS6408L — поэтому только сборка (в overlay это оговорено). OCTOSPI1 платы
пересоздаётся как `st,stm32-ospi-controller`, так же как в штатном
`b_u585i_iot02a_mspi_aps6408l.overlay`.

## Три режима

| | Именованный | AUTO | Generic |
|---|---|---|---|
| Как задаётся | `chip-variant = "ESP64H"` и т. п. | `chip-variant = "AUTO"` | `chip-variant` отсутствует |
| `cfg->chip` / `cfg->auto_detect` | `&chip_table[idx]` / false | NULL / true | NULL / false |
| Таблица в образе | только нужная запись (+ пустые слоты до её индекса) | все 9 записей, 396 байт | нет |
| Идентификация | `qspi_psram_verify_id()`: KGD и id/mask → **только `LOG_WRN`** | `qspi_psram_auto_detect()`: совпадение или `-ENODEV` | `verify_id` с `kgd_value = 0` → `LOG_INF` с ID |
| Команды, dummy, длины | из записи по `mspi-io-mode` | из найденной записи | из DT (`tar_dev_cfg`) |
| Размер | `size_bits` записи, DT `size` игнорируется | `size_bits` найденной записи; другое DT `size` → `LOG_WRN` | DT `size`, иначе `-EINVAL` |
| CE-тайминг | запись (ESP 8 мкс, ISSI 4 мкс; 1024 Б) | худший грейд семейства (AP 3, ISSI 1 мкс); каждая **ненулевая** ячейка `ce-break-config` переопределяет своё поле | `ce-break-config` из DT |
| Init-команды | `QSPI_PSRAM_STD_QPI_PARAMS` | дефолты биндинга до детекта, затем запись | дефолты биндинга (`35h/F5h/66h/99h/9Fh`), переопределяемы |

## Последовательность инициализации (`memc_mspi_qspi_psram_init`, `:718`)

1. `chip` = `cfg->chip`, либо `generic_params` из init-команд DT с
   `kgd_value = 0`.
2. `device_is_ready(cfg->bus)`.
3. `qspi_psram_check_dt_cfg()` (`:677`):
   - `mspi-io-mode` ∉ {SINGLE, QUAD} → `-ENOTSUP` (у чипов 4 линии, других
     режимов нет — это свойство устройства);
   - `mspi-data-rate` ≠ SINGLE → `-EIO`;
   - только в generic: нет `read/write-command` или `command/address-length` →
     `-EINVAL`; `rx-dummy = 0` → предупреждение.
4. `qspi_psram_force_spi_mode()` (`:635`) — **безусловно**:
   контроллер в QUAD (ошибка → `-EIO`) → `F5h`, `66h`, `99h` по 4 линиям
   (результаты команд игнорируются: чип в SPI их не декодирует) → 200 мкс →
   контроллер обратно в `spi_init_cfg` (ошибка → `-EIO`).
   Спросить чип о его режиме нельзя (Read ID в QPI недоступен), поэтому
   выход из QPI вслепую — единственный способ получить известное состояние
   после тёплого сброса.
5. `qspi_psram_reset()` — `66h`/`99h` в SPI.
6. AUTO → `qspi_psram_auto_detect()`, `chip` = найденная запись;
   иначе → `qspi_psram_verify_id()`.
7. Размер (см. таблицу режимов).
8. Только QUAD: `35h` (Enter QPI) по одной линии + 100 мкс.
9. `data->dev_cfg = cfg->tar_dev_cfg`; для именованного и AUTO
   переопределяются `read_cmd`, `write_cmd`, `rx_dummy` (по режиму),
   `cmd_length`, `addr_length`, `tx_dummy`, `mem_boundary`, `time_to_break`;
   в AUTO — worst-case tCEM и ненулевые ячейки `ce-break-config`.
10. `mspi_dev_config(MSPI_DEVICE_CONFIG_ALL, &data->dev_cfg)`.
11. `CONFIG_MSPI_MEMMAP` и `memmap-config` включён: `size` окна ≤ ёмкость
    (иначе `-EINVAL`) → `mspi_memmap_config()` → копия в `data->memmap_cfg`.
12. `CONFIG_MSPI_SCRAMBLE` — аналогично.
13. Освобождение шины, если не PM runtime auto.
14. `LOG_INF("PSRAM initialised in %s mode, %u KB", "QPI"|"SPI", …)`.

### `spi_init_cfg` — отдельный конфиг фазы инициализации

`QSPI_PSRAM_SPI_INIT_CFG(n)` (`:938`): 24 МГц, `MSPI_IO_MODE_SINGLE`, SDR,
`cmd_length = 1`, `addr_length = 0` (ADDR_DISABLED), без dummy и DQS;
`cpp`/`endian`/`ce_polarity` и `ce_num` берутся из DT (описывают проводку, а не
протокол). Это не дублирование `tar_dev_cfg`: после сброса чип слушает одну
линию, и Reset/Read ID нельзя отправлять в целевом QUAD. Для Read ID
`qspi_psram_read_id()` на время транзакции ставит `addr_length = 3`.

## Идентификация чипа

`qspi_psram_read_id()` читает 5 байт (`QSPI_PSRAM_ID_LEN`).

**`qspi_psram_verify_id()`** (`:432`, именованный и generic режимы):
- generic: `Generic PSRAM: skipping KGD check (ID MF=0x.. KGD=0x.. EID=0x..)`;
- KGD ≠ ожидаемого → `KGD 0x.. differs from the expected 0x..: the die may have
  failed the factory test` (**WRN**);
- остальные значимые биты по маске не совпали →
  `chip-variant says <имя> (.. .. ..) but the chip answers .. .. ..; continuing
  with the devicetree choice` (**WRN**);
- ошибка возвращается только при сбое самой транзакции.

**`qspi_psram_auto_detect()`** (`:484`, только при `QSPI_PSRAM_AUTO_USED`):
- перебор `chip_table`, пропуск записей с `mask[0] == 0` (защита от пустых
  слотов — при AUTO их нет, но слот с нулевой маской совпал бы с чем угодно);
- совпадение → `Detected <имя> (ID .. .. .. .. ..)`;
- иначе: все байты `00`/`FF` → `No response from the PSRAM`; KGD `0x55` →
  `PSRAM die failed the factory test`; иначе `Unknown PSRAM ID: …` → `-ENODEV`.
- Порядок таблицы важен: первое совпадение побеждает (именованные записи
  стоят раньше family-строк; общая запись «любой AP» должна была бы стоять
  последней).

## Таблица чипов

```c
struct qspi_psram_chip_params {       /* 44 байта */
	const char *name;
	uint8_t  id[5], mask[5];
	uint32_t size_bits;
	uint8_t  enter_qpi_cmd, exit_qpi_cmd;
	uint8_t  qspi_read_cmd, qspi_write_cmd;   /* 4-4-4 */
	uint8_t  spi_read_cmd,  spi_write_cmd;    /* 1-1-1 */
	uint8_t  reset_en_cmd, reset_cmd, read_id_cmd, kgd_value;
	uint8_t  cmd_length, addr_length;         /* индексы enum биндинга: 1, 3 */
	uint8_t  qspi_rx_dummy, spi_rx_dummy, default_tx_dummy;
	uint16_t ce_max_burst_bytes;              /* -> mem_boundary  */
	uint32_t ce_refresh_us;                   /* -> time_to_break */
};
```

Записи (`:186-243`):

| Индекс / строка | Имя | `id` | Размер | tCEM | Компилируется при |
|---|---|---|---|---|---|
| `[QSPI_PSRAM_VARIANT_ESP64H]` | ESP-PSRAM64H | `0D 5D 40` | 64 Мбит | 8 | `esp64h` или AUTO |
| `[QSPI_PSRAM_VARIANT_IS66WVS4M8BLL]` | IS66WVS4M8BLL | `9D 5D 40` | 32 Мбит | 4 | `is66wvs4m8bll` или AUTO |
| `[QSPI_PSRAM_VARIANT_IS66WVS8M8BLL]` | IS66WVS8M8BLL | `9D 5D 60` | 64 Мбит | 4 | `is66wvs8m8bll` или AUTO |
| family | AP-family 16/32/128 Mbit | `0D 5D` + код 0/1/3 | | 3 | AUTO |
| family | ISSI 8/16/128 Mbit | `9D 5D` + код 0/1/4 | | 1 | AUTO |

Маска везде `FF FF E0 00 00` (MF, KGD, EID[47:45]). Код плотности AP 128 Мбит
не подтверждён на кремнии.

### Условная компиляция

```c
#define QSPI_PSRAM_VARIANT_USED_OR(n, variant) DT_INST_ENUM_HAS_VALUE(n, chip_variant, variant) ||
#define QSPI_PSRAM_VARIANT_USED(variant) (DT_INST_FOREACH_STATUS_OKAY_VARGS(QSPI_PSRAM_VARIANT_USED_OR, variant) 0)
#define QSPI_PSRAM_AUTO_USED  QSPI_PSRAM_VARIANT_USED(auto)
#define QSPI_PSRAM_TABLE_USED (VARIANT_USED(esp64h) || … || QSPI_PSRAM_AUTO_USED)

#define QSPI_PSRAM_CHIP_PARAMS(n)                                              \
	COND_CODE_1(DT_INST_ENUM_HAS_VALUE(n, chip_variant, auto), (NULL),     \
		(COND_CODE_1(DT_INST_NODE_HAS_PROP(n, chip_variant),           \
			     (&chip_table[DT_INST_ENUM_IDX(n, chip_variant)]), \
			     (NULL))))
```

- Записи — designated initializers по индексу enum, **каждая обёрнута в `#if`
  без изменений** (решение автора, 13.08). Цена — нулевые слоты до индекса:
  по ELF 02.09 `IS66WVS4M8BLL` один = 88 байт, AUTO = 396 байт, generic —
  символа нет.
- AUTO перехватывается до `DT_INST_ENUM_IDX`: у `"AUTO"` индекс 3, иначе
  получился бы `&chip_table[3]`.
- `BUILD_ASSERT(ARRAY_SIZE(chip_table) <= QSPI_PSRAM_VARIANT_AUTO)` — только без
  AUTO. Предложенный на ревью 13.08 per-instance assert
  (`DT_INST_ENUM_IDX(n, chip_variant) < ARRAY_SIZE(chip_table)`) не внедрён.
- Смешанная плата (один узел именованный, другой generic) корректна:
  `QSPI_PSRAM_CHIP_PARAMS(n)` вычисляется для каждого инстанса.
- Механика `DT_INST_ENUM_HAS_VALUE`, `str2ident` и `COND_CODE_1` — research §7.

### Как добавить новую деталь

1. Значение в `enum:` у `chip-variant` в биндинге **перед `"AUTO"`**.
2. Значение в `enum qspi_psram_variant` в том же порядке, перед
   `QSPI_PSRAM_VARIANT_AUTO` (от этого зависят индекс и `BUILD_ASSERT`).
3. Запись `[QSPI_PSRAM_VARIANT_X] = { … }` под
   `#if QSPI_PSRAM_VARIANT_USED(x) || QSPI_PSRAM_AUTO_USED`, `.id`/`.mask` по
   разделу Read ID даташита, `size_bits`, `QSPI_PSRAM_STD_QPI_PARAMS` (или поля
   явно, если набор отличается), `ce_refresh_us` из даташита с комментарием об
   источнике и температурном диапазоне.
4. Если деталь закрывает family-строку той же плотности — строку убрать.
5. Проверить по ELF (`nm -S`, gdb) и CI-сборкой.

## MEMC API

`DEVICE_API(memc, …)` (`:926`): `get_size` (из `data->mem_size`, то есть
табличный/определённый/DT размер) и `get_mem_base` (только при
`CONFIG_MSPI_MEMMAP` и включённом отображении, иначе `NULL`). `read`/`write`/
`read_id` не реализованы: MEMC-устройства по определению memory-mapped, а
`read_id` требует корректного захвата шины (сейчас `qspi_psram_acquire()` есть
только под `CONFIG_PM_DEVICE`).

## PM

`qspi_psram_pm_action()` (`:577`, `CONFIG_PM_DEVICE`): при suspend — сброс
кэша и выключение memory-mapped, при resume — повторное включение из
`data->memmap_cfg`.

## Биндинг — ключевые свойства

| Свойство | Тип / ограничение | Смысл |
|---|---|---|
| `size` | int, биты | обязателен при init только в generic |
| `chip-variant` | enum `ESP64H`, `IS66WVS4M8BLL`, `IS66WVS8M8BLL`, `AUTO` | режим |
| `mspi-io-mode` | унаследован, без `const`/`default` | SINGLE или QUAD, остальное отвергается при init; отсутствие = SINGLE |
| `mspi-data-rate` | `const: "MSPI_DATA_RATE_SINGLE"` | |
| `read-command`, `write-command`, `command-length`, `address-length`, `rx-dummy` | унаследованы | только generic |
| `enter-qpi-cmd`, `exit-qpi-cmd`, `reset-en-cmd`, `reset-cmd`, `read-id-cmd` | `default: 0x35/0xF5/0x66/0x99/0x9F` | читаются `DT_INST_PROP` |
| `ce-break-config`, `memmap-config`, `mspi-max-frequency`, `mspi-hardware-ce-num` | унаследованы | см. research §7 |

`required:` в нашем биндинге нет ни у одного свойства. В описании: на STM32
узлу нужен второй compatible `"st,psram-device"` и `size`, при этом
`qspi-psram` ставится **первым**.

## Проектные решения и их обоснование

- **Таблица деталей вообще.** У PSRAM нет SFDP/JESD216: ёмкость, страница,
  tCEM, dummy не узнать во время работы. На плате автора ручное описание в DT
  дало две реальные ошибки: размер занижен вдвое и `st,timing-config = <1>`
  ломал запись.
- **Никакого вендорного кода.** Драйвер общается только с MSPI API; удалены
  `st,timing-config`, `mspi_stm32.h`/`mspi_ambiq.h`, ветки `SOC_FAMILY_*`.
- **Граница ответственности.** Драйвер устройства проверяет то, что умеет
  **чип** (SINGLE/QUAD, SDR); доступность memory-mapped в режиме и
  исполнение CE-break — ответственность контроллера (довод swift-tk и erwango).
- **`size` в битах**, как во всём дереве.
- **Несовпадение ID — предупреждение**, не ошибка: пользователь может намеренно
  ставить другой или «плохой» чип (решение автора 02.09).
- **AUTO: worst-case tCEM** — грейд из ID не читается, нарушение tCEM —
  тихая порча данных; ослабить можно через DT по ячейкам, нулевая ячейка не
  отключает защиту (исправлено по ревью 01.09).
- **Инициализация локальных переменных при объявлении** сохраняется
  (`uint8_t id[QSPI_PSRAM_ID_LEN] = {0}`) — правило автора.

## Известные долги и непроверенное

- Не внедрён per-instance `BUILD_ASSERT` (ревью 13.08, P2).
- Комментарий к `enum qspi_psram_variant` всё ещё описывает `GENERIC` как
  значение для generic-режима, хотя в конфиг он не пишется (P3).
- `mspi-io-mode` без `default` — erwango просит `default: "MSPI_IO_MODE_SINGLE"`.
- На STM32 AUTO не может задать размер окна контроллера (research §4.6).
- На железе **не проверены**: AUTO в SINGLE; переопределение `ce-break-config`
  поверх worst-case в AUTO; family-строки (AP 16/32/128, ISSI 8/16/128);
  именованный `IS66WVS8M8BLL`.
- Открыт архитектурный вопрос ревьюеров: per-device compatible и
  `qspi-psram-generic` — [04-pr-review-status.md](04-pr-review-status.md).
