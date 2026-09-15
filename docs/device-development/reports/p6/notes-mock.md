# P6 — mock подтянут к решениям P6 (форк D, 2026-09-14)

Область: `tools/api-contract/**` (кроме `checks/document.py` и `tests/test_document_checks.py` —
их правил родитель). Документ (`openapi.json`, `api-contract.md`) не менялся.

## Правила (таблица «P6 did the same …» в `tools/api-contract/README.md`)

| Правило | Где | Тест |
|---|---|---|
| Проверенный образ: `raw_full_flash`, `recovery_bundle`, `format_version=null`, `version="1"`, `cedar-c6-ota-4m-2x1792k`, `esp-hosted-mcu-3`, `signature_verified=null`, `["uart"]`; `firmware_formats=["raw_full_flash"]` | `firmware.py` `_image_json`, `state.py` | `test_verification_describes_a_whole_flash_image`, `test_capabilities_name_the_one_accepted_format` |
| `upload_max_bytes = 0x1d0000` (1 900 544); больше — `413`, ровно столько — `201` | `constants.py` | `test_the_largest_image_ends_where_the_second_slot_begins` |
| Отказы проверки: `invalid_image` (в том числе `verify_result=bare_app` — сообщение называет merged-файл `idf.py merge-bin`), `unsupported_target`, `incompatible_firmware` (таблица разделов) | `firmware.py` `_VERIFY_OUTCOMES`, `scenario.py` | `test_a_failed_verification_leaves_the_reason_on_the_upload` |
| `acknowledge_recovery=false` для `recovery_bundle` — `422 validation_failed`, задача не создаётся | `Coprocessor.start_update` | `test_an_image_that_replaces_the_bootloader_needs_acknowledge_recovery` |
| Состояние C6 (`offline`/`failed`) установку не блокирует (снят `service_not_ready`); `uart_update` и `esp32_uart` доступны ровно при `uart_mode=console`, иначе `uart_usb_bridge`/`uart_flashing`/`uart_unavailable` | `uart_update_availability`, `state.py` | `test_an_offline_coprocessor_is_installed_all_the_same`, `test_a_uart_bridged_to_usb_…`, `test_a_uart_nobody_can_own_…`, `test_a_scenario_can_be_changed_without_a_reset` |
| Порядок отказов: `ota` 503 → не Ethernet 409 → нет upload 404 → идёт установка 409 busy → не `ready` 409 invalid_state → нет подтверждения 422 → мост 409 busy → UART недоступен 503 `capability_unavailable` | `start_update` | тесты выше и `test_a_second_install_while_one_runs_is_busy`, `test_installing_an_unverified_image_is_invalid_state` |
| Установка отменяема в `queued`/`preflight`/`entering_bootloader`; с `begin` — `cancellable=false`, отмена `409 invalid_state`; отменённая освобождает файл, `last_update` не трогает | `jobs.py` `Step.cancellable`, `handlers.py`, `Coprocessor.cancelled` | `test_an_install_can_be_cancelled_until_it_begins_erasing`, `test_an_install_that_is_writing_cannot_be_cancelled`, `test_a_destructive_phase_is_never_cancellable` |
| Отменённая проверка возвращает upload в `receiving` (**DECISION**, устройству повторить) | `Firmware.verify_cancelled` | `test_a_cancelled_verification_leaves_the_file_to_verify_again` |
| Успех: `state=ready`, версия образа, `generation+1`, транспорт готов; C6 не вернулся — `failed` | `done()` | `test_a_successful_install_…`, `test_a_coprocessor_that_does_not_come_back_…` |
| Перезагрузка: файл сохраняется до последнего сброшенного куска, идущая проверка начинается заново; прерванная установка → `last_update.state=interrupted`, `error.code=boot_changed`, `recovery_required` с фазы `begin` включительно (и C6 `failed`), задача не продолжается, файл `ready` и свободен | `app.reboot`, `Firmware/Coprocessor.survive_reboot` | `test_a_reboot_keeps_the_file_…`, `test_a_reboot_during_verification_…`, `test_a_reboot_during_the_write_…`, `test_a_reboot_in_the_begin_phase_…`, `test_a_reboot_before_the_write_began_…`, `test_a_reboot_after_the_install_…`, `test_an_install_that_ended_unobserved_before_a_reboot_…` |
| Кусок: пустой — `422`, не `application/octet-stream` — `415`, больше 16 384 — `413`, следующий до конца задачи — `409 busy` (было, добавлены тесты на пустой и `text/plain`) | `write_chunk`, `app._parse_body` | `test_an_empty_chunk_writes_nothing`, `test_a_chunk_that_is_not_binary_…` |

