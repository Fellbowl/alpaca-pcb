/*
 * ============================================================================
 *  main.c - Orquestacion: TMC2209 + AS5600 + AHT21B - ESP32-S3 - ESP-IDF/FreeRTOS
 * ============================================================================
 *
 * Este archivo YA NO conoce el protocolo interno de ningun periferico: solo
 * arma la configuracion de pines/parametros de cada uno (tmc2209_config_t,
 * as5600_config_t, aht21b_config_t) y coordina las tres tasks de FreeRTOS.
 * Todo el detalle de UART/I2C, registros y CRC vive en tmc2209.c, as5600.c
 * y aht21b.c -- que a su vez NO se conocen entre si (ver la nota de
 * modularidad mas abajo).
 *
 * ----------------------------- ARQUITECTURA --------------------------------
 *
 *   Core 0 (PRO_CPU)                         Core 1 (APP_CPU)
 *   -----------------                        -----------------
 *   preset_cmd_task ---\                    motor_task
 *   (comandos internos) \                   (dueña exclusiva del UART1
 *                         > motor_cmd_queue ->  hacia el TMC2209, via
 *   mqtt_task (futuro)   /   (FreeRTOS)          el modulo tmc2209.c)
 *   i2c_sensors_task    /
 *   (AS5600 + AHT21B) -/
 *
 * Por que asi:
 *
 *   - El stack de WiFi/BT de Espressif esta anclado internamente al core 0.
 *     tmc2209_move_steps() es un bit-bang de GPIO con temporizacion por
 *     esp_rom_delay_us(): si compartiera nucleo con WiFi, cualquier ISR de
 *     radio le mete jitter y el motor pierde sincronismo o vibra. Por eso
 *     motor_task va pinneada al core 1 en solitario.
 *
 *   - motor_cmd_queue es el UNICO contrato entre "de donde viene el comando"
 *     y "como se ejecuta". Los productores activos arman
 *     un motor_cmd_t; mqtt_task manana hara lo mismo con el payload de un
 *     topico MQTT. motor_task nunca cambia.
 *
 *   - Sensores I2C: AS5600 y AHT21B viven en la MISMA task
 *     (i2c_sensors_task), en core 0, pero en DOS BUSES I2C FISICAMENTE
 *     INDEPENDIENTES (I2C_NUM_0 para el AS5600, I2C_NUM_1 para el AHT21B).
 *     Al no compartir nucleo ni recursos con motor_task, un I2C que se
 *     cuelga un instante no afecta al step generation.
 *
 *   - power_good_sem: semaforo binario que desacopla motor_task de
 *     power_monitor_task, siguiendo el mismo principio que motor_cmd_queue
 *     -- ningun modulo necesita conocer al otro. power_monitor_task es el
 *     unico que sabe leer CH224K/PG; motor_task solo sabe que existe una
 *     señal a esperar antes de energizar el driver. Ver nota de diseño
 *     mas abajo para el detalle del arranque condicionado a PG.
 *
 *   - focuser_handler: unica fuente de verdad del ESTADO logico del
 *     focuser (posicion absoluta, is_moving, temperatura, tempcomp) y
 *     unico traductor entre "posicion absoluta pedida por un transporte"
 *     y "comando relativo en motor_cmd_queue". motor_task y
 *     i2c_sensors_task le REPORTAN estado (no lo leen); cualquier
 *     transporte de red (websocket hoy, Alpaca HTTP manana) lo CONSULTA.
 *     El websocket actual sigue escribiendo DIRECTO a motor_cmd_queue a
 *     proposito -- se deja como canal de bajo nivel/debug, no pasa por
 *     focuser_handler.
 *
 * --------------------------- MODULARIDAD --------------------------------
 *
 *   tmc2209.[ch], as5600.[ch], aht21b.[ch] y focuser_handler.[ch] son
 *   AUTOCONTENIDOS: cada uno solo depende de headers de ESP-IDF/FreeRTOS
 *   (y, en el caso de focuser_handler, de motor_cmd.h -- el contrato
 *   compartido, no de otro modulo). Ninguno sabe que existen los demas.
 *   Solo main.c -- el orquestador -- los incluye a todos y los conecta.
 *
 *   Ventajas concretas de esto, no solo "orden por orden":
 *     - Cualquiera de estos modulos se puede copiar a otro proyecto
 *       ESP-IDF sin arrastrar nada mas que sus propios archivos.
 *     - Un bug o un cambio de pines en el AHT21B no puede, por
 *       construccion, filtrarse al codigo del AS5600 o del TMC2209.
 *     - Se pueden probar por separado.
 *
 * ------------------------------- COMANDOS -----------------------------------
 *
 * Por consola (USB Serial/JTAG), una linea de texto:
 *
 *     <pasos>,<direccion>\n
 *
 * Ejemplos:
 *     2000,1        -> 2000 micropasos, DIR=1 (cierra)
 *     51200,0       -> 51200 micropasos (=200 pasos completos = 1 vuelta a 1/256), DIR=0 (abre)
 *
 * Resolucion FIJA en 1/256 (mres=0). Los "pasos" del comando son MICROPASOS.
 * Convencion de direccion (fijada por tmc2209_move_steps): dir=0 ABRE el
 * focuser (posicion aumenta), dir=1 CIERRA (posicion disminuye).
 *
 * ------------------------------ CONEXIONES ----------------------------------
 *
 *   MS1 -> GPIO1  (a 0V, sin efecto: mstep_reg_select=1 hace que mande CHOPCONF.MRES)
 *   MS2 -> GPIO2  (a 0V, sin efecto, misma razon)
 *   TMC RX/TX -> GPIO7/GPIO8 unidos con resistencia ~1k -> PDN_UART (UART_NUM_1)
 *   STEP -> GPIO4
 *   DIR  -> GPIO5
 *   EN   -> GPIO6 (activo en bajo)
 *   AS5600 + AHT21B SDA/SCL -> GPIO8/GPIO9 (bus I2C compartido, I2C_NUM_0)
 *   WS status LED -> GPIO10, motor task status LED -> GPIO12
 *   Comandos -> USB Serial/JTAG nativo (consola en modo USB Serial/JTAG,
 *               default de idf.py para target esp32s3 sin chip USB-UART externo)
 *
 * --------------------------- NOTAS DE DISEÑO --------------------------------
 *
 *   - motor_task NO se autodestruye si el TMC2209 no responde al arrancar.
 *     Reintenta indefinidamente con backoff de 1s (logueando cada 10
 *     intentos) hasta lograr comunicacion y configuracion validas.
 *
 *   - Fail-fast vs degradacion controlada: tmc2209_init() aborta
 *     (ESP_ERROR_CHECK interno) si el UART del motor no se puede instalar
 *     -- sin eso el firmware entero no tiene proposito. En cambio, si el
 *     I2C de un sensor falla, i2c_sensors_task loguea el error y sigue
 *     viva con ese sensor marcado como no disponible (ready = false): un
 *     sensor caido no debe tumbar el resto del sistema.
 *
 *   - i2c_retry_detect(): la logica de "reintentar deteccion N veces con
 *     espera fija" vive aqui en main.c, no en los modulos, porque es
 *     orquestacion (que hacer si un sensor no responde al arrancar), no
 *     parte del protocolo de ningun sensor en particular. Reutiliza los
 *     mismos as5600_read_status()/aht21b_ensure_calibrated() como "probe".
 *
 *   - Deteccion inicial de AS5600/AHT21B: NO se usa i2c_master_probe(). En
 *     este driver/version de ESP-IDF probe() da falsos ESP_ERR_TIMEOUT
 *     incluso con el sensor sano. Se usa una lectura/transaccion real (la
 *     misma que usa el loop normal), asi el diagnostico de arranque queda
 *     consistente con la operacion real.
 *
 *   - Arranque de motor_task condicionado a Power Good (PG): motor_task
 *     bloquea en power_good_sem ANTES de tocar el UART del TMC2209.
 *     power_monitor_task da el semaforo una unica vez, la primera vez que
 *     ch224k_read_power_good() reporta pg_ok == true. Se uso un semaforo
 *     binario (no un flag compartido con polling) porque el consumidor
 *     (motor_task) debe quedar bloqueado sin gastar CPU mientras no haya
 *     PG, igual que ya bloquea en motor_cmd_queue mas adelante en su
 *     propio ciclo de vida. Si power_monitor_task nunca logra inicializar
 *     el CH224K, se autodestruye SIN dar el semaforo: motor_task queda
 *     esperando indefinidamente (logueando cada 2s) y jamas energiza el
 *     driver -- comportamiento deliberado: sin PG confirmado, no hay
 *     alimentacion verificada, y no se debe mover el motor a ciegas. Esto
 *     es un arranque "una sola vez": si el PG se cae DESPUES de que
 *     motor_task ya arranco, este mecanismo no lo detecta (un semaforo
 *     binario no sirve para eso). Para vigilancia continua de PG durante
 *     la operacion normal haria falta un EventGroupHandle_t con un bit
 *     set/clear segun pg_ok, chequeado (no bloqueante) en cada comando.
 *
 *   - focuser_handler NUNCA lee GPIO/UART/I2C directamente y no crea
 *     motor_cmd_queue (la recibe ya creada, igual que los drivers de
 *     sensores reciben el bus I2C ya creado). motor_task le REPORTA el
 *     resultado real de cada movimiento (con signo, ya con la direccion
 *     incorporada) DESPUES de que tmc2209_move_steps() retorna -- nunca
 *     asume que un movimiento pedido se completo integro.
 *
 *   - Observabilidad: las tres tasks guardan su TaskHandle_t y loguean
 *     uxTaskGetStackHighWaterMark() una vez, justo tras inicializar, para
 *     poder ajustar los 4096 words "a ojo" con datos reales -- relevante
 *     porque el ESP32-S3 Supermini no tiene PSRAM.
 *
 *   - PATRON DE ARCHIVO: todas las funciones se DECLARAN (prototipos) antes
 *     de app_main(), que queda arriba y se lee de un vistazo. Las
 *     definiciones completas van despues de app_main(), agrupadas por
 *     modulo.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "cJSON.h"

#include "tmc2209.h"
#include "as5600.h"
#include "aht21b.h"
#include "ch224k.h"
#include "wifi_init.h"
#include "ws_server.h"
#include "motor_cmd.h"
#include "focuser_handler.h"
#include "alpaca.h"

/* ============================================================================
 *  CONFIGURACION DE HARDWARE (pines / parametros de cada modulo)
 * ============================================================================ */

