# План: PSA-драйвер аппаратного HASH (SHA-256 / HMAC-SHA256) для STM32U5

Дата: 2026-09-14. Статус: план, кода нет. Основан на анализе текущего дерева
(Zephyr + mbedTLS 4.x + TF-PSA-Crypto, build `stm32u585xx/ext_flash_app`) и на обзоре
Nordic nrf_security (sdk-nrf `main`, веб-источники; помечено «Nordic»).
Предыдущий отчёт: [aes-hw-acceleration.md](aes-hw-acceleration.md).

## 1. Уходят ли PSA-вызовы в TF-M на STM32U5

**Нет. В этом продукте ни один вызов в TF-M не уходит.**

- Провайдер PSA выбирается в `zephyr/modules/mbedtls/Kconfig.psa.logic`:
  `PSA_CRYPTO_PROVIDER_TFM` зависит от `BUILD_WITH_TFM`, а `PSA_CRYPTO_PROVIDER_MBEDTLS` —
  от `!BUILD_WITH_TFM`.
- `BUILD_WITH_TFM` (`zephyr/modules/trusted-firmware-m/Kconfig.tfm`) требует
  `TRUSTED_EXECUTION_NONSECURE` (вариант платы `/ns`), `ARM_TRUSTZONE_M` и непустой
  `TFM_BOARD`. Среди U5 `TFM_BOARD` задан только для `b_u585i_iot02a`. Для нашей платы порта
  нет, и прошивка собирается как secure-only образ (`PSA_CRYPTO_PROVIDER_MBEDTLS=y`).
  Все PSA-вызовы выполняются внутри образа, в TF-PSA-Crypto.
- При сборке `/ns` с TF-M все PSA-вызовы NS-стороны действительно идут через вызовы в
  secure world к Crypto-партиции TF-M. **Но и там на U5 SHA/AES программные.** В
  `trusted-firmware-m/platform/ext/target/stm/b_u585i_iot02a/config.cmake` стоит
  `CRYPTO_HW_ACCELERATOR ON`, но `accelerator/CMakeLists.txt` собирает только
  `stm/common/hal/accelerator/stm.c` (включение SHSI, опционально STSAFE) и `rng.c`.
  `tf_psa_crypto_accelerator_config.h` задаёт только `MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG`.
  ALT-файлы ST (`sha256_alt.c`, `aes_alt.c`, `ccm_alt.c`, `gcm_alt.c`, `ecdsa_alt.c`…) были в
  TF-M v2.1.0 в стиле mbedTLS 3.x и исчезли при переходе на TF-PSA-Crypto
  (Nordic-агент, веб; по рассылке TF-M — в v2.3.0).
- Итог: TF-M не даёт аппаратного ускорения SHA/AES на U5. Вдобавок потребовал бы TrustZone,
  порт платы и перестройку загрузки. Путь к аппаратному ускорению — только собственный
  PSA-драйвер.

## 2. Какой драйвер проще всего

| Кандидат | Сложность | Кому помогает | Вывод |
|---|---|---|---|
| RNG | уже аппаратный | энтропия PSA | — |
| `hash_compute` SHA-256 (однократный вызов) | минимальная: без ключа, без состояния между вызовами | однократные хеши (Matter `Hash_SHA256`, `crypto_bench`) | **этап 1** |
| `mac_compute` HMAC-SHA256 (однократный вызов) | малая: ключ передаётся буфером, `HAL_HMACEx_SHA256_Start` есть в HAL U5 | **PBKDF2**: web-auth (итерации 2..N) и Matter PBKDF2 (все итерации) | **этап 1** |
| SHA-256 multipart (setup/update/finish/clone/abort) | средняя: хвост блока, сохранение контекста регистров, мьютекс, размер `psa_hash_operation_t` | HMAC multipart (HKDF, TLS PRF), хеш-транскрипт TLS, потоковый хеш Matter | этап 2 |
| Режим «только драйвер» (`MBEDTLS_PSA_ACCEL_ALG_SHA_256`, без встроенного sha256.c) | малая после этапа 2 | размер flash, `md.c` через PSA | этап 3, по желанию |
| AES-CCM/GCM (AEAD) | высокая: формат B0 для CCM, GCM IV, тег, multipart | записи Matter и TLS | этап 4, по замерам |
| AES ECB/CBC/CTR через PSA cipher | средняя | **никому**: встроенные CCM/GCM ходят в `mbedtls_block_cipher_*` → `mbedtls_aes_crypt_ecb`, мимо dispatch PSA cipher (подтверждено objdump) | не делать |
| ECDSA/ECDH через PKA | высокая | CASE, TLS | в DTS Zephyr для U5 нет узла PKA; вне плана |

