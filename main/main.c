#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "tic_capture.h"
#include "tic_cpu.h"
#include "tic_stats.h"
#include "tic_matcher.h"
#include "tic_test.h"
#if CONFIG_TIC_OUTPUT_BINARY
#include "tic_serial.h"
#endif
#if CONFIG_TIC_WIFI_ENABLED
#include "tic_wifi.h"
#endif

static const char *TAG = "tic_main";

static esp_timer_handle_t s_stats_timer;
static tic_matcher_t s_matcher;

static void stats_timer_callback(void *arg)
{
    // Timer fired = buffer didn't fill in time (low frequency case)
    tic_capture_force_swap();
}

static void reset_stats_timer(void)
{
    // Stop any pending timer and restart
    esp_timer_stop(s_stats_timer);
    esp_timer_start_once(s_stats_timer, CONFIG_TIC_STATS_PERIOD_MS * 1000);
}

void app_main(void)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "=== Time Interval Counter (TIC) ===");

#if CONFIG_TIC_OUTPUT_BINARY
    // Initialize USB CDC for binary output
    tic_serial_init();
    ESP_LOGI(TAG, "Binary output mode enabled (USB CDC)");
#endif

#if CONFIG_TIC_WIFI_ENABLED
    // Initialize WiFi MQTT transport
    ret = tic_wifi_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WiFi transport");
        return;
    }
    ESP_LOGI(TAG, "WiFi MQTT transport enabled");
#endif

    // Initialize CPU idle time measurement
    tic_cpu_init();

    // Determine loopback mode (auto-enable if gen GPIO == capture GPIO)
    bool loopback_a = (CONFIG_TIC_SIGGEN_A_GPIO == CONFIG_TIC_INPUT_GPIO_A);
    bool loopback_b = (CONFIG_TIC_SIGGEN_B_GPIO == CONFIG_TIC_INPUT_GPIO_B);

    ESP_LOGI(TAG, "Capture: A=GPIO%d B=GPIO%d", CONFIG_TIC_INPUT_GPIO_A, CONFIG_TIC_INPUT_GPIO_B);

    // Initialize capture module first (sets MCPWM group clock)
    ret = tic_capture_init(CONFIG_TIC_INPUT_GPIO_A, CONFIG_TIC_INPUT_GPIO_B);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize capture module");
        return;
    }

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

    ESP_LOGI(TAG, "Buffer: %d edges or %d ms", CONFIG_TIC_BUFFER_SIZE, CONFIG_TIC_STATS_PERIOD_MS);

    EventGroupHandle_t event_group = tic_capture_get_event_group();
    if (!event_group) {
        ESP_LOGE(TAG, "Failed to get event group");
        return;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = stats_timer_callback,
        .name = "stats_timer",
    };
    ret = esp_timer_create(&timer_args, &s_stats_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create stats timer");
        return;
    }

    // Start capture FIRST (must be ready before first edge)
    ret = tic_capture_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start capture");
        return;
    }

    // Then start signal generators
    if (CONFIG_TIC_SIGGEN_A_FREQ_HZ > 0 || CONFIG_TIC_SIGGEN_B_FREQ_HZ > 0) {
        ret = tic_siggen_start();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start signal generators");
            return;
        }
    }

    // Start one-shot timer (will be reset after each buffer is processed)
    ret = esp_timer_start_once(s_stats_timer, CONFIG_TIC_STATS_PERIOD_MS * 1000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start stats timer");
        return;
    }

    ESP_LOGI(TAG, "TIC running");

    uint32_t resolution = tic_capture_get_resolution();

    // Initialize the streaming edge matcher
    tic_matcher_init(&s_matcher, resolution);

    // Max delay for matching: use half period of expected signal
    // This ensures wrong pairings (off by one cycle) are rejected
#if CONFIG_TIC_SIGGEN_A_FREQ_HZ > 0
    double max_delay_ns = 0.5e9 / CONFIG_TIC_SIGGEN_A_FREQ_HZ;
#elif CONFIG_TIC_EXPECTED_FREQ_HZ > 0
    double max_delay_ns = 0.5e9 / CONFIG_TIC_EXPECTED_FREQ_HZ;
#else
    #error "No signal frequency configured - set TIC_SIGGEN_A_FREQ_HZ or TIC_EXPECTED_FREQ_HZ"
#endif

    while (1) {
        EventBits_t bits = xEventGroupWaitBits(
            event_group,
            TIC_BUFFER_READY_BIT,
            pdTRUE,
            pdFALSE,
            portMAX_DELAY
        );

        if (bits & TIC_BUFFER_READY_BIT) {
            // Reset timer - guarantees minimum interval between reports
            reset_stats_timer();

            bool overrun = tic_capture_check_overrun();
            if (overrun) {
                ESP_LOGW(TAG, "OVERRUN: buffer was overwritten during processing");
            }

            size_t event_count;
            tic_event_t *events = tic_capture_get_ready_buffer(&event_count);

            // Feed all events to the streaming matcher
            uint64_t last_timestamp = 0;
            for (size_t i = 0; i < event_count; i++) {
                const tic_event_t *evt = &events[i];
                if (evt->type == TIC_EVENT_OVERFLOW) {
                    tic_matcher_feed_overflow(&s_matcher);
                } else if (evt->type == TIC_EVENT_EDGE) {
                    tic_matcher_feed_edge(&s_matcher, evt->channel, evt->value, max_delay_ns);
                    // Track last timestamp for timeout calculation
                    last_timestamp = evt->value;
                }
            }

            // Timeout any stale pending edges (older than max_delay)
            if (last_timestamp > 0) {
                tic_matcher_timeout(&s_matcher, last_timestamp, max_delay_ns);
            }

            // Stats processing (for CSV output)
            tic_stats_t stats;
            tic_stats_process(events, event_count, resolution, &stats);

            // Get delay stats from matcher and merge into stats
            tic_matcher_get_stats(&s_matcher, &stats.delay, true);

            // Get CPU idle stats
            tic_cpu_stats_t cpu_stats;
            tic_cpu_get_stats(&cpu_stats, true);

#if CONFIG_TIC_OUTPUT_BINARY || CONFIG_TIC_WIFI_ENABLED
            // Get matched pairs for binary/WiFi output
            const tic_matched_pair_t *pairs;
            uint16_t pair_count;
            uint32_t edges_a, edges_b;
            uint64_t base_ts;
            tic_matcher_get_pairs(&s_matcher, &pairs, &pair_count, &edges_a, &edges_b, &base_ts);

            // Use edge counts from matcher for accurate reporting
            stats.ch_a.edge_count = edges_a;
            stats.ch_b.edge_count = edges_b;

#if CONFIG_TIC_OUTPUT_BINARY
            // Send via USB CDC
            tic_serial_send_frame(pairs, pair_count, &stats, &cpu_stats, resolution, base_ts, overrun);
#endif

#if CONFIG_TIC_WIFI_ENABLED
            // Send via WiFi MQTT
            tic_wifi_send_frame(pairs, pair_count, &stats, &cpu_stats, resolution, base_ts, overrun);
#endif
#endif

            // Print stats (CSV output to console)
            tic_stats_print(&stats, &cpu_stats);
        }
    }
}
