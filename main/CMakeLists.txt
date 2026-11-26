idf_component_register(
    SRCS "bluetooth.c" "audio_aux.c" "pantalla.c" "spotify.c" "configuracion.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_adc bt nvs_flash esp_driver_dac esp_driver_gpio esp_driver_spi
                  esp_driver_uart esp_wifi esp_event esp_netif esp_http_client
                  esp_timer mbedtls freertos log
)
