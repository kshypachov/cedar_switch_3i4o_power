# P6 — API v1: загрузка образа и отмена задач (форк F)

`src/web/api/v1/firmware.c` поверх `modules/firmware-store`; маршруты `routes.h`, ресурсы
`http_resources.h`; `system.c` (capabilities, resource_url задач), `coprocessor.c`
(`uart_update`, версия и протокол), `auth.c` (общий рабочий поток).

## Что обслуживается

`createUpload`, `getUpload`, `writeUploadChunk`, `verifyUpload`, `deleteUpload`, `cancelJob` — устройство
обслуживает **34 из 35** операций; осталась `startCoprocessorUpdate` (ждёт coprocessor-updater, форк B).

Новые ресурсы сервера (порядок имён — более частный шаблон раньше общего):
`web_api_06_a_job_cancel` `/api/v1/jobs/*/cancel` (проверка маршрутов требует ресурс на каждый путь),
`web_api_23_firmware_uploads`, `web_api_24_firmware_upload_data` `…/*/data`,
`web_api_25_firmware_upload_verify` `…/*/verify`, `web_api_26_firmware_upload` `…/*`.

## Что должна дать плата

- `fw_store_init("/lfs/firmware", k_uptime_get())` после монтирования `/lfs`, затем
  `web_api_v1_set_firmware(&hooks)` (`now_ms` можно NULL). До этого все пять операций загрузки —
  `503 service_not_ready` (проверено тестом).
- `fw_store_tick(k_uptime_get())` периодически — рабочий поток сервиса копроцессора (раз в минуту
  достаточно): иначе забытая загрузка истечёт только при следующем обращении к ней.
- `web_api_v1_set_coprocessor()` с `firmware_version` в виде `"vX.Y.Z"` (уже есть в P5).

## Решения привязки

- **Повтор `createUpload`** — как `stageNetworkConfig`: задачи нет, поэтому привязка хранит
  «ключ → id загрузки» (8 записей, 15 мин). Тот же ключ и тело — та же загрузка в её текущем
  состоянии (`201`), или `404`, если её уже удалили; тот же ключ с другим телом —
  `409 idempotency_conflict`; другой ключ при существующей загрузке — `409 busy`. `cancelJob`
  повторяется так же (он тоже не создаёт задачу): повтор после успешной отмены — `202` с той же задачей.
- **Кусок и проверка повторяются через job-manager**: ключ ищется до `fw_store_chunk_accept`, так
  что байты повтора не попадают в буфер второй раз; хеш запроса — длина и байты (web-api).
- **Один рабочий поток** (`web_v1_jobs`, `auth.c` → `v1_worker_submit()`): commit, verify и delete
  выполняются только там, как требует firmware-store. На каждую операцию один `k_work`; второй
  кусок/проверку/удаление той же загрузки хранилище не примет, пока первый не отработал.
- **Проверка, не прошедшая образ, проваливает задачу** кодом образа, а загрузка — `failed` с кодом и
  сообщением (как mock). Отмена проверки → задача `cancelled`, загрузка `receiving`. Если задачу
  проверки создать не удалось после захвата, рабочий поток снимает захват (`fw_store_verify` с
  отменой на первом вопросе).
- **Отказ удаления `busy` — до создания задачи** (установка держит файл, кусок ждёт записи, идёт
  проверка, удаление уже в очереди), чтобы отказ не оставлял записи под ключом.
- **`cancelJob`**: `job_cancel` → `202` с `resource_url` задачи; не отменяемая или завершённая →
  `409 invalid_state`; неизвестная → `404`; `network_apply` → `409 invalid_state` («через DELETE
  транзакции», как mock), даже если job-manager разрешил бы.
- **`offset`**: обязательный, только цифры, 1–10 знаков, ≤ `UINT32_MAX` (ведущие нули допустимы);
  иначе `400 invalid_query`.
- **capabilities**: `esp32_uart` доступна ровно при UART у консоли, иначе `uart_usb_bridge` /
  `uart_flashing` / `uart_unavailable` (те же слова, что у логов ESP32); `upload_max_bytes` =
  `CONFIG_FIRMWARE_STORE_MAX_BYTES`; `upload_chunk_bytes` = `MIN(CONFIG_WEB_API_OCTET_BODY_MAX,
  CONFIG_FIRMWARE_STORE_CHUNK_MAX)`.
