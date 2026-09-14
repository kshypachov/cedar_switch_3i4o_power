# P5 — мутации: coprocessor-manager и захваты network-manager

Цели: `modules/coprocessor-manager/lib/coprocessor_manager.c` (сюита
`cedar.coprocessor_manager`) и пути claim/release P5 в
`modules/network-manager/lib/network_manager.c` (сюита `cedar.network_manager`).
Мутировалась только копия в контейнере; worktree не менялся, кроме добавленных тестов.

## Как запускалось

colima монтирует только `/Volumes/Programming`, поэтому копия из scratchpad
заносилась в контейнер через `docker cp`:

1. `rsync -a --exclude src/web/frontend/node_modules --exclude .git cedar_p5/ <scratchpad>/mut-m2/cedar_p5/`
2. `docker run -d --name cedar-mut-m2 -v /Volumes/Programming/Zephyr/zephyr_latest:/ws -e ZEPHYR_BASE=/ws/zephyr cedar-sim-tests:1 sleep infinity`
3. `docker cp <scratchpad>/mut-m2/cedar_p5 cedar-mut-m2:/mut/cedar_p5`
4. Сборка **без twister**, по одной на сюиту, дальше только инкрементально:
   `cmake -GNinja -S /mut/cedar_p5/tests/<suite> -B /mut/b-<suite> -DBOARD=native_sim/native/64`, `ninja`,
   затем прогон `zephyr/zephyr.exe`. Убит — если в выводе нет
   `PROJECT EXECUTION SUCCESSFUL` или код возврата не 0.

Копия вне `/ws` модули нашла: тестовые `CMakeLists.txt` регистрируют их через
`ZEPHYR_EXTRA_MODULES` по относительному пути, а workspace находится через
`ZEPHYR_BASE=/ws/zephyr` (`/ws/.west`). Symlink `/ws2` не понадобился. Базовая сборка
без мутаций: 37/37 и 106/106. Один мутант с пересборкой — ~10–20 с.
Скрипт — `<scratchpad>/mut-m2/run_mutants.py`: точная подстановка строки; мутант,
чей образец найден не ровно один раз, не запускается. После прогона исходник
восстанавливается.

## Итог

| Файл | мутантов | убито первым прогоном | убито новыми тестами | эквивалентны / недостижимы | гонка, в sim не убиваема |
|---|---:|---:|---:|---:|---:|
| `coprocessor_manager.c` | 56 | 46 | 5 (C14, C15, C17, C19, C48) | 2 (C05, C11) | 2 (C29, C46) |
| `network_manager.c` (захваты) | 18 | 13 | 3 (N03, N07, N11) | 2 (N05, N06) | 0 |
| **итого** | **74** | **59** | **8** | **4** | **2** |

## Выжившие первого прогона и чем закрыты

| ID | Мутация | Итог |
|---|---|---|
| C14 | в ожидании тишины не сбрасывается счётчик тихих взглядов при новом байте | убит: `test_a_byte_between_quiet_looks_starts_the_count_again` (тишина, байт, тишина, тишина = 4 шага, мутант — 3) |
| C15 | таймаут `>=` → `>` | убит: `test_a_noisy_interrupt_is_given_up_exactly_at_the_timeout` (ровно 100 мс; прежний тест допускал +1 шаг) |
| C17 | предел взглядов без `+ QUIET_SAMPLES` | убит: `test_a_stuck_clock_still_ends_the_wait_after_a_bounded_number_of_looks` — часы стоят, предел только по числу взглядов (22), и шум до 21-го чтения ещё успевает смениться двумя тихими взглядами |
| C19 | после неудачной остановки из `unavailable` платформу просят подключить «unavailable» | убит: `test_a_failed_stop_from_unavailable_asks_nobody_to_attach` (на плате такой вызов возвращал бы `-EINVAL`, но просить его нельзя) |
| C48 | `release()` пропускает `what == COPROCESSOR_CLAIM_COUNT` | убит: `test_an_unknown_release_leaves_the_exclusion_alone`. В этой сборке `claims[2]` — это ровно `exclusive` (`nm -n zephyr.exe`: `claims` 0x…4a0, размер 16, `exclusive` 0x…4b0): мутант уменьшал биты исключения, и мост переставал исключать apply. Тест проверяет поведение, но убивает мутанта только при таком расположении памяти |
| N03 | `release_locked()` не сбрасывает флаг захвата | убит: `test_a_claim_is_given_back_once` — после commit и scan повторный `network_manager_init()` отдавал захваты второй раз |
| N07 | apply: отказ `job_create` по нехватке задач не возвращает захват | убит: `test_a_refusal_for_lack_of_jobs_gives_the_claim_back` (16 активных задач, 429, `held == 0`) |
| N11 | то же для scan | убит тем же тестом |
| C05 | `settle_bits_locked()` не поднимает бит текущего режима | **эквивалентен.** Режим становится `usb_bridge`/`flashing` только через `set_mode(target)`, который поднял бит намерения до переключения; режим, оставшийся прежним, сохраняет бит, поднятый при входе в него; `init` ставит `console` (бита нет). Бит текущего режима к моменту settle всегда уже поднят |
| C11 | `attach_locked(UNAVAILABLE)` возвращает 0 вместо `-EIO` | **эквивалентен.** Вызывается только как возврат старого владельца, когда старый режим `unavailable`; обе ветки оставляют `st.uart_mode == UNAVAILABLE` и функция возвращает `-EIO` |
| N05 | apply: `JOB_CREATE_EXISTING` не возвращает захват | **недостижим.** `answered_by_key()` делает тот же `job_find_by_key()` под тем же mutex network-manager прямо перед захватом; между ними `expire_compacts()` может только убрать запись. Ключ `NULL` не бывает EXISTING, пустой ключ — `JOB_CREATE_INVALID` (ветка `default`, её закрывает N07) |
| N06 | apply: `JOB_CREATE_CONFLICT` не возвращает захват | **недостижим** по той же причине |
| C29 | `set_mode`: сначала проверка захватов, потом бит намерения | **не эквивалентен, в sim не убиваем.** Разница — только если `claim()` другого потока выполнится между проверкой и `atomic_or`. Между ними нет вызова ядра, а native_sim не вытесняет поток посреди вычислений (время идёт только в простое), так что окно не открыть. Порядок «сначала свой флаг, потом чужой» обоснован в заголовке файла; проверяем чтением кода |
| C46 | `claim`: сначала чтение `exclusive`, потом инкремент счётчика | то же, с другой стороны |

## Добавленные тесты (только тесты, исходники модулей не менялись)

- `tests/coprocessor_manager/src/main.c`: +5 — зависшие часы и предел взглядов; байт между
  тихими взглядами; отказ ровно на таймауте; неудачная остановка из `unavailable`;
  неизвестный release. Сюита 37 → 42.
- `tests/network_manager/src/main.c`: +2 — захват возвращается один раз; отказ 429 по
  нехватке задач возвращает захваты apply и scan. Сюита 106 → 108.

Настоящих ошибок в производственном коде мутации не нашли.

## Проверка в worktree

`tests/ci/run-sim-tests.sh -s cedar.coprocessor_manager -s cedar.network_manager -s cedar.network_service`
— 3 из 3 конфигураций, **163 из 163** тестов (coprocessor_manager 42, network_manager 108,
network_service 13), без предупреждений. Контейнер `cedar-mut-m2` удалён.
