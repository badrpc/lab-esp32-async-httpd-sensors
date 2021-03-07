idf_component_register(
    SRCS
        "main.c"
        "connect.c"
        "sas8.c"
        "bme680.c"
        "bsec_integration.c"
        "generic_33v_300s_28d/bsec_serialized_configurations_iaq.c"
    INCLUDE_DIRS 
        "."
        "bsec"
        "generic_33v_300s_28d"
    REQUIRES
        "freemodbus"
        "libesphttpd"
        "nvs_flash"
        "bme68x_sensor_api"
)

add_prebuilt_library(bsec "bsec/libalgobsec.a")

target_link_libraries(${COMPONENT_LIB} PRIVATE bsec)

# # Import the library, specifying a target name and the library path.
# # The private dependencies of the library is also specified.
# add_prebuilt_library(prebuilt "libprebuilt.a"
#              PRIV_REQUIRES spi_flash app_update log)
# 
# # `main` calls a function from the library, so link it to `main`
# target_link_libraries(${COMPONENT_LIB} PRIVATE prebuilt)
