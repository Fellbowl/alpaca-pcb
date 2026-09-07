/*
 * ============================================================================
 *  tmc2209.h - Driver del stepper driver TMC2209 (control por UART/PDN_UART)
 * ============================================================================
 *
 * MODULO AUTOCONTENIDO: solo depende de headers de ESP-IDF/FreeRTOS. No
 * incluye ni conoce as5600.h ni aht21b.h -- este driver no sabe que existen
 * sensores I2C en el sistema, y no le hace falta saberlo.
 *
 * Tampoco conoce a focuser_handler.h -- el soporte de "halt" (Paso 5) se
 * resuelve con un CALLBACK opcional (halt_check_fn en tmc2209_config_t),
 * no con un include directo. Este modulo solo sabe que existe "una funcion
 * que le dice si debe frenar"; quien esta detras de esa funcion es
 * responsabilidad del orquestador (main.c), igual que ya pasa con
 * ws_server_config_t.on_message o motor_cmd_queue.
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
 *       .halt_check_fn = focuser_handler_halt_requested,  // opcional, Paso 5
 *   };
 *   tmc2209_t motor;
 *   tmc2209_init(&motor, &cfg);             // GPIO + UART, sin tocar el driver todavia
 *   tmc2209_configure(&motor);              // escribe GCONF/IHOLD_IRUN/CHOPCONF
 *   tmc2209_verify_chopconf(&motor);        // confirma que quedo en 1/256
 *   tmc2209_enable(&motor, true);           // EN=0 (activo en bajo)
 *   tmc2209_move_steps(&motor, 2000, 1);    // mueve con rampa (y frena si halt_check_fn lo pide)
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

/* Callback opcional que tmc2209_move_steps() consulta una vez por
 * micropaso (salvo durante su propia rampa de frenado final natural, ya
 * en deceleracion de todas formas) para saber si debe iniciar un frenado
 * anticipado. Debe ser rapido (se llama potencialmente miles de veces
 * por segundo) y no debe bloquear -- una lectura atomica es lo esperado,
 * no un mutex.
 *
 * NULL en tmc2209_config_t.halt_check_fn = sin soporte de halt,
 * comportamiento identico al de antes de que existiera este campo. */
typedef bool (*tmc2209_halt_check_fn_t)(void);

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
    tmc2209_halt_check_fn_t halt_check_fn; /* opcional (NULL = sin soporte de halt), ver arriba */
} tmc2209_config_t;

/* Handle del driver: cfg + estado derivado (retardos posteriores al pulso
 * STEP, ya precalculados por tmc2209_init() a partir de rev_per_sec_*).
 * Los nombres half_period_* se conservan por compatibilidad con la API
 * existente, aunque representan el retardo restante tras el pulso STEP. */
typedef struct {
    tmc2209_config_t cfg;
    uint32_t half_period_start_us;
    uint32_t half_period_cruise_us;
} tmc2209_t;

/* Inicializa GPIOs (MS1/MS2/STEP/DIR/EN) y el driver UART, y precalcula los
 * intervalos entre pasos de la rampa. NO toca todavia los registros del TMC2209 (eso
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

/* Genera hasta n micropasos con rampa lineal (arranque/crucero/frenado),
 * manipulando los pines STEP/DIR directamente. Bloqueante: cede la CPU
 * periodicamente (vTaskDelay) para no disparar el task watchdog, pero el
 * llamador es responsable de correr esto en una task que pueda permitirse
 * bloquear por la duracion del movimiento.
 *
 * Si cfg.halt_check_fn esta seteado y en algun momento retorna true
 * (fuera de la rampa de frenado final natural, que ya esta decelerando
 * de todas formas), el movimiento ABANDONA el plan original de n pasos e
 * inicia su propia rampa de frenado -- mismo perfil lineal que ya usa el
 * arranque/crucero, pero calculado desde la velocidad ACTUAL hacia atras,
 * para no cortar en seco. En ese caso el valor de retorno (con signo)
 * reflejara MENOS micropasos que los n originalmente pedidos: el
 * llamador NUNCA debe asumir que el retorno == n, use siempre lo que
 * esta funcion reporta como la verdad de lo ejecutado. */
int32_t tmc2209_move_steps(tmc2209_t *drv, uint32_t n, int dir_level);

#ifdef __cplusplus
}
#endif