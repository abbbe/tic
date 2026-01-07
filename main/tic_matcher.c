#include "tic_matcher.h"
#include "tic_capture.h"
#include <math.h>
#include <float.h>

void tic_matcher_init(tic_matcher_t *m, uint32_t resolution_hz)
{
    m->pending_a = 0;
    m->pending_b = 0;
    m->last_value_a = 0;
    m->last_value_b = 0;
    m->overflow_count_a = 0;
    m->overflow_count_b = 0;
    m->ns_per_tick = 1e9 / (double)resolution_hz;

    m->delay_count = 0;
    m->delay_mean = 0.0;
    m->delay_m2 = 0.0;
    m->delay_min = DBL_MAX;
    m->delay_max = -DBL_MAX;
    m->missed_a = 0;
    m->missed_b = 0;
}

// Extend 32-bit value to 64-bit using overflow tracking
static uint64_t extend_timestamp(uint32_t value, uint32_t *last_value,
                                  uint64_t *overflow_count)
{
    // Detect wraparound: if new value is much smaller than last
    if (*last_value > 0 && value < *last_value &&
        (*last_value - value) > 0x80000000UL) {
        (*overflow_count)++;
    }
    *last_value = value;
    return ((uint64_t)(*overflow_count) << 32) | value;
}

// Record a delay measurement using Welford's algorithm
static void record_delay(tic_matcher_t *m, double delay_ns)
{
    m->delay_count++;
    double delta = delay_ns - m->delay_mean;
    m->delay_mean += delta / m->delay_count;
    double delta2 = delay_ns - m->delay_mean;
    m->delay_m2 += delta * delta2;

    if (delay_ns < m->delay_min) m->delay_min = delay_ns;
    if (delay_ns > m->delay_max) m->delay_max = delay_ns;
}

bool tic_matcher_feed_edge(tic_matcher_t *m, uint8_t channel,
                           uint32_t value, double max_delay_ns)
{
    uint64_t extended;
    uint64_t *pending_this;
    uint64_t *pending_other;
    uint32_t *missed_this;
    uint32_t *missed_other;

    if (channel == TIC_CHANNEL_A) {
        extended = extend_timestamp(value, &m->last_value_a, &m->overflow_count_a);
        pending_this = &m->pending_a;
        pending_other = &m->pending_b;
        missed_this = &m->missed_a;
        missed_other = &m->missed_b;
    } else {
        extended = extend_timestamp(value, &m->last_value_b, &m->overflow_count_b);
        pending_this = &m->pending_b;
        pending_other = &m->pending_a;
        missed_this = &m->missed_b;
        missed_other = &m->missed_a;
    }

    // If there's already a pending edge on this channel, discard it
    // (new edge is always a better match candidate for future opposite edges)
    if (*pending_this != 0) {
        (*missed_this)++;
    }

    // Store this edge as pending
    *pending_this = extended;

    // Try to match with pending edge on opposite channel
    if (*pending_other != 0) {
        // Calculate delay in ticks (signed)
        int64_t delay_ticks;
        if (channel == TIC_CHANNEL_A) {
            // This is A, other is B: delay = B - A
            delay_ticks = (int64_t)(*pending_other) - (int64_t)extended;
        } else {
            // This is B, other is A: delay = B - A = this - other
            delay_ticks = (int64_t)extended - (int64_t)(*pending_other);
        }

        double delay_ns = (double)delay_ticks * m->ns_per_tick;

        if (fabs(delay_ns) <= max_delay_ns) {
            // Match! Record delay and clear both pending
            record_delay(m, delay_ns);
            *pending_this = 0;
            *pending_other = 0;
            return true;
        } else {
            // Too far apart - discard the older one
            if (*pending_other < *pending_this) {
                // Other is older, discard it
                (*missed_other)++;
                *pending_other = 0;
            }
            // Note: if this edge is older (shouldn't happen in chronological order),
            // we keep it pending and let the next edge handle it
        }
    }

    return false;
}

void tic_matcher_feed_overflow(tic_matcher_t *m)
{
    // Overflow affects both channels
    m->overflow_count_a++;
    m->overflow_count_b++;
}

void tic_matcher_timeout(tic_matcher_t *m, uint64_t current_time, double max_age_ns)
{
    double max_age_ticks = max_age_ns / m->ns_per_tick;

    if (m->pending_a != 0) {
        int64_t age = (int64_t)current_time - (int64_t)m->pending_a;
        if (age > max_age_ticks) {
            m->missed_a++;
            m->pending_a = 0;
        }
    }

    if (m->pending_b != 0) {
        int64_t age = (int64_t)current_time - (int64_t)m->pending_b;
        if (age > max_age_ticks) {
            m->missed_b++;
            m->pending_b = 0;
        }
    }
}

void tic_matcher_get_stats(tic_matcher_t *m, tic_delay_stat_t *stats, bool reset)
{
    stats->count = m->delay_count;
    stats->missed_a = m->missed_a;
    stats->missed_b = m->missed_b;
    stats->mean_ns = m->delay_mean;

    if (m->delay_count > 1) {
        double variance = m->delay_m2 / (m->delay_count - 1);
        stats->stddev_ns = sqrt(variance);
    } else {
        stats->stddev_ns = 0.0;
    }

    if (m->delay_count > 0) {
        stats->min_ns = m->delay_min;
        stats->max_ns = m->delay_max;
    } else {
        stats->min_ns = 0.0;
        stats->max_ns = 0.0;
    }

    if (reset) {
        m->delay_count = 0;
        m->delay_mean = 0.0;
        m->delay_m2 = 0.0;
        m->delay_min = DBL_MAX;
        m->delay_max = -DBL_MAX;
        m->missed_a = 0;
        m->missed_b = 0;
        // Note: don't reset pending edges or overflow counters
    }
}
