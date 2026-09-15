# Аппаратный PKA (STM32U585) для P-256 в Matter — журнал

Задача владельца (2026-09-15): запустить блок PKA, представить его mbedTLS (TF-PSA-Crypto) как PSA-драйвер
по образцу Nordic CRACEN, провести тестовые commissioning и посчитать выигрыш. Решения в начале сессии:

- **Объём по этапам.** Этап 1 — PSA-драйвер: ECDSA sign/verify, ECDH, генерация ключа и экспорт открытого ключа P-256.
  Этап 2 — патч builtin `ecp.c` в tf-psa-crypto (`mbedtls_ecp_mul`/`muladd` через PKA). Matter SPAKE2+ вызывает их
  напрямую, мимо PSA.
- **Размещение.** Драйвер и копия dispatch лежат в модуле приложения `modules/psa-driver-stm32`. Правки Zephyr и
  tf-psa-crypto — патчами в `patches/`. Коммитов нет.
- **База сравнения.** Лучшая SRAM-сборка (`../../p3/tuning/sram/README.md`): s3 или s3-p256m, commissioning 17–18 с.

Контекст: после переноса в SRAM криптография занимает ~3,8 с из ~15,6 с сопряжения на плате. Одна ECDSA-verify
длится ~0,30 с программно. Потолок выигрыша PKA — эти ~3,8 с.

## Источники

- **HAL.** STM32CubeU5 HAL PKA (`modules/hal/stm32/stm32cube/stm32u5xx/drivers/src/stm32u5xx_hal_pka.c`): ECDSASign,
  ECDSAVerif, ECCMul, PointCheck, MontgomeryParam, ECCDoubleBaseLadder. Работа опросом, ошибка операции читается из
  PKA RAM (`PKA_NO_ERROR` 0xD60D).
- **Константы кривой.** Константы и соглашение `coefSign = 1, |a| = 3` взяты из STM32CubeU5
  `Projects/B-U585I-IOT02A/Examples/PKA/PKA_ECDSA_Sign/Src/prime256v1.c`. Проверены на хосте: p и n — стандартные,
  G лежит на кривой.
- **Nordic.** sdk-nrf 08c3fec08642 (2026-09-14):
  - `subsys/nrf_security/src/drivers/cracen/cracenpsa/src/cracen_psa_sign_verify.c`
  - `…/cracen_psa_key_management.c`
  - `…/internal/ecc/*`
  - `subsys/nrf_security/src/psa_crypto_driver_wrappers.c`

  Взята архитектура: транзакция блока на один вызов, возврат `PSA_ERROR_NOT_SUPPORTED` для отката на программную
  реализацию, зануление секретов.
- **Прототип точек входа.** `modules/crypto/tf-psa-crypto/drivers/p256-m/p256-m_driver_entrypoints.c`: те же пять
  функций, те же форматы буферов. В dispatch уже есть блоки p256-m.
- **Upstream.**
  - zephyr#116564 (open) — DTS-узел PKA только для STM32WBA, биндинг `st,stm32wba-pka`.
  - zephyr#100463 (merged) — entropy не гасит RNG clock, пока PKA включён.
  - hal_stm32#400 — PKA для WBA.
  - Для U5 PSA-драйверов ST пока нет.

## Находки

- **PKA на U5 требует тактирования RNG.** `HAL_PKA_Init` ждёт `SR.INITOK` после стирания PKA RAM (таймаут 5 с).
  Драйвер entropy гасит RNG clock между пополнениями пула, если PKA не включён (`entropy_stm32.c:147-170`).
  Поэтому драйвер включает RNG clock сам перед `HAL_PKA_Init` (3 попытки) и оставляет `CR.EN` включённым.
- **PSA sign_message.** `psa_sign_message` → `psa_sign_message_builtin` → `psa_driver_wrapper_sign_hash`. Matter
  подписывает через `psa_sign_message`, так что достаточно хука `sign_hash` (`core/psa_crypto.c:3263`).

## Журнал изменений

