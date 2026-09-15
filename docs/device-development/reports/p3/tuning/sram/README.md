# Горячие данные и код сопряжения Matter — в SRAM (2026-09-15)

Запрос владельца: перенести Matter и mbedTLS из PSRAM во внутреннюю SRAM и повторить прогон
commissioning; перенести и другое горячее; только штатный механизм Zephyr (`zephyr_code_relocate`).

Гипотеза: сопряжение упирается в вычисления (PASE ~10 с, CSR ~4 с, NOC→готово ~30 с из ~55 с), а
стек потока Matter, состояние и куча mbedTLS лежат в PSRAM (OCTOSPI2), код — XIP из внешней флеши
через однопутевой ICACHE. p256-m (много стека) в потоке Matter оказался медленнее встроенного ECP
(`../fabrics/zms-prod-sectors4-p256m`: CSR 7,9 с против 3,9) — вероятно, из-за стека в PSRAM.

## Ступени (накопительно, одно дерево, `common.conf`, upstream W5500)

| Сборка | Что в SRAM | Правка |
|---|---|---|
| `sram-c0` | контроль: текущее дерево, ничего не перенесено | — |
| `sram-s1` | `.bss`/`.noinit` libCHIP.a и libtfpsacrypto.a: `sChipThreadStack`, пул `PacketBuffer`, состояние PSA/mbedTLS | убраны из `src/helpers/psram_sections.ld` (попадают в SRAM по умолчанию) |
| `sram-s2` | + код TF-PSA-Crypto: bignum, bignum_core, ecp, ecp_curves, ecdsa, sha256, aes, ccm, block_cipher, psa_crypto_{ecp,hash,mac,aead}, psa_util_internal, md, constant_time, platform_util (+ p256-m при его включении) | `zephyr_code_relocate(FILES … LOCATION RAM_TEXT NOKEEP)` в `CMakeLists.txt`, `CONFIG_CODE_DATA_RELOCATION_SRAM=y` (MPU-регион RX) |
| `sram-s3` | + арена libc malloc (Matter, mbedTLS) | `malloc.c` убран из переноса в PSRAM, `CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE=-1` (остаток SRAM после `_end`) |
| `sram-s3-p256m` | s3 + p256-m (`trials/crypto/p256m.conf`: P256M_DRIVER + `PSA_WANT_ALG_JPAKE`) | — |

`.config` s1 = c0; s2 отличается только `CODE_DATA_RELOCATION_SRAM`, s3 от s2 — только размером арены.

## Размещение (zephyr.map)

| Сборка | `_end` SRAM | Код в SRAM | Арена malloc | `__psram_used_end` | sha256 образа |
|---|---|---|---|---|---|
| c0 | 0x20013428 (79 КБ) | — | 1 МБ в PSRAM | 0x701cc048 | c4c90ef8d5328558… |
| s1 | 0x2002e2f8 (189 КБ) | — | 1 МБ в PSRAM | 0x701b1178 | c3a3a22c9a087574… |
| s2 | 0x20034958 (211 КБ) | 0x6660 (26,2 КБ) | 1 МБ в PSRAM | 0x701b1178 | 0d2870b3a8cc9195… |
| s3 | 0x20034978 | 26,2 КБ | ~570 КБ в SRAM (до 0x200c0000) | 0x700b1158 | 5d3aa145b8c2975a… |
| s3-p256m | 0x20035118 | 0x6e00 (28,2 КБ) | ~568 КБ в SRAM | 0x700b1158 | b132f91c02a0557c… |

Проверено по `nm`: в SRAM `sChipThreadStack`, `PacketBuffer::sBufferPool`, `global_data` PSA;
`mbedtls_mpi_core_montmul`, `mbedtls_ecp_mul_restartable`, `ecp_mod_p256`, `mbedtls_ecdsa_verify_restartable`,
`mbedtls_sha256_update`, `mbedtls_ccm_encrypt_and_tag` (s3-p256m: `p256_ecdsa_verify`). Во флеше остаются:

- `core/psa_crypto*.c` (`psa_hash_update` и др., слой диспетчеризации PSA) — `gen_relocate_app.py`
  сопоставляет исходник и объект по имени каталога (`core` ≠ `tfpsacrypto.dir`) и молча пропускает.
- Данные Matter, собранные в библиотеке `app` (серверные исходники из `modules/lib/matter` и
  `src/matter`), ~31 КБ в PSRAM: `Server.cpp` (`chip::Server::sServer`, 13,9 КБ), `matter_init.cpp`
  (10,1 КБ, в т.ч. стек загрузочного потока), `matter_event_loop.cpp` (4,3 КБ),
  `CodegenIntegration.cpp` (1,4 КБ). Вся `app` переносится в PSRAM одним `zephyr_code_relocate(LIBRARY app)`;
  отделить эти файлы — отдельный шаг.

## Где уходит время: криптография по шагам (c0 и s1)

Медианы по 10 commissioning, секунды, время на плате по консоли (`devsteps.py`; по логу chip-tool с
сетевыми задержками — `steps.py`). По убыванию s1:

