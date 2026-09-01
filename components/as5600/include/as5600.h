/*
 * ============================================================================
 *  as5600.h - Driver del encoder magnetico AS5600 (I2C)
 * ============================================================================
 *
 * MODULO AUTOCONTENIDO: solo depende de headers de ESP-IDF. No incluye ni
 * conoce tmc2209.h ni aht21b.h. Este driver no sabe que hay un motor ni
 * otro sensor en el sistema; solo sabe hablar con UN AS5600 sobre un
 * bus I2C que YA EXISTE.
 *
 * NOTA DE BUS COMPARTIDO: a diferencia de una version anterior de este
 * driver, as5600_init() ya NO crea su propio bus I2C (no llama a
 * i2c_new_master_bus). El AS5600 en este proyecto comparte fisicamente
 * SDA/SCL con el AHT21B, asi que el bus se crea UNA sola vez en el
 * orquestador (i2c_sensors_task() en main.c) y se pasa aqui ya
 * inicializado via cfg->bus. Este modulo solo hace
 * i2c_master_bus_add_device() con su propia direccion (0x36) y su propia
 * velocidad -- eso si es por-dispositivo y puede diferir de otros
 * dispositivos en el mismo bus.
 *
 * NOTA SOBRE DIR: el pin DIR del AS5600 (sentido de conteo del angulo) es
 * una entrada CMOS que se fija por hardware, no por I2C. En este proyecto
 * quedo cableado directo a GND de forma permanente, asi que este driver
 * ya NO lo configura ni lo toca por software -- no hay gpio_config() para
 * el. Si el sentido resultante no es el esperado por la mecanica del
 * sistema, se corrige en software restando el angulo leido de 360.0 en el
 * codigo que llama a as5600_read_angle_deg(), no aqui.
 *
 * Uso tipico (ver i2c_sensors_task() en main.c para el caso real):
 *
 *   i2c_master_bus_handle_t shared_bus = ...; // creado una vez por main.c
 *
 *   as5600_config_t cfg = {
 *       .bus = shared_bus, .i2c_freq_hz = 400000,
 *   };
 *   as5600_t enc;
 *   as5600_init(&enc, &cfg);
 *
 *   uint8_t status;
 *   as5600_read_status(&enc, &status);
 *
 *   float degrees;
 *   as5600_read_angle_deg(&enc, &degrees);
 *
 * El handle (as5600_t) es un struct transparente: el usuario lo declara y
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

/* Configuracion de UN AS5600 sobre un bus I2C ya existente. */
typedef struct {
i2c_master_bus_handle_t bus;         /* bus ya creado; dueño es el orquestador (main.c) */
uint32_t                i2c_freq_hz; /* velocidad SCL para ESTE dispositivo */
} as5600_config_t;

/* Handle del driver: cfg + el handle de dispositivo que crea
 * as5600_init() sobre el bus recibido, mas una bandera "ready" que el
 * llamador puede usar para saltarse lecturas si la inicializacion o la
 * deteccion fallaron. Ya no guarda un handle de bus propio: el bus es
 * responsabilidad de quien lo creo (ver cfg.bus). */
typedef struct {
as5600_config_t         cfg;
i2c_master_dev_handle_t dev;
bool                    ready;
} as5600_t;

/* Registra el dispositivo AS5600 (direccion fija 0x36) sobre el bus I2C
 * ya existente en cfg->bus. NO crea el bus, NO configura pin DIR (ver
 * nota de archivo -- queda fijo por hardware), NO hace ninguna lectura de
 * prueba: eso es responsabilidad del llamador (ver as5600_read_status()
 * como "probe" minimo). No toca dev->ready. Falla con ESP_ERR_INVALID_ARG
 * si cfg->bus es NULL. */
esp_err_t as5600_init(as5600_t *dev, const as5600_config_t *cfg);

/* Lee el registro STATUS (deteccion/fuerza del iman). Timeout de 100ms:
 * pensado para convivir con otras tasks sin confundir contencion de CPU
 * con un fallo real de bus. */
esp_err_t as5600_read_status(as5600_t *dev, uint8_t *out_status);

/* Lee el angulo crudo (12 bits, 0-4095) del registro RAW ANGLE. */
esp_err_t as5600_read_raw_angle(as5600_t *dev, uint16_t *out_raw);

/* Conveniencia: como as5600_read_raw_angle() pero ya convertido a grados
 * (0.0 - 360.0). */
esp_err_t as5600_read_angle_deg(as5600_t *dev, float *out_degrees);

/* Helpers para interpretar el byte de STATUS sin exponer los bits crudos
 * en el codigo que llama al driver. */
bool as5600_magnet_detected(uint8_t status);
bool as5600_magnet_too_weak(uint8_t status);
bool as5600_magnet_too_strong(uint8_t status);

#ifdef __cplusplus
}
#endif