| Дата | Что | Где |
|---|---|---|
| 09-15 | Модуль `psa-driver-stm32`: биндинг `st,stm32-pka` (dts_root модуля), Kconfig `STM32_PKA` (select `USE_STM32_HAL_PKA`), `stm32_pka.c` (запуск, мьютекс, mul / point check / sign / verify, счётчики), шелл `pka kat/bench/stats` с векторами RFC 6979 A.2.5 и ECDH, проверенными на хосте | `modules/psa-driver-stm32/` |
| 09-15 | Модуль добавлен в `ZEPHYR_EXTRA_MODULES` | `CMakeLists.txt` |
| 09-15 | DTS-узел `pka@420c2000` (AHB2 бит 19, IRQ 97) | `boards/cedar_switch_3in4out_power_rev3_stm32u585xx_ext_flash_app.overlay` |
| 09-15 | PSA-драйвер `stm32_pka_psa.c`: `sign_hash` (k — `psa_generate_random` + отбор 0<k<n, повтор при r/s=0), `verify_hash` (пара ключей → открытый через PKA), `generate_key` (только скаляр), `export_public_key` (k·G), `key_agreement` (PointCheck точки собеседника, затем k·Q). Kconfig `PSA_CRYPTO_DRIVER_STM32_PKA`, `…_DEFAULT_ON`; шелл `pka psa on\|off` — один образ для контроля и PKA | `modules/psa-driver-stm32/{include/stm32_pka_psa.h,src/stm32_pka_psa.c}` |
| 09-15 | Копия dispatch TF-PSA-Crypto (tf-psa-crypto 000d24cb5) с блоками `CONFIG_PSA_CRYPTO_DRIVER_STM32_PKA` перед p256-m в sign_hash, verify_hash, generate_key, key_agreement, export_public_key; разница с оригиналом — `dispatch-stm32-pka.diff` в этом каталоге (справочно; копия живёт в модуле приложения, дерево tf-psa-crypto не трогается, в `patches/` не входит) | `modules/psa-driver-stm32/dispatch/` |
| 09-15 | `TF_PSA_CRYPTO_DISPATCH_DIR` → копия dispatch, если драйвер включён (символ без prompt, default до `Kconfig.zephyr`) | `Kconfig` приложения |
| 09-15 | `stm32_pka_psa_enabled` закреплён в `.data`: при `DEFAULT_ON=n` он уходил в `.bss` и сдвигал `_end` на 0x10 между сравниваемыми образами | `src/stm32_pka_psa.c` |
| 09-15 | Этап 2: `stm32_pka_p256_muladd` (PKA ECCDoubleBaseLadder, Z=1 на входе, результат аффинный — `PKA_ECC_DOUBLE_LADDER_OUT_RESULT_X/Y` в `stm32u585xx.h`, RAMReset после), счётчик `muladd`, флаг `stm32_pka_ecp_enabled` (в `.data`), Kconfig `STM32_PKA_ECP_P256` + `…_DEFAULT_ON`, шелл `pka ecp on\|off\|check [n]` (`src/stm32_pka_ecp_check.c`: случайные скаляры и точки через `mbedtls_ecp_mul`/`muladd`, PKA против программного, сравнение точек), KAT `muladd` (вектор с хоста: m·U + k·Q2 = (m·d1 + k·d2)·G) | `modules/psa-driver-stm32/` |
| 09-15 | Патч builtin ECP: в `ecp_mul_restartable_internal` (после проверок privkey/pubkey) и в `mbedtls_ecp_muladd_restartable` (после `mbedtls_ecp_check_pubkey` обеих точек) для P-256 без restart-контекста вызов PKA; при другой кривой, выключенном флаге, Z≠1, скаляре вне [1, N−1] или ошибке PKA — программный путь. Всё под `CONFIG_STM32_PKA_ECP_P256`; `#include <stm32_pka.h>` вместо ручных прототипов. Применён `git -C modules/crypto/tf-psa-crypto apply` (в дереве изменён только `ecp.c`), запись в `patches/patches.yml` | `patches/tf-psa-crypto/ecp-p256-stm32-pka.patch` (sha256 a604ff1f…) |
| 09-15 | Сборка: `stm32_pka_ecp_check.c` нужен `#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS` до всех include (`psa/crypto.h` уже тянет `mbedtls/private/ecp.h`) | `src/stm32_pka_ecp_check.c` |
| 09-15 10:30 | Сборка с чистым `prj.conf` (модуль PKA включён, PSA-драйвер и ECP-крючок нет) падала: `SHELL_COND_CMD_ARG` ссылается на обработчик и при выключенной опции (`'cmd_pka_psa' undeclared`). В ветках `#else` добавлено `#define cmd_pka_psa NULL` и `cmd_pka_ecp NULL`. Найдено при проверке дерева после подтягивания `main` 4e5b8a4 (P4+P5): обе сборки, `prj.conf` и PKA-вариант, проходят | `src/stm32_pka_shell.c` |

