#pragma once
/*
 * ============================================================================
 *  ws_server.h - Servidor WebSocket embebido sobre esp_http_server - ESP-IDF
 * ============================================================================
 *
 *  Modulo AUTOCONTENIDO (mismo principio que tmc2209.[ch]/as5600.[ch]):
 *  solo conoce el protocolo HTTP/WebSocket. NO sabe que existe un motor,
 *  un TMC2209 ni ningun sensor. Su contrato con el orquestador (main.c)
 *  es puramente textual:
 *
 *    - RECIBIR: cada mensaje de texto que llega de un cliente WS se
 *      entrega tal cual, como buffer crudo, al callback on_message que
 *      main.c registra. main.c decide que significa ese texto (parsearlo
 *      como JSON de un motor_cmd_t, por ejemplo).
 *
 *    - ENVIAR: main.c arma el texto (tipicamente JSON via cJSON) que
 *      quiera mandar -- telemetria, ack de comando, lo que sea -- y llama
 *      a ws_server_broadcast(). Este modulo no construye JSON ni conoce
 *      su estructura.
 *
 *  Por que esp_http_server y no un servidor WS "puro": esp_http_server ya
 *  viene con ESP-IDF, maneja el handshake HTTP->WS, ping/pong y multiples
 *  clientes por vos. Ademas deja la puerta abierta a servir rutas REST
 *  (por ejemplo ASCOM Alpaca) en el MISMO servidor mas adelante, sin
 *  levantar un segundo listener.
 *
 *  Threading: el callback on_message se invoca DESDE la propia task del
 *  httpd (no desde tu task de aplicacion). Si necesitas tocar estado
 *  compartido con otras tasks (como motor_cmd_queue), usa las primitivas
 *  normales de FreeRTOS (xQueueSend con timeout corto) -- exactamente
 *  igual que hacen los productores activos hoy. No hagas trabajo lento ahi
 *  (nada de vTaskDelay largos ni I2C/UART directo) porque bloquearia al
 *  httpd para todos los clientes.
 * ============================================================================
 */

#include "esp_err.h"
#include <stddef.h>
#include <stdbool.h>

/* Firma del callback de mensajes entrantes. `payload` NO esta
 * null-terminado garantizado por el protocolo WS, pero esta
 * implementacion SI lo null-termina antes de invocar el callback, para
 * que puedas usar funciones de string/JSON directamente sobre el.
 * `payload` es un buffer temporal: valido solo durante la llamada, no lo
 * guardes un puntero para usar despues. */
typedef void (*ws_server_on_message_cb_t)(const char *payload, size_t len);

typedef struct {
    uint16_t port;                     /* tipicamente 80 */
    const char *ws_path;               /* ej. "/ws" */
    ws_server_on_message_cb_t on_message; /* puede ser NULL si solo vas a transmitir */
    size_t max_clients;                /* limite de conexiones WS simultaneas */
} ws_server_config_t;

/* Arranca el servidor HTTP/WS. Requiere que el WiFi ya este conectado
 * (o al menos que haya una interfaz de red activa) -- este modulo no
 * inicializa WiFi, eso es responsabilidad de otro modulo/orquestador. */
esp_err_t ws_server_start(const ws_server_config_t *cfg);

/* Detiene el servidor y libera recursos. */
esp_err_t ws_server_stop(void);

/* Envia `text` (string null-terminado) a TODOS los clientes WS
 * actualmente conectados. Seguro de llamar desde cualquier task (usa
 * httpd_queue_work internamente para marshalling al contexto del httpd).
 * No bloquea esperando que el envio termine. Si no hay clientes
 * conectados, es un no-op barato. */
esp_err_t ws_server_broadcast(const char *text);

/* true si hay al menos un cliente WS conectado. Util para no gastar
 * ciclos armando JSON de telemetria si nadie esta escuchando. */
bool ws_server_has_clients(void);
