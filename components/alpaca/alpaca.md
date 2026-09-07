# alpaca.c - Servidor HTTP ASCOM Alpaca

## Propósito

Implementa el servidor HTTP del protocolo ASCOM Alpaca para exponer el focuser por red. El módulo usa `esp_http_server`, mantiene su propia task y conecta las operaciones Alpaca con `focuser_handler`.

## Estado Actual

El componente está integrado en `main.c` y se inicia después de una conexión WiFi exitosa:

```c
wifi_init_sta() -> ws_server_start() -> alpaca_start()
```

El servidor escucha en el puerto configurado por `ALPACA_HTTP_PORT` (actualmente `11111`). Usa un segundo `ctrl_port` interno de HTTPD para coexistir con el servidor WebSocket del puerto 80.

## Endpoints Implementados

### Management

| Método | Ruta | Función |
|---|---|---|
| GET | `/management/apiversions` | Versiones de API disponibles |
| GET | `/management/v1/description` | Nombre, fabricante, versión y ubicación |

### Focuser

Base: `/api/v1/focuser/0`

| Método | Ruta | Fuente o acción |
|---|---|---|
| GET | `/connected` | Estado de conexión lógica |
| GET | `/position` | Posición absoluta en micropasos |
| GET | `/ismoving` | Estado de movimiento |
| GET | `/absolute` | Indica soporte de posición absoluta |
| GET | `/maxstep` | Límite máximo de posición |
| GET | `/maxincrement` | Incremento máximo permitido |
| GET | `/stepsize` | Tamaño de micropaso en micrómetros |
| GET | `/tempcompavailable` | Disponibilidad de compensación térmica |
| GET | `/tempcomp` | Estado de compensación térmica |
| GET | `/temperature` | Temperatura actual |
| PUT | `/move` | Solicita movimiento absoluto |
| PUT | `/halt` | Solicita parada controlada |

Los GET consultan el estado mediante funciones públicas de `focuser_handler`. El PUT `/move` llama a `focuser_handler_move_to()`, que valida el rango, calcula el delta y encola un `motor_cmd_t` relativo. El PUT `/halt` usa el flag atómico de parada, fuera de la cola normal.

## Formato de Respuesta

Las operaciones `/api/v1/...` usan la envoltura Alpaca:

```json
{
  "ClientTransactionID": 1,
  "ServerTransactionID": 1,
  "ErrorNumber": 0,
  "ErrorMessage": "",
  "Value": 12345
}
```

Las operaciones PUT responden sin el campo `Value`, conforme al formato de operaciones de escritura. Los parámetros PUT llegan como `application/x-www-form-urlencoded`, por ejemplo:

```text
Position=12345&ClientTransactionID=5
```

No se usa JSON para leer los parámetros de `/move` o `/halt`.

## Arquitectura Interna

- `alpaca_start()` copia los textos de configuración a buffers internos.
- Crea `alpaca_http_task` con el puerto, prioridad, core y stack recibidos.
- La task arranca HTTPD, registra 14 URI handlers y permanece viva para registrar heap periódicamente.
- `alpaca.c` incluye `focuser_handler.h`; `alpaca.h` mantiene una interfaz independiente y solo expone `alpaca_config_t`.
- El dispositivo es fijo: `focuser/0`, porque el firmware controla un único focuser.

## Límites y Validaciones

- Un único servidor Alpaca por firmware.
- Un único dispositivo Alpaca (`focuser/0`).
- Body PUT máximo: 128 bytes; un body mayor se rechaza, no se trunca.
- Los valores inválidos se responden con códigos Alpaca estándar o del rango de driver (`0x500`).
- Si el servidor ya está arrancado, `alpaca_start()` devuelve `ESP_ERR_INVALID_STATE`.
- Si no hay memoria para el argumento del puerto, devuelve `ESP_ERR_NO_MEM`.

## Integración con el Estado del Focuser

```text
Cliente Alpaca
    |
    +-- GET position/temperature/ismoving --> focuser_handler getters
    |
    +-- PUT move --------------------------> focuser_handler_move_to()
    |                                           |
    |                                           +--> motor_cmd_queue
    |
    +-- PUT halt --------------------------> atomic halt flag
                                                |
                                                +--> tmc2209_move_steps()
```

El servidor no controla GPIO, UART o I2C directamente. El movimiento físico sigue siendo responsabilidad exclusiva de `motor_task` y `tmc2209`.

## Relación con WebSocket

Alpaca y WebSocket son servidores HTTP independientes:

- Alpaca: puerto 11111, API estándar de control de focuser.
- WebSocket: puerto 80, comandos JSON relativos, ACK y telemetría.

Ambos pueden coexistir porque `main.c` inicia los dos con `ctrl_port` distintos. No deben confundirse sus contratos: Alpaca trabaja con posición absoluta y WebSocket conserva el canal relativo/bajo nivel.

## Dependencias

- `esp_http_server`
- FreeRTOS
- `focuser_handler`
- cJSON
- ESP-IDF logging/system APIs

## Código Legacy Relacionado

Este componente reemplaza la necesidad de usar interfaces locales o clientes específicos para controlar el focuser por red. Sin embargo, WebSocket no es legacy: sigue siendo una interfaz activa de telemetría y comandos relativos. La interfaz USB de texto y los presets automáticos están documentados como legacy en `ARCHITECTURE.md` porque no son necesarios para operar mediante Alpaca o WebSocket.