### Сборки этапа B

`pka-b-on` и `pka-b-off` собраны из одного дерева: `common.conf` + `trials/crypto/p256m.conf` + `pka.conf`, у off ещё
`pka-off.conf`. Проверено:
- `.config` отличается одной строкой `PSA_CRYPTO_DRIVER_STM32_PKA_DEFAULT_ON`;
- `CONFIG_TF_PSA_CRYPTO_DISPATCH_DIR` указывает на копию в модуле;
- `stm32_pka_transparent_*` слинкованы;
- `_end` одинаковый, 0x200351b8;
- таблицы символов равны, кроме абсолютного символа Kconfig;
- `zephyr.bin` отличается на 3 байта: значение флага в образе `.data` и две цифры строки `__TIME__` сборки.

Цепочка на плате A — `../../p3/tuning/queue/pka_chain.sh`.

### Этап B: commissioning, PKA выключен / включён

`../../p3/tuning/fabrics/zms-pka-b-off` (02:25–02:34) и `zms-pka-b-on` (02:34–02:42), плата A, 10 commissioning до 5 fabrics.
Оба прогона: 10/10, 5 fabrics в двух раундах, сохранность 14/14, ошибок хранилища 0. Медианы, секунды; шаги — время на плате
(`../../p3/tuning/sram/devsteps.py`).

| | s3-p256m (раньше) | pka-b-off | **pka-b-on** | разница off→on |
|---|---|---|---|---|
| Commissioning (chip-tool), медиана (мин–макс) | 17,1 | 19,2 (17,8–20,1) | **15,9 (15,0–17,3)** | **−3,3 с (−17 %)** |
| Всё сопряжение на плате | 14,66 | 16,73 | **13,37** | −3,36 с (−20 %) |
| CASE Sigma3 → сессия (3 × verify) | 1,07 | 1,09 | 0,34 | −0,75 |
| AddNOC: проверка цепочки (2 × verify) | 0,55 | 0,55 | 0,04 | −0,51 |
| CASE Sigma1 → Sigma2 (ECDH, sign) | 0,66 | 0,67 | 0,31 | −0,36 |
| CSR (keygen + sign) | 0,53 | 0,53 | 0,06 | −0,47 |
| AddTrustedRoot (1 × verify) | 0,27 | 0,27 | 0,02 | −0,25 |
| Attestation (sign) | 0,14 | 0,14 | 0,02 | −0,12 |
| PASE Pake1 → Pake2 (SPAKE2+, мимо PSA) | 1,15 | 0,97 | 0,96 | 0 |
| Оценка хранилища (5 fabrics) | 7,78 | 8,20 | 8,04 | — |

- Вычислительные шаги ×5–×25. Сумма по шагам с криптографией −2,5 с — совпадает с ожиданием (~2,6 с).
- PASE не изменился: SPAKE2+ идёт через `mbedtls_ecp_*` напрямую. Это этап 2.
- `pka-b-off` по шагам криптографии совпадает с s3-p256m. Общее время на 2 с больше за счёт ожиданий обмена
  (разброс 14,7–17,7 с); сравнивать надо off/on из одной серии.
- В Sigma3 остаётся 0,34 с на 3 проверки подписи по ~18 мс. Остальное — не криптография PKA (преобразование
  сертификатов, HKDF, AES-CCM, разбор TLV).

### Этап 2: builtin ECP на PKA — проверка

