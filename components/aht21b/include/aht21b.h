/*
 * ============================================================================
 *  aht21b.h - Driver del sensor de temperatura/humedad AHT21B (I2C)
 * ============================================================================
 *
 * MODULO AUTOCONTENIDO: solo depende de headers de ESP-IDF. No incluye ni
 * conoce tmc2209.h ni as5600.h. Este driver no sabe que hay un motor ni
 * otro sensor en el sistema; solo sabe hablar con UN AHT21B sobre un
 * bus I2C que YA EXISTE.
 *
 * NOTA DE BUS COMPARTIDO: a diferencia de una version anterior de este
 * driver, aht21b_init() ya NO crea su propio bus I2C (no llama a
 * i2c_new_master_bus). El AHT21B en este proyecto comparte fisicamente
 * SDA/SCL con el AS5600, asi que el bus se crea UNA sola vez en el
 * orquestador (i2c_sensors_task() en main.c) y se pasa aqui ya
 * inicializado via cfg->bus. Este modulo solo hace
 * i2c_master_bus_add_device() con su propia direccion (0x38) y su propia
 * velocidad -- eso si es por-dispositivo y puede diferir de otros
 * dispositivos en el mismo bus.
 *
 * Uso tipico (ver i2c_sensors_task() en main.c para el caso real):
 *
 *   i2c_master_bus_handle_t shared_bus = ...; // creado una vez por main.c
 *
 *   aht21b_config_t cfg = {
 *       .bus = shared_bus,
 *       .i2c_freq_hz = 100000,
 *   };
 *   aht21b_t sensor;
 *   aht21b_init(&sensor, &cfg);
 *   aht21b_ensure_calibrated(&sensor);   // solo hace falta una vez al arrancar
 *
 *   float temp_c, hum_pct;
 *   aht21b_read(&sensor, &temp_c, &hum_pct);
 *
 * El handle (aht21b_t) es un struct transparente: el usuario lo declara y
 * pasa su direccion a cada funcion. No hay malloc en este driver.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Configuracion de UN AHT21B sobre un bus I2C ya existente. */
typedef struct {
i2c_master_bus_handle_t bus;         /* bus ya creado; dueño es el orquestador (main.c) */
uint32_t                i2c_freq_hz; /* velocidad SCL para ESTE dispositivo */
} aht21b_config_t;

/* Handle del driver: cfg + el handle de dispositivo que crea
 * aht21b_init() sobre el bus recibido, mas una bandera "ready" que el
 * llamador puede usar para saltarse lecturas si la inicializacion o la
 * deteccion fallaron. Ya no guarda un handle de bus propio: el bus es
 * responsabilidad de quien lo creo (ver cfg.bus). */
typedef struct {
aht21b_config_t         cfg;
i2c_master_dev_handle_t dev;
bool                    ready;
} aht21b_t;

/* Registra el dispositivo AHT21B (direccion fija 0x38) sobre el bus I2C
 * ya existente en cfg->bus. NO crea el bus, NO hace ninguna lectura de
 * prueba ni verifica calibracion: eso es aht21b_ensure_calibrated(). No
 * toca dev->ready. Falla con ESP_ERR_INVALID_ARG si cfg->bus es NULL. */
esp_err_t aht21b_init(aht21b_t *dev, const aht21b_config_t *cfg);

/* Verifica el bit de calibracion; si no esta calibrado (tipico justo tras
 * power-on), envia la secuencia de inicializacion del sensor y reverifica.
 * Llamar una vez al arrancar, no en cada lectura. Tambien sirve como
 * "probe" de deteccion: si el sensor no esta en el bus, esto falla. */
esp_err_t aht21b_ensure_calibrated(aht21b_t *dev);

/* Secuencia completa de una lectura: dispara la medicion, espera la
 * conversion del ADC interno (~100ms) y devuelve temperatura (°C) y
 * humedad relativa (%RH) ya convertidas. Bloqueante por ese tiempo. */
esp_err_t aht21b_read(aht21b_t *dev, float *out_temp_c, float *out_hum_pct);

#ifdef __cplusplus
}
#endif