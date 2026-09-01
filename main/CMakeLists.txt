idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    # main.c incluye tmc2209.h, as5600.h y aht21b.h y llama a sus API
    # publicas -- por eso van en REQUIRES (dependencia visible), no en
    # PRIV_REQUIRES.
    REQUIRES
        tmc2209
        as5600
        aht21b
        ch224k
        wifi_init
        ws_server
        focuser_handler
        alpaca

    # freertos (QueueHandle_t/TaskHandle_t/vTaskDelay) y log (ESP_LOGx) se
    # usan solo dentro de main.c, nadie mas depende de main.c, asi que van
    # como privados.
    PRIV_REQUIRES
        freertos
        log
)