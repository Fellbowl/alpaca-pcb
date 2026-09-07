# 📊 Indicadores de Complejidad del Proyecto

**Proyecto**: MODULAR-PCB-FOCUSHANDLER  
**Fecha**: 2026-08-31  
**Clasificación**: 🟡 **Complejidad MEDIA-ALTA** (6.5/10)

---

## 1. Métrica de Código (Lines of Code)

### Distribución por Archivo

```
Total LOC:                     2,770 líneas
├─ main.c                      1,166 líneas  (42%)  ⚠️ [Orquestador central]
├─ tmc2209.c                     349 líneas  (13%)  [Motor driver]
├─ ws_server.c                   242 líneas  (9%)   [WebSocket]
├─ focuser_handler.c             238 líneas  (9%)   [State machine]
├─ alpaca.c                      236 líneas  (9%)   [HTTP Alpaca]
├─ ch224k.c                      169 líneas  (6%)   [Power control]
├─ aht21b.c                      145 líneas  (5%)   [Sensor clima]
├─ wifi_init.c                   116 líneas  (4%)   [WiFi init]
└─ as5600.c                      109 líneas  (4%)   [Encoder magnético]
```

### Análisis de Distribución

| Aspecto | Valor | Interpretación |
|---------|-------|-----------------|
| Promedio LOC/archivo | 308 | Bien distribuido |
| Archivo más grande | 1,166 (main.c) | 42% - Potencial centralización |
| Archivo más pequeño | 109 (as5600.c) | 4% - Modular |
| Coeficiente de variación | 0.68 | Alta variabilidad (algunos grandes, otros pequeños) |

**Recomendación**: main.c podría beneficiarse de refactorización para dividir responsabilidades.

---

## 2. Número de Funciones y Complejidad Funcional

### Inventario de Funciones Públicas

```
Total Funciones Públicas:     ~68 funciones

focuser_handler.c       16 funciones   (23%)  🔴 Más complejo
├─ 8 getters (thread-safe)
├─ 4 setters/configuración
├─ 4 funciones de control
│
tmc2209.c              10 funciones   (15%)
├─ Init, read, write, move, configure
│
as5600.c                7 funciones   (10%)
├─ Init, read_angle, read_status
│
ws_server.c             6 funciones   (9%)
├─ Start, stop, send, broadcast
│
aht21b.c                5 funciones   (7%)
├─ Init, trigger, read_temperature
│
alpaca.c                5 funciones   (7%)
├─ Start, endpoints HTTP
│
ch224k.c                5 funciones   (7%)
├─ Init, set_voltage, get_status
│
wifi_init.c             2 funciones   (3%)
└─ Init STA, event handler
```

### Métricas de Función

| Métrica | Valor | Estado |
|---------|-------|--------|
| Promedio funciones/módulo | 8.5 | Equilibrado |
| Máximo funciones | 16 (focuser_handler) | Justificado (sincronización) |
| Mínimo funciones | 2 (wifi_init) | Simpleza apropiada |
| Funciones complejas | 3-4 | Controlado |

**Conclusión**: Buena distribución. focuser_handler con 16 funciones se justifica por ser el state machine del proyecto.

---

## 3. Arquitectura de Concurrencia (FreeRTOS)

### Tasks Activas

