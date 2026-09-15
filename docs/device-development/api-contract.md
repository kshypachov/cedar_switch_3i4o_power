# Контракт Web API v1

Статус: проект для реализации. [OpenAPI](openapi.json) описывает HTTP surface и JSON schemas; этот документ задаёт поведение состояний, секретов и повторов. При изменении контракта обновляются оба файла. Текущие `/api/*` ещё не реализуют этот контракт.

## Общие правила

- Base URL `/api/v1`, UI и API одного origin. Транспорт — HTTP (HTTPS решением владельца не реализуется); frontend использует относительные URL. Cookie сессии выдаётся без флага `Secure`, и защита передаваемого пароля обеспечивается только доверенной локальной сетью — см. раздел 9 плана.
- JSON: UTF-8, snake_case, неизвестные поля в запросах отклоняются. Ответы могут получать новые необязательные поля в пределах v1; клиент их игнорирует. Удаление/изменение смысла поля требует v2.
- Числа размеров/секунд — JSON integers; 64-bit Matter IDs — hex-строки фиксированной длины, uptime/seq — decimal strings; нельзя терять точность в JavaScript.
- Время состояния — `uptime_ms` и `boot_id`; wall time nullable, пока часы не синхронизированы. Таймеры используют monotonic clock.
- `null` — значение неизвестно или неприменимо; отсутствие необязательного поля в mutation не должно неявно очищать секрет.
- GET не меняет состояние. `201` создаёт ресурс, `202` принимает фоновую работу, `204` завершает действие без тела. `202` не означает успешное выполнение.
- JSON request ≤ 8192 bytes; upload chunk ≤ 16384 bytes; реальные limits публикуются в capabilities. Сервер может уменьшить limits относительно проектных максимумов, клиент соблюдает объявленные значения.
- Responses API: `Cache-Control: no-store`, `X-Request-ID`. Static hashed assets могут кешироваться; index revalidates. Неизвестный API URL → JSON 404, никогда SPA redirect.
- POST/PUT/DELETE требуют admin cookie, `X-CSRF-Token` и `Idempotency-Key` (кроме login/setup и logout). Для login/setup — проверка Origin и отдельный `X-Setup-Token` для setup. Заголовки перечислены в OpenAPI.
- Idempotency key — случайная строка 16–64 ASCII chars. Scope: admin principal + method + canonical URL, включая значимые query + body hash; не только текущая cookie. Повтор того же key/body возвращает тот же resource/job, другой body → `409 idempotency_conflict`. Запись живёт минимум 15 минут и весь срок активной задачи. После перезагрузки не полагаться на RAM dedupe: сначала читать resource/job/upload state.
- Ресурсы protected, включая codes, logs, capabilities и upload. Public только auth state, login, setup и статические login assets.
- Credential verification выполняется с ограниченными ресурсами. **Проверено в P2:** у HTTP/1-сервера Zephyr нет отложенного ответа, а его единственный поток кооперативный; KDF, посчитанный прямо в нём, останавливал всю систему (>18 с при 10000 итераций). Устройство считает PBKDF2 на отдельном вытесняемом потоке, HTTP-поток ждёт его на семафоре; сеть, Matter и шелл при этом работают, но сервер другим клиентам не отвечает, поэтому число итераций — бюджет задержки: 3000, ≈1 с на вход (`reports/p2`).
- Host каждого запроса к `/api/` — IP-литерал (IPv4 или `[IPv6]`), `localhost` или имя из `CONFIG_WEB_API_EXTRA_HOSTS`, с необязательным портом, иначе `403 origin_rejected` до маршрутизации. Причина: `GET /auth/state` публикует setup token, а страница с DNS rebinding для браузера same-origin с устройством; IP-литерал в Host она отправить не может.
- Разбор JSON строгий и одинаков у устройства и mock: дубли имён, `NaN`, не-UTF-8, вложенность больше 8 и больше 64 членов объекта — `400 invalid_json`; целое — это значение (`120.0` = 120); строка с U+0000 или одиночным суррогатом — `invalid_format`. В `fields` одна запись на значение (приоритет too_long, out_of_range, invalid_format, not_allowed), все неизвестные члены, сортировка по сегментам указателя, индексы как числа.
- Заголовок, который устройство не смогло прочитать (не поместился в буфер захвата) или получило дважды, — `422 validation_failed` без `fields`.

Ошибка:

```json
{
  "error": {
    "code": "validation_failed",
    "message": "Static IPv4 requires an address and prefix",
    "request_id": "req_7b22",
    "retryable": false,
    "fields": [{"path": "/interfaces/ethernet/ipv4/address", "code": "required"}]
  }
}
```

