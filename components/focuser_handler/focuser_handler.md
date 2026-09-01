# focuser_handler.c - Manejador de Estado del Focuser

## Descripción General
Componente central de orquestación de estado del focuser.

Responsabilidades:
- Mantener estado lógico único y consistente del focuser
- Filtrar acceso a estado desde transportes (WebSocket, futura API Alpaca)
- Traducir posición absoluta pedida → comando relativo en motor_cmd_queue
- Reportar estado desde tasks de hardware (motor, sensores)
- Manejar halt (parada de emergencia) atomically

## Arquitectura de Estado

### Estructura focuser_state_t (Privada)

**Campos protegidos por mutex `s_state_mutex`:**
```c
QueueHandle_t motor_cmd_queue;      // Cola de comandos al motor
int64_t position_usteps;             // Posición actual (micropasos)
bool is_moving;                      // ¿Está moviéndose?
bool tempcomp_enabled;               // ¿Compensación de temp activa?
float temperature_c;                 // Temperatura actual
```

**Configuración inmutable (lectura sin lock):**
```c
int32_t max_step;                    // Posición máxima permitida
double step_size_um;                 // Tamaño de cada micropasos en micrómetros
bool tempcomp_available;             // ¿Disponible compensación de temp?
bool initialized;                    // ¿Inicializado?
```

### Sincronización

- **s_state_mutex**: Mutex binario FreeRTOS
  - Protege todos los campos mutables
  - Lock/unlock en funciones getter/setter

- **s_halt_requested**: Variable atómica SEPARADA
  - No usa mutex para evitar deadlock en signal handler
  - Lectura rápida sin contención de lock
  - Propósito: Emergencia que puede interruptor motor_task

## Funciones Principales

### `focuser_handler_init(const focuser_handler_config_t *cfg)`
Inicializa handler de focuser.

**Entrada:**
```c
cfg->motor_cmd_queue;      // Cola de comandos (obligatoria)
cfg->max_step;             // Posición máxima en micropasos
cfg->step_size_um;         // Tamaño de micropasos en micrómetros
cfg->tempcomp_available;   // ¿Compensación de temperatura disponible?
```

**Procedimiento:**
1. Crea mutex de estado
2. Inicializa estructura de estado
3. Posición inicial = 0 (origen lógico)
4. Nota: Homing físico debe hacerse antes de marcar como listo

**Retorna:**
- `ESP_OK`: Éxito
- `ESP_ERR_INVALID_ARG`: Config NULL o motor_cmd_queue NULL
- `ESP_ERR_NO_MEM`: No se puede crear mutex

### `focuser_handler_report_move_done(int32_t executed_usteps_signed)`
Actualiza posición tras movimiento.

**Entrada:**
- `executed_usteps_signed`: Micropasos ejecutados (con signo)
  - Positivo: Aumento de posición (abre)
  - Negativo: Disminución (cierra)

**Llamada desde:**
- motor_task tras `tmc2209_move_steps()` completa

**Nota:** Suma signada permite compensar si motor ejecuta parcialmente

### `focuser_handler_set_moving(bool moving)`
Actualiza bandera is_moving.

**Entrada:**
- `moving`: true si movimiento en progreso, false si detenido

**Llamada desde:**
- motor_task antes de movimiento (true)
- motor_task después de movimiento (false)

### `focuser_handler_set_temperature(float temp_c)`
Reporta lectura de temperatura.

**Entrada:**
- `temp_c`: Temperatura en Celsius

**Llamada desde:**
- i2c_sensors_task tras leer AHT21B

### Funciones Getter (Lectura de Estado)

#### `focuser_handler_get_position(void) → int64_t`
Retorna posición actual en micropasos.

#### `focuser_handler_get_is_moving(void) → bool`
Retorna si está en movimiento.

#### `focuser_handler_get_temperature(void) → float`
Retorna temperatura última leída.

#### `focuser_handler_get_tempcomp(void) → bool`
Retorna si compensación de temperatura está activa.

## Patrón de Acceso Seguro

### Getter Típico
```c
int64_t focuser_handler_get_position(void) {
    if (!s_state.initialized) return 0;
    lock();
    int64_t v = s_state.position_usteps;
    unlock();
    return v;
}
```

**Garantías:**
- Lectura consistente (no tearing en cambios concurrentes)
- Estado no-inicializado retorna 0 (safe default)
- Bajo overhead (lock rápido)

## Halt Mechanism

### Diseño Separado

**¿Por qué atomic_bool separado?**
- Permite parada de emergencia sin deadlock
- Signal handler podría necesitar halt sin esperar mutex
- Lectura atómica más rápida que mutex en path crítico

### `focuser_handler_request_halt(void)`
Solicita parada de emergencia.

**Llamada desde:**
- Handler USB (Ctrl+C)
- Timeout de seguridad
- Comando de parada explícito

**Acción:**
- motor_task chequea `s_halt_requested` frecuentemente
- Detiene generación de pasos cuando true

## Integración con Transportes

### Websocket
```
websocket_rx -> motor_cmd_queue (comando relativo)
              → focuser_handler_get_* (lectura de estado)
```

### Alpaca HTTP (Futuro)
```
HTTP GET /api/v1/focuser/position -> focuser_handler_get_position()
HTTP PUT /api/v1/focuser/move -> motor_cmd_queue
```

### Principios
1. **Cero conocimiento de transporte:** focuser_handler es agnóstico
2. **Cola de comandos es único contrato:** Todos los transportes → motor_cmd_queue
3. **Estado consistente:** Mutex garantiza lectura/escritura atómica

## Validaciones

### Boundary Checking (Futuro)
- `position_usteps` debe estar en [0, max_step]
- Movimiento fuera de rango: Trunca o rechaza

### Seguridad Térmica (Futuro)
- Temperature > threshold → Desactiva movimiento
- Espera enfriamiento → Permite reactivación

## Logging

- TAG: "focuser_handler"
- INFO: Inicialización con parámetros
- ERROR: Argumentos inválidos

## Dependencias

- FreeRTOS: Semáforos, queues, atomics
- `stdatomic.h`: `atomic_bool`
- `esp_log.h`: Logging

## Flujo de Datos Típico

```
Transporte (WebSocket)
    |
    ├─→ focuser_handler_get_position() ─→ JSON response
    |
    └─→ motor_cmd_queue ←─ comando relativo
        |
        motor_task ejecuta
        |
    focuser_handler_report_move_done(+2000)
        |
    position_usteps += 2000
        |
    Transporte consulta nuevo state ✓
```

## Notas de Diseño

1. **No GPIO directo:** focuser_handler nunca toca hardware
2. **No lógica de movimiento:** Solo estado, no cálculos de paso
3. **Immutable config:** Parámetros de sistema no cambian
4. **Atomic halt:** Separado de mutex para emergencias
5. **Reportes no lecturas:** Hardware tasks reportan, no leen estado

## Futuros Enhancements

- [ ] Boundary checking de posición
- [ ] Validación de température
- [ ] Rate limiting de comandos
- [ ] Historial de movimientos
- [ ] Estadísticas de uso
