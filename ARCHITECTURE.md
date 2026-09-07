# Guía de Arquitectura - MODULAR-PCB-FOCUSHANDLER

## Índice Rápido

Este archivo es una **guía de navegación** de la arquitectura del sistema. Para detalles, consulte los archivos `.md` en cada carpeta.

## Sistema Multi-Núcleo (Dual-Core ESP32-S3)

```
CORE 0 (PRO_CPU)                    CORE 1 (APP_CPU)
├─ preset_cmd_task (legacy test)    ├─ motor_task (STEP/DIR generation)
├─ i2c_sensors_task (AS5600/AHT21B) │  └─ (aislado, tiempo real)
├─ power_monitor_task (CH224K)       │
├─ preset_cmd_task (legacy test)     │
├─ ws_telemetry_task                 │
└─ WiFi/BT stack (Espressif)         └─ (sin interference)
```

**Razón de la separación:**
- WiFi ISRs en Core 0 causarían jitter en generación STEP
- Core 1 dedicado al motor garantiza timing preciso de micropasos

## Tareas Principales y Responsabilidades

| Task | Core | Prioridad | Función | Stack | Documentación |
|------|------|-----------|---------|-------|---|
| motor_task | 1 | 10 (alta) | Genera STEP/DIR al TMC2209 | 4KB | — |
| i2c_sensors_task | 0 | 6 | Lee AS5600 (ángulo) y AHT21B (clima) cada 200ms | 4KB | — |
| power_monitor_task | 0 | 1 (baja) | Monitorea CH224K, libera `power_good_sem` | 4KB | — |
| preset_cmd_task | 0 | 4 | Movimiento de prueba; termina después de encolar | 2KB | — |
| ws_telemetry_task | 0 | 3 | Broadcast JSON cada 5s | 4KB | — |

## Protocolos y Comunicación

### Motor (TMC2209)
- **Bus**: UART1 (GPIO17/18) + GPIO digital (STEP/DIR/EN)
- **Velocidad UART**: 115,200 baud
- **Protocolo**: Trinamic proprietary (4 o 8 bytes + CRC8)
- **Micropasos**: Resolución fija 1/256
- **Velocidad**: 0.05 rev/s (arranque) → 0.30 rev/s (crucero)
- **Documentación**: [tmc2209.md](components/tmc2209/tmc2209.md)

### Sensores I2C (Bus Compartido: GPIO8/9, I2C_NUM_0)
- **AS5600 (Encoder)**: Dir 0x36, 400 kHz, ángulo de 12 bits (0-360°)
- **AHT21B (Clima)**: Dir 0x38, 100 kHz, temperatura/humedad, calibración requerida
- **Polling**: Cada 200 ms desde i2c_sensors_task
- **Documentación**: [as5600.md](components/as5600/as5600.md), [aht21b.md](components/aht21b/aht21b.md)

### Alimentación (CH224K)
- **Voltaje selectivo**: 5V, 9V, 12V, 15V y 20V via USB-C PD; el firmware selecciona 20V en `power_monitor_task`
- **GPIOs**: CFG1/CFG2/CFG3 para selección, PG para Power Good
- **ADC**: Lectura de voltaje de salida (opcional)
- **Documentación**: [ch224k.md](components/ch224k/ch224k.md)

### Red (WiFi + WebSocket + Alpaca)
- **WiFi**: Station mode, SSID/password configurado en main.c
- **WebSocket**: `ws://<IP>:80/ws`, máximo 4 clientes
- **Telemetría**: JSON broadcast cada 5s
- **Alpaca**: HTTP en puerto 11111, management + GET de focuser + PUT Move/Halt; implementación parcial del contrato completo
- **Documentación**: [wifi_init.md](components/wifi_init/wifi_init.md), [ws_server.md](components/ws_server/ws_server.md), [alpaca.md](components/alpaca/alpaca.md)

## Flujo de Datos: Comando → Motor → Posición

```
┌─ USB/Serial ──┐
│ "2000,1"      │
└───────────────┘
        ↓
      productor de comandos
        ↓
  motor_cmd_t {
    .steps = 2000
    .dir = 1
  }
        ↓
  motor_cmd_queue (FreeRTOS)
        ↓
  motor_task
  tmc2209_move_steps()
        ↓
  GPIO STEP/DIR generation
  (2000 micropasos)
        ↓
  focuser_handler_report_move_done(+/-2000)
        ↓
  position_usteps += 2000
        ↓
      ┌─ Alpaca GET ────┐  ┌─ WebSocket broadcast ──┐
  │ get_position()  │  │ telemetry JSON         │
  └─────────────────┘  └────────────────────────┘
```

## Sincronización y Protección

