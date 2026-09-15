# P6 — патч ESP-Hosted: перезапуск связи после прошивки C6 (решение №2 (а))

Файл: `patches/zephyr/esp_hosted_mcu-restart-after-coprocessor-flash.patch`
(sha256 `c978d1e7089831cb4ec21e62ffe37d517f01ab6b7fe0f19e1200ee2d3d596449`), запись в
`patches/patches.yml`, строка в `patches/README.md`.

## Почему

После сброса C6 работающий драйвер получает boot priv event (P0: event 34), SPI-транспорт жив,
но: версия прошивки заполняется из события только при `fw_version == 0`, а она уже известна (или
0 на пустом C6 → первая же прошивка оставит её такой, какой её выучит событие, но повторная — нет);
`WifiInit`/`SetWifiMode`/`WifiStart` выполняются только в init сетевого устройства — новая прошивка
их не получает, scan и connect отказывают до перезагрузки STM32.

## API (объявлено в `zephyr/drivers/misc/esp_hosted_mcu/esp_hosted_mcu.h`)

```c
int esp_hosted_mcu_restart(k_timeout_t timeout);
int esp_hosted_mcu_wifi_restart(void);
```

`esp_hosted_mcu_restart(timeout)` (ядро):
- держит `esp_hosted_mcu_rpc_lock` всё время (mutex рекурсивный, собственный запрос версии берёт его
  повторно): зависший у C6 round trip RPC может только истечь, новые RPC ждут окончания перезапуска;
- под `esp_hosted_mcu_rpc_ctx_lock` снимает `pending` — ответ прошлого запуска не будет принят;
- обнуляет `fw_version` и `rx_stride_aligned` (выучены из событий прошлого запуска);
- ставит `rx_flush`: поток приёма в начале следующей итерации (не дольше `EVENT_TASK_POLL_MS`)
  отбрасывает перенесённый обрывок кадра;
- вызывает новую необязательную операцию транспорта `flush_rx` — SPI опустошает stash (кадры,
  которые передача вдвинула до перезапуска) под своим `lock`; у SDIO операции нет (NULL);
- ждёт boot event до `ESP_HOSTED_MCU_BOOT_EVENT_WAIT` (1 с), затем запрашивает
  `GetCoprocessorFwVersion` с тем же таймаутом, и так до `timeout`;
- **не трогает линию сброса (EN)** — ей владеет coprocessor-manager;
- возвращает 0, когда версия ненулевая (`esp_hosted_mcu_fw_version()`), `-ETIMEDOUT` — если нет,
  `-ENODEV` — если ядро не поднялось при старте.

`esp_hosted_mcu_wifi_restart()` (Wi-Fi-драйвер):
- `-ENODEV`, если сетевое устройство не готово: init при старте вернул ошибку (плата B: `WifiInit
  failed`), ядро держит `init_res`, `device_init()` повторно не вызывается, `net_if` его интерфейс
  не инициализировал — **без перезагрузки STM32 это не исправить, и патч не обходит ядро**;
- иначе сообщает потерю связи прошлого запуска так, как это сделало бы событие C6:
  `ASSOCIATING` → connect result `WIFI_STATUS_CONN_FAIL`; `COMPLETED` → DHCPv4 stop (если
  `WIFI_STA_AUTO_DHCPV4`), `net_if_dormant_on`, disconnect result; soft AP (`AP_STA_MODE`) →
  ap disable result;
- выполняет последовательность init заново: MAC (`IfaceMacAddrSetGet`, или `GetMACAddress` для
  «legacy» при версии 0), `WifiInit`, `SetWifiMode(STA)`, `WifiStart`. Последовательность вынесена в
  `esp_hosted_mcu_wifi_bringup()`, общую с init (init ведёт себя как прежде);
- 0 или `-EIO`, если C6 отказал в шаге.

## Блокировки и чего нет

- Ядро: см. выше; поток TX не останавливается — кадры данных, стоявшие в очереди до перезапуска,
  уйдут новой прошивке (до `WifiStart` она их отбрасывает).
- Wi-Fi: состояние интерфейсов меняется без собственного lock, как и в обработчике событий
  драйвера; вызывающий не должен одновременно выполнять scan/connect (в приложении это исключено
  claim coprocessor-manager на время обновления). Ждущий scan не будится — он истечёт по таймауту.
- MAC перечитывается в `mac_addr`, но link-адрес `net_if` не обновляется (тот же чип — тот же MAC).
- Патч сделан против драйвера **в текущем состоянии общего дерева**, которое уже содержит
  незакоммиченные правки тех же файлов из другой работы (до патча: 6 файлов драйвера,
  425 вставок / 29 удалений против HEAD `b6a5e6e8aa9`). На чистом upstream он может не наложиться.

## Проверка

- `git -C zephyr apply --check` — чисто (4 файла).
- Применён к общему дереву **2026-09-14 10:07:44 +0300** одной записью (`west patch -sm cedar_p6 -b
  patches -l patches/apply-one.yml -dm zephyr apply`, временный список удалён): «1 patches applied
  successfully». `west patch list` по полному `patches.yml` проходит схему.
- `git -C zephyr status --short` — те же строки до и после. `git -C zephyr diff --stat`: всего
  70 файлов, 2718/202 → 2925/214; файлы драйвера 6 файлов 425/29 → 632/41; строки остальных файлов
  не изменились.
- Сборка worktree `cedar_p6` (sysbuild, scratchpad `build-patch`) на пропатченном дереве: успешно,
  FLASH 1 357 516 Б (32,39 %; базовая 1 357 072 — +444 Б: вынесенная `wifi_bringup` и `spi_flush_rx`
  в таблице транспорта; сами `esp_hosted_mcu_restart`/`wifi_restart` без вызова выбрасываются
  `--gc-sections`), RAM 84 600 Б, PSRAM 2 512 496 Б; `zephyr.signed.bin` sha256 `f586fd6f…faa7d`.
  Новых предупреждений в файлах драйвера нет (`esp_hosted_mcu_ap_authmode` unused — было и до патча).
- Вызов: копия приложения в scratchpad (`p6-callcheck`, исходники worktree не менялись) с вызовом обеих
  функций из `main()` за volatile-флагом — компилируется и линкуется: в `zephyr.elf`
  `T esp_hosted_mcu_restart`, `T esp_hosted_mcu_wifi_restart`. (Первый вариант за
  `IS_ENABLED(CONFIG_DEBUG)` компилятор выбросил — связывание он не доказывал.)
- **На железе не проверялось**: сборка, чистое наложение, без изменения поведения, пока API не вызван.
