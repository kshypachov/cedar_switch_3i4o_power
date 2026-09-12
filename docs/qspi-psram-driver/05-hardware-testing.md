# Тестирование на железе

Плата, окружение, команды, тестовые программы и результаты всех прогонов
из двух сессий. Исходники тестов — [test-infra/](test-infra/README.md).

## Плата

| | |
|---|---|
| Плата | `cedar_switch_3in4out_power_rev3`, STM32U585 (`…/stm32u585xx`) |
| Определение платы | в west-workspace продукта — неотслеживаемый каталог `/Volumes/Programming/Zephyr/zephyr_latest/zephyr/boards/arm/cedar_switch_3in4out_power_rev3/`. В тестах 01–02.09 использовалась отдельная копия `/Users/kiro/Documents/Zephyr/cedar_switch_3in4out_power_rev3` (для `BOARD_ROOT` нужна обёртка `boardroot/boards/st/cedar_switch_3in4out_power_rev3/`), в августе — копия из дерева 4.4 |
| Программатор | ST-LINK, серийник **`002F002B3233510739363634`**, `st-info --probe` → dev-type `STM32U575_U585`, chipid `0x482`. На станции ещё два ST-LINK (`unknown`, `STM32H74x_H75x`) |
| Консоль | `usart1` (PA9/PA10), 115200, VCP этого ST-LINK. Имя порта менялось: `/dev/cu.usbmodem21203`, `21403`, `21303` — **всегда определять по серийнику** (`test-infra/vcp.py`) |
| NOR | W25Q128 (16 MiB) на OCTOSPI1, окно `0x90000000`; `slot0`/`image-0` — offset 0, 4 МБ; остальные 12 МБ без разметки |
| PSRAM | на OCTOSPI2, окно `0x70000000`; общая шина через OCTOSPIM: CLK и IO[3:0] порта 1 (`IOPORT_1_LOW`), nCS PA0 через порт 2 (`st,clk-port = <1>`, `st,ncs-port = <2>`) |
| Прочее | отдельная SPI-флеш на `spi1` (PB3/4/5) с littlefs/nvs — к QSPI не относится |

**Чип PSRAM перепаивается** — перед выводами сверять ID:

| Период | Чип | Read ID |
|---|---|---|
| до 01.09.2026 | ESP-PSRAM64H, 64 Мбит (8 МБ) | `0D 5D 53 12 58` |
| с 01.09.2026 | IS66WVS4M8BLL, 32 Мбит (4 МБ) | `9D 5D 40 C1 40` |

Другой ID после перепрошивки — скорее всего другой чип, а не ошибка чтения:
сначала спросить.

### Что нужно поменять в DTS платы под текущий `main`

- `compatible = "mspi-qspi-psram", "st,psram-device"` → `"qspi-psram", "st,psram-device"`
  (`qspi-psram` первым).
- `xip-config` → `memmap-config`.
- Удалить `st,timing-config` — свойство исключено из биндинга (иначе ошибка
  сборки), а `<1>` ломало запись.
- `size`: для ESP-PSRAM64H в DTS было занижено (32 Мбит при реальных 64). При
  `chip-variant` драйверу не нужен, но STM32-контроллер берёт из него размер
  окна.
- Добавить `st,csbound` на контроллер PSRAM: `<7>` для ESP-PSRAM64H на 80 МГц;
  для ISSI tCEM 4 мкс формально `<6>` или меньше (research §4.3).
- `msi-pll-mode` без включённого LSE новый clock-драйвер отвергает → в тестах
  удалялся overlay-ем (побочный эффект — UART-глитчи, research §9.1). Правильно
  включить LSE или тактировать USART1 от HSI16.
- В минимальных тестах без USB — `uart-bridge0 { status = "disabled"; }`.
- Приложение платы кладёт буферы в PSRAM через
  `zephyr_code_relocate(... LOCATION PSRAM_NOINIT)` — при недооценённом `size`
  половина памяти не используется.

