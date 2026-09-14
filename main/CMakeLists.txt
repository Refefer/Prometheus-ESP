idf_component_register(
    SRCS
        "main.c"
        "waveshare_rgb_lcd_port.c"
        "lvgl_port.c"
        "storage.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver esp_lcd esp_timer
        esp_wifi esp_http_client esp-tls esp_netif
        nvs_flash json app_update
        prom
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE=1)

# lvgl 8.3 trips format warnings on recent GCC
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_compile_options(${lvgl_lib} PRIVATE -Wno-format)
