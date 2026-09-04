/*
 * ============================================================================
 *  alpaca.c - Servidor HTTP ASCOM Alpaca
 *             (management + Paso 4: GET focuser + Paso 5: PUT move/halt)
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
 * Paso 5 (PUT move/halt): a diferencia de los GET del Paso 4, los
 * parametros de un PUT en Alpaca viajan en el BODY como
 * application/x-www-form-urlencoded (Position=1234&ClientTransactionID=5),
 * NO como JSON -- por eso alpaca_read_put_body() + httpd_query_key_value()
 * en vez de cJSON_Parse() para leer los parametros de entrada.
 */

#include <string.h>
#include <stdlib.h>
#include "alpaca.h"

#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "cJSON.h"
#include "focuser_handler.h"

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

/* Numero de dispositivo Alpaca para el focuser, fijo en 0 porque este
 * firmware controla UN SOLO focuser -- no hay necesidad de parsear el
 * numero desde la URL (ni de exponerlo en alpaca_config_t) mientras el
 * proyecto no soporte multiples dispositivos del mismo tipo. Si eso
 * cambia algun dia, aqui es donde se convertiria en parametro. */
#define ALPACA_FOCUSER_BASE_PATH  "/api/v1/focuser/0"

/* Cuantos handlers registra este modulo en total: 2 de /management +
 * 10 GET del Focuser (Paso 4) + 2 PUT (Paso 5, move/halt) = 14. Se deja
 * el limite en 16 para no tener que volver a tocarlo si se agrega algo
 * chico mas adelante (p.ej. PUT connected). */
#define ALPACA_MAX_URI_HANDLERS  16

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

/* -- Paso 4: lecturas del Focuser (todas GET, todas solo leen de
 * focuser_handler, ninguna tiene logica propia mas alla de serializar) -- */
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

/* -- Paso 5: comandos del Focuser (PUT, con logica real -- no son solo
 * serializacion, a diferencia de los GET del Paso 4) -- */
static esp_err_t alpaca_focuser_move_handler(httpd_req_t *req);
static esp_err_t alpaca_focuser_halt_handler(httpd_req_t *req);

/* -- Helpers comunes al formato de respuesta Alpaca (/api/v1/..., NO
 * /management/..., que tiene un formato mas simple y ya esta resuelto
 * arriba en alpaca_apiversions_handler/alpaca_description_handler) -- */
static uint32_t alpaca_get_client_transaction_id(httpd_req_t *req);
static uint32_t alpaca_next_server_transaction_id(void);
static esp_err_t alpaca_send_value_response(httpd_req_t *req, cJSON *value_item);

/* -- Helpers especificos de PUT (leer body form-urlencoded + responder
 * sin campo "Value", que es el formato de las operaciones PUT) -- */