Попутно: образец строки лога `esp-hosted-mcu-2.0` → `esp-hosted-mcu-3`; исходная версия C6 в mock `"0"`
(было `1.4.1`), образ — `"1"` (было `1.4.2`).

## Прогон

`tests/ci/run-contract-tests.sh`: 5 из 5 проверок (`info`: 28 из 35 — устройство ещё не обслуживает
7 операций P6), mock pytest **377 passed**, web-assets 14 passed.

## Мутации

Копия `tools/api-contract` (+ `openapi.json`, `modules`, `src`, `tools/web-assets` — их читают тесты)
в scratchpad, `pytest -x`, без `test_document_checks.py` (его правил родитель в это время). Первый
прогон дал «всё поймано» одной и той же ошибкой — копия была неполной (нет C-модуля
`api-validation`), результат отброшен; далее каждый прогон — только после зелёной базы по коду возврата.

33 мутанта: **31 пойман**, 2 выжили — эквивалентная пара:
- M23 — `reboot()` без `old.settle()`; M33 — `Coprocessor.survive_reboot` копирует `last_update`
  до того, как урегулировать задачу установки. Любая из двух защит одна обеспечивает, что
  установка, закончившаяся без опроса перед перезагрузкой, остаётся `succeeded`
  (`test_an_install_that_ended_unobserved_before_a_reboot_keeps_its_outcome`), поэтому каждый
  мутант по отдельности эквивалентен; снятие обеих ловится этим тестом. Порядок в
  `survive_reboot` исправлен как раз по выжившему M23 первого раунда.

Первый раунд (32 мутанта) оставил 4 выживших; закрыты тестами: M18 (`>=`→`>` на фазе `begin`), M21
(имя `idf.py merge-bin` в сообщении), M26 (`installing` не сбрасывается при перезагрузке), M23 — см. выше.

## Что должно последовать не в mock

- Фронтенд: `src/web/frontend/src/api/schema.gen.ts` перегенерировать (`format`, `firmware_formats`
  с `raw_full_flash`, описание `acknowledge_recovery`); `src/test/fixtures.ts:139` `upload_max_bytes`
  2097152 → 1900544, `:148` `firmware_formats: ['raw_app']` → `['raw_full_flash']`; при
  экране обновления — отправлять `acknowledge_recovery: true`.
- Устройство должно совпасть с DECISION mock: отмена проверки → `receiving`; UART `unavailable`
  при установке → `503 capability_unavailable`; порядок отказов; `boot_changed` в `interrupted`.
- ~~Открыто: какую строку отдаёт `CoprocessorStatus.firmware_version`~~ — решено родителем в
  `api-contract.md` («Решения P6 там, где контракт молчал») и перенесено в mock: `FirmwareImage.version`
  и `UpdateSummary.version` — `app_desc.version` образа («1»); `CoprocessorStatus.firmware_version` —
  версия, которую C6 сообщает по ESP-Hosted (knob `hosted_version`, по умолчанию `v3.0.6`), только при
  живом транспорте, `host_protocol` = `esp-hosted-mcu-<major>` этой версии; пустой, offline или
  прошиваемый C6 — `null`. Тест `test_the_status_version_is_what_the_c6_reports_not_the_image_version`
  (`v4.1.0` → `esp-hosted-mcu-4`). `FirmwareImage.host_protocol` остаётся значением профиля
  `esp-hosted-mcu-3`. Остальные четыре DECISION mock (порядок отказов, доступность по владельцу UART,
  отмена проверки → `receiving`, `interrupted`/`boot_changed`) записаны в том же разделе контракта.