| HTTP | Коды и смысл |
|---|---|
| 400 | `invalid_json`, `invalid_query`, `invalid_cursor` |
| 401 | `authentication_required`, `invalid_credentials`, `session_expired` |
| 403 | `csrf_failed`, `origin_rejected`, `setup_not_allowed` |
| 404 | `not_found` |
| 409 | `busy`, `stale_revision`, `invalid_state`, `offset_mismatch`, `idempotency_conflict`, `ethernet_required` |
| 410 | `resource_expired`, `boot_changed` для непереносимого RAM resource; logs используют gap metadata |
| 413 / 415 | `payload_too_large` / `unsupported_media_type` |
| 422 | `validation_failed`, `invalid_image`, `unsupported_target`, `incompatible_firmware`, `signature_invalid` |
| 429 | `rate_limited`, Retry-After seconds |
| 503 / 507 | `service_not_ready`, `capability_unavailable` / `storage_full` |
| 500 | `internal_error`; без stack trace/секретов клиенту |

После `202` ошибки hardware/SDK записываются в job с тем же `ErrorDetail`, а не задним числом в HTTP status. При временном недоступном сервисе GET system/capabilities продолжает работать.

### Ответы вне таблицы

HTTP-сервер Zephyr отвечает сам, до кода приложения, в трёх случаях; тела `Error` у этих ответов нет (измерено в P2, `reports/p2/http-server-behaviour`):

| Ответ | Когда | Что делать клиенту |
|---|---|---|
| `409 Conflict` без тела, соединение закрыто | другой клиент в этот момент досылает тело запроса к тому же пути; держится до конца его запроса или до таймаута неактивности (10 с) | повторить: ничего не выполнялось; мутацию — с тем же `Idempotency-Key` |
| `500 Internal Server Error`, `text/plain` | запрос не разбирается как HTTP/1.1 (например, тело длиннее `Content-Length`) | ошибка клиента |
| `405` | метод, которого ресурс не объявляет (HEAD) | не использовать |

Ответы `204` и `304` устройство отправляет с `Connection: close`: сервер обрамляет их как chunked и дописывает завершающий chunk, который клиент, правильно не читающий тело, иначе прочтёт перед следующим ответом.

## Сессия и устройство

| Method / path | Назначение | Результат |
|---|---|---|
| GET `/auth/state` | Требуется ли первичная настройка; пока настройка открыта — setup token | `200 AuthState` |
| POST `/auth/setup` | Назначить первый admin password при physical setup + индивидуальном setup token | `201 Session` + cookie |
| POST `/auth/session` | `{password}`; имя admin фиксировано | `200 Session` + cookie |
| GET `/auth/session` | Текущая сессия и CSRF token | `200 Session` |
| DELETE `/auth/session` | Logout | `204`, revoke cookie |
| PUT `/auth/password` | `{current_password,new_password}` | `202 Job`; при успехе все сессии отзываются, клиент логинится заново |
| GET `/system/status` | Версии, uptime, boot ID, active job IDs | `200 SystemStatus` |
| GET `/capabilities` | Возможности, лимиты, поддержанные security/methods и причины недоступности | `200 Capabilities` |
| GET `/coprocessor/status` | C6 version/state/transport/UART generation, последнее обновление | `200 CoprocessorStatus` |

Cookie `cedar_session`: HttpOnly, SameSite=Strict, Path=/, Secure для HTTPS, без Domain. Idle lifetime 30 минут, absolute lifetime 8 часов — стартовые значения. После reboot повторный login. Password не хранить в localStorage; запросы и ошибки не отражают его назад. Same-origin CSRF token выдаётся в Session. Setup endpoint закрывается после успешного сохранения admin credential; создание первого admin атомарно, при гонке победитель один (проигравший получает `409 busy`; если сохранение не удалось, настройка открывается снова с тем же токеном).

Setup token публикуется в `AuthState.setup_token`, пока `setup_allowed=true`, иначе `null` (решение владельца, раздел 13 плана). Он случаен на каждый старт и привязывает настройку к клиенту, прочитавшему состояние именно этого устройства, но **не доказывает физический доступ**: пароль назначит тот, кто первым откроет страницу в сети. Защита в момент первого включения — доверенная сеть, проверка Host и Origin.

Попытки ограничены (setup с неверным токеном, login, проверка текущего пароля): 5 бесплатных неудач с одного адреса, дальше ожидание удваивается до 300 с; и не больше 30 неудач со всех адресов за 5 минут. Ответ — `429 rate_limited` с `Retry-After`; успешный вход сбрасывает счётчик своего адреса. Проверка лимита идёт до KDF, отклонённая попытка процессор не тратит.

Смена пароля: неверный `current_password` — `401 invalid_credentials` сразу, в ответ на `PUT` (решение mock, принятое устройством); принятая смена — `202`, задача выводит и сохраняет новый verifier и завершает все сессии. Опрос задачи после этого получает `401 session_expired` — клиент читает это как успех и просит войти с новым паролем. Повреждённый сохранённый verifier блокирует вход (`503 service_not_ready`), а не открывает setup; восстановления в первой версии нет.

## Задачи

`GET /jobs/{job_id}` → `200 Job`. `POST /jobs/{job_id}/cancel` → `202 JobAccepted`, но только если `cancellable=true`. Отмена destructive phase невозможна; вернуть `409 invalid_state`. Это не принудительное отключение питания C6.