Образ `pka-c-on`, 2026-09-15 02:54, первый прогон (консоль — `pka-c-ecp-harness-cut/console.log.gz`; повтор с исправленной
обвязкой — `pka-c-ecp/`):

- **`pka kat`: PASS, 13 из 13.** В том числе `muladd m·U + k·Q2 == R` за 18,4 мс, так что результат ECCDoubleBaseLadder
  действительно аффинный.
- **`pka ecp check 20`: `mismatches=0`, PASS.** 20 раундов, в каждом свежие случайные скаляры и точки, результаты
  `mbedtls_ecp_mul`/`mbedtls_ecp_muladd` с выключенным и включённым PKA совпали.
  - `mbedtls_ecp_mul`: 187,1 мс программно → 19,8 мс на PKA (×9,5).
  - `mbedtls_ecp_muladd`: 389,3 мс → 18,7 мс (×21).
- **`pka bench 10`:** muladd 18,2 мс, mul 19,5 мс, sign 17,3 мс, verify 18,6 мс. Счётчики ошибок равны 0.
- **Сбой обвязки, не прошивки.** У `pka ecp check` в `pka_shell_run.py` не было маркера конца: сработал запасной 5 с,
  проверка шла ~12 с, её вывод не попал в `run.json`, вывод следующих команд сдвинулся. `pka_chain2.sh` не нашла
  «pka ecp check: PASS» и остановилась до commissioning. Маркер добавлен, цепочка перезапущена целиком.

### Этап 2: commissioning (серия pka-c)

`../../p3/tuning/fabrics/zms-pka-c-off` (02:57–03:06): PSA-драйвер включён, ECP-крючок выключен. По прошивке это то же,
что `pka-b-on`.
- 10/10, 5 fabrics, сохранность 14/14, ошибок хранилища 0.
- Commissioning по chip-tool: медиана 18,2 с против 15,9 у `pka-b-on`; всё сопряжение на плате — 15,67 против 13,37 с.
- **Шаги с криптографией на плате те же:** Sigma3 0,39 / 0,34, Sigma2 0,37 / 0,31, CSR 0,06 / 0,06, PASE 0,93 / 0,96.
- **Замедлились все шаги обмена по chip-tool, на 0,1–0,3 с каждый** (ReadCommissioningInfo 2,41 / 2,09,
  ConfigRegulatory 0,90 / 0,60, ArmFailSafe 0,90 / 0,77). Средняя загрузка Mac в 03:06 — 12,7 (chip-tool работает на Mac).
  По `ps`: `spotlightknowledged` (индексация Spotlight, ~92 % CPU уже 44 мин) и `duetexpertd` (~91 %). Сборок в это время не было.
- Разница — фон на хосте, а не прошивка. Выигрыш этапа 2 считается только внутри серии: `pka-c-off` против `pka-c-on`.

`zms-pka-c-on` (03:06–03:14), PSA-драйвер и ECP-крючок включены: 10/10, 5 fabrics, сохранность 14/14, ошибок хранилища 0.

| Серия pka-c, медианы, с | pka-c-off | **pka-c-on** | разница |
|---|---|---|---|
| Commissioning (chip-tool), медиана (мин–макс) | 18,25 (17,0–19,9) | **17,4 (15,6–19,6)** | −0,85 |
| Фаза PASE по консоли (`run_fabrics`) | 1,57 | **0,69** | −0,88 |
| PASE Pake1 → Pake2, chip-tool | 1,22 | **0,35** | −0,87 |
| PASE Pake1 → Pake2, на плате | 0,93 | **0,37** | −0,56 |
| Всё сопряжение на плате | 15,67 | 14,79 | −0,88 |
| Шаги с ECDSA/ECDH (Sigma3, Sigma2, AddNOC, CSR) | 0,39 / 0,37 / 0,04 / 0,06 | 0,38 / 0,38 / 0,04 / 0,06 | 0 |
| Оценка хранилища (5 fabrics) | 9,38 | 10,16 | не объяснено, см. ниже |

