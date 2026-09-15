# Аппаратная криптография на ESP32: Zephyr и ESP-IDF

Дата: 2026-09-14. Источники двух видов:
- локальное дерево: `zephyr`, `modules/hal/espressif`, проект CP `SoC_and_w5500/esp32c6-hosted-cp`,
  ESP-IDF 5.5.5 в `~/esp/idf5.5.5-matter1.5`;
- GitHub через `gh` (espressif/esp-idf, espressif/mbedtls, espressif/esp-hosted-mcu).
Пометка «(агент)» — факт найден веб-обзором и локально не перепроверялся.
Сравнение с STM32: [aes-hw-acceleration.md](aes-hw-acceleration.md),
[hw-hash-driver-plan.md](hw-hash-driver-plan.md).

## Итог

| | Zephyr на ESP32 | ESP-IDF 5.5.x (наш C6) | ESP-IDF 6.x |
|---|---|---|---|
| mbedTLS | 4.x / TF-PSA-Crypto (Zephyr) | 3.6.6 | 4.1.x / TF-PSA-Crypto 1.1.x (форк Espressif) |
| Аппаратное ускорение в mbedTLS/PSA | **нет**, всё программно | **да**, через `*_ALT` | **да**, PSA-драйверы + `*_ALT` для MPI/ECC |
| Отдельный API | Zephyr crypto API: AES ECB/CBC/CTR, SHA-224/256/384/512 | — | — |

Espressif уже перешёл на PSA-драйверы в собственном SDK. В Zephyr эти драйверы лежат в
`hal_espressif`, но не подключены: Zephyr собирает только HAL AES/SHA для драйверов crypto API.

## 1. Zephyr на ESP32

- **Драйверы Zephyr crypto API** (автор Sylvio Alves, Espressif, коммиты `391ffabd666` и
  `9b3bb868559`, 2025-10-21):
  - `drivers/crypto/crypto_esp32_aes.c`: ECB/CBC/CTR, пул сессий
    `CONFIG_ESP32_CRYPTO_AES_SESSIONS_MAX=4`;
  - `drivers/crypto/crypto_esp32_sha.c`: SHA-224/256/384/512 (что поддерживает чип), multipart
    только при `SOC_SHA_SUPPORT_RESUME` (у оригинального ESP32 его нет → `-ENOTSUP`);
  - включаются по умолчанию, если в DT узлы `aes`/`sha` в `okay`. У ESP32-C6
    (`dts/riscv/espressif/esp32c6/esp32c6_common.dtsi`) оба `okay`;
  - узлов RSA/MPI, ECC, HMAC, DS в DT Zephyr нет.
- **Сборка в `hal_espressif/zephyr`:** под `CONFIG_CRYPTO_ESP32` компилируются только
  `components/esp_hal_security/aes_hal.c` и `sha_hal.c` (с hal_espressif #642 — каждый под своим
  символом). Из `components/mbedtls/port/psa_driver` подключаются только include-пути, исходники
  драйверов не собираются.
- **mbedTLS/PSA в Zephyr на ESP32** — встроенная программная реализация, как у STM32. Wi-Fi
  supplicant (`CONFIG_ESP32_WIFI_MBEDTLS_CRYPTO`) после zephyr #105869 и hal_espressif #531 работает
  через PSA API, то есть тоже программно.
- **Позиция Espressif по PSA в Zephyr:** sylvioalves в zephyr #109634 — «will test in ESP32 socs».
  PR с PSA-драйверами ESP32 для Zephyr не найдено (поиск в zephyr и hal_espressif, 2026-09-14).
- **Ошибка:** zephyr #117861 — ключ AES программировался вне `aes_lock`, параллельные сессии
  шифровали чужим ключом. Исправлено в #116964 (влит 2026-08-27, бэкпорты #117879 в v4.3 и
  #117880 в v4.4). **В нашем дереве этого исправления нет**: последний коммит
  `crypto_esp32_aes.c` — `cf648967dd7` от 2026-07-27.
- **Тесты:** `tests/crypto/crypto_aes` и `crypto_hash` запускаются на esp32, s2, s3, c3, c6, h2.

## 2. ESP-IDF 6.x: как Espressif подключил ускорители (агент)

- Переход на mbedTLS 4 / PSA — в v6.0 («PSA-first»). master, v6.0.3 и головы release/v6.x
  используют `espressif/mbedtls` ветку `mbedtls-4.1.1-idf` (TF-PSA-Crypto 1.1.1 скопирован в дерево).
