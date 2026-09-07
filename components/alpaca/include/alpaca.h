#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------------
 *  Configuracion de arranque
 *
 *  Los punteros de texto se COPIAN internamente a buffers propios del
 *  modulo en alpaca_start() -- no es necesario que el llamador los
 *  mantenga vivos despues de la llamada.
 *
 *  Dos grupos de metadata, porque Alpaca los distingue en el protocolo:
 *
 *    - server_name/manufacturer/manufacturer_version/location: nivel
 *      SERVIDOR, expuestos en GET /management/v1/description. Describen
 *      "que es esta caja de firmware", no un dispositivo en particular
 *      (un mismo servidor Alpaca podria, en teoria, exponer varios
 *      dispositivos).
 *
 *    - device_name/device_description/driver_info/driver_version:
 *      nivel DISPOSITIVO, expuestos en GET /api/v1/focuser/0/{name,
 *      description,driverinfo,driverversion}. Describen especificamente
 *      "que es el focuser" -- son parte del "Common Device Interface"
 *      que ASCOM Conform Universal exige en TODO dispositivo Alpaca,
 *      independiente del tipo (focuser, camera, etc).
 * ---------------------------------------------------------------------- */
typedef struct {
    uint16_t    port;                  /* puerto TCP del servidor Alpaca (no confundir con WS_SERVER_PORT) */

    /* ---- Metadata de SERVIDOR (/management/v1/description) ---- */
    const char *server_name;
    const char *manufacturer;
    const char *manufacturer_version;
    const char *location;

    /* ---- Metadata de DISPOSITIVO (/api/v1/focuser/0/...) ----
     * device_description tiene un limite practico de 64 caracteres en
     * el spec de Alpaca (para poder usarse en headers FITS, que limitan
     * 80 caracteres incluyendo el nombre del header) -- no se trunca
     * automaticamente aqui, es responsabilidad de quien arma la config. */
    const char *device_name;
    const char *device_description;
    const char *driver_info;
    const char *driver_version;
    const char *device_unique_id;

    /* Parametros de la task interna (alpaca_http_task). */
    UBaseType_t task_priority;
    BaseType_t  task_core_id;
    uint32_t    task_stack_words;
} alpaca_config_t;

/* ----------------------------------------------------------------------
 *  alpaca_start()
 *
 *  Arranca esp_http_server, registra los endpoints de /management y del
 *  Focuser (lecturas del Paso 4, comandos del Paso 5, y el Common Device
 *  Interface que ASCOM Conform Universal exige en cualquier dispositivo
 *  Alpaca), y lanza la task interna que mantiene el servidor vivo.
 *
 *  Debe llamarse DESPUES de que la conexion WiFi este arriba. Retorna
 *  ESP_OK si el servidor arranco y los handlers quedaron registrados;
 *  un error en caso contrario, sin tumbar el resto del firmware.
 * ---------------------------------------------------------------------- */
esp_err_t alpaca_start(const alpaca_config_t *cfg);

#ifdef __cplusplus
}
#endif