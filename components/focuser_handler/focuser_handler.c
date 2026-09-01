#include "focuser_handler.h"

#include <string.h>
#include <stdatomic.h>
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "focuser_handler";

/* ---- Estado protegido por s_state_mutex ----
 * Cualquier transporte (websocket, futura alpaca_http_task) lee/escribe
 * este estado UNICAMENTE a traves de las funciones publicas de este
 * archivo. Nunca acceso directo a los campos, ni siquiera desde dentro
 * del propio componente fuera de las funciones de abajo. */
typedef struct {
    QueueHandle_t motor_cmd_queue;

    int64_t position_usteps;
    bool    is_moving;
    bool    tempcomp_enabled;
    float   temperature_c;

    /* Config inmutable tras init: se lee sin lock. */
    int32_t max_step;
    double  step_size_um;
    bool    tempcomp_available;

    bool initialized;
} focuser_state_t;

static focuser_state_t s_state;
static SemaphoreHandle_t s_state_mutex = NULL;

/* Flag de halt: variable atomica SEPARADA del mutex de arriba a
 * proposito -- ver justificacion en focuser_handler.h. */
static atomic_bool s_halt_requested = false;

static inline void lock(void)   { xSemaphoreTake(s_state_mutex, portMAX_DELAY); }
static inline void unlock(void) { xSemaphoreGive(s_state_mutex); }

esp_err_t focuser_handler_init(const focuser_handler_config_t *cfg)
{
    if (cfg == NULL || cfg->motor_cmd_queue == NULL) {
        ESP_LOGE(TAG, "Config invalida: motor_cmd_queue es obligatoria.");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_state_mutex == NULL) {
        s_state_mutex = xSemaphoreCreateMutex();
        if (s_state_mutex == NULL) {
            ESP_LOGE(TAG, "No se pudo crear el mutex de estado.");
            return ESP_ERR_NO_MEM;
        }
    }

    memset(&s_state, 0, sizeof(s_state));
    s_state.motor_cmd_queue    = cfg->motor_cmd_queue;
    s_state.max_step           = cfg->max_step;
    s_state.step_size_um       = cfg->step_size_um;
    s_state.tempcomp_available = cfg->tempcomp_available;

    /* posicion de arranque = origen logico (0). Si el mecanismo necesita
     * un homing fisico real antes de confiar en Position, eso se
     * resuelve ANTES de marcar el focuser como listo para el transporte
     * (p.ej. bloqueando Connected=true hasta completar homing) -- no es
     * responsabilidad de este init. */
    s_state.position_usteps = 0;
    s_state.initialized = true;

    atomic_store(&s_halt_requested, false);

    ESP_LOGI(TAG, "Inicializado. max_step=%ld step_size=%.3f um tempcomp_available=%d",
             (long)s_state.max_step, s_state.step_size_um, (int)s_state.tempcomp_available);
    return ESP_OK;
}

void focuser_handler_report_move_done(int32_t executed_usteps_signed)
{
    if (!s_state.initialized) {
        return;
    }
    lock();
    s_state.position_usteps += (int64_t)executed_usteps_signed;
    unlock();
}

void focuser_handler_set_moving(bool moving)
{
    if (!s_state.initialized) {
        return;
    }
    lock();
    s_state.is_moving = moving;
    unlock();
}

void focuser_handler_set_temperature(float temp_c)
{
    if (!s_state.initialized) {
        return;
    }
    lock();
    s_state.temperature_c = temp_c;
    unlock();
}

int64_t focuser_handler_get_position(void)
{
    if (!s_state.initialized) {
        return 0;
    }
    lock();
    int64_t v = s_state.position_usteps;
    unlock();
    return v;
}

bool focuser_handler_get_is_moving(void)
{
    if (!s_state.initialized) {
        return false;
    }
    lock();
    bool v = s_state.is_moving;
    unlock();
    return v;
}

float focuser_handler_get_temperature(void)
{
    if (!s_state.initialized) {
        return 0.0f;
    }
    lock();
    float v = s_state.temperature_c;
    unlock();
    return v;
}

bool focuser_handler_get_tempcomp(void)
{
    if (!s_state.initialized) {
        return false;
    }
    lock();
    bool v = s_state.tempcomp_enabled;
    unlock();
    return v;
}

void focuser_handler_set_tempcomp(bool enabled)
{
    if (!s_state.initialized) {
        return;
    }
    /* Se acepta el valor aunque el hardware no soporte tempcomp (Alpaca
     * permite escribir TempComp aunque TempCompAvailable sea false sin
     * que eso sea un error de protocolo) -- pero queda sin efecto real.
     * Quien aplique la correccion feedforward debe consultar TAMBIEN
     * focuser_handler_get_tempcomp_available() antes de actuar. */
    lock();
    s_state.tempcomp_enabled = enabled;
    unlock();
}

int32_t focuser_handler_get_max_step(void)
{
    return s_state.max_step; /* inmutable tras init, no necesita lock */
}

double focuser_handler_get_step_size(void)
{
    return s_state.step_size_um; /* inmutable tras init */
}

bool focuser_handler_get_tempcomp_available(void)
{
    return s_state.tempcomp_available; /* inmutable tras init */
}

esp_err_t focuser_handler_move_to(int64_t target_position_usteps, TickType_t queue_timeout)
{
    if (!s_state.initialized) {
        ESP_LOGE(TAG, "focuser_handler_move_to() llamado antes de init.");
        return ESP_ERR_INVALID_STATE;
    }

    if (target_position_usteps < 0 || target_position_usteps > s_state.max_step) {
        ESP_LOGW(TAG, "Move rechazado: %lld fuera de rango [0, %ld]",
                 (long long)target_position_usteps, (long)s_state.max_step);
        return ESP_ERR_INVALID_ARG;
    }

    lock();
    int64_t current = s_state.position_usteps;
    unlock();

    int64_t delta = target_position_usteps - current;
    if (delta == 0) {
        return ESP_OK; /* ya esta en esa posicion, nada que encolar */
    }

    /* Convencion fijada por tmc2209_move_steps(): dir=0 abre (posicion
     * aumenta), dir=1 cierra (posicion disminuye). */
    motor_cmd_t cmd = {
        .steps = (uint32_t)(delta > 0 ? delta : -delta),
        .dir   = (delta > 0) ? 0 : 1,
    };

    if (xQueueSend(s_state.motor_cmd_queue, &cmd, queue_timeout) != pdTRUE) {
        ESP_LOGW(TAG, "motor_cmd_queue llena, Move descartado (steps=%lu dir=%d).",
                 (unsigned long)cmd.steps, cmd.dir);
        return ESP_ERR_TIMEOUT;
    }

    /* Se marca is_moving=true de inmediato: un cliente Alpaca que
     * consulte IsMoving justo despues de que Move() retorne debe verlo
     * en true, aunque motor_task todavia no haya sacado el comando de
     * la queue. motor_task es quien lo vuelve a false al terminar
     * (ver focuser_handler_set_moving() en la nota de integracion). */
    focuser_handler_set_moving(true);
    return ESP_OK;
}

void focuser_handler_request_halt(void)
{
    atomic_store(&s_halt_requested, true);
}

bool focuser_handler_halt_requested(void)
{
    return atomic_load(&s_halt_requested);
}

void focuser_handler_halt_consume(void)
{
    atomic_store(&s_halt_requested, false);
}