- **PSA-драйверы** в `components/mbedtls/port/psa_driver/` (есть и в нашем `hal_espressif`):
  `esp_aes` (+GCM), `esp_sha` (core/parallel_engine), `esp_mac` (HMAC transparent/opaque, CMAC),
  `esp_md` (MD5 из ROM), `esp_ecdsa`, `esp_rsa_ds`, secure element.
- **Включение:** `port/include/mbedtls/esp_config.h` по Kconfig ставит `ESP_*_DRIVER_ENABLED`,
  `MBEDTLS_PSA_ACCEL_ALG_*` и снимает `MBEDTLS_PSA_BUILTIN_ALG_*`. Сам TF-PSA-Crypto подключается
  как `TF_PSA_CRYPTO_USER_CONFIG_FILE "mbedtls/esp_config.h"`
  (`hal_espressif/components/mbedtls/CMakeLists.txt:159-161`).
- **Dispatch:** форк содержит driver JSON (`esp_{aes,sha,md5,ecdsa_*}_driver.json`) и
  сгенерированный, закоммиченный `psa_crypto_driver_wrappers.h`. HMAC, CMAC, RSA-DS и secure
  element, по-видимому, дописаны в него руками. Тот же подход, что у TF-M с CC3XX.
- **Что осталось на `*_ALT`** (Espressif вернул их в форк; upstream TF-PSA-Crypto их удалил):
  - `MBEDTLS_MPI_EXP_MOD_ALT` и `MPI_MUL_MPI_ALT` (RSA/bignum);
  - `MBEDTLS_ECP_MUL_ALT` и `ECP_VERIFY_ALT` (точечное умножение ECC).
  Планов перевести их на PSA не найдено.
- **Опции Kconfig** (все `y` по умолчанию, кроме отмеченных): `MBEDTLS_HARDWARE_AES`, `_GCM`
  (только где `SOC_AES_SUPPORT_GCM`), `_SHA`, `_MPI`, `_ECC`, `_ECDSA_VERIFY`; `_ECDSA_SIGN=n`
  (ключ в eFuse); `_RSA_DS_PERIPHERAL=n`.
- **Ограничения:**
  - один lock на периферию, у AES и SHA он общий (общий DMA);
  - AES DMA от 512 Б (при `AES_HW_SMALL_DATA_LEN_OPTIM`), короче — блочный режим;
  - на время DMA держатся PM-локи (без light sleep, максимальная частота CPU);
  - esp-idf PR #19027 (open): аппаратное MPI медленнее программного на малых размерах —
    ESP32 256 бит: 9.78 µs программно против 19.49 µs аппаратно, а ECDHE P-256 делает тысячи таких
    умножений;
  - esp-idf #19065: AES-CCM объявлен ускоренным, но драйвера CCM нет; работает только с
    `MBEDTLS_GCM_SUPPORT_NON_AES_CIPHER=y` (важно для Matter/Thread);
  - #18640: на S3 I2S через GDMA портит результаты AES/SHA;
  - v6.0 добавила 27–41 КБ flash в HTTPS-примерах.
- **ESP-TEE** (C6 и др.) выставляет AES, SHA, HMAC, DS, ECC и ECDSA из защищённого хранилища
  как secure services, плюс PSA ITS и attestation.

## 3. Периферия по чипам (агент, `soc/<chip>/.../Kconfig.soc_caps.in`)

| SoC | AES | SHA | MPI (макс. бит) | HMAC | DS | ECC | ECDSA |
|---|---|---|---|---|---|---|---|
| ESP32 | 128/192/256, без DMA | 1/256/384/512 | 4096 | – | – | – | – |
| S2 | DMA, GCM | 1…512 | 4096 | + | + | – | – |
| S3 | GDMA | 1…512 | 4096 | + | + | – | – |
| C3 | GDMA, 128/256 | 1/224/256 | 3072 | + | + | – | – |
| **C6** | **GDMA, 128/256, без GCM** | **1/224/256** | **3072** | **+** | **+** | **+** | **–** |
| H2 | GDMA | 1/224/256 | 3072 | + | + | + | + |
| C5 | GDMA | 1…512 | 3072 | + | + | + | + (P-384) |
| P4 | GDMA, GCM | 1…512 | 4096 | + | + | + | + (P-384) |
| C61 | – | 1/224/256 | – | – | – | + | + |

