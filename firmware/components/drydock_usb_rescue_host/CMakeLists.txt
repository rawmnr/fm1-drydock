idf_component_register(
    SRCS "drydock_usb_rescue_host.c"
    INCLUDE_DIRS "include"
    REQUIRES drydock_proto drydock_usb_host usb heap)
