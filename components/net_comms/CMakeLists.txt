## net_comms — P5 WiFi APSTA, captive portal, mDNS, HTTP server, WebSocket engine sink.
## Registers as an engine sink: streams DataPoints as binary WS frames + JSON events.
## Feeds real SSID/IP/URL back to ui_tft QR code (wires P4 WiFi-info stub).

idf_component_register(
    SRCS "src/net_comms.c"
    INCLUDE_DIRS "include"
    REQUIRES
        echem_core
        acq_engine
        ui_tft
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        joltwallet__littlefs
        espressif__mdns
        json
        lwip
        freertos
        log
        esp_common
)
