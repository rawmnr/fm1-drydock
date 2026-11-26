idf_component_register(
    SRCS "drydock_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES drydock_boot drydock_storage drydock_usb_host drydock_usb_midi_host
             drydock_usb_msc_device drydock_usb_rescue_host
             esp32_s3_touch_amoled_1_8 esp_driver_i2c lvgl)