## Окружение сборки

```sh
export ZEPHYR_BASE=/Users/kiro/CLionProjects/zephyr          # ветка PR
export ZEPHYR_SDK_INSTALL_DIR=$HOME/zephyr-sdk-1.0.1
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
M=/Volumes/Programming/Zephyr/zephyr_latest/modules          # модули west-workspace
V=/Volumes/Programming/Zephyr/zephyr_latest/.venv            # python venv
```

В августе workspace лежал в `/Volumes/LocalData/Programming/Zephyr/latest/`
и в начале сентября переехал — пути `/Volumes/LocalData/...` в транскриптах
и сохранённых командах устарели.

`west` упирается в конфликт workspace, поэтому сборка напрямую через CMake:

```sh
cmake -GNinja -S <app> -B <build> \
  -DBOARD=cedar_switch_3in4out_power_rev3 \
  -DBOARD_ROOT=<boardroot> \
  -DZEPHYR_MODULES="$M/hal/stm32;$M/hal/cmsis;$M/hal/cmsis_6" \
  -DDTC_OVERLAY_FILE=<overlay>          # или -DEXTRA_DTC_OVERLAY_FILE=... поверх DTS платы
cmake --build <build>                   # или ninja -C <build>
```

Сэмпл `memc` для платы автора:
`-S $ZEPHYR_BASE/samples/drivers/memc … -DEXTRA_CONF_FILE="boards/b_u585i_iot02a_mspi_qspi_psram.conf;<свой.conf>"`.

### Прошивка

```sh
SER=002F002B3233510739363634
# st-flash (август)
st-flash --serial $SER --connect-under-reset --reset write build/zephyr/zephyr.bin 0x08000000
st-flash --serial $SER --connect-under-reset reset
# STM32CubeProgrammer (сентябрь)
CLI="/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/Resources/bin/STM32_Programmer_CLI"
"$CLI" -c port=swd sn=$SER mode=UR -w build/zephyr/zephyr.hex -v -rst
```

Без `--connect-under-reset` `st-flash` иногда отвечает «Can not connect to target».

### Захват консоли

```sh
PORT=$(python3 test-infra/vcp.py 002F002B3233510739363634) || exit 1
python3 test-infra/capture.py $PORT run.log 200 &    # переживает переподключения VCP при сбросе
# прошивка / сброс, затем wait
```

Порядок, давший полный лог: сначала прошивка и её завершение, потом захват со
сбросом; иначе VCP может переподключиться во время прошивки и потерять начало.

### Проверки по ELF без прошивки

```sh
G=$HOME/zephyr-sdk-1.0.1/arm-zephyr-eabi/bin/arm-zephyr-eabi-gdb
$G -batch -ex "p memc_mspi_qspi_psram_config_0.chip" \
          -ex "p/x memc_mspi_qspi_psram_config_0.tar_dev_cfg.read_cmd" \
          -ex "p memc_mspi_qspi_psram_config_0.tar_dev_cfg.rx_dummy" build/zephyr/zephyr.elf
$G -batch -ex "p mspi_stm32_dev_data_0.hmspi.ospi.Init.DeviceSize" \
          -ex "p mspi_stm32_dev_data_0.hmspi.ospi.Init.MemoryType" build/zephyr/zephyr.elf
$HOME/zephyr-sdk-1.0.1/arm-zephyr-eabi/bin/arm-zephyr-eabi-nm -S build/zephyr/zephyr.elf | grep chip_table
```

### CI локально

