# firmware-store module root. Layout mirrors the other in-tree modules:
#   include/firmware_store/ - public API: the store (firmware_store.h), the pure
#                             full-flash image check (fw_image.h) and MD5 (fw_md5.h)
#   lib/                    - the implementation

zephyr_include_directories(include)

add_subdirectory_ifdef(CONFIG_FIRMWARE_STORE lib)
