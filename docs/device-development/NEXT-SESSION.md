Продолжаем разработку веб-интерфейса Cedar. Рабочий каталог —
/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power

Сначала прочти:
- docs/device-development/development-plan.md — план, этапы P0–P8, раздел 12 про тесты
- docs/device-development/api-contract.md — поведение API, состояния, идемпотентность
- docs/device-development/openapi.json — 35 операций, 48 схем
- modules/job-manager/README.md — первый готовый модуль, образец для остальных
- tests/ci/README.md — как запускать sim-тесты

Память проекта подхватится сама, там детали по плате, PSRAM, прошивке C6 и
истории с W5500.

## Что уже сделано

P0 частично: подключён esp-serial-flasher (v2.0.0, submanifest, проверен на
плате), прочитана реальная таблица разделов C6, измерено место в /lfs (4992 КиБ
свободно, расширять не нужно), удалена запись frontend submodule, разобран
отказ приложения при старте (оказался устаревшим артефактом в build/), найден и
устранён мультикаст-флуд, душивший систему.

P1 начат: модуль modules/job-manager — жизненный цикл и идемпотентность
длительных операций. 17 тестов, все проходят, набор проверен тремя мутациями.
Модуль зарегистрирован в CMakeLists.txt и включён в prj.conf.

Появилась инфраструктура sim-тестов: tests/ci/ (Dockerfile + run-sim-tests.sh).

## Первое, что нужно сделать

Закоммитить. Предыдущий коммит захватил не всё — не отслеживаются modules/,
tests/, docs/device-development/, docs/upstream/. Проверь git status и собери
осмысленные коммиты. build/ должен попасть в .gitignore, а не в индекс.

Отдельно: в дереве zephyr (чужой репозиторий, /Volumes/Programming/Zephyr/
zephyr_latest/zephyr) лежит наш патч drivers/ethernet/eth_w5500.c +
eth_w5500_priv.h — включение бита MMB. Его снесёт следующий west update.
Нужно сохранить его как патч-файл в репозитории приложения и описать в
release manifest. Черновик запроса в апстрим уже есть в docs/upstream/.

## Дальше по P1

Следующие модули, оба чистая логика и ложатся в существующий каркас тестов:

1. device-config-store — версии схем, атомарные snapshot, pending/committed
   поколения, хранение секретов. Основа для сетевых транзакций из раздела 5
   плана: candidate → apply → confirm → commit, с откатом по таймауту.
2. Валидация и коды ошибок из api-contract.md — единые helpers для разбора и
   отказов, чтобы обработчики маршрутов не плодили свои форматы.

Порядок такой: сначала публичный заголовок с обоснованием решений, потом
реализация, потом тесты — всё в одном коммите. Так требует раздел 12.

## Как запускать тесты

Sim-уровень (Linux-контейнер, потому что native_sim не собирается на macOS):

    colima start --cpu 4 --memory 8 --disk 60 --mount /Volumes/Programming:w
    tests/ci/run-sim-tests.sh                      # весь набор
    tests/ci/run-sim-tests.sh -s cedar.job_manager # одна сюита

Не пытайся собрать native_sim напрямую на macOS — POSIX-архитектура Zephyr
работает только под Linux. qemu_cortex_m3 собирается, но прогон висит.

Сборка прошивки:

    cd /Volumes/Programming/Zephyr/zephyr_latest
    .venv/bin/west build -b cedar_switch_3in4out_power_rev3/stm32u585xx/ext_flash_app \
      --sysbuild -d <build-dir> cedar_switch_3in4out_power

Не используй каталог build/ в репозитории: CLion перегенерирует его
параллельно, и получаются испорченные артефакты — на этом уже потерян час.
Бери отдельный каталог в scratchpad.

## Работа с платой

Плата подключена и исправна. ST-LINK V3SET, серийник
002F002B3233510739363634. Консоль /dev/cu.usbmodem11303, 115200.
Проводной интерфейс хоста к плате — en7 (192.168.88.17), Wi-Fi en0 не
используй для IPv6. Плата — 192.168.88.14, fe80::8234:28ff:fe10:1273.

Прошивка только приложения, MCUboot уже во внутренней флеш и его трогать не
надо:

    CLI="/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/Resources/bin/STM32_Programmer_CLI"
    EL=$PWD/CLIVEONE-W25Q128_STM32U545-PB10-PA4-PB1-PB0-PA7-PA6.stldr
    "$CLI" -c port=swd sn=002F002B3233510739363634 mode=NORMAL -el "$EL" \
       -w <build>/cedar_switch_3in4out_power/zephyr/zephyr.signed.bin 0x90000000 -v
    "$CLI" -c port=swd sn=002F002B3233510739363634 mode=NORMAL -hardRst

Именно mode=NORMAL: UR и HOTPLUG отваливаются с "Unable to get core ID".
Захват консоли переживает пропадание VCP при сбросе:

    .venv/bin/python docs/qspi-psram-driver/test-infra/capture.py /dev/cu.usbmodem11303 <файл> <секунд>

Если порт занят — это CLion держит его своим serial monitor, проверь
lsof /dev/tty.usbmodem11303 и попроси закрыть.

Реле на PA5, PC4, PC5, PB2, все active-low. Каждый сброс платы их
переинициализирует, владелец это слышит — лишних перезагрузок не делай.

## Что известно и не надо перепроверять

Поток W5500 создаётся как K_PRIO_COOP(2) и внутри while(int_pin) делает
синхронный опросный SPI, не уступая процессор. Любой достаточный поток в
сегменте снова задушит логи и шелл. Симптом снят блокировкой IPv4-мультикаста,
причина осталась — в апстриме это issue #115626, где уже пишут, что потоки не
должны быть кооперативными. Разумнее дождаться апстрима, чем чинить самим.

Биты Sn_MR охарактеризованы на железе двумя методами: MMB режет только
IPv4-мультикаст, MIP6B — только IPv6-мультикаст. MIP6B на этом изделии
применять нельзя: ломает Neighbor Discovery и SLAAC, а Matter живёт на
IPv6-мультикасте.

## Открытые вопросы, которые всплыли и ждут решения

- HTTP-сервер слушает только IPv4 (0.0.0.0:80, нет [::]:80) — решить, нужен ли
  доступ по IPv6, это вопрос к P2
- DNS-SD advertising выключен, для Matter commissioning понадобится
- settings_registry при старте пишет "0 keys registered" — выглядит
  подозрительно, не разбирались
- нумерация выводов реле: владелец называет 21/24/25/28, в DTS PA5/PC4/PC5/PB2,
  расхождение на единицу — сверить со схемой
- в P0 осталось: чистый sysbuild отдельно от CLion, 20 холодных/тёплых стартов,
  состояние rollback в CP firmware

## Как работать

Не полагайся на описания в даташитах и документации без проверки — за эту
сессию несколько раз оказывалось, что документ говорит одно, а железо делает
другое. Если вывод важен, измеряй.

Перед тем как чинить, убедись что дефект настоящий: один «баг» оказался
устаревшим бинарником, одна «поломка» — голоданием потока, а не зависанием.
