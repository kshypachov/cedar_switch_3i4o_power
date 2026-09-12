# Исследования и технические находки

Всё, что было выяснено в сессиях по даташитам, по коду Zephyr и
экспериментами на плате. Номера строк в чужих файлах (`mspi_stm32_ospi.c`,
`edtlib.py` и т. п.) — **на дату исследования**, в текущем `main` они могли
сместиться. Номера строк нашего драйвера — на `869e497cf70`.

Содержание:
1. [QPI PSRAM: протокол и параметры из даташитов](#1-qpi-psram-протокол-и-параметры-из-даташитов)
2. [Read ID и рынок QSPI PSRAM](#2-read-id-и-рынок-qspi-psram-исследование-01092026)
3. [tCEM: физика и почему его нельзя подобрать тестом](#3-tcem-физика-и-почему-его-нельзя-подобрать-тестом)
4. [Контроллер STM32 OCTOSPI](#4-контроллер-stm32-octospi-driversmspimspi_stm32_ospic)
5. [Другие MSPI-контроллеры](#5-другие-mspi-контроллеры)
6. [Существующие драйверы PSRAM в дереве](#6-существующие-драйверы-psram-в-дереве)
7. [Devicetree, биндинги, Kconfig: правила и ловушки](#7-devicetree-биндинги-kconfig-правила-и-ловушки)
8. [Процесс Zephyr: CI, compliance, оформление](#8-процесс-zephyr-ci-compliance-оформление)
9. [Побочные находки на плате](#9-побочные-находки-на-плате)
10. [Источники](#10-источники)

---

## 1. QPI PSRAM: протокол и параметры из даташитов

### 1.1 Набор команд (ESP-PSRAM64H и IS66WVS идентичны)

| Команда | Код | Шина | Примечание |
|---|---|---|---|
| Read | `03h` | 1-1-1 | без dummy, до ~33 МГц |
| Fast Read | `0Bh` | 1-1-1 | 8 dummy; до 84 МГц (ESP) / 104 МГц (ISSI) |
| Fast Read Quad | `EBh` | 1-4-4 в SPI, 4-4-4 в QPI | 6 dummy |
| Write | `02h` | 1-1-1 | без dummy |
| Quad Write | `38h` | 1-4-4 в SPI, 4-4-4 в QPI | без dummy |
| Enter Quad Mode | `35h` | только SPI | |
| Exit Quad Mode | `F5h` | в QPI | |
| Reset Enable / Reset | `66h` / `99h` | | |
| Read ID | `9Fh` | SPI; у ISSI и в QPI | 24-битный don't-care адрес, 0 wait |
| Set Burst Length | `C0h` | | переключение wrap 1024/32 |

- DUAL-команд (`3Bh`/`BBh`) и 1-1-4 (`6Bh`/`32h`) нет. Физически 4 линии данных
  → OCTAL невозможен. Октальные PSRAM (APS6408L, IS66WVH, HyperRAM) — другой
  класс (OPI, DDR, DQS), в дереве для них `memc_mspi_aps_z8`.
- Имеющий смысл для этого биндинга набор режимов MSPI: `SINGLE` (1-1-1) и `QUAD`
  (4-4-4) — реализованы; `QUAD_1_4_4` (EBh/38h без входа в QPI) — возможное
  расширение: полная квадовая полоса без QPI, Read ID работает на каждой
  загрузке. ISSI прямо указывает «SPI Protocol: 1-1-1 & 1-4-4».

### 1.2 ESP-PSRAM64H (Espressif, кристалл AP Memory)

- Таблица 10-5: **tCEM max 8 мкс**, **tCPH min 50 нс**, KGD pass `0b0101_1101` = `0x5D`.
- До 133 МГц (64H), **84 МГц при пересечении границы страницы**.
- §5.2: страница 1 КБ (CA[9:0]).
- Таблица 5-4: `EBh` — 6 wait cycles, `38h` — 0; `9Fh` и `35h` — **N/A в QPI**.
- §5.3: «The device powers up in SPI Mode».
- §5.5 Command Termination: *«CE# must be pulled high immediately after all
  read/write operations. Not doing so will block internal refresh operations and
  cause memory failure.»*
- SFDP нет. Read ID даёт только MF ID, KGD и EID.
- На плате: `0D 5D 53 12 58` (AP-семейство, плотность `010` = 64 Мбит).

### 1.3 ISSI IS66/67WVS (1M8, 2M8, 4M8, 8M8F)

- AC Characteristics (§7.6): **tCEM 4 мкс (≤85 °C), 1 мкс (≤105 °C)**;
  tCPH min 1 tCLK; страница 1024 байта; до 104 МГц.
- Чтение и запись **всегда заворачиваются внутри страницы 1 КБ** — линейного
  burst через границу страницы нет. Длина wrap переключается `C0h` (1024/32).
- Регистр ID (таблица 6.2), 64 бита MSB-first: MF [63:56] = `0x9D`;
  KGD [55:48] = `0x55` fail / `0x5D` pass; плотность [47:45]; [44:0] — Reserved.
- Read ID доступен **в любой момент** и в SPI, и в QPI (6 wait); ответ
  повторяется по кругу с бита 7 MF, пока CE# низкий.
- Заказной код `IS66 WVS 1M8 ALL-104 N L I`: в ID попадает только плотность;
  напряжение (ALL 1,8 В / BLL 3,0 В), 66/67 и температурный грейд (I/A1/A2) — нет.
- На плате: IS66WVS4M8BLL → `9D 5D 40 C1 40`.
- 12.08 сверка шла по даташиту 1M8; 01.09 агент подтвердил те же tCEM по
  даташитам 2M8/4M8/8M8F.

### 1.4 Значения в таблице драйвера

| Запись | Размер | tCEM (`ce_refresh_us`) | Источник |
|---|---|---|---|
| `ESP64H` | 64 Мбит | 8 мкс | ESP-PSRAM64H, табл. 10-5 |
| `IS66WVS4M8BLL` | 32 Мбит | 4 мкс | ISSI, §7.6 (≤85 °C) |
| `IS66WVS8M8BLL` | 64 Мбит | 4 мкс | ISSI, §7.6 (≤85 °C) |
| family-строки AUTO | по ID | AP 3 мкс, ISSI 1 мкс | худший грейд семейства |

Общее для всех (`QSPI_PSRAM_STD_QPI_PARAMS`): `35h/F5h/66h/99h/9Fh`, QPI `EBh/38h`
с 6 dummy, SPI `0Bh/02h` с 8 dummy, команда 1 байт, адрес 3 байта, tx dummy 0,
burst 1024, KGD `0x5D`. Детали на ISSI выше 85 °C должны задавать
`ce-break-config` явно.

---

## 2. Read ID и рынок QSPI PSRAM (исследование 01.09.2026)

Полные отчёты агентов — [sessions/ca0c50e7-agent-reports.md](sessions/ca0c50e7-agent-reports.md).

### 2.1 Структура ответа

`9Fh` + 24-битный don't-care адрес, без wait: байт 0 — MF ID, байт 1 — KGD,
дальше EID[47:0]. У AP-семейства Read ID гарантирован только как шаг
power-up-инициализации после global reset, и только в SPI; у ISSI — всегда.
Отсюда порядок в драйвере: force SPI → reset → Read ID.

### 2.2 Вендоры

| Вендор | Детали (quad) | MF ID | Примечание |
|---|---|---|---|
| AP Memory | APS1604M, APS3204L, APS6404L (-SQN/-SQR/-SQH/-3SQR…), APS12804O | `0x0D` | родоначальник класса |
| Espressif | ESP-PSRAM32/64/64H | `0x0D` | ребрендинг кристаллов AP |
| IPUS | IPS1604/3204/6404, IPS1704L | `0x0D` | клон, копирует ID |
| Lyontek | LY68L6400, LY68S6400, LY68S3200 | `0x0D` | AP-совместимый |
| Vilsion | VTI7064LSM/MSM | `0x0D` | клон |
| Ramsun | RS3204/6404 | предп. `0x0D` | ребрендинг, даташита нет |
| CascadeTeq | CSS1604/3204/6404, CSS12804 | предп. `0x0D` | даташита нет |
| ISSI | IS66/67WVS1M8…8M8F, IS66WVS16M8F | `0x9D` | собственная разработка |

Quad PSRAM нет у Winbond (HyperRAM/parallel), XTX, GigaDevice, Puya, Zetta,
AMIC, Fudan и др. Плотности на рынке 8–128 Мбит; **256 Мбит quad не существует**
(только octal).

### 2.3 Что можно и нельзя определить по ID

| Вопрос | Ответ |
|---|---|
| Семейство (AP-линия / ISSI) | да: `0x0D` / `0x9D` |
| Кристалл годен | да: KGD `0x5D` (брак `0x55`) |
| Плотность | да: EID[47:45], **кодировка зависит от семейства** |
| Вендор внутри AP-линии | нет: клоны сознательно копируют весь ID |
| Скоростной/температурный грейд → tCEM | **нет** |
| Напряжение (1,8/3,3 В) | нет (драйверу не нужно) |

- **EID[44:0] у AP-семейства — per-die manufacturing ID** (разный у каждого
  кристалла), у ISSI — Reserved. Поэтому сравнение только по маске.
- Параметры протокола внутри семейства едины (иначе клоны не были бы
  drop-in): команды, dummy на общих частотах, адрес, страница 1 КБ.
  **Электрические лимиты различаются**: максимальная частота 100 (LY68L6400
  по одному источнику, 133 по другому), 104 (IPUS, Vilsion), 133 (AP -3SQR,
  LY68S), 144 МГц (AP -SQH); tCEM 8 мкс у стандартных грейдов, 3 мкс у AP 105 °C
  (до ревизии 10.2021 — 4 мкс).

### 2.4 Кодировка плотности и коллизия

| EID[47:45] | AP-семейство (16 Мбит << n) | ISSI (8 Мбит << n) |
|---|---|---|
| `000` | 16 Мбит | 8 Мбит |
| `001` | 32 Мбит | 16 Мбит |
| `010` | 64 Мбит | **32 Мбит** |
| `011` | 128 Мбит (предположительно) | 64 Мбит |
| `100` | — | 128 Мбит |

Один и тот же код `010` — 64 Мбит у AP и 32 Мбит у ISSI → ключ записи — пара
(MF, код плотности), маска `FF FF E0 00 00`, ~9 записей на весь рынок.

Флаги: на рисунке даташита APS3204L показан `010` — вероятно опечатка
(по кодировке должно быть `001`), не проверено на кремнии. Запись AP 128 Мбит
тоже не подтверждена на кремнии.

### 2.5 Практика esp-idf

`components/esp_psram/esp32/esp_psram_impl_quad.c`: читает 8 байт, валидность —
**только KGD == 0x5D, MF не проверяется**; плотность — EID[47:45]. Два
хардкод-исключения: EID[47:40] == `0x20` (32 Мбит VER0, другой режим клока),
`0x26` (64 Мбит trial, отдающий код 32 Мбит). Новый драйвер
(`esp_quad_psram_defs_ap.h`) определяет `PSRAM_QUAD_MFID_AP 0xD`, но тоже
валидирует по KGD.

### 2.6 Выводы для AUTO

- ID определяет **протокол и размер**; скорость и тайминги — консервативно
  или из devicetree.
- Частота — всегда из DT (`mspi-max-frequency`): чип на 104 МГц молча
  заведётся на 133 и будет сыпать ошибки.
- tCEM — худший грейд семейства (AP 3 мкс, ISSI 1 мкс) с переопределением через
  `ce-break-config`.
- Незнакомый ID — ошибка init с выводом всех байт, без тихого отката на
  generic. Отдельные диагнозы: шина молчит (все `00`/`FF`), брак (KGD `0x55`),
  неизвестный чип.
- Имя в логе — семейство («AP-family 64 Mbit»), а не модель. Исключение —
  совпадение с именованной записью (например, любой AP 64 Мбит логируется как
  ESP-PSRAM64H).
- Wrap Read ID документирован у ISSI и наблюдается у AP → чтение 5 байт
  безопасно, но полагаться на wrap нельзя — маска решает вопрос короткого ID.

---

## 3. tCEM: физика и почему его нельзя подобрать тестом

- PSRAM — DRAM с самообновлением. Пока CE# низкий, **внутренний refresh
  заблокирован**. Превышение tCEM срывает обновление **других строк**, а не
  портит передаваемые данные.
- Отказ **вероятностный**, порог **экспоненциально зависит от температуры**
  (поэтому грейды 8/3 мкс у AP, 4/1 мкс у ISSI) и определяется **худшей
  ячейкой** кристалла. Тест может доказать нарушение, но не безопасность.
  Никто (esp-idf, SDRAM-контроллеры) рефреш-тайминги экспериментально не
  подбирает.
- На столе чип терпел заметно больше спецификации: в неограниченном режиме
  порча начиналась около 2 КБ непрерывной серии на 80 МГц (≈51 мкс), а
  `st,csbound = <10>` (25,6 мкс) проходил тест. Это «работает на столе,
  отказывает в поле».
- Цена консервативного значения (QPI 80 МГц, байт = 2 такта, накладные
  ≈0,2 мкс): при tCEM 8 мкс в окно CE влезает ~300 байт (оверхед ~3%), при 1 мкс
  — ~40 байт (~20%). Это потеря пропускной способности, а не целостности.
- Тестировать при init допустимо то, что ломается **сразу и всегда**:
  размер (wrap-тест по степеням двойки), smoke-тест записи/чтения (неверные
  dummy/команды видны сразу). Нельзя — refresh/retention и частотный запас.
- **Урок про стресс-тест**: фаза `memcpy` из RAM порчу не ловит (чтения из
  RAM дают шине паузы, 17 МБ/с), ловит только плотный цикл 32-битных записей
  (36 МБ/с). Тест только «реалистичным» потоком дал бы ложный PASS.

---

## 4. Контроллер STM32 OCTOSPI (`drivers/mspi/mspi_stm32_ospi.c`)

Всё в этом разделе — **не наш код**. Кандидаты в отдельный PR/issue.

### 4.1 CE-break не доходит до железа

- `mem_boundary` и `time_to_break` из `mspi_dev_config()` только сохраняются
  (`:619-625`, позже `:640-644`) — нигде не программируются. То же в
  `mspi_stm32_xspi.c` и `mspi_stm32_qspi.c`.
- В HAL (`OSPI_InitTypeDef`, `stm32u5xx_hal_ospi.h:82-94`) есть нужные поля:

  | Поле HAL | Регистр | Назначение | Соответствие MSPI |
  |---|---|---|---|
  | `ChipSelectBoundary` | DCR3.CSBOUND | отпускать nCS на границах 2^n **байт** | `mem_boundary` |
  | `Refresh` | DCR4.REFRESH | отпускать nCS каждые Refresh+1 тактов | `time_to_break` |
  | `MaxTran` | DCR3.MAXTRAN | отпускать nCS каждые MaxTran+1 байт при запросе шины вторым OCTOSPI | арбитраж в muxed |

- Сейчас `ChipSelectBoundary` = свойство **контроллера** `st,csbound` (`:1496`),
  `Refresh` и `MaxTran` = 0 (выключены). Всё применяется один раз в
  `HAL_OSPI_Init` (`:1238`) на весь контроллер — для per-device значений нужен
  re-init или прямая запись DCR3/DCR4 при смене устройства.
- Биндинг `st,csbound` описывает значение как «2^(csbound) **bits**» — по RM
  это байты (ошибка документации).
- Драйвер устройства выставить `st,csbound` не может и не должен: DT статичен,
  а правильная граница зависит от **реальной** SCK, которую знает только
  контроллер (предделитель, округление).

### 4.2 Эксперимент с `st,csbound` (11–12.08, ESP-PSRAM64H, 80 МГц)

| `st,csbound` | Граница | Длительность пакета | Результат | К tCEM 8 мкс |
|---|---|---|---|---|
| 0 | нет | ~100 мс сплошь | **FAIL** с +0x800 | в тысячи раз больше |
| 7 | 128 Б | 3,4 мкс | PASS, 224 мс/проход (4 МБ) | запас 2,4× |
| 8 | 256 Б | 6,6 мкс | PASS, 216 мс | запас 1,2× |
| 10 | 1024 Б | 25,6 мкс | PASS, 211 мс | **превышение 3,2×** |

Потеря пропускной способности: 2,5% при 256 Б, 6% при 128 Б.
12.08 при `<0>` плотный цикл дал ~2 092 000 битых слов из 2 097 152 начиная
с +0x800, при `<7>` — 100/100 чисто и во внутренней флеш, и в XIP.
17.08 в восстановленной копии платы не было `st,csbound` — массовые ошибки,
после возврата `<7>` — PASS.

**Вывод**: наивное отображение `mem_boundary` (1024) → CSBOUND неверно.
Правильно — `min(mem_boundary, граница по временному бюджету из
time_to_break и фактической SCK)`, опционально плюс REFRESH.

### 4.3 Расчёт границы

```
B_max = (f_sck × t_бюджет − C_накладные) × байт_за_такт
```

Quad SDR — 0,5 байта за такт; накладные на пакет ≈14 тактов (команда 2,
адрес 6, dummy 6); бюджет ≈75% tCEM.

| SCK | Бюджет 6 мкс | B_max | CSBOUND | Пакет | Накладные |
|---|---|---|---|---|---|
| 80 МГц | 480 тактов | 233 Б | 7 (128 Б) | 3,4 мкс | +5,2% |
| 80 МГц | | | 8 (256 Б) | 6,6 мкс | +2,7% |
| 40 МГц | 240 тактов | 113 Б | 6 (64 Б) | 3,6 мкс | +11% |
| 20 МГц | 120 тактов | 53 Б | 5 (32 Б) | 3,9 мкс | +18% |

**Чем ниже частота, тем меньше должна быть граница.** Для IS66WVS4M8BLL на
50 МГц QPI: `<7>` = 128 Б ≈ 5,1 мкс — **больше 4 мкс**; `<6>` ≈ 2,6 мкс
(подходит для ≤85 °C); `<4>` ≈ 0,6 мкс (подходит и для 1 мкс). Прогоны на ISSI
02.09 шли с `<7>` и прошли — при комнатной температуре; формально корректнее `<6>`.

### 4.4 Прочие дефекты

- **`:1338` — переполнение `tx_dummy`**: `tx_dummy - turnaround_cycles` без
  клампа, поле `uint16_t`; у QPI PSRAM `tx_dummy = 0`, `st,timing-config = <1>` →
  65535 dummy-циклов, полностью битая запись в memory-mapped. То же
  `mspi_stm32_xspi.c:1348`. По смыслу turnaround относится к чтению.
- **`:258-265` — таймаут memory-mapped**: период захардкожен `0x34`, включается
  только при обнаружении MUXEN, считает такты **простоя** — непрерывный поток не
  разрывает.
- **`:1495` — `ChipSelectHighTime = 1`**: на 80 МГц такт 12,5 нс, а
  ESP-PSRAM64H требует tCPH ≥ 50 нс → нужно ≥4 такта. У ISSI tCPH = 1 tCLK —
  достаточно.
- Семантика блокировки контроллера изменилась между 4.4 и `main`
  (`MSPI_DEVICE_CONFIG_NONE` — ранний `return 0`, лок до
  `mspi_get_channel_status`); при двух устройствах на контроллере стоит
  перепроверить.
- Предлагаемый порядок отдельного PR: кламп `tx_dummy` → `Refresh` из
  `time_to_break` и `ChipSelectBoundary` из `mem_boundary` с
  перепрограммированием в `dev_config` → `MaxTran` для muxed.

### 4.5 Memory-mapped в `MSPI_IO_MODE_SINGLE`

```c
/* mspi_stm32_ospi.c:208 (копия в mspi_stm32_xspi.c:159) */
if ((dev_data->dev_cfg.io_mode == MSPI_IO_MODE_SINGLE) &&
    (mspi_stm32_ospi_hal_address_size(dev_data->dev_cfg.addr_length) ==
     HAL_OSPI_ADDRESS_24_BITS)) {
	LOG_ERR("MSPI_IO_MODE_SINGLE in 3Bytes addressing is not supported");
	return -EIO;
}
```

- Происхождение: `drivers/flash/flash_stm32_ospi.c:1011` с комментарием
  `/* OPI mode and 3-bytes address size not supported by memory */` —
  ограничение **памяти** разработчика (MX25LM51245G на платах ST в OPI требует
  4-байтный адрес), скопированное в MSPI-драйверы.
- В RM0456 (OCTOSPI, memory-mapped) такого ограничения нет: `IMODE/ADMODE/DMODE`
  и `ADSIZE` независимы. STM32 QUADSPI (`mspi_stm32_qspi.c`) такой проверки не имеет.
- **Эксперимент 17.08**: проверка локально закомментирована → SINGLE named и
  manual (`0Bh`/8 dummy, `02h`) — PASS 100/100, 1747 мс/проход (×3,9 медленнее
  QPI). С проверкой init честно завершается `Failed to enable memory mapping`.
  erwango согласился, что проверку надо переработать.

### 4.6 Размер окна и требования к дочернему узлу

- `.DeviceSize = MSPI_STM32_INST_MEM_ADDR_BITS(index, 26)` (`:1524` на 01.09) —
  DEVSIZE зашивается **из DT-свойства `size` дочернего узла при init
  контроллера**, до запуска драйвера PSRAM. `memmap_on()` его не
  перепрограммирует, `memmap_cfg.size` игнорируется.
  - AUTO не может расширить окно: определённый по ID размер идёт в
    `memc_get_size()`, а окно остаётся по DT (расхождение → предупреждение).
  - Неверный `chip-variant` с бо́льшим размером → приложение выходит за окно →
    **BUS FAULT** (наблюдалось на `0x90400000` при окне 4 МБ).
  - Полноценное решение — контроллер берёт размер из `memmap_cfg.size` в
    `mspi_memmap_config()` (отдельный PR, согласование семантики MSPI API).
- До изменений DCR1 был `0x071f0000` (MTYP = 7 — зарезервировано, DEVSIZE = 31).
  PR #114756 (ExaltZephyr) добавил программирование `DeviceSize`/`MemoryType` и
  `BUILD_ASSERT(MSPI_STM32_HAS_SUPPORTED_CHILD(index), "MSPI controller must have
  a child with compatible st,nor/st,psram-device")`. В `main` к 17.08 это уже
  было: узел PSRAM на STM32 должен иметь `compatible = "qspi-psram",
  "st,psram-device"` и `size`. После rebase в ELF: `DeviceSize = 23`,
  `MemoryType = 0`. (На ревью #114756 12.08 замечено: макрос разворачивает
  детей без разделителя — сломается при двух и более дочерних узлах.)
- Порядок compatible важен: биндинг выбирается по **первому** совпавшему.
  11.08 в DTS платы стояло `"mspi-qspi-psram", "st,psram-device"`, а у
  `mspi-qspi-psram` после переименования биндинга не было → узел описывался
  `st,psram-device-mspi.yaml`, который требует `read-command`/`write-command`.
  `qspi-psram` нужно ставить первым.

### 4.7 Errata ES0499 (STM32U575/U585), связанные с CSBOUND

- Порча данных при 8/16-битном чтении последнего слова перед границей CSBOUND,
  если следующее чтение адресует первое слово следующей страницы, пока первое
  не завершилось. Обход ST — только 32-битные обращения (для PSRAM как системной
  памяти невыполнимо; стресс-тесты шли словами и не могли это поймать).
- Потеря последнего байта перед разрывом при SDR-чтении в **clock mode 3** —
  сдвиг всех последующих данных. Драйвер и DT используют mode 0 — не касается,
  но `mspi-cpp-mode` в DT платы не должен переопределяться.

---

## 5. Другие MSPI-контроллеры

| Контроллер | Драйвер | Memory-mapped в SINGLE | `ce-break-config` |
|---|---|---|---|
| Ambiq Apollo3 | `mspi_ambiq_ap3.c` | да (XIP без проверки io_mode) | **исполняет**: `mem_boundary` → `eDMABoundary`, `time_to_break` → `ui16DMATimeLimit` (×10, в 0,1 мкс), `:841-849`; на старом SoC при ненулевых значениях ошибка |
| Ambiq Apollo4/5 | `mspi_ambiq_ap5.c` | да | |
| STM32 QUADSPI (F4/F7/L4) | `mspi_stm32_qspi.c` | да | не программирует |
| STM32 OCTOSPI (U5, H7, L5) | `mspi_stm32_ospi.c` | нет (артефакт `:208`) | только сохраняет |
| STM32 XSPI (H5, H7RS, N6) | `mspi_stm32_xspi.c` | нет (копия `:159`) | только сохраняет |
| Synopsys DW SSI (nRF54H20/nRF9280) | `mspi_dw.c` | нет — `:723` «XIP not available in single line mode», реальное ограничение IP | хранит |
| эмулятор | `mspi_emul.c` | — | `zephyr,emul-device-mspi.yaml`: default `[0, 0]` |

Отсюда довод swift-tk: ограничения живут в контроллерах и разные у разных
контроллеров — драйвер устройства проверяет только то, что умеет **чип**
(SINGLE/QUAD, SDR), а отказ memory-mapped возвращает контроллер.

---

## 6. Существующие драйверы PSRAM в дереве

| Драйвер | Покрывает | Проверка ID | Параметры | Режимы |
|---|---|---|---|---|
| `memc_mspi_aps6404l.c` | AP Memory APS6404L | `vendor_id != 0x0D` → **предупреждение** (`:331`) | всё из DT | SINGLE, QUAD (`:289-296`) |
| `memc_mspi_is66wv.c` (Nordic, 11.06.2026) | ISSI IS66/67WV (WVS и WVR) | `vendor_id != 0x9D` → **отказ** (`:419`) | всё из DT; полный MEMC API | SINGLE, QUAD |
| `memc_mspi_aps_z8.c` | APS6408L/APS256 | — | октальный DDR (`:492`, `:500`) | только OCTAL |
| **наш** `memc_mspi_qspi_psram.c` | ESP-PSRAM64H, IS66WVS4M8/8M8, AUTO, generic | мягкая сверка по маске | таблица / AUTO / DT | SINGLE, QUAD |

- Командный набор у `aps6404l`, `is66wv` и нашего драйвера совпадает до байта.
- **Эксперимент 11.08**: `aps6404l` на ESP-PSRAM64H в muxed на STM32 — PASS,
  когда в DT заданы `read-command`, `write-command`, `command-length`,
  `address-length`, `rx-dummy`, `ce-break-config`. После тёплого сброса —
  `Vendor ID does not match expected value of 0xd`, продолжает работу (чип
  остался в QPI, SPI-фаза init идёт вхолостую, совпадение режимов случайно).
  Наш драйвер на тёплом старте читает ID верно благодаря `force_spi_mode`.
- Уникальное у нашего драйвера: параметры по имени микросхемы, generic-режим
  без привязки к вендору, восстановление из QPI, AUTO по ID.
- Оба соседа наследуют `size` из `jedec,jesd216.yaml` — в битах.

---

## 7. Devicetree, биндинги, Kconfig: правила и ловушки

**Kconfig**
- `scripts/dts/gen_driver_kconfig_dts.py:26-34` генерирует
  `DT_HAS_<COMPAT>_ENABLED` (верхний регистр, `-,.@/+` → `_`). Ссылка в
  `depends on` на несуществующий символ тихо равна `n` — драйвер пропадает из
  сборки без ошибок (ругань только при присваивании в `.conf`, `kconfig.py:54`).
- `depends on $(dt_compat_on_bus,$(DT_COMPAT_QSPI_PSRAM),mspi)` — как у соседей.

**Именование (#108082)**: compatible без вендорного префикса (`qspi-psram`),
файл `<device>-mspi.yaml` в `dts/bindings/mtd/`; Kconfig и имя файла драйвера
сохраняют `MSPI` (`CONFIG_MEMC_MSPI_IS66WV`, `memc_mspi_is66wv.c`).

**Выбор биндинга и инстансы**
- Биндинг узла выбирается по **первому** compatible, у которого есть YAML;
  `default:` берутся только из него.
- `gen_defines.py:272` нумерует инстансы по **каждому** compatible узла →
  `DT_DRV_COMPAT qspi_psram` видит узел, даже если `qspi-psram` стоит вторым.
  Но тогда свойства со значением по умолчанию из нашего биндинга не
  сгенерируются, и `DT_INST_PROP(n, enter_qpi_cmd)` не соберётся (так было с
  `"ambiq,mspi-device", "qspi-psram"`). Сейчас используется `DT_INST_PROP`
  (etienne-lms, пункт f), а биндинг требует ставить `qspi-psram` первым.
- `DT_INST_PROP` на необязательном свойстве без `default` → `undeclared
  identifier` (было с `mspi-hardware-ce-num`) → `DT_INST_PROP_OR`.
- Свойство в узле, не объявленное в биндинге → **ошибка** devicetree (после
  удаления `st,timing-config` строка в DTS платы ломает сборку).

**Правила наследования свойств (edtlib)**
- Дочерний биндинг может **добавить** к унаследованному свойству
  `description`, `required`, `const`, `default` и т. п. Переопределить уже
  заданный ключ (например `enum`) — ошибка «from included file overwritten»
  (`_merge_props`/`_bad_overwrite`, `edtlib.py:3119-3136`). Обход —
  `property-blocklist` при `include` и полное переопределение свойства.
- **Сужать enum у `mspi-io-mode` нельзя даже обходом**: `MSPI_DEVICE_CONFIG_DT()`
  превращает строку в C-значение через `DT_ENUM_IDX_OR` (`mspi/devicetree.h:42`)
  — порядковый номер в списке биндинга. `enum: [SINGLE, QUAD]` даст QUAD
  индекс 1 = `MSPI_IO_MODE_DUAL`. Безопасен только префикс исходного списка
  (сохраняет индексы, но пропускает DUAL). Решается только переходом общего
  макроса на `DT_STRING_TOKEN` — изменение MSPI-подсистемы. Ни один MSPI-биндинг
  устройств enum не сужает.
- `const` не делает свойство обязательным: без свойства
  `MSPI_DEVICE_CONFIG_DT` подставит `SINGLE` через `DT_ENUM_IDX_OR` → рантайм-проверка
  всё равно нужна. С `const` неверное значение ловится при сборке:
  `value of property 'mspi-io-mode' … is different from the 'const' value`.
- `title:` — легальный ключ верхнего уровня (`edtlib.py:458,474`), ~350
  биндингов его используют.
- `default:` в upstream-биндингах допустим для фиксированных свойств железа с
  объяснением в `description` (`doc/build/dts/bindings-upstream.rst`,
  «Rules for default values»).
- `size` для памяти в Zephyr — **в битах** (`jedec,jesd216.yaml:25-27` и ≥8
  биндингов). Читаемость — `<DT_SIZE_M(64)>`.

**Макросы, используемые драйвером**
- `DT_INST_ENUM_HAS_VALUE(n, chip_variant, auto)` → склейка
  `DT_N_S_…_P_chip_variant_ENUM_VAL_auto_EXISTS`; этот `#define` существует,
  только если у узла стоит `"AUTO"`; `IS_ENABLED` превращает
  «определён/нет» в 1/0. Значения enum проходят через `str2ident` — **нижний
  регистр**. `auto` (ключевое слово C) безопасен: склеивается препроцессором
  до разбора ключевых слов.
- Рядом генерируется `..._ENUM_IDX 3` для `"AUTO"` — поэтому
  `QSPI_PSRAM_CHIP_PARAMS(n)` перехватывает AUTO до `DT_INST_ENUM_IDX`, иначе
  получился бы `&chip_table[3]`.
- `COND_CODE_1(DT_INST_NODE_HAS_PROP(n, chip_variant), (&chip_table[…]), (NULL))`
  раскрывает только выбранную ветку — в generic-сборке идентификатор
  `chip_table` не появляется, хотя таблица вырезана.

**MSPI DT-свойства**
- `memmap-config = <enable address_offset size permission>`
  (`mspi/devicetree.h:85-91`, `struct mspi_memmap_cfg` `mspi.h:321`). Без него
  драйвер инициализирует чип, но окно памяти мёртвое, `memc_get_mem_base()` →
  `NULL`. На STM32 `size` игнорируется (окно на всю область контроллера);
  драйвер проверяет только `size ≤ ёмкость`.
- `ce-break-config = <mem_boundary time_to_break>` (байты, мкс;
  `mspi/devicetree.h:58-62`, определено в `mspi-device.yaml:196-212`) — ноль =
  без ограничения; исполняет его контроллер (§5).
- Прецедент трёх источников параметров — `drivers/flash/Kconfig.nor:19`,
  `choice SPI_NOR_SFDP` (MINIMAL / DEVICETREE / RUNTIME).

**Per-chip compatible (предложение erwango/swift-tk), проверено на
реализуемость 13.08 и 01.09**
- `issi,is66wvs4m8bll.yaml` / `esp-psram64h-mspi.yaml` с
  `include: qspi-psram-mspi.yaml` и `default:` для `size`, `read-command`,
  `write-command`, `rx-dummy`, `command-length`, `address-length`,
  `ce-break-config`; узел `compatible = "<chip>", "qspi-psram"`.
- Уходят из C: enum вариантов, `chip_table[]` с `#if`, `BUILD_ASSERT`,
  ветвления named/generic.
- Теряется: выбор набора команд по `mspi-io-mode` (дефолт — одно значение →
  QUAD-значения по умолчанию, SINGLE переопределяется вручную или фолбэк в
  драйвере); нужно своё свойство для KGD. AUTO-таблица при этом остаётся
  оправданной — данные неизвестного чипа в DT не выразить.

---

## 8. Процесс Zephyr: CI, compliance, оформление

- **CI ребейзит каждый коммит PR на актуальный `main`** (`Rebasing (1/5)…`).
  Merge-коммит конфликт не лечит — он живёт внутри коммита; Zephyr требует
  линейную историю.
- Compliance (gitlint): **UC3** — заголовок `subsystem: subject`; **UC4** —
  строки тела ≤ 75; **UC6** — тело не пустое. Checkpatch
  `COMMIT_LOG_LONG_LINE` — предупреждение, но compliance красный. Identity:
  автор == `Signed-off-by` (исправлено на `kshypachov@outlook.com`). Коммиты,
  созданные кнопкой «Commit suggestion», не имеют `Signed-off-by` и префикса.
- Локальный запуск:
  `scripts/ci/check_compliance.py -c <base>..HEAD -m Gitlint -m Identity -m Checkpatch
  -m DevicetreeBindings -m YAMLLint -m Nits -m BinaryFiles` (`KconfigBasic`
  локально падает с `WestNotFound`, если дерево не west-workspace).
- Twister: `build_only: true` → статус `NOT RUN (build)` / «built (not run)» —
  это норма. `platform:<board>:DTC_OVERLAY_FILE=boards/…` в `extra_args` —
  стандартный приём (~120 мест в дереве).
- Build-only на плате с другим чипом допустим (прецеденты
  `sample.drivers.memc.nrf_mspi`, `tests/drivers/memc/ram_api`), но стоит
  оговорить в overlay: на B-U585I-IOT02A физически октальный APS6408L; STM32-плат
  с QPI PSRAM в дереве нет.
- SonarQube Cognitive Complexity: +1 за ветвление/цикл/тернарник/цепочку `&&`/`||`,
  +N за вложенность; порог 25. На мерж не влияет.
- Копирайт: `doc/contribute/guidelines.rst:105-130` рекомендует
  `SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors`, личная
  строка — дополнительно; `SPDX-License-Identifier: Apache-2.0`.
- Переименование PR: веб (Edit у заголовка), `gh pr edit 104100 --repo
  zephyrproject-rtos/zephyr --title …`, либо REST `PATCH
  /repos/zephyrproject-rtos/zephyr/pulls/104100`. `gh` в системе не
  установлен — комментарии PR читались через REST API.
- После force-push инлайн-комментарии, привязанные к старым строкам,
  становятся outdated, но не resolved — их нужно закрывать ответами.

---

## 9. Побочные находки на плате

### 9.1 Одиночные битовые ошибки в UART-логе

- Симптом: в hex-дампе сэмпла `memc` один сбойный символ на ~2 КБ текста
  (`'7'→'6'`, `'2'→'3'`, `'d'→'f'`), позиция меняется между прогонами, только в
  `CONFIG_LOG_MODE_IMMEDIATE`. memcmp всех 8 МБ на устройстве при этом проходил.
- Причина: для сборки под новый Zephyr из DTS платы убран `msi-pll-mode`
  (аппаратная калибровка MSI по LSE требует LSE, а он выключен) → USART1
  тактируется от некалиброванного RC MSI → ошибка частоты на границе допуска
  UART. Immediate-режим шлёт сплошной поток без пауз.
- Лечение (вне драйвера): USART1 от HSI16 или включить LSE и вернуть
  `msi-pll-mode`. Для тестов — не печатать дампы, итог считать на устройстве.

### 9.2 Сброс во время XIP ломает следующую загрузку

Сброс MCU посреди memory-mapped обращения к NOR оставляет флеш в незавершённой
транзакции; следующая загрузка падает на `SFDP magic 50474673 invalid`,
воспроизводится 100%. Один чистый старт восстанавливает. `flash_stm32_ospi` не
сбрасывает микросхему перед чтением SFDP. Для продукта: watchdog/кнопка во
время XIP могут сделать плату незагружаемой до снятия питания. Лечение —
`66h/99h` (или `F0h`) до первого чтения SFDP, как `force_spi_mode` у PSRAM.

### 9.3 Прочее

- Muxed: PSRAM на OCTOSPI2 использует CLK и IO[3:0] OCTOSPI1 через OCTOSPIM;
  `pinctrl-0` у octospi2 содержит только nCS. Пины мультиплексирует драйвер
  NOR на octospi1 (приоритеты init: флеш 50, memc 80). В минимальном тесте без
  драйвера флеш Read ID даёт нули.
- NRST от ST-LINK не сбрасывает PSRAM: питание сохраняется, чип остаётся в
  QPI. Для чистого эксперимента — обесточивание платы.
- ST-LINK может зависнуть на уровне USB (`LIBUSB_ERROR_TIMEOUT`, `pipe is
  stalled`, chipid `0x000`) — VCP при этом работает. Лечится переподключением
  USB-кабеля программатора, не питанием платы.
- `sys_reboot(SYS_REBOOT_COLD)` в stage1 под отладчиком не перезапустил MCU
  (~4 мин тишины) — после `st-flash reset` handover прошёл.
- Shell log backend роняет ранние сообщения init — для логов драйвера
  `CONFIG_SHELL=n`. Полный дамп сэмпла `memc` на 115200 идёт ~30 мин; сэмпл
  плюс MSPI-драйвер печатают по 8192 строки → «messages dropped».

---

## 10. Источники

**Даташиты**
- ESP-PSRAM64/64H: <https://www.espressif.com/sites/default/files/documentation/esp-psram64_esp-psram64h_datasheet_en.pdf>
  (зеркало <https://cdn-shop.adafruit.com/product-files/4677/4677_esp-psram64_esp-psram64h_datasheet_en.pdf>)
- ISSI IS66/67WVS: 1M8 <https://www.issi.com/WW/pdf/66-67WVS1M8ALL-BLL.pdf>,
  2M8 <https://www.issi.com/WW/pdf/66-67WVS2M8ALL-BLL.pdf>,
  4M8 <https://www.issi.com/WW/pdf/66-67WVS4M8ALL-BLL.pdf>,
  8M8F <https://www.issi.com/WW/pdf/66-67WVS8M8FALL-BLL.pdf>
  (issi.com блокирует ботов; архив:
  <http://web.archive.org/web/20250621201126/https://www.issi.com/WW/pdf/66-67WVS8M8FALL-BLL.pdf>)
- AP Memory: список <https://www.apmemory.com/en/product/iotram/SPIQSPI>,
  APS6404L-SQH <https://www.apmemory.com/en/downloadFiles/0324112120r9601773>,
  APS1604M-SQR <https://www.apmemory.com/tw/downloadFiles/0324112120r6586620>,
  APS6404L-SQN <https://resources.ampheo.com/static/datasheets/ap-memory/aps6404l-sqn-sn.pdf>
- IPUS IPS1704L: <https://dl.sipeed.com/TANG/Nano/Spec/IPUS_64Mbit_SQPI_Datasheet%C2%A0(1704).pdf>,
  IPS6404: <https://github.com/raphaelbs/esp32-cam-ai-thinker/blob/master/assets/IPUS_IPS6404_Datasheet.pdf>
- Lyontek LY68L6400: <https://www.lyontek.com.tw/pdf/ddr/LY68L6400-1.2.pdf>, семейство <https://www.lyontek.com.tw/en/serialsram.html>
- Vilsion VTI7064: <https://w.electrodragon.com/w/images/8/81/VTI7064LSMxx.pdf>
- Ramsun: <https://www.sramsun.com/list-354-1.html>; CascadeTeq:
  <https://en.manduic.com/cpzx/3740/CSS6404SS-L>, <https://en.manduic.com/cpzx/3747/CSS12804SS-O>

**ST**
- Errata ES0499 (STM32U575/U585): <https://www.st.com/resource/en/errata_sheet/es0499-stm32u575xx-and-stm32u585xx-device-errata-stmicroelectronics.pdf>
- FAQ QUADSPI/OCTOSPI/HSPI/XSPI: <https://community.st.com/t5/stm32-mcus/overall-faqs-for-quadspi-octospi-hspi-xspi/ta-p/670534>
- RM0456 (STM32U5 reference manual), раздел OCTOSPI

**Код**
- esp-idf: <https://github.com/espressif/esp-idf/blob/master/components/esp_psram/esp32/esp_psram_impl_quad.c>,
  `components/esp_psram/device/esp_quad_psram_defs_ap.h`, `esp_psram_impl_ap_quad.c`
- ESP-IDF external RAM: <https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/external-ram.html>

**Zephyr PR и issue**
- #104100 (этот PR), #108082 (конвенция имён MSPI-биндингов), #105219
  (APS256/APS6408L на MSPI), #109773 (stm32_ospi fixes), #103910 (закрыт),
  #114756 (STM32 MSPI DeviceSize/MemoryType), issue #103721
