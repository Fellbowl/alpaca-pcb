# main.c - Orquestación Principal

## Propósito
Archivo orquestador del sistema ESP32-S3. Coordina múltiples tareas FreeRTOS en dos núcleos, inicializa periféricos (TMC2209, AS5600, AHT21B, CH224K), gestiona Wi-Fi, servidor WebSocket, protocolo Alpaca, y mantiene desacoplamiento modular mediante colas y semáforos. No contiene lógica de protocolos específicos—solo arma configuración y coordina tasks.

## Arquitectura General

### Distribución de Núcleos
- **Core 0 (PRO_CPU)**: 
  - `mqtt_task` (no implementada) - Punto de extensión futuro
  - `i2c_sensors_task` - Gestiona sensores en un bus I2C compartido
  - Reservado para stack WiFi/BT de Espressif

- **Core 1 (APP_CPU)**: 
  - `motor_task` - Tarea exclusiva para control del TMC2209
  - Sin competencia de ISR de radio, garantiza timing preciso del motor

### Razón de la Distribución

1. **Motor en Core 1 aislado**: El stack WiFi de Espressif está anclado al core 0. Las ISRs de radio causarían jitter en `tmc2209_move_steps()` (bit-bang de GPIO con precisión de microsegundos), resultando en pérdida de sincronismo o vibración del motor.

2. **Sensores I2C en Core 0**: AS5600 y AHT21B se registran sobre el mismo bus físico creado por `i2c_sensors_task` (`I2C_NUM_0`, SDA GPIO8, SCL GPIO9). Las lecturas se ejecutan secuencialmente en la misma task, por lo que no hay carrera entre sensores y un bloqueo no afecta al Core 1.

3. **Desacoplamiento mediante colas**: `motor_cmd_queue` es el contrato común entre las interfaces activas y el motor. WebSocket y Alpaca son las interfaces remotas activas; MQTT todavía no está implementado.

4. **Semáforo de Power Good (PG)**: `power_good_sem` desacopla `motor_task` de `power_monitor_task`. Motor solo energiza el driver después de confirmar PG.

## Módulos Incluidos

| Módulo | Función | Responsabilidad |
|--------|---------|-----------------|
| tmc2209.c | Driver del motor paso a paso | UART del motor, registros, CRC |
| as5600.c | Driver del encoder magnético | I2C del encoder, lectura de ángulo |
| aht21b.c | Driver sensor temperatura/humedad | I2C del sensor, calibración |
| ch224k.c | Driver controlador de voltaje | GPIOs de configuración, selección de voltaje |
| focuser_handler.c | Lógica del focuser | Estado único de verdad, posición, tempcomp |
| ws_server.c | Servidor WebSocket | HTTP, handshake WS, broadcast |
| wifi_init.c | Inicialización de WiFi | Stack WiFi, eventos, reconexión |
| alpaca.c | Servidor ASCOM Alpaca | Management, estado del focuser, Move y Halt |

## Configuración de Hardware

### Pines TMC2209
- MS1 (GPIO1), MS2 (GPIO2) - Configuración de micropasos (fijados)
- RX/TX (GPIO18/GPIO17) → UART_NUM_1 con resistencia ~1k → PDN_UART
- STEP (GPIO5), DIR (GPIO6), EN (GPIO21, activo en bajo)

### Pines de Control
- Power Good (PG) → GPIO específico en CH224K (entrada con pull-up)
- Status LED WS → GPIO10
- Status LED Motor → GPIO12

### Sensores I2C
- **I2C_NUM_0**: AS5600 (0x36) + AHT21B (0x38) en bus compartido
- **I2C_NUM_1**: No utilizado por el firmware actual

## Protocolo de Comandos

### Entrada por Consola USB Serial
```
<pasos>,<direccion>\n
```

**Ejemplos:**
- `2000,1` → 2000 micropasos, DIR=1 (cierra focuser)
- `51200,0` → 51200 micropasos (200 pasos completos = 1 vuelta a 1/256), DIR=0 (abre)

