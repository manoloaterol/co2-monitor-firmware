#include "scd30_cfg.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "scd30_i2c.h"
#include "sensirion_common.h"

static const char *TAG = "SCD30";

static uint16_t s_altitude_m;
static uint16_t s_temp_offset_c100;
static bool     s_asc_active;

// Long enough for the display loop to reach its next pass even when a sensor
// transaction is timing out, which takes a few hundred milliseconds.
#define FRC_WAIT_MS 5000

// A request handed from the HTTP task to the display loop. The loop owns the
// bus: the Sensirion driver sends a command, waits and then reads, and another
// task slipping a transaction into that gap would corrupt both.
typedef enum { FRC_IDLE, FRC_PENDING, FRC_RUNNING } frc_state_t;

static portMUX_TYPE      s_frc_lock = portMUX_INITIALIZER_UNLOCKED;
static frc_state_t       s_frc_state;
static uint16_t          s_frc_ref;
static esp_err_t         s_frc_err;
static SemaphoreHandle_t s_frc_done;

// Reads first and writes only on a difference: the sensor keeps these in an
// EEPROM with a limited number of write cycles, and this runs on every boot.
static void apply_one(const char *what,
                      int16_t (*get)(uint16_t *),
                      int16_t (*set)(uint16_t),
                      uint16_t wanted,
                      uint16_t *out)
{
    uint16_t current = 0;

    if (get(&current) != NO_ERROR) {
        ESP_LOGW(TAG, "cannot read %s", what);
        *out = 0;
        return;
    }

    if (current == wanted) {
        *out = current;
        return;
    }

    if (set(wanted) != NO_ERROR) {
        ESP_LOGE(TAG, "cannot set %s", what);
        *out = current;   // report what the sensor still holds, not the wish
        return;
    }

    // Read back rather than assume: a write that returns success and a value
    // that survives are not the same claim. If the read fails there is no
    // better guess than the write having worked, but the log says so.
    uint16_t before = current;
    if (get(&current) != NO_ERROR) {
        ESP_LOGW(TAG, "%s written but not verified", what);
        current = wanted;
    }

    ESP_LOGI(TAG, "%s %u -> %u", what, (unsigned)before, (unsigned)current);
    *out = current;
}

void scd30_cfg_apply(uint16_t altitude_m, uint16_t temp_offset_c100)
{
    apply_one("altitude (m)", scd30_get_altitude_compensation,
              scd30_set_altitude_compensation, altitude_m, &s_altitude_m);

    apply_one("temperature offset (c/100)", scd30_get_temperature_offset,
              scd30_set_temperature_offset, temp_offset_c100, &s_temp_offset_c100);

    uint16_t asc = 0;
    if (scd30_get_auto_calibration_status(&asc) != NO_ERROR) {
        ESP_LOGW(TAG, "cannot read the self-calibration status");
    }
    s_asc_active = (asc != 0);

    ESP_LOGI(TAG, "sensor holds: altitude %u m, offset %.2f C, self-calibration %s",
             (unsigned)s_altitude_m, s_temp_offset_c100 / 100.0f,
             s_asc_active ? "on" : "off");

    if (s_frc_done == NULL) {
        s_frc_done = xSemaphoreCreateBinary();
    }
}

void scd30_cfg_applied(uint16_t *altitude_m, uint16_t *temp_offset_c100)
{
    if (altitude_m != NULL) {
        *altitude_m = s_altitude_m;
    }
    if (temp_offset_c100 != NULL) {
        *temp_offset_c100 = s_temp_offset_c100;
    }
}

bool scd30_cfg_asc_active(void)
{
    return s_asc_active;
}

esp_err_t scd30_cfg_frc(uint16_t ref_ppm)
{
    if (s_frc_done == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    // Drops the answer to an earlier request that timed out here but was
    // still carried out, so it is not taken for the answer to this one.
    xSemaphoreTake(s_frc_done, 0);

    taskENTER_CRITICAL(&s_frc_lock);
    bool busy = (s_frc_state != FRC_IDLE);
    if (!busy) {
        s_frc_ref   = ref_ppm;
        s_frc_state = FRC_PENDING;
    }
    taskEXIT_CRITICAL(&s_frc_lock);
    if (busy) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_frc_done, pdMS_TO_TICKS(FRC_WAIT_MS)) == pdTRUE) {
        return s_frc_err;
    }

    // Withdrawn if the loop has not picked it up yet. If it has, it is too late
    // to stop, and the caller only learns that the outcome is unknown.
    taskENTER_CRITICAL(&s_frc_lock);
    if (s_frc_state == FRC_PENDING) {
        s_frc_state = FRC_IDLE;
    }
    taskEXIT_CRITICAL(&s_frc_lock);
    return ESP_ERR_TIMEOUT;
}

void scd30_cfg_service(void)
{
    taskENTER_CRITICAL(&s_frc_lock);
    bool     run = (s_frc_state == FRC_PENDING);
    uint16_t ref = s_frc_ref;
    if (run) {
        s_frc_state = FRC_RUNNING;
    }
    taskEXIT_CRITICAL(&s_frc_lock);
    if (!run) {
        return;
    }

    int16_t err = scd30_force_recalibration(ref);
    if (err == NO_ERROR) {
        // The sensor echoes the reference until it is powered down, which at
        // least confirms the command arrived intact.
        uint16_t held = 0;
        if (scd30_get_force_recalibration_status(&held) == NO_ERROR) {
            ESP_LOGI(TAG, "recalibrated to %u ppm (sensor reports %u)",
                     (unsigned)ref, (unsigned)held);
        } else {
            ESP_LOGI(TAG, "recalibrated to %u ppm, not read back", (unsigned)ref);
        }
    } else {
        ESP_LOGE(TAG, "recalibration to %u ppm failed: %d", (unsigned)ref, err);
    }
    s_frc_err = (err == NO_ERROR) ? ESP_OK : ESP_FAIL;

    taskENTER_CRITICAL(&s_frc_lock);
    s_frc_state = FRC_IDLE;
    taskEXIT_CRITICAL(&s_frc_lock);
    xSemaphoreGive(s_frc_done);
}
