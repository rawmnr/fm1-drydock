idf_component_register(
    SRCS "app_main.c" "boot_mode.c"
    INCLUDE_DIRS "."
    REQUIRES esp32_s3_touch_amoled_1_8 drydock_storage drydock_usb_msc_device drydock_usb_host
             drydock_usb_midi_host drydock_usb_rescue_host esp_driver_i2c esp_hw_support
             esp_psram esp_system spi_flash usb console)
