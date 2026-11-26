idf_component_register(
    SRCS "drydock_usb_host.c" "drydock_usb_probe.c"
    INCLUDE_DIRS "include"
    REQUIRES usb esp_hw_support esp_driver_gpio hal)