Оценка хранилища хуже, чем в серии pka-b (8,0–8,2). Часть этого — на плате, и фоном хоста её не объяснить:
- **Старт Matter с 5 fabrics** (время работы платы до "Matter stack initialized", хост не участвует) в обоих раундах: pka-b-on 2,72 / 2,65 с, pka-c-off 3,07 / 2,99, pka-c-on 3,30 / 3,03. Это не единичный выброс, а сдвиг всей серии.
- **Удаление fabrics** (5,3 → 6,0–6,9 с) идёт через CASE с chip-tool, поэтому здесь фон хоста может быть причиной.
- Это тот же класс зависимых от сборки таймингов, что и у SPI interrupt. Между образами b и c раскладка сдвинулась примерно на +0x2a0 (добавлен код крючка ECP).
- Причина не найдена. Для задачи PKA это не важно: шаги с ECP не пишут в хранилище.

**Повтор пары** на тех же образах (`zms-pka-c2-off` / `zms-pka-c2-on`, 03:19–03:35). Нагрузка Mac была ~6, против 12,7 в первой серии.

| Серия pka-c2, медианы, с | pka-c2-off | **pka-c2-on** | разница |
|---|---|---|---|
| Commissioning (chip-tool), медиана (мин–макс) | 18,4 (16,9–20,0) | **17,9 (16,4–20,0)** | −0,5 |
| PASE Pake1 → Pake2, chip-tool | 1,22 | **0,34** | −0,88 |
| PASE Pake1 → Pake2, на плате | 0,93 | **0,37** | −0,56 |
| Всё сопряжение на плате | 15,96 | 15,23 | −0,73 |
| Оценка хранилища (5 fabrics) | 9,36 | 9,08 | — |

- **PASE воспроизвёлся точно.** Выигрыш этапа 2 на самом шаге — 0,88 с по chip-tool (0,56 с на плате, остальное — ответ хосту).
- **Общая медиана менее надёжна.** Выигрыш −0,5…−0,85 с тонет в разбросе commissioning (±1,5 с): его дают шаги обмена и запись fabric, а не криптография.
- **Медленный старт Matter** с 5 fabrics в образах серии c тоже воспроизвёлся: 3,07 / 3,10 с при нормальной нагрузке хоста. Это свойство образа, а не фон хоста.

Выигрыш этапа 2 — PASE: ~0,9 с по хосту в обеих сериях, совпадает с ожиданием ~0,8 с. Остальные шаги не изменились.
`devsteps.py` сначала показал PASE на плате 1,50 с: Pake2 искался как первый TX позже 0,5 с после Pake1, а на PKA он
уходит через ~0,35 с. Порог снижен до 0,15 с (подтверждение уходит за 10–20 мс), прежние прогоны пересчитаны — не
изменились.

### Плата B (J-Link), тот же образ

2026-09-15 09:23–09:38, прогон `../../p3/tuning/fabrics/zms-boardb-pka-c-on`. Образ `pka-c-on` залит через bench MCUboot (mcumgr по USB CDC).
- **Результат:** 10/10, 5 fabrics, сохранность 14/14, ошибок хранилища 0.
- **Commissioning:** по chip-tool медиана 17,45 с (16,2–18,7), на плате 14,95 с. Плата A на этом же образе: 17,4 / 17,9 с.
- **Шаги с криптографией совпадают с платой A до сотых:** PASE 0,37, Sigma3 0,38, Sigma2 0,38, CSR 0,06, AddNOC verify 0,04. PKA работает одинаково на обеих платах.
- **Прежний образ платы B** без SRAM/PKA (`zms-boardb-prod-sectors4-w5500up`) давал 57,2 с.
- **Медленный старт Matter** с 5 fabrics (3,10 с) у этого образа есть и на второй плате.

**Apple Home на плате B** — сессия `../../p3/apple-home/20260915-0948`, 09:49. Обе fabric Apple записаны:
- **fabric 1 — 16,6 с** на плате. Без PKA на плате A (`sram-s3`) было 18,5 с. PASE сократился с 1,5 до 0,71 с; Attestation, CSR и проверка NOC занимают по 20–60 мс.
- **fabric 2 — 8,6 с** при fail-safe 30 с.
- **Что осталось:** паузы Apple по 3–4 с между командами. На стороне платы ускорять больше нечего.
- **Потери обмена:** на плате B больше неудачных повторных отправок к узлам Apple (13 недоставленных за 1,5 мин против 5 за 8,5 мин на плате A). Это вопрос W5500 и сети, а не PKA.