```sh
cd $ZEPHYR_BASE
python3 scripts/twister -T samples/drivers/memc --integration -O <out> -j 8 \
  -x ZEPHYR_MODULES="$M/hal/stm32;$M/hal/cmsis;$M/hal/cmsis_6;$M/hal/nxp;$M/hal/nordic;$M/hal/ambiq"
python3 scripts/twister -p b_u585i_iot02a/stm32u585xx -T samples/drivers/memc --build-only -O <out>
python3 scripts/ci/check_compliance.py -c <base>..HEAD \
  -m Gitlint -m Identity -m Checkpatch -m DevicetreeBindings -m BinaryFiles -m YAMLLint -m Nits
git format-patch -1 <sha> --stdout | ./scripts/checkpatch.pl --no-tree -
```

## Топологии тестов

**A — август (плата как в продукте).** NOR на OCTOSPI1 с драйвером
`flash_stm32_ospi`, PSRAM `psram0` на `&octospi2` (узел заменяется overlay
`test-infra/overlays/aug_run_{named,manual}.overlay`), 80 МГц, окно PSRAM
`0x70000000`, `st,csbound = <7>` на `&octospi2` в копии платы. После rebase на
`main` узлу нужен второй compatible `st,psram-device` (в сохранённых overlay
его нет).

**B — сентябрь (только PSRAM).** `test-infra/overlays/cedar_psram_auto_octospi1.overlay`:
`/delete-node/ &octospi1`, узел `spi@420d1400` пересоздаётся как
`st,stm32-ospi-controller` с одной PSRAM (nCS порт 2, IO порт 1), окно
`0x90000000`, `clock-frequency` 80 МГц, `mspi-max-frequency` 50 МГц,
`st,csbound = <7>`, `compatible = "qspi-psram", "st,psram-device"`, удалён
`msi-pll-mode`. Для named/generic копировался с заменой `chip-variant`/свойств,
`size` — под реальный чип.

## XIP: исполнение из внешней NOR с PSRAM на той же шине

Итоговая схема (модель 2):

1. **stage1** (внутренняя флеш; драйвер флеш + `CONFIG_FLASH_STM32_NOR_MEMMAP` +
   тестируемый драйвер PSRAM): если образ в NOR отличается — стирает, пишет
   по offset 0, проверяет и делает холодную перезагрузку, чтобы загрузка
   с передачей управления никогда не писала во флеш.
2. На загрузке передачи управления: проверка окна `0x90000000`, проверки PSRAM
   (16 слов) при старте, после чтений флеш и после remap.
3. **ICACHE remap**: регион 0, база `0x02000000` → `0x90000000`, 4 МБ,
   `LL_ICACHE_MASTER2_PORT`, `LL_ICACHE_OUTPUT_BURST_WRAP` — совпадает с
   `boot-hook.c` загрузчика платы.
4. **Прыжок**: `__disable_irq`, SysTick off, очистка `NVIC->ICER/ICPR`,
   `SCB->VTOR = 0x02000000`, **`MPU->CTRL = 0`**, **`__set_MSPLIM(0)`,
   `__set_PSPLIM(0)`** (иначе первый push нового образа — STKOF),
   `CONTROL = 0`, `MSP = vt[0]`, переход на `vt[1]`.
5. **XIP-образ**: слинкован на `CONFIG_FLASH_BASE_ADDRESS=0x02000000` (окно
   remap), **без драйверов OCTOSPI** (`CONFIG_FLASH=n`, `CONFIG_MEMC=n`) — оба
   окна настроены stage1. В логе `code at 0x2000515` подтверждает выборку
   инструкций через remap.
6. Образ встраивается в stage1: `xxd -i -n xip_image zephyr.bin > stage1/src/xip_image.h`.

Грабли:
- Не сбрасывать плату во время потока в XIP — NOR зависает в транзакции,
  следующая загрузка `SFDP magic … invalid` (research §9.2).
- `sys_reboot(SYS_REBOOT_COLD)` под отладчиком может не перезапустить MCU —
  `st-flash --connect-under-reset reset`.
