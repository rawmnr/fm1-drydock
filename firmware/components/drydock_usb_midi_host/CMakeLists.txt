idf_component_register(
    SRCS "drydock_usb_midi_host.c"
    INCLUDE_DIRS "include"
    REQUIRES drydock_proto drydock_usb_host usb heap)
