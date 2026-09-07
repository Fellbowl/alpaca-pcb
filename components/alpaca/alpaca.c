/*
 * ============================================================================
 *  alpaca.c - Servidor HTTP ASCOM Alpaca
 *             (management + Paso 4: GET focuser + Paso 5: PUT move/halt
 *              + Common Device Interface: metadata, Connected, TempComp,
 *              y los stubs deprecados que ASCOM Conform Universal exige
 *              que EXISTAN aunque no hagan nada real)
 * ============================================================================
 *
 * Ver alpaca.h para el contrato publico. Este archivo (no el header) es el
 * que incluye focuser_handler.h -- alpaca.h sigue sin exponer ningun tipo
 * de focuser_handler, asi que cualquiera que solo necesite arrancar el
 * servidor (alpaca_start()) no arrastra esa dependencia.
 *
 * focuser_handler es un singleton (funciones globales, sin handle), igual
 * que este propio modulo -- por eso los handlers de abajo lo llaman
 * directo, sin que alpaca_config_t necesite un puntero nuevo para
 * "conectarlos".
 *
 * Decision de diseño -- InterfaceVersion=3, no 4:
 *   El spec de Platform 7 (2024) introdujo IFocuserV4, que deprecia
 *   escribir Connected para conectar/desconectar y en su lugar espera
 *   Connect()/Disconnect()/Connecting -- una maquina de estados
 *   asincrona (la conexion puede demorar). Para un focuser embebido que
 *   esta "conectado" mientras el ESP32 tiene corriente, esa maquina de
 *   estados es trabajo real sin beneficio real. Se declara
 *   InterfaceVersion=3 (IFocuserV3), que sigue usando el modelo clasico
 *   GET/PUT Connected -- spec-legal y muchisimo mas simple.
 *
 * Decision de diseño -- Connected NO gatea el resto de los endpoints:
 *   Connected es una bandera de PROTOCOLO (vive en este archivo, no en
 *   focuser_handler -- no es parte del estado FISICO del focuser), legible
 *   y escribible, pero NO se valida en los demas handlers. Un driver
 *   Alpaca "estricto" rechazaria toda otra operacion con ErrorNumber
 *   0x407 (NotConnected) mientras Connected=false; para un focuser
 *   embebido sin motivo real para "desconectarse" en software, esa
 *   validacion cruzada en los 14 handlers restantes es complejidad sin
 *   beneficio real. Si algun cliente Alpaca real llegara a fallar por
 *   esto en la practica, es un cambio acotado a agregar aqui.
 *
 * Paso 5 (PUT move/halt/tempcomp/connected): a diferencia de los GET del
 * Paso 4, los parametros de un PUT en Alpaca viajan en el BODY como
 * application/x-www-form-urlencoded (Position=1234&ClientTransactionID=5),
 * NO como JSON -- por eso alpaca_read_put_body() + httpd_query_key_value()
 * en vez de cJSON_Parse() para leer los parametros de entrada.
 */

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdatomic.h>
#include "alpaca.h"

#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "cJSON.h"
#include "focuser_handler.h"

static const char *TAG = "ALPACA";

/* Alpaca no fija un tamaño maximo para estos strings; los buffers de aqui
 * son generosos para cualquier nombre/manufacturer/location/etc. razonable
 * sin gastar heap (son estaticos, no malloc). Si algun dia hace falta
 * mas, subir esta constante es un cambio de una linea. */
#define ALPACA_STR_FIELD_MAX  64

/* Cuanto esperar entre logs de heap una vez que el servidor ya esta
 * arriba y estable -- solo diagnostico, no afecta el comportamiento del
 * servidor. */
#define ALPACA_HEAP_LOG_PERIOD_MS  30000

/* Numero de dispositivo Alpaca para el focuser, fijo en 0 porque este
 * firmware controla UN SOLO focuser -- no hay necesidad de parsear el
 * numero desde la URL (ni de exponerlo en alpaca_config_t) mientras el
 * proyecto no soporte multiples dispositivos del mismo tipo. Si eso
 * cambia algun dia, aqui es donde se convertiria en parametro. */
#define ALPACA_FOCUSER_BASE_PATH  "/api/v1/focuser/0"

