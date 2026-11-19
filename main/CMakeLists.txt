idf_component_register(
    SRCS "main.c" "settings.c" "jk_bms.c" "energy.c" "control.c" "net.c" "telegram.c" "commands.c" "web.c" "ota.c"
         "diag.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_uart esp_driver_gpio esp_wifi esp_netif esp_event nvs_flash
                  esp_http_client esp_http_server mbedtls json esp_timer
                  app_update esp_app_format bootloader_support hal soc
)

# Дамп паники пишется в UART0 через uart_hal_write_txfifo — перехватываем, чтобы сохранить в RTC-память (diag.c)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=uart_hal_write_txfifo")
