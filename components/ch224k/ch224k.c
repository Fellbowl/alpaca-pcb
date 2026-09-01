#include "ch224k.h"
#include "esp_log.h"

static const char *TAG = "CH224K";

esp_err_t ch224k_init(ch224k_t *dev, const ch224k_config_t *cfg)
{
    if (dev == NULL || cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    dev->config = *cfg;
    dev->ready = false;
    dev->adc_handle = NULL;

    /* Configurar GPIOs de salida: CFG1, CFG2, CFG3 */
    gpio_config_t io_conf_out = {
        .pin_bit_mask = (1ULL << cfg->pin_cfg1) | (1ULL << cfg->pin_cfg2) | (1ULL << cfg->pin_cfg3),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf_out);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error al configurar pines de salida CFG");
        return err;
    }

    /* Configurar GPIO de entrada: Power Good (PG) con pull-up */
    if (cfg->pin_pg >= 0) {
        gpio_config_t io_conf_in = {
            .pin_bit_mask = (1ULL << cfg->pin_pg),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        err = gpio_config(&io_conf_in);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error al configurar pin PG");
            return err;
        }
    }

    /* Configurar ADC si está habilitado */
    if (cfg->enable_adc) {
        adc_oneshot_unit_init_cfg_t init_config1 = {
            .unit_id = cfg->adc_unit,
        };
        err = adc_oneshot_new_unit(&init_config1, &dev->adc_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error al inicializar unidad ADC");
            return err;
        }

        adc_oneshot_chan_cfg_t chan_config = {
            .bitwidth = ADC_BITWIDTH_DEFAULT,
            .atten = ADC_ATTEN_DB_12, /* Hasta ~3.3V de entrada al GPIO */
        };
        err = adc_oneshot_config_channel(dev->adc_handle, cfg->adc_channel, &chan_config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error al configurar canal ADC");
            adc_oneshot_del_unit(dev->adc_handle);
            return err;
        }
    }

    /* Perfil por defecto: 5V por seguridad */
    ch224k_set_voltage(dev, CH224K_VOLTAGE_5V);

    dev->ready = true;
    ESP_LOGI(TAG, "Libreria CH224K inicializada correctamente.");
    return ESP_OK;
}

esp_err_t ch224k_set_voltage(ch224k_t *dev, ch224k_voltage_t voltage)
{
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (voltage) {
        case CH224K_VOLTAGE_5V:
            gpio_set_level(dev->config.pin_cfg1, 1);
            gpio_set_level(dev->config.pin_cfg2, 0);
            gpio_set_level(dev->config.pin_cfg3, 0);
            break;
        case CH224K_VOLTAGE_9V:
            gpio_set_level(dev->config.pin_cfg1, 0);
            gpio_set_level(dev->config.pin_cfg2, 0);
            gpio_set_level(dev->config.pin_cfg3, 0);
            break;
        case CH224K_VOLTAGE_12V:
            gpio_set_level(dev->config.pin_cfg1, 0);
            gpio_set_level(dev->config.pin_cfg2, 0);
            gpio_set_level(dev->config.pin_cfg3, 1);
            break;
        case CH224K_VOLTAGE_15V:
            gpio_set_level(dev->config.pin_cfg1, 0);
            gpio_set_level(dev->config.pin_cfg2, 1);
            gpio_set_level(dev->config.pin_cfg3, 1);
            break;
        case CH224K_VOLTAGE_20V:
            gpio_set_level(dev->config.pin_cfg1, 0);
            gpio_set_level(dev->config.pin_cfg2, 1);
            gpio_set_level(dev->config.pin_cfg3, 0);
            break;
        default:
            return ESP_ERR_INVALID_ARG;
    }

    dev->current_voltage = voltage;
    ESP_LOGI(TAG, "Voltaje USB-PD actualizado a perfil %d", (int)voltage);
    return ESP_OK;
}

esp_err_t ch224k_read_voltage(ch224k_t *dev, float *out_volts)
{
    if (dev == NULL || out_volts == NULL || !dev->config.enable_adc || dev->adc_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int raw_val = 0;
    esp_err_t err = adc_oneshot_read(dev->adc_handle, dev->config.adc_channel, &raw_val);
    if (err != ESP_OK) {
        return err;
    }

    /* Voltaje en el GPIO (mV) considerando atenuacion de 12dB (max ~3300mV) */
    float pin_voltage_mv = ((float)raw_val / 4095.0f) * 3300.0f;

    /* Escalado por el divisor de tension R1 / R2 */
    float r1 = dev->config.r1_kohm;
    float r2 = dev->config.r2_kohm;
    *out_volts = (pin_voltage_mv / 1000.0f) * ((r1 + r2) / r2);

    return ESP_OK;
}

esp_err_t ch224k_read_power_good(ch224k_t *dev, bool *out_pg_ok)
{
    if (dev == NULL || out_pg_ok == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (dev->config.pin_pg < 0) {
        *out_pg_ok = true; /* Si no se asigno pin, asumir siempre OK */
        return ESP_OK;
    }

    /* PG activo en bajo (0 = alimentacion entregada correctamente) */
    *out_pg_ok = (gpio_get_level(dev->config.pin_pg) == 0);
    return ESP_OK;
}

esp_err_t ch224k_deinit(ch224k_t *dev)
{
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (dev->adc_handle != NULL) {
        adc_oneshot_del_unit(dev->adc_handle);
        dev->adc_handle = NULL;
    }

    dev->ready = false;
    return ESP_OK;
}