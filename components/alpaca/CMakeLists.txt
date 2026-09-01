idf_component_register(
    SRCS "alpaca.c"
    INCLUDE_DIRS "include"
    # alpaca.h expone UBaseType_t/BaseType_t (freertos/FreeRTOS.h) en
    # alpaca_config_t -- por eso "freertos" va en REQUIRES (publico).
    REQUIRES
        esp_driver_i2c
    # esp_http_server (httpd_handle_t, httpd_start, etc.) y json (cJSON.h,
    # para armar las respuestas de /management) se usan SOLO dentro de
    # alpaca.c, ningun tipo de ninguno de los dos aparece en alpaca.h --
    # por eso van en PRIV_REQUIRES, no en REQUIRES.
    PRIV_REQUIRES
        esp_http_server
)