/* ---- TMC2209 ---- */
#define PIN_MS1      1
#define PIN_MS2      2
#define PIN_UART_RX  18
#define PIN_UART_TX  17
#define PIN_STEP     5
#define PIN_DIR      6
#define PIN_EN       21

#define TMC_UART_PORT   UART_NUM_1
#define TMC_ADDR        0

#define TMC_IHOLD       6
#define TMC_IRUN        24
#define TMC_IHOLDDELAY  1
#define TMC_MRES        0     /* 0 = 1/256 microstepping (resolucion FIJA) */

#define TARGET_REV_PER_SEC_START   0.05   /* velocidad de arranque, ~10 RPM */
#define TARGET_REV_PER_SEC_CRUISE  0.30   /* velocidad de crucero, ~18 RPM  */

#define MICROSTEPS          256u
#define FULL_STEPS_PER_REV  200
#define MAX_STEPS_PER_COMMAND  (FULL_STEPS_PER_REV * MICROSTEPS * 4u)

#define MOTOR_CMD_QUEUE_LEN  4

/* ---- focuser_handler: limites logicos del focoser -----
 * TODO: reemplazar por los valores reales de tu mecanismo una vez
 * caracterizado el recorrido fisico total. FOCUSER_MAX_STEP_USTEPS debe
 * ser el limite superior de position_usteps (recorrido completo del
 * focuser en micropasos); FOCUSER_STEP_SIZE_UM son los micrometros de
 * desplazamiento del elemento optico por CADA micropaso, derivados de tu
 * paso de husillo/engranaje -- es lo que Alpaca reporta como StepSize y
 * lo que conecta tu CFZ de 125 um con "cuantos micropasos son un CFZ". */
#define FOCUSER_MAX_STEP_USTEPS   206300  
#define FOCUSER_STEP_SIZE_UM      0.239                                   
#define FOCUSER_TEMPCOMP_AVAILABLE true

/* ---- Bus I2C compartido (AS5600 + AHT21B en el mismo bus fisico) ---- */
#define PIN_I2C_SENSORS_SDA   8
#define PIN_I2C_SENSORS_SCL   9
#define I2C_SENSORS_PORT      I2C_NUM_0