static esp_err_t alpaca_read_put_body(httpd_req_t *req, char *buf, size_t buf_size);
static uint32_t alpaca_get_form_param_uint32(const char *form, const char *key);
static esp_err_t alpaca_send_put_response(httpd_req_t *req, uint32_t client_transaction_id,
                                           int error_number, const char *error_message);

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
    config.max_uri_handlers = ALPACA_MAX_URI_HANDLERS;

    /* ctrl_port: puerto UDP de loopback interno que httpd usa para su
     * propia comunicacion (no es el puerto TCP publico que expone el
     * servidor). HTTPD_DEFAULT_CONFIG() lo fija en un valor fijo
     * (32768) -- si ws_server (tambien esp_http_server) ya lo tomo
     * primero, esta segunda instancia falla al crear su "ctrl socket"
     * con ESP_FAIL/errno 112, SIN IMPORTAR que los puertos TCP publicos
     * (80 vs este) sean distintos. Se fija a mano en un valor que no
     * choque con el de ws_server -- ver nota en alpaca.h/alpaca_config_t
     * si en el futuro conviene hacerlo configurable en vez de sumar 1
     * aqui mismo. */
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

    /* ---- Paso 4: rutas GET de solo lectura del Focuser ----
     * Registro repetitivo a proposito: cada httpd_uri_t es un dato, no
     * logica, y una tabla explicita es mas facil de auditar/extender
     * (Paso 5 solo agrega 2 entradas mas, PUT move/halt) que una macro
     * que genere esto -- mismo criterio de legibilidad-sobre-cleverness
     * ya aplicado en el resto del proyecto. */
    httpd_uri_t focuser_uris[] = {
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
        /* ---- Paso 5: comandos ---- */
        { .uri = ALPACA_FOCUSER_BASE_PATH "/move",              .method = HTTP_PUT, .handler = alpaca_focuser_move_handler,              .user_ctx = NULL },
        { .uri = ALPACA_FOCUSER_BASE_PATH "/halt",              .method = HTTP_PUT, .handler = alpaca_focuser_halt_handler,              .user_ctx = NULL },
    };
    for (size_t i = 0; i < sizeof(focuser_uris) / sizeof(focuser_uris[0]); i++) {
        esp_err_t reg_err = httpd_register_uri_handler(alpaca_httpd, &focuser_uris[i]);
        if (reg_err != ESP_OK) {
            ESP_LOGE(TAG, "[alpaca_http_task] No se pudo registrar '%s' (%s).",
                     focuser_uris[i].uri, esp_err_to_name(reg_err));
        }
    }

    uint32_t heap_after = esp_get_free_heap_size();
    ESP_LOGI(TAG, "[alpaca_http_task] Heap libre DESPUES de httpd_start()+handlers: %lu bytes "
                   "(costo aproximado: %ld bytes)",
             (unsigned long)heap_after, (long)heap_before - (long)heap_after);
    ESP_LOGI(TAG, "[alpaca_http_task] Servidor Alpaca (management + focuser GET) escuchando en puerto %u.",
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

/* ============================================================================
 *  IMPLEMENTACION - HELPERS DE RESPUESTA /api/v1/... (Alpaca "device API")
 * ============================================================================
 *
 *  A diferencia de /management/..., el formato de respuesta del "Device
 *  API" de Alpaca exige ademas ErrorNumber/ErrorMessage, y
 *  ClientTransactionID debe reflejar lo que mando el cliente (no un 0
 *  fijo). Se centraliza aqui para que los 10 handlers de abajo (y los
 *  que se agreguen en el Paso 5) sean una linea cada uno.
 * ============================================================================ */

/* Lee "ClientTransactionID" del query string, si vino. Los clientes
 * Alpaca reales (N.I.N.A., ASCOM Conform Universal) siempre lo mandan;
 * si no vino (p.ej. una prueba manual con curl sin query string), se
 * responde con 0 -- no es un error de protocolo. */
static uint32_t alpaca_get_client_transaction_id(httpd_req_t *req)
{
    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len == 0) {
        return 0;
    }

    /* +1 para el terminador nulo que pide httpd_req_get_url_query_str().
     * 128 es generoso para los pocos parametros que maneja Alpaca
     * (ClientID, ClientTransactionID, y en el Paso 5 Position/Connected). */
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

/* ServerTransactionID: contador incremental propio del servidor, sin
 * relacion con ClientTransactionID. No se protege con mutex/atomico a
 * proposito: la configuracion por defecto de esp_http_server atiende
 * las peticiones HTTP de forma SECUENCIAL en una unica task interna
 * (no hay un worker por conexion), asi que dos llamadas a esta funcion
 * nunca se solapan en el tiempo. Si en el futuro este servidor pasara a
 * un modo multi-worker, esto necesitaria un lock. */
static uint32_t alpaca_next_server_transaction_id(void)
{
    static uint32_t counter = 0;
    counter++;
    return counter;
}

/* Arma y envia {"Value":<value_item>,"ClientTransactionID":...,
 * "ServerTransactionID":...,"ErrorNumber":0,"ErrorMessage":""} --
 * ErrorNumber/ErrorMessage van fijos en "sin error" porque los 10
 * handlers del Paso 4 son lecturas puras que no pueden fallar del lado
 * del protocolo (si el dato no esta disponible, focuser_handler ya
 * devuelve un valor por defecto seguro, ver sus getters). Un error real
 * de Alpaca (p.ej. Position invalida en el futuro PUT move del Paso 5)
 * necesitara su propio helper con ErrorNumber != 0.
 *
 * Toma ownership de value_item: lo consume (cJSON_AddItemToObject) y no
 * hay que liberarlo aparte. */
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
 * ============================================================================
 *
 *  Cada handler es una sola llamada a un getter de focuser_handler mas
 *  el envoltorio de respuesta -- ninguno tiene logica propia. Si algun
 *  dia hace falta validacion o transformacion de un valor, es señal de
 *  que esa logica deberia vivir en focuser_handler, no aqui.
 * ============================================================================ */

/* GET .../connected
 * focuser_handler no modela un estado de conexion (es un focuser
 * embebido, siempre presente mientras el firmware corre) -- se responde
 * TRUE fijo. Si el mecanismo llegara a necesitar un estado real de "no
 * listo" (p.ej. homing pendiente), ese estado deberia vivir en
 * focuser_handler con su propio getter, no inventarse aqui. */
static esp_err_t alpaca_focuser_connected_handler(httpd_req_t *req)
{
    return alpaca_send_value_response(req, cJSON_CreateBool(true));
}

/* GET .../position -- posicion absoluta actual, en microsteps. */
static esp_err_t alpaca_focuser_position_handler(httpd_req_t *req)
{
    int64_t position = focuser_handler_get_position();
    return alpaca_send_value_response(req, cJSON_CreateNumber((double)position));
}

/* GET .../ismoving */
static esp_err_t alpaca_focuser_ismoving_handler(httpd_req_t *req)
{
    return alpaca_send_value_response(req, cJSON_CreateBool(focuser_handler_get_is_moving()));
}

/* GET .../absolute -- TRUE fijo: el unico modo de movimiento que expone
 * este firmware es absoluto (focuser_handler_move_to() recibe una
 * posicion objetivo, no un delta). */
static esp_err_t alpaca_focuser_absolute_handler(httpd_req_t *req)
{
    return alpaca_send_value_response(req, cJSON_CreateBool(true));
}

/* GET .../maxstep -- limite superior de Position, en microsteps. */
static esp_err_t alpaca_focuser_maxstep_handler(httpd_req_t *req)
{
    int32_t max_step = focuser_handler_get_max_step();
    return alpaca_send_value_response(req, cJSON_CreateNumber((double)max_step));
}

/* GET .../maxincrement -- Alpaca lo define como el maximo delta
 * permitido en un solo Move() relativo. Este firmware no impone un
 * limite mas chico que el recorrido total, asi que se reporta igual a
 * MaxStep -- convencion estandar cuando no hay un limite por-movimiento
 * independiente. */
static esp_err_t alpaca_focuser_maxincrement_handler(httpd_req_t *req)
{
    int32_t max_step = focuser_handler_get_max_step();
    return alpaca_send_value_response(req, cJSON_CreateNumber((double)max_step));
}

/* GET .../stepsize -- micrometros por microstep. */
static esp_err_t alpaca_focuser_stepsize_handler(httpd_req_t *req)
{
    double step_size = focuser_handler_get_step_size();
    return alpaca_send_value_response(req, cJSON_CreateNumber(step_size));
}

/* GET .../tempcompavailable */
static esp_err_t alpaca_focuser_tempcompavailable_handler(httpd_req_t *req)
{
    return alpaca_send_value_response(req, cJSON_CreateBool(focuser_handler_get_tempcomp_available()));
}

/* GET .../tempcomp -- estado actual de la compensacion termica. El PUT
 * correspondiente (focuser_handler_set_tempcomp() ya existe en
 * focuser_handler) es parte del Paso 5, no de este. */
static esp_err_t alpaca_focuser_tempcomp_handler(httpd_req_t *req)
{
    return alpaca_send_value_response(req, cJSON_CreateBool(focuser_handler_get_tempcomp()));
}

/* GET .../temperature -- grados Celsius, ultima lectura del AHT21B
 * reportada por i2c_sensors_task. */
static esp_err_t alpaca_focuser_temperature_handler(httpd_req_t *req)
{
    float temp_c = focuser_handler_get_temperature();
    return alpaca_send_value_response(req, cJSON_CreateNumber((double)temp_c));
}

/* ============================================================================
 *  IMPLEMENTACION - HELPERS DE PUT (body form-urlencoded + respuesta sin "Value")
 * ============================================================================ */

/* Lee el body completo de una peticion PUT a un buffer NUL-terminado.
 * Rechaza (ESP_ERR_INVALID_SIZE) en vez de truncar silenciosamente si el
 * body no cabe -- un body de Alpaca truncado a la fuerza podria parsear
 * "Position=123" como si fuera un valor valido cuando en realidad el
 * cliente mando algo mas largo que se corto a la mitad. */
static esp_err_t alpaca_read_put_body(httpd_req_t *req, char *buf, size_t buf_size)
{
    size_t len = req->content_len;

    if (len >= buf_size) {
        ESP_LOGW(TAG, "Body PUT demasiado largo (%u bytes, limite %u).",
                 (unsigned)len, (unsigned)buf_size);
        return ESP_ERR_INVALID_SIZE;
    }

    if (len == 0) {
        /* Un PUT sin body es valido en Alpaca (p.ej. Halt sin
         * ClientTransactionID) -- se trata como cadena vacia, no como
         * error. */
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

/* Extrae un parametro numerico sin signo de un body ya leido (formato
 * "clave=valor&clave=valor"). httpd_query_key_value() sirve igual para
 * query strings de GET que para bodies form-urlencoded de PUT -- mismo
 * formato clave=valor. Retorna 0 si el parametro no vino (Alpaca no lo
 * exige: ClientTransactionID ausente no es un error de protocolo). */
static uint32_t alpaca_get_form_param_uint32(const char *form, const char *key)
{
    char value[16];
    if (httpd_query_key_value(form, key, value, sizeof(value)) != ESP_OK) {
        return 0;
    }
    return (uint32_t)strtoul(value, NULL, 10);
}

/* Arma y envia la respuesta de una operacion PUT -- SIN campo "Value"
 * (eso es exclusivo de las respuestas GET, ver alpaca_send_value_response()).
 * error_number=0 + error_message="" para el caso exitoso.
 *
 * Nota de protocolo: incluso un comando RECHAZADO por motivos logicos del
 * dispositivo (Position invalida, motor ocupado) se responde con HTTP
 * 200 y el error va codificado en ErrorNumber/ErrorMessage -- un codigo
 * HTTP distinto de 200 esta reservado para errores de TRANSPORTE (ruta
 * invalida, fallo catastrofico interno), no para rechazos normales del
 * protocolo Alpaca. */
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

/* PUT .../move
 * Body esperado: "Position=<entero>[&ClientID=...][&ClientTransactionID=...]"
 *
 * A diferencia de los GET del Paso 4, este handler SI tiene logica
 * propia: parsea y valida Position, y traduce el resultado de
 * focuser_handler_move_to() a los codigos de error Alpaca
 * correspondientes. La validacion de rango [0, MaxStep] ya la hace
 * focuser_handler_move_to() -- este handler no duplica esa logica, solo
 * mapea su esp_err_t a un ErrorNumber Alpaca. */
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

    /* Mismo timeout corto que usan cmd_input_task/ws_on_message para
     * xQueueSend sobre motor_cmd_queue: si motor_task esta ocupada, se
     * informa al cliente en vez de bloquear la task httpd (bloquearla
     * afectaria a TODOS los clientes Alpaca conectados, no solo a este). */
    esp_err_t err = focuser_handler_move_to((int64_t)target, pdMS_TO_TICKS(100));

    if (err == ESP_ERR_INVALID_ARG) {
        return alpaca_send_put_response(req, client_txn_id, ALPACA_ERR_INVALID_VALUE,
                                         "Position fuera de rango [0, MaxStep].");
    }
    if (err == ESP_ERR_TIMEOUT) {
        /* "Motor ocupado" no tiene un codigo Alpaca estandar dedicado --
         * se usa el piso del rango reservado a errores especificos del
         * driver (0x500-0xFFF) en vez de forzar uno de los estandar que
         * no describe bien la situacion. */
        return alpaca_send_put_response(req, client_txn_id, ALPACA_ERR_DRIVER_BASE,
                                         "El motor esta ocupado, comando descartado.");
    }
    if (err != ESP_OK) {
        return alpaca_send_put_response(req, client_txn_id, ALPACA_ERR_DRIVER_BASE,
                                         "Error interno al encolar el movimiento.");
    }

    return alpaca_send_put_response(req, client_txn_id, 0, "");
}

/* PUT .../halt
 * Body esperado (opcional): "[ClientID=...][&ClientTransactionID=...]"
 * -- no tiene parametros propios, solo los comunes de Alpaca.
 *
 * NO bloquea esperando a que el motor termine de frenar: solo levanta
 * la bandera (focuser_handler_request_halt()) y responde de inmediato.
 * motor_task/tmc2209_move_steps() son quienes consumen esa bandera e
 * inician la rampa de frenado real (ver tmc2209.c); un cliente Alpaca
 * que necesite saber cuando el movimiento realmente termino debe
 * consultar IsMoving despues de este PUT, como indica el estandar. */
static esp_err_t alpaca_focuser_halt_handler(httpd_req_t *req)
{
    char body[ALPACA_PUT_BODY_MAX];
    uint32_t client_txn_id = 0;

    if (alpaca_read_put_body(req, body, sizeof(body)) == ESP_OK) {
        client_txn_id = alpaca_get_form_param_uint32(body, "ClientTransactionID");
    }
    /* Si el body no se pudo leer (body demasiado largo, etc.) igual se
     * atiende el halt -- un error leyendo un body opcional no deberia
     * impedir una operacion de seguridad como detener el motor. */

    focuser_handler_request_halt();

    return alpaca_send_put_response(req, client_txn_id, 0, "");
}