#include "tic_stats.h"
#include "esp_log.h"
#include <math.h>
#include <float.h>

static const char *TAG = "tic_stats";

void tic_stats_process(const tic_event_t *events, size_t event_count,
                       uint32_t resolution_hz, tic_stat_t *stats)
{
    // Initialize stats
    stats->count = 0;
    stats->overflow_count = 0;
    stats->min_ns = DBL_MAX;
    stats->max_ns = 0.0;
    stats->mean_ns = 0.0;
    stats->stddev_ns = 0.0;

    if (event_count < 2) {
        // Need at least 2 edges to compute a period
        return;
    }

    // Convert resolution to ns per tick
    double ns_per_tick = 1e9 / (double)resolution_hz;

    // Welford's online algorithm variables
    double mean = 0.0;
    double m2 = 0.0;  // Sum of squared deviations
    uint32_t n = 0;   // Count of periods

    // Track overflow count for extended timestamps
    uint64_t overflow_count = 0;
    uint32_t prev_value = 0;
    bool have_prev = false;

    for (size_t i = 0; i < event_count; i++) {
        const tic_event_t *event = &events[i];

        if (event->type == TIC_EVENT_OVERFLOW) {
            overflow_count++;
            stats->overflow_count++;
            continue;
        }

        if (event->type == TIC_EVENT_EDGE) {
            if (!have_prev) {
                // First edge, just record it
                prev_value = event->value;
                have_prev = true;
                continue;
            }

            // Calculate period with overflow handling
            uint64_t prev_extended = prev_value;
            uint64_t curr_extended = event->value + (overflow_count << 32);

            // Handle wrap-around within the same overflow epoch
            // If curr < prev and no overflow logged, the timer wrapped
            if (event->value < prev_value && overflow_count == 0) {
                curr_extended += (1ULL << 32);
            }

            uint64_t period_ticks = curr_extended - prev_extended;
            double period_ns = (double)period_ticks * ns_per_tick;

            // Update statistics using Welford's algorithm
            n++;
            double delta = period_ns - mean;
            mean += delta / n;
            double delta2 = period_ns - mean;
            m2 += delta * delta2;

            // Update min/max
            if (period_ns < stats->min_ns) {
                stats->min_ns = period_ns;
            }
            if (period_ns > stats->max_ns) {
                stats->max_ns = period_ns;
            }

            // Update previous value for next iteration
            prev_value = event->value;
            // Reset overflow count after each period calculation
            overflow_count = 0;
        }
    }

    // Finalize statistics
    stats->count = n;
    stats->mean_ns = mean;

    if (n > 1) {
        double variance = m2 / (n - 1);  // Sample variance
        stats->stddev_ns = sqrt(variance);
    } else {
        stats->stddev_ns = 0.0;
    }

    // Handle case where no periods were calculated
    if (n == 0) {
        stats->min_ns = 0.0;
        stats->max_ns = 0.0;
    }
}

void tic_stats_print(const tic_stat_t *stats)
{
    ESP_LOGI(TAG, "=== TIC Statistics ===");

    if (stats->count == 0) {
        ESP_LOGI(TAG, "No periods measured (need at least 2 edges)");
        ESP_LOGI(TAG, "Overflows: %lu", (unsigned long)stats->overflow_count);
        return;
    }

    // Convert to appropriate units for display
    double min_us = stats->min_ns / 1000.0;
    double max_us = stats->max_ns / 1000.0;
    double mean_us = stats->mean_ns / 1000.0;
    double stddev_us = stats->stddev_ns / 1000.0;

    // Calculate frequency from mean period
    double freq_hz = 0.0;
    if (stats->mean_ns > 0) {
        freq_hz = 1e9 / stats->mean_ns;
    }

    ESP_LOGI(TAG, "Period:  count=%lu  min=%.3f us  mean=%.3f us  max=%.3f us  stddev=%.3f us",
             (unsigned long)stats->count, min_us, mean_us, max_us, stddev_us);
    ESP_LOGI(TAG, "Frequency: %.3f Hz", freq_hz);
    ESP_LOGI(TAG, "Overflows: %lu", (unsigned long)stats->overflow_count);
}
