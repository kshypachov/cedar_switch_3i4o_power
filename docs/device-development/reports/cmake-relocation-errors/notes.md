# «Cannot find source file: …PSRAM_DATA_BSS_NOINIT:COPY» при конфигурации в CLion

2026-09-18.

## Симптом

При перезагрузке CMake-проекта в CLion конфигурация приложения печатает десятки
`CMake Error in CMakeLists.txt: Cannot find source file:` с «путями» вида
`<проект>/PSRAM_DATA_BSS_NOINIT:COPY`, `<проект>/NOKEEP:<файл>.c`,
`<файл>.c,` и `RAM_TEXT:COPY`, после чего всё равно пишет
`Build files have been written to …`.

## Причина

1. `zephyr_code_relocate()` (zephyr/cmake/modules/extensions.cmake) хранит директивы
   переноса строками `LOCATION:FLAGS:FILES,FILTER` в свойстве `INTERFACE_SOURCES`
   пользовательской цели `code_data_relocation_target`
   (cmake/modules/kernel.cmake, `add_custom_target`). Оттуда их читает
   cmake/linker/ld/target_relocation.cmake для gen_relocate_app.py. Это не файлы.
2. CLion 2026.2.2 (обновился 2026-09-16) идёт со своим CMake 4.3.1 и кладёт в
   build-каталог запрос File API `codemodel-v2`.
3. Начиная с CMake 4.3, при ответе на `codemodel-v2` CMake разбирает
   `INTERFACE_SOURCES` и у пользовательских целей как исходники — и не находит их.

Проверено минимальным проектом (цель `add_custom_target` + строка в
`INTERFACE_SOURCES` + пустой файл запроса `.cmake/api/v1/query/codemodel-v2`):

| CMake | без запроса | с запросом codemodel-v2 |
|---|---|---|
| 3.31.6, 4.0.3, 4.1.2, 4.2.3 | чисто | чисто |
| 4.3.1 (CLion), 4.4.2 (Homebrew) | чисто | 3 ошибки «Cannot find source file» |

## Влияние

Сообщения безвредны: код возврата cmake 0, `build.ninja` генерируется полностью
(`ninja -n` проходит до `zephyr.elf`), шаг gen_relocate_app на месте — директивы
передаются ему через `$<TARGET_PROPERTY:…,INTERFACE_SOURCES>` как раньше.
Страдает только модель проекта CLion для этой служебной цели. `west build` из
терминала запроса File API не создаёт и ошибок не печатает.

## Варианты

- Не обращать внимания (ничего не ломается).
- В CLion указать CMake ≤ 4.2 (Settings → Build → Toolchains → CMake).
- Патч Zephyr: хранить директивы в собственном свойстве (например
  `ZEPHYR_CODE_RELOCATION`) вместо `INTERFACE_SOURCES` — правка extensions.cmake,
  ld/target_relocation.cmake и arcmwdt/target.cmake; кандидат в upstream-issue.
  Не сделан.

## Установлено

CMake 4.2.3 (из wheel `cmake==4.2.3` с PyPI) скопирован в
`/Users/kiro/Applications/cmake-4.2.3/bin/cmake`. Проверено 2026-09-18: sysbuild-конфигурация
проекта (`west build --cmake-only`, плата rev3/ext_flash_app) с запросом `codemodel-v2` —
код возврата 0, ни одного «Cannot find source file», ответ File API сформирован.

## Вторая ошибка той же сборки: `zap-cli` — «Bad CPU type in executable»

После пересоздания `build/` генерация ZAP (`modules/lib/matter/scripts/tools/zap/generate.py`)
падает с `OSError: [Errno 86] Bad CPU type in executable: 'zap-cli'`. Используется
`modules/lib/matter/.environment/cipd/packages/zap/zap-cli` (v2026.05.12-nightly) — это
x86_64: Matter нарочно ставит на macOS версию amd64 (`scripts/setup/zap.json`, «until usable
arm64 zap build is available»). Rosetta 2 на машине нет (`arch -x86_64 /usr/bin/true` →
Bad CPU type), система на macOS 27.0. Раньше Rosetta была (есть её кеш /var/db/oah), но
`/Library/Apple/usr/libexec/oah` переписан 2026-09-03 13:34 (дата установки macOS 27): в нём
остался только `RosettaLinux` (для Linux-ВМ), рантайма Rosetta для macOS и пакета-квитанции
нет — обновление системы её сняло. Раньше это не проявлялось, пока сгенерированные ZAP-файлы
лежали в старом `build/`. Лечение: `softwareupdate --install-rosetta --agree-to-license`
(либо arm64 zap-cli из релизов project-chip/zap через `ZAP_INSTALL_PATH`).