## Итог

| | Программно (p256-m + builtin, код/данные/куча в SRAM) | PKA: PSA-драйвер | PKA: PSA-драйвер + ECP (SPAKE2+) |
|---|---|---|---|
| ECDSA verify / sign / ECDH (`matter crypto_bench`) | 390 / 133 / 124 мс | 37,5\* / 17,8 / 19,5 мс | те же |
| `mbedtls_ecp_mul` / `muladd` (SPAKE2+) | 188 / 386 мс | те же (мимо PSA) | 19,8 / 17,9 мс |
| Commissioning, chip-tool, в одной серии | 19,2 с (pka-b-off) | **15,9 с (pka-b-on), −3,3 с** | ещё **−0,5…−0,85 с** (серии pka-c, pka-c2); сам PASE −0,88 с в обеих |
| Шаги с криптографией на плате | ~3,1 с | ~0,5 с | ~0,9 → ~0,3 с на PASE |

\* verify ключом-парой: вывод открытого ключа + проверка; открытым ключом — 18,5 мс.

- **Итог по chip-tool:** против лучшей программной сборки PKA экономит ~4,2 с на commissioning (сумма разниц внутри серий:
  −3,3 и −0,85). Для «тихого» хоста, как в серии pka-b: 19,2 → ~15 с. На плате криптография теперь ~0,8 с из ~13 с.
- **Что осталось:** ожидания обмена (~0,7–0,9 с на шаг commissioning, MRP/подтверждения, чтения атрибутов) и хранилище.
  Дальнейший выигрыш — не в криптографии.
- **Стоимость:** flash +9,4 КБ (PSA-драйвер, шелл, HAL PKA) / +12,5 КБ с ECP-крючком (1 302 796 → 1 315 312 Б); SRAM `_end`
  +0x340 (832 Б), из них 0x280 Б кода `ecp_pka_mul` вместе с `ecp.c` в SRAM.

## Открытые вопросы (решение владельца)

1. **Включить в прошивку.** `prj.conf` не менялся: модуль `STM32_PKA` включается по DT-узлу, но PSA-драйвер и ECP-крючок
   пока только во фрагментах `../../p3/tuning/queue/trials/crypto/` (`pka.conf`, `pka-ecp.conf`, измерено поверх `p256m.conf`). Варианты:
   - (а) `CONFIG_PSA_CRYPTO_DRIVER_STM32_PKA=y` + `CONFIG_STM32_PKA_ECP_P256=y` в `prj.conf` без p256-m (запасной путь —
     builtin; такой набор не измерялся);
   - (б) то же вместе с p256-m, как в замерах.
2. **Тактирование RNG не гасится никогда.** При постоянно включённом `PKA_CR.EN` драйвер entropy больше не выключает RNG
   clock — лишнее потребление. Альтернатива: включать PKA на время операции (+~0,3 мс на запуск).
3. **Опрос вместо прерываний.** `PKA_PollEndOfOperation` крутит цикл в потоке Matter ~18 мс на операцию. Если нужна
   отзывчивость других потоков — `HAL_PKA_*_IT` + семафор (IRQ 97).
4. **Патч общего tf-psa-crypto.** `patches/tf-psa-crypto/ecp-p256-stm32-pka.patch` применён к общему дереву модуля; без
   `CONFIG_STM32_PKA_ECP_P256` код не компилируется. `west update` его снимет — повторять через `west patch apply`.