- Вариант «PSRAM поднимается в самом XIP-образе» (без stage1) не доведён:
  нужно исполнять MSPI-драйвер и `stm32u5xx_hal_ospi.c` из SRAM
  (`CODE_DATA_RELOCATION`) и иметь в образе драйвер флеш; зависал на
  `mspi_stm32_ospi_init`.

Промежуточные версии stage1 и XIP-образа — `test-infra/xip-intermediate/`
(итоговые правились через shell и не восстанавливаются).

## Тестовые программы

| Программа | Когда | Что делает |
|---|---|---|
| probe ёмкости (алиасинг) | 11.08 | маркеры по +1/+2/+4/+8 МБ — определил реальные 8 МБ |
| hash-тест 4 МБ | 11.08 | 10 проходов запись+проверка на загрузку (медленный — tCEM не проявлялся) |
| muxed-тесты | 11.08 | чередование 256 КБ PSRAM / 4 КБ флеш; пословное с чтением 64 Б флеш каждые 64 слова; оба окна memory-mapped, 8 проходов |
| paced-тест | 11.08 | 64 байта с паузой 20 мс; 64 КБ словами с паузой 1 мкс каждые 256 Б |
| `stress_test_two_phase.c` | 12–17.08 | 100 проходов × 8 МБ, новый seed: A — плотный цикл 32-битных записей, B — `memcpy` чанков 4 КБ; `SUMMARY`/`RESULT` |
| сэмпл `samples/drivers/memc` + `verify_run.py` | 01.09 | детект, init, отсутствие ошибок/dropped, монотонность дампа, вердикт `Read data matches written data` |
| `psram_stress` + `analyze_stress.py` | 01–02.09 | 100 циклов × 2 паттерна (адрес и инверсия) на весь объём непрерывно, сброс D-cache, проверка на устройстве, в консоль только служебные строки и `TEST PASS/FAIL` |

## Результаты

### Август, топология A (ESP-PSRAM64H)

| Дата | Тест | Результат |
|---|---|---|
| 11.08 | 3 холодных старта × 10 проходов × 4 МБ | 30/30 чисто, 373 мс/проход (~22 МБ/с) |
| 11.08 | muxed: грубое и пословное чередование, 4 загрузки | PASS, флеш не повреждена |
| 11.08 | оба окна memory-mapped, пословное чередование | 8 проходов, checksum флеш стабилен, 0 ошибок PSRAM |
| 11.08 | плотный цикл без CE-break | порча с +0x800: 3584 ошибки из 4096 |
| 11.08 | paced, внутренняя флеш и XIP | PASS |
| 11.08 | `st,csbound` 0/7/8/10 | см. research §4.2 |
| 12.08 | stress 100 × 8 МБ, `csbound=0` | FAIL: ~2 092 000 / 2 097 152 слов с +0x800 (tight), stream чисто |
| 12.08 | stress, `csbound=7`, внутренняя / XIP | 100/100 чисто; tight 447 мс, stream 945 / 997 мс |
| 12–13.08 | матрица named/manual × внутренняя/XIP | 4 × PASS 100/100 (~12,8 ГБ) |
| 17.08 | та же матрица + `rx-dummy=<4>` | 4 × PASS; негативный — FAIL, как ожидалось |
| 17.08 | SINGLE, проверка контроллера на месте | `MSPI_IO_MODE_SINGLE in 3Bytes addressing is not supported` → `Failed to enable memory mapping` |
| 17.08 | SINGLE named/manual, проверка убрана локально | PASS 100/100, 1747 мс/проход |
| 17.08 | финальные 5 тестов на `c0ec254a479` | 4 × PASS + негативный FAIL; ELF: named `chip = &chip_table`, manual `chip = NULL`, `read_cmd = 0xEB`, `rx_dummy = 6` |

### Негативные и граничные случаи