/* Version de la interfaz IFocuserVx que este firmware implementa --
 * fijo en 3 a proposito, ver nota de diseño arriba (evita la maquina de
 * estados asincrona Connect()/Disconnect()/Connecting de IFocuserV4). */
#define ALPACA_FOCUSER_INTERFACE_VERSION  3

/* Cuantos handlers registra este modulo en total:
 *   2  /management
 *   10 GET del Focuser (Paso 4)
 *   2  PUT move/halt (Paso 5)
 *   6  GET del Common Device Interface (name, description, driverinfo,
 *      driverversion, interfaceversion, supportedactions)
 *   2  PUT connected/tempcomp
 *   4  PUT deprecados (action, commandblind, commandbool, commandstring)
 *   = 26. Se deja margen en 32 para no tener que volver a tocar esto. */
#define ALPACA_MAX_URI_HANDLERS  32

/* Tamaño maximo aceptado para el body de un PUT. Los bodies de Alpaca
 * son cortos (unos pocos "Clave=Valor" separados por '&'); 128 bytes es
 * generoso incluso para Position (hasta 10 digitos) + ClientID +
 * ClientTransactionID juntos. Un body mas largo que esto se rechaza en
 * vez de truncarlo silenciosamente -- ver alpaca_read_put_body(). */
#define ALPACA_PUT_BODY_MAX  128

/* Codigos de error estandar de ASCOM Alpaca (ASCOM Alpaca API Reference,
 * seccion de "Common Error Numbers"). El rango 0x500-0xFFF esta
 * reservado para errores especificos de cada driver -- se usa el piso de
 * ese rango para casos que no encajan en ninguno de los estandar (p.ej.
 * "motor ocupado", que no tiene codigo Alpaca dedicado). */
#define ALPACA_ERR_NOT_IMPLEMENTED  0x400
#define ALPACA_ERR_INVALID_VALUE    0x401
#define ALPACA_ERR_DRIVER_BASE      0x500

/* ============================================================================
 *  ESTADO INTERNO DEL MODULO
 * ============================================================================
 *
 *  Un solo servidor Alpaca por firmware (igual que ws_server.c en este
 *  proyecto): variables estaticas de archivo, no una instancia
 *  parametrizable. Si el dia de mañana hiciera falta mas de un servidor
 *  Alpaca en el mismo binario, esto se convertiria en un contexto pasado
 *  por puntero -- no es el caso hoy.
 * ============================================================================ */
static httpd_handle_t alpaca_httpd = NULL;

static char alpaca_server_name[ALPACA_STR_FIELD_MAX];
static char alpaca_manufacturer[ALPACA_STR_FIELD_MAX];
static char alpaca_manufacturer_version[ALPACA_STR_FIELD_MAX];
static char alpaca_location[ALPACA_STR_FIELD_MAX];

static char alpaca_device_name[ALPACA_STR_FIELD_MAX];
static char alpaca_device_description[ALPACA_STR_FIELD_MAX];
static char alpaca_driver_info[ALPACA_STR_FIELD_MAX];
static char alpaca_driver_version[ALPACA_STR_FIELD_MAX];

/* Connected: bandera de PROTOCOLO, no de estado fisico del focuser (ver
 * nota de diseño al inicio del archivo). atomic_bool por el mismo motivo
 * que focuser_handler usa atomic_bool para su flag de halt: lectura sin
 * mutex, coherente con que esp_http_server atiende peticiones de forma
 * secuencial por defecto (no hay carrera real hoy), pero deja la
 * intencion explicita si el dia de mañana cambia el modelo de threading. */
static atomic_bool alpaca_connected = true;

/* ============================================================================
 *  PROTOTIPOS
 * ============================================================================ */

static void alpaca_http_task(void *arg);
static esp_err_t alpaca_apiversions_handler(httpd_req_t *req);
static esp_err_t alpaca_description_handler(httpd_req_t *req);

/* -- Paso 4: lecturas del Focuser -- */
static esp_err_t alpaca_focuser_connected_handler(httpd_req_t *req);
static esp_err_t alpaca_focuser_position_handler(httpd_req_t *req);
static esp_err_t alpaca_focuser_ismoving_handler(httpd_req_t *req);
static esp_err_t alpaca_focuser_absolute_handler(httpd_req_t *req);
static esp_err_t alpaca_focuser_maxstep_handler(httpd_req_t *req);
static esp_err_t alpaca_focuser_maxincrement_handler(httpd_req_t *req);
static esp_err_t alpaca_focuser_stepsize_handler(httpd_req_t *req);
static esp_err_t alpaca_focuser_tempcompavailable_handler(httpd_req_t *req);
static esp_err_t alpaca_focuser_tempcomp_handler(httpd_req_t *req);
static esp_err_t alpaca_focuser_temperature_handler(httpd_req_t *req);