```json
{
  "id": "job_f0d2",
  "boot_id": "boot_03aa",
  "kind": "coprocessor_update",
  "state": "running",
  "phase": "writing",
  "progress": {"completed": 262144, "total": 1048576, "unit": "bytes"},
  "cancellable": false,
  "created_uptime_ms": "100000",
  "updated_uptime_ms": "103000",
  "resource_url": "/api/v1/coprocessor/status",
  "error": null
}
```

States: `queued → running → waiting_confirmation → succeeded`; возможны `failed`, `cancelled`, `interrupted`. Не все kinds имеют waiting_confirmation. Progress относится к текущей phase, может начинаться с нуля после смены phase; не изображать его как общий монотонный процент. `total=null` означает неизвестный объём.

JobAccepted: `{job_id,job_url,resource_url}` + Location job URL + Retry-After. Poll interval 500–1000 ms для active jobs, до 2–5 s в background. Job manager хранит 16 полных terminal records; до 256 компактных итогов связанных с idempotency доступны через тот же GET jobs минимум 15 минут. Active records не вытесняются. При исчерпании dedupe capacity — 429 до приёма нового действия. Firmware update summary и незавершённая network transaction имеют durable journal; после boot незавершённое обновление — interrupted, network — rollback. Остальные RAM jobs не переживают reboot.

## Matter

| Method / path | Запрос | Ответ |
|---|---|---|
| GET `/matter/status` | — | ready/state, commissioned, fabric_count |
| GET `/matter/commissioning` | — | текущее окно, mode/source, remaining_seconds, codes_available |
| POST `/matter/commissioning` | `{mode:"basic",timeout_seconds:300}` | `202 JobAccepted` |
| DELETE `/matter/commissioning` | — | `202 JobAccepted`, закрытие уже закрытого окна идемпотентно |
| GET `/matter/onboarding-codes` | — | `{available,reason,qr_payload,manual_pairing_code,setup_passcode}` |
| GET `/matter/fabrics` | — | `{items:[Fabric],count}` |

Создание окна при уже открытом: повтор idempotency key возвращает исходную job; новый запрос → `409 invalid_state`, без молчаливой замены окна другого администратора. `state=not_ready` блокирует mutations с 503. Timeout сверяется с SDK limits из capabilities (проектный диапазон 180–900 секунд, сверить в P0).

При закрытом окне codes response `available=false`, все три кодовых поля null. При enhanced window, созданном внешним controller, исходный PIN может быть неизвестен: `reason="passcode_unavailable"`. Показывать factory codes вместо действующих запрещено. `manual_pairing_code` строка 11 или 21 цифра; `setup_passcode` строка 8 цифр. `qr_payload` начинается `MT:`. В ответах не генерировать случайные тестовые QR, не являющиеся кодами устройства.

Fabric schema: `id` opaque composite identifier (должен учитывать root identity, не один fabric_id), `fabric_index`, `fabric_id`/`node_id` по 16 hex digits, `vendor_id`, `label`. Никаких предположений об online/offline контроллеров. GET отдаёт консистентный snapshot; список небольшой, pagination v1 не нужна.

**Реализовано в P3** (`modules/matter-service/README.md`). Решения там, где контракт молчал:

- GET-ресурсы Matter отвечают из снимка, который обновляет поток Matter по событиям стека; HTTP-обработчик Matter не ждёт. До старта стека окно закрыто, fabrics пусты, коды — `service_not_ready`, а `MatterStatus.state` говорит, что данные ещё не прочитаны.
- `remaining_seconds` известен только для окна, открытого через веб. Окно, открытое контроллером или на самом устройстве (`source=local`, отладочная команда шелла), отдаёт `0`: SDK не раскрывает запрошенный ими timeout.
- Коды есть у любого basic-окна, кто бы его ни открыл: basic использует собственный passcode устройства. У enhanced-окна — `passcode_unavailable`.
- `DELETE /matter/commissioning` закрывает любое открытое окно, в том числе открытое контроллером: действие явное и принадлежит локальному администратору.
- 422, 503 и 409 проверяются до создания задачи, поэтому отказ не оставляет записи под `Idempotency-Key`, и исправленный повтор не получает старый отказ. Если между приёмом запроса и его исполнением окно открылось другим путём или стек отказал (идёт commissioning), задача завершается `failed` с `invalid_state`.
- Пределы окна в `capabilities` — диапазон контракта 180–900 с, сужаемый пределами SDK; на закреплённом SDK они совпадают (минимум 3 минуты, максимум 15 минут без extended advertising).
- `capabilities.features.matter.reason`, пока стек не `ready`, — его состояние: `not_ready`, `starting` или `failed`.
- Окно объявляется только через DNS-SD: BLE на плате нет.

## Сеть

`GET /network/status` — runtime link/address/DNS/route/SSID/RSSI без секретов. `GET /network/config` — подтверждённая configuration, revision, pending_transaction_id. Это разные ресурсы.

