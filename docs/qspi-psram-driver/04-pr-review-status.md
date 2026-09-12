# Статус ревью PR #104100

Состояние на **02.09.2026 07:33 UTC** (конец второй сессии). После этого PR
не проверялся — перед продолжением работы стоит заново прочитать комментарии.

PR: <https://github.com/zephyrproject-rtos/zephyr/pull/104100>
Заголовок: `drivers: memc: add MSPI QPI PSRAM driver` (переименован 17.08).

## Участники

| Ник | Роль | Позиция |
|---|---|---|
| swift-tk | мейнтейнер MSPI | CHANGES_REQUESTED; хочет убрать C-таблицу в пользу per-device compatible |
| erwango | мейнтейнер STM32 | CHANGES_REQUESTED (обновлён 02.09); build-time валидация в биндингах |
| etienne-lms | ревьюер | мелкие замечания (14.08), просил сквош |
| mariopaja | контрибьютор MSPI | 06.07: не разделить ли на MSPI-слой и memc-драйвер |
| FRASTM | ST | 03.07: заголовок PR про QSPI, а не STM32 OSPI |
| rruuaanng | ревьюер | ревью 5041259982: `title:` в биндинге |

## Треды и их состояние

| # | Кто | Суть | Код | Ответ в треде |
|---|---|---|---|---|
| 1 | erwango | копирайт без правообладателя | ✅ SPDX проекта + автор | outdated, не resolved |
| 2 | swift-tk, `:373` | «You are expecting it to fail?» — `if (!mspi_dev_config(...))` | ✅ ошибка возвращается | на 17.08 — единственный тред, где последнее слово не наше; нужна одна строка |
| 3 | swift-tk, `:452/:521` | дублирующий `mspi_dev_config` со `spi_init_cfg`; зачем serial-init, если потом force-exit | ✅ вызов убран | текст подготовлен (приложение B) |
| 4 | swift-tk ×3 | переименование compat/биндинга под #108082 | ✅ | — |
| 5 | swift-tk (общий) | sample/test для сборки в CI | ✅ 3 build-only конфигурации на `b_u585i_iot02a` | попросить переревьюить |
| 6 | swift-tk + erwango, `:428` | serial-режим не должен запрещаться драйвером устройства | ✅ SINGLE поддержан | в треде осталась фраза «no way to test MODE_SINGLE» — устарела, см. приложение B |
| 7 | erwango | таблица в рантайме «isn't the usual zephyr approach» | ✅ compile-out (коммит `e2953b72575`) | отвечено; позже erwango: «I don't understand why we wouldn't use them» (свойства `mspi-device.yaml`), ждёт вердикта swift-tk |
| 8 | **swift-tk, строка 103 (25.08)** | **убрать C-таблицу; параметры в DT; init-команды — макросами; per-device compatible, наследующий `qspi-psram`, с дефолтами** | ❌ | **главный открытый пункт** |
| 9 | etienne-lms a–i | см. ниже | a, b, c, e, f, g ✅; d — отклонён; h ✅ | ответы даны (17.08) |
| 10 | rruuaanng | `title:` в биндинге | ✅ применено через GitHub (`4235a29895d`) | — |
| 11 | mariopaja (06.07) | разделить MSPI-слой и memc-драйвер | не требуется | на 17.08 без ответа; в разборе 01.09 открытым не значился |
| 12 | FRASTM (03.07) | заголовок PR | ✅ | можно коротко подтвердить |
| 13 | **erwango, биндинг строка 89 (02.09)** | `mspi-io-mode`: `default: "MSPI_IO_MODE_SINGLE"` и сузить `enum` до поддерживаемых | ❌ | **открыт** |
| 14 | **erwango, биндинг строка 28 (02.09)** | generic-режим — отдельный compatible `qspi-psram-generic` с `required: true`, вместо проверок при init | ❌ | **открыт** |
| — | SonarQube | Quality Gate passed; Cognitive Complexity исправлена | ✅ | — |

### etienne-lms, «Some minor nitpicking comments» (14.08)

| Пункт | Замечание | Итог |
|---|---|---|
| a | `description: >` → `\|` (15 мест) | сделано |
| b | `const` для `mspi-io-mode` и `mspi-data-rate` | data-rate — `const`; io-mode — `const` снят после добавления SINGLE |
| c | убрать `__LINE__` из `LOG_ERR` (24 места) | сделано |
| d | `uint8_t id[3] = {0}` — инициализация не нужна | **оставлено намеренно** (правило автора) |
| e | перенести содержимое `.h` в `.c` | сделано, заголовок удалён |
| f | `DT_INST_PROP` вместо `_OR` для свойств с `default:` | сделано для 5 init-команд |
| g | пустые экранированные строки в `DEFINE`-макросе | сделано |
| h | переименовать PR | `drivers: memc: add MSPI QPI PSRAM driver` |
| i | сквош | обещан в конце |

