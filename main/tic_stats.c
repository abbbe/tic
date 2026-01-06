#include "tic_stats.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include <math.h>
#include <float.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "tic_stats";

// Row counter for header repeat
static uint32_t s_row_count = 0;

// Helper to initialize channel stats
static void init_channel_stats(tic_channel_stat_t *stats)
{
    stats->count = 0;
    stats->edge_count = 0;
    stats->overflow_count = 0;
    stats->min_ns = DBL_MAX;
    stats->max_ns = -DBL_MAX;
    stats->mean_ns = 0.0;
    stats->stddev_ns = 0.0;
}

// Helper to initialize delay stats
static void init_delay_stats(tic_delay_stat_t *stats)
{
    stats->count = 0;
    stats->missed_a = 0;
    stats->missed_b = 0;
    stats->min_ns = DBL_MAX;
    stats->max_ns = -DBL_MAX;
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

// Helper to finalize delay stats
static void finalize_delay_stats(tic_delay_stat_t *stats, double mean, double m2, uint32_t n)
{
    stats->count = n;
    stats->mean_ns = mean;

    if (n > 1) {
        double variance = m2 / (n - 1);
        stats->stddev_ns = sqrt(variance);
    } else {
        stats->stddev_ns = 0.0;
    }

    if (n == 0) {
        stats->min_ns = 0.0;
        stats->max_ns = 0.0;
    }
}

void tic_stats_process(const tic_event_t *events, size_t event_count,
                       uint32_t resolution_hz, tic_stats_t *stats)
{
    // Initialize stats for both channels and delay
    init_channel_stats(&stats->ch_a);
    init_channel_stats(&stats->ch_b);
    init_delay_stats(&stats->delay);

    if (event_count < 1) {
        // Finalize with zeros to clear DBL_MAX/DBL_MIN init values
        finalize_channel_stats(&stats->ch_a, 0, 0, 0);
        finalize_channel_stats(&stats->ch_b, 0, 0, 0);
        finalize_delay_stats(&stats->delay, 0, 0, 0);
        return;
    }

    // Convert resolution to ns per tick
    double ns_per_tick = 1e9 / (double)resolution_hz;

    // Welford's algorithm variables for each channel
    double mean_a = 0.0, m2_a = 0.0;
    double mean_b = 0.0, m2_b = 0.0;
    uint32_t n_a = 0, n_b = 0;

    // Track previous edge for each channel (for period calculation)
    uint32_t prev_value_a = 0, prev_value_b = 0;
    bool have_prev_a = false, have_prev_b = false;

    // Track overflow count for extended timestamps (per channel)
    uint64_t overflow_count_a = 0, overflow_count_b = 0;

    // === First pass: calculate period statistics for each channel ===
    for (size_t i = 0; i < event_count; i++) {
        const tic_event_t *event = &events[i];

        if (event->type == TIC_EVENT_OVERFLOW) {
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

                if (event->value < prev_value_a && overflow_count_a == 0) {
                    curr_extended += (1ULL << 32);
                }

                uint64_t period_ticks = curr_extended - prev_extended;
                double period_ns = (double)period_ticks * ns_per_tick;

                n_a++;
                double delta = period_ns - mean_a;
                mean_a += delta / n_a;
                double delta2 = period_ns - mean_a;
                m2_a += delta * delta2;

                if (period_ns < stats->ch_a.min_ns) stats->ch_a.min_ns = period_ns;
                if (period_ns > stats->ch_a.max_ns) stats->ch_a.max_ns = period_ns;

                prev_value_a = event->value;
                overflow_count_a = 0;

            } else if (event->channel == TIC_CHANNEL_B) {
                stats->ch_b.edge_count++;

                if (!have_prev_b) {
                    prev_value_b = event->value;
                    have_prev_b = true;
                    continue;
                }

                uint64_t prev_extended = prev_value_b;
                uint64_t curr_extended = event->value + (overflow_count_b << 32);

                if (event->value < prev_value_b && overflow_count_b == 0) {
                    curr_extended += (1ULL << 32);
                }

                uint64_t period_ticks = curr_extended - prev_extended;
                double period_ns = (double)period_ticks * ns_per_tick;

                n_b++;
                double delta = period_ns - mean_b;
                mean_b += delta / n_b;
                double delta2 = period_ns - mean_b;
                m2_b += delta * delta2;

                if (period_ns < stats->ch_b.min_ns) stats->ch_b.min_ns = period_ns;
                if (period_ns > stats->ch_b.max_ns) stats->ch_b.max_ns = period_ns;

                prev_value_b = event->value;
                overflow_count_b = 0;
            }
        }
    }

    // Finalize period statistics
    finalize_channel_stats(&stats->ch_a, mean_a, m2_a, n_a);
    finalize_channel_stats(&stats->ch_b, mean_b, m2_b, n_b);

    // === Second pass: calculate relative delays between channels ===
    // Use half the mean period as the matching window
    double window_ns = 0.0;
    if (stats->ch_a.mean_ns > 0 && stats->ch_b.mean_ns > 0) {
        window_ns = (stats->ch_a.mean_ns + stats->ch_b.mean_ns) / 4.0;  // Quarter period window
    } else if (stats->ch_a.mean_ns > 0) {
        window_ns = stats->ch_a.mean_ns / 2.0;
    } else if (stats->ch_b.mean_ns > 0) {
        window_ns = stats->ch_b.mean_ns / 2.0;
    } else {
        // No period data, can't calculate delays
        return;
    }

    // Welford's for delay
    double mean_delay = 0.0, m2_delay = 0.0;
    uint32_t n_delay = 0;

    // Count events per channel
    size_t a_count = 0, b_count = 0;
    for (size_t i = 0; i < event_count; i++) {
        if (events[i].type == TIC_EVENT_EDGE) {
            if (events[i].channel == TIC_CHANNEL_A) a_count++;
            else if (events[i].channel == TIC_CHANNEL_B) b_count++;
        }
    }

    if (a_count == 0 || b_count == 0) {
        // Can't calculate delays without both channels
        stats->delay.missed_a = a_count;
        stats->delay.missed_b = b_count;
        // Zero out min/max (they're still at DBL_MAX/-DBL_MAX from init)
        stats->delay.min_ns = 0.0;
        stats->delay.max_ns = 0.0;
        return;
    }

    // Calculate window in ticks for faster comparison
    int64_t window_ticks = (int64_t)(window_ns / ns_per_tick);

    // Allocate array to track which B edges have been matched (1:1 matching)
    bool *b_matched = calloc(b_count, sizeof(bool));
    if (!b_matched) {
        ESP_LOGE(TAG, "Failed to allocate b_matched array");
        return;
    }

    // Build index of B edge positions for efficient lookup
    size_t *b_indices = malloc(b_count * sizeof(size_t));
    if (!b_indices) {
        free(b_matched);
        ESP_LOGE(TAG, "Failed to allocate b_indices array");
        return;
    }
    size_t b_idx = 0;
    for (size_t i = 0; i < event_count && b_idx < b_count; i++) {
        if (events[i].type == TIC_EVENT_EDGE && events[i].channel == TIC_CHANNEL_B) {
            b_indices[b_idx++] = i;
        }
    }

    // Linear O(n) algorithm: process events in order, match each A with nearest unmatched B
    // Only advance search position when we actually match, so spurious edges don't skew subsequent matches
    const size_t MAX_LOOKAHEAD = 10;  // Search up to 10 B edges ahead/behind

    size_t last_matched_b = 0;  // Track last matched B index (not event position)

    for (size_t i = 0; i < event_count; i++) {
        const tic_event_t *evt_a = &events[i];

        if (evt_a->type != TIC_EVENT_EDGE || evt_a->channel != TIC_CHANNEL_A) {
            continue;
        }

        // Search for closest unmatched B within limited range around last match
        int64_t best_delay_ticks = INT64_MAX;
        size_t best_b_idx = SIZE_MAX;

        size_t search_start = (last_matched_b > MAX_LOOKAHEAD) ? (last_matched_b - MAX_LOOKAHEAD) : 0;
        size_t search_end = (last_matched_b + MAX_LOOKAHEAD + 1 < b_count) ? (last_matched_b + MAX_LOOKAHEAD + 1) : b_count;

        for (size_t bi = search_start; bi < search_end; bi++) {
            if (b_matched[bi]) continue;  // Skip already matched B edges

            const tic_event_t *evt_b = &events[b_indices[bi]];

            // Calculate delay (B - A) in ticks
            int64_t delay_ticks = (int64_t)evt_b->value - (int64_t)evt_a->value;

            // Check if within window (using ticks, not ns, for speed)
            if (llabs(delay_ticks) < window_ticks) {
                if (llabs(delay_ticks) < llabs(best_delay_ticks)) {
                    best_delay_ticks = delay_ticks;
                    best_b_idx = bi;
                }
            }
        }

        if (best_b_idx != SIZE_MAX) {
            b_matched[best_b_idx] = true;  // Mark B as consumed
            last_matched_b = best_b_idx;   // Advance search position only on match
            double delay_ns = (double)best_delay_ticks * ns_per_tick;

            // Update delay statistics using Welford's
            n_delay++;
            double delta = delay_ns - mean_delay;
            mean_delay += delta / n_delay;
            double delta2 = delay_ns - mean_delay;
            m2_delay += delta * delta2;

            if (delay_ns < stats->delay.min_ns) stats->delay.min_ns = delay_ns;
            if (delay_ns > stats->delay.max_ns) stats->delay.max_ns = delay_ns;
        }
    }

    free(b_matched);
    free(b_indices);

    // Calculate missed pulses
    stats->delay.missed_a = a_count - n_delay;
    stats->delay.missed_b = b_count - n_delay;

    // Finalize delay statistics
    finalize_delay_stats(&stats->delay, mean_delay, m2_delay, n_delay);
}

