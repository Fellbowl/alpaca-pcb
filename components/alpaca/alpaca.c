/*
 * ============================================================================
 *  alpaca.c - Servidor HTTP ASCOM Alpaca (arranque + endpoints de management)
 * ============================================================================
 *
 * Ver alpaca.h para el contrato publico y la nota de por que este modulo
 * NO conoce todavia a focuser_handler (eso llega en el Paso 4/5).
 */

#include <string.h>
#include <stdlib.h>
#include "alpaca.h"

#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "cJSON.h"

static const char *TAG = "ALPACA";

/* Alpaca no fija un tamaño maximo para estos strings; los buffers de aqui
 * son generosos para cualquier nombre/manufacturer/location razonable sin
 * gastar heap (son estaticos, no malloc). Si algun dia hace falta mas,
 * subir estas constantes es un cambio de una linea. */
#define ALPACA_STR_FIELD_MAX  64

/* Cuanto esperar entre logs de heap una vez que el servidor ya esta
 * arriba y estable -- solo diagnostico, no afecta el comportamiento del
 * servidor. */
#define ALPACA_HEAP_LOG_PERIOD_MS  30000

/* ============================================================================
 *  ESTADO INTERNO DEL MODULO
 * ============================================================================
 *
 *  Un solo servidor Alpaca por firmware (igual que ws_server.c en este
 *  proyecto): variables estaticas de archivo, no una instancia
 *  parametrizable. Si el dia de mañana hiciera falta mas de un servidor
 *  Alpaca en el mismo binario, esto se convertiria en un contexto pasado
 *  por puntero -- no es el caso hoy, y agregar esa flexibilidad ahora
 *  seria complejidad sin uso real (mismo criterio que ya se aplico en
 *  otros modulos del proyecto).
 * ============================================================================ */
static httpd_handle_t alpaca_httpd = NULL;

static char alpaca_server_name[ALPACA_STR_FIELD_MAX];
static char alpaca_manufacturer[ALPACA_STR_FIELD_MAX];
static char alpaca_manufacturer_version[ALPACA_STR_FIELD_MAX];
static char alpaca_location[ALPACA_STR_FIELD_MAX];

/* ============================================================================
 *  PROTOTIPOS
 * ============================================================================ */

static void alpaca_http_task(void *arg);
static esp_err_t alpaca_apiversions_handler(httpd_req_t *req);
static esp_err_t alpaca_description_handler(httpd_req_t *req);

/* Copia un string de origen (posiblemente NULL) a un buffer fijo,
 * garantizando terminacion nula y sin desbordar. Centraliza la logica
 * defensiva que alpaca_start() necesita para cada uno de los 4 campos de
 * texto de alpaca_config_t. */