**Рекомендация: начинать с однократных вызовов `psa_hash_compute` и `psa_mac_compute(HMAC-SHA256)`.**
Почему это проще остального:

1. Однократные точки входа не используют `psa_hash_operation_t` и `psa_mac_operation_t`.
   Размеры структур не меняются, ABI для Matter и mbedTLS остаётся прежним, нужны только
   case в функциях `*_compute` диспетчера.
2. Встроенная реализация остаётся и служит запасным путём при `PSA_ERROR_NOT_SUPPORTED`.
   Можно начать с нескольких алгоритмов и длин.
3. Аппаратный блок захватывается только на время одного вызова, сохранять регистры не нужно.
4. Горячий цикл PBKDF2 — ровно `mac_compute`:
   - web-auth → `psa_key_derivation_pbkdf2_generate_block` (`core/psa_crypto.c`): U1 через
     multipart MAC, итерации 2..N через `psa_driver_wrapper_mac_compute`;
   - Matter `PBKDF2_sha256::pbkdf2_sha256` (`CHIPCryptoPALPSA.cpp`): `psa_mac_compute` на каждой
     итерации.
   Сейчас одна итерация = HMAC = 4 программных сжатия SHA-256 с накладными расходами PSA.
   С аппаратным HMAC это одна операция HASH-блока (ключ → данные → ключ).

Встроенный HMAC (`drivers/builtin/src/psa_crypto_mac.c:96-149`) строится поверх публичного
`psa_hash_setup/update/finish`, то есть через dispatch. Поэтому этап 2 ускорит все multipart-HMAC
(HKDF в Matter, TLS PRF) без отдельного MAC-драйвера.

## 3. Как драйвер подключается к Zephyr / TF-PSA-Crypto (факты)

- **Dispatch в Zephyr написан руками, не сгенерирован:**
  `modules/crypto/tf-psa-crypto/dispatch/psa_crypto_driver_wrappers.h` (3410 строк).
  Коммиты `[zep noup] Hardcode CC3XX entry points` и т.п. Драйверы вставлены блоками
  `#if defined(PSA_CRYPTO_DRIVER_CC3XX)`. Порядок в `*_compute` и `*_setup`: тестовый драйвер →
  CC3XX → builtin (`MBEDTLS_PSA_BUILTIN_HASH`). В `update/finish/clone/abort` выбор идёт через
  `switch (operation->id)`.
- **Контексты операций** — union `psa_driver_hash_context_t` / `psa_driver_mac_context_t` в
  `dispatch/include/psa/crypto_driver_contexts_{primitives,composites}.h`. Они встроены в
  `include/psa/crypto_struct.h:74` и `:181`.
- **Выбор каталога dispatch:** `CONFIG_TF_PSA_CRYPTO_DISPATCH_DIR` (строка без prompt,
  default — каталог модуля). Он используется в `core/CMakeLists.txt:9` (исходник `_no_static.c`),
  `:153` (**PUBLIC** include → попадает всем потребителям через `mbedtls_iface`) и
  `CMakeLists.txt:432` (private include ядра).
- **Конфиг драйвера:** `CONFIG_TF_PSA_CRYPTO_DRIVER_CONFIG_FILE` (строка без prompt, пустая)
  подключается в конце `zephyr/modules/mbedtls/configs/config-tf-psa-crypto.h:189-190`.
  Сюда пойдут `#define PSA_CRYPTO_DRIVER_STM32` и позже `MBEDTLS_PSA_ACCEL_ALG_SHA_256`.