| Шаг | Протокол / операции на плате | c0 | s1 |
|---|---|---|---|
| CASE: Sigma3 → сессия | SIGMA (CASE): расшифровка AES-CCM, проверка NOC-цепочки комиссионера (ICAC←RCAC, NOC←ICAC) и подписи Sigma3 — 3 × ECDSA-verify P-256 | 10,18 | 7,79 |
| PASE: Pake1 → Pake2 | SPAKE2+ P-256 (сторона verifier: w0, L заданы, PBKDF2 на плате нет): несколько умножений на произвольную точку, SHA-256/HMAC/HKDF | 9,83 | 7,55 |
| AddNOC: проверка цепочки | NOC←ICAC, ICAC←RCAC — 2 × ECDSA-verify | 6,79 | 5,20 |
| CASE: Sigma1 → Sigma2 | эфемерный ключ P-256, ECDH, HKDF, ECDSA-sign операционным ключом, AES-CCM | 4,35 | 3,22 |
| AddTrustedRoot | самоподпись RCAC — 1 × ECDSA-verify (+ TLV→X.509) | 3,45 | 2,65 |
| CSR | генерация ключа P-256 + ECDSA-sign (PKCS#10) | 3,01 | 2,25 |
| Attestation | ECDSA-sign ключом DAC | 1,17 | 0,77 |
| AddNOC: запись fabric, ACL | не криптография (хранилище) | 0,87 | 0,69 |
| Всё сопряжение на плате | от начала PASE до «Commissioning completed» | 53,83 | 42,51 |

- Криптография в s1 — ~29,4 с из 42,5 (69 %); остальное — ожидания обмена (MRP, ~0,3 с на шаг) и чтение атрибутов.
- Одна ECDSA-verify ≈ 2,6 с (s1): 2,65 (1 проверка) / 5,20 (2) / 7,79 (3) — линейно. Подпись 0,77 с: у подписи
  базовая точка с предвычисленной таблицей (comb), у проверки — `muladd` с произвольным открытым ключом.
  6 проверок на сопряжение = 15,6 с — самая дорогая операция; её ускоряет p256-m (ступень s3-p256m).
- s1 (данные в SRAM) ускорил все вычислительные шаги одинаково, на 23–34 %.

## Результаты

Прогон: `../fabrics/run_fabrics.py`, плата A, 10 commissioning до 5 fabrics (методика — `../README.md`),
цепочка `../queue/sram_chain.sh`, 2026-09-15 00:51–01:51 (первый запуск s1 убит хостом из-за нехватки памяти
на 4-м commissioning, данные — `../fabrics/zms-sram-s1-killed`; s1 повторён целиком). Все пять сборок: 10/10
commissioning, 5 fabrics в обоих раундах, сохранность 14/14, ошибок хранилища 0.

Медианы, секунды. «chip-tool» — commissioning целиком по хосту; шаги — время на плате (`devsteps.py`).

| | c0 | s1 | s2 | s3 | s3-p256m |
|---|---|---|---|---|---|
| Commissioning (chip-tool) | 56,2 | 44,9 | 38,2 | **18,1** | **17,1** |
| Всё сопряжение на плате | 53,83 | 42,51 | 35,78 | **15,59** | **14,66** |
| CASE Sigma3 → сессия (3 × verify) | 10,18 | 7,79 | 6,59 | 1,17 | 1,07 |
| PASE Pake1 → Pake2 (SPAKE2+) | 9,83 | 7,55 | 6,23 | 0,95 | 1,15 |
| AddNOC: проверка цепочки (2 × verify) | 6,79 | 5,20 | 4,40 | 0,61 | 0,55 |
| CASE Sigma1 → Sigma2 | 4,35 | 3,22 | 2,67 | 0,42 | 0,66 |
| AddTrustedRoot (1 × verify) | 3,45 | 2,65 | 2,23 | 0,30 | 0,27 |
| CSR (keygen + sign) | 3,01 | 2,25 | 1,88 | 0,28 | 0,53 |
| Attestation (sign) | 1,17 | 0,77 | 0,65 | 0,10 | 0,14 |
| AddNOC: запись fabric, ACL | 0,87 | 0,69 | 0,63 | 0,85 | 0,80 |
| Оценка хранилища (старт 5 + удаление 5) | 13,52 | 9,85 | 7,44 | 7,39 | 7,78 |

Выводы:
- **Главный выигрыш — арена malloc в SRAM (s3): ×2 к s2, одна ECDSA-verify 2,2 → 0,30 с.** mbedTLS
  (builtin ECP/bignum) постоянно выделяет и освобождает MPI; `sys_heap` в PSRAM (метаданные чанков в OCTOSPI2)
  был узким местом. Данные Matter/crypto (s1) дали −20 %, код crypto в SRAM (s2) — ещё −16 %.
- После s3 криптография — ~3,8 с из 15,6 с на плате (25 %); остальное — ожидания обмена (~0,7 с на шаг
  commissioning: MRP/подтверждения, чтение атрибутов) и хранилище.
- p256-m в s3 почти не отличается (14,66 против 15,59 на плате, диапазоны перекрываются): проверка подписи
  чуть быстрее, keygen/sign/ECDH медленнее. Прежнее «p256-m медленнее в потоке Matter» объяснялось стеком/кучей
  в PSRAM.
- Запас арены (~570 КБ) не измерен — пик malloc при 5 fabrics надо снять (`CONFIG_SYS_HEAP_RUNTIME_STATS`).
