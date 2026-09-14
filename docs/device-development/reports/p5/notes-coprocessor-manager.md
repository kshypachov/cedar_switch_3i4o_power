# P5 — заметки по coprocessor-manager (fork C)

Решения по ходу реализации ядра и половины взаимоисключения в network-manager.
Для сведения в README отчёта.

## Решения

| Вопрос | Решение | Почему |
|---|---|---|
| Порядок маркеров | `paused` пишется **после** остановки RX консоли (detach + тишина), `reset` с новой generation — **до** установки обработчика консоли | Байты, принятые до передачи, лягут перед `paused`; ни один байт новой generation не ляжет перед `reset`; неудачная остановка не оставляет ложного `paused` |
| Неудачная остановка RX | `-ETIMEDOUT`, старый владелец получает обработчик обратно, режим не меняется, маркеров нет | Ничего не передано — логу сообщать нечего |
| Ошибка `uart_detach()` | Считается неудачной остановкой (`-ETIMEDOUT`, `rx_stop_failures++`) | Detach с ошибкой не гарантирует снятие обработчика |
| Консоль не вернулась | После `reset` пишется `paused` «UART could not be given back to the log»; режим — прежний (мост) или `unavailable` | Лог не должен утверждать, что консоль читает |
| Ожидание тишины | Два подряд неизменных отсчёта через `RX_QUIET_STEP_MS`=5 мс, таймаут 100 мс; число взглядов ограничено ещё и по таймауту/шагу | Время переключения при мгновенной остановке — 10 мс (владелец просил минимальное время); часы, которые не идут, не превратят шумный UART в зависание |
| `note_banner()` | `k_mutex_lock(K_NO_WAIT)`: если менеджер занят переключением или сбросом, баннер игнорируется | Баннер во время сброса — его собственный; рабочий поток сборки строк не должен ждать переключения |
| Отказ по claim | Не ошибка переключения: `last_switch_error` не меняется | `last_switch_error` — для отказов оборудования (`-ETIMEDOUT`/`-EIO`) |
| Сброс при `usb_bridge` | Разрешён | Заголовок запрещает только при `flashing` и claim apply |
| Сброс, у которого `c6_reset()` вернул ошибку | Generation не растёт, маркера нет | Импульс не подтверждён |
| `transport_ready` | Необязательная функция платформы, читается в `get_status()` без блокировок | Готовность транспорта меняется независимо от переходов менеджера |
| Kconfig-зависимость от log-store | **Нет** `depends on LOG_STORE`; нужны только типы — модуль `log-store` регистрируется рядом (его include-каталог безусловный) | Иначе sim-сюита тянула бы библиотеку log-store другой ветки работ |
| Импульсы EN/BOOT | Kconfig `COPROCESSOR_MANAGER_EN_PULSE_MS`=100, `_BOOT_HOLD_MS`=200 для адаптера платы; ядро вызывает только `c6_reset(download)` | Последовательность `src/plugin_wifi/wifi.c` |
| `init()` | Сбрасывает все claims; `-EIO`, если консоль не подключилась (менеджер работает в `unavailable`) | Для sim-перезапусков; на плате init один раз до сети |

## network-manager: хуки взаимоисключения

- `enum network_exclusive { NETWORK_EXCLUSIVE_APPLY, NETWORK_EXCLUSIVE_SCAN, NETWORK_EXCLUSIVE_COUNT }`.
- В `struct network_iface_ops` (перед `ctx`): `int (*exclusive_claim)(void *ctx, enum network_exclusive what)` и `void (*exclusive_release)(void *ctx, enum network_exclusive what)`. Необязательные, только парой (иначе `network_manager_init()` → `-EINVAL`). Вызываются под мьютексом network-manager — не блокировать, не вызывать обратно.
- Apply: claim после всех проверок (id, состояние, диапазон таймаута), **до** `job_create` и журнала. Отказ → `409 busy` (`API_ERR_BUSY`, `-EBUSY`), транзакция остаётся `staged`, задачи и журнала нет, ключ идемпотентности свободен. Освобождение — в `finish()` (committed, rolled_back по запросу и по таймауту, failed при отказе адаптера, записи commit или журнала), а также на путях `job_create` EXISTING/CONFLICT/исчерпание. Повтор по ключу отвечает до claim — второго claim нет.
- Scan: claim после проверок «идёт изменение»/«идёт scan», до `job_create`. Освобождение, когда результат готов, scan отказал или его обогнал apply.
- `network_manager_init()` при повторной инициализации отдаёт удерживаемые claims старыми ops.
- `network_manager_restore_defaults()` claim **не берёт**: вызывается на старте из правила пяти стартов, до того как мост или прошивка могут существовать. Если его начнут вызывать позже — добавить claim apply.

## Для интеграции (родитель)

- Адаптер платы: `exclusive_claim` → `coprocessor_manager_claim(APPLY|WIFI_SCAN)`, `exclusive_release` → `coprocessor_manager_release()`.
- Сообщение отказа network-manager: «The Wi-Fi coprocessor is taken by its USB bridge, a firmware update or a reset; retry when it is back». Mock (`mock/network.py`, `mock/wifi.py`) такого отказа не знает — выровнять по решению родителя.
- `marker()` платформы вызывается под мьютексом менеджера на потоке запросившего: должен сам сбросить частичную строку (под своим lock ассемблера) и записать маркер в log-store, не ожидая рабочего потока.
