Плата B, 2026-09-15 11:40–11:50. Владелец: «если выбран Ethernet, после перезагрузки Wi-Fi не выбирается, даже если Ethernet не поднялся; если выдернуть Ethernet во время работы, маршрут меняется на Wi-Fi».

Образ `main-p6-merge` (sha256 6a8e2fbb…, main df852ea + рабочее дерево: SRAM, ZMS, PKA), Wi-Fi `k2` настроен владельцем через веб.

## 1. Плата не отвечала: bench MCUboot стоял в BusFault

К 11:41 плата не отвечала:
- консоль молчала;
- ping и HTTP на 192.168.88.13 не проходили;
- USB CDC приложения (`2037394C…`) не перечислялся, остались мост консоли и J-Link.

J-Link `h` + `regs` в 11:48, без сброса:
- **Регистры:** PC 0x08015C0A, LR 0x08015C1B, xPSR 0x29000005 (IPSR 5, BusFault). SP=MSP 0x200102A0, MSPLIM 0x2000FB08. SHCSR 0x00070002 (BusFault active). CFSR 0 и HFSR 0 — уже сброшены обработчиком. MMFAR = BFAR = 0x200102A4.
- **Сохранённый кадр исключения** в 0x200102C8: R0 0x1B, R1 0x0801A445, R3 0x080089AD, LR 0x0800D4ED, PC 0x0800D4EC, xPSR 0x69000000.

Адреса во внутренней флеш-памяти, то есть это не приложение (оно слинковано на 0x02000000). По ELF стендового MCUboot платы B с крупными кадрами recovery (`e74ea288…/scratchpad/build-mcuboot-fast/mcuboot/zephyr/zephyr.elf`, 09-14 16:43, filesz 0x1a6cc; конфиг `reports/p6/hw/bench-mcuboot/mcuboot-cdc-recovery-fast.conf`):
- PC кадра 0x0800D4EC — `boot_configure_alias_9000_to_0200`, `sysbuild/mcuboot_hooks/boot-hook.c:64`, из `boot_go_hook`, строка 214. Это чтение отладочного дампа первых байт `0x90000000` (внешняя флеш через OCTOSPI в режиме memory-mapped) сразу после настройки ICACHE-алиаса.
- Цепочка: `z_arm_bus_fault` → `z_arm_fault` (fault.c:1202) → `z_fatal_error` (fatal.c:146) → останов. PC 0x08015C0A попал в `z_heap_alloc_helper` (kheap.c:56), это место остановленного цикла после фатальной ошибки.

**Вывод:** при той перезагрузке, после которой владелец не увидел Wi-Fi, приложение вообще не стартовало. MCUboot упал на чтении внешней флеш по адресу 0x90000000, то есть флеш в этот момент не была отображена в память. Какой перезагрузкой это вызвано (веб, кнопка, питание), пока неизвестно. Сброс через J-Link (`r`, `g`) в 11:49:50 прошёл нормально.

## 2. Загрузка после сброса J-Link (кабель Ethernet был подключён)

- 11:49:50 — `Starting bootloader`, алиас ICACHE и дамп `0x90000000` в порядке. `Jumping to the first image slot` — 11:49:59, после 5 с ожидания DFU.
- `network configuration revision 1: clean`; `ethernet: DHCP`, затем `wifi: DHCP` на 2,707 с (подключение к Wi-Fi при загрузке принято).
- **Маршрут:** Wi-Fi DHCP 192.168.88.23 на 6,48 с → `default route via wifi`; Ethernet DHCP 192.168.88.13 на 12,02 с → `default route via ethernet`.
- **После старта:** `wifi status` COMPLETED (k2, WPA3-SAE, RSSI −57); `wlan0` 192.168.88.23, DHCP bound; `eth0` 192.168.88.13, по умолчанию интерфейс 1.

При выбранном Ethernet выбор маршрута при загрузке работает: пока Ethernet не получил адрес, маршрут был через Wi-Fi.

## 3. Что осталось проверить

- **Загрузка с выдернутым кабелем Ethernet.** Есть гипотеза по коду, не подтверждена. `push_config()` (`modules/network-manager/lib/network_manager.c:1182–1191`) запускает DHCP на Wi-Fi только если `wifi_connect()` при загрузке вернул 0. А `rejoin()` из `process_policy()` DHCP не запускает (`WIFI_STA_AUTO_DHCPV4=n`). Если C6 откажет в подключении при загрузке, Wi-Fi подключится без IPv4, и маршрут на него не перейдёт. В этой загрузке подключение было принято.
- **Сбой MCUboot на `0x90000000`.** Какая перезагрузка к нему приводит и почему флеш не отображена. Дамп в хуке — отладочный код владельца, не удалять.

## 4. Исправление в network-manager (11:55–12:05)

- **Тест:** `tests/network_manager/src/main.c`, `test_a_wifi_join_refused_at_boot_gets_an_address_on_rejoin`. Сохранена конфигурация с Wi-Fi, перезагрузка с выдернутым Ethernet, `wifi_connect` при загрузке возвращает `-ENODEV`, через 6 с повторное подключение проходит. Тест проверяет, что у Wi-Fi есть IPv4 и маршрут по умолчанию через Wi-Fi.
- **До исправления** тест падал: `net.wifi.configured is false` (main.c:2017; 108 из 109 прошли), в логе `configuring the interfaces from revision 1 failed (-19)`.
- **Правка:** `rejoin()` в `modules/network-manager/lib/network_manager.c` после успешного `wifi_connect()` вызывает `configure(WIFI, ipv4, true)`, так же как `push_config()`. Адаптер хранит применённую конфигурацию, поэтому повторное подключение уже сконфигурированного радио ничего не меняет.
- **После:** `cedar.network_manager` и `cedar.network_service` проходят (twister, native_sim/native/64, контейнер `cedar-sim-tests:1`). Прошивка `main-wifi-rejoin` собрана (sha256 6240e385…).
- **На плате проверить:** загрузку с выдернутым Ethernet. Ожидаемый лог: `configuring … failed (-19)` или успешное `wifi: DHCP`, затем `Wi-Fi rejoin requested (0)`, `net_dhcpv4: Received` и `default route via wifi`.