- **CMake-хук `TF_PSA_CRYPTO_DRIVER_TARGETS`** (`drivers/CMakeLists.txt:94-113`) заполняется только
  внутренним списком драйверов. Внешний драйвер через него не добавить. Он собирается как
  отдельная Zephyr-библиотека, а заголовок точек входа отдаётся через
  `zephyr_include_directories()`: `tfpsacrypto` линкуется с `zephyr_interface`.
- **Встроенный SHA-256 выключается** только при `MBEDTLS_PSA_ACCEL_ALG_SHA_256`
  (`drivers/builtin/include/mbedtls/private/crypto_adjust_config_enable_builtins.h:528`).
  Тогда `md.c` переходит на PSA (`include/tf-psa-crypto/private/crypto_adjust_config_support.h:99-101`,
  `MBEDTLS_MD_SHA256_VIA_PSA`). Сейчас `mbedtls_md` вызывает `mbedtls_sha256` напрямую (ELF).
- **Matter подстраивается сам:** `modules/lib/matter/src/platform/Zephyr/CHIPPlatformConfig.h:42`
  задаёт `CHIP_CONFIG_SHA256_CONTEXT_SIZE = sizeof(psa_hash_operation_t)`, плюс `static_assert` в
  `CHIPCryptoPALPSA.cpp:349`. Править Matter не придётся, но Matter обязан видеть тот же
  каталог dispatch (см. решение 3).
- **Текущие размеры** (gdb по `zephyr.elf`): `psa_hash_operation_t` 112 Б, `psa_mac_operation_t`
  192 Б, `psa_key_derivation_operation_t` 280 Б, `mbedtls_sha256_context` 104 Б.
- **Инициализация:** `psa_crypto_init()` вызывается из `_mbedtls_init`
  (`SYS_INIT POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT`), раньше `crypto_stm32_hash`
  (`CONFIG_CRYPTO_INIT_PRIORITY=90`). `psa_driver_wrapper_init()` вызывается внутри.
- **Потокобезопасность:** в Zephyr нет Kconfig для `MBEDTLS_THREADING_C`. PSA-ядро (слоты ключей)
  в этой сборке не защищено мьютексами. Проблема существовала и раньше, драйвер её не решает.
  Решение вынесено отдельно (§8).

## 4. HAL HASH на U5 (факты)

- Алгоритмы: MD5, SHA-1, SHA-224, SHA-256 (`HASH_ALGOSELECTION_*`). HMAC — аппаратный режим
  (`HAL_HMACEx_SHA224/256_Start`, в т.ч. `_IT`/`_DMA`).
- Однократный вызов: `HAL_HASHEx_SHA256_Start()` — `CR_INIT` + ALGO, `NBVALIDBITS`, запись DIN
  словами, `DCAL`, опрос `DCIS` (`HASH_WaitOnFlagUntilTimeout`, `HAL_GetTick` из
  `zephyr/soc/st/stm32/common/stm32cube_hal.c`), чтение HR.
- Multipart: `HAL_HASHEx_SHA256_Accmlt()` принимает **только размер, кратный 4** (иначе
  `HAL_ERROR`). Хвост уходит через `HAL_HASHEx_SHA256_Accmlt_End()`. `CR_INIT` выставляется
  только при `hhash->Phase == HAL_HASH_PHASE_READY`.
- Переключение контекста: `HAL_HASH_ContextSaving/Restoring()` сохраняет IMR, STR, CR и 54 CSR —
  буфер `(54+3)*4 = 228 Б`.
- Существующий Zephyr-драйвер `drivers/crypto/crypto_stm32_hash.c` умеет только однократный
  хеш через Zephyr crypto API (multipart → `-ENOTSUP`). Как бэкенд PSA он не подходит; как
  пример инициализации (clock, `HAL_HASH_Init`) — подходит.

## 5. Что берём у Nordic (CRACEN)

Источники: sdk-nrf `subsys/nrf_security/src/drivers/cracen/cracenpsa/src/cracen_psa_hash.c`,
`cracen_psa_primitives.h`, `sxsymcrypt/src/platform/baremetal/cmdma_hw.c`,
`src/psa_crypto_driver_wrappers.c`.

