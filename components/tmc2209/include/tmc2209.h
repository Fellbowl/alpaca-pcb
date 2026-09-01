/*
 * ============================================================================
 *  tmc2209.h - Driver del stepper driver TMC2209 (control por UART/PDN_UART)
 * ============================================================================
 *
 * MODULO AUTOCONTENIDO: solo depende de headers de ESP-IDF/FreeRTOS. No
 * incluye ni conoce as5600.h ni aht21b.h -- este driver no sabe que existen
 * sensores I2C en el sistema, y no le hace falta saberlo.
 *
 * Uso tipico (ver motor_task() en main.c para el caso real):
 *
 *   tmc2209_config_t cfg = {
 *       .uart_port = UART_NUM_1,
 *       .pin_uart_rx = 7, .pin_uart_tx = 8,
 *       .pin_step = 4, .pin_dir = 5, .pin_en = 6,
 *       .pin_ms1 = 1, .pin_ms2 = 2,
 *       .driver_addr = 0,
 *       .ihold = 6, .irun = 24, .iholddelay = 1,
 *       .mres = 0,                          // 0 = 1/256 microstepping
 *       .rev_per_sec_start = 0.05,
 *       .rev_per_sec_cruise = 0.30,
 *   };
 *   tmc2209_t motor;
 *   tmc2209_init(&motor, &cfg);             // GPIO + UART, sin tocar el driver todavia
 *   tmc2209_configure(&motor);              // escribe GCONF/IHOLD_IRUN/CHOPCONF
 *   tmc2209_verify_chopconf(&motor);        // confirma que quedo en 1/256
 *   tmc2209_enable(&motor, true);           // EN=0 (activo en bajo)
 *   tmc2209_move_steps(&motor, 2000, 1);    // mueve con rampa
 *
 * El handle (tmc2209_t) es un struct transparente, no un puntero opaco: es
 * el mismo estilo que ya se usaba en main.c para as5600_ctx_t/aht21_ctx_t.
 * El usuario lo declara (en stack o estatico) y pasa su direccion; no hay
 * malloc en ningun lado de este driver.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/uart.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Configuracion de pines/parametros de UN TMC2209. Se pasa una sola vez a
 * tmc2209_init(); a partir de ahi vive copiada dentro del handle. */
typedef struct {
    uart_port_t uart_port;     /* puerto UART dedicado, propiedad exclusiva de este driver */
    int pin_uart_rx;
    int pin_uart_tx;
    int pin_step;
    int pin_dir;
    int pin_en;                 /* activo en bajo */
    int pin_ms1;
    int pin_ms2;
    uint8_t driver_addr;        /* direccion UART del TMC2209 (0 si PDN_UART sin direccionar) */
    uint8_t ihold;
    uint8_t irun;
    uint8_t iholddelay;
    uint8_t mres;                /* 0 = 1/256 microstepping (resolucion FIJA soportada hoy) */
    double  rev_per_sec_start;   /* velocidad de arranque de la rampa trapezoidal */
    double  rev_per_sec_cruise;  /* velocidad de crucero de la rampa trapezoidal */
} tmc2209_config_t;

/* Handle del driver: cfg + estado derivado (semiperiodos de la rampa, ya
 * precalculados por tmc2209_init() a partir de rev_per_sec_*). */
typedef struct {
    tmc2209_config_t cfg;
    uint32_t half_period_start_us;
    uint32_t half_period_cruise_us;
} tmc2209_t;

/* Inicializa GPIOs (MS1/MS2/STEP/DIR/EN) y el driver UART, y precalcula los
 * semiperiodos de la rampa. NO toca todavia los registros del TMC2209 (eso
 * lo hace tmc2209_configure()) ni lo habilita (EN queda en 1 = deshabilitado). */
esp_err_t tmc2209_init(tmc2209_t *drv, const tmc2209_config_t *cfg);

/* Escribe GCONF (stealthChop), IHOLD_IRUN y CHOPCONF (microstepping fijo
 * segun cfg.mres). No retorna esp_err_t: un eco UART incompleto durante la
 * escritura no prueba que esta haya fallado (puede ser ruido en el camino
 * de vuelta); la verificacion real es tmc2209_verify_chopconf(). */
void tmc2209_configure(tmc2209_t *drv);

/* Lee CHOPCONF de vuelta y confirma que el campo MRES coincide con
 * cfg.mres. Esta es la unica verificacion fuerte de que tmc2209_configure()
 * realmente surtio efecto en el driver fisico. */
esp_err_t tmc2209_verify_chopconf(tmc2209_t *drv);

/* Lee y loguea GSTAT/DRV_STATUS/MSCNT. Si GSTAT indica reset, reconfigura
 * el driver automaticamente (llama a tmc2209_configure() internamente). */
void tmc2209_check_status(tmc2209_t *drv, const char *when);

/* Habilita (EN=0) o deshabilita (EN=1) el driver. Activo en bajo. */
void tmc2209_enable(tmc2209_t *drv, bool enable);

/* Lee el contador de interfaz UART (IFCNT) del driver -- util como "ping"
 * minimo para confirmar comunicacion antes de configurar nada. */
esp_err_t tmc2209_read_ifcnt(tmc2209_t *drv, uint32_t *out_value);

/* Genera n micropasos con rampa lineal (arranque/crucero/frenado),
 * manipulando los pines STEP/DIR directamente. Bloqueante: cede la CPU
 * periodicamente (vTaskDelay) para no disparar el task watchdog, pero el
 * llamador es responsable de correr esto en una task que pueda permitirse
 * bloquear por la duracion del movimiento. */
int32_t tmc2209_move_steps(tmc2209_t *drv, uint32_t n, int dir_level);

#ifdef __cplusplus
}
#endif
