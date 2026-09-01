#pragma once

/*
 * focuser_handler
 * ================
 * Unica fuente de verdad del ESTADO logico del focuser (posicion,
 * is_moving, temperatura, tempcomp) y unico traductor entre "posicion
 * absoluta pedida por un transporte" y "comando relativo en
 * motor_cmd_queue".
 *
 * Por que existe este modulo en vez de que cada transporte (websocket,
 * futuro Alpaca HTTP) escriba directo a variables sueltas o a la queue:
 *
 *   - Vas a tener MAS DE UN transporte de red escribiendo/leyendo el
 *     mismo estado al mismo tiempo. Sin un punto unico de coordinacion
 *     con mutex, eso es una condicion de carrera esperando a pasar.
 *
 *   - motor_task es la UNICA fuente real de "cuantos micropasos se
 *     ejecutaron de verdad" (por halt, por lo que sea). focuser_handler
 *     nunca asume que un movimiento pedido se completo integro: espera
 *     a que motor_task se lo reporte explicitamente.
 *
 *   - Igual que con el bus I2C compartido entre AS5600 y AHT21B: este
 *     modulo no crea motor_cmd_queue, solo recibe el handle ya creado
 *     por app_main. No conoce a motor_task ni a ws_server; ellos si lo
 *     conocen a el.
 *
 * Threading:
 *   - Los getters/setters de estado "vivo" (posicion, is_moving, temp,
 *     tempcomp) usan un mutex FreeRTOS. Se pueden llamar desde cualquier
 *     task (motor_task, i2c_sensors_task, ws_server callbacks, futura
 *     alpaca_http_task) sin coordinacion adicional.
 *   - El flag de halt usa una variable atomica SEPARADA del mutex a
 *     proposito: motor_task lo consulta dentro del bucle de pulsos de
 *     move_steps(), potencialmente miles de veces por segundo. Tomar un
 *     mutex ahi meteria latencia variable justo en el codigo mas
 *     sensible a timing del firmware.
 */

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "motor_cmd.h"

/* ---- Configuracion de inicializacion ----
 * Todos los campos de config quedan inmutables despues de
 * focuser_handler_init(): no se protegen con mutex porque nunca
 * cambian en caliente. Si algun dia max_step necesita ser configurable
 * en runtime, se vuelve parte del estado protegido, no de esta config. */
typedef struct {
    QueueHandle_t motor_cmd_queue;   /* ya creada en app_main, no la crea este modulo */
    int32_t max_step;                /* limite superior de Position, en microsteps */
    double  step_size_um;            /* micrometros por microstep -- StepSize de Alpaca */
    bool    tempcomp_available;      /* si el firmware soporta compensacion termica */
} focuser_handler_config_t;

esp_err_t focuser_handler_init(const focuser_handler_config_t *cfg);

/* ======================= Escrituras desde motor_task =======================
 * motor_task es la UNICA task que debe llamar a estas dos funciones, y
 * solo despues de que tmc2209_move_steps() retorne (ver nota sobre el
 * retorno de micropasos ejecutados). */

/* executed_usteps_signed: lo que tmc2209_move_steps() reporto como
 * REALMENTE ejecutado, YA CON SIGNO (positivo = abrir/dir=0, negativo =
 * cerrar/dir=1). Puede tener menor magnitud que lo pedido si hubo halt a
 * medio camino. No se recibe 'dir' por separado a proposito: el signo YA
 * es la direccion, pasar ambos por separado solo abre la puerta a que
 * algun llamador futuro los pase inconsistentes entre si. */
void focuser_handler_report_move_done(int32_t executed_usteps_signed);

void focuser_handler_set_moving(bool moving);

/* ======================= Escrituras desde i2c_sensors_task ======================= */
void focuser_handler_set_temperature(float temp_c);

/* ======================= Lecturas (cualquier transporte) ======================= */
int64_t focuser_handler_get_position(void);
bool    focuser_handler_get_is_moving(void);
float   focuser_handler_get_temperature(void);
bool    focuser_handler_get_tempcomp(void);
void    focuser_handler_set_tempcomp(bool enabled);
int32_t focuser_handler_get_max_step(void);
double  focuser_handler_get_step_size(void);
bool    focuser_handler_get_tempcomp_available(void);

/* ======================= Comando de movimiento (Absoluto -> Relativo) =======================
 *
 * target_position_usteps es ABSOLUTO (mismo contrato que Focuser.Position
 * de Alpaca). La funcion:
 *   1. Valida rango contra max_step.
 *   2. Calcula delta = target - posicion_actual.
 *   3. Si delta == 0, retorna ESP_OK sin encolar nada.
 *   4. Arma un motor_cmd_t RELATIVO y lo encola en motor_cmd_queue.
 *   5. Marca is_moving=true de inmediato (antes de que motor_task saque
 *      el comando de la queue), para que un cliente que consulte
 *      IsMoving justo despues de Move() ya lo vea en true.
 *
 * No bloquea esperando a que el movimiento fisico termine -- eso se
 * refleja despues via focuser_handler_get_is_moving() /
 * focuser_handler_get_position(), actualizados por motor_task cuando
 * el movimiento realmente concluye. */
/* Convencion de dir (fijada por tmc2209_move_steps()): dir=0 abre
 * (posicion aumenta), dir=1 cierra (posicion disminuye). */
esp_err_t focuser_handler_move_to(int64_t target_position_usteps, TickType_t queue_timeout);

/* ======================= Halt =======================
 * NO toca motor_cmd_queue -- Halt es fuera de banda respecto al
 * contrato normal de comandos, porque motor_task puede estar bloqueada
 * generando pulsos (no haciendo xQueueReceive) cuando llega.
 *
 * Flujo esperado dentro de move_steps()/tmc2209_move_steps():
 *   - Consultar focuser_handler_halt_requested() en cada iteracion del
 *     bucle de pulsos (lectura atomica, costo despreciable).
 *   - Al detectarlo, iniciar rampa de frenado en vez de cortar en seco.
 *   - Al terminar de frenar, llamar a focuser_handler_halt_consume().
 */
void focuser_handler_request_halt(void);
bool focuser_handler_halt_requested(void);
void focuser_handler_halt_consume(void);
