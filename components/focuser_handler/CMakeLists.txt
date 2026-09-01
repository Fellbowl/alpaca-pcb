idf_component_register(
    SRCS "focuser_handler.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos esp_common
)