- **CoprocessorStatus**: `uart_update` — как `esp32_uart`; `firmware_version` — версия от C6 при
  живом транспорте; `host_protocol` — `esp-hosted-mcu-<major>` этой версии, иначе null (раньше при
  транспорте без версии было `esp-hosted-mcu`); `last_update` — null до updater.
- **`resource_url` задач**: кусок и проверка → `/api/v1/firmware/uploads/{id}` (привязка помнит 8
  последних пар задача→загрузка), установка → `/api/v1/coprocessor/status`, удаление → null (mock).

## startCoprocessorUpdate (добавлено по сообщению координатора)

- Маршрут `POST /coprocessor/updates` (`CSRF | IDEMPOTENT | BODY_REQUIRED`, тело `UpdateRequest`:
  `upload_id` непрозрачный id ≤ 64, `method` из `ota|uart`, `acknowledge_recovery` bool), ресурс
  `web_api_27_coprocessor_updates`. Устройство обслуживает **35 из 35** операций.
- Порядок: повтор по ключу (`job_find_by_key`) → та же задача; `ota` → 503 `capability_unavailable`;
  не Ethernet (хук `request_over_ethernet` в `struct web_api_v1_coprocessor`; нет хука — не
  Ethernet) → 409 `ethernet_required`; хранилище не открыто → 503 `service_not_ready`; неизвестная
  загрузка → 404; установка уже активна → 409 `busy`; загрузка не `ready` → 409 `invalid_state`;
  `acknowledge_recovery=false` → 422; UART у USB-моста (или у другого прошивальщика) → 409 `busy`;
  UART `unavailable` → 503 `capability_unavailable`; `coprocessor_updater_check` `-EBUSY` → 409.
- Задача `coprocessor_update` создаётся отменяемой (updater выключает отмену на `begin`);
  `coprocessor_updater_start` с ошибкой → задача отменяется, ответ 409 `busy` (`-EBUSY`) или 500;
  `coprocessor_updater_run` — на том же рабочем потоке v1, что и файловые операции firmware-store.
  Удержание файла (`fw_store_set_in_use`) делает платформа платы в `image_open/close`, не привязка.
- Без `CONFIG_COPROCESSOR_UPDATER` маршрут есть и отвечает 503 `capability_unavailable`.
- CoprocessorStatus: `state="updating"` при `uart_mode=flashing` **или** активной установке;
  `uart_update` и `capabilities.esp32_uart` — `uart_flashing` при активной установке; `last_update` —
  `UpdateSummary` из `coprocessor_updater_get_state().last` (`version` null, если пуста; `error` —
  ErrorDetail с `request_id` ответа или null).
- Прошивка с glue платы собирается: FLASH 1 413 320 Б (33,72 %), RAM 86 728 Б, PSRAM 2 546 112 Б.

## Найдено: HTTP/1-сервер Zephyr не передаёт Content-Type (блокирует куски на устройстве)

**Исправлено координатором в web-api** (захват `Content-Type` девятым заголовком,
`CONFIG_HTTP_SERVER_CAPTURE_HEADER_COUNT=9`). Что было найдено:

`zephyr/subsys/net/lib/http/http_server_http1.c` нигде не заполняет `client->content_type` — это
делает только HTTP/2 (`http_server_http2.c:1404`). `web_api_http.c` (`begin_request`) читает
Content-Type именно оттуда, поэтому по HTTP/1 у каждого запроса `headers.content_type == NULL`:

- правило 8 для JSON («Content-Type не application/json → 415») по HTTP/1 никогда не срабатывает
  (отсутствие типа допускается);
- **октетный маршрут требует `application/octet-stream` и отвечает 415 любому куску** — на
  настоящем сервере (`tests/web_api_http`) кусок получает 415, хотя тип отправлен.

Unit-тесты `tests/web_api` этого не видят: они ставят `headers.content_type` сами. Исправление — в
`modules/web-api` (вне области форка F): зарегистрировать захват `Content-Type`
(`HTTP_SERVER_REGISTER_HEADER_CAPTURE`) и брать значение из захваченных заголовков; захватов уже 8
при `CONFIG_HTTP_SERVER_CAPTURE_HEADER_COUNT=8` — в `prj.conf` приложения и обоих тестов нужно 9.

