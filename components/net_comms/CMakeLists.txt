## net_comms skeleton — filled in P5.
## WiFi APSTA + captive portal + mDNS (ubiopot.local) + esp_http_server + WebSocket.
## Registers as an engine sink: streams DataPoints as binary WS frames + JSON events.
## Also feeds real SSID/IP/URL back to ui_tft QR code (wires P4 WiFi-info stub).

idf_component_register(
    SRCS "src/net_comms.c"
    INCLUDE_DIRS "include"
    REQUIRES echem_core acq_engine
)