/* -- Paso 5: comandos del Focuser (PUT) -- */
static esp_err_t alpaca_focuser_move_handler(httpd_req_t *req);
static esp_err_t alpaca_focuser_halt_handler(httpd_req_t *req);
static esp_err_t alpaca_focuser_connected_put_handler(httpd_req_t *req);
static esp_err_t alpaca_focuser_tempcomp_put_handler(httpd_req_t *req);

/* -- Common Device Interface: metadata (GET) -- */
static esp_err_t alpaca_focuser_name_handler(httpd_req_t *req);
static esp_err_t alpaca_focuser_devicedescription_handler(httpd_req_t *req);
static esp_err_t alpaca_focuser_driverinfo_handler(httpd_req_t *req);
static esp_err_t alpaca_focuser_driverversion_handler(httpd_req_t *req);
static esp_err_t alpaca_focuser_interfaceversion_handler(httpd_req_t *req);
static esp_err_t alpaca_focuser_supportedactions_handler(httpd_req_t *req);

/* -- Common Device Interface: metodos deprecados (PUT) -- deben EXISTIR
 * y responder NotImplemented, no dar 404 -- ConformU los valida. Los 4
 * comparten un unico handler porque su comportamiento es identico. */
static esp_err_t alpaca_not_implemented_put_handler(httpd_req_t *req);

/* -- Helpers comunes al formato de respuesta Alpaca -- */
static uint32_t alpaca_get_client_transaction_id(httpd_req_t *req);
static uint32_t alpaca_next_server_transaction_id(void);
static esp_err_t alpaca_send_value_response(httpd_req_t *req, cJSON *value_item);

