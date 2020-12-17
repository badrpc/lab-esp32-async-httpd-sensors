idf_component_register(
    SRCS
        "main.c"
        "connect.c"
        "sas8.c"
    INCLUDE_DIRS "."
    REQUIRES
        "freemodbus"
        "libesphttpd"
        "nvs_flash"
)