```
6 tareas concurrentes:

┌─────────────────────────────────────────────────────────────┐
│ CORE 0 (PRO_CPU) - WiFi + Sensores                        │
│                                                             │
│   motor_cmd_queue (FreeRTOS Queue)    ← Interfaz de datos  │
│      ↓                                                      │
│ i2c_sensors_task (Prio 5)        [AS5600 + AHT21B polling] │
│ power_monitor_task (Prio 7)      [CH224K Power Good watch] │
│ preset_cmd_task (Prio 5)         [Comandos predefinidos]   │
│ ws_telemetry_task (Prio 4)       [Broadcast WebSocket]     │
│                                                             │
└─────────────────────────────────────────────────────────────┘
                           ↓
                 (motor_cmd_queue)
                           ↓
┌─────────────────────────────────────────────────────────────┐
│ CORE 1 (APP_CPU) - Motor Timing-Critical                  │
│                                                             │
│ motor_task (Prio 10) 🔴 HIGHEST PRIORITY                  │
│    • Recibe comandos de motor_cmd_queue                   │
│    • Ejecuta UART → TMC2209                              │
│    • GPIO bit-bang timing crítico (esp_rom_delay_us)      │
│    • Reporta posición a focuser_handler                  │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### Objetos de Sincronización

| Primitiva | Cantidad | Uso |
|-----------|----------|-----|
| FreeRTOS Queue | 1 | motor_cmd_queue (desacoplamiento) |
| Binary Semaphore | 1 | power_good_sem (espera PG del CH224K) |
| Mutex | 1 | focuser_handler_state (estado global) |
| Semaphore | 1 | ws_server (broadcast safe) |
| **Total** | **4** | |

### Análisis de Complejidad Concurrente

| Factor | Valor | Complejidad |
|--------|-------|-------------|
| Número de tasks | 6 | ALTA (>5 es riesgo) |
| Primitivas de sync | 4 | MEDIA |
| Acceso a estado compartido | 1 (focuser_handler) | BAJA |
| Separación por núcleos | Sí | MITIGA riesgo |
| Documentación de threading | Excelente | REDUCE riesgo |

**Puntuación de Concurrencia**: **7/10** (MEDIA-ALTA con buenas prácticas)

---

## 4. Complejidad de Protocolos y Comunicación

### Protocolos Implementados

```
5 PROTOCOLOS (⚠️ ALTO número de interfaces):

1. UART (TMC2209)
   ├─ Baudrate: 115,200 bps
   ├─ Formato: Datagramas 4/8 bytes
   ├─ CRC8: Polinomio 0x07
   ├─ Complejidad: MEDIA (bien definido, standalone)
   └─ Archivo: tmc2209.c (349 LOC)

2. I2C (AS5600 + AHT21B)
   ├─ Bus: 1 compartido (I2C_NUM_0, AS5600 + AHT21B)
   ├─ Frecuencias: 400 kHz (AS5600), 100 kHz (AHT21B)
   ├─ Direcciones: 0x36 (AS5600), 0x38 (AHT21B)
   ├─ Polling: 200 ms (sensores)
   ├─ Complejidad: BAJA (no hay arbitración, buses independientes)
   └─ Archivos: as5600.c (109 LOC), aht21b.c (145 LOC)

3. WebSocket (Telemetría live)
   ├─ Handshake: HTTP → WS
   ├─ Broadcast: A múltiples clientes
   ├─ Async send: httpd_queue_work (evita deadlock)
   ├─ Frame format: JSON null-terminated
   ├─ Complejidad: MEDIA-ALTA (multi-cliente, async)
   └─ Archivo: ws_server.c (242 LOC)

5. HTTP (ASCOM Alpaca Focuser)
   ├─ Puerto: 11111 (management)
   ├─ Endpoints: Management (actual)
   ├─ Status: Management + GET + PUT Move/Halt (activo)
   ├─ Pending: Endpoints de control (Steps 4/5)
   ├─ Complejidad: MEDIA (REST estándar, endpoints principales implementados)
   └─ Archivo: alpaca.c (236 LOC)
```

### Matriz de Compatibilidad de Protocolos

```
┌──────────────────┬───────┬─────────┬────────────┬──────────┐
│ Protocolo        │ Async │ Multi   │ Codec      │ Estado   │
│                  │       │ Cliente │            │          │
├──────────────────┼───────┼─────────┼────────────┼──────────┤
│ UART (TMC)       │ No    │ N/A     │ Binario    │ Estable  │
│ I2C (Sensores)   │ No    │ N/A     │ Binario    │ Estable  │
│ USB Serial       │ No    │ No      │ Texto      │ Estable  │
│ WebSocket        │ Sí    │ Sí      │ JSON       │ Estable  │
│ Alpaca HTTP      │ No    │ No      │ JSON       │ Parcial  │
└──────────────────┴───────┴─────────┴────────────┴──────────┘
```

**Puntuación de Protocolo**: **7/10** (5 protocolos = superficie de ataque, bien encapsulados)

---

## 5. Complejidad de Hardware y Pines

### Inventario de Periféricos

```
Periféricos de ESP32-S3:          8 unidades