/* -- Helpers especificos de PUT -- */
static esp_err_t alpaca_read_put_body(httpd_req_t *req, char *buf, size_t buf_size);
static uint32_t alpaca_get_form_param_uint32(const char *form, const char *key);
static esp_err_t alpaca_parse_form_bool(const char *form, const char *key, bool *out_value);
static esp_err_t alpaca_send_put_response(httpd_req_t *req, uint32_t client_transaction_id,
                                           int error_number, const char *error_message);

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

    copy_field(alpaca_server_name, sizeof(alpaca_server_name), cfg->server_name, "Alpaca Device");
    copy_field(alpaca_manufacturer, sizeof(alpaca_manufacturer), cfg->manufacturer, "Unknown");
    copy_field(alpaca_manufacturer_version, sizeof(alpaca_manufacturer_version), cfg->manufacturer_version, "0.0.0");
    copy_field(alpaca_location, sizeof(alpaca_location), cfg->location, "Unknown");

    copy_field(alpaca_device_name, sizeof(alpaca_device_name), cfg->device_name, "Focuser");
    copy_field(alpaca_device_description, sizeof(alpaca_device_description), cfg->device_description, "Generic focuser");
    copy_field(alpaca_driver_info, sizeof(alpaca_driver_info), cfg->driver_info, "Unknown driver");
    copy_field(alpaca_driver_version, sizeof(alpaca_driver_version), cfg->driver_version, "0.0.0");

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
    config.max_uri_handlers = ALPACA_MAX_URI_HANDLERS;
    config.ctrl_port = ESP_HTTPD_DEF_CTRL_PORT + 1;

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

    /* ---- Rutas del Focuser: lecturas (Paso 4), comandos (Paso 5) y
     * Common Device Interface (metadata + stubs deprecados). Tabla
     * explicita a proposito -- cada httpd_uri_t es un dato, no logica,
     * y una tabla es mas facil de auditar que una macro que la genere. */
    httpd_uri_t focuser_uris[] = {
        /* -- Paso 4: lecturas -- */
        { .uri = ALPACA_FOCUSER_BASE_PATH "/connected",         .method = HTTP_GET, .handler = alpaca_focuser_connected_handler,         .user_ctx = NULL },
        { .uri = ALPACA_FOCUSER_BASE_PATH "/position",          .method = HTTP_GET, .handler = alpaca_focuser_position_handler,          .user_ctx = NULL },
        { .uri = ALPACA_FOCUSER_BASE_PATH "/ismoving",          .method = HTTP_GET, .handler = alpaca_focuser_ismoving_handler,          .user_ctx = NULL },
        { .uri = ALPACA_FOCUSER_BASE_PATH "/absolute",          .method = HTTP_GET, .handler = alpaca_focuser_absolute_handler,          .user_ctx = NULL },
        { .uri = ALPACA_FOCUSER_BASE_PATH "/maxstep",           .method = HTTP_GET, .handler = alpaca_focuser_maxstep_handler,           .user_ctx = NULL },
        { .uri = ALPACA_FOCUSER_BASE_PATH "/maxincrement",      .method = HTTP_GET, .handler = alpaca_focuser_maxincrement_handler,      .user_ctx = NULL },
        { .uri = ALPACA_FOCUSER_BASE_PATH "/stepsize",          .method = HTTP_GET, .handler = alpaca_focuser_stepsize_handler,          .user_ctx = NULL },
        { .uri = ALPACA_FOCUSER_BASE_PATH "/tempcompavailable", .method = HTTP_GET, .handler = alpaca_focuser_tempcompavailable_handler, .user_ctx = NULL },
        { .uri = ALPACA_FOCUSER_BASE_PATH "/tempcomp",          .method = HTTP_GET, .handler = alpaca_focuser_tempcomp_handler,          .user_ctx = NULL },
        { .uri = ALPACA_FOCUSER_BASE_PATH "/temperature",       .method = HTTP_GET, .handler = alpaca_focuser_temperature_handler,       .user_ctx = NULL },

        /* -- Paso 5: comandos -- */
        { .uri = ALPACA_FOCUSER_BASE_PATH "/move",              .method = HTTP_PUT, .handler = alpaca_focuser_move_handler,              .user_ctx = NULL },
        { .uri = ALPACA_FOCUSER_BASE_PATH "/halt",              .method = HTTP_PUT, .handler = alpaca_focuser_halt_handler,              .user_ctx = NULL },
        { .uri = ALPACA_FOCUSER_BASE_PATH "/connected",         .method = HTTP_PUT, .handler = alpaca_focuser_connected_put_handler,     .user_ctx = NULL },
        { .uri = ALPACA_FOCUSER_BASE_PATH "/tempcomp",          .method = HTTP_PUT, .handler = alpaca_focuser_tempcomp_put_handler,      .user_ctx = NULL },

        /* -- Common Device Interface: metadata (GET) -- */
        { .uri = ALPACA_FOCUSER_BASE_PATH "/name",              .method = HTTP_GET, .handler = alpaca_focuser_name_handler,              .user_ctx = NULL },
        { .uri = ALPACA_FOCUSER_BASE_PATH "/description",       .method = HTTP_GET, .handler = alpaca_focuser_devicedescription_handler, .user_ctx = NULL },
        { .uri = ALPACA_FOCUSER_BASE_PATH "/driverinfo",        .method = HTTP_GET, .handler = alpaca_focuser_driverinfo_handler,        .user_ctx = NULL },
        { .uri = ALPACA_FOCUSER_BASE_PATH "/driverversion",     .method = HTTP_GET, .handler = alpaca_focuser_driverversion_handler,     .user_ctx = NULL },
        { .uri = ALPACA_FOCUSER_BASE_PATH "/interfaceversion",  .method = HTTP_GET, .handler = alpaca_focuser_interfaceversion_handler,  .user_ctx = NULL },
        { .uri = ALPACA_FOCUSER_BASE_PATH "/supportedactions",  .method = HTTP_GET, .handler = alpaca_focuser_supportedactions_handler,  .user_ctx = NULL },

        /* -- Common Device Interface: deprecados, deben existir y
         * responder NotImplemented (ver alpaca_not_implemented_put_handler) -- */
        { .uri = ALPACA_FOCUSER_BASE_PATH "/action",            .method = HTTP_PUT, .handler = alpaca_not_implemented_put_handler,       .user_ctx = NULL },
        { .uri = ALPACA_FOCUSER_BASE_PATH "/commandblind",      .method = HTTP_PUT, .handler = alpaca_not_implemented_put_handler,       .user_ctx = NULL },
        { .uri = ALPACA_FOCUSER_BASE_PATH "/commandbool",       .method = HTTP_PUT, .handler = alpaca_not_implemented_put_handler,       .user_ctx = NULL },
        { .uri = ALPACA_FOCUSER_BASE_PATH "/commandstring",     .method = HTTP_PUT, .handler = alpaca_not_implemented_put_handler,       .user_ctx = NULL },
    };
    for (size_t i = 0; i < sizeof(focuser_uris) / sizeof(focuser_uris[0]); i++) {
        esp_err_t reg_err = httpd_register_uri_handler(alpaca_httpd, &focuser_uris[i]);
        if (reg_err != ESP_OK) {
            ESP_LOGE(TAG, "[alpaca_http_task] No se pudo registrar '%s' [%s] (%s).",
                     focuser_uris[i].uri, focuser_uris[i].method == HTTP_GET ? "GET" : "PUT",
                     esp_err_to_name(reg_err));
        }
    }

    uint32_t heap_after = esp_get_free_heap_size();
    ESP_LOGI(TAG, "[alpaca_http_task] Heap libre DESPUES de httpd_start()+handlers: %lu bytes "
                   "(costo aproximado: %ld bytes)",
             (unsigned long)heap_after, (long)heap_before - (long)heap_after);
    ESP_LOGI(TAG, "[alpaca_http_task] Servidor Alpaca (management + focuser + common device interface) "
                   "escuchando en puerto %u.", (unsigned)port);
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

