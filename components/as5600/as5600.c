/*
 * ============================================================================
 *  as5600.c - Implementacion del driver AS5600
 * ============================================================================
 */

#include <string.h>
#include "as5600.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
static const char *TAG = "AS5600";

/* ---- Direccion y registros del AS5600 (detalle interno, no en el .h) ---- */
#define AS5600_I2C_ADDR         0x36

#define AS5600_REG_STATUS       0x0B
#define AS5600_REG_RAW_ANGLE_H  0x0C  /* 0x0C:0x0D, 12 bits, angulo sin filtrar */

#define AS5600_STATUS_MD_BIT    0x20  /* magnet detected */
#define AS5600_STATUS_ML_BIT    0x10  /* magnet too weak */
#define AS5600_STATUS_MH_BIT    0x08  /* magnet too strong */

esp_err_t as5600_init(as5600_t *dev, const as5600_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(dev != NULL && cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "as5600_init: argumento NULL");
    ESP_RETURN_ON_FALSE(cfg->bus != NULL, ESP_ERR_INVALID_ARG, TAG,
                         "as5600_init: bus I2C compartido es NULL (debe crearlo el orquestador)");

    memset(dev, 0, sizeof(*dev));
    dev->cfg = *cfg;

    /* Dispositivo sobre el bus I2C YA EXISTENTE, compartido con el
     * AHT21B. Sin gpio_config para DIR: quedo fijo a GND por hardware. */
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AS5600_I2C_ADDR,
        .scl_speed_hz = cfg->i2c_freq_hz,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(cfg->bus, &dev_config, &dev->dev), TAG,
                         "No se pudo agregar el dispositivo I2C");

    return ESP_OK;
}

esp_err_t as5600_read_status(as5600_t *dev, uint8_t *out_status)
{
    ESP_RETURN_ON_FALSE(dev != NULL && out_status != NULL, ESP_ERR_INVALID_ARG, TAG,
                         "as5600_read_status: argumento NULL");

    uint8_t reg_buf[1] = { AS5600_REG_STATUS };
    uint8_t rx_buf[1] = { 0 };

    ESP_RETURN_ON_ERROR(
        i2c_master_transmit_receive(dev->dev, reg_buf, sizeof(reg_buf), rx_buf, sizeof(rx_buf), pdMS_TO_TICKS(100)),
        TAG, "Fallo leyendo STATUS");

    *out_status = rx_buf[0];
    return ESP_OK;
}

esp_err_t as5600_read_raw_angle(as5600_t *dev, uint16_t *out_raw)
{
    ESP_RETURN_ON_FALSE(dev != NULL && out_raw != NULL, ESP_ERR_INVALID_ARG, TAG,
                         "as5600_read_raw_angle: argumento NULL");

    uint8_t reg_buf[1] = { AS5600_REG_RAW_ANGLE_H };
    uint8_t rx_buf[2] = { 0 };

    ESP_RETURN_ON_ERROR(
        i2c_master_transmit_receive(dev->dev, reg_buf, sizeof(reg_buf), rx_buf, sizeof(rx_buf), pdMS_TO_TICKS(100)),
        TAG, "Fallo leyendo RAW ANGLE");

    *out_raw = (uint16_t)(((rx_buf[0] << 8) | rx_buf[1]) & 0x0FFF); /* 12 bits utiles */
    return ESP_OK;
}

esp_err_t as5600_read_angle_deg(as5600_t *dev, float *out_degrees)
{
    ESP_RETURN_ON_FALSE(out_degrees != NULL, ESP_ERR_INVALID_ARG, TAG,
                         "as5600_read_angle_deg: out_degrees es NULL");

    uint16_t raw = 0;
    esp_err_t err = as5600_read_raw_angle(dev, &raw);
    if (err != ESP_OK) {
        return err;
    }

    *out_degrees = (raw * 360.0f) / 4096.0f;
    return ESP_OK;
}

bool as5600_magnet_detected(uint8_t status)
{
    return (status & AS5600_STATUS_MD_BIT) != 0;
}

bool as5600_magnet_too_weak(uint8_t status)
{
    return (status & AS5600_STATUS_ML_BIT) != 0;
}

bool as5600_magnet_too_strong(uint8_t status)
{
    return (status & AS5600_STATUS_MH_BIT) != 0;
}