**Resolución fija**: 1/256 micropasos (mres=0, salida de CHOPCONF)

**Convención de dirección**:
- DIR=0: Abre focuser (posición aumenta)
- DIR=1: Cierra focuser (posición disminuye)

## Tareas FreeRTOS Principales

| Task | Core | Prioridad | Responsabilidad |
|------|------|-----------|-----------------|
| motor_task | 1 | 10 (alta) | Genera pulsos STEP al TMC2209, reporta posición a focuser_handler |
| i2c_sensors_task | 0 | 6 | Lee AS5600 (ángulo) y AHT21B (temperatura) periódicamente, reporta a focuser_handler |
| power_monitor_task | 0 | 1 (baja) | Monitorea CH224K, libera power_good_sem cuando PG=OK |
| preset_cmd_task | 0 | 4 | Movimiento de prueba/legacy: encola un movimiento predefinido y termina |
| ws_telemetry_task | 0 | 3 | Broadcast JSON con estado cada 5s si cliente WS conectado |

## Notas de Diseño Críticas

### 1. Fail-Fast vs Degradación Controlada
- **TMC2209 (UART motor)**: `ESP_ERROR_CHECK()` en `tmc2209_init()` → aborta si falla (sin motor no hay propósito).
- **Sensores I2C**: Si AS5600 o AHT21B no responden → marcan `ready=false`, task continúa, sistema sigue operativo.
- **WiFi/WebSocket**: Fallos no detienen motor—solo pierde control remoto.

### 2. Reintentos de Arranque
- **motor_task**: Reintenta indefinidamente (backoff 1s, log cada 10 intentos) hasta comunicación TMC2209 válida.
- **Sensores I2C**: `i2c_retry_detect()` → máximo 8 intentos × 100ms = 800ms total antes de desistir.
- Reintentos son **orquestación**, no protocolo—viven en main.c, no en drivers.

### 3. Detección de Sensores I2C
- **NO usa** `i2c_master_probe()` (genera falsos timeouts con sensores válidos en esta versión de ESP-IDF).
- Usa la misma lectura/transacción real que el loop operativo (AS5600 lee STATUS, AHT21B asegura calibración).

### 4. Condicionamiento de Arranque a PG
- `motor_task` bloquea en `power_good_sem` **ANTES** de tocar UART del TMC2209.
- `power_monitor_task` da el semáforo binario una única vez, cuando `ch224k_read_power_good()` reporta `pg_ok == true` por primera vez.
- Si `power_monitor_task` nunca inicializa CH224K, se autodestruye sin dar semáforo: `motor_task` queda esperando indefinidamente (logueando cada 2s), el driver **nunca se energiza** (comportamiento deliberado sin PG confirmado).
- **Limitación**: Vigila PG solo en arranque. Para supervisión continua durante operación normal sería necesario `EventGroupHandle_t` con bit set/clear.

### 5. Modularidad Constructiva
- Cada módulo (tmc2209, as5600, aht21b, focuser_handler) es **autocontenido**: solo depende de headers de ESP-IDF/FreeRTOS, nunca de otros módulos.
- Solo `main.c` los conoce a todos y los conecta.
- **Ventajas prácticas**:
  - Cualquier módulo se copia a otro proyecto ESP-IDF sin dependencias adicionales.
  - Un bug en AHT21B no puede filtrarse al código de AS5600 o TMC2209 por construcción.
  - Se pueden probar independientemente.

### 6. focuser_handler como Fuente Única de Verdad
- `focuser_handler` **nunca** lee GPIO/UART/I2C directamente.
- No crea `motor_cmd_queue` (la recibe creada).
- `motor_task` le **REPORTA** el resultado real de cada movimiento (con signo, incluyendo dirección) **DESPUÉS** de que `tmc2209_move_steps()` retorna.
- **No asume** que todo movimiento pedido se completó integro (respeta actualizaciones parciales).

