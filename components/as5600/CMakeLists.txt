idf_component_register(
    SRCS "as5600.c"
    INCLUDE_DIRS "include"
    # as5600.h expone i2c_port_t / i2c_master_bus_handle_t / i2c_master_dev_handle_t
    # (definidos en driver/i2c_master.h) en as5600_config_t y as5600_t. En
    # ESP-IDF reciente ese header vive en el componente "esp_driver_i2c",
    # no en "driver" legacy -- por eso va en REQUIRES (publico).
    REQUIRES
        esp_driver_i2c
    # esp_driver_gpio (driver/gpio.h, solo para el pin DIR), freertos
    # (vTaskDelay/pdMS_TO_TICKS) y log (ESP_LOGx) se usan solo dentro de
    # as5600.c.
    PRIV_REQUIRES
        esp_driver_gpio
        freertos
        log
)