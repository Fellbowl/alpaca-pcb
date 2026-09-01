#pragma once
/*
 * ============================================================================
 *  wifi_init.h - Conexion WiFi en modo estacion (STA) - ESP32-S3 - ESP-IDF
 * ============================================================================
 *
 *  Modulo AUTOCONTENIDO: solo depende de headers de ESP-IDF, nunca de otro
 *  modulo propio (mismo principio que tmc2209.[ch]/as5600.[ch]/aht21b.[ch]).
 *  No sabe que existe ws_server.c ni ningun sensor -- su unico trabajo es
 *  conectar a una red WiFi existente y bloquear hasta obtener IP (o fallar
 *  tras los reintentos configurados).
 *
 *  Uso tipico desde main.c, ANTES de arrancar ws_server:
 *
 *      wifi_init_config_app_t cfg = {
 *          .ssid = "TuRedWiFi",
 *          .password = "TuPassword",
 *          .max_retries = 10,
 *      };
 *      esp_err_t err = wifi_init_sta(&cfg);
 *      if (err != ESP_OK) {
 *          // Sin WiFi no hay ws_server -- decide aqui si es fatal o si
 *          // sigues sin control remoto (igual que el resto del firmware,
 *          // esto es decision de orquestacion, no del modulo).
 *      }
 * ============================================================================
 */

#include "esp_err.h"
#include <stdint.h>

typedef struct {
    const char *ssid;
    const char *password;
    uint8_t max_retries;   /* reintentos de conexion antes de dar ESP_FAIL */
} wifi_init_config_app_t;

/* Inicializa NVS (si hace falta), el stack WiFi en modo STA, y bloquea
 * hasta conectar y obtener IP via DHCP, o hasta agotar max_retries.
 * Loguea la IP obtenida con ESP_LOGI al conectar. Idempotente: llamarla
 * dos veces no rompe nada, pero no esta pensado para reconexion en caliente
 * (eso lo maneja el propio stack WiFi/LWIP con sus reintentos internos
 * despues de la conexion inicial). */
esp_err_t wifi_init_sta(const wifi_init_config_app_t *cfg);

/* true si actualmente hay IP asignada (conectado). Util para que main.c
 * decida si tiene sentido arrancar ws_server o esperar. */
bool wifi_init_is_connected(void);
