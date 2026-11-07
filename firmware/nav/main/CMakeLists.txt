idf_component_register(SRCS "main.c" "ble_nav.c" "font_vn_16.c" "font_vn_14.c" "font_vn_12.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_lcd driver fatfs sdmmc esp_timer esp_driver_uart bt nvs_flash
                       EMBED_FILES "streets.bin" "route.bin")