/* ---- AS5600 ---- */
#define AS5600_I2C_FREQ_HZ    400000

/* ---- AHT21B ---- */
#define AHT21_I2C_FREQ_HZ     100000

/* ---- Timing y reintentos comunes de i2c_sensors_task ---- */
#define I2C_BUS_SETTLE_MS          150   /* estabilizacion de tension/lineas tras boot */
#define I2C_DETECT_MAX_ATTEMPTS    8
#define I2C_DETECT_RETRY_DELAY_MS  100
#define I2C_SENSOR_POLL_PERIOD_MS  200

/* ---- Consola ---- */
/* ---- Indicadores de estado ---- */
#define PIN_WS_STATUS_LED      10
#define PIN_MOTOR_STATUS_LED   12

/* ---- CH224K Power Delivery ---- */
#define PIN_CH224K_CFG1   38
#define PIN_CH224K_CFG2   48
#define PIN_CH224K_CFG3   47
#define PIN_CH224K_PG     15
#define PIN_CH224K_VSENSE   4    /* fijo por HW: ADC_UNIT_1 canal 3 en el S3 */

#define CH224K_ADC_UNIT     ADC_UNIT_1
#define CH224K_ADC_CHANNEL  ADC_CHANNEL_3  /* GPIO3 en ESP32-S3 */
#define CH224K_R1_KOHM      20.0f
#define CH224K_R2_KOHM      2.7f

#define PRESET_STEPS_PER_MOVE   51200
#define PRESET_DIR              0
#define PRESET_MOVE_COUNT       1

/* ---- Espera de motor_task por Power Good ---- */
#define MOTOR_PG_WAIT_LOG_PERIOD_MS  2000

/* ---- WiFi / WebSocket ---- */
#define WIFI_SSID       "JuanPablo"
#define WIFI_PASSWORD   "password"
#define WIFI_MAX_RETRIES 10

#define WS_SERVER_PORT   80
#define WS_PATH          "/ws"
#define WS_MAX_CLIENTS   4

#define WS_TELEMETRY_PERIOD_MS  5000

#define WS_TELEMETRY_PERIOD_MS  5000

/* ---- Alpaca HTTP ----
 * Puerto DISTINTO de WS_SERVER_PORT a proposito: son dos servidores HTTP
 * independientes conviviendo en el mismo firmware, cada uno en su propio
 * puerto. alpaca.[ch] no conoce estas macros -- viajan como argumentos
 * en alpaca_config_t, igual que focuser_handler_config_t recibe
 * FOCUSER_MAX_STEP_USTEPS. */
#define ALPACA_HTTP_PORT         11111
#define ALPACA_SERVER_NAME       "PUJ Focuser Controller"
#define ALPACA_MANUFACTURER      "Javeriana - Grupo 5"
#define ALPACA_MANUFACTURER_VER  "0.1.0"
#define ALPACA_LOCATION          "Bogota, Colombia"
#define ALPACA_TASK_PRIORITY     3
#define ALPACA_TASK_CORE_ID      0
#define ALPACA_TASK_STACK_WORDS  4096
#define ALPACA_DEVICE_NAME         "PUJ Focuser"
#define ALPACA_DEVICE_DESCRIPTION  "Focuser controlado por TMC2209 + AS5600 + AHT21B"
#define ALPACA_DRIVER_INFO         "Javeriana G5 - Control Electronico de Enfoque Automatico"
#define ALPACA_DRIVER_VERSION      "0.1"
#define ALPACA_DEVICE_UNIQUE_ID    "puj-focuser-esp32-001"


/* ============================================================================
 *  TIPOS
 * ============================================================================
 *
 *  motor_cmd_t YA NO se declara aqui: vive en motor_cmd.h, un header
 *  neutral incluido tanto por main.c como por focuser_handler.h. Ninguno
 *  de los dos es "dueño" del contrato -- es lo que viaja por
 *  motor_cmd_queue entre cualquier fuente de comandos (consola, preset,
 *  websocket) y motor_task.
 */

/* ============================================================================
 *  ESTADO GLOBAL
 * ============================================================================ */

static const char *TAG = "APP";

static QueueHandle_t motor_cmd_queue;

/* Semaforo binario: power_monitor_task lo da UNA VEZ, la primera vez que
 * confirma PG=OK. motor_task lo toma antes de inicializar el TMC2209.
 * Desacopla ambas tasks igual que motor_cmd_queue desacopla el origen del
 * comando de quien lo ejecuta -- ninguna de las dos conoce el modulo de
 * la otra. */
static SemaphoreHandle_t power_good_sem;

static tmc2209_t motor;
static as5600_t  encoder;
static aht21b_t  climate;
static i2c_master_bus_handle_t i2c_sensors_bus = NULL; 

static TaskHandle_t motor_task_handle = NULL;
static TaskHandle_t i2c_sensors_task_handle = NULL;

static ch224k_t power_pd;
static TaskHandle_t power_task_handle = NULL;

static bool g_last_pg_ok = false;
static uint16_t g_last_angle = 0;


/* ============================================================================
 *  PROTOTIPOS
 * ============================================================================ */

/* -- Utilidad comun: reintentos de deteccion I2C con backoff fijo -- */
static esp_err_t i2c_retry_detect(esp_err_t (*probe_fn)(void *ctx), void *ctx,
                                   int max_attempts, uint32_t delay_ms, const char *sensor_name);
static esp_err_t as5600_probe(void *ctx);
static esp_err_t aht21_probe(void *ctx);

/* -- Manejo de comandos de WebSocket -- */
static void ws_on_message(const char *payload, size_t len);

/* -- Tasks de FreeRTOS -- */
static void motor_task(void *arg);
static void i2c_sensors_task(void *arg);
static void power_monitor_task(void *arg);
static void preset_cmd_task(void *arg);
static void ws_telemetry_task(void *arg);

/* ============================================================================
 *  APP_MAIN
 *
 *  Crea la queue de comandos, inicializa focuser_handler y arranca las
 *  tasks. No hace nada mas: toda la logica de arranque de cada subsistema
 *  vive dentro de su propia task (que a su vez delega en el modulo
 *  correspondiente), no aqui.
 * ============================================================================ */