### motor_cmd_queue (FreeRTOS Queue)
- **Longitud**: 4 elementos
- **Tipo**: motor_cmd_t (steps, dir)
- **Productor**: `preset_cmd_task` (legacy), `ws_on_message`, `focuser_handler_move_to()` desde Alpaca
- **Consumidor**: motor_task
- **Propósito**: Desacoplar origen de comandos de ejecución

### power_good_sem (Semáforo Binario)
- **Significado**: Power Good confirmado por CH224K
- **Liberador**: power_monitor_task (una sola vez)
- **Bloqueador**: motor_task (ANTES de tocar UART del motor)
- **Propósito**: Garantizar voltaje disponible antes de energizar driver
- **Comportamiento si PG falla**: motor_task espera indefinidamente (sin energizar motor)

### focuser_handler (Mutex + atomic_bool)
- **Mutex**: Protege estado mutable (position, temperature, is_moving)
- **atomic_bool**: Flag de halt separado (rápido, sin deadlock)
- **Readers**: ws_server telemetry y Alpaca HTTP
- **Writers**: motor_task (position), i2c_sensors_task (temperature)

## Estado Focuser

```c
focuser_state_t {
    position_usteps;        // Inmutable: max_step, step_size_um
    is_moving;              // Reportado por motor_task
    temperature_c;          // Reportado por i2c_sensors_task
    tempcomp_enabled;       // Escribible (API)
    
    motor_cmd_queue;        // Referencia a cola
}
```

**Acceso**:
- `focuser_handler_get_position()` → int64_t
- `focuser_handler_get_is_moving()` → bool
- `focuser_handler_get_temperature()` → float
- `focuser_handler_move_to(pos, timeout)` → esp_err_t (arma comando y encola)

## Flujo de Arranque (app_main)

1. **Configura LEDs de estado** (GPIO10, GPIO12)
2. **Crea motor_cmd_queue** (4 elementos)
3. **Inicializa focuser_handler** (recibe max_step, step_size_um)
4. **Crea power_good_sem** (binario, iniciialmente vacío)
5. **Conecta WiFi** (bloquea hasta OK o fallo)
   - Si OK → inicia ws_server + alpaca_start
   - Si falla → continúa sin control remoto
6. **Crea tasks en orden**:
   - motor_task (Core 1) - se bloquea en power_good_sem
   - i2c_sensors_task (Core 0)
   - power_monitor_task (Core 0) - libera power_good_sem cuando PG=OK
      - preset_cmd_task (Core 0, legacy de prueba)
   - ws_telemetry_task (Core 0) - si WiFi OK

## Notas de Diseño Críticas

### 1. Modularidad Absoluta
Cada componente es autocontenido:
- tmc2209.c: NO sabe de as5600, aht21b, focuser_handler
- as5600.c: NO sabe de tmc2209, aht21b
- aht21b.c: NO sabe de tmc2209, as5600
- focuser_handler.c: SOLO sabe de motor_cmd.h
- Solo main.c interconecta todos

**Beneficio**: Cada módulo se copia a otro proyecto ESP-IDF sin cambios

### 2. Fail-Fast vs Degradación
- **Motor (UART)**: `ESP_ERROR_CHECK()` → aborta si falla (sin motor = sin propósito)
- **Sensores I2C**: Marcan `ready=false` si fallan → sistema continúa
- **WiFi/WebSocket**: Fallos no paran motor → solo pierde control remoto

### 3. Un solo bus I2C

`i2c_sensors_task` crea un único bus `I2C_NUM_0` en SDA GPIO8/SCL GPIO9 y registra AS5600 y AHT21B sobre él. Las lecturas se hacen secuencialmente en la misma task. `I2C_NUM_1` no se utiliza actualmente.

### 4. SIN i2c_master_probe()
Porque genera falsos timeouts en esta versión de ESP-IDF.
Usa transacciones reales como probe:
- AS5600: Lee STATUS
- AHT21B: Lee STATUS o asegura calibración

### 5. Reintentos Condicionados
Reintentos viven en orquestador (main.c), no en drivers:
- Drivers: Retornan error/OK
- main.c: Cuenta fallos, reintenta con delay, marca como no disponible

## Comandos y Protocolos

### USB/Serial Console (legacy opcional)
```
<pasos>,<dir>\n
Ejemplos:
  2000,1      → 2000 micropasos, cierra (dir=1)
  51200,0     → 1 vuelta completa, abre (dir=0)
  
Max: 204,800 micropasos por comando
```

### WebSocket JSON
```json
{
  "cmd": "move",
  "steps": 51200,
  "dir": 0
}
```