## errno → HTTP (реализовано)

| Операция | errno хранилища | Ответ |
|---|---|---|
| createUpload | размер > `MAX_BYTES` (до хранилища, в т.ч. > 4 ГиБ) / `-EFBIG` | 413 `payload_too_large` |
| | `-EBUSY` | 409 `busy` |
| | `-ENOSPC` | 507 `storage_full` |
| | `-EINVAL` | 422 `validation_failed` |
| | `-EAGAIN` | 503 `service_not_ready` |
| | прочее | 500 `internal_error` |
| getUpload | `-ENOENT` | 404 `not_found` |
| writeUploadChunk | нет/плохой `offset` | 400 `invalid_query` |
| | `-ENOENT` | 404 |
| | `-EINVAL` | 409 `invalid_state` |
| | `-EBUSY` | 409 `busy` |
| | `-ERANGE` | 409 `offset_mismatch` («The next acceptable offset is N») |
| | `-ENODATA`, `-EOVERFLOW` | 422 `validation_failed` |
| | `-E2BIG` | 413 |
| | commit `-ENOSPC` / иное (задача) | задача `failed`: `storage_full` (не повторяемо) / `internal_error` (повторяемо) |
| verifyUpload | `-ENOENT` / `-EBUSY` / иное | 404 / 409 `busy` / 409 `invalid_state` |
| | verify `-EIO` (задача) | `failed` `internal_error` |
| deleteUpload | нет загрузки / занята | 404 / 409 `busy` |
| | delete `-EBUSY` / иное (задача) | `failed` `busy` / `internal_error` |
| любая загрузка | job-manager полон | 429 `rate_limited`, `Retry-After: 1` |
| без `web_api_v1_set_firmware` | — | 503 `service_not_ready` |

## Тесты и мутации

- `tests/web_api` (`cedar.web_api`): 167 случаев (было 164 до P6-привязок загрузки; `src/v1_firmware.c` —
  загрузка, повторы, отказы, проверка, отмена, удаление, установка: порядок отказов, полный прогон
  на рабочем потоке до `succeeded` и `last_update`, отмена до `begin` и 409 после, `interrupted` после
  повторного init; хранилище — LittleFS на sim-flash 1 МиБ, updater — `tests/fakes/fake_update_platform`
  с образом через firmware-store).
- `tests/web_api_http`: + кусок 16 КиБ фрагментами через настоящий сервер, 415 без Content-Type, 413
  больше 16 384 байт без сохранения, локальный адрес запроса (после исправления web-api координатором).
- Весь sim-уровень: 849 из 849 в 19 конфигурациях. Контракт: 5/5 проверок, 378 + 14 pytest, 35 из 35.
- Мутации (копия в контейнере `forkf-mut`, по одной, судит `cedar.web_api`), раунд 1 — 44 мутанта
  `firmware.c`, `coprocessor.c`, `system.c`: 40 убиты, 4 выжили:
  - F01 (`n >= 64` в формате sha256) и F02 (`a–z` вместо `a–f`) — не было случая из 65 цифр и с буквой
    после `f`: добавлен `test_create_upload_sha256_is_64_lowercase_hex_digits`;
  - C05 (версия без цифр даёт протокол) — добавлен случай с версией `"unknown"`;
  - C03 (`protocol_known` без `known &&`) читал неинициализированный буфер версии: буфер теперь
    инициализирован пустой строкой, мутант эквивалентен.
- Раунд 2 (копия после добавления startCoprocessorUpdate и правок): F01, F02, C05 и 20 мутантов
  установки/статуса U01–U20 (порядок отказов, повтор по ключу, хук Ethernet, отменяемость задачи,
  постановка на рабочий поток, `updating`/`uart_flashing` при активной установке, поля `last_update`)
  — **23 убиты**; C03 выжил, как ожидалось (эквивалентен после инициализации буфера).
- Итого: 68 мутантов, 67 убиты, 1 эквивалентный.
- Прошивка: FLASH 1 413 336 Б (33,72 %), RAM 86 728 Б, PSRAM 2 546 112 Б, `zephyr.signed.bin`
  `0190b350…44eb` (сборка в scratchpad форка F).