Wi-Fi discovery: `POST /network/wifi/scans` с `{}` → `202 JobAccepted`; после завершения `GET /network/wifi/scans/{job_id}` → results. Maximum 64 AP records, `truncated=true` при превышении. Сохранять BSSID: один SSID может иметь несколько AP. `ssid` — UI display, `ssid_base64` — точные 0–32 bytes, security enum и `connect_supported`, RSSI, channel. Неподдерживаемые enterprise AP видны, но не выбираются для подключения.

Создание candidate: `POST /network/transactions` → `201 NetworkTransaction`. Candidate существует в RAM 300 секунд до apply, одновременно один; секреты не отражаются в response.

```json
{
  "base_revision": 7,
  "config": {
    "preferred_interface": "ethernet",
    "dns": {"mode": "automatic", "servers": []},
    "interfaces": {
      "ethernet": {
        "enabled": true,
        "ipv4": {"mode": "dhcp", "address": null, "prefix_length": null, "gateway": null}
      },
      "wifi": {
        "enabled": true,
        "ssid_base64": "TXlXaUZp",
        "security": "wpa3_sae",
        "hidden": false,
        "credential": {"action": "replace", "value": "example-password"},
        "ipv4": {"mode": "dhcp", "address": null, "prefix_length": null, "gateway": null}
      }
    }
  }
}
```

Значение пароля в примере вымышленное. `credential.action`: `keep` (value отсутствует), `replace` (value обязателен), `clear` (value отсутствует). Смена SSID/security с `keep` отклоняется, кроме явного совпадения сохранённого профиля. Для `open` пароль очищается явно, для enabled protected network нужен пароль. API возвращает только `password_set`.

IPv4 static: address и prefix 1–30 обязательны; gateway nullable (изолированная LAN без router допустима); если задан — проверить пригодность и подсеть. DHCP требует null для static fields. DNS automatic берётся от выбранного доступного маршрута; при static-only обычно нужен manual DNS, UI сообщает это, но LAN по IP без DNS допустима. Manual DNS — 1–2 адреса IPv4/IPv6. Одновременно выключить оба интерфейса запрещено. Хотя бы один рабочий путь восстановления требуется до применения.

| Method / path | Действие |
|---|---|
| GET `/network/transactions/{transaction_id}` | state, base_revision, redacted candidate, remaining time, reconnect hints, error |
| POST `/network/transactions/{transaction_id}/apply` | `{confirmation_timeout_seconds:120}` → `202 JobAccepted` |
| POST `/network/transactions/{transaction_id}/confirm` | `{}` → `202 JobAccepted`, commit на storage worker |
| DELETE `/network/transactions/{transaction_id}` | discard candidate либо rollback applied config → `202 JobAccepted` |

Transaction states: `staged → applying → awaiting_confirmation → committed`; ветка отката `rolling_back → rolled_back`, также `failed`, `expired`. Apply держит одну job до подтверждения/отката. Confirm и rollback отвечают ссылкой на ту же job, без создания второй конфликтующей network operation; discard staged может иметь отдельную короткую job. Отменять network job следует через transaction DELETE, `jobs/cancel` для неё недоступен.

После записи pending journal применять сеть только после `202` либо после отсоединения исходного клиента. При потере ответа клиент восстанавливает transaction через `network/config.pending_transaction_id`. Commit только после health conditions; неверный revision → 409. Reboot до durable commit всегда восстанавливает последнюю committed config. Если commit уже устойчиво записан, reboot сохраняет новую сеть, даже если UI не увидел ответ.

Смена IP меняет browser origin: автоматически переносить cookie/CSRF на новый IP нельзя. UI предлагает reconnect links/имя устройства, при необходимости пользователь логинится заново и подтверждает известный transaction ID; таймер 120 секунд учитывает это. Reconnect hints не содержат пароль/token. Endpoint confirm не подтверждает доступность всех интерфейсов лишь по факту получения HTTP запроса.

**Реализовано в P4** (`modules/network-manager/README.md`, `src/services/network/README.md`). Решения там, где контракт молчал; mock повторяет каждое (`tools/api-contract/README.md`):

