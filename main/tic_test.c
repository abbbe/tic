#include "tic_test.h"
#include "driver/mcpwm_timer.h"
#include "driver/mcpwm_oper.h"
#include "driver/mcpwm_cmpr.h"
#include "driver/mcpwm_gen.h"
#include "esp_log.h"

static const char *TAG = "tic_test";

static mcpwm_timer_handle_t s_timer = NULL;
static mcpwm_oper_handle_t s_oper = NULL;
static mcpwm_cmpr_handle_t s_cmpr = NULL;
static mcpwm_gen_handle_t s_gen = NULL;

// Internal init function with loopback option
static esp_err_t tic_test_init_internal(int gpio_num, uint32_t freq_hz, bool loopback)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "Initializing PWM generator on GPIO %d at %lu Hz (loopback=%d)",
             gpio_num, (unsigned long)freq_hz, loopback);

    // Calculate timer parameters
    // Use 1 MHz resolution for good precision at typical test frequencies
    uint32_t resolution_hz = 1000000;  // 1 MHz
    uint32_t period_ticks = resolution_hz / freq_hz;

    // Create timer
    mcpwm_timer_config_t timer_config = {
        .group_id = 0,
        .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = resolution_hz,
        .period_ticks = period_ticks,
        .count_mode = MCPWM_TIMER_COUNT_MODE_UP,
    };
    ret = mcpwm_new_timer(&timer_config, &s_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create timer: %s", esp_err_to_name(ret));
        return ret;
    }

    // Create operator
    mcpwm_operator_config_t oper_config = {
        .group_id = 0,
    };
    ret = mcpwm_new_operator(&oper_config, &s_oper);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create operator: %s", esp_err_to_name(ret));
        return ret;
    }

    // Connect operator to timer
    ret = mcpwm_operator_connect_timer(s_oper, s_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to connect operator to timer: %s", esp_err_to_name(ret));
        return ret;
    }

    // Create comparator (50% duty cycle)
    mcpwm_comparator_config_t cmpr_config = {
        .flags.update_cmp_on_tez = true,
    };
    ret = mcpwm_new_comparator(s_oper, &cmpr_config, &s_cmpr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create comparator: %s", esp_err_to_name(ret));
        return ret;
    }

    // Set comparator value for 50% duty cycle
    ret = mcpwm_comparator_set_compare_value(s_cmpr, period_ticks / 2);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set comparator value: %s", esp_err_to_name(ret));
        return ret;
    }

    // Create generator (with optional loopback)
    mcpwm_generator_config_t gen_config = {
        .gen_gpio_num = gpio_num,
        .flags.io_loop_back = loopback,
    };
    ret = mcpwm_new_generator(s_oper, &gen_config, &s_gen);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create generator: %s", esp_err_to_name(ret));
        return ret;
    }

    // Set generator actions:
    // - Set high when timer is empty (count = 0)
    // - Set low when timer reaches compare value
    ret = mcpwm_generator_set_action_on_timer_event(s_gen,
        MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set timer action: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = mcpwm_generator_set_action_on_compare_event(s_gen,
        MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, s_cmpr, MCPWM_GEN_ACTION_LOW));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set compare action: %s", esp_err_to_name(ret));
        return ret;
    }

    // Enable timer
    ret = mcpwm_timer_enable(s_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable timer: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "PWM generator initialized: period=%lu ticks, resolution=%lu Hz",
             (unsigned long)period_ticks, (unsigned long)resolution_hz);

    return ESP_OK;
}

esp_err_t tic_test_init(int gpio_num, uint32_t freq_hz)
{
    return tic_test_init_internal(gpio_num, freq_hz, true);  // With loopback
}

esp_err_t tic_test_init_no_loopback(int gpio_num, uint32_t freq_hz)
{
    return tic_test_init_internal(gpio_num, freq_hz, false);  // Without loopback
}

esp_err_t tic_test_start(void)
{
    if (!s_timer) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Starting test signal generator");
    return mcpwm_timer_start_stop(s_timer, MCPWM_TIMER_START_NO_STOP);
}

esp_err_t tic_test_stop(void)
{
    if (!s_timer) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Stopping test signal generator");
    return mcpwm_timer_start_stop(s_timer, MCPWM_TIMER_STOP_EMPTY);
}
