# psa-driver-stm32 module root.
#   include/        — stm32_pka.h: P-256 primitives on the PKA block
#   src/            — PKA bring-up and primitives, shell (KAT, bench)
# Design, measurements and the change log: docs/device-development/reports/crypto/pka/README.md

if(CONFIG_STM32_PKA)
    zephyr_include_directories(include)
    zephyr_library_named(psa_driver_stm32)
    zephyr_library_sources(src/stm32_pka.c)
    zephyr_library_sources_ifdef(CONFIG_STM32_PKA_SHELL src/stm32_pka_shell.c)
    zephyr_library_sources_ifdef(CONFIG_PSA_CRYPTO_DRIVER_STM32_PKA src/stm32_pka_psa.c)
    if(CONFIG_STM32_PKA_ECP_P256 AND CONFIG_STM32_PKA_SHELL)
        zephyr_library_sources(src/stm32_pka_ecp_check.c)  # 'pka ecp check': mbedtls_ecp_* PKA vs software
        zephyr_library_link_libraries(mbedTLS)
    endif()
endif()
