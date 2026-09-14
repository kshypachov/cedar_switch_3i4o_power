# P5 — mock: logs and coprocessor brought to the device

Fork D1, 2026-09-14. Only `tools/api-contract/**` changed.

## Rules now shared with the device

| Rule | Mock |
|---|---|
| `esp32_logs` follows the UART owner (`uart_mode == console`), not ESP-Hosted; reasons `uart_usb_bridge` / `uart_flashing` / `uart_unavailable` | `Logs.esp32_availability()`, `Coprocessor.uart_mode()`; scenario `uart_mode` |
| `CoprocessorStatus.uart_mode` from the scenario (install job still reports `flashing`); no longer `unavailable` just because the C6 is offline | `firmware.py` |
| `LogSources`: stm32 generation 0, esp32 = coprocessor generation; esp32 available/reason = `esp32_logs`; `dropped_count` = lost before the ring | `logs.py` |
| Two rings (per source, `log_ring_records`), one seq, records arrive over clock time (`log_rate_per_s`) | `Logs.settle()` from `DeviceState.settle()` |
| Cursor = version, boot tag, filter hash, next seq, checksum. Other boot → newest `limit` + current boot_id + `gap=true`; empty/corrupt/other filter/past head → 400 `invalid_cursor` | `_encode_cursor` / `_decode_cursor` |
| Cursor behind an overwritten record → `gap=true`, continue at the oldest kept | `page()` |
| Page bounded by `limit`, 16384 bytes of JSON (64 reserved for the cursor), and records looked at (`log_scan_budget`); a cut page has `has_more=true`, cursor at the next record to look at | `page()` |
| `contains` folds A–Z only; `module` exact | `ascii_lower()` |
| Export: `gap` record (level/module null, source of the lost ring, seq of the first record after) when part of the snapshot is overwritten (`log_export_lost` stands in for the race); text header `# cedar logs <boot_id>: bounded snapshot, nothing from before this boot; gaps are marked`, gap line `# gap: …`, `<uptime_ms> <source> <level|-> <module|-> <message>` with CR/LF as `\n` | `export()` |

## Control plane

- Scenario keys: `uart_mode` ("console"), `log_rate_per_s` (0), `log_ring_records` (10000),
  `log_scan_budget` (1000000), `log_export_lost` (0) — `POST /__mock/scenario`, `POST /__mock/reset`
  with `{"scenario": …}`, or `--scenario key=value`.
- `POST /__mock/reboot` — new boot: new `boot_id`, uptime 0, sessions/jobs/rings/transactions gone;
  admin password, committed network config and C6 version kept. The browser has to sign in again.

## Tests

`tests/ci/run-contract-tests.sh`: checks 5/5, mock suite 352 passed (340 before P5 → +11 in
`test_mock_logs.py`, +1 in `test_mock_control.py`), generator 14. Changed intent in two existing
tests: offline C6 → `uart_mode` stays `console`; `/__mock/reboot` is now a control endpoint (the
unknown-endpoint test uses `shutdown`).

## Proposed contract wording (not applied — openapi.json / api-contract.md are not this fork's)

Append to «Логи» in api-contract.md:

- `dropped_count` — записи, потерянные до кольца (ядро лога, переполнение UART); перезапись кольца
  сообщает `gap`, а не `dropped_count`.
- Страница ограничена также буфером ответа устройства и числом просмотренных за запрос записей:
  `items` может быть короче `limit` и даже пустым при `has_more=true`; `next_cursor` — следующая
  запись для просмотра.
- `contains` сравнивает без учёта регистра только латинские A–Z.
- Cursor другой загрузки — не ошибка: текущий хвост (как без cursor), новый `boot_id`, `gap=true`.
- В экспорте потерянный участок — запись `kind="gap"`, `level`/`module` null, `source` — кольцо,
  потерявшее записи, `seq` — первой записи после разрыва; в text — строка `# gap: …`.
- `esp32_logs.available` следует владельцу UART (`uart_mode=console`), причины `uart_usb_bridge`,
  `uart_flashing`, `uart_unavailable`; готовность ESP-Hosted — `CoprocessorStatus.transport_ready`.
- `esp32_uart` и `CoprocessorStatus.uart_update` до P6 — `available=false`, `reason="not_implemented"`
  (mock сохраняет модель P6 для экрана обновления).
