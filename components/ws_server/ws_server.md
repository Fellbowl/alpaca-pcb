# ws_server.c - Servidor WebSocket

## Propósito
Implementa un servidor WebSocket basado en esp_http_server (HTTPD) que permite comunicación bidireccional en tiempo real con clientes remotos. Maneja handshake HTTP → WebSocket, recepción de frames, y broadcast asincrónico a múltiples clientes conectados.

## Características Principales

### 1. Servidor HTTP/WebSocket
- Basado en `esp_http_server` de ESP-IDF.
- Soporte para múltiples clientes WebSocket simultáneos.
- Envío asincrónico de frames desde tasks que no son la del httpd.

### 2. Manejo de Conexiones
- **Handshake inicial**: esp_http_server negocia automáticamente el upgrade a WebSocket.
- **Conexión exitosa**: Se loguea con `fd` (file descriptor) del cliente.
- **Desconexión**: Se detecta cuando se recibe frame CLOSE o se cierra la conexión.

### 3. Recepción de Frames
- Soporte para frames de texto (HTTPD_WS_TYPE_TEXT).
- Automatización de buffer: primero sin `buf` para obtener tamaño, luego lectura completa.
- **Garantía de contrato**: El buffer pasado al callback está null-terminado (`\0`).
- Ignorado silenciosamente: frames de control vacíos (ping/pong).

## Estructura Interna

### Estado Global Protegido
```c
static httpd_handle_t s_server = NULL;              // Handle del servidor HTTP
static ws_server_on_message_cb_t s_on_message = NULL; // Callback de mensajes
static SemaphoreHandle_t s_state_mutex = NULL;       // Mutex que protege s_server y s_on_message
```

### Tipo de Datos Privado: ws_async_send_arg_t
```c
typedef struct {
    httpd_handle_t hd;    // Handle del servidor HTTPD
    int fd;               // File descriptor del cliente WebSocket
    char *text;           // Buffer de texto (heap), liberado tras envío
} ws_async_send_arg_t;
```

**Propósito**: Encapsular datos para trabajo encolado asincrónico. El patrón `httpd_queue_work()` es el mecanismo oficial de esp_http_server para enviar datos WS desde tasks externas (es **inseguro** usar `httpd_ws_send_frame()` fuera de la task del httpd).

## API Pública

### 1. Inicialización: ws_server_start()
```c
esp_err_t ws_server_start(const ws_server_config_t *cfg)
```
- **Parámetros**: Estructura con puerto, path del WebSocket, máximo de clientes, callback de mensajes.
- **Validaciones**: 
  - `cfg != NULL` y `cfg->ws_path != NULL`.
  - Crea mutex de estado si no existe.
  - Configura HTTPD con puertos y límites.
  - Registra handler del WebSocket en la ruta especificada.
- **Salida**: `ESP_OK` si éxito, código de error si falla.
- **Log**: Imprime puerto y path en que escucha.

### 2. Finalización: ws_server_stop()
```c
esp_err_t ws_server_stop(void)
```
- Detiene servidor HTTP.
- Limpia referencias globales.
- Seguro llamar si ya está detenido (retorna `ESP_OK`).

### 3. Broadcast: ws_server_broadcast()
```c
esp_err_t ws_server_broadcast(const char *text)
```
- Envía el mismo mensaje a **todos** los clientes WebSocket conectados.
- **Seguro desde cualquier task**: Usa `httpd_queue_work()` para encolar cada envío.
- **Robustez**: Si envío a un cliente falla (ej. se desconectó), solo se loguea (WARN); el broadcast a otros continúa.
- **Manejo de memoria**: Copia `text` en heap por cliente, liberada tras envío.

## Manejo de Handlers

### ws_handler() - Handler Principal HTTP/WebSocket
```c
static esp_err_t ws_handler(httpd_req_t *req)
```

**Fases**:

1. **Handshake (HTTP GET)**
   - esp_http_server ya negoció upgrade antes de llamar este handler.
   - Solo loguea la conexión.

2. **Recepción de datos**
   - Primera llamada a `httpd_ws_recv_frame(buf=NULL)` → obtiene tamaño.
   - Segunda llamada → recibe payload en buffer allocado.
   - Maneja frame CLOSE (desconexión).
   - Ignora frames de control vacíos.

