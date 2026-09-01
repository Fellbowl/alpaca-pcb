# main.c - Orquestación Principal

## Propósito
Archivo central que orquesta toda la arquitectura del ESP32-S3 para el MODULAR-PCB-FOCUSHANDLER. Coordina tres tareas de FreeRTOS independientes (motor_task, i2c_sensors_task, cmd_input_task) y gestiona la comunicación entre módulos periféricos (TMC2209, AS5600, AHT21B, CH224K) mediante estructuras de datos compartidas desacopladas.

## Arquitectura General

### Distribución de Núcleos
- **Core 0 (PRO_CPU)**: 
  - `cmd_input_task` - Lee comandos por USB Serial/JTAG
  - `mqtt_task` (futuro) - Recibirá comandos MQTT
  - `i2c_sensors_task` - Gestiona sensores en buses I2C
  - Reservado para stack WiFi/BT de Espressif

- **Core 1 (APP_CPU)**: 
  - `motor_task` - Tarea exclusiva para control del TMC2209
  - Sin competencia de ISR de radio, garantiza timing preciso del motor

### Razón de la Distribución

1. **Motor en Core 1 aislado**: El stack WiFi de Espressif está anclado al core 0. Las ISRs de radio causarían jitter en `tmc2209_move_steps()` (bit-bang de GPIO con precisión de microsegundos), resultando en pérdida de sincronismo o vibración del motor.

2. **Sensores I2C en Core 0**: AS5600 y AHT21B usan dos buses I2C físicamente independientes (I2C_NUM_0 y I2C_NUM_1). Si un bus se bloquea momentáneamente, no afecta al Core 1.

3. **Desacoplamiento mediante colas**: `motor_cmd_queue` es el único contrato entre entrada de comandos (consola USB, MQTT) y ejecución. `cmd_input_task` y futuro `mqtt_task` solo saben armar comandos; `motor_task` nunca cambia.

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
- **I2C_NUM_1**: Disponible para futuros sensores

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

## Notas de Diseño Críticas

### 1. Fail-Fast vs Degradación Controlada
- `tmc2209_init()` usa `ESP_ERROR_CHECK()` internamente: si el UART no funciona, aborta el firmware (sin motor no hay propósito).
- Si un sensor I2C falla: `i2c_sensors_task` loguea el error, marca sensor como `ready=false`, pero continúa ejecutándose. Un sensor caído no tumba el sistema.

### 2. Reintentos de Arranque
- `motor_task` reintenta indefinidamente con backoff de 1s (loguea cada 10 intentos) hasta conseguir comunicación válida con TMC2209.
- `i2c_retry_detect()` en main.c maneja la lógica de reintentos N veces con espera fija (orquestación, no parte del protocolo).

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
void cmd_input_task(void *arg);

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

## Compilación y Ejecución

El proyecto usa **ESP-IDF con CMake**. Configuración:
- Target: `esp32s3`
- Entrada de comandos: USB Serial/JTAG nativo (sin chip USB-UART externo)
- WebSocket: Por WiFi (futuro, se inicializa en main)

## Archivos Relacionados
- `motor_cmd.h` - Definición de estructura de comandos
- `sdkconfig` - Configuración de ESP-IDF
- `CMakeLists.txt` - Compilación