**Respuesta (ACK)**:
```json
{
  "ack": "queued",
  "steps": 51200,
  "dir": 0
}
```

**Telemetría (broadcast cada 5s)**:
```json
{
  "power_good": true,
  "encoder_angle": 45.5,
  "temperature": 25.3
}
```

### Alpaca HTTP (activo)
```
GET http://<IP>:11111/api/v1/focuser/0/position
PUT http://<IP>:11111/api/v1/focuser/0/move
PUT http://<IP>:11111/api/v1/focuser/0/halt
```

## Configuración de Hardware (main.c)

### Pines TMC2209
```c
PIN_STEP = 5,  PIN_DIR = 6,   PIN_EN = 21
PIN_UART_RX = 18,  PIN_UART_TX = 17  (UART_NUM_1)
MS1 = GPIO1, MS2 = GPIO2 (fijos a 0V)
```

### Pines I2C Compartido
```c
SDA = GPIO8,  SCL = GPIO9  (I2C_NUM_0)
AS5600 (0x36) + AHT21B (0x38)
```

### Pines CH224K
```c
CFG1 = GPIO38, CFG2 = GPIO48, CFG3 = GPIO47
Power Good = GPIO15
Voltage Sense ADC = GPIO3
```

### LEDs de Estado
```c
WS Status LED = GPIO10  (ON si cliente WS conectado)
Motor Status LED = GPIO12  (ON si motor_task vivo)
```

## Constantes Globales Importantes

| Constante | Valor | Uso |
|-----------|-------|-----|
| MICROSTEPS | 256 | Resolución fija 1/256 |
| FULL_STEPS_PER_REV | 200 | Pasos por vuelta |
| FOCUSER_MAX_STEP_USTEPS | 206300 | Límite de rango |
| FOCUSER_STEP_SIZE_UM | 0.239 | Micrómetros por micropaso |
| I2C_SENSOR_POLL_PERIOD_MS | 200 | Lectura sensores cada 200ms |
| WS_TELEMETRY_PERIOD_MS | 5000 | Broadcast cada 5s |
| TARGET_REV_PER_SEC_START | 0.05 | Velocidad arranque (~10 RPM) |
| TARGET_REV_PER_SEC_CRUISE | 0.30 | Velocidad crucero (~18 RPM) |

## Archivos de Referencia

- **[main.md](main/main.md)** — Orquestación, flow de arranque
- **[tmc2209.md](components/tmc2209/tmc2209.md)** — Motor UART + GPIO
- **[as5600.md](components/as5600/as5600.md)** — Encoder magnético
- **[aht21b.md](components/aht21b/aht21b.md)** — Sensor clima
- **[ch224k.md](components/ch224k/ch224k.md)** — Power Delivery
- **[focuser_handler.md](components/focuser_handler/focuser_handler.md)** — State machine
- **[wifi_init.md](components/wifi_init/wifi_init.md)** — WiFi
- **[ws_server.md](components/ws_server/ws_server.md)** — WebSocket
- **[alpaca.md](components/alpaca/alpaca.md)** — ASCOM Alpaca

## Estado y Próximos Pasos

- Alpaca management: implementado.
- Alpaca GET del focuser: implementado.
- Alpaca PUT `move`/`halt`: implementado.
- Pendiente para conformidad completa: PUT `connected`, PUT `tempcomp`, metadatos del dispositivo y pruebas con cliente Alpaca real.
- Próximos pasos opcionales: rate limiting, estadísticas, homing automático y retirar canales legacy si ya no se necesitan.

## Código Legacy u Opcional

No son necesarios para operar mediante Alpaca o WebSocket:

| Elemento | Ubicación | Uso actual |
|---|---|---|
| `preset_cmd_task()` y `PRESET_*` | `main/main.c` | Movimiento automático fijo de prueba |
| Bloque `mqtt_task` comentado | final de `main/main.c` | Punto de extensión no activo |

Estos elementos pueden retirarse después de validar que el arranque no necesita consola local ni movimiento de prueba. No son legacy `motor_cmd_queue`, `motor_task`, `focuser_handler`, `ws_server` ni `alpaca`: forman parte de las rutas activas.

## Troubleshooting Rápido

| Síntoma | Posible Causa | Verificar |
|---------|---|---|
| Motor no se mueve | PG no OK | `g_last_pg_ok` en logs, CH224K |
| UART timeout | Cable/conexión | GPIO17/18, 115200 baud |
| Sensor no responde | Bus I2C muerto | Voltaje, pull-up resistencias |
| WiFi no conecta | SSID/password | main.c #define WIFI_SSID/PASSWORD |
| Stack overflow | Task stack insuficiente | Aumentar stack en xTaskCreatePinnedToCore |

