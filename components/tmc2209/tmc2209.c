/*
 * ============================================================================
 *  tmc2209.c - Implementacion del driver TMC2209
 * ============================================================================
 *
 * Protocolo UART de Trinamic: datagramas de 4 bytes (lectura) u 8 bytes
 * (escritura) con CRC8, sobre PDN_UART (RX/TX unidos con una resistencia
 * ~1k). Todo el detalle de registros (direcciones, campos de bits) es
 * privado de este archivo -- el header solo expone operaciones de alto
 * nivel (configure, verify, move_steps, etc.), no los registros crudos.
 */

#include <string.h>
#include "tmc2209.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "TMC2209";

/* ---- Registros TMC2209 usados (detalle interno, no se expone en el .h) ---- */
#define REG_GCONF      0x00
#define REG_GSTAT      0x01
#define REG_IFCNT      0x02
#define REG_IHOLD_IRUN 0x10
#define REG_CHOPCONF   0x6C
#define REG_DRV_STATUS 0x6F
#define REG_MSCNT      0x6A

#define MICROSTEPS_FULL_1_256   256u
#define FULL_STEPS_PER_REV      200u
#define YIELD_EVERY_N_STEPS     500u   /* para no disparar el task watchdog */
#define STEP_PULSE_HIGH_US      2u

/* ============================================================================
 *  PROTOCOLO UART DE BAJO NIVEL (privado del modulo)
 * ============================================================================ */

/* Calcula el CRC8 usado por el protocolo TMC2209. */
static uint8_t tmc_crc8(const uint8_t *datagram, uint8_t len)
{
    uint8_t crc = 0;

    for (uint8_t i = 0; i < len; i++) {
        uint8_t current_byte = datagram[i];

        for (uint8_t j = 0; j < 8; j++) {
            if ((crc >> 7) ^ (current_byte & 0x01)) {
                crc = (uint8_t)((crc << 1) ^ 0x07);
            } else {
                crc = (uint8_t)(crc << 1);
            }
            current_byte >>= 1;
        }
    }

    return crc;
}

/* Envia por UART un comando de escritura al TMC2209 (arma datagrama + CRC).
 *
 * No retorna esp_err_t a proposito: un eco incompleto no prueba que la
 * escritura fallo (puede ser ruido solo en el camino de vuelta). Loguear la
 * anomalia aqui basta para depurar sin inventar una señal de error poco
 * confiable; la verificacion real de una escritura es leerla de vuelta (ver
 * tmc2209_verify_chopconf()). */
static void tmc_write(tmc2209_t *drv, uint8_t reg, uint32_t value)
{
    uint8_t dg[8];
    dg[0] = 0x05;
    dg[1] = drv->cfg.driver_addr;
    dg[2] = reg | 0x80;
    dg[3] = (uint8_t)(value >> 24);
    dg[4] = (uint8_t)(value >> 16);
    dg[5] = (uint8_t)(value >> 8);
    dg[6] = (uint8_t)(value);
    dg[7] = tmc_crc8(dg, 7);

    uart_flush_input(drv->cfg.uart_port);
    uart_write_bytes(drv->cfg.uart_port, (const char *)dg, 8);
    uart_wait_tx_done(drv->cfg.uart_port, pdMS_TO_TICKS(20));

    uint8_t echo[8];
    int n_echo = uart_read_bytes(drv->cfg.uart_port, echo, 8, pdMS_TO_TICKS(20));
    if (n_echo != 8) {
        ESP_LOGW(TAG, "tmc_write(reg=0x%02X): eco incompleto (%d/8 bytes). "
                       "Posible ruido/contencion en el UART del TMC2209.", reg, n_echo);
    }
}

/* Lee un registro del TMC2209 por UART, valida CRC y retorna el valor.
 * Distingue ESP_ERR_TIMEOUT (no hubo respuesta) de ESP_ERR_INVALID_CRC
 * (hubo respuesta pero corrupta). */
