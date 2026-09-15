# Замеры криптографии на плате: p256-m (2026-09-14)

Вопрос владельца: насколько ускорится сопряжение Matter с p256-m. Все прогоны — без правок
исходников Zephyr и Matter, образы в scratchpad.

## Образы

- `prod-zms-shell`: прошивка из репозитория (ZMS 4 сектора, кеш 2048, опрос SPI) +
  `settings-backends/configs/common.conf` (шелл settings) — тот же, что `p3/tuning/fabrics/zms-prod-sectors4`.
- `prod-zms-shell-p256m`: то же + `CONFIG_MBEDTLS_PSA_P256M_DRIVER_ENABLED=y` +
  `CONFIG_PSA_WANT_ALG_JPAKE=y` (`../../p3/tuning/queue/trials/crypto/p256m.conf`).
  `.config` отличается ровно этими двумя строками; код eth_w5500/esp_hosted одинаков; образ +2,3 КБ.

Почему нужен `PSA_WANT_ALG_JPAKE`: p256-m объявляет secp256r1 ускоренной, и TF-PSA-Crypto
перестаёт собирать встроенный ECP. Matter (`CHIPCryptoPALPSA.cpp`, `Spake2p_P256_SHA256_HKDF_HMAC::PointMul`,
`PointAddMul`, `ComputeL`) вызывает `mbedtls_ecp_mul`/`mbedtls_ecp_muladd` напрямую — линковка падает.
Задать `MBEDTLS_ECP_C` / `MBEDTLS_ECP_DP_SECP256R1_ENABLED` через `CONFIG_TF_PSA_CRYPTO_USER_CONFIG_FILE`
нельзя: `#error "... was removed in TF-PSA_Crypto 1.0"`. `PSA_WANT_ALG_DETERMINISTIC_ECDSA` включает
`MBEDTLS_ECP_C`, но не кривую secp256r1 (при ускоренной кривой) — SPAKE2+ сломался бы при PASE.
EC J-PAKE p256-m не ускоряет, поэтому конфиг сам включает встроенные ECP + secp256r1 + BIGNUM.
Цена — код J-PAKE. Вариант Matter с SPAKE2+ через PSA (`chip_crypto_spake2p = "psa"`,
`PSASpake2p.cpp`) модуль Zephyr наружу не выводит, а встроенного `PSA_ALG_SPAKE2P_MATTER` в
Kconfig Zephyr нет — это правка вендорского Matter, не пробовали.

## `matter crypto_bench 10` (`crypto_bench_run.py`, среднее на операцию)

| Операция | Без p256-m | С p256-m | Ускорение |
|---|---|---|---|
| P-256 ECDH | 1546,0 мс | 112,0 мс | ×13,8 |
| P-256 ECDSA sign | 745,1 мс | 170,1 мс | ×4,4 |
| P-256 ECDSA verify | 3053,3 мс | 430,2 мс | ×7,1 |
| P-256 keygen | 0,96 мс | 137,1 мс | не сравнимо |
| SHA-256 512 Б | 0,83 мс | 0,49 мс | ×1,7 |
| HMAC-SHA256 512 Б | 1,25 мс | 0,78 мс | ×1,6 |
| HKDF-SHA256 32 Б | 1,42 мс | 1,18 мс | ×1,2 |
| AES-CCM enc / dec 128 Б | 2,40 / 2,27 мс | 2,08 / 1,95 мс | ×1,15 |

Оговорки:
- keygen без p256-m 0,96 мс: встроенный PSA, по-видимому, откладывает вычисление открытого ключа
  до экспорта (Matter экспортирует его сразу) — цена переносится, сравнивать нельзя.
- SHA/HMAC/AES p256-m не затрагивает, но они быстрее на 15–70 %. Вероятно, это раскладка кода
  (приложение исполняется с внешней флеш через ICACHE 1-way, см. `p3/tuning/diag/README.md`),
  а не p256-m. Один прогон на образ.
- SPAKE2+ (PASE) в бенче нет и p256-m его не ускоряет (прямые `mbedtls_ecp_*`).

Результаты сопряжения (5 fabrics, chip-tool) — `p3/tuning/fabrics/zms-prod-sectors4-p256m`.