1. **Состояние живёт в контексте операции, не в железе.** `cracen_hash_operation_s` = алгоритм
   + сохранённое состояние HW + буфер неполного блока + `bytes_left_for_next_block`.
   В `update`: если данных меньше блока — только копирование в буфер, HW не трогается;
   иначе `reserve HW → restore (или create) → подать буфер и целые блоки → save → release`.
   Поэтому параллельные операции работают, а `clone` = `memcpy`, `abort` = зануление.
2. **Аппаратный блок захватывается на один вызов** (мьютекс + счётчик пользователей для
   питания/тактирования), и никогда между вызовами.
3. **Dispatch с запасным путём:** драйверы пробуются по порядку; всё, кроме
   `PSA_ERROR_NOT_SUPPORTED`, возвращается сразу; `operation->id` фиксирует владельца для
   последующих вызовов.
4. **Kconfig-цепочка** `PSA_WANT_x → PSA_USE_<DRV>_<API>_DRIVER → PSA_NEED_<DRV>_x → PSA_ACCEL_x`.
   Выбор делается для каждого API. У нас — упрощённая версия: `PSA_CRYPTO_DRIVER_STM32_HASH`,
   `_HMAC`.
5. У Nordic нет порога по длине: мелкие `update` просто буферизуются. Нужен ли порог нам
   (HW медленнее на коротких данных?), решит бенчмарк.
6. Тесты Nordic/Oberon: наборы `test_suite_psa_crypto_hash`, `_driver_wrappers`, `.pbkdf2`,
   `.concurrent`, PSA arch tests.

## 6. Дизайн драйвера

### Раскладка

```
modules/psa-driver-stm32/            # модуль приложения, как остальные modules/*
  zephyr/module.yml
  CMakeLists.txt                     # zephyr_library(); zephyr_include_directories(include)
  Kconfig                            # PSA_CRYPTO_DRIVER_STM32[_HASH|_HMAC]
  include/stm32_psa_driver.h         # прототипы точек входа, контекст (этап 2)
  include/stm32_psa_driver_config.h  # TF_PSA_CRYPTO_DRIVER_CONFIG_FILE
  src/stm32_psa_hash.c               # hash_compute (+ multipart на этапе 2)
  src/stm32_psa_hmac.c               # mac_compute HMAC-SHA256
  src/stm32_hash_hw.c                # clock, HAL_HASH_Init, мьютекс, (save/restore)
  dispatch/                          # копия dispatch TF-PSA-Crypto с блоками STM32
    psa_crypto_driver_wrappers.h
    psa_crypto_driver_wrappers_no_static.{c,h}
    include/psa/crypto_driver_contexts_{primitives,composites}.h
```

### Kconfig

- `PSA_CRYPTO_DRIVER_STM32_HASH`: `depends on PSA_CRYPTO_PROVIDER_MBEDTLS &&
  DT_HAS_ST_STM32_HASH_ENABLED && PSA_WANT_ALG_SHA_256`, `select USE_STM32_HAL_HASH`,
  `select USE_STM32_HAL_HASH_EX`.
- `PSA_CRYPTO_DRIVER_STM32_HMAC`: `depends on PSA_CRYPTO_DRIVER_STM32_HASH && PSA_WANT_ALG_HMAC`.
- `TF_PSA_CRYPTO_DRIVER_CONFIG_FILE` и `TF_PSA_CRYPTO_DISPATCH_DIR` не имеют prompt, из
  `prj.conf` их не задать. Задаём `default ... if PSA_CRYPTO_DRIVER_STM32_HASH`. Для
  `DISPATCH_DIR` у Zephyr уже есть default, и побеждает первый определённый. Поэтому default
  ставим в корневой `Kconfig` приложения до `source "Kconfig.zephyr"`. Проверить порядок на
  этапе 1 по `build/.../zephyr/.config`.
- `CONFIG_CRYPTO_STM32_HASH` в `prj.conf` выключить, чтобы у HASH-блока не было двух владельцев.

### Точки входа, этап 1

