# Flujo General de Inicialización y Distribución

## Alcance

Este documento resume el arranque de `main.c` y la distribución del firmware en los dos cores del ESP32-S3. Los detalles internos de cada task están documentados por separado.

## Flujo de inicialización

`app_main()` prepara primero los recursos compartidos y después crea las tareas. La inicialización del hardware específico ocurre dentro de cada task, no directamente en `app_main()`.

```mermaid
flowchart TD
    BOOT[Boot del ESP32-S3] --> APP[app_main]
    APP --> LED[Configurar LEDs de estado]
    LED --> QUEUE[Crear motor_cmd_queue]
    QUEUE -->|falla| STOP1[Registrar error y terminar inicializacion]
    QUEUE --> FH[Inicializar focuser_handler]
    FH --> SEM[Crear power_good_sem]
    SEM -->|falla| STOP2[Registrar error y terminar inicializacion]
    SEM --> WIFI[Intentar conexion WiFi]
    WIFI -->|falla| LOCAL[Continuar en modo local]
    WIFI -->|exito| WS[Iniciar servidor WebSocket]
    WS -->|exito| TELEMETRY[Crear ws_telemetry_task]
    WS -->|falla| NO_WS[Continuar sin WebSocket]
    TELEMETRY --> ALPACA[Iniciar servidor Alpaca]
    NO_WS --> ALPACA
    ALPACA --> TASKS[Crear tasks principales]
    LOCAL --> TASKS
    TASKS --> MOTOR[motor_task]
    TASKS --> SENSORS[i2c_sensors_task]
    TASKS --> POWER[power_monitor_task]
    TASKS --> PRESET[preset_cmd_task]
    POWER -->|PG confirmado| SEMAPHORE[Dar power_good_sem]
    SEMAPHORE --> MOTOR_INIT[Inicializar TMC2209]
    MOTOR --> MOTOR_INIT
```

## Secuencia de `app_main()`

1. Configura GPIO10 y GPIO12 como salidas y apaga ambos LEDs.
2. Crea `motor_cmd_queue`, que será el canal común para todos los comandos de movimiento.
3. Inicializa `focuser_handler` con la cola y los límites lógicos del focuser.
4. Crea `power_good_sem` vacío.
5. Ejecuta `wifi_init_sta()`.
   - La llamada bloquea hasta conectar o agotar los reintentos.
   - Si falla, el motor y los sensores continúan funcionando sin control remoto.
6. Si WiFi conecta:
   - Inicia el servidor WebSocket.
   - Si el WebSocket inicia correctamente, crea `ws_telemetry_task`.
   - Inicia el servidor Alpaca, que crea internamente sus tareas HTTP y discovery UDP.
7. Crea `motor_task` en el Core 1.
8. Crea `i2c_sensors_task` en el Core 0.
9. Crea `power_monitor_task` en el Core 0.
10. Crea `preset_cmd_task` en el Core 0.
11. `app_main()` termina; FreeRTOS continúa ejecutando las tasks de forma concurrente.

## Distribución por core

```mermaid
flowchart LR
    subgraph C0[Core 0 - PRO_CPU]
        WIFI[WiFi y stack de red]
        I2C[i2c_sensors_task<br/>prioridad 6]
        POWER[power_monitor_task<br/>prioridad 1]
        PRESET[preset_cmd_task<br/>prioridad 4]
        TELEMETRY[ws_telemetry_task<br/>prioridad 3]
        ALPACA[alpaca_http_task<br/>alpaca_discovery_task]
    end

    subgraph C1[Core 1 - APP_CPU]
        MOTOR[motor_task<br/>prioridad 10]
        TMC[TMC2209: STEP/DIR]
    end

    POWER -->|power_good_sem| MOTOR
    PRESET -->|motor_cmd_queue| MOTOR
    NETWORK[WebSocket / Alpaca] -->|motor_cmd_queue o focuser_handler| MOTOR
    MOTOR --> TMC
    I2C --> STATE[Estado del focuser]
    MOTOR --> STATE
    STATE --> TELEMETRY
    STATE --> ALPACA
```

## Motivo de la distribución

- **Core 1:** queda dedicado a `motor_task`, que genera los pulsos STEP/DIR y requiere temporizacion estable.
- **Core 0:** concentra WiFi, servidores de red, sensores, monitor de alimentacion y comandos de prueba.
- El stack WiFi/BT y sus interrupciones permanecen en el Core 0; separar el motor reduce el jitter durante la generacion de micropasos.
- Las lecturas I2C son bloqueantes y se ejecutan en el Core 0, sin interferir directamente con el control del motor.

## Sincronización durante el arranque

```mermaid
sequenceDiagram
    participant A as app_main
    participant M as motor_task
    participant P as power_monitor_task
    participant Q as motor_cmd_queue
    participant T as TMC2209

    A->>Q: Crear cola
    A->>A: Crear power_good_sem
    A->>M: Crear task en Core 1
    M->>M: Esperar power_good_sem
    A->>P: Crear task en Core 0
    P->>P: Inicializar CH224K y leer PG
    P->>M: Dar power_good_sem una vez cuando PG=OK
    M->>T: Inicializar y verificar TMC2209
    M->>Q: Esperar comandos
    Q-->>M: Entregar motor_cmd_t
    M->>T: Ejecutar movimiento
```

## Contratos globales

| Recurso | Creador | Consumidor/productor | Funcion |
|---|---|---|---|
| `motor_cmd_queue` | `app_main` | Productores de comandos -> `motor_task` | Desacoplar la solicitud de movimiento de su ejecucion |
| `power_good_sem` | `app_main` | `power_monitor_task` -> `motor_task` | Impedir que el TMC2209 arranque sin alimentacion confirmada |
| Estado de `focuser_handler` | `app_main` | `motor_task` e `i2c_sensors_task` escriben; red consulta | Compartir posicion, movimiento y temperatura |

## Ramas de fallo del arranque

- **No se crea la cola o el semaforo:** `app_main()` registra el error y no crea las tasks.
- **WiFi falla:** se omiten WebSocket y Alpaca; el funcionamiento local continúa.
- **WebSocket falla:** no se crea la task de telemetria, pero Alpaca se intenta iniciar y el resto continúa.
- **Alpaca falla:** el motor, los sensores y el WebSocket no dependen de ese servidor.
- **CH224K falla:** `power_monitor_task` termina sin liberar el semaforo; `motor_task` permanece esperando y no energiza el TMC2209.
- **Sensor I2C falla:** su task continúa con ese sensor no disponible; no detiene el motor.

## Resumen para un diagrama unico

```text
Boot
  -> app_main configura LEDs
  -> crear cola de comandos
  -> inicializar estado del focuser
  -> crear semaforo PG
  -> conectar WiFi
       -> fallo: modo local
       -> exito: WebSocket + telemetria + Alpaca
  -> crear motor_task en Core 1
  -> crear tasks de sensores, potencia y preset en Core 0
  -> power_monitor_task confirma PG
  -> motor_task inicializa TMC2209
  -> productores colocan comandos en motor_cmd_queue
  -> motor_task ejecuta movimientos
  -> sensores y motor actualizan estado
  -> red consulta o transmite ese estado
```

## Referencias

- Orquestacion: `main/main.c`.
- Flujo de motor: `main/motor_task.md`.
- Flujo de sensores: `main/i2c_sensors_task.md`.
- Flujo de alimentacion: `main/power_monitor_task.md`.
- Flujo de telemetria: `main/ws_telemetry_task.md`.