### 7. Observabilidad en Stack
- Cada task guarda su `TaskHandle_t` y loguea `uxTaskGetStackHighWaterMark()` una vez tras inicializar.
- Permite ajustar los 4096 words de stack con datos reales (importante: ESP32-S3 Supermini sin PSRAM).

## Patrón de Código

```c
// 1. DECLARACIONES de funciones (prototipos) antes de app_main()
void motor_task(void *arg);
void i2c_sensors_task(void *arg);

// 2. app_main() arriba, se lee de un vistazo
void app_main() {
    // Inicialización y creación de tasks
}

// 3. DEFINICIONES completas después de app_main(), agrupadas por módulo
static void motor_task(void *arg) { ... }
static void i2c_sensors_task(void *arg) { ... }
```

## Estado Compartido

### motor_cmd_queue
- Cola FreeRTOS que encola comandos de movimiento.
- Único contrato entre entrada (USB, futuro MQTT) y ejecución.

### power_good_sem
- Semáforo binario que bloquea `motor_task` hasta confirmar PG.
- Garantiza que el driver no se energiza sin verificación de voltaje.

### focuser_handler (mutex protegido)
- Posición absoluta (position_usteps)
- Estado de movimiento (is_moving)
- Temperatura (temperature_c)
- Configuración de tempcomp

## Flujo de Arranque (app_main)

1. **Configura LEDs de estado** (GPIO10, GPIO12)
2. **Crea motor_cmd_queue** (4 elementos, tipo motor_cmd_t)
3. **Inicializa focuser_handler** con límites lógicos del focuser
4. **Crea power_good_sem** (semáforo binario)
5. **Conecta WiFi** (bloquea hasta conectado o agota reintentos)
   - Si WiFi OK → inicia ws_server y alpaca_start
   - Si falla → continúa sin control remoto
6. **Crea tasks en orden**:
   - motor_task (Core 1, prioridad 10)
   - i2c_sensors_task (Core 0, prioridad 6)
   - power_monitor_task (Core 0, prioridad 1)
  - preset_cmd_task (actualmente activa como movimiento de prueba; legacy)
   - ws_telemetry_task (si WiFi OK)

## Constantes de Configuración Principales

| Parámetro | Valor | Uso |
|-----------|-------|-----|
| MICROSTEPS | 256 | Resolución fija 1/256 |
| FULL_STEPS_PER_REV | 200 | Pasos completos por revolución |
| MAX_STEPS_PER_COMMAND | 204800 | Límite por comando (51200 × 4) |
| MOTOR_CMD_QUEUE_LEN | 4 | Elementos en cola de comandos |
| FOCUSER_MAX_STEP_USTEPS | 206300 | Rango máximo de movimiento |
| FOCUSER_STEP_SIZE_UM | 0.239 | Micrómetros por micropaso |
| I2C_SENSOR_POLL_PERIOD_MS | 200 | Lectura de sensores cada 200ms |
| WS_TELEMETRY_PERIOD_MS | 5000 | Broadcast WebSocket cada 5s |
| TARGET_REV_PER_SEC_START | 0.05 | Velocidad de arranque (~10 RPM) |
| TARGET_REV_PER_SEC_CRUISE | 0.30 | Velocidad crucero (~18 RPM) |

## Compilación y Ejecución

```bash
# Configurar target y build
. $HOME/esp/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build

# Flashear y monitorear
idf.py -p /dev/ttyACM0 flash monitor
```

## Archivos Relacionados
- `motor_cmd.h` - Contrato de cola de comandos (struct motor_cmd_t)
- `focuser_handler.h` - API de state del focuser
- Drivers: `tmc2209.h`, `as5600.h`, `aht21b.h`, `ch224k.h`
- `ws_server.h`, `wifi_init.h`, `alpaca.h` - Networking
- `sdkconfig` - Configuración ESP-IDF
- `CMakeLists.txt` - Build rules