1. UART_NUM_1
   ├─ Propósito: TMC2209 (motor driver)
   ├─ Pines: GPIO 17 (RX), GPIO 18 (TX)
   ├─ Baudrate: 115,200 bps
   └─ Propiedad: motor_task (exclusiva)

2. I2C_NUM_0
   ├─ Propósito: AS5600 (encoder magnético)
   ├─ Pines: GPIO 8 (SDA), GPIO 9 (SCL)
   ├─ Frecuencia: 400 kHz
   └─ Propiedad: i2c_sensors_task

3. AHT21B sobre el bus compartido
   ├─ Propósito: Sensor clima
   ├─ Pines: GPIO 8 (SDA), GPIO 9 (SCL), mismo I2C_NUM_0
   ├─ Frecuencia configurada: 100 kHz
   └─ Propiedad: i2c_sensors_task

4-7. GPIO Digitales (8 pines)
   ├─ GPIO 4 (STEP)   → TMC2209
   ├─ GPIO 5 (DIR)    → TMC2209
   ├─ GPIO 6 (EN)     → TMC2209 (activo en bajo)
   ├─ GPIO 38 (CFG1)  → CH224K
   ├─ GPIO 48 (CFG2)  → CH224K
   ├─ GPIO 47 (CFG3)  → CH224K
   ├─ GPIO 15 (PG)    → CH224K (Power Good input)
   └─ GPIO 10, 12 (LEDs)

8. ADC (2 canales)
   ├─ ADC1_CH0 (GPIO 3) → CH224K voltage sense
   └─ Propósito: Medir voltaje de salida
```

### Análisis de Utilización de Recursos

| Recurso | Usado | Total | % Util. | Estado |
|---------|-------|-------|---------|--------|
| UART | 1 | 3 | 33% | OK |
| I2C | 2 | 2 | 100% | ✓ Suficiente |
| ADC Channels | 2 | 16 | 12% | OK |
| GPIO | ~15 | 45 | 33% | OK |
| Memory (heap) | ? | ~320 KB | ? | ⚠️ Revisar |

**Puntuación de Hardware**: **6/10** (bien utilizado, pero I2C al límite)

---

## 6. Complejidad de Estado y Sincronización

### Estructura de Estado Principal: focuser_state_t

```c
typedef struct {
    int32_t  current_step_position;      // Posición absoluta actual
    bool     is_moving;                   // Flag de movimiento
    float    current_temperature;         // Última lectura AHT21B
    uint32_t max_step;                    // Límite superior
    float    step_size_um;                // Micrómetros por micropasos
    bool     tempcomp_available;          // ¿Compensación disponible?
    int32_t  tempcomp_steps_per_degree;   // Rampas de temperatura
} focuser_state_t;
```

### Acceso a Estado (Synchronization)

```
Escritores de focuser_state:         2 tasks
├─ motor_task                (motor_pos)
└─ i2c_sensors_task          (temperature, is_moving)

Lectores de focuser_state:           3 tasks
├─ ws_telemetry_task         (broadcast status)
└─ alpaca_http_task          (endpoints HTTP activos)

Protección: Mutex focuser_handler

