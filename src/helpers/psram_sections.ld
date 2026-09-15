/*
 * libweb_auth.a сюда намеренно не входит: стек потока вычисления ключа
 * (web_auth_psa.c) должен быть в SRAM, а не в PSRAM, данные модуля — 2,3 КБ.
 *
 * libCHIP.a и libtfpsacrypto.a сюда тоже не входят (2026-09-15, по решению владельца):
 * их .bss/.noinit (стек потока Matter sChipThreadStack, пул PacketBuffer, объекты сервера,
 * состояние PSA/mbedTLS, ~190 КБ) — горячие при сопряжении и остаются в SRAM
 * (reports/p3/tuning/sram).
 *
 * .bss/.noinit библиотек, которые gen_relocate_app.py не находит (settings_registry и
 * модули приложения из modules/: их архивы лежат не в каталоге app) или путает по
 * одинаковому имени файла (esp_hosted_mcu), в PSRAM. .data у них нет. Фрагмент включается
 * в snippets-sections.ld — раньше выходных секций .bss и .noinit, поэтому эти входные
 * секции попадают сюда.
 * Обнуление [__psram_ext_bss_start, __psram_ext_bss_end) — psram_ext_bss_zero()
 * в src/helpers/memory.c (PRE_KERNEL_1, до конструкторов C++).
 * Секция идёт в регионе PSRAM последней: __psram_used_end — начало внешней кучи.
 */
SECTION_PROLOGUE(.psram_ext_bss,(NOLOAD),)
{
	. = ALIGN(4);
	__psram_ext_bss_start = .;
	*libsettings_registry.a:*(.bss .bss.* COMMON)
	*libesp_hosted_mcu.a:*(.bss .bss.* COMMON)
	*libesp_hosted_mcu_core.a:*(.bss .bss.* COMMON)
	*libjob_manager.a:*(.bss .bss.* COMMON)
	*libdevice_config_store.a:*(.bss .bss.* COMMON)
	*libapi_validation.a:*(.bss .bss.* COMMON)
	*libnetwork_manager.a:*(.bss .bss.* COMMON)
	*libweb_api.a:*(.bss .bss.* COMMON)
	*libweb_assets.a:*(.bss .bss.* COMMON)
	*libmatter_service.a:*(.bss .bss.* COMMON)
	*liblog_store.a:*(.bss .bss.* COMMON)
	*libzephyr_log_source.a:*(.bss .bss.* COMMON)
	*libesp32_log_source.a:*(.bss .bss.* COMMON)
	*libcoprocessor_manager.a:*(.bss .bss.* COMMON)
	*libfirmware_store.a:*(.bss .bss.* COMMON)
	*libesp_loader_adapter.a:*(.bss .bss.* COMMON)
	*libcoprocessor_updater.a:*(.bss .bss.* COMMON)
	*libsystem_image_store.a:*(.bss .bss.* COMMON)
	*libsystem_updater.a:*(.bss .bss.* COMMON)
	. = ALIGN(4);
	__psram_ext_bss_end = .;
	*libsettings_registry.a:*(.noinit .noinit.*)
	*libesp_hosted_mcu.a:*(.noinit .noinit.*)
	*libesp_hosted_mcu_core.a:*(.noinit .noinit.*)
	*libjob_manager.a:*(.noinit .noinit.*)
	*libdevice_config_store.a:*(.noinit .noinit.*)
	*libapi_validation.a:*(.noinit .noinit.*)
	*libnetwork_manager.a:*(.noinit .noinit.*)
	*libweb_api.a:*(.noinit .noinit.*)
	*libweb_assets.a:*(.noinit .noinit.*)
	*libmatter_service.a:*(.noinit .noinit.*)
	*liblog_store.a:*(.noinit .noinit.*)
	*libzephyr_log_source.a:*(.noinit .noinit.*)
	*libesp32_log_source.a:*(.noinit .noinit.*)
	*libcoprocessor_manager.a:*(.noinit .noinit.*)
	*libfirmware_store.a:*(.noinit .noinit.*)
	*libesp_loader_adapter.a:*(.noinit .noinit.*)
	*libcoprocessor_updater.a:*(.noinit .noinit.*)
	*libsystem_image_store.a:*(.noinit .noinit.*)
	*libsystem_updater.a:*(.noinit .noinit.*)
	. = ALIGN(4);
	__psram_used_end = .;
} GROUP_LINK_IN(PSRAM)