- Поля ошибок кандидата — указатели в тело запроса (`/config/interfaces/ethernet/ipv4/gateway`), коды и порядок — как у mock: ошибка хранит первые шесть полей, поэтому порядок решает, какие дойдут до клиента. Любая ошибка внутри `CredentialChange` или адреса DNS-сервера (оба — `oneOf`) — одно поле `conflicting` на самом значении.
- Адрес и шлюз проверяются правилом `api_ipv4_is_usable_host()`: кроме адреса подсети и широковещательного — 0/8, 127/8, 169.254/16, 224/4 и выше; шлюз не оценивается, если сам адрес уже неверен. `0.0.0.0` и `::` среди DNS-серверов — `invalid_format` по индексу.
- Таймер подтверждения взводится при apply, а не после изменения интерфейсов; `remaining_seconds` считает его и во время `applying`, с округлением вниз. Пределы `confirmation_timeout_seconds` — 60–300, как в схеме; capabilities публикует те же.
- Confirm проверяет link, адрес и маршрут каждого включённого интерфейса; пока они не готовы — `409 invalid_state`, транзакция остаётся `awaiting_confirmation` без ошибки. Commit пишет рабочий поток после `202`; повтор confirm до записи — та же задача, после принятого confirm откат — `409 invalid_state`.
- Таймаут подтверждения: транзакция `rolling_back` → `rolled_back` с ошибкой `resource_expired`, задача `failed` с тем же кодом. Откат по запросу: `rolled_back` без ошибки, задача `succeeded`. Отказ интерфейсов или записи commit: `failed` с `internal_error`, прежняя конфигурация восстановлена. Задача сети из `waiting_confirmation` снова становится `running`, пока пишет commit или откатывает.
- Повтор с тем же `Idempotency-Key`: apply, discard и scan дедуплицирует job-manager; staging (201) и confirm/rollback применённой транзакции помнит привязка — ответ тот же ресурс, другое тело — `409 idempotency_conflict`.
- Транзакция, которую откатила перезагрузка, — `410 boot_changed`; неизвестный id — 404.
- `reconnect_urls` — только `http://<статический IPv4>/` включённых интерфейсов в состояниях `applying` и `awaiting_confirmation`. Для DHCP адрес заранее неизвестен, а имени, которое устройство могло бы гарантировать, нет (mDNS устройства — только Matter), поэтому список пуст.
- Wi-Fi недоступен, когда копроцессор не прошёл инициализацию (плата B: флеш C6 пуста). Статус Wi-Fi несёт ошибку `capability_unavailable` и при выключенном Wi-Fi; кандидат с `wifi.enabled=true` — `422` на `/config/interfaces/wifi/enabled` (`not_allowed`); scan — `503 capability_unavailable`. `wifi_security_modes` в capabilities — режимы драйвера сборки (open, wpa2_psk, wpa3_sae): установленная прошивка C6 их не сообщает, а контракт требует хотя бы один.
- Scan запрещён (`409 busy`) во время `applying`, `awaiting_confirmation`, `rolling_back` и пока идёт другой scan; staged-кандидат его не блокирует. Результаты хранит только последний scan, запрос более старого — `410 resource_expired`.
- Статус интерфейса: `state` из `disabled`, `down`, `connecting`, `addressing`, `ready`, `failed` (`connected` не выдаётся: подключённый Wi-Fi без адреса — `addressing`); SSID для показа — UTF-8 с заменой каждой недопустимой последовательности на U+FFFD, как делает mock.
- Внутреннее ограничение, не контрактное: при хотя бы одном включённом интерфейсе staging требует link хотя бы на одном из них (путь восстановления, раздел 5 плана); mock этого не воспроизводит, у него link всегда есть.

## Логи

| Method / path | Query / ответ |
|---|---|
| GET `/logs/sources` | source capabilities, available, generation, dropped counters |
| GET `/logs/records` | `source=all|stm32|esp32`, cursor optional, `min_level`, `module`, `contains`, `limit=1..100` |
| GET `/logs/export` | те же filters, `format=ndjson|text`, `max_records=1..2000`; attachment snapshot |

`LogPage`: `{boot_id,items,next_cursor,has_more,gap,dropped_count}`. Все источники используют глобальный seq, присваиваемый STM32 при capture, чтобы source=all имел устойчивый порядок. `level=unknown`/null допускается для boot/UART output; min_level не выбрасывает unknown records. Cursor opaque, связан с фильтрами и boot. При изменении filters клиент убирает cursor. Cursor указывает следующую позицию сканирования, даже если ни одна запись не прошла filter.

Без cursor вернуть последние `limit` подходящих записей от старых к новым. При wrap вернуть доступные новые записи и `gap=true`. При cursor прежнего boot вернуть текущий хвост, новый boot_id и gap=true. Повреждённый или несовместимый с filters cursor → 400. Медленный браузер теряет старые records с явным gap, не тормозит UART/Zephyr logging.

Export фиксирует верхнюю seq и выдаёт bounded snapshot; копирование/пины не должны блокировать producer. Если часть snapshot потеряна, добавить явный gap record. NDJSON — по одному LogRecord в строке; text export содержит warning line. Экспорт не обещает историю до старта ring или предыдущих boot. Для live страницы polling 500–1000 ms, следующий запрос только после завершения предыдущего.

**Реализовано в P5** (`modules/log-store/README.md`, `src/web/api/v1/logs.c`, `src/web/api/v1/coprocessor.c`). Решения там, где контракт молчал; mock повторяет каждое (`tools/api-contract/README.md`):

