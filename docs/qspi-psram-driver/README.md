# Драйвер QPI PSRAM для Zephyr (PR #104100)

Документация, собранная 11.09.2026 из двух больших сессий Claude Code, чтобы
не потерять контекст и результаты исследований. Сами транскрипты Claude Code
удаляет через 30 дней после последнего изменения (`cleanupPeriodDays` не задан).

## Что это

Универсальный драйвер QPI PSRAM на шине MSPI для Zephyr:
`drivers/memc/memc_mspi_qspi_psram.c` + биндинг `qspi-psram`. Режимы: деталь
по имени (`chip-variant`), автоопределение по Read ID (`AUTO`), ручное
описание в devicetree (generic). Режимы шины SINGLE и QUAD, MEMC API
(`get_size`, `get_mem_base`). Проверен на STM32U585 с ESP-PSRAM64H и
IS66WVS4M8BLL, включая muxed-шину с NOR и исполнение кода из внешней флеш.

| | |
|---|---|
| PR | <https://github.com/zephyrproject-rtos/zephyr/pull/104100> — `drivers: memc: add MSPI QPI PSRAM driver` |
| Репозиторий | `/Users/kiro/CLionProjects/zephyr`, форк `github.com/kshypachov/zephyr` |
| Ветка | `driver/ospi_psram`, HEAD `869e497cf70`, 7 коммитов на upstream `main` `edc09eb9efe`, синхронизирована с `origin` |
| Состояние ревью | CHANGES_REQUESTED от swift-tk и erwango: per-device compatible, `qspi-psram-generic`, default для `mspi-io-mode` |
| Плата для тестов | `cedar_switch_3in4out_power_rev3` (этот проект) |
| Копия в продукте | драйвер применён `git apply` в `/Volumes/Programming/Zephyr/zephyr_latest/zephyr` (11.09.2026) и используется для PSRAM платы на OCTOSPI2 — при изменениях в PR синхронизировать |

## Документы

| Файл | О чём |
|---|---|
| [01-history.md](01-history.md) | хронология обеих сессий по дням, коммиты, решения автора с цитатами |
| [02-driver-design.md](02-driver-design.md) | устройство драйвера на `869e497cf70`: режимы, init, таблица, compile-out, AUTO, биндинг, долги |
| [03-research.md](03-research.md) | даташиты, рынок и Read ID, tCEM, контроллер STM32 OCTOSPI, другие контроллеры и драйверы, правила DT/Kconfig, процесс Zephyr, находки на плате, источники |
| [04-pr-review-status.md](04-pr-review-status.md) | треды ревью и их статус, разбор открытых вопросов, подготовленные тексты ответов |
| [05-hardware-testing.md](05-hardware-testing.md) | плата, окружение, команды сборки и прошивки, XIP, тестовые программы, все результаты, правила прогонов |
| [test-infra/](test-infra/README.md) | восстановленные тесты, скрипты и overlay |
| [sessions/](sessions/) | выгрузки диалогов (реплики без вывода инструментов) |

## Исходные сессии

| ID | Даты (UTC) | Содержание |
|---|---|---|
| `4f134e91-de8a-4842-91de-43f694338a46` | 11.08 – 17.08.2026 | приведение PR в порядок, rebase, отладка на плате, tCEM и `st,csbound`, XIP, MEMC API, compile-out таблицы, serial-режим, CI-сборка, rebase и compliance |
| `ca0c50e7-2140-4345-b60b-7afd84168b67` | 01.09 – 02.09.2026 | разбор ревью, AUTO-детект, исследование рынка QSPI PSRAM, стресс-тест, мягкая проверка ID, коммит AUTO |

Обе запускались из `/Users/kiro/CLionProjects/zephyr`. Пока транскрипты не
удалены, их можно продолжить:

```sh
cd /Users/kiro/CLionProjects/zephyr
claude --resume 4f134e91-de8a-4842-91de-43f694338a46   # удалится ~16.09.2026
claude --resume ca0c50e7-2140-4345-b60b-7afd84168b67   # удалится ~02.10.2026
```

В `sessions/`:
- `4f134e91-dialog.md`, `ca0c50e7-dialog.md` — все реплики пользователя и
  текстовые ответы Claude по времени (без вывода инструментов);
- `4f134e91-compact-summaries.md` — две сводки компакции контекста (12.08 и
  17.08), в них перечислены все сообщения пользователя до компакции;
- `ca0c50e7-agent-reports.md` — полные отчёты трёх исследовательских агентов
  по рынку PSRAM и отчёт `/code-review` от 01.09.

Короткие факты также лежат в memory проекта zephyr
(`~/.claude/projects/-Users-kiro-CLionProjects-zephyr/memory/`): плата и
программатор, VCP по серийнику, ID-ландшафт PSRAM, проверка всего лога,
инициализация переменных.

## Правила работы с этим драйвером

- Автор анализирует **только свой драйвер**. Чужой код (контроллер MSPI, clock,
  UART) — только как контекст, находки по нему — в отдельные PR/issue.
- Правки — ровно то, что попросили, минимальным диффом, без перестройки кода.
- «Только анализ» / «Исследование» — ничего не менять.
- Не коммитить и не пушить без явной просьбы. Коммит: автор и `Signed-off-by`
  `Kirill Shypachov <kshypachov@outlook.com>`, без упоминания Claude, текст
  сообщения — сначала на одобрение. Пушит автор сам.
- Файлы добавлять в git **по пути**, никогда `git add -A`; `.mcp.json` не
  коммитить.
- Сквош всех коммитов PR — в конце, после закрытия замечаний.
- Локальные переменные инициализируются при объявлении, даже если ревьюер
  считает это лишним.
- Правила прогонов на железе — [05-hardware-testing.md](05-hardware-testing.md#правила-проведения-тестов).
