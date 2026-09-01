idf_component_register(
    SRCS "ch224k.c"
    INCLUDE_DIRS "include"
    # tmc2209.h expone uart_port_t (definido en driver/uart.h) en su propia
    # firma publica (tmc2209_config_t.uart_port). En ESP-IDF reciente ese
    # header ya no vive en el componente "driver" legacy, sino en
    # "esp_driver_uart" -- por eso va aqui, en REQUIRES (dependencia
    # publica: quien incluya tmc2209.h necesita ese tipo resuelto tambien).
    REQUIRES
        esp_adc
        esp_driver_gpio
    # freertos (vTaskDelay), esp_rom (esp_rom_delay_us) y log (ESP_LOGx)
    # se usan solo dentro de ch224k.c, asi que son dependencias privadas.
    PRIV_REQUIRES
        freertos
        esp_rom
        log
)