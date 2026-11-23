idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES esp32_s3_touch_amoled_1_8 usb)

# Smoke 3 : probe USB Host (compile avec:  idf.py -DSMOKE_USB_HOST=1 build)
if(SMOKE_USB_HOST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SMOKE_USB_HOST=1)
endif()