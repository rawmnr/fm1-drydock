idf_component_register(
    SRCS "drydock_storage.c"
    INCLUDE_DIRS "include"
    REQUIRES esp32_s3_touch_amoled_1_8 mbedtls cjson)