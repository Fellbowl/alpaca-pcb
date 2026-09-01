idf_component_register(
    SRCS "aht21b.c"
    INCLUDE_DIRS "include"
    # aht21b.h expone i2c_port_t / i2c_master_bus_handle_t / i2c_master_dev_handle_t
    # (definidos en driver/i2c_master.h) en aht21b_config_t y aht21b_t. En
    # ESP-IDF reciente ese header vive en el componente "esp_driver_i2c",
    # no en "driver" legacy -- por eso va en REQUIRES (publico).
    REQUIRES
        esp_driver_i2c
    # freertos (vTaskDelay/pdMS_TO_TICKS) y log (ESP_LOGx) se usan solo
    # dentro de aht21b.c. El AHT21B no usa ningun GPIO propio (a diferencia
    # del AS5600 con su pin DIR), asi que no necesita esp_driver_gpio.
    PRIV_REQUIRES
        freertos
        log
)