static esp_err_t tmc_read(tmc2209_t *drv, uint8_t reg, uint32_t *out_value)
{
    ESP_RETURN_ON_FALSE(out_value != NULL, ESP_ERR_INVALID_ARG, TAG, "tmc_read: out_value es NULL");

    uint8_t dg[4];
    dg[0] = 0x05;
    dg[1] = drv->cfg.driver_addr;
    dg[2] = reg & 0x7F;
    dg[3] = tmc_crc8(dg, 3);

    uart_flush_input(drv->cfg.uart_port);
    uart_write_bytes(drv->cfg.uart_port, (const char *)dg, 4);
    uart_wait_tx_done(drv->cfg.uart_port, pdMS_TO_TICKS(20));

    uint8_t echo[4];
    int n_echo = uart_read_bytes(drv->cfg.uart_port, echo, 4, pdMS_TO_TICKS(20));

    uint8_t resp[8];
    int n = uart_read_bytes(drv->cfg.uart_port, resp, 8, pdMS_TO_TICKS(50));

    if (n_echo != 4 || n != 8) {
        ESP_LOGE(TAG, "Sin respuesta del TMC2209 (echo=%d, resp=%d). Revisa el cableado UART.", n_echo, n);
        return ESP_ERR_TIMEOUT;
    }

    uint8_t crc_calc = tmc_crc8(resp, 7);
    if (crc_calc != resp[7]) {
        ESP_LOGE(TAG, "CRC invalido en respuesta (esperado 0x%02X, recibido 0x%02X)", crc_calc, resp[7]);
        return ESP_ERR_INVALID_CRC;
    }

    *out_value = ((uint32_t)resp[3] << 24) | ((uint32_t)resp[4] << 16) |
                 ((uint32_t)resp[5] << 8)  | (uint32_t)resp[6];
    return ESP_OK;
}

/* ============================================================================
 *  API PUBLICA
 * ============================================================================ */

