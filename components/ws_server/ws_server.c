/*
 * ============================================================================
 *  ws_server.c - ver ws_server.h para contrato y notas de diseño
 * ============================================================================
 */
#include "ws_server.h"

#include <string.h>
#include <stdlib.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "ws_server";

static httpd_handle_t s_server = NULL;
static ws_server_on_message_cb_t s_on_message = NULL;
static SemaphoreHandle_t s_state_mutex = NULL;   /* protege s_server/s_on_message */

/* Estructura que viaja al trabajo asincrono encolado en el httpd. El
 * patron httpd_queue_work es el mecanismo OFICIAL de esp_http_server
 * para enviar datos por WS desde una task que no es la del httpd -- las
 * funciones httpd_ws_send_frame() normales solo son seguras desde dentro
 * de un handler que ya corre en la task del httpd. */
typedef struct {
    httpd_handle_t hd;
    int fd;
    char *text;   /* buffer propio (heap), liberado tras el envio */
} ws_async_send_arg_t;

/* ---- Handler HTTP/WS ---- */
static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* Handshake inicial: esp_http_server ya negocio el upgrade a WS
         * antes de llamar este handler con method==GET. Solo logueamos. */
        ESP_LOGI(TAG, "Cliente WS conectado (fd=%d)", httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(ws_pkt));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    /* Primero con buf=NULL para que rellene ws_pkt.len con el tamaño real
     * del frame -- patron estandar de esp_http_server. */
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame (tamaño) fallo: %s", esp_err_to_name(ret));
        return ret;
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGI(TAG, "Cliente WS desconectado (fd=%d)", httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    if (ws_pkt.len == 0) {
        return ESP_OK; /* ping/pong u otro frame de control sin payload */
    }

    /* +1 para null-terminar: el contrato de este modulo (ver header)
     * garantiza al callback un buffer terminado en '\0'. */
    uint8_t *buf = calloc(1, ws_pkt.len + 1);
    if (buf == NULL) {
        ESP_LOGE(TAG, "Sin memoria para frame de %d bytes", ws_pkt.len);
        return ESP_ERR_NO_MEM;
    }
    ws_pkt.payload = buf;

    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame (payload) fallo: %s", esp_err_to_name(ret));
        free(buf);
        return ret;
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT && s_on_message != NULL) {
        s_on_message((const char *)buf, ws_pkt.len);
    }

    free(buf);
    return ESP_OK;
}

static const httpd_uri_t s_ws_uri_template = {
    .method = HTTP_GET,
    .handler = ws_handler,
    .is_websocket = true,
};

esp_err_t ws_server_start(const ws_server_config_t *cfg)
{
    if (cfg == NULL || cfg->ws_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_state_mutex == NULL) {
        s_state_mutex = xSemaphoreCreateMutex();
        if (s_state_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    httpd_config_t httpd_cfg = HTTPD_DEFAULT_CONFIG();
    httpd_cfg.server_port = cfg->port;
    httpd_cfg.max_open_sockets = (cfg->max_clients > 0) ? cfg->max_clients : 4;
    /* uri_match_fn por defecto (httpd_uri_match_wildcard) alcanza para un
     * solo path fijo como "/ws"; si mas adelante agregas rutas REST de
     * Alpaca en el mismo servidor, revisa que no colisionen. */

    esp_err_t err = httpd_start(&s_server, &httpd_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start fallo: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t ws_uri = s_ws_uri_template;
    ws_uri.uri = cfg->ws_path;

    err = httpd_register_uri_handler(s_server, &ws_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo registrar handler WS en '%s': %s", cfg->ws_path, esp_err_to_name(err));
        httpd_stop(s_server);
        s_server = NULL;
        return err;
    }

    s_on_message = cfg->on_message;

    ESP_LOGI(TAG, "Servidor WS escuchando en puerto %u, path '%s'", cfg->port, cfg->ws_path);
    return ESP_OK;
}

esp_err_t ws_server_stop(void)
{
    if (s_server == NULL) {
        return ESP_OK;
    }
    esp_err_t err = httpd_stop(s_server);
    s_server = NULL;
    s_on_message = NULL;
    return err;
}

/* Callback que corre YA DENTRO de la task del httpd (llamado via
 * httpd_queue_work). Aqui si es seguro usar httpd_ws_send_frame(). */
static void ws_async_send_exec(void *arg)
{
    ws_async_send_arg_t *a = (ws_async_send_arg_t *)arg;

    httpd_ws_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = (uint8_t *)a->text;
    frame.len = strlen(a->text);

    esp_err_t err = httpd_ws_send_frame_async(a->hd, a->fd, &frame);
    if (err != ESP_OK) {
        /* Fallo de envio a un cliente puntual (p.ej. se desconecto justo
         * ahora) no debe tumbar el broadcast a los demas -- solo loguea. */
        ESP_LOGW(TAG, "Envio WS a fd=%d fallo: %s", a->fd, esp_err_to_name(err));
    }

    free(a->text);
    free(a);
}

esp_err_t ws_server_broadcast(const char *text)
{
    if (s_server == NULL || text == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Enumera los sockets abiertos del httpd y filtra los que son
     * conexiones WS activas. Patron documentado en esp_http_server para
     * "server-initiated" WS send. */
    size_t max_fds = CONFIG_LWIP_MAX_SOCKETS;
    int client_fds[CONFIG_LWIP_MAX_SOCKETS];
    size_t fds = max_fds;

    esp_err_t err = httpd_get_client_list(s_server, &fds, client_fds);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "httpd_get_client_list fallo: %s", esp_err_to_name(err));
        return err;
    }

    size_t sent_count = 0;

    for (size_t i = 0; i < fds; i++) {
        int fd = client_fds[i];
        if (httpd_ws_get_fd_info(s_server, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
            continue; /* socket HTTP normal, no un cliente WS */
        }

        ws_async_send_arg_t *arg = malloc(sizeof(ws_async_send_arg_t));
        if (arg == NULL) {
            ESP_LOGW(TAG, "Sin memoria para encolar envio a fd=%d, se omite.", fd);
            continue;
        }
        arg->hd = s_server;
        arg->fd = fd;
        arg->text = strdup(text);
        if (arg->text == NULL) {
            free(arg);
            continue;
        }

        if (httpd_queue_work(s_server, ws_async_send_exec, arg) != ESP_OK) {
            ESP_LOGW(TAG, "httpd_queue_work fallo para fd=%d", fd);
            free(arg->text);
            free(arg);
            continue;
        }
        sent_count++;
    }

    return (sent_count > 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

bool ws_server_has_clients(void)
{
    if (s_server == NULL) {
        return false;
    }

    size_t max_fds = CONFIG_LWIP_MAX_SOCKETS;
    int client_fds[CONFIG_LWIP_MAX_SOCKETS];
    size_t fds = max_fds;

    if (httpd_get_client_list(s_server, &fds, client_fds) != ESP_OK) {
        return false;
    }

    for (size_t i = 0; i < fds; i++) {
        if (httpd_ws_get_fd_info(s_server, client_fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
            return true;
        }
    }
    return false;
}
