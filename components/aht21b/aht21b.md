# aht21b.c - Driver del Sensor AHT21B

## Propósito
Implementa el driver I2C para el sensor de temperatura y humedad relativa AHT21B. Maneja inicialización, calibración, disparo de mediciones y lectura de datos sin bloquearse indefinidamente, diseñado para operar en bus I2C compartido.

## Especificaciones del Sensor

### Dirección y Protocolo
- **Dirección I2C**: 0x38 (7 bits)
- **Bus I2C compartido**: Con AS5600, ambos en I2C_NUM_0 del ESP32-S3
- **Protocolo**: Estándar I2C con lectura de bytes (sin selección de registro explícita)

### Comandos Internos
| Comando | Bytes | Propósito |
|---------|-------|----------|
| `0xAC` + 0x33 + 0x00 | 3 | Dispara medición |
| `0xBE` + 0x08 + 0x00 | 3 | Inicializa sensor (solo si no calibrado) |

### Registros
- **STATUS (bit 7)**: Busy (1 = ocupado, conversión en curso)
- **STATUS (bit 3)**: Calibrated (1 = sensor calibrado)
- **Tiempo de conversión**: 80 ms mínimo (driver usa 100 ms + margen)

### Lectura de STATUS
**Peculiaridad importante**: No se escribe dirección de registro como en I2C estándar. El AHT21B **siempre** devuelve STATUS como primer byte de cualquier lectura, sin comando previo de address.

## Estructuras de Datos

### aht21b_t (Handle del Dispositivo)
```c
struct {
    i2c_master_device_handle_t dev;  // Handle del dispositivo en el bus I2C
    aht21b_config_t cfg;              // Configuración (copia)
};
```

### aht21b_config_t (Configuración)
```c
struct {
    i2c_master_bus_handle_t bus;      // Bus I2C YA EXISTENTE (creado por orquestador)
    uint32_t i2c_freq_hz;             // Frecuencia I2C (ej. 400000 Hz)
};
```

## API Pública

### 1. Inicialización: aht21b_init()
```c
esp_err_t aht21b_init(aht21b_t *dev, const aht21b_config_t *cfg)
```

**Flujo**:
1. Valida parámetros (dev y cfg no NULL, bus no NULL).
2. Limpia estructura `dev` (memset a 0).
3. Copia configuración a `dev->cfg`.
4. Registra dispositivo en bus I2C YA EXISTENTE usando `i2c_master_bus_add_device()`.
5. Retorna ESP_OK si éxito, código de error si falla.

**Importante**: 
- **NO crea el bus I2C** (debe ser creado por el orquestador en main.c).
- Solo se registra como dispositivo en el bus compartido.
- El bus es el mismo que usa AS5600 (reducción de pines).

### 2. Asegura Calibración: aht21b_ensure_calibrated()
```c
esp_err_t aht21b_ensure_calibrated(aht21b_t *dev)
```

**Flujo**:
1. Lee STATUS del sensor.
2. Verifica bit 3 (CALIBRATED).
3. Si **no** está calibrado:
   - Loguea WARN con valor de STATUS actual.
   - Envía comando de inicialización (0xBE 0x08 0x00).
   - Espera 10 ms.
   - Relee STATUS.
4. Retorna ESP_OK si calibrado, ESP_ERR_INVALID_STATE si falla.

**Patrón**: Llamado típicamente una única vez en arranque del sistema.

### 3. Lectura de STATUS (Privada): aht21_read_status()
```c
static esp_err_t aht21_read_status(aht21b_t *dev, uint8_t *out_status)
```
- Lee un byte del sensor (sin escribir dirección, devuelve STATUS automáticamente).
- Timeout: 100 ms.
- Retorna ESP_OK con status en `out_status`, o código de error.

### 4. Disparo de Medición (Privada): aht21_trigger_measurement()
```c
static esp_err_t aht21_trigger_measurement(aht21b_t *dev)
```
- Envía comando de medición (0xAC 0x33 0x00).
- Timeout: 100 ms.
- **No espera la conversión**: Retorna de inmediato tras enviar comando.
- La lectura de datos (aht21b_read()) espera después.

## Diseño de Integración

### Responsabilidades del Driver
- **SÍ hace**: Protocolo I2C, secuencias de comandos, calibración.
- **NO hace**: 
  - Crear el bus I2C (responsabilidad de orquestador).
  - Parsear temperatura/humedad (métodos privados no expuestos).
  - Manejo de task/timing (responsabilidad de consumidor).