```c
psa_status_t stm32_hash_compute(psa_algorithm_t alg, const uint8_t *in, size_t in_len,
                                uint8_t *hash, size_t hash_size, size_t *hash_len);
psa_status_t stm32_mac_compute(const psa_key_attributes_t *attr,
                               const uint8_t *key, size_t key_len, psa_algorithm_t alg,
                               const uint8_t *in, size_t in_len,
                               uint8_t *mac, size_t mac_size, size_t *mac_len);
```

- Для всего, кроме `PSA_ALG_SHA_256`, `PSA_ALG_HMAC(PSA_ALG_SHA_256)` и ключа типа
  `PSA_KEY_TYPE_HMAC`, возвращать `PSA_ERROR_NOT_SUPPORTED` (запасной путь — builtin).
  Позже добавить SHA-224.
- Мьютекс `k_mutex` на всё время вызова. Не вызывать из ISR (таких потребителей нет; записать в
  README модуля).
- Ошибки HAL (`HAL_TIMEOUT`, `HAL_ERROR`) → `PSA_ERROR_HARDWARE_FAILURE`, после них
  `HAL_HASH_DeInit/Init`.
- Инициализация лениво при первом вызове или в `psa_driver_wrapper_init`: включить clock HASH
  (`STM32_CLOCK_CONTROL_NODE` уже готов), `HAL_HASH_Init` с `DataType = HASH_DATATYPE_8B`.
  Не опираться на Zephyr-устройство `crypto_stm32_hash`: оно инициализируется позже.
- Правка dispatch: в `psa_driver_wrapper_hash_compute` и `psa_driver_wrapper_mac_compute`
  добавить блок `#if defined(PSA_CRYPTO_DRIVER_STM32)` перед builtin с
  `if (status != PSA_ERROR_NOT_SUPPORTED) return status;`. **`MBEDTLS_PSA_ACCEL_ALG_*` на этапе
  1 не определять**: иначе пропадёт builtin, которому нужны multipart HMAC и hash.

### Этап 2: multipart

- Контекст `stm32_hash_operation_t`: `alg`, `uint8_t buf[64]`, `buf_len`,
  `uint32_t hw_ctx[57]` (228 Б), `bool started`. Итого ≈ 300 Б →
  `psa_hash_operation_t` вырастет со 112 до ≈ 310 Б, `psa_mac_operation_t` — примерно на ту же
  величину (внутри HMAC лежит hash-операция). Пересчитать стеки (PBKDF2 worker web-auth, TLS,
  потоки Matter) по `thread analyzer`.
- Модель как у CRACEN: **сохранение и восстановление на каждом вызове** (restore → подача целых
  блоков, кратных 4 → save). Модель «владелец HW» (переключать только при смене операции)
  быстрее для PBKDF2, но при операции, брошенной без `abort`, даёт висячий указатель. Её —
  только если замеры потребуют.
- `update` с данными меньше остатка блока — только буфер. `clone` — `memcpy`; `abort` —
  зануление.
- В dispatch: `setup` пробует STM32 первым; `update/finish/clone/abort` — `case
  PSA_CRYPTO_STM32_DRIVER_ID`. Новый член union в `crypto_driver_contexts_primitives.h`, новый ID
  в enum.
- **HAL или регистры** — решает прототип (этап 1a): HAL-машина состояний (`Phase`, `State`,
  `__HAL_LOCK`) не рассчитана на подмену контекста между вызовами. Прямая работа с CR/DIN/STR/HR/CSR
  по RM0456 может оказаться проще. Проверить на плате:
  `Accmlt(64) → ContextSaving → однократный Start другой операции → ContextRestoring → Accmlt_End`
  и сравнить с программным результатом.

### Этап 3 (опционально): только драйвер

`#define MBEDTLS_PSA_ACCEL_ALG_SHA_256` в конфиге драйвера — встроенный `sha256.c` уходит, `md.c`
идёт через PSA. Условие: всё, что хеширует через `mbedtls_md`, запускается после
`psa_crypto_init` (POST_KERNEL). HMAC остаётся встроенным поверх аппаратного hash —
`MBEDTLS_PSA_ACCEL_ALG_HMAC` не определять.