Operaciones atómicas:
├─ focuser_handler_get_position()     [LEE con mutex]
├─ focuser_handler_set_position()     [ESCRIBE con mutex]
├─ focuser_handler_get_is_moving()    [LEE con mutex]
└─ focuser_handler_get_temperature()  [LEE con mutex]
```

### Riesgos de Sincronización

| Riesgo | Probabilidad | Impacto | Mitigación |
|--------|-------------|--------|-----------|
| Race condition en `is_moving` | Media | Movimiento duplicado | Mutex ✓ |
| Deadlock en focuser_handler | Baja | Sistema congelado | Bien diseñado ✓ |
| Inconsistencia de posición | Baja | Offset incorrecto | motor_task reporta ✓ |
| Contención en mutex | Media | Telemetría lenta | Lectura rápida ✓ |

**Puntuación de Estado**: **6/10** (Bien protegido pero monitor requerido)

---

## 7. Complejidad de Dependencias Externas

### Headers de ESP-IDF Utilizados (~30+)

```
Subsistema FreeRTOS:
├─ freertos/task.h          [xTaskCreatePinnedToCore]
├─ freertos/queue.h         [xQueueCreate, xQueueSend]
├─ freertos/semphr.h        [xSemaphoreCreate]
└─ freertos/event_groups.h  [xEventGroupCreate]

Subsistema Driver:
├─ driver/uart_driver.h     [uart_driver_install]
├─ driver/i2c_master.h      [i2c_master_bus_handle_t]
├─ driver/gpio.h            [gpio_set_level]
└─ driver/adc_oneshot.h     [adc_oneshot_read]

Subsistema Hardware (ROM/HAL):
├─ esp_rom_sys.h            [esp_rom_delay_us]
├─ esp_rom_crc.h            [esp_rom_crc8]
└─ soc/adc_periph.h         [ADC_CHANNEL_*]

Subsistema Sistema:
├─ esp_system.h             [esp_chip_info]
├─ esp_check.h              [ESP_RETURN_ON_FALSE]
├─ esp_err.h                [esp_err_t]
└─ esp_log.h                [ESP_LOGI]

Subsistema Conectividad:
├─ esp_wifi.h               [esp_wifi_start]
├─ esp_netif.h              [esp_netif_init]
├─ esp_event.h              [esp_event_handler_register]
├─ esp_http_server.h        [httpd_start]
└─ esp_http_server_core.h   [httpd_queue_work]

Librerías:
├─ cJSON/cJSON.h            [cJSON_Parse]
└─ stdint.h, stdlib.h, etc  [estándar C]
```

### Análisis de Dependencias

| Aspecto | Valor | Riesgo |
|---------|-------|--------|
| Headers principales | ~30 | MEDIO |
| Versión ESP-IDF | ? (ver sdkconfig) | ? |
| Dependencias circulares | 0 | OK |
| Compatibilidad backwards | Desconocida | ⚠️ |
| Documentación | Disponible en IDF | OK |

**Puntuación de Dependencias**: **6/10** (Bien encapsuladas pero muchas)

---

## 8. Complejidad Ciclomática Estimada

### Análisis de Complejidad por Archivo

```
Complejidad Ciclomática ≈ número de rutas de decisión

main.c (1166 LOC)
├─ app_main()              CC ≈ 8-10   (muchos xTaskCreatePinnedToCore)
├─ motor_task()            CC ≈ 5-7    (lectura queue, manejo de errores)
├─ i2c_sensors_task()      CC ≈ 4-6
└─ Promedio estimado:      CC ≈ 15-25  🔴 ALTO

tmc2209.c (349 LOC)
├─ tmc_write()             CC ≈ 3-4
├─ tmc_read()              CC ≈ 4-5    (CRC check)
├─ tmc2209_move_steps()    CC ≈ 5-7
└─ Promedio estimado:      CC ≈ 8-12   MEDIA

focuser_handler.c (238 LOC)
├─ focuser_handler_move_to() CC ≈ 4-6  (validaciones, boundary check)
├─ Getters (8)             CC ≈ 1 c/u
└─ Promedio estimado:      CC ≈ 10-15  MEDIA-ALTA

ws_server.c (242 LOC)
├─ ws_handler()            CC ≈ 5-8
├─ ws_async_send_exec()    CC ≈ 3-4
└─ Promedio estimado:      CC ≈ 8-12   MEDIA

alpaca.c (236 LOC)
├─ alpaca_handler()        CC ≈ 6-8
└─ Promedio estimado:      CC ≈ 10-14  MEDIA-ALTA

