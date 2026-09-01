/*
 * ============================================================================
 *  alpaca.h - Servidor HTTP ASCOM Alpaca (arranque + endpoints de management)
 * ============================================================================
 *
 * Paso 3 del plan de migracion Alpaca: este modulo SOLO levanta el
 * servidor esp_http_server y expone los dos endpoints de /management
 * (apiversions y description). A proposito no conoce focuser_handler,
 * motor_cmd_queue ni ningun otro modulo del proyecto -- el objetivo unico
 * de este paso es confirmar que el servidor arranca sin competir por heap
 * con el resto de las tasks en un ESP32-S3 sin PSRAM.
 *
 * Igual que tmc2209.[ch]/as5600.[ch]/focuser_handler.[ch]: autocontenido,
 * solo depende de headers de ESP-IDF/FreeRTOS. main.c es el unico que lo
 * incluye y lo conecta, pasandole su propia configuracion de pines/red
 * (aqui: puerto/nombre/etc) via alpaca_config_t -- este modulo no tiene
 * macros propias de proyecto, todo llega desde afuera.
 *
 * Los pasos 4/5 (GET de solo lectura, PUT Move/Halt) van a requerir que
 * este modulo SI conozca focuser_handler.h (para leer/escribir el estado
 * logico del focuser) -- eso se agrega cuando llegue ese paso, no antes.
 */

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
 *  Los punteros de texto (server_name, manufacturer, etc.) se COPIAN
 *  internamente a buffers propios del modulo en alpaca_start() -- no es
 *  necesario que el llamador los mantenga vivos despues de la llamada
 *  (a diferencia de wifi_init_config_app_t, donde SSID/password si se
 *  usan por referencia en otros modulos del proyecto). Esto evita que
 *  alpaca.c dependa de que main.c use macros `static const char *` vs
 *  literales de vida corta.
 * ---------------------------------------------------------------------- */
typedef struct {
    uint16_t    port;                  /* puerto TCP del servidor Alpaca (no confundir con WS_SERVER_PORT) */
    const char *server_name;           /* -> Value.ServerName en /management/v1/description */
    const char *manufacturer;          /* -> Value.Manufacturer */
    const char *manufacturer_version;  /* -> Value.ManufacturerVersion */
    const char *location;              /* -> Value.Location */

    /* Parametros de la task interna (alpaca_http_task). Se dejan
     * configurables, no fijos con #define adentro de alpaca.c, por la
     * misma razon que tmc2209_config_t recibe pines en vez de tenerlos
     * hardcodeados: este modulo no debe saber nada especifico de ESTE
     * proyecto, solo de ASCOM Alpaca en general. */
    UBaseType_t task_priority;
    BaseType_t  task_core_id;
    uint32_t    task_stack_words;
} alpaca_config_t;

/* ----------------------------------------------------------------------
 *  alpaca_start()
 *
 *  Arranca esp_http_server en el puerto indicado, registra los handlers
 *  de /management/apiversions y /management/v1/description, y lanza una
 *  task interna (pinneada al core indicado) que se queda viva logueando
 *  heap libre periodicamente -- el dato que se necesita para el
 *  checkpoint critico del Paso 3 antes de invertir en el Paso 4.
 *
 *  Debe llamarse DESPUES de que la conexion WiFi este arriba (igual que
 *  ws_server_start() en el proyecto): un httpd sin red no tiene proposito
 *  y este modulo no verifica conectividad por su cuenta.
 *
 *  Retorna ESP_OK si el servidor arranco y los handlers quedaron
 *  registrados; ESP_FAIL (o el error de httpd_start()/xTaskCreate) en
 *  caso contrario. Igual que el resto de los modulos del proyecto, un
 *  fallo aqui NO debe tumbar el resto del firmware -- es responsabilidad
 *  de main.c decidir que hacer con el error (tipicamente: solo loguear
 *  y seguir sin Alpaca, igual que ya hace con WiFi/ws_server).
 * ---------------------------------------------------------------------- */
esp_err_t alpaca_start(const alpaca_config_t *cfg);

#ifdef __cplusplus
}
#endif