Строка C6 проверена локально по ESP-IDF 5.5.5 `components/soc/esp32c6/include/soc/soc_caps.h`:
`SOC_AES/MPI/SHA/HMAC/DIG_SIGN/ECC/RNG_SUPPORTED`, `SOC_AES_GDMA`, AES-128/256,
`SOC_SHA_SUPPORT_SHA1/224/256`, `SOC_SHA_SUPPORT_RESUME`, `SOC_RSA_MAX_BIT_LEN 3072`,
`SOC_CRYPTO_DPA_PROTECTION_SUPPORTED`. Макроса ECDSA нет.

## 4. Наш C6-копроцессор (esp-hosted)

- **Сборка:** `SoC_and_w5500/esp32c6-hosted-cp`, ESP-IDF 5.5.5, ESP-Hosted 3.0.6, `sdkconfig` от
  2026-09-06. В ESP-IDF 5.5.5 mbedTLS **3.6.6** (`build_info.h`), то есть ускорение через `*_ALT`,
  не через PSA.
- **`sdkconfig`:** `CONFIG_MBEDTLS_HARDWARE_AES=y` (`AES_USE_INTERRUPT`), `HARDWARE_SHA=y`,
  `HARDWARE_MPI=y` (`MPI_USE_INTERRUPT`), `HARDWARE_ECC=y` (`ECC_OTHER_CURVES_SOFT_FALLBACK`),
  `ESP_WIFI_MBEDTLS_CRYPTO=y`, WPA3-SAE/OWE включены. `MBEDTLS_HARDWARE_GCM` нет (у C6 нет режима GCM).
- **Вывод:** криптография Wi-Fi supplicant (WPA2/WPA3-SAE, OWE) на C6 уже аппаратная. Шифрование
  кадров CCMP, по данным агента, делает MAC-блок Wi-Fi.
- **Разгрузки криптографии на хост нет:** в `rpc_v1.proto`/`rpc_v2.proto` esp-hosted-mcu нет
  crypto RPC (агент). Для STM32 (TLS, Matter, PBKDF2 web-auth) C6 ничего не ускоряет.
- Проблема CCM из esp-idf #19065 касается только IDF 6.x, наш C6 на 5.5.5 её не имеет.

## 4a. Matter-модуль Zephyr на ESP32

**Ответ: криптография программная.** Факты:

- **Модуль:** connectedhomeip `v1.6.0.0` в `modules/lib/matter`. Сборка — `config/zephyr`
  (`chip-module`, `chip-gn`), платформа — `src/platform/Zephyr`.
  - В upstream-манифесте Zephyr модуля нет. В нашем `zephyr/west.yml` он добавлен локально
    (`git diff`: remote `matter`, проект `connectedhomeip`).
  - Добавление в Zephyr обсуждается в RFC zephyr #104127 (asmellby, open, `In progress`, `TSC`).
    Nordic предлагает оставить его внешним модулем. teburd поднял вопрос о версиях mbedTLS: ответ
    nordicjm — «побеждает mbedTLS Zephyr».
- **Криптография модуля:**
  - `config/zephyr/chip-gn/args.gni`: `chip_crypto = "mbedtls"`, `chip_external_mbedtls = true`;
  - `config/zephyr/Kconfig`: `CHIP_CRYPTO_PSA` только включает `PSA_WANT_*`.
  - Оба бэкенда (mbedTLS PAL и PSA PAL) вызывают mbedTLS/TF-PSA-Crypto Zephyr, никакого своего
    криптокода или хуков под железо в `config/zephyr` и `src/platform/Zephyr` нет.
- **Упоминаний ESP32/Espressif** в `config/zephyr` нет. Из вендорских файлов подключается только
  `nxp/chip-module/Kconfig.defaults`.
- **На ESP32 под Zephyr** этот модуль получит встроенный программный TF-PSA-Crypto Zephyr:
  - PSA-драйверы Espressif из `hal_espressif` Zephyr не собирает;
  - драйверы `crypto_esp32_aes/sha` (Zephyr crypto API) Matter не вызывает;
  - Kconfig Wi-Fi ESP32 (`drivers/wifi/esp32/Kconfig.esp32`: `select MBEDTLS`, `PSA_CRYPTO`,
    `PSA_WANT_*`) только включает алгоритмы.
