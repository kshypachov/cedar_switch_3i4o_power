# Очередь прогонов (копия из scratchpad сессии)

Скрипты и очереди, которыми шли прогоны на плате. Пути в них указывают на
scratchpad сессии 2026-09-13 (`/private/tmp/claude-501/.../scratchpad/tuning`),
сборки лежали там же в `builds/<backend>-<trial>`.

- `queueN.txt` — строки `backend|trial|kind|build-dir|change|files`; порядок прогонов.
- `run_queue2.sh <queue>` — прогоняет очередь; попытка сделана, если есть её `metrics.json`
  (первый `run_queue.sh` путал `zms/002-base` и `file/002-base`).
- `zms_finish.sh` — цепочка 00:25: 034–035, сборка comboB, 036, затем File.
- `build_batch.sh <list> <N>` + `build_entry.sh` — параллельная сборка по `build_trial.sh`
  (при N=4 однажды упал `zap-cli` с SIGBUS; дальше N=3).
- `make_queue.py` — первая очередь; `progress.py` — прогресс-бар; `FOLLOWUPS.md` — план уточнений.
- `trials/` — conf/overlay всех попыток.

Очередь запускалась отвязанно от сессии: `perl -e 'use POSIX qw(setsid); setsid(); exec @ARGV' run_queue2.sh queue.txt`.
Ловушка: `pgrep -f run_trial.py` находит и командную строку того, кто её содержит (цепочку ожидания) — ждать по PID.

2026-09-14 00:57: `run_queue3.sh` — то же, что `run_queue2.sh`, плюс необязательное 7-е поле
`cycles` (для подтверждения на 10 циклах); `queue5.txt` — File 002/003-base, затем
подтверждение ZMS 037–039, затем остальной File. `zms_finish.sh` и `run_queue2.sh` сняты.

2026-09-14 09:33: `fabrics_chain.sh` — проверка на 5 fabrics. Ждёт конца `file_finish.sh`
(File 032–034) и любого `run_trial`/`run_fabrics`, собирает по `build-fabrics.list`
(zms-comboA-sectors8, file-comboA-ml96, file-comboA-ml64; conf/overlay в `trials/fabrics/`)
и прогоняет `queue-fabrics.txt` (строки `backend|name|build-dir|change|files`) через
`fabrics/run_fabrics.py`; сделано, если есть `fabrics/<backend>-<name>/metrics.json`.

2026-09-14 09:44: file/032-comboA (read-size 32 + interrupt) хуже базы → `run_queue3.sh`
остановлен до 034 (подтверждение на 10 циклах), 033 доработала как повтор. `fabrics_chain.sh`
переписан: сборка file-comboB (`trials/file3/comboB.conf`: interrupt + без сна при ожидании),
`queue7.txt` (035-comboB, 036 повтор interrupt), затем `queue8.txt` с 037 — подтверждение
на 10 циклах лучшего из двух (comboB, только если лучше interrupt больше шума 0,60 с), затем
5 fabrics; кандидаты File там — interrupt без read-size 32 (MAX_LINES 128/96/64).

2026-09-14 10:40: найдено, что образы после 09-13 23:55 собраны с изменённым другим
потоком `eth_w5500.c` (см. tuning/README.md, «Смешанные исходники»). `fabrics_chain.sh`
остановлен до 5 fabrics (037 доработала на старом образе). `rebuild_chain.sh`: после
037 собирает по `build-rebuild.list` file-base3, zms-base3, file-spi_interrupt-y3
(проверяет `w5500_rx_resync` в ELF — исходники те же, что у comboA/comboB), гонит
`queue9.txt` (File 038 база, 039 interrupt, ZMS 040 база — все на текущих исходниках),
затем `queue-fabrics.txt` (File 128 теперь на file-spi_interrupt-y3).

2026-09-14 13:10, решение владельца — 5 fabrics без прерываний SPI: `fabrics_polled_chain.sh`
собирает 6 образов одной пачкой (`build-fabrics2.list`, conf в `trials/fabrics2/`), проверяет
применение настроек, отсутствие `CONFIG_SPI_STM32_INTERRUPT=y` и одинаковый код
eth_w5500/esp_hosted во всех образах (подпись по `nm`), затем `queue-fabrics2.txt`:
ZMS кеш 2048 на 32/8/4 секторах, File MAX_LINES 128/96/64. Прежние `fabrics_chain.sh`,
`rebuild_chain.sh` и `queue-fabrics.txt` (кандидаты с interrupt) не запускались до 5 fabrics.
Python в цепочке запускается с `< /dev/null`: цикл читает очередь со stdin.

2026-09-15 09:15 — повторная полная копия scratchpad сессии `cb63d62f…` (без `builds/` 53 ГБ и `work/`):
- `trials/` — все conf и overlay попыток, включая `trials/crypto/`:
  - `p256m.conf`;
  - `pka.conf`, `pka-off.conf` — PSA-драйвер PKA и его выключение при старте;
  - `pka-ecp.conf`, `pka-ecp-off.conf` — крючок `ecp.c` и его выключение.
- `conf/` — общие conf бэкендов (`common.conf`, `zms.conf`, …).
- `logs/` — текстовые логи цепочек и прогонов. `queue1.out` — сквозной журнал всех цепочек, `fabrics-*.out` — вывод `run_fabrics.py`.
- Цепочки:
  - `sram_chain.sh` — ступени переноса в SRAM (c0, s1–s3, s3-p256m);
  - `pka_chain.sh` — этапы A/B PKA;
  - `pka_chain2.sh` — этап 2 (`SERIES=2` даёт повторную пару `pka-c2-*`);
  - `apple_after_p256m.sh`, `build_one.sh`, `phases.py`, `queue8.txt`.
- **Анализаторы шагов** хранятся не здесь: `../sram/devsteps.py` и `../sram/steps.py`. Порог Pake2 там исправлен на 0,15 с.
- **Какие образы прошивались:** это видно по `image.sha256` в каталоге прогона. Conf-фрагменты прогона лежат там же, рядом с `run.jsonl`.