esp_err_t tmc2209_init(tmc2209_t *drv, const tmc2209_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(drv != NULL && cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "tmc2209_init: argumento NULL");

    memset(drv, 0, sizeof(*drv));
    drv->cfg = *cfg;

    /* ---- GPIOs del motor ---- */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << cfg->pin_ms1) | (1ULL << cfg->pin_ms2) |
                         (1ULL << cfg->pin_step) | (1ULL << cfg->pin_dir) |
                         (1ULL << cfg->pin_en),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "No se pudo configurar GPIOs del motor");

    gpio_set_level(cfg->pin_ms1, 0);
    gpio_set_level(cfg->pin_ms2, 0);
    gpio_set_level(cfg->pin_en, 1);   /* deshabilitado hasta tmc2209_enable() */
    gpio_set_level(cfg->pin_step, 0);
    gpio_set_level(cfg->pin_dir, 0);

    /* ---- UART hacia el driver ----
     * ESP_ERROR_CHECK (aborta) a proposito: sin este UART el driver entero
     * no tiene proposito, asi que fallar rapido y ruidoso aqui es
     * preferible a seguir arrancando en un estado inutil. */
    uart_config_t uart_cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(cfg->uart_port, 256, 256, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(cfg->uart_port, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(cfg->uart_port, cfg->pin_uart_tx, cfg->pin_uart_rx,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    /* ---- Rampa: precalcular el intervalo entre pasos a partir de rev_per_sec_* ----
     * Cada iteracion genera el pulso STEP completo y espera una sola vez
     * antes del siguiente pulso, por lo que aqui debe usarse el periodo
     * completo del paso, no un semiperiodo. */
    uint32_t steps_per_rev = FULL_STEPS_PER_REV * MICROSTEPS_FULL_1_256;
    uint32_t start_period_us = (uint32_t)(1000000.0 / (cfg->rev_per_sec_start * steps_per_rev));
    uint32_t cruise_period_us = (uint32_t)(1000000.0 / (cfg->rev_per_sec_cruise * steps_per_rev));
    drv->half_period_start_us = (start_period_us > STEP_PULSE_HIGH_US)
        ? start_period_us - STEP_PULSE_HIGH_US : 1;
    drv->half_period_cruise_us = (cruise_period_us > STEP_PULSE_HIGH_US)
        ? cruise_period_us - STEP_PULSE_HIGH_US : 1;

    ESP_LOGI(TAG, "Intervalos STEP: arranque=%lu us, crucero=%lu us (%lu micropasos/rev)",
             (unsigned long)drv->half_period_start_us, (unsigned long)drv->half_period_cruise_us,
             (unsigned long)steps_per_rev);

    return ESP_OK;
}

void tmc2209_configure(tmc2209_t *drv)
{
    if (drv == NULL) {
        return;
    }

    /* 0xC0 = bits pdn_disable(6) y mstep_reg_select(7) en 1; en_spreadcycle(2)
     * queda en 0, o sea el driver arranca en modo stealthChop (silencioso),
     * NO en spreadCycle. Para spreadCycle, poner el bit 2 en 1 (0xC4). */
    tmc_write(drv, REG_GCONF, 0x000000C0);

    uint32_t ihold_irun = ((uint32_t)(drv->cfg.iholddelay & 0x0F) << 16) |
                           ((uint32_t)(drv->cfg.irun       & 0x1F) << 8)  |
                           ((uint32_t)(drv->cfg.ihold      & 0x1F));
    tmc_write(drv, REG_IHOLD_IRUN, ihold_irun);

    uint32_t chopconf = 0x00010153UL |
                         ((uint32_t)(drv->cfg.mres & 0x0F) << 24) |
                         (1UL << 28); /* intpol=1 */
    tmc_write(drv, REG_CHOPCONF, chopconf);

    vTaskDelay(pdMS_TO_TICKS(10));
}

esp_err_t tmc2209_verify_chopconf(tmc2209_t *drv)
{
    ESP_RETURN_ON_FALSE(drv != NULL, ESP_ERR_INVALID_ARG, TAG, "tmc2209_verify_chopconf: drv es NULL");

    uint32_t chopconf = 0;
    esp_err_t err = tmc_read(drv, REG_CHOPCONF, &chopconf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo leer CHOPCONF para verificar (%s).", esp_err_to_name(err));
        return err;
    }

    uint32_t mres_leido = (chopconf >> 24) & 0x0F;
    ESP_LOGI(TAG, "CHOPCONF leido = 0x%08lX  MRES=%lu (esperado %u)",
             (unsigned long)chopconf, (unsigned long)mres_leido, (unsigned)drv->cfg.mres);

    if (mres_leido != (uint32_t)drv->cfg.mres) {
        ESP_LOGE(TAG, "MISMATCH: el driver no quedo en la resolucion esperada. "
                       "Revisa la union RX/TX del PDN_UART.");
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

void tmc2209_check_status(tmc2209_t *drv, const char *when)
{
    if (drv == NULL) {
        return;
    }

    uint32_t gstat = 0, drv_status = 0, mscnt = 0;

    if (tmc_read(drv, REG_MSCNT, &mscnt) == ESP_OK) {
        ESP_LOGI(TAG, "[%s] MSCNT=%lu", when, (unsigned long)mscnt);
    }

    esp_err_t err_gstat = tmc_read(drv, REG_GSTAT, &gstat);
    if (err_gstat == ESP_OK) {
        ESP_LOGI(TAG, "[%s] GSTAT=0x%02lX reset=%d drv_err=%d uv_cp=%d",
                 when, (unsigned long)gstat,
                 (int)(gstat & 0x01), (int)((gstat >> 1) & 0x01), (int)((gstat >> 2) & 0x01));

        if (gstat & 0x01) {
            ESP_LOGE(TAG, "  -> RESET DETECTADO, reconfigurando...");
            tmc_write(drv, REG_GSTAT, 0x07);
            tmc2209_configure(drv);
            tmc2209_verify_chopconf(drv);
        }
        if (gstat & 0x02) {
            ESP_LOGW(TAG, "  -> drv_err activo (posible corto). Limpiando bandera...");
        }
        tmc_write(drv, REG_GSTAT, 0x07);
    } else {
        ESP_LOGE(TAG, "[%s] No se pudo leer GSTAT (%s)", when, esp_err_to_name(err_gstat));
    }

    esp_err_t err_drv = tmc_read(drv, REG_DRV_STATUS, &drv_status);
    if (err_drv == ESP_OK) {
        int s2ga = (drv_status >> 2) & 0x01, s2gb = (drv_status >> 3) & 0x01;
        int s2vsa = (drv_status >> 4) & 0x01, s2vsb = (drv_status >> 5) & 0x01;
        int otpw = drv_status & 0x01, ot = (drv_status >> 1) & 0x01;
        uint32_t cs_actual = (drv_status >> 16) & 0x1F;

        ESP_LOGI(TAG, "[%s] DRV_STATUS=0x%08lX otpw=%d ot=%d s2gA=%d s2gB=%d s2vsA=%d s2vsB=%d cs_actual=%lu",
                 when, (unsigned long)drv_status, otpw, ot, s2ga, s2gb, s2vsa, s2vsb, (unsigned long)cs_actual);

        if (s2ga || s2gb || s2vsa || s2vsb) {
            ESP_LOGE(TAG, "  -> CORTO detectado, revisa cableado/bobinas antes de seguir.");
        }
        if (ot) {
            ESP_LOGE(TAG, "  -> Sobretemperatura (shutdown). Deja enfriar el driver.");
        } else if (otpw) {
            ESP_LOGW(TAG, "  -> Pre-alerta de temperatura.");
        }
    } else {
        ESP_LOGE(TAG, "[%s] No se pudo leer DRV_STATUS (%s)", when, esp_err_to_name(err_drv));
    }
}

void tmc2209_enable(tmc2209_t *drv, bool enable)
{
    if (drv == NULL) {
        return;
    }
    /* Activo en bajo: enable=true -> EN=0 */
    gpio_set_level(drv->cfg.pin_en, enable ? 0 : 1);
}

esp_err_t tmc2209_read_ifcnt(tmc2209_t *drv, uint32_t *out_value)
{
    ESP_RETURN_ON_FALSE(drv != NULL, ESP_ERR_INVALID_ARG, TAG, "tmc2209_read_ifcnt: drv es NULL");
    return tmc_read(drv, REG_IFCNT, out_value);
}

int32_t tmc2209_move_steps(tmc2209_t *drv, uint32_t n, int dir_level)
{
    if (drv == NULL || n == 0) {
        return 0;
    }

    gpio_set_level(drv->cfg.pin_dir, dir_level);
    esp_rom_delay_us(20);

    int32_t usteps_count = 0;
    uint32_t ramp = n / 4;
    if (ramp == 0) {
        ramp = 1;
    }

    /* ---- Estado de frenado anticipado (Paso 5: halt) ----
     * halting: una vez true, ya abandonamos el plan original de n pasos
     * y estamos ejecutando NUESTRA propia rampa de frenado -- no se
     * vuelve a consultar halt_check_fn ni se vuelve a entrar en las
     * ramas de arranque/crucero/frenado-natural de abajo.
     *
     * decel_remaining: cuantos micropasos le faltan a esta rampa de
     * frenado propia para llegar a la velocidad de arranque (0 = ya
     * llegamos, momento de parar del todo). Se inicializa al detectar
     * el halt con el "progreso de aceleracion equivalente" en ese
     * instante (ver mas abajo) para que el frenado sea simetrico a
     * como se acelero -- ni mas brusco ni mas lento que la rampa normal. */
    bool halting = false;
    uint32_t decel_remaining = 0;

    for (uint32_t i = 0; i < n; i++) {
        /* Chequeo de halt: solo si hay callback configurado, solo si
         * todavia no iniciamos nuestra propia rampa de frenado, y solo
         * si no estamos ya dentro de la rampa de frenado NATURAL del
         * final del movimiento planeado (i >= n - ramp) -- ahi ya se
         * esta decelerando de todas formas, intervenir no aporta nada
         * y solo complicaria la contabilidad de pasos ejecutados. */
        if (!halting && drv->cfg.halt_check_fn != NULL &&
            i < n - ramp && drv->cfg.halt_check_fn()) {
            /* accel_progress: en que punto EQUIVALENTE de la rampa de
             * aceleracion estamos ahora mismo, segun la velocidad
             * actual -- si i < ramp todavia estamos acelerando, asi
             * que es directamente i; si ya pasamos el ramp, estamos a
             * velocidad de crucero, equivalente a "recien terminada"
             * la aceleracion (progreso = ramp completo). */
            uint32_t accel_progress = (i < ramp) ? i : ramp;
            halting = true;
            decel_remaining = accel_progress;
            ESP_LOGI(TAG, "[tmc2209_move_steps] Halt solicitado en paso %lu/%lu -- "
                           "frenando en %lu micropasos (en vez de continuar hasta %lu).",
                     (unsigned long)i, (unsigned long)n,
                     (unsigned long)(decel_remaining + 1), (unsigned long)n);
        }

        uint32_t half_period;
        bool halt_final_pulse = false;

        if (halting) {
            /* Misma formula lineal que la rampa de arranque/frenado
             * normal, pero usando decel_remaining como "distancia a
             * la velocidad de arranque" en vez de una posicion fija
             * dentro de n -- por eso la velocidad de este primer pulso
             * de frenado coincide exactamente con la que ya traiamos
             * (continuidad, sin salto brusco de velocidad). */
            half_period = drv->half_period_start_us -
                (uint32_t)(((int64_t)(drv->half_period_start_us - drv->half_period_cruise_us) * decel_remaining) / ramp);
            /* decel_remaining==0 -> este es el pulso final, a la
             * velocidad mas lenta -- se EJECUTA (igual que el ultimo
             * pulso de una rampa de frenado natural, ver rama de abajo
             * con j==0) y DESPUES de hacerlo se termina el movimiento
             * entero, sin llegar a los n pasos originalmente pedidos. */
            halt_final_pulse = (decel_remaining == 0);
        } else if (i < ramp) {
            half_period = drv->half_period_start_us -
                (uint32_t)(((int64_t)(drv->half_period_start_us - drv->half_period_cruise_us) * i) / ramp);
        } else if (i >= n - ramp) {
            uint32_t j = n - 1 - i;
            half_period = drv->half_period_start_us -
                (uint32_t)(((int64_t)(drv->half_period_start_us - drv->half_period_cruise_us) * j) / ramp);
        } else {
            half_period = drv->half_period_cruise_us;
        }

        gpio_set_level(drv->cfg.pin_step, 1);
        esp_rom_delay_us(2);
        gpio_set_level(drv->cfg.pin_step, 0);
        esp_rom_delay_us(half_period);
        usteps_count += 1 - (dir_level << 1);

        if (halting) {
            if (halt_final_pulse) {
                break; /* rampa de frenado completa, movimiento terminado */
            }
            decel_remaining--;
        }

        if ((i % YIELD_EVERY_N_STEPS) == 0) {
            vTaskDelay(1);
        }
    }
    return usteps_count;
}