#include "tic_stats.h"
#include "esp_log.h"
#include <math.h>
#include <float.h>

static const char *TAG = "tic_stats";

// Helper to initialize channel stats
static void init_channel_stats(tic_channel_stat_t *stats)
{
    stats->count = 0;
    stats->edge_count = 0;
    stats->overflow_count = 0;
    stats->min_ns = DBL_MAX;
    stats->max_ns = 0.0;
    stats->mean_ns = 0.0;
    stats->stddev_ns = 0.0;
}

// Helper to finalize channel stats after processing
static void finalize_channel_stats(tic_channel_stat_t *stats, double mean, double m2, uint32_t n)
{
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

void tic_stats_process(const tic_event_t *events, size_t event_count,
                       uint32_t resolution_hz, tic_stats_t *stats)
{
    // Initialize stats for both channels
    init_channel_stats(&stats->ch_a);
    init_channel_stats(&stats->ch_b);

    if (event_count < 1) {
        return;
    }

    // Convert resolution to ns per tick
    double ns_per_tick = 1e9 / (double)resolution_hz;

    // Welford's algorithm variables for each channel
    double mean_a = 0.0, m2_a = 0.0;
    double mean_b = 0.0, m2_b = 0.0;
    uint32_t n_a = 0, n_b = 0;

    // Track previous edge for each channel
    uint32_t prev_value_a = 0, prev_value_b = 0;
    bool have_prev_a = false, have_prev_b = false;

    // Track overflow count for extended timestamps (per channel)
    uint64_t overflow_count_a = 0, overflow_count_b = 0;

    for (size_t i = 0; i < event_count; i++) {
        const tic_event_t *event = &events[i];

        if (event->type == TIC_EVENT_OVERFLOW) {
            // Count overflows for both channels
            stats->ch_a.overflow_count++;
            stats->ch_b.overflow_count++;
            overflow_count_a++;
            overflow_count_b++;
            continue;
        }

        if (event->type == TIC_EVENT_EDGE) {
            if (event->channel == TIC_CHANNEL_A) {
                stats->ch_a.edge_count++;

                if (!have_prev_a) {
                    prev_value_a = event->value;
                    have_prev_a = true;
                    continue;
                }

                // Calculate period with overflow handling
                uint64_t prev_extended = prev_value_a;
                uint64_t curr_extended = event->value + (overflow_count_a << 32);

                // Handle wrap-around
                if (event->value < prev_value_a && overflow_count_a == 0) {
                    curr_extended += (1ULL << 32);
                }

                uint64_t period_ticks = curr_extended - prev_extended;
                double period_ns = (double)period_ticks * ns_per_tick;

                // Update statistics using Welford's algorithm
                n_a++;
                double delta = period_ns - mean_a;
                mean_a += delta / n_a;
                double delta2 = period_ns - mean_a;
                m2_a += delta * delta2;

                // Update min/max
                if (period_ns < stats->ch_a.min_ns) {
                    stats->ch_a.min_ns = period_ns;
                }
                if (period_ns > stats->ch_a.max_ns) {
                    stats->ch_a.max_ns = period_ns;
                }

                prev_value_a = event->value;
                overflow_count_a = 0;

            } else if (event->channel == TIC_CHANNEL_B) {
                stats->ch_b.edge_count++;

                if (!have_prev_b) {
                    prev_value_b = event->value;
                    have_prev_b = true;
                    continue;
                }

                // Calculate period with overflow handling
                uint64_t prev_extended = prev_value_b;
                uint64_t curr_extended = event->value + (overflow_count_b << 32);

                // Handle wrap-around
                if (event->value < prev_value_b && overflow_count_b == 0) {
                    curr_extended += (1ULL << 32);
                }

                uint64_t period_ticks = curr_extended - prev_extended;
                double period_ns = (double)period_ticks * ns_per_tick;

                // Update statistics using Welford's algorithm
                n_b++;
                double delta = period_ns - mean_b;
                mean_b += delta / n_b;
                double delta2 = period_ns - mean_b;
                m2_b += delta * delta2;

                // Update min/max
                if (period_ns < stats->ch_b.min_ns) {
                    stats->ch_b.min_ns = period_ns;
                }
                if (period_ns > stats->ch_b.max_ns) {
                    stats->ch_b.max_ns = period_ns;
                }

                prev_value_b = event->value;
                overflow_count_b = 0;
            }
        }
    }

    // Finalize statistics for both channels
    finalize_channel_stats(&stats->ch_a, mean_a, m2_a, n_a);
    finalize_channel_stats(&stats->ch_b, mean_b, m2_b, n_b);
}

void tic_stats_print_channel(const tic_channel_stat_t *stats, const char *channel_name)
{
    if (stats->edge_count == 0) {
        ESP_LOGI(TAG, "Channel %s: no edges captured", channel_name);
        return;
    }

    if (stats->count == 0) {
        ESP_LOGI(TAG, "Channel %s: %lu edges (need 2+ for period)", channel_name,
                 (unsigned long)stats->edge_count);
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

    ESP_LOGI(TAG, "Channel %s: %lu edges, %lu periods", channel_name,
             (unsigned long)stats->edge_count, (unsigned long)stats->count);
    ESP_LOGI(TAG, "  Period: min=%.3f us  mean=%.3f us  max=%.3f us  stddev=%.3f us",
             min_us, mean_us, max_us, stddev_us);
    ESP_LOGI(TAG, "  Freq: %.3f Hz", freq_hz);
}

void tic_stats_print(const tic_stats_t *stats)
{
    ESP_LOGI(TAG, "=== TIC Statistics ===");
    tic_stats_print_channel(&stats->ch_a, "A");
    tic_stats_print_channel(&stats->ch_b, "B");
}