### Этап 4 (по замерам): AES-CCM однократно

По той же схеме: однократный `psa_driver_wrapper_aead_encrypt/decrypt` для `PSA_ALG_CCM` через
HAL CRYP (Matter шифрует сообщения однократным `psa_aead_encrypt`, `CHIPCryptoPALPSA.cpp:105`).
Multipart и GCM остаются builtin. Отдельный план — после данных этапов 0–1.

## 7. Проверка

**Этап 0 — базовая линия до любых правок** (плата A, текущая прошивка):
- `matter crypto_bench 100` (SHA-256 512B, AES-CCM enc/dec) → записать µs.
- `web_auth kdf 3000` → время одной деривации.
- Добавить в `src/matter/crypto_bench.cpp` (код приложения): HMAC-SHA256 32 Б, SHA-256 64 Б и
  4096 Б, PBKDF2-HMAC-SHA256 1000 итераций. Результаты — в этот каталог (`bench/baseline.md`).

**Корректность:**
- KAT SHA-256: пустое сообщение, 3, 55, 56, 63, 64, 65 Б, 1000 Б, 1 МБ `'a'`. Вектора из
  `tf-psa-crypto/tests/suites/test_suite_psa_crypto_hash.data` и `test_suite_sha256.data`.
- HMAC-SHA256: RFC 4231 (в т.ч. ключ > 64 Б — режим LKEY у HW).
- PBKDF2-HMAC-SHA256: `test_suite_psa_crypto.pbkdf2.data`.
- Дифференциальный тест: случайные длины 0..4096, результат драйвера == результат builtin
  (на этапах 1–2 builtin в сборке).
- Этап 2: разбиение сообщения на все точки 0..130; две перемежающиеся операции; `clone`
  посередине; `abort` без `finish`; две нити одновременно.
- Тест `static_assert`/runtime: `sizeof(psa_hash_operation_t)` одинаков в TU приложения и в
  ядре (функция в драйвере возвращает размер, тест сравнивает), и Matter компилируется с тем же
  каталогом dispatch — проверить флаги GN/`compile_commands.json`.
- Оформление: ztest-приложение в `tests/` проекта (native_sim с заглушкой HAL нельзя — только на
  плате) или шелл-команда `crypto_selftest` в диагностике.

**Сквозные проверки на плате:**
- Вход в web UI (PBKDF2), смена пароля.
- TLS-сокеты приложения (`NET_SOCKETS_SOCKOPT_TLS=y`): рукопожатие.
- Matter: commissioning (PASE: PBKDF2 + SPAKE2+, CASE: HKDF/HMAC), повторное подключение
  контроллера.
- Бенчмарки этапа 0 после этапов 1 и 2 — таблица «до/после» в отчёт.

## 8. Решения владельца и риски

1. **Копия dispatch в модуле или патч в `patches/`** против `modules/crypto/tf-psa-crypto`.
   Копия не трогает общий воркспейс (он общий с другими проектами), но при обновлении Zephyr
   может разойтись с оригиналом. Патч при обновлении ломается явно, зато меняет общий checkout.
   Предлагаю копию и скрипт `diff` против оригинала в шаге обновления.
2. **Порог по длине** (короткие данные в software): только если бенчмарк покажет, что HW
   медленнее.
3. **PSA-ядро без `MBEDTLS_THREADING_C`** — отдельная задача (слоты ключей используются из
   потоков Matter, HTTP, TLS). Nordic закрывает её `threading_alt.c` на `k_mutex`.
4. **Питание:** HASH-clock включён постоянно vs по требованию. При появлении режимов Stop —
   проверить сохранение регистров HASH.
5. Если на этапе 0 окажется, что PBKDF2/хеши не узкое место, ограничиться этапом 1.

## 9. Куда отправлять изменения (upstream)