void app_main(void)
{
    gpio_config_t led_cfg = {
        .pin_bit_mask = (1ULL << PIN_WS_STATUS_LED) | (1ULL << PIN_MOTOR_STATUS_LED),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&led_cfg));
    ESP_ERROR_CHECK(gpio_set_level(PIN_WS_STATUS_LED, 0));
    ESP_ERROR_CHECK(gpio_set_level(PIN_MOTOR_STATUS_LED, 0));

    motor_cmd_queue = xQueueCreate(MOTOR_CMD_QUEUE_LEN, sizeof(motor_cmd_t));
    if (motor_cmd_queue == NULL) {
        ESP_LOGE(TAG, "No se pudo crear motor_cmd_queue");
        return;
    }

    /* focuser_handler: se inicializa apenas existe motor_cmd_queue, ANTES
     * de arrancar cualquier task. Ni motor_task ni i2c_sensors_task ni
     * ws_on_message dependen de el para arrancar (el websocket sigue de
     * bajo nivel, sin pasar por aqui), pero motor_task SI necesita poder
     * llamar a focuser_handler_report_move_done()/set_moving() desde su
     * primer comando ejecutado. */
    focuser_handler_config_t fh_cfg = {
        .motor_cmd_queue    = motor_cmd_queue,
        .max_step           = (int32_t)FOCUSER_MAX_STEP_USTEPS,
        .step_size_um       = FOCUSER_STEP_SIZE_UM,
        .tempcomp_available = FOCUSER_TEMPCOMP_AVAILABLE,
    };
    ESP_ERROR_CHECK(focuser_handler_init(&fh_cfg));

    /* Semaforo de Power Good: se crea ANTES de lanzar cualquier task,
     * porque tanto motor_task como power_monitor_task lo necesitan desde
     * su primera iteracion. */
    power_good_sem = xSemaphoreCreateBinary();
    if (power_good_sem == NULL) {
        ESP_LOGE(TAG, "No se pudo crear power_good_sem");
        return;
    }

    BaseType_t created;

    /* ---- WiFi + WebSocket + Alpaca: van PRIMERO, antes de las demas
     * tasks -----
     * wifi_init_sta() BLOQUEA hasta conectar o agotar reintentos, asi que
     * si esto fuera lo ultimo del archivo, motor_task/i2c_sensors_task
     * tardarian en arrancar sin necesidad. No es fatal si WiFi falla:
     * el resto del firmware (motor, sensores) sigue funcionando sin
     * control remoto -- solo se pierden ws_server y el servidor Alpaca.
     *
     * IMPORTANTE: wifi_init_sta() SOLO se llama UNA VEZ aqui. Llamarla
     * dos veces revienta con ESP_ERR_INVALID_STATE en
     * esp_event_loop_create_default() (el event loop de WiFi no se puede
     * crear dos veces) -- por eso tanto ws_server_start() como
     * alpaca_start() viven DENTRO del mismo bloque 'else', no en un
     * segundo if/else separado. */
    wifi_init_config_app_t wifi_cfg = {
        .ssid = WIFI_SSID,
        .password = WIFI_PASSWORD,
        .max_retries = WIFI_MAX_RETRIES,
    };

    if (wifi_init_sta(&wifi_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "WiFi no conecto -- continuando SIN control remoto WS ni Alpaca.");
    } else {
        ws_server_config_t ws_cfg = {
            .port = WS_SERVER_PORT,
            .ws_path = WS_PATH,
            .on_message = ws_on_message,
            .max_clients = WS_MAX_CLIENTS,
        };

        if (ws_server_start(&ws_cfg) != ESP_OK) {
            ESP_LOGE(TAG, "No se pudo arrancar ws_server.");
        } else {
            created = xTaskCreatePinnedToCore(ws_telemetry_task, "ws_telemetry_task",
                                               4096, NULL, 3, NULL, 0);
            if (created != pdPASS) {
                ESP_LOGE(TAG, "No se pudo crear ws_telemetry_task");
            }
        }

        /* alpaca_start(): servidor Alpaca activo. Se llama AQUI,
         * dentro del MISMO bloque de WiFi ok (no en un if/else nuevo),
         * por la misma razon que ws_server_start() -- sin red no tiene
         * caso levantar un servidor HTTP. El modulo alpaca.[ch] no sabe
         * nada de este proyecto especifico (ni de focuser_handler
         * todavia, eso llega en el Paso 4/5): toda la config sale de
         * aca, igual que focuser_handler_init(). Un fallo aqui no es
         * fatal -- el resto del firmware sigue funcionando sin Alpaca. */
                alpaca_config_t alpaca_cfg = {
            .port                 = ALPACA_HTTP_PORT,
            .server_name          = ALPACA_SERVER_NAME,
            .manufacturer         = ALPACA_MANUFACTURER,
            .manufacturer_version = ALPACA_MANUFACTURER_VER,
            .location             = ALPACA_LOCATION,
            .device_name          = ALPACA_DEVICE_NAME,
            .device_description   = ALPACA_DEVICE_DESCRIPTION,
            .driver_info          = ALPACA_DRIVER_INFO,
            .driver_version       = ALPACA_DRIVER_VERSION,
            .device_unique_id     = ALPACA_DEVICE_UNIQUE_ID,
            .task_priority        = ALPACA_TASK_PRIORITY,
            .task_core_id         = ALPACA_TASK_CORE_ID,
            .task_stack_words     = ALPACA_TASK_STACK_WORDS,
        };
        if (alpaca_start(&alpaca_cfg) != ESP_OK) {
            ESP_LOGE(TAG, "No se pudo arrancar el servidor Alpaca.");
        }

    }

    /* motor_task: core 1 en solitario, prioridad alta (control de tiempo real) */
    created = xTaskCreatePinnedToCore(motor_task, "motor_task", 4096, NULL, 10, &motor_task_handle, 1);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "No se pudo crear motor_task");
    }

    /* i2c_sensors_task: AS5600 + AHT21B */
    created = xTaskCreatePinnedToCore(i2c_sensors_task, "i2c_sensors_task", 4096, NULL, 6, &i2c_sensors_task_handle, 0);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "No se pudo crear i2c_sensors_task");
    }

    /* power_monitor_task: prioridad baja (1) en Core 0 para no interferir con las demás.
     * Se crea DESPUES de motor_task a proposito: motor_task ya queda bloqueada
     * en power_good_sem esperando, y power_monitor_task es quien primero
     * confirma PG y libera ese bloqueo -- el orden de creacion no afecta la
     * correctitud (motor_task espera con timeout desde su primera vuelta),
     * pero mantiene el archivo legible en el mismo orden que la arquitectura
     * descrita arriba. */
    created = xTaskCreatePinnedToCore(power_monitor_task, "power_monitor_task", 2048, NULL, 1, &power_task_handle, 0);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "No se pudo crear power_monitor_task");
    }

    /* preset_cmd_task: sin UART disponible (USB-C en modo PD), encola
     * movimientos fijos en vez de leer consola. Prioridad baja: no es
     * critica en tiempo, solo necesita llenar la queue eventualmente. */
    created = xTaskCreatePinnedToCore(preset_cmd_task, "preset_cmd_task", 2048, NULL, 4, NULL, 0);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "No se pudo crear preset_cmd_task");
    }
}

