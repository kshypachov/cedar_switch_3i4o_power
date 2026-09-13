#include <zephyr/linker/iterable_sections.h>

/* The resources of the one HTTP service, src/web/web_server.c. */
ITERABLE_SECTION_ROM(http_resource_desc_http_service, Z_LINK_ITERABLE_SUBALIGN)
