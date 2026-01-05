#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "tic_capture.h"
#include "tic_stats.h"
#include "tic_test.h"

static const char *TAG = "tic_main";

// Configuration
#define TEST_GPIO           4       // GPIO for test signal and capture
#define TEST_FREQ_HZ        1000    // Test signal frequency (1 kHz)
#define EDGES_PER_BUFFER    1000    // Number of edges per statistics window (edge-triggered swap)
#define STATS_PERIOD_MS     1000    // Periodic stats interval (time-triggered swap)

// Periodic timer callback - forces buffer swap for stats even with low/no signal
static void periodic_timer_callback(void *arg)
{
    tic_capture_force_swap();
}

void app_main(void)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "=== Time Interval Counter (TIC) ===");
    ESP_LOGI(TAG, "Phase 1: Single channel period measurement");
    ESP_LOGI(TAG, "Test GPIO: %d, Test frequency: %d Hz", TEST_GPIO, TEST_FREQ_HZ);
    ESP_LOGI(TAG, "Edges per buffer: %d (or every %d ms)", EDGES_PER_BUFFER, STATS_PERIOD_MS);

    // Initialize test signal generator (MCPWM PWM)
    ret = tic_test_init(TEST_GPIO, TEST_FREQ_HZ);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize test signal generator");
        return;
    }

    // Initialize capture module with loopback enabled
    ret = tic_capture_init(TEST_GPIO, true, EDGES_PER_BUFFER);
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

    // Start test signal generator
    ret = tic_test_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start test signal generator");
        return;
    }

    // Start capture
    ret = tic_capture_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start capture");
        return;
    }

    // Start periodic timer
    ret = esp_timer_start_periodic(periodic_timer, STATS_PERIOD_MS * 1000);  // microseconds
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start periodic timer");
        return;
    }

    ESP_LOGI(TAG, "TIC running. Stats after %d edges OR every %d ms (whichever first).",
             EDGES_PER_BUFFER, STATS_PERIOD_MS);
    ESP_LOGI(TAG, "Expected period: %.3f us (%.1f Hz)",
             1000000.0 / TEST_FREQ_HZ, (double)TEST_FREQ_HZ);
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

            // Process events and calculate statistics
            tic_stat_t stats;
            tic_stats_process(events, event_count, resolution, &stats);

            // Print statistics
            ESP_LOGI(TAG, "Buffer #%lu: %zu edges captured", (unsigned long)buffer_count, event_count);
            tic_stats_print(&stats);
            ESP_LOGI(TAG, "");  // Empty line for readability
        }
    }
}
