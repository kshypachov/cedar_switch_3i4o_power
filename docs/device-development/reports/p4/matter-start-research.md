# Запуск Matter до появления сети — исследование P4

Вопрос владельца: обязательно ли запускать Matter только после старта сети, или его
можно запустить сразу — сокеты откроются без ошибок, а трафик пойдёт, когда появится
сеть?

Метод: чтение кода (`modules/lib/matter` — только чтение, Zephyr, приложение) и
наблюдения P4 на плате B. На плате эксперимент не ставился. Где вывод — умозаключение,
это сказано.

## Короткий ответ

Сокеты — да: Matter стартует без сети без ошибок, и операционный трафик пойдёт, когда
появится адрес. **Обнаружение (DNS-SD/mDNS) — нет**: minimal mDNS выбирает интерфейсы
один раз при запуске, и если в этот момент у W5500 нет IPv6-адреса, устройство не
анонсирует себя, пока что-то не перезапустит DNS-SD. Сам по себе ранний старт даёт
«тихое» устройство с логом, который выглядит нормально. Ранний старт возможен, если
приложение само перезапускает DNS-SD при появлении адреса (одно событие стеку, без
правок вендорского Matter).

## Как сейчас (проверено в коде)

- `main.c` → `matter_service_chip_init()`; `ethernet_interfaces_init()` регистрирует
  `start_matter` на `NET_EVENT_IPV6_ADDR_ADD` (`src/lib-init/net_init.c`) и поднимает
  W5500.
- `start_matter` (`src/matter/matter_init.cpp`) один раз (атомарная защита) запускает
  поток `matter_boot_thread`: `MemoryInit`, `InitChipStack`, DAC, кластеры,
  `Server::Init`, `StartEventLoopTask`.
- Почему ждать IPv6 — нигде не записано. Событие приходит при добавлении адреса, ещё
  до завершения DAD (`zephyr/subsys/net/ip/net_if.c`), то есть и сейчас Matter стартует
  с tentative link-local.
- Следствие текущей схемы: плата, включённая без кабеля, не запускает Matter вовсе,
  пока кабель не вставят.

## Транспорт (проверено)

- Сборка на сокетах; IPv4 и TCP-эндпоинт в CHIP выключены (`args.gn`:
  `chip_inet_config_enable_ipv4 = false`, `chip_inet_config_enable_tcp_endpoint = false`).
- Единственный слушатель — UDP IPv6 на `::`, порт 5540, без интерфейса
  (`src/app/server/Server.cpp`). Bind на `::` в Zephyr не требует адреса: интерфейс
  выбирается как default (`net_context.c`, `net_if_ipv6_select_src_iface`), ошибки нет.
- Приём не привязан к интерфейсу; отправка выбирает интерфейс по адресату и без link
  возвращает `-ENETDOWN` — ошибка одной отправки, стек её только логирует.
- `InitChipStack` требует settings, энтропию и PSA, но не сеть.

## Minimal mDNS — препятствие (проверено)

- `DnssdServer::StartServer` пересоздаёт сокеты анонсёра. Список интерфейсов — только
  поднятые (`IsUp()`) и с IPv6-адресом
  (`src/lib/dnssd/minimal_mdns/AddressPolicy_DefaultImpl.cpp`). На старте W5500 без
  carrier — список пуст.
- При пустом списке `Listen` не открывает сокетов, возвращает успех, в логе всё равно
  «CHIP minimal mDNS started advertising.»; `kDnssdInitialized` не публикуется, поэтому
  нет «Server initialization complete» и `kServerReady`.
- Перезапуск DNS-SD: только по `kDnssdInitialized`/`kDnssdRestartNeeded`
  (`src/app/server/Dnssd.cpp`), открытию/закрытию окна commissioning, ICD. На Zephyr
  `kDnssdRestartNeeded` публикует лишь `platform/Zephyr/wifi/WiFiManager.cpp`, который в
  эту сборку не входит; `ConnectivityManagerImpl` для Zephyr не подписан на `net_mgmt`.
- Адреса в ответах читаются в момент ответа — поэтому в P4 новый SLAAC-адрес был виден
  без перезапуска Matter (`reports/p4/hw`, лог `22`). Зафиксирован только набор
  интерфейсов.
- Тот же пробел уже есть для Wi-Fi: интерфейс, поднявшийся после старта Matter, mDNS не
  получает.

## Риски раннего старта

| Риск | Статус |
|---|---|
| DNS-SD молчит после перезагрузки коммиссионированного устройства | проверено в коде |
| Отправки до link — `ENETDOWN` в логе | проверено в коде, некритично |
| DAD | не препятствие — сейчас старт тоже до DAD (вывод) |
| Сокет на `::` привязан к default-интерфейсу на момент bind (например, Wi-Fi) | отправка выбирает интерфейс заново — вероятно безвредно (вывод) |
| Ложный link up W5500 каждые ~2,9 с без кабеля (плата B) | перезапуск DNS-SD по событию интерфейса сработает так же часто — нужен debounce |
| Первое вступление в `ff02::fb` без MLD-отчёта | важно только для коммутаторов с MLD snooping (вывод) |

## Что понадобится для раннего старта

1. Запускать Matter в `main()` без ожидания адреса.
2. В приложении (не в `modules/lib/matter`): на `NET_EVENT_IPV6_ADDR_ADD` (и, возможно,
   `NET_EVENT_IF_UP`), с debounce и только после старта event loop, публиковать
   `DeviceEventType::kDnssdRestartNeeded` через `PlatformMgr().ScheduleWork`/`PostEvent` —
   так же, как делает `WiFiManager`. Это же закрывает пробел с Wi-Fi.

Это область Matter (P3) и файлы `src/matter/*`, `src/lib-init/net_init.c`; в P4 не
менялось.

## Эксперимент на плате B (не выполнялся)

1. Ранний старт, кабель вынут → включить → вставить кабель: ожидается «CHIP minimal mDNS
   started advertising.» без «Server initialization complete»; `dns-sd -B _matterc._udp`
   и `_matter._tcp` — платы нет.
2. Контроль: открыть окно через API — `StartServer` вызывается, запись появляется.
3. С перехватом `kDnssdRestartNeeded`: запись появляется через 1–2 с после адреса, без
   «MDNS failed to join multicast group».
4. `net iface` — `ff02::fb` на интерфейсе W5500.
5. Коммиссионированный узел, перезагрузка с поздним кабелем — chip-tool достаёт его по
   operational discovery.
6. Кабель вынут — частота перезапусков при ложном link up.