/* ============================================================================
 *  IMPLEMENTACION - UTILIDAD COMUN: REINTENTOS DE DETECCION I2C
 * ============================================================================ */

/* Reintenta 'probe_fn' hasta max_attempts veces con 'delay_ms' entre
 * intentos, logueando cada fallo con su esp_err_to_name(). Vive en main.c
 * (no en un modulo) porque es orquestacion, no protocolo de un sensor en
 * particular: decide que hacer si un sensor no responde al arrancar,
 * reutilizando las funciones de lectura que cada driver ya expone. */
static esp_err_t i2c_retry_detect(esp_err_t (*probe_fn)(void *ctx), void *ctx, int max_attempts, uint32_t delay_ms, const char *sensor_name)
{
    esp_err_t err = ESP_FAIL;

    for (int attempt = 1; attempt <= max_attempts; attempt++) {
        err = probe_fn(ctx);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "[%s] Intento de deteccion %d/%d fallo (%s)",
                 sensor_name, attempt, max_attempts, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    return err;
}

/* Adaptadores a la firma que espera i2c_retry_detect() (esp_err_t (*)(void *)).
 * Envuelven las funciones publicas de cada modulo sin que los modulos
 * necesiten saber que existe este mecanismo de reintento. */
static esp_err_t as5600_probe(void *ctx)
{
    uint8_t status = 0;
    return as5600_read_status((as5600_t *)ctx, &status);
}

static esp_err_t aht21_probe(void *ctx)
{
    return aht21b_ensure_calibrated((aht21b_t *)ctx);
}

/* Invocado DESDE la task del httpd (ver nota de threading en
 * ws_server.h) cada vez que llega un mensaje de texto por WS. Traduce
 * JSON -> motor_cmd_t y lo encola directamente en la cola compartida,
 * reutilizando el mismo contrato (motor_cmd_queue). Formato esperado:
 *   {"cmd":"move","steps":51200,"dir":0}
 *
 * A PROPOSITO no pasa por focuser_handler_move_to(): este canal es
 * relativo/bajo-nivel por decision de diseño (ver nota de arquitectura al
 * inicio del archivo), no absoluto. focuser_handler sigue enterandose del
 * resultado igual, porque motor_task reporta CUALQUIER movimiento
 * ejecutado sin importar quien lo encolo.
 */
static void ws_on_message(const char *payload, size_t len)
{
    (void)len;

    cJSON *root = cJSON_Parse(payload);
    if (root == NULL) {
        ws_server_broadcast("{\"error\":\"json invalido\"}");
        return;
    }

    const cJSON *cmd_field = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (!cJSON_IsString(cmd_field) || strcmp(cmd_field->valuestring, "move") != 0) {
        ws_server_broadcast("{\"error\":\"cmd desconocido, usa 'move'\"}");
        cJSON_Delete(root);
        return;
    }

    const cJSON *steps_field = cJSON_GetObjectItemCaseSensitive(root, "steps");
    const cJSON *dir_field   = cJSON_GetObjectItemCaseSensitive(root, "dir");

    if (!cJSON_IsNumber(steps_field) || !cJSON_IsNumber(dir_field)) {
        ws_server_broadcast("{\"error\":\"faltan steps/dir numericos\"}");
        cJSON_Delete(root);
        return;
    }

    long steps = (long)steps_field->valuedouble;
    int dir = (int)dir_field->valuedouble;

    if (steps < 0 || (uint32_t)steps > MAX_STEPS_PER_COMMAND || (dir != 0 && dir != 1)) {
        ws_server_broadcast("{\"error\":\"steps/dir fuera de rango\"}");
        cJSON_Delete(root);
        return;
    }

    cJSON_Delete(root);

    motor_cmd_t cmd = { .steps = (uint32_t)steps, .dir = dir };

    /* Mismo timeout corto: si motor_task esta
     * ocupada, se informa al cliente en vez de bloquear la task del
     * httpd (bloquearla ahi afectaria a TODOS los clientes WS). */
    if (xQueueSend(motor_cmd_queue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        ws_server_broadcast("{\"error\":\"motor ocupado\"}");
    } else {
        char ack[64];
        snprintf(ack, sizeof(ack), "{\"ack\":\"queued\",\"steps\":%lu,\"dir\":%d}",
                 (unsigned long)cmd.steps, cmd.dir);
        ws_server_broadcast(ack);
    }
}

/* ============================================================================
 *  IMPLEMENTACION - TASKS DE FREERTOS
 * ============================================================================ */

/* ---------------------------------------------------------------------------
 *  MOTOR_TASK - Core 1
 *  Unica duena del handle `motor` (tmc2209_t). Bloquea en la queue; no le
 *  importa quien puso el comando ahi. Es tambien la UNICA task que le
 *  reporta resultados a focuser_handler: nadie mas debe llamar a
 *  focuser_handler_report_move_done()/set_moving().
 * --------------------------------------------------------------------------- */
static void motor_task(void *arg)
{
    (void)arg;

    /* La task queda viva durante toda la operación del motor, incluso
     * mientras espera Power Good o un comando en la queue. */

    /* Espera a Power Good ANTES de tocar el UART/GPIOs del TMC2209. Timeout
     * corto (en vez de portMAX_DELAY) unicamente para poder loguear
     * periodicamente que se sigue esperando -- si power_monitor_task nunca
     * confirma PG (p.ej. CH224K no inicializo), este bucle no termina
     * nunca y el driver jamas se energiza a ciegas. */
    ESP_LOGI(TAG, "[motor_task] Esperando confirmacion de Power Good (PG) antes de inicializar el driver...");
    while (xSemaphoreTake(power_good_sem, pdMS_TO_TICKS(MOTOR_PG_WAIT_LOG_PERIOD_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "[motor_task] PG aun no confirmado, esperando...");
    }
    ESP_LOGI(TAG, "[motor_task] Power Good confirmado. Inicializando TMC2209.");

        tmc2209_config_t cfg = {
        .uart_port = TMC_UART_PORT,
        .pin_uart_rx = PIN_UART_RX,
        .pin_uart_tx = PIN_UART_TX,
        .pin_step = PIN_STEP,
        .pin_dir = PIN_DIR,
        .pin_en = PIN_EN,
        .pin_ms1 = PIN_MS1,
        .pin_ms2 = PIN_MS2,
        .driver_addr = TMC_ADDR,
        .ihold = TMC_IHOLD,
        .irun = TMC_IRUN,
        .iholddelay = TMC_IHOLDDELAY,
        .mres = TMC_MRES,
        .rev_per_sec_start = TARGET_REV_PER_SEC_START,
        .rev_per_sec_cruise = TARGET_REV_PER_SEC_CRUISE,
        /* halt_check_fn: tmc2209.c NO conoce focuser_handler (modularidad,
         * ver tmc2209.h) -- este callback es lo unico que lo conecta con
         * el resto del sistema. main.c, como orquestador, es quien hace
         * ese cableado, igual que ya hace con motor_cmd_queue o
         * ws_server_config_t.on_message. */
        .halt_check_fn = focuser_handler_halt_requested,
    };

    /* tmc2209_init() aborta internamente (ESP_ERROR_CHECK) si el UART no
     * se puede instalar -- sin eso el firmware entero no tiene proposito. */
    ESP_ERROR_CHECK(tmc2209_init(&motor, &cfg));

    /* Retardo para que el TMC2209 complete su Power-On Reset (POR). */
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_LOGI(TAG, "[motor_task] Verificando comunicacion con el TMC2209...");

    /* No se usa vTaskDelete(NULL) ante un fallo de arranque: en vez de
     * matar la task para siempre, se reintenta indefinidamente con
    * backoff de 1s (logueando cada 10 intentos).
     * en cuanto el driver responda, motor_task se recupera sola. */
    bool driver_ready = false;
    uint32_t attempt = 0;

    while (!driver_ready) {
        attempt++;

        uint32_t ifcnt = 0;
        bool tmc_ok = false;

        for (int i = 0; i < 5; i++) {
            if (tmc2209_read_ifcnt(&motor, &ifcnt) == ESP_OK) {
                tmc_ok = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        if (!tmc_ok) {
            if ((attempt % 10) == 1) {
                ESP_LOGE(TAG, "[motor_task] No se pudo leer el driver (intento %lu). "
                               "Revisa alimentacion/cableado UART del TMC2209. "
                               "Reintentando en segundo plano cada 1s...",
                         (unsigned long)attempt);
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        ESP_LOGI(TAG, "[motor_task] Comunicacion OK. IFCNT=%lu", (unsigned long)ifcnt);

        tmc2209_check_status(&motor, "ANTES de configurar");
        tmc2209_configure(&motor);

        if (tmc2209_verify_chopconf(&motor) != ESP_OK) {
            if ((attempt % 10) == 1) {
                ESP_LOGE(TAG, "[motor_task] El driver no quedo confirmado en la resolucion "
                               "esperada (intento %lu). Reintentando...", (unsigned long)attempt);
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        driver_ready = true;
    }

    tmc2209_enable(&motor, true);
    vTaskDelay(pdMS_TO_TICKS(50));
    tmc2209_check_status(&motor, "DESPUES de habilitar (EN=0)");

    ESP_LOGI(TAG, "[motor_task] Listo en core %d, esperando comandos en la queue.", xPortGetCoreID());
    ESP_LOGI(TAG, "[motor_task] Stack libre minimo tras inicializacion: %u words.",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));

    motor_cmd_t cmd;
    while (true) {
        if (xQueueReceive(motor_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        focuser_handler_set_moving(true);

        ESP_LOGI(TAG, "[motor_task] Ejecutando %lu micropasos, DIR=%d", (unsigned long)cmd.steps, cmd.dir);
        ESP_ERROR_CHECK(gpio_set_level(PIN_MOTOR_STATUS_LED, 1));

        /* tmc2209_move_steps() ahora retorna int32_t: el desplazamiento
         * REALMENTE ejecutado, ya con signo (positivo = abrio/dir=0,
         * negativo = cerro/dir=1). Puede tener menor magnitud que
         * cmd.steps si el movimiento se interrumpio (halt) a medio
         * camino -- por eso se reporta el retorno, nunca cmd.steps. */
        int32_t executed_signed = tmc2209_move_steps(&motor, cmd.steps, cmd.dir);

        ESP_ERROR_CHECK(gpio_set_level(PIN_MOTOR_STATUS_LED, 0));

        focuser_handler_report_move_done(executed_signed);
        focuser_handler_set_moving(false);

        /* Paso 5: se consume el flag de halt SIEMPRE tras cada
         * movimiento, se haya pedido o no -- focuser_handler_halt_consume()
         * es idempotente (solo pone el atomic_bool en false), y hacerlo
         * incondicionalmente evita tener que comparar cmd.steps contra
         * executed_signed para "adivinar" si hubo un halt real. Si no
         * se consumiera aqui, un halt pedido DURANTE este movimiento
         * pero que tmc2209_move_steps() ya atendio internamente dejaria
         * la bandera en true y el SIGUIENTE comando de movimiento se
         * frenaria de inmediato sin razon. */
        focuser_handler_halt_consume();

        vTaskDelay(pdMS_TO_TICKS(50)); /* asentamiento antes de leer estado */
        tmc2209_check_status(&motor, "tras comando");
        printf("OK %lu,%d (ejecutado=%ld)\n", (unsigned long)cmd.steps, cmd.dir, (long)executed_signed);
    }
}

/* ---------------------------------------------------------------------------
 *  I2C_SENSORS_TASK - Core 0
 *  Unica duena de los handles `encoder` (as5600_t) y `climate` (aht21b_t).
 *  No toca motor_cmd_queue ni nada del TMC2209, asi que vive en core 0 sin
 *  afectar el step generation de motor_task (core 1). Un sensor que falla
 *  al inicializar queda con ready=false y simplemente se salta en el loop;
 *  no tumba a la task ni al otro sensor. Es la UNICA task que le reporta
 *  temperatura a focuser_handler.
 * --------------------------------------------------------------------------- */
static void i2c_sensors_task(void *arg)
{
    (void)arg;

    /* Retardo para permitir la estabilizacion de tension y las lineas I2C
     * antes de tocar el bus. */
    vTaskDelay(pdMS_TO_TICKS(I2C_BUS_SETTLE_MS));

    /* ---- Bus I2C UNICO, compartido por AS5600 y AHT21B ----
     * Se crea aqui, en el orquestador, porque ningun modulo debe saber
     * si comparte bus con otro. Cada modulo solo recibe el handle ya
     * creado via cfg->bus. */
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_SENSORS_PORT,
        .sda_io_num = PIN_I2C_SENSORS_SDA,
        .scl_io_num = PIN_I2C_SENSORS_SCL,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t bus_err = i2c_new_master_bus(&bus_cfg, &i2c_sensors_bus);
    if (bus_err != ESP_OK) {
        ESP_LOGE(TAG, "[i2c_sensors_task] No se pudo crear el bus I2C compartido (%s). "
                       "Ni AS5600 ni AHT21B podran inicializarse. Task terminada.",
                 esp_err_to_name(bus_err));
        vTaskDelete(NULL);
        return;
    }

    /* ---- Inicializacion AS5600 ---- */
    as5600_config_t as5600_cfg = {
        .bus = i2c_sensors_bus,
        .i2c_freq_hz = AS5600_I2C_FREQ_HZ,
    };

    esp_err_t err = as5600_init(&encoder, &as5600_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[i2c_sensors_task] Init del AS5600 fallo (%s).", esp_err_to_name(err));
    } else {
        /* Deteccion inicial con reintentos, usando una lectura real de
         * STATUS -- es la misma operacion que usa el loop de abajo. */
        err = i2c_retry_detect(as5600_probe, &encoder, I2C_DETECT_MAX_ATTEMPTS,
                                I2C_DETECT_RETRY_DELAY_MS, "AS5600");
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "[i2c_sensors_task] AS5600 no respondio tras %d intentos (%s). "
                           "Sigo igual entrando al loop: puede seguir asentandose.",
                     I2C_DETECT_MAX_ATTEMPTS, esp_err_to_name(err));
        } else {
            encoder.ready = true;
            ESP_LOGI(TAG, "[i2c_sensors_task] AS5600 detectado y listo en core %d.", xPortGetCoreID());
        }
    }

    /* ---- Inicializacion AHT21B ----
     * MISMO bus fisico que el AS5600 (i2c_sensors_bus), no un bus
     * independiente. No depende de que encoder.ready haya salido true. */
    aht21b_config_t aht21_cfg = {
        .bus = i2c_sensors_bus,
        .i2c_freq_hz = AHT21_I2C_FREQ_HZ,
    };

    err = aht21b_init(&climate, &aht21_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[i2c_sensors_task] Init del AHT21B fallo (%s).", esp_err_to_name(err));
    } else {
        err = i2c_retry_detect(aht21_probe, &climate, I2C_DETECT_MAX_ATTEMPTS,
                                I2C_DETECT_RETRY_DELAY_MS, "AHT21");
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "[i2c_sensors_task] AHT21B no respondio/calibro tras %d intentos (%s). "
                           "Sigo igual entrando al loop: puede seguir asentandose.",
                     I2C_DETECT_MAX_ATTEMPTS, esp_err_to_name(err));
        } else {
            climate.ready = true;
            ESP_LOGI(TAG, "[i2c_sensors_task] AHT21B detectado y calibrado en core %d.", xPortGetCoreID());
        }
    }

    ESP_LOGI(TAG, "[i2c_sensors_task] Stack libre minimo tras inicializacion: %u words.",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));

    while (true) {
        /* ---- AS5600: angulo + estado del iman ---- */
        if (encoder.ready) {
            uint8_t status = 0;
            uint16_t degrees = 0.0f;

            esp_err_t err_status = as5600_read_status(&encoder, &status);
            esp_err_t err_angle  = as5600_read_raw_angle(&encoder, &degrees);
            g_last_angle = degrees;

            if (err_status == ESP_OK && err_angle == ESP_OK) {
                ESP_LOGI(TAG, "[AS5600] angulo=%d deg iman_detectado=%d",
                         degrees, as5600_magnet_detected(status));
                if (!as5600_magnet_detected(status)) {
                    ESP_LOGW(TAG, "  -> Iman no detectado (problema mecanico, no electrico).");
                }
            } else {
                ESP_LOGE(TAG, "[AS5600] Fallo de lectura en el loop (status=%s angle=%s)",
                         esp_err_to_name(err_status), esp_err_to_name(err_angle));
            }
        }

        /* ---- AHT21B: temperatura + humedad ----
         * aht21b_read() bloquea ~100ms (conversion del ADC interno).
         * Comparte bus fisico con el AS5600, pero i2c_master es
         * thread-safe por transaccion, y aqui ambas lecturas ocurren
         * secuencialmente en la misma task -- no hay carrera. */
        if (climate.ready) {
            float temp_c = 0.0f, hum_pct = 0.0f;
            esp_err_t err_read = aht21b_read(&climate, &temp_c, &hum_pct);

            if (err_read == ESP_OK) {
                ESP_LOGI(TAG, "[AHT21] temperatura=%.2f C  humedad=%.2f %%RH", temp_c, hum_pct);
                focuser_handler_set_temperature(temp_c);
            } else {
                ESP_LOGE(TAG, "[AHT21] Fallo de lectura en el loop (%s).", esp_err_to_name(err_read));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(I2C_SENSOR_POLL_PERIOD_MS));
    }
}

/* ----------------------------------------------------------------------------
 * TAREA DE FREERTOS: LECTURA DE VOLTAJE (ADC)
 *
 * Ademas de reportar VBUS/PG periodicamente, es la UNICA task que da
 * power_good_sem -- y solo la primera vez que confirma PG=OK, para que
 * motor_task pueda arrancar el TMC2209. El flag motor_released evita
 * llamadas repetidas a xSemaphoreGive() en cada vuelta del loop (serian
 * inofensivas en un semaforo binario, pero el log de "liberando motor_task"
 * quedaria repitiendose cada 500ms).
 * ---------------------------------------------------------------------------- */
static void power_monitor_task(void *arg)
{
    (void)arg;

    ch224k_config_t cfg = {
        .pin_cfg1    = PIN_CH224K_CFG1,
        .pin_cfg2    = PIN_CH224K_CFG2,
        .pin_cfg3    = PIN_CH224K_CFG3,
        .pin_pg      = PIN_CH224K_PG,
        .enable_adc  = true,
        .adc_unit    = CH224K_ADC_UNIT,
        .adc_channel = CH224K_ADC_CHANNEL,
        .r1_kohm     = CH224K_R1_KOHM,
        .r2_kohm     = CH224K_R2_KOHM,
    };

    esp_err_t err = ch224k_init(&power_pd, &cfg);
    if (err != ESP_OK) {
        /* SIN dar power_good_sem: si el CH224K no inicializo, no hay forma
         * confiable de confirmar PG. motor_task se queda esperando para
         * siempre -- a proposito, ver nota de diseño al inicio del archivo. */
        ESP_LOGE(TAG, "[power_monitor_task] No se pudo inicializar CH224K (%s).", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    /* Perfil de 12V para alimentar drivers/motores. */
    ch224k_set_voltage(&power_pd, CH224K_VOLTAGE_20V);

    ESP_LOGI(TAG, "[power_monitor_task] Stack libre minimo tras inicializacion: %u words.",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));

    bool motor_released = false;

    while (true) {
        float v_bus = 0.0f;
        bool  pg_ok = false;

        esp_err_t err_v  = ch224k_read_voltage(&power_pd, &v_bus);
        esp_err_t err_pg = ch224k_read_power_good(&power_pd, &pg_ok);

        if (err_v == ESP_OK && err_pg == ESP_OK) {
            ESP_LOGI(TAG, "[power_monitor_task] VBUS=%.2f V  PG=%s", v_bus, pg_ok ? "OK" : "NO_POWER_GOOD");
            g_last_pg_ok = pg_ok;

            if (pg_ok && !motor_released) {
                xSemaphoreGive(power_good_sem);
                motor_released = true;
                ESP_LOGI(TAG, "[power_monitor_task] PG confirmado, liberando motor_task.");
            }
        } else {
            ESP_LOGE(TAG, "[CH224K] Fallo leyendo estado (v=%s pg=%s)", esp_err_to_name(err_v), esp_err_to_name(err_pg));
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* ---------------------------------------------------------------------------
 *  PRESET_CMD_TASK - Core 0
 *  Movimiento interno de prueba mientras no haya acceso a UART
 *  (USB-C ocupado como fuente PD, no como canal de datos). Encola N
 *  movimientos fijos y luego se autodestruye -- no compite con
 *  la consola, solo por espacio en motor_cmd_queue,
 *  que es exactamente el contrato que ya existia.
 * --------------------------------------------------------------------------- */
static void preset_cmd_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "[preset_cmd_task] Encolando %d movimientos preconfigurados "
                   "(%d pasos, dir=%d cada uno).",
             PRESET_MOVE_COUNT, PRESET_STEPS_PER_MOVE, PRESET_DIR);

    for (int i = 0; i < PRESET_MOVE_COUNT; i++) {
        motor_cmd_t cmd = { .steps = PRESET_STEPS_PER_MOVE, .dir = PRESET_DIR };

        /* portMAX_DELAY a proposito: como no hay otra tarea de consola,
         * aqui no hay nada mas que hacer salvo esperar a que motor_task
         * desocupe espacio en la queue. Encolar los 5 sin descartar
         * ninguno es el objetivo. */
        if (xQueueSend(motor_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            ESP_LOGE(TAG, "[preset_cmd_task] xQueueSend fallo inesperadamente en movimiento %d/%d.",
                     i + 1, PRESET_MOVE_COUNT);
            continue;
        }

        ESP_LOGI(TAG, "[preset_cmd_task] Movimiento %d/%d encolado.", i + 1, PRESET_MOVE_COUNT);
    }

    ESP_LOGI(TAG, "[preset_cmd_task] Los %d movimientos fueron encolados. Task terminada.",
             PRESET_MOVE_COUNT);
    vTaskDelete(NULL);
}

static void ws_telemetry_task(void *arg)
{
    (void)arg;

    TickType_t last_telemetry = xTaskGetTickCount();

    while (true) {
        bool has_client = ws_server_has_clients();
        ESP_ERROR_CHECK(gpio_set_level(PIN_WS_STATUS_LED, has_client ? 1 : 0));

        if (has_client && (xTaskGetTickCount() - last_telemetry >= pdMS_TO_TICKS(WS_TELEMETRY_PERIOD_MS))) {
            last_telemetry = xTaskGetTickCount();

            cJSON *root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "power_good", g_last_pg_ok);
            cJSON_AddNumberToObject(root, "encoder_angle", g_last_angle);
            cJSON_AddNumberToObject(root, "climate_temperature", (double)focuser_handler_get_temperature());
            cJSON_AddNumberToObject(root, "focuser_position", (double)focuser_handler_get_position());
            cJSON_AddBoolToObject(root, "focuser_is_moving", focuser_handler_get_is_moving());

            char *json_str = cJSON_PrintUnformatted(root);
            if (json_str != NULL) {
                ws_server_broadcast(json_str);
                cJSON_free(json_str);
            }
            cJSON_Delete(root);
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ============================================================================
 *  PUNTO DE EXTENSION (no implementado, referencia)
 *
 *  static void mqtt_task(void *arg)
 *  {
 *      // 1. esp_mqtt_client_start(), suscribirse a un topico como
 *      //    "motor/cmd" con payload "pasos,direccion" o JSON.
 *      // 2. En el handler de MQTT_EVENT_DATA, parsear el payload igual
 *      //    que armar un motor_cmd_t.
 *      // 3. xQueueSend(motor_cmd_queue, &cmd, pdMS_TO_TICKS(100));
 *      //
 *      // El callback de esp-mqtt corre en la task interna del cliente
 *      // MQTT: no hace falta crear otra task propia para esto, basta con
 *      // el xQueueSend() dentro del handler.
 *  }
 * ============================================================================ */