| Конфигурация | Поведение |
|---|---|
| generic без `size` | `Generic mode requires size in DT`, init прерван |
| `mspi-io-mode` не задан (12.08, до SINGLE) | `Only MSPI_IO_MODE_QUAD supported, got 0`; сейчас это валидный SINGLE |
| `mspi-io-mode = OCTAL` | `mspi-io-mode 7 not supported, use SINGLE or QUAD` |
| `rx-dummy = <4>` вместо 6 | init OK; tight `BAD 2097152 @0x0`, stream ~8,35 из 8,39 МБ битые |
| `read-command = <0x03>` в QPI | init OK, чтение — мусор с адреса 0 |
| `chip-variant = "ESP64H"` на IS66WVS4M8BLL | WRN о несовпадении, `initialised … 8192 KB`, BUS FAULT по `0x90400000` |
| DT `size` ≠ определённому в AUTO | `DT size 8388608 B differs from the detected 4194304 B; using the detected size` |

### Сентябрь, топология B

| Чип | Режим | Лог init | Результат |
|---|---|---|---|
| ESP-PSRAM64H | AUTO, сэмпл memc | `Detected ESP-PSRAM64H (ID 0D 5D 53 12 58)`, `PSRAM initialised in QPI mode, 8192 KB` | `Read data matches written data`; `verify_run.py`: PASS, 0 глитчей в deferred-режиме |
| ESP-PSRAM64H | AUTO, `psram_stress` | то же | TEST PASS 100 циклов, 89 с (~890 мс/цикл, ~18 МБ/с при 50 МГц) |
| IS66WVS4M8BLL | AUTO | `Detected IS66WVS4M8BLL (ID 9D 5D 40 C1 40)` | TEST PASS, 447 мс/цикл |
| IS66WVS4M8BLL | generic | `Generic PSRAM: skipping KGD check (ID MF=0x9D KGD=0x5D EID=0x40)` | TEST PASS, 447–448 мс |
| IS66WVS4M8BLL | named | без WRN, без Detected/Generic | TEST PASS, 447–448 мс |

ELF 02.09: generic — `chip_table` нет; named `IS66WVS4M8BLL` — 88 байт, в
образе только имя `IS66WVS4M8BLL`; AUTO — 396 байт, все 9 имён.

### Сравнение с `aps6404l` (11.08, топология A)

In-tree `aps6404l` на ESP-PSRAM64H с полным описанием в DT: `RESULT: PASS`
(paced). Три мягких сброса подряд — PASS, но каждый раз
`<wrn> memc_mspi_aps6404l: Vendor ID does not match expected value of 0xd`.

## Правила проведения тестов

- Порт VCP — только по серийнику ST-LINK (memory zephyr-проекта
  `stlink-vcp-by-serial.md`).
- Проверять **весь** лог, а не grep ожидаемых строк: `grep -iE
  "err|fail|fault|warn"` плюс хвост. Тест обязан печатать явный итог
  (`TEST PASS` / `TEST FAIL: причина`).
- Анализировать только последнюю загрузку в захвате: в начало лога
  попадает хвост предыдущей прошивки.
- Все сравнения данных — на устройстве; в консоль не печатать дампы.
- Стресс должен содержать плотный цикл записей, а не только `memcpy`.
- Не прошивать конфигурации под другой чип без согласия автора.
- NRST не сбрасывает PSRAM; для чистого старта — обесточить плату.
- Зависание ST-LINK (`LIBUSB_ERROR_TIMEOUT`, chipid 0x000) — переподключить USB
  программатора; логи, снятые в это время, не считать результатом.
- Логи драйвера при init: `CONFIG_SHELL=n` (shell backend роняет ранние
  строки). Для сэмпла memc: `LOG_DEFAULT_LEVEL=1`, MSPI до WRN, буфер 8–32 КБ,
  deferred-режим; `CONFIG_LOG_MODE_IMMEDIATE` с полным дампом — ~30 мин и
  UART-глитчи. Для `psram_stress` — `CONFIG_LOG_MODE_MINIMAL`.