- Сборку Matter-модуля Zephyr на ESP32 на железе не проверяли. Issue и PR о Matter на ESP32 под
  Zephyr не найдено (zephyr, connectedhomeip, 2026-09-14).
- Ситуация та же, что у нашей STM32U5. Изменится, только когда Espressif подключит свои
  PSA-драйверы через механизм zephyr #110328.

Для сравнения — не Zephyr:

- **Порт ESP-IDF** (`src/platform/ESP32`) в connectedhomeip — это не модуль Zephyr:
  - `src/platform/Zephyr` (общий), `nrfconnect`, `telink` и `nxp` не упоминают esp32;
  - `src/platform/ESP32` — порт для ESP-IDF.
  - Если собрать Matter на Zephyr для ESP32 через общую платформу `Zephyr` с `CHIP_CRYPTO_PSA`,
    PSA-вызовы обработает встроенный TF-PSA-Crypto Zephyr, то есть **программно**. Espressif
    PSA-драйверы в Zephyr не собираются, а драйверы crypto API (`crypto_esp32_*`) Matter не
    вызывает.
  - Выбор Kconfig Zephyr для ESP32 (`drivers/wifi/esp32/Kconfig.esp32`: `select MBEDTLS`,
    `PSA_CRYPTO`, `PSA_WANT_*`) лишь включает алгоритмы, ускорения не даёт.
- **ESP-IDF (esp-matter)** — аппаратно: Matter поверх mbedTLS/PSA IDF с ускорителями из §2.
  Дополнительно `src/platform/ESP32/ESP32CHIPCryptoPAL.cpp` и `ESP32SecureCertDACProvider.cpp` при
  `CONFIG_USE_ESP32_ECDSA_PERIPHERAL` подписывают DAC аппаратным ECDSA ключом из eFuse через
  opaque PSA-ключ `esp_ecdsa_opaque_key_t` (`psa_crypto_driver_esp_ecdsa.h`). На C6 нет
  ECDSA-периферии, это для H2, C5, P4 и др.

## 4b. Оценка объёма: аппаратная криптография PSA на ESP32 под Zephyr

Оценка для дизайна «Zephyr на ESP32». Нашему изделию не нужна: C6 работает на IDF.
Цифры — порядок величины для одного опытного инженера с платой. Прототип не делался.

### Проверенные факты, от которых зависит оценка

