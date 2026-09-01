# wifi_init.c - Inicialización WiFi

## Descripción General
Componente de inicialización y conexión WiFi como estación (STA).

Características:
- Conexión automática a red SSID/password
- Reintentos con límite configurable
- Sincronización de eventos con EventGroup
- Integración con NVS para almacenamiento de calibración RF
- Logging de eventos de conexión

## Conceptos Principales

### Event Group
FreeRTOS EventGroup con dos bits:
- **WIFI_CONNECTED_BIT (BIT0)**: IP obtenida exitosamente
- **WIFI_FAIL_BIT (BIT1)**: Agotados reintentos sin conexión

## Función Principal

### `wifi_init_sta(const wifi_init_config_app_t *cfg)`

**Entrada:**
- `cfg`: Estructura con:
  - `ssid`: SSID de red WiFi
  - `password`: Contraseña WiFi
  - `max_retries`: Máximo de intentos de reconexión

**Procedimiento:**

1. **Validación de parámetros**
   - Verifica cfg, ssid y password no NULL

2. **Inicialización NVS**
   - Inicializa "Non-Volatile Storage" requerido por stack WiFi
   - Guarda calibración RF entre reinicios
   - Si partición corrupta/llena: erase y reintenta

3. **Creación de EventGroup**
   - `xEventGroupCreate()` para sincronización de eventos

4. **Inicialización de stack WiFi**
   - `esp_netif_init()`: Infraestructura de red
   - `esp_event_loop_create_default()`: Loop de eventos
   - `esp_netif_create_default_wifi_sta()`: Interfaz STA
   - `esp_wifi_init()`: Stack WiFi

5. **Registro de event handlers**
   - `WIFI_EVENT`: Conexión/desconexión
   - `IP_EVENT`: Obtención de IP

6. **Configuración de credenciales**
   - Copia SSID y password a estructura `wifi_config_t`
   - Establece autenticación WPA2_PSK

7. **Inicio de WiFi**
   - `esp_wifi_set_mode(WIFI_MODE_STA)`
   - `esp_wifi_set_config(WIFI_IF_STA, &wifi_config)`
   - `esp_wifi_start()`

8. **Bloqueo en EventGroup**
   - Espera a WIFI_CONNECTED_BIT o WIFI_FAIL_BIT
   - Sin timeout adicional (max_retries proporciona límite)

## Event Handler

### Eventos Manejados

**WIFI_EVENT_STA_START**
- WiFi iniciado, inicia conexión con `esp_wifi_connect()`

**WIFI_EVENT_STA_DISCONNECTED**
- Conexión perdida
- Si reintentos < max_retries:
  - Incrementa contador
  - Reintenta con `esp_wifi_connect()`
  - Log de reintento
- Si reintentos agotados:
  - Establece WIFI_FAIL_BIT
  - EventGroup despierta consumer

**IP_EVENT_STA_GOT_IP**
- IP asignada por DHCP
- Log de IP obtenida (formato IPSTR/IP2STR)
- Resetea contador de reintentos
- Establece flag `s_connected = true`
- Establece WIFI_CONNECTED_BIT

## Sincronización

- **EventGroupHandle_t**: Despierta consumer sin polling
- **s_retry_count**: Contador de intentos fallidos
- **s_max_retries**: Límite de reintentos (configurable)
- **s_connected**: Estado de conexión actual

### Garantías
- Blocking wait hasta conexión o fallo
- No consume CPU mientras espera (sleep en EventGroup)
- Compatible con FreeRTOS tasks concurrentes

## Integración con Sistema

### En main.c
```
+-------------------+
| cmd_input_task    |
| mqtt_task         | -> motor_cmd_queue
| i2c_sensors_task  |
+-------------------+
        |
    wifi_init_sta() -> WiFi + servidor WS
        |
   +----------+
   | servidor |
   | WS/HTTP  |
   +----------+
```

### NVS
- Stack WiFi usa NVS automáticamente
- Almacena calibración RF
- Si ya inicializado en otra parte: no-op seguro

## Consideraciones de Diseño

1. **Max Retries:** Previene reconexión infinita
2. **Autenticación WPA2:** Estándar seguro
3. **DHCP automático:** Obtiene IP del router
4. **No timeout de bloqueo:** `max_retries` proporciona límite inherente
5. **Logging detallado:** Facilita debug de problemas WiFi

## Error Handling

- `ESP_ERR_INVALID_ARG`: cfg/ssid/password NULL
- `ESP_ERR_NO_MEM`: No se puede crear EventGroup
- `ESP_ERROR_CHECK()`: NVS y stack WiFi (abort si fallan)
  - NVS es requisito no-negociable
  - WiFi stack requiere componentes base correctos

## Logging

- TAG: "wifi_init"
- INFO: IP obtenida (IPSTR)
- WARNING: Reintentos en progreso
- ERROR (via ESP_ERROR_CHECK): Fallos críticos

## Dependencias

- FreeRTOS: EventGroup, tasks
- ESP-IDF WiFi:
  - `esp_wifi.h`: Stack WiFi
  - `esp_event.h`: Event loop
  - `esp_netif.h`: Network interface
  - `nvs_flash.h`: NVS storage
- `esp_log.h`: Logging

## Flujo de Eventos Típico

```
wifi_init_sta() llamado
         |
    NVS init OK
         |
    WiFi stack init
         |
    Event handlers registrados
         |
    esp_wifi_start()
         |
    WIFI_EVENT_STA_START -> esp_wifi_connect()
         |
    [Intento 1: WIFI_EVENT_STA_DISCONNECTED]
         |
    [Reintento N: esp_wifi_connect()]
         |
    IP_EVENT_STA_GOT_IP -> EventGroup set CONNECTED
         |
    wifi_init_sta() retorna ESP_OK
```
