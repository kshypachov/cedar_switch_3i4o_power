# Аппаратный AES в mbedTLS на STM32U5 — анализ сборки

Дата: 2026-09-14. Статический анализ текущей сборки
`build/cedar_switch_3in4out_power` (board `cedar_switch_3in4out_power_rev3/stm32u585xx/ext_flash_app`,
mbedTLS 4.x + TF-PSA-Crypto, `PSA_CRYPTO_PROVIDER_MBEDTLS`). На плате не запускалось.

## Вывод

**Нет. mbedTLS/PSA сейчас шифрует AES программно.** Аппаратные блоки AES и HASH
STM32U585 используются только драйверами Zephyr crypto API (`crypto_stm32`,
`crypto_stm32_hash`), а их никто не вызывает. Из аппаратной криптографии STM32 mbedTLS
использует только **RNG** (`ENTROPY_STM32_RNG` → `zephyr_entropy.c` →
`MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG`).

| Блок | Есть в SoC/DTS | Драйвер Zephyr | mbedTLS/PSA использует |
|------|----------------|----------------|-------------------------|
| RNG  | `rng` okay     | `ENTROPY_STM32_RNG` | да (энтропия) |
| AES  | `aes` okay (`stm32u5_crypt.dtsi`) | `CRYPTO_STM32=y` (HAL CRYP) | **нет**, программный `aes.c` |
| HASH | `hash` okay    | `CRYPTO_STM32_HASH=y` (HAL HASH) | **нет**, программный `sha256.c` |

## Доказательства

1. **В ELF есть программная реализация** (`arm-zephyr-eabi-nm -S zephyr.elf`):
   `mbedtls_internal_aes_encrypt` 0x3b8, `mbedtls_internal_aes_decrypt` 0x3b8,
   `aes_gen_tables` 0x208, `mbedtls_aes_setkey_enc`, `mbedtls_ccm_*`, `mbedtls_gcm_*`,
   `mbedtls_internal_sha256_process` 0x144.
2. **В mbedTLS 4.x нет хука для ускорителя.** Хуки вида `MBEDTLS_AES_ALT` удалены,
   в `zephyr/modules/mbedtls/configs` остались только `PLATFORM_*_ALT`. Ускоритель можно
   подключить только как PSA transparent driver: `PSA_ACCEL_*` плюс entry points в
   `psa_crypto_driver_wrappers.h`, через `CONFIG_TF_PSA_CRYPTO_DRIVER_CONFIG_FILE`.
   Этот параметр в сборке пустой. Dispatch знает только cc3xx, p256-m и test driver.
   В `modules/hal/stm32` PSA-драйвера нет.
3. **Реализацию AES выбирает `aes.c`:** она аппаратная только для AESNI (x86) и AESCE (ARMv8-A).
   Для Cortex-M33 остаётся программная.
4. **Драйвер `crypto_stm32` никто не вызывает.** `ninja -t deps` показывает, что
   `zephyr/crypto/*.h` подключают только `crypto_stm32.c`, `crypto_stm32_hash.c` и
   `net_context.c`. `net_context.c` берёт из `cipher.h` только типы и не вызывает ни
   `cipher_*`, ни `hash_*`. Потенциальные потребители в дереве (LoRaWAN, OSDP,
   IEEE 802.15.4, netmidi2, cc2520) выключены. Функции драйвера попадают в образ только
   через таблицу API устройства.
5. **Путь Matter:** `CHIPCryptoPALPSA.cpp` → `psa_aead_encrypt(CCM)` →
   `psa_crypto_aead.c` → `mbedtls_ccm_*` → `mbedtls_aes_crypt_ecb` →
   `mbedtls_internal_aes_encrypt`. TLS (GCM) и PBKDF2-HMAC-SHA256 (веб-аутентификация, P2)
   тоже работают программно.
6. **MCUboot:** `BOOT_SIGNATURE_TYPE_NONE`, криптографию не использует.

## TF-M

В TF-M PSA-вызовы уходят только при сборке `/ns` (`BUILD_WITH_TFM` →
`PSA_CRYPTO_PROVIDER_TFM`). Для нашей платы порта TF-M нет, образ собран как secure-only.
Даже для `b_u585i_iot02a` порт TF-M при `CRYPTO_HW_ACCELERATOR ON` собирает только `stm.c` и
`rng.c`, поэтому SHA и AES в secure world тоже программные. Подробности и план драйвера — в
[hw-hash-driver-plan.md](hw-hash-driver-plan.md).

## Сопутствующие находки

- **Таблицы AES строятся при старте и лежат в PSRAM.** `CONFIG_MBEDTLS_AES_ROM_TABLES`
  не включён (default n), поэтому `aes_gen_tables` строит FSb/FT0-3/RSb/RT0-3
  (~8.5 KiB) в `.bss`. Эта `.bss` перенесена в PSRAM (`0x7019606c..0x7019766c`,
  регион `PSRAM` 0x70000000). Каждый раунд программного AES читает эти таблицы через
  QSPI PSRAM. `AES_FEWER_TABLES` зависит от `ROM_TABLES` и тоже выключен.
- **SHA-256 — медленный вариант:** `CONFIG_MBEDTLS_SHA256_SMALLER=y` (Zephyr default).
- **Драйверы `crypto_stm32*` собираются и инициализируются впустую:** ~2.3 KiB кода,
  `crypto_stm32_sessions` 0xb8 и `crypto_stm32_dev_data` 0x98 в RAM, инициализация на
  priority 90. Причина: `default y` при `&aes`/`&hash` `status = "okay"` в DTS платы и
  явные `CONFIG_CRYPTO_STM32*=y` в `prj.conf`. Оставлять ли их — решение владельца.

## Замеров нет

Шелл-команда `matter crypto_bench [iterations]` (`src/matter/crypto_bench.cpp`;
SHA-256 512B, AES-CCM encrypt/decrypt через PSA) в образ входит, но её результатов нет
ни в `docs/`, ни в `logs/`. Прежде чем решать про PSA-драйвер STM32 или про
`AES_ROM_TABLES`, нужно снять базовую линию на плате.

## Что можно сделать (не сделано, решение владельца)

1. Дешёвый вариант: `CONFIG_MBEDTLS_AES_ROM_TABLES=y`. Таблицы уйдут во flash (XIP QSPI)
   и перестанут строиться при старте. Сравнить скорость `crypto_bench` до и после:
   сейчас таблицы в PSRAM, после будут во flash.
2. Аппаратный AES: написать PSA transparent driver (AES-ECB/CCM/GCM поверх HAL CRYP) и
   подключить его через `TF_PSA_CRYPTO_DRIVER_CONFIG_FILE`. Готового драйвера в Zephyr или
   hal_stm32 нет. Выгода для коротких пакетов Matter не очевидна: HAL CRYP добавляет
   накладные расходы на каждый вызов. Без замеров решать рано.
3. SHA-256 так же через HASH-блок: ускорит PBKDF2 (3000 итераций в P2).