1. **Dispatch Espressif совместим с ядром Zephyr.**
   - Форк `espressif/mbedtls@mbedtls-4.1.1-idf` построен на том же TF-PSA-Crypto 1.1.1, что и
     Zephyr.
   - Ядро Zephyr (`core/*.c`, `drivers/builtin/src/*.c`, `dispatch/*_no_static.c`) вызывает **67**
     функций `psa_driver_wrapper_*`. `tf-psa-crypto/core/psa_crypto_driver_wrappers.h` (3752 строки)
     + `dispatch/psa_crypto_driver_wrappers_no_static.h` форка определяют ровно те же 67, разница
     пустая.
   - В файле 99 блоков `ESP_*_DRIVER_ENABLED`: AES 20, CMAC 8, ECDSA 33+12+12+8, HMAC opaque 12,
     HMAC transparent 8, RSA-DS 11, SHA 7. CC3XX нет.
   - Контексты лежат в `include/psa/crypto_driver_contexts_{primitives,composites}.h` (Zephyr ждёт
     их в `dispatch/include/psa/`, достаточно перенести).
   - Через `TF_PSA_CRYPTO_DISPATCH_DIR` (zephyr #110328, уже в дереве) файлы берутся почти как есть.
2. **Код драйверов уже лежит в `hal_espressif`** (`components/mbedtls/port`, синхронизация с IDF
   master), но Zephyr его не собирает. Объёмы (строк):
   - PSA-слой: `esp_sha` ≈ 1.5 тыс. (core-вариант), `esp_aes` 945 (+GCM), `esp_mac` 1210
     (HMAC transparent 375), `esp_ecdsa` 1384, `esp_rsa_ds` 1421;
   - ядро периферии: `sha/core/sha.c` 393, `aes/esp_aes.c` 754, `esp_aes_gcm.c` 728,
     `aes/dma/esp_aes_dma_core.c` 1401, `crypto_shared_gdma.c` 233, `bignum` 900, `ecc` 206.
3. **Чего нет в Zephyr-порте `hal_espressif`:**
   - компонента `esp_security`: `esp_crypto_lock.c` отсутствует (есть только заголовок
     `zephyr/esp32s2/include/esp_crypto_lock.h`), `esp_crypto_periph_clk.h` отсутствует;
   - заголовков FreeRTOS: AES/SHA/GDMA используют семафоры и критические секции в 5 файлах
     (34 строки), `esp_intr_alloc` в 1, `esp_pm_lock` в 1 (8 строк);
   - `esp_key_mgr` (есть только для c5/s31), он используется в 6 файлах (34 строки). `esp_efuse`
     используется в 6 файлах — это нужно только ECDSA, HMAC opaque и DS;
   - **драйвера GDMA-каналов**: `esp_crypto_shared_gdma.c` вызывает `gdma_new_ahb_channel()`, а в
     `components/esp_driver_dma/src` есть только `gdma_priv.h`. Zephyr собирает лишь
     `gdma_hal_*.c` (`CONFIG_DMA_ESP32`). DMA в `esp_aes.c` и `sha/core/sha.c` включается на этапе
     компиляции по `SOC_AES_SUPPORT_DMA`/`SOC_SHA_SUPPORT_DMA` (у C6 = 1), Kconfig его не выключить:
     нужен патч «только блочный режим» или порт API каналов.
4. **ALT для MPI/ECC нет:** в `modules/crypto/tf-psa-crypto/drivers/builtin/src/bignum.c` и `ecp.c`
   нет `MBEDTLS_MPI_*_ALT` и `MBEDTLS_ECP_*_ALT` (grep пуст). Путь Espressif для RSA/ECC в Zephyr
   недоступен без патча форка TF-PSA-Crypto.
5. **CCM:** в `esp_config.h:1765` объявлен `MBEDTLS_PSA_ACCEL_ALG_CCM`, но драйвера CCM нет (grep
   `PSA_ALG_CCM` по `psa_driver` пуст, esp-idf #19065). При `MBEDTLS_PSA_ACCEL_KEY_TYPE_AES`
   встроенные CCM/GCM переходят на `MBEDTLS_BLOCK_CIPHER_AES_VIA_PSA`
   (`crypto_adjust_config_tweak_builtins.h:103-105`) → `psa_cipher_encrypt(ECB)` **на каждый
   16-байтный блок** (`block_cipher.c:177`) через драйвер и его lock.
6. **Контекст SHA Espressif** `esp_sha_hash_operation_t` = указатель + тип (8 Б), сам контекст
   (`esp_sha256_context` ≈ 116 Б, `esp_sha512_context` ≈ 230 Б) выделяется в куче. `psa_hash_operation_t`
   не растёт, но на каждую хеш-операцию приходится malloc/free, а `clone` делает глубокую копию.
   Регистры не сохраняются: состояние хранится программно, возобновление через `SOC_SHA_SUPPORT_RESUME`.
7. **Конфликт владельцев периферии:** Zephyr-драйверы `crypto_esp32_aes/sha` держат свои `k_mutex`,
   драйверы Espressif — `esp_crypto_lock`. При одновременном включении нужен общий lock или
   взаимоисключающий Kconfig.
8. **ABI:** `mbedtls/esp_config.h` уже включается в Zephyr-сборке Wi-Fi supplicant
   (`crypto_mbedtls.c`, `tls_mbedtls.c`, `fastpbkdf2.c`). В hal_espressif есть Zephyr-специфичные
   правки (`204c89c8ce`, `155a9d9ea0`, Siddhant Modi, 2026-05). Если `ESP_*_DRIVER_ENABLED` будут
   видны не во всех единицах трансляции, union контекстов разойдётся — тот же риск, что в плане
   STM32 §6.
9. **Ориентир по объёму интеграции:** Silicon Labs zephyr #110414 (свой dispatch поверх #110328,
   драйверы уже в hal_silabs) — +335/−4, 10 файлов.

### Путь A — собрать драйверы Espressif из hal_espressif

| Уровень | Что | Новый/изменённый код | Оценка |
|---|---|---|---|
| A1 | SHA-1/224/256 + HMAC (transparent), блочный режим без DMA | CMake/Kconfig в hal_espressif и zephyr ~100; конфиг драйвера (выжимка `esp_config.h` 187–300) ~80; dispatch (копия + перенос контекстов + скрипт синхронизации) ~50; shim `esp_crypto_lock` ~60, `esp_crypto_periph_clk` ~40, FreeRTOS-семафоры и `esp_pm_lock` через патчи синхронизации ~100; guards `esp_key_mgr`/`efuse` ~50; патч «без DMA» для SHA ~30 | ~500 строк, **2–3 недели** с отладкой и тестами |
| A2 | AES ECB/CBC/CTR + GCM (C6: AES аппаратно, GHASH программно) + CMAC | + `esp_aes*.c`; либо патч «только блочный режим» (GCM сейчас зовёт `esp_aes_process_dma_gcm`) ~100, либо порт API GDMA-каналов поверх Zephyr DMA ~400–600; прерывания `esp_intr_alloc` → Zephyr IRQ ~50 | **+2–4 недели** |
| A3 | CCM для Matter | (а) не ускорять AES-ключ: CCM/GCM остаются программными, Matter ничего не получает; (б) ускорять: CCM поблочно через PSA ECB, почти наверняка медленнее программного; (в) новый AEAD-драйвер CCM на аппаратных AES-CTR + CBC-MAC ~300–500 | (в) **+1–2 недели** |
| — | RSA/MPI, ECC/ECDH/ECDSA | в Zephyr нет ALT; писать настоящие PSA-драйверы ECDH/ECDSA поверх ECC-умножения (у C6 нет ECDSA-периферии) — месяцы; esp-idf PR #19027: аппаратное MPI на 256 бит медленнее программного | **вне оценки**; реальный выигрыш для P-256 — программный `MBEDTLS_PSA_P256M_DRIVER_ENABLED` |

Для других чипов: у оригинального ESP32 другой SHA (parallel engine, без resume), у S2/S3/P4 —
аппаратный GCM и SHA-512. Каждая серия — отдельная проверка.

### Путь B — свой тонкий PSA-драйвер поверх `sha_hal`/`aes_hal`

- Zephyr уже собирает `aes_hal.c`/`sha_hal.c` (`CONFIG_CRYPTO_ESP32`), а `crypto_esp32_sha.c`
  содержит логику resume (`sha_hw_restore`).
- Объём как в плане STM32:
  - hash + HMAC однократно и multipart ~600–900 строк, **2–3 недели**;
  - AES ECB/CBC/CTR ~400 строк, **+1–2 недели**;
  - CCM/GCM — по варианту (в) выше.
- Плюсы: нет FreeRTOS, GDMA, heap_caps и зависимости от синхронизации с IDF.
- Минусы: повторяет код Espressif; в upstream вряд ли примут в обход hal_espressif.

### Вывод

- **Самый дешёвый рабочий шаг** — A1 (SHA/HMAC), около 500 строк glue, 2–3 недели. Ускоряет
  хеши, HMAC и PBKDF2, для Matter AES-CCM ничего не даёт.
- **Matter AES-CCM** требует A2 + A3(в): в сумме **5–9 недель**.
- **ECC** (главная нагрузка CASE/PASE) реально не ускорить без изменений TF-PSA-Crypto. Для него
  лучше p256-m.
- **Кому делать:**
  - по всем признакам путь A — работа Espressif: hal_espressif синхронизируется с IDF master,
    Zephyr-правки `esp_config.h` уже идут, sylvioalves в zephyr #109634 обещал проверить на ESP32;
  - разумный порядок для внешнего участника: issue/вопрос в zephyr или hal_espressif;
  - если делать самим — прототип A1 (1–2 дня) с замером `tests/benchmarks/psa_crypto` на C6
    до оценки остального.

## 5. Что это значит для нас

1. Использовать аппаратный C6 для криптографии STM32 нельзя без собственного RPC поверх
   esp-hosted: латентность SPI плюс сериализация, для коротких операций вряд ли выгодно.
   Не рассматривать.
2. Espressif — полезный второй образец (после Nordic):
   - драйвер SHA с сохранением контекста (`esp_sha/core`, `SOC_SHA_SUPPORT_RESUME`);
   - порог DMA для AES;
   - общий lock на связанные периферии;
   - интеграция через сгенерированный или ручной dispatch в форке — то, что Zephyr #110328
     позволяет делать без форка.
3. Предупреждение из PR #19027 применимо и к нам: аппаратный блок бывает медленнее программной
   реализации на коротких данных. Этап 0 плана (замеры) обязателен.
4. Если C6 переведут на IDF 6.x — проверить Wi-Fi/WPA3 (PSA-драйверы, #19065 CCM) и рост flash.
