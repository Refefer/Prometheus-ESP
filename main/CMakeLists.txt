idf_component_register(
    SRCS
        "main.c"
        "waveshare_rgb_lcd_port.c"
        "lvgl_port.c"
        "storage.c"
        "secrets.c"
        "wifi_mgr.c"
        "ui_theme.c"
        "ui_fmt.c"
        "http_util.c"
        "poller.c"
        "config.c"
        "ui_tile.c"
        "ui_tile_kinds.c"
        "font_num_44.c"
        "font_num_64.c"
        "font_num_96.c"
        "ui_widgets.c"
        "ui_kbd.c"
        "ui_setup.c"
        "ui_endpoints.c"
        "ui_browser.c"
        "ui_panelcfg.c"
        "ui_layouts.c"
        "timekeep.c"
        "webcfg.c"
        "sysmon.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver esp_lcd esp_timer
        esp_wifi esp_http_client esp-tls esp_netif esp_event
        nvs_flash json app_update esp_http_server
        lwip
        prom
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE=1)

# lvgl 8.3 trips format warnings on recent GCC
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_compile_options(${lvgl_lib} PRIVATE -Wno-format)
