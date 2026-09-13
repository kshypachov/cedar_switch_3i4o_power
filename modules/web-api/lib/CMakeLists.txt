zephyr_library_named(web_api)

zephyr_library_sources(
    web_api.c
    json_reader.c
    json_writer.c
)
zephyr_library_sources_ifdef(CONFIG_WEB_API_HTTP web_api_http.c)
