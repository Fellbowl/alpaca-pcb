/*
 * ============================================================================
 *  wifi_init.c - ver wifi_init.h para contrato y notas de diseño
 * ============================================================================
 */
#include "wifi_init.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_init";

/* Bits del event group: uno para "conectado con IP", otro para
 * "se agotaron los reintentos y no se pudo conectar". */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_event_group = NULL;
static uint8_t s_retry_count = 0;
static uint8_t s_max_retries = 10;
static bool s_connected = false;

static void event_handler(void *arg, esp_event_base_t event_base,
                           int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_retry_count < s_max_retries) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGW(TAG, "Reintentando conexion WiFi (%u/%u)...", s_retry_count, s_max_retries);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "IP obtenida: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        s_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_init_sta(const wifi_init_config_app_t *cfg)
{
    if (cfg == NULL || cfg->ssid == NULL || cfg->password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_max_retries = cfg->max_retries;
    s_retry_count = 0;

    /* NVS es requerido por el stack WiFi para guardar calibracion RF.
     * Si ya fue inicializado en otro lado de la app, esto es no-op seguro
     * salvo por el caso de particion corrupta/llena, que se limpia y
     * reintenta -- patron estandar de los ejemplos de ESP-IDF. */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t idf_wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&idf_wifi_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    wifi_config_t wifi_config = { 0 };
    strncpy((char *)wifi_config.sta.ssid, cfg->ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, cfg->password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Conectando a SSID '%s'...", cfg->ssid);

    /* Bloquea hasta CONNECTED o FAIL. Sin timeout adicional aqui porque
     * WIFI_FAIL_BIT ya se dispara cuando se agotan max_retries -- no hay
     * riesgo de bloqueo indefinido salvo que el propio stack WiFi nunca
     * dispare STA_DISCONNECTED (no deberia pasar). */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }

    ESP_LOGE(TAG, "No se pudo conectar tras %u intentos.", s_max_retries);
    return ESP_FAIL;
}

bool wifi_init_is_connected(void)
{
    return s_connected;
}