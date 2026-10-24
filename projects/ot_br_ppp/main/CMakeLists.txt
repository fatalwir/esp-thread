idf_component_register(SRCS "esp_ot_br.c"
                            "ppp_modem.c"
                            "br_settings.c"
                            "console_capture.c"
                            "web_server.c"
                       INCLUDE_DIRS "."
                       EMBED_TXTFILES "web/index.html")

# Copy everything printed to the console into console_capture.c's ring
# buffer for the web console. --wrap also rewrites references from ESP-IDF
# components and prebuilt libraries, e.g. the logger's default &vprintf.
target_link_libraries(${COMPONENT_LIB} INTERFACE
                      "-Wl,--wrap=printf"
                      "-Wl,--wrap=vprintf"
                      "-Wl,--wrap=puts"
                      "-Wl,--wrap=putchar")
