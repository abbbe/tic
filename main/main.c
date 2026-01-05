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

// Periodic timer callback - forces buffer swap for stats even with low/no signal
static void periodic_timer_callback(void *arg)
{
    tic_capture_force_swap();
}

void app_main(void)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "=== Time Interval Counter (TIC) ===");
    ESP_LOGI(TAG, "Dual channel period measurement");

#if CONFIG_TIC_LOOPBACK_TEST_MODE
    ESP_LOGI(TAG, "Mode: LOOPBACK TEST");
    ESP_LOGI(TAG, "Channel A: GPIO %d (loopback), Test frequency: %d Hz",
             CONFIG_TIC_INPUT_GPIO_A, CONFIG_TIC_TEST_FREQ_HZ);
    ESP_LOGI(TAG, "Channel B: GPIO %d (disabled in loopback mode)", CONFIG_TIC_INPUT_GPIO_B);
#else
    ESP_LOGI(TAG, "Mode: EXTERNAL INPUT");
    ESP_LOGI(TAG, "Channel A: GPIO %d", CONFIG_TIC_INPUT_GPIO_A);
    ESP_LOGI(TAG, "Channel B: GPIO %d", CONFIG_TIC_INPUT_GPIO_B);
#if CONFIG_TIC_PWM_OUTPUT_ENABLE
    ESP_LOGI(TAG, "PWM Output: GPIO %d, Frequency: %d Hz",
             CONFIG_TIC_PWM_OUTPUT_GPIO, CONFIG_TIC_PWM_OUTPUT_FREQ_HZ);
#endif
#endif
    ESP_LOGI(TAG, "Edges per buffer: %d (or every %d ms)",
             CONFIG_TIC_EDGES_PER_BUFFER, CONFIG_TIC_STATS_PERIOD_MS);

#if CONFIG_TIC_LOOPBACK_TEST_MODE
    // Initialize test signal generator with loopback on Channel A
    ret = tic_test_init(CONFIG_TIC_INPUT_GPIO_A, CONFIG_TIC_TEST_FREQ_HZ);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize test signal generator");
        return;
    }
#elif CONFIG_TIC_PWM_OUTPUT_ENABLE
    // Initialize PWM output on separate pin (no loopback)
    ret = tic_test_init_no_loopback(CONFIG_TIC_PWM_OUTPUT_GPIO, CONFIG_TIC_PWM_OUTPUT_FREQ_HZ);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize PWM output");
        return;
    }
#endif

    // Initialize capture module with both channels
    // In loopback mode, only Channel A is active (Channel B disabled with -1)
    // In external mode, both channels are active
    int gpio_b = -1;  // Disabled by default
#if !CONFIG_TIC_LOOPBACK_TEST_MODE
    gpio_b = CONFIG_TIC_INPUT_GPIO_B;
#endif

    bool loopback = false;
#if CONFIG_TIC_LOOPBACK_TEST_MODE
    loopback = true;
#endif

    ret = tic_capture_init(CONFIG_TIC_INPUT_GPIO_A, gpio_b, loopback, CONFIG_TIC_EDGES_PER_BUFFER);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize capture module");
        return;
    }

    // Get event group handle
    EventGroupHandle_t event_group = tic_capture_get_event_group();
    if (!event_group) {
        ESP_LOGE(TAG, "Failed to get event group");
        return;
    }

    // Create periodic timer for forcing buffer swap (ensures stats even with 0 edges)
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

#if CONFIG_TIC_LOOPBACK_TEST_MODE || CONFIG_TIC_PWM_OUTPUT_ENABLE
    // Start test signal / PWM output generator
    ret = tic_test_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start signal generator");
        return;
    }
#endif

    // Start capture
    ret = tic_capture_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start capture");
        return;
    }

    // Start periodic timer
    ret = esp_timer_start_periodic(periodic_timer, CONFIG_TIC_STATS_PERIOD_MS * 1000);  // microseconds
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start periodic timer");
        return;
    }

    ESP_LOGI(TAG, "TIC running. Stats after %d edges OR every %d ms (whichever first).",
             CONFIG_TIC_EDGES_PER_BUFFER, CONFIG_TIC_STATS_PERIOD_MS);
#if CONFIG_TIC_LOOPBACK_TEST_MODE
    ESP_LOGI(TAG, "Expected period: %.3f us (%.1f Hz)",
             1000000.0 / CONFIG_TIC_TEST_FREQ_HZ, (double)CONFIG_TIC_TEST_FREQ_HZ);
#endif
    ESP_LOGI(TAG, "");

    // Get capture resolution
    uint32_t resolution = tic_capture_get_resolution();

    // Main loop - wait for buffer ready events
    uint32_t buffer_count = 0;
    while (1) {
        // Wait for buffer ready signal (from ISR edge count or periodic timer)
        EventBits_t bits = xEventGroupWaitBits(
            event_group,
            TIC_BUFFER_READY_BIT,
            pdTRUE,              // Clear bit on exit
            pdFALSE,             // Wait for any bit
            portMAX_DELAY        // Wait forever (timer ensures periodic wakeup)
        );

        if (bits & TIC_BUFFER_READY_BIT) {
            buffer_count++;

            // Get the ready buffer
            size_t event_count;
            tic_event_t *events = tic_capture_get_ready_buffer(&event_count);

            // Process events and calculate statistics for both channels
            tic_stats_t stats;
            tic_stats_process(events, event_count, resolution, &stats);

            // Print statistics
            ESP_LOGI(TAG, "--- Buffer #%lu: %zu events ---", (unsigned long)buffer_count, event_count);
            tic_stats_print(&stats);
            ESP_LOGI(TAG, "");  // Empty line for readability
        }
    }
}