3. **Procesamiento**
   - Si frame es TEXT y existe callback: llama `s_on_message(buffer, len)`.
   - Buffer está null-terminado (más allá de lo requerido por WebSocket).

4. **Liberación de recursos**
   - Buffer allocado se libera tras procesar.

### ws_async_send_exec() - Callback de Envío Asincrónico
```c
static void ws_async_send_exec(void *arg)
```
- Ejecutado **dentro de la task del httpd** (seguro para `httpd_ws_send_frame_async()`).
- Construye frame WebSocket con tipo TEXT.
- Envía usando `httpd_ws_send_frame_async()`.
- Loguea WARN si falla envío a un cliente puntual (no aborta).
- Libera memoria temporal.

## Patrones de Diseño

### 1. Mutex para Estado Compartido
- Protege `s_server` y `s_on_message` contra condiciones de carrera.
- Obtiene lock en `ws_server_start()` y `ws_server_stop()`.
- Broadcast no usa lock (httpd_get_client_list es thread-safe).

### 2. Trabajo Encolado Asincrónico (httpd_queue_work)
```c
httpd_queue_work(s_server, ws_async_send_exec, arg);
```
- **Por qué**: `httpd_ws_send_frame()` no es thread-safe desde tareas externas.
- **Patrón oficial** de esp_http_server para "server-initiated" WS send.
- Colas el trabajo en la task del httpd; se ejecuta cuando httpd está listo.

### 3. Enumeración de Clientes para Broadcast
```c
httpd_get_client_list(s_server, &fds, client_fds);
for (size_t i = 0; i < fds; i++) {
    if (httpd_ws_get_fd_info(s_server, fd) == HTTPD_WS_CLIENT_WEBSOCKET) {
        // Es un cliente WebSocket, encolar envío
    }
}
```
- Filtra sockets HTTP normales de clientes WebSocket activos.

## Configuración (esp_http_server)

```c
httpd_config_t httpd_cfg = HTTPD_DEFAULT_CONFIG();
httpd_cfg.server_port = cfg->port;
httpd_cfg.max_open_sockets = cfg->max_clients > 0 ? cfg->max_clients : 4;
```

- **Puerto**: Configurable en `ws_server_config_t`.
- **Máximo de sockets**: Por defecto 4 si no especificado.
- **URI matching**: `httpd_uri_match_wildcard()` por defecto (suficiente para path fijo).

## Manejo de Errores

| Condición | Comportamiento |
|-----------|-----------------|
| `cfg == NULL` o `cfg->ws_path == NULL` | Retorna `ESP_ERR_INVALID_ARG` |
| `text == NULL` en broadcast | Retorna `ESP_ERR_INVALID_STATE` |
| `httpd_start()` falla | Retorna error de httpd |
| Registro del handler falla | Detiene servidor, retorna error |
| Envío a cliente falla | Loguea WARN, continúa con otros clientes |
| Sin memoria para buffer de envío | Loguea WARN, omite cliente |
| Desconexión durante handshake | Detectada al recibir frame CLOSE |

## Limitaciones Conocidas

1. **Ruta única**: Soporta un único path WebSocket (ej. `/ws`). Si se necesitan múltiples rutas o endpoints REST adicionales, `httpd_uri_match_fn` debe revisarse para evitar colisiones.

2. **Broadcast síncrono parcial**: El broadcast encolado es "best effort"; si la queue de httpd se llena, trabajos posteriores pueden descartarse (documentación de `httpd_queue_work()`).

3. **Sin validación de frames**: No checkea formato/contenido del payload; responsabilidad del callback.

4. **Envío bloqueante desde clientes**: `httpd_ws_send_frame()` es síncrono desde el cliente → puede bloquearse si red lenta.

## Casos de Uso

- **Motor paso a paso**: Enviar telemetría de posición/estado en tiempo real a cliente web.
- **Sensores**: Broadcast de lecturas de temperatura, humedad, ángulo del encoder.
- **Control remoto**: Cliente web envía comandos ("mover 2000 pasos"), servidor los procesa.

## Archivos Relacionados
- `ws_server.h` - Interfaz pública y contratos
- `main.c` - Inicialización del servidor y setup de callback
- `esp_http_server` (ESP-IDF) - Documentación oficial de HTTPD
