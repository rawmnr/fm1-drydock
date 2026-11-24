idf_component_register(
    SRCS "drydock_storage.c"
    INCLUDE_DIRS "include"
    REQUIRES fatfs wear_levelling mbedtls cjson)