| Что меняется | Репозиторий | Кто отвечает |
|---|---|---|
| Ядро TF-PSA-Crypto (общие изменения, не вендорские) | github.com/Mbed-TLS/TF-PSA-Crypto, ветка `development` | команда Mbed TLS (проект Trusted Firmware). Правила — `CONTRIBUTING.md`: DCO sign-off, тесты, запись в `ChangeLog.d`, обсуждение в issues или рассылке mbed-tls@lists.trustedfirmware.org |
| Dispatch Zephyr (`dispatch/`, коммиты `[zep noup]`) | github.com/zephyrproject-rtos/tf-psa-crypto (форк, `zephyr/west.yml` revision `000d24cb5`) | `MAINTAINERS.yml` → «West project: tf-psa-crypto»: ceolin, valeriosetti, tomi-font; метка `area: TF-PSA-Crypto` |
| Интеграция в Zephyr: `modules/mbedtls/` (Kconfig, `config-tf-psa-crypto.h`, CMake), `tests/crypto/psa`, `tests/benchmarks/psa_crypto` | github.com/zephyrproject-rtos/zephyr | «West project: mbedtls»: d3zd3z, ceolin, valeriosetti, tomi-font; collaborator ithinuel |
| `drivers/crypto/`, `tests/crypto/` | zephyr | «Drivers: Crypto»: ceolin (collaborator valeriosetti) |
| HAL STM32 | github.com/zephyrproject-rtos/hal_stm32 | «West project: hal_stm32»: erwango + коллабораторы ST |

- В upstream TF-PSA-Crypto в `drivers/` нет драйверов реального железа (только builtin, everest,
  p256-m, pqcp). Вендорский драйвер STM32 туда вряд ли примут; предмет для upstream — только
  общие механизмы.
- Обычный порядок для форка модуля в Zephyr: PR в `zephyrproject-rtos/tf-psa-crypto` и
  параллельно PR в `zephyr` с обновлением revision в `west.yml` (помечается DNM, пока PR
  модуля не влит). Пример — NXP MCXA, zephyr PR #118311 (по данным веб-обзора, не проверено).
- Прежде чем писать код для upstream: issue/RFC в Zephyr (метки `area: TF-PSA-Crypto`,
  `area: Crypto / RNG`, упомянуть мейнтейнеров) — как подключать вендорские PSA-драйверы,
  раз dispatch в форке правится руками, и где жить драйверу STM32 (`drivers/crypto`,
  `modules/mbedtls` или `hal_stm32`).
- Для этапа 0 в Zephyr уже есть бенчмарк `tests/benchmarks/psa_crypto` (`hash.c`: однократный
  `psa_hash_compute` SHA-1/224/256/384/512; `cipher.c`).

## 10. Issues и PR по теме (проверено через `gh`, 2026-09-14)

### Механизм подключения драйверов: уже решён и влит

- **zephyr #110328** «modules: mbedtls: Allow custom driver dispatch implementations»
  (asmellby, Silicon Labs), влит 2026-08-17. Коммит `e603d04fa7c` — **есть в нашем дереве**
  (предок HEAD). Добавил `TF_PSA_CRYPTO_DISPATCH_DIR` и `TF_PSA_CRYPTO_DRIVER_CONFIG_FILE`.
  Мейнтейнеры (valeriosetti, frkv) называют это направлением, согласованным Arm и вендорами на
  встречах «PSA Crypto Drivers». План в §6 опирается ровно на этот механизм.
- **zephyr #110414** «soc: silabs: Hardware acceleration for TF-PSA-Crypto» (open, DNM) —
  эталон вендорской интеграции поверх #110328. Свой dispatch и конфиг лежат в модуле
  `hal_silabs`, defaults — в `soc/silabs/.../Kconfig.defconfig`. Раскладку нашего модуля
  сверить с ним.
- **zephyr #109634** (JordanYates, закрыт) — альтернатива с патчем dispatch под OOT-ускорители
  (включая multipart hash). Пример downstream-интеграции: Embeint/infuse-sdk, коммит `4d8cf6b`.
- **zephyr #116463** (влит) — hotfix тестов после переноса заголовков контекстов в
  `dispatch/include/psa/`.
- **Mbed-TLS/TF-PSA-Crypto #809** (open) — завершение переноса driver wrappers в `dispatch/`
  upstream. Раскладка файлов dispatch ещё поменяется: копию в нашем модуле придётся сверять при
  обновлении Zephyr.
