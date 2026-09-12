# Board boot hooks for the sysbuild MCUboot image (see ../../sysbuild.cmake).
if(CONFIG_BOOT_GO_HOOKS)
  zephyr_library()
  zephyr_library_sources(boot-hook.c)
  zephyr_library_link_libraries(MCUBOOT_BOOTUTIL)
endif()