- Страница ограничена ещё и буфером ответа устройства (16 КиБ) и числом просмотренных за запрос записей (`CONFIG_LOG_STORE_SCAN_BUDGET`, 2048): `items` может быть короче `limit` и даже пустым при `has_more=true`; `next_cursor` — следующая непросмотренная запись. `has_more` точен: после `limit` записей устройство смотрит на одну дальше и возвращает её обратно. `limits.log_page_records` = 100 — верхняя граница, а не обещание.
- Cursor другой загрузки — не ошибка: ответ как без cursor (последние `limit`), новый `boot_id`, `gap=true`. Пустой, повреждённый, выданный для других фильтров или указывающий дальше головы кольца cursor — `400 invalid_cursor`. Значения query, нарушающие схему, — `400 invalid_query` и проверяются раньше cursor.
- `contains` — подстрока без учёта регистра только для A–Z; `module` — точное совпадение. `module` длиннее 64 байт (но не длиннее 64 символов) допустим и ничего не находит.
- `dropped_count` — записи выбранных источников, не дошедшие до кольца (потери ядра лога Zephyr и переполнение приёма UART). Перезапись кольца сообщает `gap`, а не счётчик.
- `source_generation`: у STM32 всегда 0 (перезапуск STM32 — смена `boot_id`), у ESP32 — поколение coprocessor-manager: растёт при сбросе C6 через EN, при баннере ROM, которого менеджер не вызывал, и при возврате UART консоли. Маркеры — записи `kind=reset|paused`, `level=null`, `module="coprocessor"`.
- Экспорт отдаётся потоком (chunked), по одному буферу ответа на кусок. Потерянный участок — запись `kind="gap"` (`level`/`module` null, `source` — кольцо, `seq` — первой записи после разрыва; если кольцо съело всё до конца снимка — верхняя граница снимка); в text — строка `# gap: …`. Text: заголовок `# cedar logs <boot_id>: bounded snapshot, nothing from before this boot; gaps are marked`, затем `<uptime_ms> <source> <level|-> <module|-> <message>`, CR/LF внутри сообщения — `\n`. Заголовки `Content-Type: application/x-ndjson` или `text/plain; charset=utf-8`, `Content-Disposition: attachment; filename="cedar-logs.ndjson"` или `cedar-logs.txt`.
- Клиент, переставший читать ответ, отключается через 5 с ожидания отправки (`CONFIG_WEB_API_HTTP_SEND_TIMEOUT_MS`): поток HTTP-сервера один и отправляет блокирующе, без таймаута зависшая вкладка держала бы всех.
- `capabilities.features.esp32_logs.available` следует владельцу UART: `true` только при `uart_mode=console`; иначе `reason` — `uart_usb_bridge`, `uart_flashing` или `uart_unavailable`. Готовность ESP-Hosted — это `CoprocessorStatus.transport_ready`, от неё логи не зависят (C6 без прошивки печатает ROM). `esp32_uart` и `CoprocessorStatus.uart_update` — `available=false`, `reason="not_implemented"` до P6.
- `CoprocessorStatus.state`: `updating` при `uart_mode=flashing`; `ready` при готовом транспорте; `starting` первые 20 с после старта; `failed`, если C6 что-то прислал по UART, но транспорта нет (плата B); `offline`, если не прислал ничего. `firmware_version` — версия от ESP-Hosted только при готовом транспорте, `host_protocol` тогда `esp-hosted-mcu`; `partition_layout_id` и `last_update` — null до P6.
- Операции переключения `uart_mode` в API нет (контракт не расширялся): мост включается командой шелла `coproc mode bridge` или открытием USB CDC платы (DTR). Пока UART у моста или у прошивальщика, сетевой apply отвечает `409 busy`; scan — только пока UART у прошивальщика.

## Upload и ESP32 update

### Формат файла

~~Первая версия принимает обычный app `.bin` ESP32-C6 из поддерживаемой сборки.~~ **Решение владельца P6 (2026-09-14): первая версия принимает только полный файл `raw_full_flash`** — результат `idf.py merge-bin` сборки CP (загрузчик на `0x0`, таблица разделов на `0x8000`, пустой otadata, приложение на `0x10000`), который записывается в C6 целиком с `0x0`. Один формат покрывает первую прошивку пустого чипа, обычное обновление и восстановление; сервисных процедур нет. Цена записана явно: запись с `0x0` стирает NVS C6 (кеш Wi-Fi драйвера и калибровку PHY) и возвращает загрузку на `ota_0`; источник истины Wi-Fi — настройки STM32, которые STM32 заново передаёт C6. Отдельный app `.bin` отклоняется `invalid_image`; `raw_app` и `cedar_package` остаются зарезервированными значениями enum.

Сервер самостоятельно определяет format, chip, version и разметку. Пользователь не вводит адрес flash. Проверка файла (`verifyUpload`, **реализовано в P6**, `modules/firmware-store`):

