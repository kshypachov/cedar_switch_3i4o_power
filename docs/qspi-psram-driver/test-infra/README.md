# Тестовая обвязка

Файлы восстановлены 11.09.2026 из транскриптов сессий `4f134e91` и `ca0c50e7`:
к пустым файлам заново применены все записи (Write) и правки (Edit) в том
порядке, в каком они шли в сессии. В оригинале всё лежало в scratchpad сессий,
который к этому моменту уже был удалён. Правки, сделанные через shell (`cp`,
`sed`, `perl`), при таком повторе не воспроизводятся — где это важно, указано.

Как собирать, прошивать и снимать логи — [../05-hardware-testing.md](../05-hardware-testing.md).

| Файл | Сессия | Статус | Назначение |
|---|---|---|---|
| `psram_stress/` (`CMakeLists.txt`, `prj.conf`, `src/main.c`) | ca0c50e7, 01.09 | итоговая версия | 100 циклов × 2 паттерна на весь объём через `memc_get_mem_base()`, проверка на устройстве, `TEST PASS/FAIL`. Нужен alias `psram0` и `CONFIG_MEMC=y`, `CONFIG_MSPI_MEMMAP=y` |
| `analyze_stress.py` | ca0c50e7, 02.09 | итоговая | `analyze_stress.py <log> <auto\|generic\|named>` — разбор последней загрузки: ожидаемые/запрещённые строки init по режиму, последовательность циклов 1..100, отсутствие ошибок, точный вердикт. **Ожидания зашиты под IS66WVS4M8BLL** (ID `9D 5D 40 C1 40`, 4096 KB, окно `0x90000000`) — для другого чипа поправить |
| `verify_run.py` | ca0c50e7, 01.09 | итоговая | проверка прогона `samples/drivers/memc`; ожидания под ESP-PSRAM64H (8192 KB) |
| `stress_test_two_phase.c` | 4f134e91, 12.08 | итоговая | двухфазный стресс (плотный цикл + `memcpy`), 100 проходов. Требует `-DPSRAM_ADDR`, `-DPSRAM_TOTAL` (по умолчанию `0x70000000`, 8 МБ) и `MODEL_NAME` (строка). В августе копировался в `src/main.c` приложений для внутренней флеш и XIP |
| `capture.py` | 4f134e91 | итоговая | захват UART на N секунд, переоткрывает порт при переподключении VCP. `capture.py <port> <log> <seconds>` (нужен `pyserial`) |
| `vcp.py` | 4f134e91 | итоговая | серийник USB → `/dev/cu.*` через `ioreg`. `vcp.py 002F002B3233510739363634` |
| `overlays/cedar_psram_auto_octospi1.overlay` | ca0c50e7, 01.09 | итоговая | топология B: PSRAM одна на `spi@420d1400`, `chip-variant = "AUTO"`, `st,csbound = <7>`, удалён `msi-pll-mode`. Для named/generic заменить `chip-variant` или описать параметры; `size` — под реальный чип |
| `overlays/aug_run_named.overlay`, `aug_run_manual.overlay` | 4f134e91, 17.08 | до rebase на `main` | топология A (`&octospi2`, 80 МГц). Для текущего `main` добавить `"st,psram-device"` вторым compatible; для ESP-PSRAM64H `size = <DT_SIZE_M(64)>` |
| `xip-intermediate/stage1_main.c` | 4f134e91, 11.08 | **промежуточная** | первая ступень XIP: запись образа в NOR, ICACHE remap, передача управления. Итоговая версия отличается: проверки PSRAM по 16 слов в трёх точках, нужные `#include` (`<zephyr/cache.h>`, `<zephyr/sys/reboot.h>`, `<stm32u5xx_ll_icache.h>`). Ценность — последовательности remap и прыжка |
| `xip-intermediate/xip_image_main.c` | 4f134e91, 11.08 | **промежуточная** | ранний XIP-образ (8 проходов по 1 МБ). В итоге вместо него собирался `stress_test_two_phase.c` с `CONFIG_FLASH_BASE_ADDRESS=0x02000000`, `CONFIG_FLASH=n`, `CONFIG_MEMC=n` |

Не сохранялись: `stage1/src/xip_image.h` (генерируется `xxd -i`), копия
определения платы с `st,csbound` (`boardroot/`), логи прогонов.
