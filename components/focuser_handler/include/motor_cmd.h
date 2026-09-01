#pragma once

#include <stdint.h>

/*
 * Contrato entre "quien pide un movimiento" y "quien lo ejecuta".
 *
 * Vive en su propio header -- no dentro de tmc2209.h ni de
 * focuser_handler.h -- porque ninguno de los dos es su dueno. Es el tipo
 * que viaja por motor_cmd_queue entre CUALQUIER fuente de comandos
 * (consola, preset_cmd_task, websocket, y ahora focuser_handler para
 * Alpaca) y motor_task. Todos los productores y motor_task incluyen este
 * header; no se incluyen entre si.
 *
 * IMPORTANTE: steps es un desplazamiento RELATIVO, nunca una posicion
 * absoluta. La traduccion de "posicion absoluta pedida" a "pasos
 * relativos + direccion" es responsabilidad de quien arma el comando
 * (p.ej. focuser_handler_move_to()), no de motor_task.
 */
typedef struct {
    uint32_t steps;   /* micropasos relativos a mover */
    int dir;          /* 0 o 1, mismo sentido que ya usa move_steps()/tmc2209_move_steps() */
} motor_cmd_t;