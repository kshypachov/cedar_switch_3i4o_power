zephyr_library_named(settings_registry)

zephyr_library_sources(
	settings_registry.c
	settings_registry_validators.c
)

# Section slot for the SETTING_REGISTRY_DEFINE descriptors (see iterable.ld).
zephyr_linker_sources(DATA_SECTIONS iterable.ld)
