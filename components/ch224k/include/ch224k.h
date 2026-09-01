#pragma once

#include <stdbool.h>
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"

typedef enum {
    CH224K_VOLTAGE_5V,
    CH224K_VOLTAGE_9V,
    CH224K_VOLTAGE_12V,
    CH224K_VOLTAGE_15V,
    CH224K_VOLTAGE_20V,
} ch224k_voltage_t;

typedef struct {
    gpio_num_t   pin_cfg1;
    gpio_num_t   pin_cfg2;
    gpio_num_t   pin_cfg3;
    int          pin_pg;       /* -1 si no se usa */
    bool         enable_adc;
    adc_unit_t   adc_unit;
    adc_channel_t adc_channel;
    float        r1_kohm;
    float        r2_kohm;
} ch224k_config_t;

typedef struct {
    ch224k_config_t         config;
    bool                     ready;
    ch224k_voltage_t         current_voltage;
    adc_oneshot_unit_handle_t adc_handle;
} ch224k_t;

esp_err_t ch224k_init(ch224k_t *dev, const ch224k_config_t *cfg);
esp_err_t ch224k_set_voltage(ch224k_t *dev, ch224k_voltage_t voltage);
esp_err_t ch224k_read_voltage(ch224k_t *dev, float *out_volts);
esp_err_t ch224k_read_power_good(ch224k_t *dev, bool *out_pg_ok);
esp_err_t ch224k_deinit(ch224k_t *dev);