### Responsabilidades del Orquestador (main.c)
- Crear el bus I2C único (I2C_NUM_0).
- Registrar tanto AHT21B como AS5600 en ese bus.
- Llamar `aht21b_init()` para registrarse.
- Dentro de `i2c_sensors_task`: ciclo de lectura periódica.
- Llamar `aht21b_ensure_calibrated()` en arranque.

### Sin Bloqueo Indefinido
- Todos los timeouts I2C son explícitos: `pdMS_TO_TICKS(100)`.
- Si el sensor no responde, I2C timeout ocurre y se retorna error (no se cuelga task).
- Consumidor (main.c) reintenta N veces con delay fijo entre intentos.

## Manejo de Errores

| Situación | Comportamiento |
|-----------|-----------------|
| `dev == NULL` o `cfg == NULL` | Retorna `ESP_ERR_INVALID_ARG` |
| Bus I2C no inicializado | Retorna `ESP_ERR_INVALID_ARG` (validación en init) |
| Fallo al agregar dispositivo al bus | Retorna error de `i2c_master_bus_add_device()` |
| Timeout en lectura/escritura I2C | Retorna `ESP_ERR_TIMEOUT` |
| Sensor no responde | Loguea ERROR, retorna error de I2C |
| No se calibra tras init | Retorna `ESP_ERR_INVALID_STATE` |

## Integración con Sistema

### Bus I2C Compartido
```
I2C_NUM_0 (SDA=GPIO8, SCL=GPIO9)
  ├─ AHT21B (0x38) ← este driver
  └─ AS5600 (0x36)  ← driver as5600.c
```

### Task que lo Usa
- **i2c_sensors_task** (main.c, Core 0):
  - Lee sensores en loop periódico (ej. cada 100 ms).
  - Llama a métodos públicos de aht21b.
  - No comparte core con motor_task, así que I2C bloqueante no afecta precisión del motor.

### Reporting de Datos
- Temperatura se reporta a `focuser_handler` vía `focuser_handler_set_temperature()`.
- Usado para compensación de temperatura en focuser si está habilitada.

## Macros y Constantes

```c
#define AHT21_I2C_ADDR              0x38
#define AHT21_CMD_TRIGGER           0xAC   // + 0x33, 0x00
#define AHT21_CMD_INIT              0xBE   // + 0x08, 0x00
#define AHT21_STATUS_BUSY_BIT       0x80   // bit 7
#define AHT21_STATUS_CALIBRATED_BIT 0x08   // bit 3
#define AHT21_CONVERSION_DELAY_MS   100    // 80 ms min + margen
```

## Logging
- **TAG**: "AHT21B"
- **Niveles**:
  - ERROR: Fallos en I2C, argumentos inválidos.
  - WARN: Sensor no calibrado al detectarlo.
  - INFO: (Opcional) Completado satisfactoriamente.

## Notas de Diseño

### 1. Modularidad
- Autocontenido: solo depende de ESP-IDF/FreeRTOS.
- No sabe nada de focuser_handler, motor_cmd_queue, websocket.
- Se puede copiar a otro proyecto ESP-IDF intacto.

### 2. Sin Polling Infinito
- Si bus I2C se cuelga, timeout de 100 ms hace que task escape.
- main.c cuenta los fallos consecutivos y marca sensor como no disponible.

### 3. Calibración Lazy
- Calibración se verifica/ejecuta en `aht21b_ensure_calibrated()` bajo demanda.
- No ocurre automáticamente en init.
- Permite que main.c controle cuándo ejecutar init pesado (en arranque, no durante operación).

## Casos de Uso

### Temperatura Ambiente
```c
// En i2c_sensors_task:
float temp = leer_temperatura_aht21b();  // (privado en este driver, expuesto por métodos públicos)
focuser_handler_set_temperature(temp);   // Reporta al focuser
```

### Compensación de Temperatura
- Cambios de temperatura afectan óptica del focuser.
- Temperatura se trackea en `focuser_handler` y puede aplicarse corrección automática si habilitada.

## Archivos Relacionados
- `aht21b.h` - Interfaz pública e `aht21b_config_t`
- `main.c` - Orquestador que crea bus I2C y llama init
- `i2c_sensors_task()` en main.c - Loop de lectura periódica
- `focuser_handler.c` - Receptor de lecturas de temperatura
- Datasheet AHT21B - Especificaciones de protocolo y timeouts
