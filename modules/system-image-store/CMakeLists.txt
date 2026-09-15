# system-image-store module root. Layout mirrors the other in-tree modules:
#   include/system_image_store/ - public API: the store (system_image_store.h) and
#                                 the pure MCUboot image check (mcuboot_image.h)
#   lib/                        - the implementation

zephyr_include_directories(include)

add_subdirectory_ifdef(CONFIG_SYSTEM_IMAGE_STORE lib)