- загрузчик (`0x0`): заголовок `0xE9`, `chip_id` ESP32-C6, сегменты в пределах `0x8000`, контрольная сумма, дописанный SHA-256, `bootloader_desc`; остаток до `0x8000` — `0xFF`;
- таблица разделов (`0x8000`): записи и MD5-запись верны, разметка точно совпадает с поддерживаемой (`partition_layout_id`); остаток до `0x9000` — `0xFF`;
- `0x9000–0x10000` — `0xFF` (NVS стёрт, otadata пуст, phy_init не используется);
- приложение (`0x10000`): заголовок, `chip_id`, сегменты, контрольная сумма, дописанный SHA-256, `app_desc`; конец приложения — конец файла и не дальше начала `ota_1` (`0x1d0000`, это `limits.upload_max_bytes`).

Чужой `chip_id` в загрузчике или приложении — `unsupported_target`; разметка не из поддерживаемых — `incompatible_firmware`; остальное — `invalid_image`. `FirmwareImage`: `format="raw_full_flash"`, `kind="recovery_bundle"`, `version` — из `app_desc`, `partition_layout_id` — имя совпавшей разметки, `host_protocol` — значение профиля устройства (**с образом не сверяется**: сборки Wi-Fi CP и OT CP по заголовкам неразличимы, профиль совместимости отложен владельцем), `signature_verified=null`, `allowed_methods=["uart"]`. UART update пишет только по проверенному layout; OTA в первой версии недоступна (см. ниже).

Дополнительное расширение: `.cedarfw`, содержащий приложение C6 или recovery bundle. Это не требование первой версии; поддержку объявляет `firmware_formats` в capabilities. Предлагаемый формат для будущего release tooling:

Предлагаемый framing: 8 ASCII bytes `CEDARFW1`, затем `uint32_le metadata_length` (≤8192), затем UTF-8 JSON metadata, затем binary payload. Metadata содержит `manifest_base64`, `key_id`, `signature_base64`. Подпись ECDSA P-256/SHA-256 в DER над точными декодированными bytes manifest (без JSON reserialization). Manifest — UTF-8 JSON: `format_version=1`, `target`, `board_ids`, `version`, `host_protocol`, `partition_layout_id`, `kind`, `components[]`. Component: `role`, `payload_offset`, `size_bytes`, `sha256`. Offsets внутри payload, не адреса flash. Проверить пересечения, границы, дубликаты roles, trailing bytes и allowlist layout. Лимиты metadata и общей длины учитывать до allocation. Signing key provisioning решается при включении расширения bundle после базового P6.

Это проект расширенного формата, подлежащий фиксации вместе с release tooling; API работает с opaque file bytes. Для raw `.bin` `signature_verified=null`, для принятого подписанного bundle — true. Наличие digest не равно проверке подписи. URL downloader в v1 отсутствует.

### HTTP flow

1. `POST /firmware/uploads`: `{filename,size_bytes,sha256}` → `201 Upload`, `Location`. SHA-256 здесь для всего файла, frontend считает локально; сервер независимо пересчитывает. Одна активная upload. Образ хранится файлом в LittleFS: имя на файловой системе формирует сервер, client filename сохраняется только как отображаемое значение и никогда не участвует в пути. Свободное место проверяется до приёма первого байта, и квота удерживается на всё время загрузки; `storage_full` возвращается заранее, а не по факту отказа записи.
2. `PUT /firmware/uploads/{upload_id}/data?offset=N`, Content-Type `application/octet-stream`, chunk до capabilities limit → `202 JobAccepted`. Worker дописывает chunk в файл и выполняет `fsync`; только после этого `received_bytes` увеличивается, иначе значение нельзя предъявлять как устойчивый offset после сбоя. До завершения job следующий chunk не принимается.
3. `GET /firmware/uploads/{upload_id}` возвращает authoritative offset/state. `received_bytes` — следующий допустимый offset, а не принятые в RAM bytes. После обрыва/повтора сначала poll, затем повторить только недостающий chunk. Повтор key возвращает ту же job; новый key со старым offset → 409, ничего не перезаписывать молча.
4. После всех bytes `POST /firmware/uploads/{upload_id}/verify` с `{}` → `202`. Проверить full digest, image/target/layout/protocol/size; для поддерживаемого bundle также manifest/signature. `state=ready` только после всех применимых проверок.
5. `POST /coprocessor/updates` с `{upload_id,method:"ota"|"uart",acknowledge_recovery:false}` → `202`. Для recovery_bundle нужен true; доступность метода приходит от сервера. **P6:** `raw_full_flash` — это `recovery_bundle`, поэтому без `acknowledge_recovery=true` — `422 validation_failed`. Состояние C6 (`offline`, `failed`) установку не блокирует: это путь первой прошивки пустого чипа и восстановления. Отказы до задачи: upload не `ready` — `409 invalid_state`; UART у USB-моста или установка уже идёт — `409 busy`; запрос не через Ethernet — `409 ethernet_required`. **В первой версии поддержан только `method="uart"`**: `method="ota"` отклоняется `503 capability_unavailable`, а `capabilities.features.esp32_ota` и `coprocessor.ota` возвращают `available=false` с `reason="not_implemented"`. Значение `ota` сохранено в enum, чтобы добавление метода позже не требовало v2. Клиент выбирает метод по capabilities и не показывает недоступный как отключённый переключатель.
6. Poll job и coprocessor/status. `succeeded` только после reset/reconnect, подтверждения версии/совместимости и требуемого rollback confirm. Если C6 не ожил, ошибка с recovery_required, а не 100% success.
7. `DELETE /firmware/uploads/{upload_id}` → `202` cleanup job. Пока install использует файл, 409. Не выполнять автоудаление готового пакета до фиксации update outcome.

