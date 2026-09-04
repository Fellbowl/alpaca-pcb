idf_component_register(
    SRCS "alpaca.c"
    INCLUDE_DIRS "include"
    # alpaca.h expone UBaseType_t/BaseType_t (freertos/FreeRTOS.h) en
    # alpaca_config_t -- por eso "freertos" va en REQUIRES (publico).
    REQUIRES
        freertos
    # esp_http_server y focuser_handler se usan SOLO dentro de alpaca.c --
    # ningun tipo de ninguno de los dos aparece en alpaca.h -- por eso
    # van en PRIV_REQUIRES, no en REQUIRES. cJSON.h NO va en REQUIRES ni
    # PRIV_REQUIRES: se resuelve via idf_component.yml (espressif/cjson),
    # no como componente built-in "json" (ese no existe en ESP-IDF v6).
    #
    # focuser_handler se agrega aqui a partir del Paso 4: alpaca.c ya
    # llama directo a sus getters (focuser_handler_get_position(), etc.)
    # porque focuser_handler es un singleton sin handle, igual que este
    # propio modulo -- no hace falta pasarlo por alpaca_config_t.
    PRIV_REQUIRES
        esp_http_server
        focuser_handler
)