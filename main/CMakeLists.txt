idf_component_register(
    SRCS "main.c"
         "app_ui.c"
         "app_config.c"
         "lan_config.c"
         "power_link.c"
         "power_metrics.c"
         "provisioning.c"
         "web_form.c"
         "ui_font_14.c"
         "ui_font_20.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_app_format esp_event esp_http_client esp_http_server esp_hw_support
             esp_netif esp_timer esp-tls esp_wifi json nvs_flash
)
