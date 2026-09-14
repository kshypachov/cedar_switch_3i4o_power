# P5 — экран «Логи» (фронтенд)

Fork D2, 2026-09-14. Изменения только в `src/web/frontend/**` и в этом файле.

## Файлы

| Файл | Что |
|---|---|
| `src/api/logs.ts` | `getLogRecords`, `getLogSources`, `exportUrl` (ссылка на `/api/v1/logs/export`) |
| `src/api/types.ts` | `LogSource`, `LogSources`, `LogRecord`, `LogPage` из сгенерированной схемы |
| `src/features/logs/controller.ts` | Чистое ядро: фильтры → query, cursor, строки gap/boot, ключи, дедупликация, предел 2000 строк, `formatUptime`, `isAtBottom` |
| `src/features/logs/useLogFeed.ts` | Живой хвост на общем планировщике: 1000 мс, без перекрытия, догон до 5 страниц при `has_more`, сброс cursor на `invalid_cursor`, остановка на 404, отмена при уходе; на паузе задача остановлена |
| `src/features/logs/LogsScreen.tsx` | Фильтры (источник, уровень, модуль, текст с задержкой 400 мс), пауза, «К новым записям», таблица строк, выгрузка NDJSON/текст, источники с `dropped_count`, причиной недоступности ESP32 и `uart_mode` |
| `src/state/router.ts`, `src/components/Layout.tsx`, `src/App.tsx` | Маршрут `/logs`, пункт меню «Логи» |
| `src/i18n/ru.ts` | Строки `nav.logs`, `logs.*` |
| `src/i18n/dictionary.test.ts` | Словарь покрывает `LogSource.id`, `LogRecord.kind`, `LogRecord.level`, `CoprocessorStatus.uart_mode` |
| `src/test/fixtures.ts` | `logPage`, `logPageNext`, `logPageAfterReboot`, `logSources`, `logSourcesBridge`, `coprocessorBridge` (проверяются Ajv) |
| `src/styles.css` | Стили таблицы логов и строк gap/boot/маркеров |
| `src/features/network/NetworkScreen.test.tsx` | Список пунктов меню дополнен «Логи» (единственная правка вне списка файлов fork) |
| `e2e/logs.spec.ts`, `e2e/device.spec.ts` | e2e против mock; на плате — открыть «Логи» (только чтение); проверка карточки ESP32 без ожидания «недоступно» (P5 отдаёт `coprocessor/status`) |
| `README.md` | Решения и тесты экрана |

## Решения экрана

- Без cursor — хвост; дальше cursor из ответа. Пустая страница с `has_more=true` — нормальный ответ (бюджет просмотра устройства), cursor всё равно продвигается.
- Смена фильтра и `400 invalid_cursor` сбрасывают cursor и строки (без показа ошибки): хвост мог бы повторить уже показанные записи.
- `gap` — строка на месте разрыва, только для запроса с cursor; смена `boot_id` — строка-разделитель с новым id. Ключ строки — boot, source, kind, seq.
- Пауза останавливает опрос полностью; продолжение — с прежнего cursor, перезаписанное за паузу приходит как `gap` (проверено e2e на mock: кольцо 50 записей, 1000 записей за паузу).
- Выгрузка — обычная ссылка того же origin с текущими фильтрами (cookie сессии идёт сама).
- Строки лога — текстовые узлы React; разметка в сообщении показывается как текст.

## Тесты

- `tests/ci/run-frontend-tests.sh`: vitest **17 файлов, 260 тестов, все проходят** (P4 — 222; новые: `controller.test.ts` 15, `LogsScreen.test.tsx` 12, словарь +4 перечисления, фикстуры +6); сборка 100 283 Б gzip из 524 288; e2e против mock **18 из 18** (из них 5 — `logs.spec.ts`), `device.spec.ts` пропущен без `E2E_DEVICE_URL`.
- e2e `logs.spec.ts`: фильтр источника, пауза, заголовки обеих выгрузок; настоящий gap от перезаписи кольца за паузу (`log_ring_records`, `log_rate_per_s`, `/__mock/advance`); причина `uart_usb_bridge` и `uart_mode`; перезагрузка mock (выход, новый `boot_id`); разделитель загрузки и gap в живом хвосте — ответ на cursor прошлой загрузки подставлен `page.route`, потому что перезагрузка mock завершает сессию и страница не может пронести cursor через неё.

## Нужно от устройства

- Всё, что предполагает экран, совпадает с правилами mock D1: страница может быть короче `limit` или пустой при `has_more=true`; cursor прошлой загрузки — 200 с новым `boot_id` и `gap=true`; `LogSource.reason` из `uart_usb_bridge`/`uart_flashing`/`uart_unavailable` (незнакомая причина показывается как есть); экспорт отдаёт `Content-Disposition: attachment`.
- Устройство до P5 отвечает 404 — экран пишет «Недоступно в этой версии прошивки» и перестаёт спрашивать.
