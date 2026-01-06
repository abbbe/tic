#include "tic_test.h"
#include "driver/mcpwm_timer.h"
#include "driver/mcpwm_oper.h"
#include "driver/mcpwm_cmpr.h"
#include "driver/mcpwm_gen.h"
#include "driver/mcpwm_sync.h"
#include "esp_log.h"

static const char *TAG = "tic_siggen";

#define TIMER_RESOLUTION_HZ 1000000  // 1 MHz = 1us per tick

// Generator A handles
static mcpwm_timer_handle_t s_timer_a = NULL;
static mcpwm_oper_handle_t s_oper_a = NULL;
static mcpwm_cmpr_handle_t s_cmpr_a = NULL;
static mcpwm_gen_handle_t s_gen_a = NULL;

// Generator B handles
static mcpwm_timer_handle_t s_timer_b = NULL;
static mcpwm_oper_handle_t s_oper_b = NULL;
static mcpwm_cmpr_handle_t s_cmpr_b = NULL;
static mcpwm_gen_handle_t s_gen_b = NULL;

// Sync source for phase alignment
static mcpwm_sync_handle_t s_sync_src = NULL;

// Stored delay for B (in timer ticks)
static int32_t s_delay_ticks = 0;

// Helper to create one PWM channel
static esp_err_t create_pwm_channel(int group_id, int gpio_num, bool loopback,
                                     uint32_t freq_hz,
                                     mcpwm_timer_handle_t *timer,
                                     mcpwm_oper_handle_t *oper,
                                     mcpwm_cmpr_handle_t *cmpr,
                                     mcpwm_gen_handle_t *gen)
{
    esp_err_t ret;
    uint32_t period_ticks = TIMER_RESOLUTION_HZ / freq_hz;

    // Create timer
    mcpwm_timer_config_t timer_config = {
        .group_id = group_id,
        .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = TIMER_RESOLUTION_HZ,
        .period_ticks = period_ticks,
        .count_mode = MCPWM_TIMER_COUNT_MODE_UP,
    };
    ret = mcpwm_new_timer(&timer_config, timer);
    if (ret != ESP_OK) return ret;

    // Create operator
    mcpwm_operator_config_t oper_config = {
        .group_id = group_id,
    };
    ret = mcpwm_new_operator(&oper_config, oper);
    if (ret != ESP_OK) return ret;

    ret = mcpwm_operator_connect_timer(*oper, *timer);
    if (ret != ESP_OK) return ret;

    // Create comparator (50% duty cycle)
    mcpwm_comparator_config_t cmpr_config = {
        .flags.update_cmp_on_tez = true,
    };
    ret = mcpwm_new_comparator(*oper, &cmpr_config, cmpr);
    if (ret != ESP_OK) return ret;

    ret = mcpwm_comparator_set_compare_value(*cmpr, period_ticks / 2);
    if (ret != ESP_OK) return ret;

    // Create generator
    mcpwm_generator_config_t gen_config = {
        .gen_gpio_num = gpio_num,
        .flags.io_loop_back = loopback,
    };
    ret = mcpwm_new_generator(*oper, &gen_config, gen);
    if (ret != ESP_OK) return ret;

    // Set generator actions: high on empty, low on compare
    ret = mcpwm_generator_set_action_on_timer_event(*gen,
        MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH));
    if (ret != ESP_OK) return ret;

    ret = mcpwm_generator_set_action_on_compare_event(*gen,
        MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, *cmpr, MCPWM_GEN_ACTION_LOW));
    if (ret != ESP_OK) return ret;

    ret = mcpwm_timer_enable(*timer);
    if (ret != ESP_OK) return ret;

    return ESP_OK;
}

