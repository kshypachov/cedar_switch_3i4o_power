# P5 — мутации фронтенда (логи) и mock (логи, uart_mode, busy)

Форк M3, 2026-09-14. Мутанты применялись только в копиях в scratchpad
(`mut-m3/repo`: `tools/api-contract`, `docs/device-development`, `src/web/api/v1/*.h`,
`modules/api-validation`, `modules/web-assets`, `src/web/frontend`), без docker.
Раннер — замена ровно одного вхождения строки, прогон, восстановление файла.

- фронтенд: `npx vitest run src/features/logs`;
- mock: `pytest -x tests/test_mock_logs.py tests/test_mock_network.py tests/test_mock_wifi.py tests/test_mock_control.py tests/test_mock_firmware.py`.

## Итог

| Файл | Мутантов | Поймано с первого прохода | Выжило → закрыто тестом | Эквивалентны |
|---|---:|---:|---:|---:|
| `src/web/frontend/src/features/logs/controller.ts` | 25 | 23 | 2 (F12, F23) | 0 |
| `src/web/frontend/src/api/logs.ts` | 1 | 0 | 1 (F26) | 0 |
| `src/web/frontend/src/features/logs/useLogFeed.ts` | 4 | 2 | 2 (F29, F30) | 0 |
| `tools/api-contract/cedar_contract/mock/logs.py` | 27 | 21 | 5 (M05, M07, M11, M21, M27) | 1 (M06) |
| `mock/network.py` (apply busy) | 2 | 2 | 0 | 0 |
| `mock/wifi.py` (scan busy) | 2 | 2 | 0 | 0 |
| `mock/firmware.py` (uart_mode при установке) | 1 | 1 | 0 | 0 |
| `mock/state.py` (`esp32_logs`) | 1 | 1 | 0 | 0 |
| **итого** | **63** | **52** | **10** | **1** |

После добавленных тестов живых мутантов нет, кроме одного эквивалентного.
Производственных ошибок мутации не нашли.

## Выжившие и чем закрыты

| Мутант | Изменение | Тест |
|---|---|---|
| F12 | ключ строки без `kind` | `controller.test.ts`: «keeps a gap record and a message that share boot, source and seq» |
| F23 | `isAtBottom`: `<= slack` → `< slack` | граница ровно 8 px в «follows only near the end of the list» |
| F26 | `exportUrl` пишет `undefined` в ссылку | «leave a parameter without a value out of the export link» |
| F29 | после `invalid_cursor` хвост на следующем тике, а не сразу | `LogsScreen.test.tsx`: «asks for the tail at once after a refused cursor» (<500 мс) |
| F30 | нет проверки `signal.aborted` после ответа | «does not apply a page that arrives after its filters changed» (отложенный ответ старого фильтра) |
| M05 | gap: `>= next_seq` → `> next_seq` | `test_mock_logs.py`: «a cursor at the record the ring just overwrote reports a gap» |
| M07 | байтовая граница `>` → `>=` | «a record that fills the buffer exactly is kept» |
| M11 | `dropped_count` одного источника вместо выбранных | «dropped_count is the selected sources» |
| M21 | в text-экспорте не экранируются одиночные LF и CR | «export text escapes a lone LF and a lone CR» |
| M27 | страница не резервирует место под cursor | «a page keeps room for the cursor it has not encoded yet» |

## Эквивалентный

M06 — `0 < self._overwritten[s] >= next_seq` → `self._overwritten[s] >= next_seq`.
Разница только при `next_seq == 0`. Mock выдаёт cursor с `next_seq ≥ 1`: это `seq`
записи, а `seq` начинается с 1, или `_next_seq ≥ 1`. Собрать cursor с 0 через API
нельзя, контрольная сумма его отвергает. Для любого выданного cursor условие
`0 >= next_seq` ложно.

## Пойманные с первого прохода (кратко)

- Фронтенд: условия `filterQuery`, trim модуля, cursor и limit в запросе, `format` экспорта,
  ранний выход `withFilters`, `bootId` при смене фильтра, `dropCursor`, `boot_id` в ключе,
  строки boot и gap, дедупликация, `ROW_CAP` и `trimmed`, cursor и `bootId` после страницы,
  формат uptime, догрузка при `has_more`, обработка `invalid_cursor`.
- Mock: причины `esp32_logs`, доступность, generation STM32, gap для cursor прошлой загрузки,
  отказ от байтовой границы, бюджет просмотра, `has_more`, cursor за головой, boot → 400,
  проверки хеша фильтров и контрольной суммы, ASCII-свёртка, `module`, `min_level`
  (с null и unknown), `seq` gap-записи, строка `# gap:`, уровень gap-записи, `_tail_start`,
  `_trim`, cursor продолжения, busy для apply и scan, `uart_mode=flashing` при установке,
  `esp32_logs` в capabilities.