/* ============================================================================
 *  IMPLEMENTACION - HELPERS DE RESPUESTA /api/v1/... (GET)
 * ============================================================================ */
static uint32_t alpaca_get_client_transaction_id(httpd_req_t *req)
{
    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len == 0) {
        return 0;
    }

    char query[128];
    if (query_len >= sizeof(query)) {
        ESP_LOGW(TAG, "Query string mas largo de lo esperado (%u bytes), se ignora.",
                 (unsigned)query_len);
        return 0;
    }
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return 0;
    }

    char value[16];
    if (httpd_query_key_value(query, "ClientTransactionID", value, sizeof(value)) != ESP_OK) {
        return 0;
    }

    return (uint32_t)strtoul(value, NULL, 10);
}

static uint32_t alpaca_next_server_transaction_id(void)
{
    static uint32_t counter = 0;
    counter++;
    return counter;
}

static esp_err_t alpaca_send_value_response(httpd_req_t *req, cJSON *value_item)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "Value", value_item);
    cJSON_AddNumberToObject(root, "ClientTransactionID", alpaca_get_client_transaction_id(req));
    cJSON_AddNumberToObject(root, "ServerTransactionID", alpaca_next_server_transaction_id());
    cJSON_AddNumberToObject(root, "ErrorNumber", 0);
    cJSON_AddStringToObject(root, "ErrorMessage", "");

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);

    cJSON_free(json_str);
    cJSON_Delete(root);
    return ret;
}

/* ============================================================================
 *  IMPLEMENTACION - PASO 4: LECTURAS DEL FOCUSER
 * ============================================================================ */

/* GET .../connected -- ahora lee el flag real de protocolo (ver nota de
 * diseño al inicio del archivo), ya no un TRUE fijo. */
static esp_err_t alpaca_focuser_connected_handler(httpd_req_t *req)
{
    return alpaca_send_value_response(req, cJSON_CreateBool(atomic_load(&alpaca_connected)));
}

static esp_err_t alpaca_focuser_position_handler(httpd_req_t *req)
{
    int64_t position = focuser_handler_get_position();
    return alpaca_send_value_response(req, cJSON_CreateNumber((double)position));
}

static esp_err_t alpaca_focuser_ismoving_handler(httpd_req_t *req)
{
    return alpaca_send_value_response(req, cJSON_CreateBool(focuser_handler_get_is_moving()));
}

static esp_err_t alpaca_focuser_absolute_handler(httpd_req_t *req)
{
    return alpaca_send_value_response(req, cJSON_CreateBool(true));
}

static esp_err_t alpaca_focuser_maxstep_handler(httpd_req_t *req)
{
    int32_t max_step = focuser_handler_get_max_step();
    return alpaca_send_value_response(req, cJSON_CreateNumber((double)max_step));
}