void tic_stats_print_channel(const tic_channel_stat_t *stats, const char *channel_name)
{
    if (stats->edge_count == 0) {
        ESP_LOGI(TAG, "Ch %s: no edges", channel_name);
        return;
    }

    if (stats->count == 0) {
        ESP_LOGI(TAG, "Ch %s: %lu edges (need 2+)", channel_name,
                 (unsigned long)stats->edge_count);
        return;
    }

    double min_us = stats->min_ns / 1000.0;
    double max_us = stats->max_ns / 1000.0;
    double mean_us = stats->mean_ns / 1000.0;
    double stddev_us = stats->stddev_ns / 1000.0;
    double freq_hz = (stats->mean_ns > 0) ? 1e9 / stats->mean_ns : 0.0;

    ESP_LOGI(TAG, "Ch %s: %lu edges, %.3f Hz, period [%.3f..%.3f..%.3f] us, std=%.3f us",
             channel_name, (unsigned long)stats->edge_count, freq_hz,
             min_us, mean_us, max_us, stddev_us);
}

void tic_stats_print_delay(const tic_delay_stat_t *stats)
{
    if (stats->count == 0) {
        ESP_LOGI(TAG, "Delay B-A: no matched pairs (missed A=%lu, B=%lu)",
                 (unsigned long)stats->missed_a, (unsigned long)stats->missed_b);
        return;
    }

    // Choose appropriate units
    double min_val, max_val, mean_val, stddev_val;
    const char *unit;

    double abs_mean = fabs(stats->mean_ns);
    if (abs_mean < 1000.0) {
        // Use nanoseconds
        min_val = stats->min_ns;
        max_val = stats->max_ns;
        mean_val = stats->mean_ns;
        stddev_val = stats->stddev_ns;
        unit = "ns";
    } else if (abs_mean < 1000000.0) {
        // Use microseconds
        min_val = stats->min_ns / 1000.0;
        max_val = stats->max_ns / 1000.0;
        mean_val = stats->mean_ns / 1000.0;
        stddev_val = stats->stddev_ns / 1000.0;
        unit = "us";
    } else {
        // Use milliseconds
        min_val = stats->min_ns / 1000000.0;
        max_val = stats->max_ns / 1000000.0;
        mean_val = stats->mean_ns / 1000000.0;
        stddev_val = stats->stddev_ns / 1000000.0;
        unit = "ms";
    }

    ESP_LOGI(TAG, "Delay B-A: %lu pairs, mean=%.3f %s, std=%.3f %s, [%.3f..%.3f]",
             (unsigned long)stats->count, mean_val, unit, stddev_val, unit,
             min_val, max_val);

    if (stats->missed_a > 0 || stats->missed_b > 0) {
        ESP_LOGI(TAG, "  Missed: A=%lu, B=%lu",
                 (unsigned long)stats->missed_a, (unsigned long)stats->missed_b);
    }
}