**Решения P6 там, где контракт молчал** (устройство и mock одинаково, `tools/api-contract/README.md`):

- Две разные версии. `FirmwareImage.version` и `UpdateSummary.version` — `app_desc.version` образа (у текущей сборки CP `"1"`). `CoprocessorStatus.firmware_version` — версия прошивки, которую сообщает сам C6 по ESP-Hosted (`"v3.0.6"`), и только при живом транспорте; `host_protocol` — `"esp-hosted-mcu-<major>"` той же версии. Health check установки требует ответа ESP-Hosted с версией, а не совпадения этих двух строк.
- Порядок отказов `startCoprocessorUpdate`: `ota` → 503; не Ethernet → 409 `ethernet_required`; неизвестный upload → 404; установка уже идёт → 409 `busy`; upload не `ready` → 409 `invalid_state`; нет `acknowledge_recovery` → 422; UART у USB-моста → 409 `busy`; UART `unavailable` → 503 `capability_unavailable`.
- `capabilities.features.esp32_uart` и `CoprocessorStatus.uart_update` доступны, пока UART у консоли; иначе `reason` — `uart_usb_bridge`, `uart_flashing` или `uart_unavailable`. Состояние C6 доступность не ограничивает.
- Отмена проверки (`cancelJob` на `firmware_verify`) возвращает upload в `receiving`; установка отменяема только до `begin`.
- Установка, прерванная перезапуском STM32, — `last_update.state="interrupted"`, `error.code="boot_changed"`, `recovery_required=true`, если запись дошла до `begin`; файл остаётся `ready`.

Upload states: `receiving`, `verifying`, `ready`, `failed`; новый файл требует новый upload ID. Файл и метаданные переживают перезагрузку вместе с LittleFS; после старта offset восстанавливается по фактическому размеру файла, сверенному с метаданными, а незавершённые файлы прошлых попыток удаляются по метаданным, а не по факту наличия в каталоге. Upload expire через 24 ч uptime без активности, active install блокирует expire; после reboot начинается новый период. Без достоверных часов не обещать wall-clock TTL. `storage_full` обнаруживать до разрушительных действий.

Install phases: `preflight`, `entering_bootloader` (UART), `begin`, `writing`, `verifying`, `activating`, `reconnecting`, `health_check`, `confirming`, `complete`; recovery outcome в job error. В первой версии фазы `activating` и `confirming` в UART-пути не используются: подтверждением служит успешный `health_check` после normal boot. В v1 требуется рабочий Ethernet и HTTP install request, пришедший через Ethernet; только link-up недостаточно. `ethernet_required` возвращается до reset/erase. Нельзя использовать Wi-Fi как источник последующих chunks уже начатой UART прошивки.

Network apply/scan и firmware install конфликтуют; log GET и status GET — нет. При timeout в протоколе записи повтор произвольного блока не всегда безопасен: политику повтора backend определяет по протоколу, при неопределённости выполняется abort и recovery, а не слепой resend. После перезапуска STM32 не продолжать destructive install автоматически, сначала reconciliation и явный retry.

## Совместимость и проверки контракта

- Source of truth форматов — OpenAPI; бизнес-инварианты, такие как static subnet, active window, UART ownership и подпись пакета, валидируются сервисом, не только JSON schema.
- Из OpenAPI получать client types и fixtures. Строгое C-генерирование сервера не требуется; route handlers используют общие parser/error helpers.
- Contract checks: internal `$ref`, required path parameters, unique operationId, valid examples, undocumented route detection (с P2: таблица маршрутов устройства `src/web/api/v1/routes.h` сверяется с документом, включая флаги) и responses при fragment/abort/concurrency (sim-сюита `tests/web_api_http` на настоящем сервере Zephyr).
- ~~Старые endpoints мигрировать или выключать по feature flag после проверки потребителей.~~ **Решение владельца 2026-09-13: весь legacy HTTP удалён в P2** — сервис на 8080 с `/upload` (запись образа STM32 без авторизации) и все `/api/*` без авторизации (reboot, relays, mqtt, device info, matter control). Их URL отвечают JSON 404. Обновление STM32 по сети недоступно до отдельного этапа; Matter управление реле сохраняется. Проверено на плате: legacy-пути — 404, порт 8080 закрыт.
- Event push не входит в v1. Добавление WebSocket позже должно иметь отдельную message schema и cursor recovery, а не менять REST response semantics.
