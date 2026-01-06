#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "tic_capture.h"
#include "tic_stats.h"
#include "tic_test.h"

static const char *TAG = "tic_main";

static void periodic_timer_callback(void *arg)
{
    tic_capture_force_swap();
}

void app_main(void)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "=== Time Interval Counter (TIC) ===");

    // Determine loopback mode (auto-enable if gen GPIO == capture GPIO)
    bool loopback_a = (CONFIG_TIC_SIGGEN_A_GPIO == CONFIG_TIC_INPUT_GPIO_A);
    bool loopback_b = (CONFIG_TIC_SIGGEN_B_GPIO == CONFIG_TIC_INPUT_GPIO_B);

    ESP_LOGI(TAG, "Capture: A=GPIO%d B=GPIO%d", CONFIG_TIC_INPUT_GPIO_A, CONFIG_TIC_INPUT_GPIO_B);

    // Initialize signal generator A
    if (CONFIG_TIC_SIGGEN_A_FREQ_HZ > 0) {
        ESP_LOGI(TAG, "SigGen A: GPIO%d freq=%dHz loopback=%d",
                 CONFIG_TIC_SIGGEN_A_GPIO, CONFIG_TIC_SIGGEN_A_FREQ_HZ, loopback_a);

        ret = tic_siggen_init_a(CONFIG_TIC_SIGGEN_A_GPIO, CONFIG_TIC_SIGGEN_A_FREQ_HZ, loopback_a);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to init signal generator A");
            return;
        }
    } else {
        loopback_a = false;
    }

    // Initialize signal generator B
    if (CONFIG_TIC_SIGGEN_B_FREQ_HZ > 0) {
        ESP_LOGI(TAG, "SigGen B: GPIO%d freq=%dHz delay=%dns loopback=%d",
                 CONFIG_TIC_SIGGEN_B_GPIO, CONFIG_TIC_SIGGEN_B_FREQ_HZ,
                 CONFIG_TIC_SIGGEN_B_DELAY_NS, loopback_b);

        ret = tic_siggen_init_b(CONFIG_TIC_SIGGEN_B_GPIO, CONFIG_TIC_SIGGEN_B_FREQ_HZ,
                                 CONFIG_TIC_SIGGEN_B_DELAY_NS, loopback_b);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to init signal generator B");
            return;
        }
    } else {
        loopback_b = false;
    }

    ESP_LOGI(TAG, "Buffer: %d edges or %d ms", CONFIG_TIC_EDGES_PER_BUFFER, CONFIG_TIC_STATS_PERIOD_MS);

    // Initialize capture module
    ret = tic_capture_init(CONFIG_TIC_INPUT_GPIO_A, CONFIG_TIC_INPUT_GPIO_B,
                            loopback_a, loopback_b, CONFIG_TIC_EDGES_PER_BUFFER);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize capture module");
        return;
    }

    EventGroupHandle_t event_group = tic_capture_get_event_group();
    if (!event_group) {
        ESP_LOGE(TAG, "Failed to get event group");
        return;
    }

    esp_timer_handle_t periodic_timer;
    const esp_timer_create_args_t timer_args = {
        .callback = periodic_timer_callback,
        .name = "stats_timer",
    };
    ret = esp_timer_create(&timer_args, &periodic_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create periodic timer");
        return;
    }

    // Start signal generators
    if (CONFIG_TIC_SIGGEN_A_FREQ_HZ > 0 || CONFIG_TIC_SIGGEN_B_FREQ_HZ > 0) {
        ret = tic_siggen_start();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start signal generators");
            return;
        }
    }

    ret = tic_capture_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start capture");
        return;
    }

    ret = esp_timer_start_periodic(periodic_timer, CONFIG_TIC_STATS_PERIOD_MS * 1000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start periodic timer");
        return;
    }

    ESP_LOGI(TAG, "TIC running");

    uint32_t resolution = tic_capture_get_resolution();

    while (1) {
        EventBits_t bits = xEventGroupWaitBits(
            event_group,
            TIC_BUFFER_READY_BIT,
            pdTRUE,
            pdFALSE,
            portMAX_DELAY
        );

        if (bits & TIC_BUFFER_READY_BIT) {
            size_t event_count;
            tic_event_t *events = tic_capture_get_ready_buffer(&event_count);

            tic_stats_t stats;
            tic_stats_process(events, event_count, resolution, &stats);
            tic_stats_print(&stats);
        }
    }
}
