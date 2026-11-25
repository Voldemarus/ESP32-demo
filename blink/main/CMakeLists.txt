idf_component_register(
    SRCS "blink_example_main.c"
    REQUIRES
        esp_common
        esp_libc        # базовая C-библиотека (заменяет newlib)
        freertos
        log
        esp_wifi
        nvs_flash
        esp_event
        lwip            # явно, так как не является общим
        esp_netif
        esp_driver_gpio
        esp_timer
)