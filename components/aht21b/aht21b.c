/*
 * ============================================================================
 *  aht21b.c - Implementacion del driver AHT21B
 * ============================================================================
 */

#include <string.h>
#include "aht21b.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "AHT21B";

/* ---- Direccion y comandos del AHT21B (detalle interno, no en el .h) ---- */
#define AHT21_I2C_ADDR              0x38

#define AHT21_CMD_TRIGGER           0xAC   /* + 0x33, 0x00 */
#define AHT21_CMD_INIT              0xBE   /* + 0x08, 0x00 (solo si no esta calibrado) */

#define AHT21_STATUS_BUSY_BIT       0x80
#define AHT21_STATUS_CALIBRATED_BIT 0x08

#define AHT21_CONVERSION_DELAY_MS   100    /* 80ms minimo de datasheet + margen */

/* Lee el byte de STATUS (bit7=busy, bit3=calibrado). A diferencia de un
 * register-read clasico, esto NO escribe direccion de registro antes: el
 * AHT21B siempre devuelve STATUS como primer byte de cualquier lectura. */
static esp_err_t aht21_read_status(aht21b_t *dev, uint8_t *out_status)
{
    ESP_RETURN_ON_FALSE(dev != NULL && out_status != NULL, ESP_ERR_INVALID_ARG, TAG,
                         "aht21_read_status: argumento NULL");

    uint8_t rx = 0;
    ESP_RETURN_ON_ERROR(i2c_master_receive(dev->dev, &rx, 1, pdMS_TO_TICKS(100)), TAG,
                         "Fallo leyendo status");

    *out_status = rx;
    return ESP_OK;
}

/* Dispara una medicion (secuencia 0xAC 0x33 0x00). Solo envia el comando:
 * no espera la conversion ni lee el resultado, eso lo hace aht21b_read(). */
static esp_err_t aht21_trigger_measurement(aht21b_t *dev)
{
    uint8_t cmd[3] = { AHT21_CMD_TRIGGER, 0x33, 0x00 };
    ESP_RETURN_ON_ERROR(i2c_master_transmit(dev->dev, cmd, sizeof(cmd), pdMS_TO_TICKS(100)), TAG,
                         "Fallo enviando trigger de medicion");
    return ESP_OK;
}

esp_err_t aht21b_init(aht21b_t *dev, const aht21b_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(dev != NULL && cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "aht21b_init: argumento NULL");
    ESP_RETURN_ON_FALSE(cfg->bus != NULL, ESP_ERR_INVALID_ARG, TAG,
                         "aht21b_init: bus I2C compartido es NULL (debe crearlo el orquestador)");

    memset(dev, 0, sizeof(*dev));
    dev->cfg = *cfg;

    /* Dispositivo sobre el bus I2C YA EXISTENTE, compartido con el
     * AS5600. Este modulo no crea bus ni guarda un handle de bus propio:
     * solo se registra como dispositivo en cfg->bus. */
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AHT21_I2C_ADDR,
        .scl_speed_hz = cfg->i2c_freq_hz,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(cfg->bus, &dev_config, &dev->dev), TAG,
                         "No se pudo agregar el dispositivo I2C");

    return ESP_OK;
}

esp_err_t aht21b_ensure_calibrated(aht21b_t *dev)
{
    ESP_RETURN_ON_FALSE(dev != NULL, ESP_ERR_INVALID_ARG, TAG, "aht21b_ensure_calibrated: dev es NULL");

    uint8_t status = 0;
    ESP_RETURN_ON_ERROR(aht21_read_status(dev, &status), TAG, "No se pudo leer status inicial");

    if (status & AHT21_STATUS_CALIBRATED_BIT) {
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Sensor no calibrado (status=0x%02X), enviando secuencia de init...", status);

    uint8_t cmd[3] = { AHT21_CMD_INIT, 0x08, 0x00 };
    ESP_RETURN_ON_ERROR(i2c_master_transmit(dev->dev, cmd, sizeof(cmd), pdMS_TO_TICKS(100)), TAG,
                         "Fallo enviando init");

    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_RETURN_ON_ERROR(aht21_read_status(dev, &status), TAG, "No se pudo releer status tras init");

    return (status & AHT21_STATUS_CALIBRATED_BIT) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

/*
 * Formato de los 7 bytes de respuesta (status + 5 de datos + 1 de CRC8):
 *   rx[0]        = status (bit7=busy, bit3=calibrado)
 *   rx[1..2]     = humedad, bits altos (20 bits totales entre rx[1..3])
 *   rx[3]        = nibble alto -> fin de humedad; nibble bajo -> inicio de temp
 *   rx[4..5]     = temperatura, bits bajos
 *   rx[6]        = CRC8 de rx[0..5] (no se valida aqui, ver nota en el .h)
 *
 * humedad_raw (20 bits) = rx[1]<<12 | rx[2]<<4 | rx[3]>>4
 * temp_raw    (20 bits) = (rx[3]&0x0F)<<16 | rx[4]<<8 | rx[5]
 * %RH = humedad_raw / 2^20 * 100
 * °C  = temp_raw    / 2^20 * 200 - 50
 *
 * Nota CRC: el AHT21B SI envia un septimo byte de CRC8 (polinomio
 * x^8+x^5+x^4+1, poly 0x31, init 0xFF), distinto al del TMC2209. Por ahora
 * se descarta sin validar -- si aparecen lecturas erraticas que otro
 * sensor no tiene, vale la pena implementar la verificacion antes de
 * sospechar de otra cosa.
 */
esp_err_t aht21b_read(aht21b_t *dev, float *out_temp_c, float *out_hum_pct)
{
    ESP_RETURN_ON_FALSE(dev != NULL && out_temp_c != NULL && out_hum_pct != NULL,
                         ESP_ERR_INVALID_ARG, TAG, "aht21b_read: argumento NULL");

    ESP_RETURN_ON_ERROR(aht21_trigger_measurement(dev), TAG, "No se pudo disparar medicion");

    vTaskDelay(pdMS_TO_TICKS(AHT21_CONVERSION_DELAY_MS));

    uint8_t rx[7] = { 0 };
    ESP_RETURN_ON_ERROR(i2c_master_receive(dev->dev, rx, sizeof(rx), pdMS_TO_TICKS(100)), TAG,
                         "Fallo leyendo datos");

    if (rx[0] & AHT21_STATUS_BUSY_BIT) {
        ESP_LOGW(TAG, "Sensor aun ocupado (busy=1) al leer; dato descartado.");
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t raw_hum  = ((uint32_t)rx[1] << 12) | ((uint32_t)rx[2] << 4) | (rx[3] >> 4);
    uint32_t raw_temp = (((uint32_t)rx[3] & 0x0F) << 16) | ((uint32_t)rx[4] << 8) | rx[5];

    *out_hum_pct = (raw_hum  * 100.0f) / 1048576.0f;        /* /2^20 */
    *out_temp_c  = ((raw_temp * 200.0f) / 1048576.0f) - 50.0f;

    return ESP_OK;
}
