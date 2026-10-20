idf_component_register(
    SRCS
        "main.c"
        "net_9100.c"
        "usb_printer.c"
        "label_proto.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_netif
        esp_event
        esp_timer
        usb
        lwip
)