## Открытые вопросы и разбор (на 02.09)

### A. per-device compatible (swift-tk, тред 8) — главный блокер

Предложение: вместо `chip-variant` + `chip_table[]` — отдельный биндинг на
каждую деталь с `default:` значениями из даташита. Реализуемость проверена
(research §7). Честные контраргументы, которые стоит озвучить в треде, а не
молча переделывать:

1. Дефолт в биндинге — одно значение, а драйвер сейчас выбирает набор команд
   и dummy по `mspi-io-mode` (`EBh`/6 для QUAD, `0Bh`/8 для SINGLE,
   `memc_mspi_qspi_psram.c:834-838`). Дефолтами будут QUAD-значения, для SINGLE
   придётся переопределять три свойства или держать фолбэк в драйвере.
2. KGD — нужно своё свойство (swift-tk разрешил «psram specific properties if
   necessary»).
3. Таблица для AUTO остаётся оправданной: данные **неизвестного** чипа в DT не
   выразить. Лучше представить AUTO как отдельную фичу, а не как довод за
   таблицу для именованных частей.

### B. erwango 02.09 — `mspi-io-mode`

- `default: "MSPI_IO_MODE_SINGLE"` — **прав, принять**. В базовом
  `mspi-device.yaml` дефолта нет, добавить ключ разрешено; уйдёт фраза «note that
  an omitted value means SINGLE».
- Сужение `enum` — **ловушка**: edtlib запрещает переопределять `enum`
  унаследованного свойства, а главное, `MSPI_DEVICE_CONFIG_DT` берёт
  `DT_ENUM_IDX` — список `[SINGLE, QUAD]` превратит QUAD в `MSPI_IO_MODE_DUAL`.
  В сниппете erwango был префикс исходного списка — индексы сохраняются, но DUAL
  проходит. Ответ: default принимаем, про enum объяснить индексную зависимость
  и оставить рантайм-проверку в `qspi_psram_check_dt_cfg`.

### C. erwango 02.09 — `qspi-psram` + `qspi-psram-generic`

По существу прав: `qspi-psram` — табличные режимы (именованный и AUTO,
`chip-variant` → `required`), `qspi-psram-generic` — все параметры передачи
`required: true`. Драйвер обслуживает оба compatible (второй `DT_DRV_COMPAT`
через `#undef`), `qspi_psram_check_dt_cfg` почти исчезает. Цена — второй YAML и
дублирование instance-макроса.

**Рекомендация 02.09**: A и C тянут в одну сторону («пусть биндинги
валидируют и параметризуют»). Ответить в оба треда **одним планом до начала
кода**: база `qspi-psram-generic` с обязательными свойствами → per-device
compatible с дефолтами поверх → AUTO как отдельное значение/compatible. Код
AUTO (таблица id+mask, детект, worst-case tCEM) рефакторинг переживёт — меняется
только источник признака режима.

### D. Прочее до мержа

- Сквош всех коммитов в один (включая веб-коммит `4235a29895d`, который не
  пройдёт gitlint UC3).
- Позакрывать outdated-треды короткими ответами, попросить swift-tk и erwango
  переревьюить.
- Отдельно (не в этом PR): `mspi_stm32_ospi.c` — CE-break из
  `mem_boundary`/`time_to_break`, кламп `tx_dummy`, CSHT vs tCPH, проверка
  SINGLE + 24-bit (research §4).

---

## Приложение A. Комментарий в PR от 12.08

Версия, приведённая к формулировкам автора (опубликованный текст мог
отличаться). Часть утверждений с тех пор устарела: драйвер поддерживает SINGLE,
AUTO, есть CI-сборка.