esp_err_t tic_siggen_init_a(int gpio, uint32_t freq_hz, bool loopback)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "Init A: GPIO%d freq=%luHz loopback=%d", gpio, (unsigned long)freq_hz, loopback);

    ret = create_pwm_channel(0, gpio, loopback, freq_hz,
                              &s_timer_a, &s_oper_a, &s_cmpr_a, &s_gen_a);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create generator A: %s", esp_err_to_name(ret));
        return ret;
    }

    // Create software sync source (shared between A and B)
    if (!s_sync_src) {
        mcpwm_soft_sync_config_t sync_config = {};
        ret = mcpwm_new_soft_sync_src(&sync_config, &s_sync_src);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create sync source: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    // Configure timer A to reset to 0 on sync
    mcpwm_timer_sync_phase_config_t sync_phase_a = {
        .sync_src = s_sync_src,
        .count_value = 0,
        .direction = MCPWM_TIMER_DIRECTION_UP,
    };
    ret = mcpwm_timer_set_phase_on_sync(s_timer_a, &sync_phase_a);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set sync phase A: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

esp_err_t tic_siggen_init_b(int gpio, uint32_t freq_hz, int32_t delay_ns, bool loopback)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "Init B: GPIO%d freq=%luHz delay=%ldns loopback=%d",
             gpio, (unsigned long)freq_hz, (long)delay_ns, loopback);

    ret = create_pwm_channel(0, gpio, loopback, freq_hz,
                              &s_timer_b, &s_oper_b, &s_cmpr_b, &s_gen_b);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create generator B: %s", esp_err_to_name(ret));
        return ret;
    }

    // Convert delay from ns to timer ticks (1 MHz = 1000 ns per tick)
    s_delay_ticks = delay_ns / 1000;

    // Get period for bounds checking
    uint32_t period_ticks = TIMER_RESOLUTION_HZ / freq_hz;

    // Wrap delay into valid range [0, period)
    int32_t phase_ticks = s_delay_ticks % (int32_t)period_ticks;
    if (phase_ticks < 0) {
        phase_ticks += period_ticks;
    }

    ESP_LOGI(TAG, "B delay: %ld ns = %ld ticks (phase=%ld in period=%lu)",
             (long)delay_ns, (long)s_delay_ticks, (long)phase_ticks, (unsigned long)period_ticks);

    // Create sync source if not already created
    if (!s_sync_src) {
        mcpwm_soft_sync_config_t sync_config = {};
        ret = mcpwm_new_soft_sync_src(&sync_config, &s_sync_src);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create sync source: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    // Configure timer B to reset to phase_ticks on sync
    mcpwm_timer_sync_phase_config_t sync_phase_b = {
        .sync_src = s_sync_src,
        .count_value = (uint32_t)phase_ticks,
        .direction = MCPWM_TIMER_DIRECTION_UP,
    };
    ret = mcpwm_timer_set_phase_on_sync(s_timer_b, &sync_phase_b);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set sync phase B: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

esp_err_t tic_siggen_start(void)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "Starting signal generators");

    if (s_timer_a) {
        ret = mcpwm_timer_start_stop(s_timer_a, MCPWM_TIMER_START_NO_STOP);
        if (ret != ESP_OK) return ret;
    }

    if (s_timer_b) {
        ret = mcpwm_timer_start_stop(s_timer_b, MCPWM_TIMER_START_NO_STOP);
        if (ret != ESP_OK) return ret;
    }

    // Trigger sync to align phases
    if (s_sync_src) {
        ret = mcpwm_soft_sync_activate(s_sync_src);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to activate sync: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    return ESP_OK;
}

esp_err_t tic_siggen_stop(void)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "Stopping signal generators");

    if (s_timer_a) {
        ret = mcpwm_timer_start_stop(s_timer_a, MCPWM_TIMER_STOP_EMPTY);
        if (ret != ESP_OK) return ret;
    }

    if (s_timer_b) {
        ret = mcpwm_timer_start_stop(s_timer_b, MCPWM_TIMER_STOP_EMPTY);
        if (ret != ESP_OK) return ret;
    }

    return ESP_OK;
}
