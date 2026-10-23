idf_component_register(
    SRCS
        "main.c"
        "net_bringup.c"
        "net_9100.c"
        "usb_printer.c"
        "printer_id.c"
        "welcome.c"
        "raster_zpl.c"
        "ipp_server.c"
        "c6_update.c"
        "portal.c"
        "portal_dns.c"
        "web_ui.c"
        "log_tap.c"
        "app_ota.c"
        "app_restart.c"
        "net_sock.c"
        "wifilabel_nvs.c"
    INCLUDE_DIRS "."
    EMBED_FILES "../c6/eh_cp_wifi_sta.bin"
    REQUIRES
        nvs_flash
        esp_netif
        esp_event
        esp_timer
        esp_eth
        esp_wifi
        esp_http_server
        usb
        lwip
        mdns
        app_update
        espressif__esp_hosted
        label_toolkit
)

# Shared secret for POST /update. The file is gitignored. An empty or missing
# file closes the endpoint.
set(WIFILABEL_OTA_TOKEN_FILE "${CMAKE_SOURCE_DIR}/.ota-token")
set(WIFILABEL_OTA_TOKEN "")
if(EXISTS "${WIFILABEL_OTA_TOKEN_FILE}")
    file(READ "${WIFILABEL_OTA_TOKEN_FILE}" WIFILABEL_OTA_TOKEN)
    string(STRIP "${WIFILABEL_OTA_TOKEN}" WIFILABEL_OTA_TOKEN)
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${WIFILABEL_OTA_TOKEN_FILE}")
target_compile_definitions(${COMPONENT_LIB} PRIVATE WIFILABEL_OTA_TOKEN=\"${WIFILABEL_OTA_TOKEN}\")