```text
Hi all, sorry for leaving this PR unattended for so long.

I have unified the driver and reworked the checks @swift-tk pointed at — a failing
mspi_dev_config() is no longer treated as an expected outcome but returns the error,
and the duplicated device configuration before the QPI exit sequence is gone. I also
found a small mistake in my own binding, where several properties were marked required
although they are not. And I made the driver more universal: if you use a known part
you can simply name it and the driver will pull the size, the commands and the timings
itself, or you can spell everything out by hand. Both configurations are in the binding
as examples:

Named part — the driver knows the rest:

&octospi2 {
        psram0: psram@0 {
                compatible = "qspi-psram";
                reg = <0>;
                chip-variant = "ESP64H";
                mspi-max-frequency = <DT_FREQ_M(80)>;
                mspi-io-mode = "MSPI_IO_MODE_QUAD";
                memmap-config = <1 0 0 0>;
        };
};

Any other QPI PSRAM — describe it yourself:

&octospi2 {
        psram0: psram@0 {
                compatible = "qspi-psram";
                reg = <0>;
                size = <DT_SIZE_M(64)>;
                mspi-max-frequency = <DT_FREQ_M(80)>;
                mspi-io-mode = "MSPI_IO_MODE_QUAD";
                read-command = <0xeb>;
                write-command = <0x38>;
                command-length = "INSTR_1_BYTE";
                address-length = "ADDR_3_BYTE";
                rx-dummy = <6>;
                ce-break-config = <1024 8>;
                memmap-config = <1 0 0 0>;
        };
};

I wanted this to be as universal as the flash drivers are today. The trouble is that
PSRAM gives you nothing to compute from: there is no SFDP and no JESD216 table, Read ID
returns only the manufacturer byte, KGD and EID. Size, maximum burst length, the refresh
interval — none of it is discoverable at runtime, so I keep a table of known parts,
which can be extended as more of them show up. The driver also implements the MEMC API,
so the size it knows is available to the application through memc_get_size().

I also went through the reset procedure and finished it properly. A device does not
always restart with a full power cycle, so the memory stays in the mode it was
configured in before, and it matters to bring it back to the state it has after a
reset. Not all existing drivers do that today — on my board aps6404l reads a garbage
vendor ID after a warm reset, warns about it and carries on anyway.

I can see that drivers have appeared in the meantime whose functionality partially
overlaps with this one, although mine is broader. So the question is really whether
this code is still wanted, or whether this PR should be closed.

The problem I ran into: the STM32 MSPI controller driver never releases the chip select
inside a long transfer, which PSRAM does need — mem_boundary and time_to_break are
passed to it through mspi_dev_config() but never programmed into the hardware. The
workaround is to limit the number of bytes per transaction with st,csbound on the
controller node, which costs roughly 2.5 to 6% of throughput depending on the boundary
you pick. The proper fix belongs in the controller driver and I would do it in a
separate PR.
```

Позже уточнено: `ChipSelectBoundary` драйвер контроллера **программирует** из
`st,csbound`; не выводит его из `mem_boundary`, а `Refresh` не программирует
вовсе. Формулировку «never programmed» стоит смягчить при следующем ответе.

## Приложение B. Подготовленные короткие ответы

**swift-tk, `:373`** (тред outdated, но без ответа):
> The inverted condition is gone — `mspi_dev_config()` is now checked for an
> error like any other call and the failure is returned.

**swift-tk, `:452/:521`** (подготовлено 12.08):
> Both fixed, thanks. The `mspi_dev_config()` with `spi_init_cfg` in `init` is
> gone — it was redundant, because `qspi_psram_force_spi_mode()` ends by
> restoring exactly that configuration, so init now calls it directly and the
> controller is left ready for the 1-line commands that follow. […] What is
> still ignored on purpose are the three commands themselves — Exit QPI, Reset
> Enable and Reset sent on 4 lines. A chip that is already in SPI mode does not
> decode them, and that is harmless. As for why the exit is unconditional: there
> is no way to ask the chip which mode it is in. Read ID is N/A in QPI per the
> datasheet, so a chip left in QPI by a previous boot answers identification with
> zeros. Issuing the exit sequence blindly is the only way to reach a known state.

**Дополнение в тред `:428` про SINGLE** (17.08):
> Verified on STM32U585 + ESP-PSRAM64H with the SINGLE/24-bit check in
> `mspi_stm32_ospi.c:208` removed locally: 100 × 8 MB stress passes in SPI mode
> with both chip-variant and generic DT descriptions. With the check in place the
> driver correctly reports the controller's `-EIO` from `mspi_memmap_config()`.

**etienne-lms, пункт b** (17.08):
> Added `const` for `mspi-data-rate`. For `mspi-io-mode` I have not, because
> following @swift-tk's comment the driver now supports SINGLE as well as QUAD,
> so the property is not a single value anymore; the accepted set is checked at
> init.

**etienne-lms, пункт d**: оставить инициализацию как стиль автора (переменные
инициализируются при объявлении). Формулировку Claude от 17.08 «the buffer is
logged in the error path» использовать **нельзя** — в текущем коде при ошибке
чтения ID буфер не логируется.

**mariopaja** (идея ответа, 17.08): разделение уже такое — контроллерная часть
живёт в `drivers/mspi/mspi_stm32_ospi.c` (#105219), этот PR — только
device-часть в `drivers/memc`, работает поверх MSPI API с любым контроллером.

**erwango, enum у `mspi-io-mode`** (02.09, идея ответа): default принимаем;
сузить enum нельзя без поломки — `MSPI_DEVICE_CONFIG_DT` берёт индекс через
`DT_ENUM_IDX_OR`, и `[SINGLE, QUAD]` превратит QUAD в `MSPI_IO_MODE_DUAL`;
поэтому допустимые режимы проверяются при init, как у остальных MSPI-устройств.