- **zephyr #115241** (RFC, закрыт) — модуль `tf-psa-crypto-drivers`. Официальный репозиторий
  github.com/Mbed-TLS/tf-psa-crypto-drivers, форк zephyrproject-rtos/tf-psa-crypto-drivers
  (создан 2026-08-27, в нашем `west.yml` его пока нет). Вендоры — только `arm` (cc3xx).
  Правила (frkv, #110432): лицензия по уставу TrustedFirmware, вендор владеет своей папкой, PR
  проводит Arm, без бинарных блобов.
- **zephyr #43712** «PSA Crypto API adoption» закрыт 2026-08-10: переход на PSA считается
  завершённым, аппаратные ускорители — «WIP».
- **zephyr #104917** (закрыт): valeriosetti подтверждает, что `_ALT` удалены и ускоритель
  раньше можно было добавить только патчем `psa_crypto_driver_wrappers.h`.

### STM32

- **zephyr #107997** «Crypto hardware support for MCUboot» (nrg83, закрыт ботом как stale, без
  ответа). Ровно наша ситуация: STM32 с `CONFIG_CRYPTO_STM32_HASH=y`, а SHA-256 всё равно
  считает builtin `sha256.c`.
- **zephyr #110432** «stm32: Add secure AES support» (SAES для U5, open, stale). Ключевая
  переписка:
  - erwango (ST, 2026-06-04): Zephyr идёт к «только PSA», вопрос, стоит ли добавлять новые
    in-tree драйверы crypto API;
  - erwango (2026-07-06): PSA-драйверы STM32 должны жить в **hal_stm32**, у ST
    **внутренняя команда над этим работает, ожидали «end of August»**, код сообщества до этого
    не принимают;
  - erwango (2026-08-07): предложил пока снова принимать in-tree драйверы crypto API,
    valeriosetti согласился.
- **zephyr #118746** (etienne-lms, ST, open 2026-09-10): узлы `aes`/`cryp`/`hash` во всех
  подходящих STM32 DTSI; в описании — «will help their integration as PSA Crypto Drivers in a
  latter change». Работа ST над PSA-драйверами, судя по всему, близка к публикации.
- **zephyr #118739** (etienne-lms, open): AES для HAL2 (stm32c5xx); **hal_stm32 #408** (open)
  к нему.
- **zephyr #110230** (open): таймауты HAL CRYP на U5 не учитывают делитель AHB2/SHSI для
  AES/SAES — учесть на этапе 4.
- **zephyr #106402** (закрыт): ошибка PKA HAL на H7RS; erwango отправил в репозиторий STM32Cube —
  ошибки HAL чинятся у ST, не в Zephyr.
- **zephyr #107611**, **#108257**, **#108876** — исправления DT (узел AES на SoC без AES, H5/L4).
- **zephyr #66980** (закрыт) — возможное переполнение длины в `crypto_stm32.c`.
- История in-tree драйвера (Zephyr crypto API): #21868 CRYP, #36991 AES, #78669 L4,
  #93189 H7 CCM/GCM, #93923 HASH, #114222 H5, #111621/#114269 C5; open #118321 (MP13 hash),
  #116564 (PKA WBA).

### Что это меняет в плане

1. Механизм (свой `DISPATCH_DIR` + `DRIVER_CONFIG_FILE`) официальный и уже в дереве.
   Патчить `tf-psa-crypto` не нужно, решение §8.1 снимается в пользу копии dispatch в модуле.
2. **Прежде чем писать драйвер, спросить ST о статусе внутренних PSA-драйверов** (erwango,
   etienne-lms; комментарием в #118746 или новым issue со ссылкой на #110432 и #107997). Если
   HASH/AES для U5 выходят в hal_stm32 в ближайшие недели, наш драйвер свести к этапу 0 + проверке
   их реализации.
3. Если делаем свой: модуль приложения по образцу #110414, чтобы его можно было выбросить, когда
   появится драйвер ST. В upstream отдавать тесты/бенчмарки и найденные проблемы, а не
   конкурирующий драйвер.