static esp_err_t alpaca_focuser_maxincrement_handler(httpd_req_t *req)
{
    int32_t max_step = focuser_handler_get_max_step();
    return alpaca_send_value_response(req, cJSON_CreateNumber((double)max_step));
}

static esp_err_t alpaca_focuser_stepsize_handler(httpd_req_t *req)
{
    double step_size = focuser_handler_get_step_size();
    return alpaca_send_value_response(req, cJSON_CreateNumber(step_size));
}

static esp_err_t alpaca_focuser_tempcompavailable_handler(httpd_req_t *req)
{
    return alpaca_send_value_response(req, cJSON_CreateBool(focuser_handler_get_tempcomp_available()));
}

static esp_err_t alpaca_focuser_tempcomp_handler(httpd_req_t *req)
{
    return alpaca_send_value_response(req, cJSON_CreateBool(focuser_handler_get_tempcomp()));
}

static esp_err_t alpaca_focuser_temperature_handler(httpd_req_t *req)
{
    float temp_c = focuser_handler_get_temperature();
    return alpaca_send_value_response(req, cJSON_CreateNumber((double)temp_c));
}

/* ============================================================================
 *  IMPLEMENTACION - COMMON DEVICE INTERFACE: METADATA (GET)
 * ============================================================================
 *
 *  Todas de solo lectura, todas fijas (config de alpaca_start() o
 *  ALPACA_FOCUSER_INTERFACE_VERSION) -- ninguna consulta a
 *  focuser_handler, porque esta metadata describe el DRIVER, no el
 *  estado del focuser fisico.
 * ============================================================================ */
static esp_err_t alpaca_focuser_name_handler(httpd_req_t *req)
{
    return alpaca_send_value_response(req, cJSON_CreateString(alpaca_device_name));
}

static esp_err_t alpaca_focuser_devicedescription_handler(httpd_req_t *req)
{
    return alpaca_send_value_response(req, cJSON_CreateString(alpaca_device_description));
}

static esp_err_t alpaca_focuser_driverinfo_handler(httpd_req_t *req)
{
    return alpaca_send_value_response(req, cJSON_CreateString(alpaca_driver_info));
}

static esp_err_t alpaca_focuser_driverversion_handler(httpd_req_t *req)
{
    return alpaca_send_value_response(req, cJSON_CreateString(alpaca_driver_version));
}

static esp_err_t alpaca_focuser_interfaceversion_handler(httpd_req_t *req)
{
    return alpaca_send_value_response(req, cJSON_CreateNumber(ALPACA_FOCUSER_INTERFACE_VERSION));
}

/* GET .../supportedactions -- array vacio: este driver no expone
 * ninguna extension custom via Action(). */
static esp_err_t alpaca_focuser_supportedactions_handler(httpd_req_t *req)
{
    return alpaca_send_value_response(req, cJSON_CreateArray());
}

/* ============================================================================
 *  IMPLEMENTACION - HELPERS DE PUT
 * ============================================================================ */
static esp_err_t alpaca_read_put_body(httpd_req_t *req, char *buf, size_t buf_size)
{
    size_t len = req->content_len;

    if (len >= buf_size) {
        ESP_LOGW(TAG, "Body PUT demasiado largo (%u bytes, limite %u).",
                 (unsigned)len, (unsigned)buf_size);
        return ESP_ERR_INVALID_SIZE;
    }

    if (len == 0) {
        buf[0] = '\0';
        return ESP_OK;
    }

    int received = httpd_req_recv(req, buf, len);
    if (received <= 0) {
        ESP_LOGW(TAG, "Fallo leyendo el body PUT (retorno %d).", received);
        return ESP_FAIL;
    }
    buf[received] = '\0';
    return ESP_OK;
}

static uint32_t alpaca_get_form_param_uint32(const char *form, const char *key)
{
    char value[16];
    if (httpd_query_key_value(form, key, value, sizeof(value)) != ESP_OK) {
        return 0;
    }
    return (uint32_t)strtoul(value, NULL, 10);
}

/* Parsea un parametro booleano Alpaca ("true"/"false", sin distinguir
 * mayusculas, que es como lo mandan los clientes reales). Retorna
 * ESP_ERR_NOT_FOUND si el parametro no vino, ESP_ERR_INVALID_ARG si vino
 * pero no es "true" ni "false", ESP_OK si se parseo correctamente. */