static void copy_field(char *dst, size_t dst_size, const char *src, const char *fallback)
{
    const char *source = (src != NULL) ? src : fallback;
    strncpy(dst, source, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

/* ============================================================================
 *  IMPLEMENTACION PUBLICA
 * ============================================================================ */
esp_err_t alpaca_start(const alpaca_config_t *cfg)
{
    if (cfg == NULL) {
        ESP_LOGE(TAG, "alpaca_start: cfg es NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (alpaca_httpd != NULL) {
        ESP_LOGW(TAG, "alpaca_start: el servidor ya estaba arrancado, se ignora.");
        return ESP_ERR_INVALID_STATE;
    }

    /* Copiar los campos de texto ANTES de crear la task: el llamador no
     * tiene garantizado mantener vivos sus punteros despues de esta
     * funcion (ver nota en alpaca.h). */
    copy_field(alpaca_server_name, sizeof(alpaca_server_name), cfg->server_name, "Alpaca Device");
    copy_field(alpaca_manufacturer, sizeof(alpaca_manufacturer), cfg->manufacturer, "Unknown");
    copy_field(alpaca_manufacturer_version, sizeof(alpaca_manufacturer_version), cfg->manufacturer_version, "0.0.0");
    copy_field(alpaca_location, sizeof(alpaca_location), cfg->location, "Unknown");

    /* El puerto viaja a la task via un pequeño heap alloc de un solo
     * uint16_t porque xTaskCreatePinnedToCore no tiene forma de pasar un
     * valor por copia (solo un puntero void*) y el puerto no puede vivir
     * en una variable estatica compartida si algun dia se soporta mas de
     * un servidor -- se libera dentro de la propia task apenas lo lee. */
    uint16_t *port_arg = malloc(sizeof(uint16_t));
    if (port_arg == NULL) {
        ESP_LOGE(TAG, "alpaca_start: no se pudo reservar memoria para el argumento de puerto.");
        return ESP_ERR_NO_MEM;
    }
    *port_arg = cfg->port;

    BaseType_t created = xTaskCreatePinnedToCore(
        alpaca_http_task,
        "alpaca_http_task",
        cfg->task_stack_words,
        port_arg,
        cfg->task_priority,
        NULL,
        cfg->task_core_id);

    if (created != pdPASS) {
        ESP_LOGE(TAG, "alpaca_start: no se pudo crear alpaca_http_task.");
        free(port_arg);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* ============================================================================
 *  IMPLEMENTACION - TASK INTERNA
 * ============================================================================
 *
 *  Recibe el puerto por 'arg' (uint16_t* reservado en alpaca_start()),
 *  arranca httpd, registra los handlers de management, loguea el
 *  checkpoint de heap del Paso 3 y luego se queda viva logueando heap
 *  cada ALPACA_HEAP_LOG_PERIOD_MS -- httpd_start() ya crea su propia task
 *  interna para atender conexiones, asi que este loop lento no bloquea
 *  ninguna peticion entrante.
 * ============================================================================ */
static void alpaca_http_task(void *arg)
{
    uint16_t port = *(uint16_t *)arg;
    free(arg);

    uint32_t heap_before = esp_get_free_heap_size();
    ESP_LOGI(TAG, "[alpaca_http_task] Heap libre ANTES de httpd_start(): %lu bytes",
             (unsigned long)heap_before);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.max_uri_handlers = 4; /* margen chico para las rutas del Paso 4/5 */

    esp_err_t err = httpd_start(&alpaca_httpd, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[alpaca_http_task] No se pudo arrancar httpd Alpaca (%s). Task terminada.",
                 esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    httpd_uri_t apiversions_uri = {
        .uri = "/management/apiversions",
        .method = HTTP_GET,
        .handler = alpaca_apiversions_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(alpaca_httpd, &apiversions_uri);

    httpd_uri_t description_uri = {
        .uri = "/management/v1/description",
        .method = HTTP_GET,
        .handler = alpaca_description_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(alpaca_httpd, &description_uri);

    uint32_t heap_after = esp_get_free_heap_size();
    ESP_LOGI(TAG, "[alpaca_http_task] Heap libre DESPUES de httpd_start()+handlers: %lu bytes "
                   "(costo aproximado: %ld bytes)",
             (unsigned long)heap_after, (long)heap_before - (long)heap_after);
    ESP_LOGI(TAG, "[alpaca_http_task] Servidor Alpaca (solo management) escuchando en puerto %u.",
             (unsigned)port);
    ESP_LOGI(TAG, "[alpaca_http_task] Stack libre minimo tras inicializacion: %u words.",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(ALPACA_HEAP_LOG_PERIOD_MS));
        ESP_LOGI(TAG, "[alpaca_http_task] Heap libre actual: %lu bytes",
                 (unsigned long)esp_get_free_heap_size());
    }
}

/* ============================================================================
 *  IMPLEMENTACION - HANDLERS DE /management
 * ============================================================================ */

/* GET /management/apiversions
 * Formato Alpaca: {"Value":[1],"ClientTransactionID":0,"ServerTransactionID":0}
 * Solo version 1 soportada por ahora. */
static esp_err_t alpaca_apiversions_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *versions = cJSON_CreateArray();
    cJSON_AddItemToArray(versions, cJSON_CreateNumber(1));
    cJSON_AddItemToObject(root, "Value", versions);
    cJSON_AddNumberToObject(root, "ClientTransactionID", 0);
    cJSON_AddNumberToObject(root, "ServerTransactionID", 0);

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);

    cJSON_free(json_str);
    cJSON_Delete(root);
    return ret;
}

/* GET /management/v1/description
 * Formato Alpaca: {"Value":{"ServerName":...,"Manufacturer":...,
 * "ManufacturerVersion":...,"Location":...},"ClientTransactionID":0,
 * "ServerTransactionID":0} */
static esp_err_t alpaca_description_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *value = cJSON_CreateObject();
    cJSON_AddStringToObject(value, "ServerName", alpaca_server_name);
    cJSON_AddStringToObject(value, "Manufacturer", alpaca_manufacturer);
    cJSON_AddStringToObject(value, "ManufacturerVersion", alpaca_manufacturer_version);
    cJSON_AddStringToObject(value, "Location", alpaca_location);
    cJSON_AddItemToObject(root, "Value", value);
    cJSON_AddNumberToObject(root, "ClientTransactionID", 0);
    cJSON_AddNumberToObject(root, "ServerTransactionID", 0);

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);

    cJSON_free(json_str);
    cJSON_Delete(root);
    return ret;
}