static void print_header(void)
{
#if CONFIG_TIC_OUTPUT_CSV
    printf("CSV%lu\tA_N,A_Hz,A_min_us,A_avg_us,A_max_us,A_std_us,"
           "B_N,B_Hz,B_min_us,B_avg_us,B_max_us,B_std_us,"
           "D_N,D_min_ns,D_avg_ns,D_max_ns,D_std_ns,D_missA,D_missB\n",
           (unsigned long)s_row_count);
#else
    ESP_LOGI(TAG, "A_N  |   A_Hz|A_min_us|A_avg_us|A_max_us|A_std_us|"
                  "B_N  |   B_Hz|B_min_us|B_avg_us|B_max_us|B_std_us|"
                  "D_N  |D_min_ns|D_avg_ns|D_max_ns|D_std_ns|D_missA|D_missB");
    ESP_LOGI(TAG, "-----|-------|--------|--------|--------|--------|"
                  "-----|-------|--------|--------|--------|--------|"
                  "-----|--------|--------|--------|--------|-------|-------");
#endif
}

void tic_stats_print(const tic_stats_t *stats)
{
    // Print header: once for CSV, every 24 rows for table
#if CONFIG_TIC_OUTPUT_CSV
    if (s_row_count == 0) {
        print_header();
    }
#else
    if (s_row_count % 24 == 0) {
        print_header();
    }
#endif
    s_row_count++;

    // Channel A values
    double a_hz = (stats->ch_a.mean_ns > 0) ? 1e9 / stats->ch_a.mean_ns : 0.0;
    double a_min = stats->ch_a.min_ns / 1000.0;
    double a_avg = stats->ch_a.mean_ns / 1000.0;
    double a_max = stats->ch_a.max_ns / 1000.0;
    double a_std = stats->ch_a.stddev_ns / 1000.0;

    // Channel B values
    double b_hz = (stats->ch_b.mean_ns > 0) ? 1e9 / stats->ch_b.mean_ns : 0.0;
    double b_min = stats->ch_b.min_ns / 1000.0;
    double b_avg = stats->ch_b.mean_ns / 1000.0;
    double b_max = stats->ch_b.max_ns / 1000.0;
    double b_std = stats->ch_b.stddev_ns / 1000.0;

    // Delay values (already in ns)
    double d_min = stats->delay.min_ns;
    double d_avg = stats->delay.mean_ns;
    double d_max = stats->delay.max_ns;
    double d_std = stats->delay.stddev_ns;

#if CONFIG_TIC_OUTPUT_CSV
    printf("CSV%lu\t%lu,%.2f,%.3f,%.3f,%.3f,%.3f,"
           "%lu,%.2f,%.3f,%.3f,%.3f,%.3f,"
           "%lu,%.3f,%.3f,%.3f,%.3f,%lu,%lu\n",
           (unsigned long)s_row_count,
           (unsigned long)stats->ch_a.edge_count, a_hz, a_min, a_avg, a_max, a_std,
           (unsigned long)stats->ch_b.edge_count, b_hz, b_min, b_avg, b_max, b_std,
           (unsigned long)stats->delay.count, d_min, d_avg, d_max, d_std,
           (unsigned long)stats->delay.missed_a, (unsigned long)stats->delay.missed_b);
#else
    ESP_LOGI(TAG, "%5lu|%7.2f|%8.3f|%8.3f|%8.3f|%8.3f|"
                  "%5lu|%7.2f|%8.3f|%8.3f|%8.3f|%8.3f|"
                  "%5lu|%8.3f|%8.3f|%8.3f|%8.3f|%7lu|%7lu",
             (unsigned long)stats->ch_a.edge_count, a_hz, a_min, a_avg, a_max, a_std,
             (unsigned long)stats->ch_b.edge_count, b_hz, b_min, b_avg, b_max, b_std,
             (unsigned long)stats->delay.count, d_min, d_avg, d_max, d_std,
             (unsigned long)stats->delay.missed_a, (unsigned long)stats->delay.missed_b);
#endif
}