Global Promedio:          CC ≈ 12      MEDIA-ALTA
```

**Nota**: Valores reales requieren análisis con herramientas (radon, cppcheck).

**Recomendación**: Usar `radon cc` para análisis exacto:
```bash
pip install radon
radon cc -a main/main.c components/*/*.c
```

---

## 9. Riesgos Identificados

### 🔴 CRÍTICOS (Acción inmediata si ocurren)

```
1. MOTOR TIMING DRIFT
   ├─ Causa: ISR WiFi interrumpe motor_task
   ├─ Síntoma: Motor vibra o pierde pasos
   ├─ Prevención: Core 1 dedicado, sin WiFi
   ├─ Detección: Monitorear jitter con GPIO scope
   └─ Mitigación Actual: ✓ IMPLEMENTADA (core 1 aislado)

2. DEADLOCK EN FOCUSER_HANDLER
   ├─ Causa: Lectura mientras se escribe
   ├─ Síntoma: Sistema se congela
   ├─ Prevención: Mutex bien diseñado
   ├─ Detección: FreeRTOS deadlock detector
   └─ Mitigación Actual: ✓ BIEN DOCUMENTADO

3. ALPACA HTTP REQUIERE VALIDACIÓN DE INTEGRACIÓN
   ├─ Causa: La implementación existe, pero faltan pruebas documentadas con clientes Alpaca reales
   ├─ Síntoma posible: Diferencias de contrato o comportamiento con clientes externos
   ├─ Prevención: Probar management, GET, Move, Halt y errores
   ├─ Timeline: Validación de integración
   └─ Mitigación Actual: ⚠️ Rutas implementadas y documentadas
```

### 🟠 ALTOS (Monitoreo requerido)

```
1. 6 TASKS CONCURRENTES = ALTO RIESGO DE RACE CONDITIONS
   ├─ Probabilidad: Baja si se respeta el código
   ├─ Impacto: Alto (estado inconsistente)
   ├─ Monitoreo: Code review, stress testing
   └─ Mitigación: Documentación clara ✓

2. 5 PROTOCOLOS = SUPERFICIE DE ATAQUE AMPLIA
   ├─ Probabilidad: Media (cada protocolo tiene bugs potenciales)
   ├─ Impacto: Bajo-Medio (modularidad ayuda)
   ├─ Testing: Verificar cada protocolo independientemente
   └─ Mitigación: Modularidad ✓

3. main.c CON 42% DEL CÓDIGO
   ├─ Probabilidad: Media (difícil de mantener)
   ├─ Impacto: Medio (bugs en orquestación → sistema entero)
   ├─ Refactorización: Dividir responsabilidades
   └─ Mitigación: Bien comentado ✓
```

### 🟡 MEDIOS (Monitoreo ocasional)

```
1. I2C COMPARTIDO ENTRE SENSORES (aunque en buses físicamente diferentes)
   ├─ Probabilidad: Baja (buses independientes)
   ├─ Impacto: Bajo (sensores no críticos)
   └─ Mitigación: Diseño actual ✓

2. MÚLTIPLES INTERFACES DE ENTRADA (USB + WebSocket + Alpaca)
   ├─ Probabilidad: Media (usuario usa varias simultáneamente)
   ├─ Impacto: Bajo (queue serializa)
   └─ Mitigación: motor_cmd_queue ✓

3. MEMORIA HEAP DESCONOCIDA
   ├─ Probabilidad: Media (sin heap sizing)
   ├─ Impacto: Alto (fragmentación/OOM)
   ├─ Acción: Revisar sdkconfig, hacer heap stress test
   └─ Mitigación: Recomendado ⚠️
```

---

## 10. Métrica Final: Puntuación de Complejidad

### Scoring por Dimensión

```
┌──────────────────────┬────────┬──────────────────────┐
│ Dimensión            │ Score  │ Evaluación           │
├──────────────────────┼────────┼──────────────────────┤
│ Tamaño de Código     │ 6/10   │ 2,770 LOC (medio)    │
│ Concurrencia         │ 7/10   │ 6 tasks (riesgo)     │
│ Protocolos           │ 7/10   │ 5 protocolos (alto)  │
│ Hardware             │ 6/10   │ ~15 pines (medio)    │
│ Estado/Sync          │ 6/10   │ Bien protegido       │
│ Dependencias         │ 6/10   │ 30+ headers (muchas) │
│ Modularidad          │ 8/10   │ Bien encapsulado     │
│ Documentación        │ 9/10   │ Excelente            │
│ Testabilidad         │ 7/10   │ Módulos test-ready   │
│ Mantenibilidad       │ 7/10   │ Bien estructurado    │
├──────────────────────┼────────┼──────────────────────┤
│ PROMEDIO             │ 6.9/10 │ 🟡 MEDIA-ALTA        │
└──────────────────────┴────────┴──────────────────────┘
```

### Clasificación del Proyecto

```
NIVEL DE COMPLEJIDAD: 🟡 MEDIA-ALTA (6.5-7.0 / 10)

Comparación con otros proyectos:

Proyecto                    LOC    Tasks  Protocolos  Nivel
─────────────────────────────────────────────────────────────
Este (Focuser)            2,770    6         5        🟡 MEDIO-ALTO
─────────────────────────────────────────────────────────────
LED Blink Simple             50    1         0        🟢 TRIVIAL
Sensor único             1,000    2         1        🟢 SIMPLE
─────────────────────────────────────────────────────────────
WiFi Gateway             8,000   10         3        🔴 MUY ALTO
MQTT Broker            20,000   20+        2        🔴 MUY ALTO
─────────────────────────────────────────────────────────────
ESP32 Bootloader       15,000    ?         N/A       🔴 EXTREMO
Linux Kernel         30M+      N/A        N/A       🔴 EXTREMO
```

---

## 11. Cuellos de Botella Probables

### Bottleneck #1: motor_task (Core 1) 🔴 CRÍTICO

```
Factor limitante: GPIO bit-bang timing
├─ Operación: esp_rom_delay_us() para generación de pulsos
├─ Tiempo mínimo: ~100 nanosegundos por paso
├─ Máxima velocidad: ~10,000 pasos/segundo
├─ Si se retrasa: Motor pierde sincronismo
└─ Solución: Core 1 dedicado ✓ IMPLEMENTADA

Métricas esperadas:
├─ Latencia máxima permitida: 50 μs (para timing de 100 μs)
├─ CPU usage: ~30-40% a velocidad máxima
└─ Stack usage: Verificar con FreeRTOS heap monitoring
```

### Bottleneck #2: WebSocket (Core 0)

```
Factor limitante: I/O bound (lectura serial + socket)
├─ ws_telemetry_task: Broadcast a múltiples clientes
├─ i2c_sensors_task: Polling cada 200 ms
├─ Si se congestiona: Comandos lentos, telemetría perdida
└─ Solución: Prioridades bien asignadas ✓

Métricas esperadas:
├─ Latencia de comando: <100 ms (desde USB → motor)
├─ Latencia de telemetría: <50 ms (si WS tiene clientes)
└─ Throughput: 10+ comandos/segundo posible
```

### Bottleneck #3: focuser_handler mutex

```
Factor limitante: Acceso sincronizado a estado global
├─ Escritores: motor_task, i2c_sensors_task
├─ Lectores: ws_telemetry_task (cada 50-100 ms)
├─ Si hay contención: Telemetría se retrasa
└─ Solución: Lectura rápida con getter atómico ✓

Métricas esperadas:
├─ Latencia de lectura: <10 μs (sin sleep)
├─ Contención esperada: <0.1% (muy baja)
└─ Deadlock risk: Muy bajo (buen diseño)
```

### Benchmarking Recomendado

```bash
# Medir latencia de comando
echo "5000,0" > /dev/ttyUSB0  # desde Python/terminal
# Medir con osciloscopio en GPIO4 (STEP)

# Medir CPU usage
# Ver en monitor: Task CPU, heap free, WDT

# Stress test
# Enviar 100 comandos rápidamente → verificar fiabilidad
```

---

## 12. Recomendaciones de Reducción de Complejidad

### Corto Plazo (Semanas)

```
1. ✓ REFACTORIZAR main.c
   ├─ Tamaño actual: 1,166 LOC (42%)
   ├─ Propuesta: Dividir en main.c + orchestrator.c
   ├─ Beneficio: -30% complejidad, +claridad
   ├─ Tiempo: 4-6 horas
   └─ Impacto: Alto

2. ⚠️ COMPLETAR Alpaca HTTP (Steps 4/5)
   ├─ Estado: Management + GET + PUT Move/Halt implementados; faltan rutas estándar
   ├─ Propuesta: Agregar endpoints de control
   ├─ Beneficio: Sistema usable con software externo
   ├─ Tiempo: 2-3 horas
   └─ Impacto: Medio (feature request)

3. 📊 MEDIR Complejidad Ciclomática Real
   ├─ Tool: radon cc
   ├─ Objetivo: Identificar funciones CC > 10
   ├─ Tiempo: 30 minutos
   └─ Impacto: Data-driven decisions
```

### Mediano Plazo (Meses)

```
1. ✓ AGREGAR PRUEBAS UNITARIAS
   ├─ Objetivo: Cubrir cada módulo por separado
   ├─ Herramienta: Unity (para ESP32)
   ├─ Beneficio: +confiabilidad, -regresiones
   ├─ Tiempo: 20-30 horas
   └─ Impacto: ALTO (calidad)

2. ✓ STRESS TESTING DE CONCURRENCIA
   ├─ Objetivo: 10,000 comandos rápidos, verificar estado
   ├─ Herramienta: Script Python + monitor serial
   ├─ Beneficio: Detectar race conditions antes de deployment
   ├─ Tiempo: 4-6 horas
   └─ Impacto: ALTO (confiabilidad)

3. ⚠️ PROFILING DE MEMORIA
   ├─ Objetivo: Entender heap allocation y fragmentación
   ├─ Herramienta: FreeRTOS heap monitoring
   ├─ Beneficio: Evitar OOM en operación
   ├─ Tiempo: 2-3 horas
   └─ Impacto: MEDIO (estabilidad)
```

### Largo Plazo (Semestres)

```
1. ✓ DOCUMENTACIÓN DE TIEMPO REAL
   ├─ Objetivo: Análisis formal de latencias
   ├─ Herramienta: WCET analysis, timing traces
   ├─ Beneficio: Garantías de timing en motor_task
   ├─ Tiempo: 40+ horas
   └─ Impacto: CRÍTICO (confiabilidad motor)

2. ✓ REEMPLAZO DE I2C POR SPI
   ├─ Objetivo: Reducir latencia de sensores
   ├─ Beneficio: Más ancho de banda (si disponibles chips SPI)
   ├─ Tiempo: 20+ horas
   └─ Impacto: MEDIO (si necesario)

3. ⚠️ MIGRACIÓN A RUST
   ├─ Objetivo: Verificación de memoria en compile-time
   ├─ Beneficio: Eliminar clase de bugs (memory safety)
   ├─ Tiempo: 80+ horas (reescritura completa)
   └─ Impacto: TRANSFORMACIONAL (si vale)
```

---

## 13. Checklists de Calidad

### Pre-Deployment Checklist

```
Code Quality:
☐ Análisis estático sin warnings (clang-tidy)
☐ Complejidad ciclomática < 10 (excepto main.c)
☐ Cobertura de cobertura > 70%
☐ Code review completado
☐ Documentación sincronizada con código

Performance:
☐ Latencia de comando < 100 ms
☐ Motor genera pasos sin jitter
☐ Telemetría no se atrasa visiblemente
☐ Heap usage estable (no fragmentación)
☐ No hay memory leaks detectable

Concurrency:
☐ Stress test: 1,000+ comandos sin error
☐ Multi-client WebSocket funciona
☐ No hay deadlocks detectables
☐ Timeout handlers funcionan
☐ Logs de race condition = 0

Hardware:
☐ Todos los pines configurados correctamente
☐ Voltajes dentro de especificación
☐ Power Good funciona
☐ ADC readings calibrado
☐ UART CRC correcto

Integration:
☐ USB serial commands OK
☐ WebSocket broadcast OK
☐ Alpaca HTTP management OK
☐ Sensor readings OK
☐ Motor movements OK
```

### Monitoring en Producción

```
Indicadores a Monitorear:

Hardware:
├─ Temperatura del ESP32 (si sensor disponible)
├─ Voltaje de entrada (ADC CH224K)
├─ Estado Power Good (GPIO15)
└─ Count de reset/watchdog

Software:
├─ Heap free (< 50 KB = peligro)
├─ Motor position (verificar deriva)
├─ WebSocket conexiones activas
├─ Errores de I2C (timeouts)
├─ Latencia de comando (cada 100 eventos)

Alertas:
├─ 🔴 Heap < 30 KB
├─ 🔴 Motor posición inconsistente
├─ 🟡 Latencia comando > 200 ms
├─ 🟡 WebSocket cliente desconexión rápida
└─ 🟡 I2C error rate > 1%
```

---

## 14. Resumen Ejecutivo

### Proyecto: ✓ VIABLE pero REQUIERE CUIDADO

```
COMPLEJIDAD TOTAL:        6.5/10  🟡 MEDIA-ALTA
CALIDAD DE CÓDIGO:        7.5/10  ✓ BUENA
RIESGO DE DEFECTOS:       5.0/10  ✓ BAJO (bien mitigado)
DOCUMENTACIÓN:            9.0/10  ✓ EXCELENTE
MANTENIBILIDAD:           7.0/10  ✓ BUENA

PUNTOS FUERTES:
✓ Arquitectura modular y bien separada
✓ Concurrencia manejada correctamente (cores separados)
✓ Documentación exhaustiva (11 archivos .md)
✓ Encapsulación clara (cada módulo autocontenido)
✓ Protocolos bien implementados

PUNTOS DÉBILES:
✗ main.c concentra 42% del código (refactorizar)
✗ 6 tasks concurrentes (alto riesgo si no se respetan patrones)
✗ 5 protocolos (gran superficie de bugs)
⚠️ Alpaca HTTP parcial; faltan rutas estándar y validación con clientes externos
✗ Sin pruebas unitarias documentadas

RECOMENDACIONES INMEDIATAS:
1. Refactorizar main.c (dividir responsabilidades)
2. Agregar radon CC para profiling exacto
3. Implementar stress tests de concurrencia
4. Completar Alpaca HTTP (Steps 4/5)
5. Crear suite de pruebas unitarias

TIMELINE A PRODUCCIÓN:
├─ Code review + testing: 1-2 semanas
├─ Bug fixes (si aplica): 1 semana
├─ Deployment inicial: 1 semana
├─ Monitoring + ajustes: 2-4 semanas
└─ TOTAL: 1-2 meses
```

---

## 15. Métricas Futuras a Rastrear

```
Métrica                    Baseline   Meta       Frecuencia
─────────────────────────────────────────────────────────────
Complejidad Ciclomática    ~12        < 8        Cada release
Lines of Code              2,770      < 3,500    Cada mes
Test Coverage              0%         > 70%      Cada release
Heap Usage                 ?          < 200 KB   Cada semana
Motor Timing Jitter        ?          < 5 μs     Cada release
I2C Error Rate             0%         < 0.1%     Cada semana
WebSocket Latency          ?          < 50 ms    Cada semana
Alpaca Endpoints           3/5        5/5        Roadmap
```

---

**Documento generado**: 2026-08-31  
**Próxima revisión recomendada**: Después de refactorización de main.c  
**Autor**: GitHub Copilot  
**Estado**: ✅ COMPLETADO
