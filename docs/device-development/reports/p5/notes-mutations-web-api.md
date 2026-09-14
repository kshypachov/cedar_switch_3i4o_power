# P5 — мутации web-api и привязок логов/копроцессора (форк M4)

Только в копии: worktree скопирован rsync (без `node_modules`, `.git`) в scratchpad
`mut-m4/cedar_p5`, передан `docker cp` в контейнер `cedar-mut-m4` (образ
`cedar-sim-tests:1`, `/ws` = рабочее пространство west). Сборка сюиты один раз
`cmake -GNinja -S /mut/cedar_p5/tests/<suite> -B /mut/b-<suite> -DBOARD=native_sim/native/64`,
на мутант — правка файла в копии, инкрементальный `ninja`, запуск `zephyr.exe`; убит —
ненулевой код, нет `PROJECT EXECUTION SUCCESSFUL` или таймаут. Раннер —
`mut-m4/run_mutants.py`, список — `mut-m4/mutants.json` (каждый мутант проверен на
единственное вхождение до запуска). Исходники worktree не менялись, только тесты.

## Итог

| Файл | Мутантов | Убиты сразу | Убиты новыми тестами | Эквивалентны / недостижимы |
|---|---:|---:|---:|---:|
| `src/web/api/v1/logs.c` (запрос, страница, экспорт, источники) | 49 | 30 | 14 | 4 (L03, L17, L32, L42) + 1 недостижим (S02) |
| `src/web/api/v1/coprocessor.c`, строка `esp32_logs` в `system.c` | 9 | 6 | 3 (C03, C04, C06) | 0 |
| `modules/web-api/lib/web_api.c` (поток, query, заголовок) | 19 | 13 | 5 (W09, W10, W13, W14, W15) | 1 (W16) |
| `modules/web-api/lib/json_writer.c` (rollback, room) | 5 | 2 | 2 (J02, J04) | 1 (J05) |
| `modules/web-api/lib/web_api_http.c` (адаптер, поток, SNDTIMEO) | 10 | 8 | 0 | 2 (H05, H06) |
| **итого** | **92** | **59** | **24** | **8** (+ S02 недостижим) |

Мутанты адаптера — на `web_api_http` (настоящий сервер Zephyr по loopback): H01 (поток не
продолжается) и H04 (продолжение не распознаётся) — таймаут; H02, H03, H10 — экспорт кусками
не доходит целиком; H07, H08, H09 (`SO_SNDTIMEO` не ставится / в 10 раз больше / не на
каждом запросе) — зависший клиент экспорта держит сервер.

Проверка worktree после всех тестов: `tests/ci/run-sim-tests.sh -s cedar.web_api -s cedar.web_api_http` —
**160 из 160** (2 конфигурации). Контейнер `cedar-mut-m4` удалён.

(В строке `logs.c` учтены S02–S05 из `v1_get_log_sources`; S01 — строка `esp32_logs` в `system.c`.)

## Выжившие, признанные эквивалентными

- **L03** — `rc <= 0` → `rc < 0` в `query_int`: пустое значение даёт `v = 0`, и проверка
  диапазона `v < min` (min = 1) отвергает его тем же `invalid_query`.
- **L17** — `rc > 0` → `rc >= 0` перед `log_store_cursor_decode`: пустой cursor декодер
  отвергает `-EINVAL` (тест log-store), ответ тот же `invalid_cursor`.
- **L32** — убран `st->remaining--` в экспорте: `log_store_reader_tail(max_records)` ставит
  читателя ровно на новейшие `max_records` совпадений, а `upper_seq` отсекает всё новее
  снимка; после gap читатель прыгает только вперёд. Второй счётчик ничего не отсекает.
- **L42** — убран `st->reader.gap = false` после `reader_tail`: `reader_init()` обнуляет
  читателя целиком (`log_store.c:488`).
- **S02** — `!log_store_ready()` → `0`: в сюите хранилище инициализировано до любого
  запроса, API «деинициализации» нет; на плате ветка возможна только до `SYS_INIT`
  zephyr-log-source, когда HTTP-сервер ещё не запущен. Недостижима в sim.
- **W16** — `i + 2U < vn` → `<=` при разборе `%XX`: лишний прочитанный символ — это
  символ query сразу за значением, то есть `&` или NUL, не шестнадцатеричная цифра, и
  декодирование не срабатывает ни в каком из вариантов.
- **H05** — `slot_release()` без `web_api_stream_end()`: единственный потоковый обработчик
  (экспорт) регистрирует `end = NULL`, а флаг `active` снимают `slot_take()` и начало
  `web_api_dispatch()` при следующем использовании слота. Эквивалентен **для текущих
  обработчиков**; станет наблюдаемым, как только поток получит `end`, освобождающий
  ресурс, — тогда нужен тест на ABORTED с таким обработчиком.
- **H06** — `slot_take()` без `web_api_stream_end()`: дублирует вызов в начале
  `web_api_dispatch()`; защита в глубину, эквивалентен.
- **J05** — `pos + 1 >= cap` → `pos >= cap` в `room`: при `pos == cap - 1` оба варианта дают
  0 (`cap - pos - 1 = 0`), `pos >= cap` у писателя не бывает.

## Добавленные тесты (только тесты)

`tests/web_api/src/v1_logs.c` (сюита `v1`, 64 → 75):
- `test_logs_integer_query_values_are_digits_only` — `limit=0a` (L04);
- `test_logs_module_filter_at_its_limits` — модуль ровно 64 символа/байта совпадает; длинный
  кириллический модуль ничего не находит, но `dropped_count` выбранного источника верен
  (L08, L09, L27);
- `test_logs_truncated_records_and_source_stm32` — `truncated` в ответе, `source=stm32`
  (L14, L44);
- `test_logs_scan_budget_is_spent_on_the_page_not_the_tail` — хвост не расходует бюджет
  страницы; страница, съевшая бюджет на записях, говорит `has_more=true` (L19, L20);
- `test_logs_export_marks_a_gap_between_records` — gap посреди экспорта: одна строка gap с
  `seq` следующей записи, дальше сохранившаяся часть снимка (L30, L31);
- `test_logs_export_finds_a_rare_match_past_the_scan_budget` — кусок, исчерпавший бюджет без
  совпадений, не завершает экспорт (L33);
- `test_logs_export_text_writes_every_line_break_as_an_escape` — одиночный CR и CRLF (L34);
- `test_coprocessor_state_and_version_follow_the_transport` — `starting` в первые 20 с,
  `offline`/`failed` по `rx_seen`, версия только при готовом транспорте (C03, C04, C06);
- `test_logs_a_new_request_ends_a_stream_left_open` — dispatch посреди потока завершает его
  (W09, W10);
- `test_logs_an_empty_contains_is_no_filter` — `contains=` = отсутствие фильтра, cursor
  продолжает (L12: с мутантом пустой `contains` менял дайджест фильтра, и cursor без него
  отвергался как `invalid_cursor`);
- в `test_logs_export_marks_what_the_ring_overwrote_meanwhile` — источник gap в конце
  экспорта (L40).

`tests/web_api/src/middleware.c` (30 → 33): `test_query_get_decodes_form_encoding`,
`test_query_get_finds_the_named_parameter`, `test_query_get_bounds` (W13, W14, W15).

`tests/web_api/src/json_writer.c` (7 → 8): `test_room_and_rollback` (J02, J04).

По ходу: первый вариант `test_logs_module_filter_at_its_limits` переполнял свой `path[160]`
(41 + 198 символов) и ронял сюиту segfault-ом — ошибка теста, исправлено (`path[300]`).

## Ошибки в производственном коде

Не найдено.