static esp_err_t alpaca_parse_form_bool(const char *form, const char *key, bool *out_value)
{
    char value[8];
    if (httpd_query_key_value(form, key, value, sizeof(value)) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    if (strcasecmp(value, "true") == 0) {
        *out_value = true;
        return ESP_OK;
    }
    if (strcasecmp(value, "false") == 0) {
        *out_value = false;
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t alpaca_send_put_response(httpd_req_t *req, uint32_t client_transaction_id,
                                           int error_number, const char *error_message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "ClientTransactionID", client_transaction_id);
    cJSON_AddNumberToObject(root, "ServerTransactionID", alpaca_next_server_transaction_id());
    cJSON_AddNumberToObject(root, "ErrorNumber", error_number);
    cJSON_AddStringToObject(root, "ErrorMessage", error_message != NULL ? error_message : "");

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);

    cJSON_free(json_str);
    cJSON_Delete(root);
    return ret;
}

/* ============================================================================
 *  IMPLEMENTACION - PASO 5: COMANDOS DEL FOCUSER (PUT)
 * ============================================================================ */

static esp_err_t alpaca_focuser_move_handler(httpd_req_t *req)
{
    char body[ALPACA_PUT_BODY_MAX];
    if (alpaca_read_put_body(req, body, sizeof(body)) != ESP_OK) {
        return alpaca_send_put_response(req, 0, ALPACA_ERR_INVALID_VALUE,
                                         "No se pudo leer el cuerpo de la peticion.");
    }

    uint32_t client_txn_id = alpaca_get_form_param_uint32(body, "ClientTransactionID");

    char position_str[16];
    if (httpd_query_key_value(body, "Position", position_str, sizeof(position_str)) != ESP_OK) {
        return alpaca_send_put_response(req, client_txn_id, ALPACA_ERR_INVALID_VALUE,
                                         "Falta el parametro 'Position'.");
    }

    char *endptr = NULL;
    long target = strtol(position_str, &endptr, 10);
    if (endptr == position_str || target < 0) {
        return alpaca_send_put_response(req, client_txn_id, ALPACA_ERR_INVALID_VALUE,
                                         "Position invalida (debe ser un entero >= 0).");
    }

    esp_err_t err = focuser_handler_move_to((int64_t)target, pdMS_TO_TICKS(100));

    if (err == ESP_ERR_INVALID_ARG) {
        return alpaca_send_put_response(req, client_txn_id, ALPACA_ERR_INVALID_VALUE,
                                         "Position fuera de rango [0, MaxStep].");
    }
    if (err == ESP_ERR_TIMEOUT) {
        return alpaca_send_put_response(req, client_txn_id, ALPACA_ERR_DRIVER_BASE,
                                         "El motor esta ocupado, comando descartado.");
    }
    if (err != ESP_OK) {
        return alpaca_send_put_response(req, client_txn_id, ALPACA_ERR_DRIVER_BASE,
                                         "Error interno al encolar el movimiento.");
    }

    return alpaca_send_put_response(req, client_txn_id, 0, "");
}

static esp_err_t alpaca_focuser_halt_handler(httpd_req_t *req)
{
    char body[ALPACA_PUT_BODY_MAX];
    uint32_t client_txn_id = 0;

    if (alpaca_read_put_body(req, body, sizeof(body)) == ESP_OK) {
        client_txn_id = alpaca_get_form_param_uint32(body, "ClientTransactionID");
    }

    focuser_handler_request_halt();

    return alpaca_send_put_response(req, client_txn_id, 0, "");
}

/* PUT .../connected
 * Body esperado: "Connected=true|false[&ClientTransactionID=...]"
 *
 * Solo actualiza la bandera de PROTOCOLO -- no toca focuser_handler, no
 * enciende/apaga nada fisico, y NO gatea el resto de los endpoints (ver
 * nota de diseño al inicio del archivo). */
static esp_err_t alpaca_focuser_connected_put_handler(httpd_req_t *req)
{
    char body[ALPACA_PUT_BODY_MAX];
    if (alpaca_read_put_body(req, body, sizeof(body)) != ESP_OK) {
        return alpaca_send_put_response(req, 0, ALPACA_ERR_INVALID_VALUE,
                                         "No se pudo leer el cuerpo de la peticion.");
    }

    uint32_t client_txn_id = alpaca_get_form_param_uint32(body, "ClientTransactionID");

    bool connected = false;
    esp_err_t parse_err = alpaca_parse_form_bool(body, "Connected", &connected);
    if (parse_err == ESP_ERR_NOT_FOUND) {
        return alpaca_send_put_response(req, client_txn_id, ALPACA_ERR_INVALID_VALUE,
                                         "Falta el parametro 'Connected'.");
    }
    if (parse_err != ESP_OK) {
        return alpaca_send_put_response(req, client_txn_id, ALPACA_ERR_INVALID_VALUE,
                                         "'Connected' debe ser 'true' o 'false'.");
    }

    atomic_store(&alpaca_connected, connected);
    return alpaca_send_put_response(req, client_txn_id, 0, "");
}

/* PUT .../tempcomp
 * Body esperado: "TempComp=true|false[&ClientTransactionID=...]"
 *
 * A diferencia de focuser_handler_set_tempcomp() (que a proposito acepta
 * cualquier valor, incluso con TempCompAvailable=false, dejandolo "sin
 * efecto"), el PROTOCOLO Alpaca exige rechazar explicitamente un intento
 * de poner TempComp=true cuando TempCompAvailable=false -- por eso esa
 * validacion vive ACA (capa de protocolo), no en focuser_handler (capa
 * de estado fisico), sin tener que volver mas estricto a focuser_handler
 * para otros llamadores (p.ej. el websocket) que puedan preferir el
 * comportamiento permisivo actual. */
static esp_err_t alpaca_focuser_tempcomp_put_handler(httpd_req_t *req)
{
    char body[ALPACA_PUT_BODY_MAX];
    if (alpaca_read_put_body(req, body, sizeof(body)) != ESP_OK) {
        return alpaca_send_put_response(req, 0, ALPACA_ERR_INVALID_VALUE,
                                         "No se pudo leer el cuerpo de la peticion.");
    }

    uint32_t client_txn_id = alpaca_get_form_param_uint32(body, "ClientTransactionID");

    bool tempcomp = false;
    esp_err_t parse_err = alpaca_parse_form_bool(body, "TempComp", &tempcomp);
    if (parse_err == ESP_ERR_NOT_FOUND) {
        return alpaca_send_put_response(req, client_txn_id, ALPACA_ERR_INVALID_VALUE,
                                         "Falta el parametro 'TempComp'.");
    }
    if (parse_err != ESP_OK) {
        return alpaca_send_put_response(req, client_txn_id, ALPACA_ERR_INVALID_VALUE,
                                         "'TempComp' debe ser 'true' o 'false'.");
    }

    if (tempcomp && !focuser_handler_get_tempcomp_available()) {
        return alpaca_send_put_response(req, client_txn_id, ALPACA_ERR_INVALID_VALUE,
                                         "TempComp no disponible en este hardware (TempCompAvailable=false).");
    }

    focuser_handler_set_tempcomp(tempcomp);
    return alpaca_send_put_response(req, client_txn_id, 0, "");
}

/* ============================================================================
 *  IMPLEMENTACION - COMMON DEVICE INTERFACE: METODOS DEPRECADOS (PUT)
 * ============================================================================
 *
 *  Action, CommandBlind, CommandBool y CommandString estan deprecados
 *  desde la interfaz V2 en favor de extensiones especificas por
 *  dispositivo -- este driver no implementa ninguna, pero ConformU
 *  exige que las 4 rutas EXISTAN (no 404) y respondan con el error de
 *  protocolo correcto (NotImplemented), no que hagan algo real. Por eso
 *  las 4 comparten este unico handler -- el comportamiento es
 *  identico, no hay logica que diferenciar entre ellas. */
static esp_err_t alpaca_not_implemented_put_handler(httpd_req_t *req)
{
    char body[ALPACA_PUT_BODY_MAX];
    uint32_t client_txn_id = 0;

    if (alpaca_read_put_body(req, body, sizeof(body)) == ESP_OK) {
        client_txn_id = alpaca_get_form_param_uint32(body, "ClientTransactionID");
    }

    return alpaca_send_put_response(req, client_txn_id, ALPACA_ERR_NOT_IMPLEMENTED,
                                     "No implementado: este driver no expone acciones ni comandos custom.");
}