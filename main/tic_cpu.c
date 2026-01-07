#include "tic_cpu.h"
#include "esp_freertos_hooks.h"
#include <stdatomic.h>

static atomic_uint_fast32_t s_tick_count[2];
static atomic_uint_fast32_t s_idle_count[2];

// Track if we've already counted idle for the current tick
static uint32_t s_last_tick_cpu0;
static uint32_t s_last_tick_cpu1;

static void IRAM_ATTR tick_hook_cpu0(void)
{
    s_tick_count[0]++;
}

static void IRAM_ATTR tick_hook_cpu1(void)
{
    s_tick_count[1]++;
}

static bool IRAM_ATTR idle_hook_cpu0(void)
{
    // Only count idle once per tick
    uint32_t current_tick = s_tick_count[0];
    if (current_tick != s_last_tick_cpu0) {
        s_last_tick_cpu0 = current_tick;
        s_idle_count[0]++;
    }
    return false;  // Call repeatedly (we filter duplicates ourselves)
}

static bool IRAM_ATTR idle_hook_cpu1(void)
{
    uint32_t current_tick = s_tick_count[1];
    if (current_tick != s_last_tick_cpu1) {
        s_last_tick_cpu1 = current_tick;
        s_idle_count[1]++;
    }
    return false;
}

void tic_cpu_init(void)
{
    esp_register_freertos_tick_hook_for_cpu(tick_hook_cpu0, 0);
    esp_register_freertos_tick_hook_for_cpu(tick_hook_cpu1, 1);
    esp_register_freertos_idle_hook_for_cpu(idle_hook_cpu0, 0);
    esp_register_freertos_idle_hook_for_cpu(idle_hook_cpu1, 1);
}

void tic_cpu_get_stats(tic_cpu_stats_t *stats, bool reset)
{
    // Read idle first, then tick - ensures idle <= tick even if tick occurs between reads
    uint32_t i0 = s_idle_count[0];
    uint32_t i1 = s_idle_count[1];
    uint32_t t0 = s_tick_count[0];
    uint32_t t1 = s_tick_count[1];

    // Utilization = 100 - idle_percent
    float idle0 = (t0 > 0) ? (100.0f * i0 / t0) : 100.0f;
    float idle1 = (t1 > 0) ? (100.0f * i1 / t1) : 100.0f;
    stats->util_pct_cpu0 = 100.0f - idle0;
    stats->util_pct_cpu1 = 100.0f - idle1;

    if (reset) {
        s_tick_count[0] = 0;
        s_tick_count[1] = 0;
        s_idle_count[0] = 0;
        s_idle_count[1] = 0;
        s_last_tick_cpu0 = 0;
        s_last_tick_cpu1 = 0;
    }
}
