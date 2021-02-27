set (libesphttpd_PRIV_INCLUDE_DIRS "core"
                                   "util")

idf_component_register(
    SRCS "esp8266_wrapper.c"
    INCLUDE_DIRS "."
)