5. **Upstream.** ST готовит PSA-драйверы в hal_stm32 (#110432, #118746). Этот драйвер — локальный, в upstream не
   предлагался.

### Ожидание перед прогонами commissioning (записано до результатов)

Одно commissioning на плате в s3/s3-p256m: 6 × ECDSA-verify (AddTrustedRoot 1, AddNOC 2, Sigma3 3), ~4 × sign (Attestation, CSR,
Sigma2, …), 1–2 × ECDH, 1–2 × keygen+экспорт.
- **Программно:** ~2,9 с.
- **С PKA:** ≈ 6×18 + 4×17 + 2×20 + 2×20 мс ≈ 0,26 с.
- **Итог этапа B:** экономия ~2,6 с из ~15,6 с на плате (~17 с по chip-tool).
- **Этап 2 (SPAKE2+):** Pake1→Pake2 0,95 с в s3, ~3 muladd → ≈0,1–0,15 с на PKA, ещё ~0,8 с.
- **Потолок всей работы с PKA:** ~13–14 с против ~17 с. Дальше время съедают ожидания обмена (~0,7 с на шаг
  commissioning: MRP, чтения атрибутов), а не вычисления. Микробенчмарки ×10–×18 на итог так не переносятся.

### Этап B: вызовы Matter через PSA попадают в PKA

Образ `pka-b-on`, 2026-09-15 02:24, прогон `pka-b-psa/run.json`. Шаги: `pka kat` (PASS), `pka stats reset`,
`matter crypto_bench 10` (API Matter `P256Keypair`/`P256PublicKey` → PSA), `pka stats`, `pka psa off`, тот же бенчмарк
программно (p256-m + builtin), `pka psa on`.

| `matter crypto_bench`, мс на операцию | PKA (psa on) | программно (psa off: p256-m) | ускорение |
|---|---|---|---|
| P-256 ECDSA verify | 37,5 | 389,8 | ×10 |
| P-256 ECDSA sign | 17,8 | 133,4 | ×7,5 |
| P-256 ECDH | 19,5 | 124,0 | ×6,4 |
| P-256 keygen | 0,7 | 125,2 | ×180 (открытый ключ не считается до экспорта) |
| SHA-256 / HMAC / HKDF / AES-CCM | без изменений (0,3–0,9) | | |

`pka stats` после бенчмарка с PKA: `sign` 10 вызовов, `verify` 10, `mul` 21, `point_check` 10, ошибок 0. Значит, все
операции P-256 из бенчмарка дошли до драйвера. После `pka psa off` счётчики не растут.

- **ECDH:** проверка точки и умножение.
- **Verify 37,5 мс = умножение + проверка.** Бенчмарк (`src/matter/crypto_bench.cpp:300`) проверяет подпись ключом-парой,
  поэтому драйвер сначала выводит открытый ключ (k·G, 19 мс), затем проверяет подпись (18 мс). В сопряжении Matter проверяет
  подписи импортированными открытыми ключами (`P256PublicKey`, `CHIPCryptoPALPSA.cpp:752`), там это одна операция, ~18 мс.
- **Keygen в бенчмарке не экспортирует открытый ключ,** поэтому с PKA он занимает только выбор случайного скаляра.

## Результаты

### Этап A: PKA сам по себе (KAT, бенчмарк)

Сборка `pka-a` = текущее дерево (s3) + модуль PKA. Прогон `pka_shell_run.py pka-a-kat` на плате A, 2026-09-15 02:08,
вывод — `pka-a-kat/run.json`. Прошивка и загрузка без ошибок, Matter поднялся, `stm32_pka: PKA ready`.

**KAT: PASS, 12 из 12.**
- d·G = U (RFC 6979);
- точка U на кривой, U' (y^1) — нет;
- подписи «sample» и «test» бит-в-бит равны r, s из RFC 6979;
- verify принимает верные подписи и отвергает с изменённым s;
- ECDH x(d1·Q2) = x(d2·U) = секрет с хоста.

Скорость, среднее по 10 (`pka bench 10`, тики 100 мкс), программная реализация из того же образа
(`matter crypto_bench 10`, builtin ECP, код/данные/куча в SRAM):

| Операция P-256 | PKA | Программно | Ускорение |
|---|---|---|---|
| ECDSA verify | 18,5 мс | 333,7 мс | ×18 |
| ECDSA sign | 17,3 мс | 83,0 мс | ×4,8 |
| ECDH (k·Q) | 19,4 мс | 175,1 мс | ×9 |
| k·G (открытый ключ) | 19,4 мс | — | — |
| проверка точки | 0,2 мс | — | — |

Запуск PKA (RAM erase + INITOK) ~0,3 мс, RNG-clock-ловушка не проявилась (одна попытка).
