# P5 — мутации модулей логов

Файлы: `modules/log-store/lib/log_store.c`, `modules/esp32-log-source/lib/esp32_log_source.c`,
`modules/zephyr-log-source/lib/zephyr_log_source.c`. Сюиты: `cedar.log_store` (обе
конфигурации: 4 КиБ и 256 КиБ), `cedar.esp32_log_source`, `cedar.zephyr_log_source`.

## Как

- Копия worktree (без `node_modules`, `.git`, `dist`) в scratchpad, `docker cp` в свой
  контейнер `cedar-sim-tests:1` (`/mut`; вторая копия `/mut2` — для zephyr-log-source, чья
  сюита компилирует и `log_store.c`, и для перепроверок); `/ws` — workspace, как в
  `run-sim-tests.sh`. Эталонный прогон копии до мутаций: 70/70, 29/29, 11/11.
- Мутант — точная замена строки (каждая встречается ровно один раз), `docker cp` одного
  файла, twister на сюиту, возврат файла. Убит — сюита упала (или зависла до таймаута
  twister: LS09); выжил — всё зелёное.
- Исходники модулей не менялись. Для каждого выжившего тест добавлен в worktree, эталон с
  новыми тестами на немутированной копии зелёный (log_store 86/86, esp32 33/33,
  zephyr 14/14), затем тот же мутант перезапущен и убит.
- Итоговый прогон worktree без изменений (`run-sim-tests.sh -s cedar.log_store
  -s cedar.log_store.production_sizes -s cedar.esp32_log_source -s cedar.zephyr_log_source`):
  **133 из 133** в 4 конфигурациях.

## Итог

| Файл | мутантов | убито сразу | выжило | убито добавленными тестами | эквивалентны |
|---|---:|---:|---:|---:|---:|
| `log_store.c` | 40 | 29 | 11 | 8 | 3 |
| `esp32_log_source.c` | 28 | 24 | 4 | 4 | 0 |
| `zephyr_log_source.c` | 17 | 12 | 5 | 5 | 0 |

Производственных ошибок мутации не нашли: все выжившие — пробелы тестов на границах.

## Выжившие и что их закрыло

### log_store.c

| Мутант | Изменение | Закрыт |
|---|---|---|
| LS01 | `length_sane`: `len >= RECORD_MIN` → `>` | `test_the_smallest_record_reads_back_both_ways` (запись без модуля и текста — ровно 32 Б — читается вперёд и обходится назад) |
| LS02 | `utf8_cut`: `back <= 4` → `< 4` | **эквивалентен**: при `back == 4` условие `back < need` невозможно (`need ≤ 4`), обе версии возвращают `n` |
| LS07 | модуль длиннее 64 не режется при записи | `test_a_long_module_is_cut_before_the_text_is_placed` (300-байтный модуль и модуль с буквой на границе 64; текст после модуля цел) |
| LS08 | вытеснение `>` → `>=` | `test_a_ring_filled_to_the_last_byte_overwrites_nothing` (кольцо, заполненное до байта, ничего не вытесняет; следующая запись — одну) |
| LS13 | `kind > GAP` → `>=` | `test_the_last_level_and_kind_are_accepted` |
| LS20 | `contains`: `i + n <= hay_len` → `<` | `test_contains_at_the_end_and_the_whole_text` |
| LS22 | `reader_at` без ограничения позицией head | `test_a_reader_placed_past_the_head_stands_at_it` (cursor такого читателя принимается) |
| LS25 | `reader_tail`: `bp <= tail` → `<` | **эквивалентен**: при `bp == tail` проверка `bp - tail < len` (0 < 32) тут же завершает обход того же кольца, в том числе пустого |
| LS35 | снята проверка канонического написания cursor | `test_a_cursor_with_its_unused_bits_set_is_invalid` |
| LS36 | текст `contains` не входит в хеш фильтра | `test_a_cursor_is_bound_to_the_filter_text` (`abc`/`abd`, `net`/`nfs`) |
| LS39 | контрольная сумма cursor без последнего байта позиций | **эквивалентен**: непокрытый байт `raw[24]` — старший байт позиции ESP32; любое его изменение даёт позицию ≥ 2⁵⁶, больше head, и декодер отвечает `-EINVAL` той же проверкой |

### esp32_log_source.c

| Мутант | Изменение | Закрыт |
|---|---|---|
| ES01 | минимум строки ESP-IDF 7 → 9 байт | `test_the_shortest_esp_idf_line_parses` (`I (0) t:`) |
| ES12 | финальный байт CSI `<= 0x7E` → `< 0x7E` | `test_a_csi_ending_in_tilde_is_removed_whole` (`ESC [3~`) |
| ES27 | ESC не бросает начатый UTF-8 символ | `test_an_escape_abandons_a_started_character` (`E2 ESC[0m 82 AC` не становится «€») |
| ES28 | баннер `len >= 8` → `> 8` | `test_a_bare_banner_line_is_a_banner` (строка ровно `ESP-ROM:`) |

### zephyr_log_source.c

| Мутант | Изменение | Закрыт |
|---|---|---|
| ZS07 | трёхбайтовые overlong (`E0 80..9F`) приняты | новая строка `overlong three-byte` в `test_sanitise_rules` |
| ZS11 | отказ кольца не считается потерей | `test_a_message_the_store_refuses_is_counted` |
| ZS12 | обрез сырого буфера не помечает `truncated` | `test_a_cut_raw_output_is_truncated_even_if_it_shrinks` (1500 управляющих байтов → один U+FFFD, `truncated`) |
| ZS14 | `truncated = raw_cut` → `false` | тот же тест |
| ZS15 | модуль с `source_id == 0` без имени | `test_the_module_with_source_id_0_is_named` (`src/first.c` регистрирует `aaa_first`, первый по имени; тест проверяет, что его id — 0) |

## Тесты, добавленные в worktree

- `tests/log_store/src/main.c`: 8 тестов (раздел «found by mutation»).
- `tests/esp32_log_source/src/main.c`: 4 теста.
- `tests/zephyr_log_source/src/main.c`: 3 теста и строка в `test_sanitise_rules`;
  `src/first.c` и его строка в `CMakeLists.txt`.
