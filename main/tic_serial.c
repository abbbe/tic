#include "tic_serial.h"
#include "esp_log.h"
#include "esp_crc.h"
#include "tinyusb.h"
#include "tusb_cdc_acm.h"
#include "class/cdc/cdc_device.h"  // for tud_cdc_connected()
#include "sdkconfig.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "tic_serial";

// Frame sequence number
static uint32_t s_seq = 0;

// 64-bit timestamp extension
static uint32_t s_last_raw_ts = 0;
static uint64_t s_ts_high = 0;

// Extend 32-bit timestamp to 64-bit, handling overflow
static uint64_t extend_timestamp(uint32_t raw_ts)
{
    // Detect overflow: if new timestamp is much smaller than last
    if (raw_ts < s_last_raw_ts && (s_last_raw_ts - raw_ts) > 0x80000000) {
        s_ts_high += 0x100000000ULL;
    }
    s_last_raw_ts = raw_ts;
    return s_ts_high | raw_ts;
}

void tic_serial_init(void)
{
    ESP_LOGI(TAG, "Initializing USB CDC for binary output");

    // TinyUSB driver configuration
    const tinyusb_config_t tusb_cfg = {
        .port = TINYUSB_PORT_FULL_SPEED_0,
        .task = {
            .size = 4096,
            .priority = 5,
            .xCoreID = 0,
        },
    };

    esp_err_t ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install TinyUSB driver: %s", esp_err_to_name(ret));
        return;
    }

    // CDC ACM configuration
    tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = NULL,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL,
    };

    ret = tinyusb_cdcacm_init(&acm_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init CDC ACM: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "USB CDC initialized");
}

void tic_serial_send_frame(const tic_event_t *events, size_t event_count,
                           uint32_t resolution_hz, bool overflow)
{
    if (event_count == 0) {
        return;
    }

    // Don't send if host hasn't opened the CDC port
    if (!tud_cdc_connected()) {
        return;
    }

    // Find first edge timestamp for 64-bit extension
    uint64_t first_edge_ts = 0;
    for (size_t i = 0; i < event_count; i++) {
        if (events[i].type == TIC_EVENT_EDGE) {
            first_edge_ts = extend_timestamp(events[i].value);
            break;
        }
    }

    // Build frame header
    tic_frame_header_t header = {
        .magic = TIC_FRAME_MAGIC,
        .version = 1,
        .resolution_hz = resolution_hz,
        .seq = s_seq++,
        .first_edge_ts = first_edge_ts,
        .event_count = (uint16_t)event_count,
        .flags = overflow ? TIC_FRAME_FLAG_OVERFLOW : 0,
    };

    // Calculate CRC32 over header + events
    uint32_t crc = esp_crc32_le(0, (const uint8_t *)&header, sizeof(header));
    crc = esp_crc32_le(crc, (const uint8_t *)events, event_count * sizeof(tic_event_t));

    // Send header
    tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (uint8_t *)&header, sizeof(header));
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(100));

    // Send events in chunks to avoid overflowing the CDC TX FIFO
    const size_t CHUNK_SIZE = 64;  // Events per chunk (512 bytes)
    const uint8_t *event_data = (const uint8_t *)events;
    size_t total_bytes = event_count * sizeof(tic_event_t);
    size_t sent = 0;

    while (sent < total_bytes) {
        size_t chunk = total_bytes - sent;
        if (chunk > CHUNK_SIZE * sizeof(tic_event_t)) {
            chunk = CHUNK_SIZE * sizeof(tic_event_t);
        }

        tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, event_data + sent, chunk);
        tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(100));
        sent += chunk;
    }

    // Send CRC32 at end of frame
    tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (uint8_t *)&crc, sizeof(crc));
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(100));
}

void tic_serial_send_stats(const tic_stats_t *stats, const tic_cpu_stats_t *cpu_stats)
{
    // Don't send if host hasn't opened the CDC port
    if (!tud_cdc_connected()) {
        return;
    }

    // Send stats as JSON line for compatibility
    char buf[512];
    int len = snprintf(buf, sizeof(buf),
        "{\"type\":\"stats\","
        "\"a\":{\"n\":%lu,\"hz\":%.2f,\"min_us\":%.3f,\"avg_us\":%.3f,\"max_us\":%.3f,\"std_us\":%.3f},"
        "\"b\":{\"n\":%lu,\"hz\":%.2f,\"min_us\":%.3f,\"avg_us\":%.3f,\"max_us\":%.3f,\"std_us\":%.3f},"
        "\"d\":{\"n\":%lu,\"min_ns\":%.3f,\"avg_ns\":%.3f,\"max_ns\":%.3f,\"std_ns\":%.3f,"
        "\"miss_a\":%lu,\"miss_b\":%lu}",
        (unsigned long)stats->ch_a.count,
        stats->ch_a.count > 0 ? 1e9 / stats->ch_a.mean_ns : 0.0,
        stats->ch_a.min_ns / 1000.0,
        stats->ch_a.mean_ns / 1000.0,
        stats->ch_a.max_ns / 1000.0,
        stats->ch_a.stddev_ns / 1000.0,
        (unsigned long)stats->ch_b.count,
        stats->ch_b.count > 0 ? 1e9 / stats->ch_b.mean_ns : 0.0,
        stats->ch_b.min_ns / 1000.0,
        stats->ch_b.mean_ns / 1000.0,
        stats->ch_b.max_ns / 1000.0,
        stats->ch_b.stddev_ns / 1000.0,
        (unsigned long)stats->delay.count,
        stats->delay.min_ns,
        stats->delay.mean_ns,
        stats->delay.max_ns,
        stats->delay.stddev_ns,
        (unsigned long)stats->delay.missed_a,
        (unsigned long)stats->delay.missed_b);

    if (cpu_stats) {
        len += snprintf(buf + len, sizeof(buf) - len,
            ",\"cpu\":{\"cpu0\":%.1f,\"cpu1\":%.1f}",
            cpu_stats->util_pct_cpu0,
            cpu_stats->util_pct_cpu1);
    }

    len += snprintf(buf + len, sizeof(buf) - len, "}\n");

    tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (uint8_t *)buf, len);
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(10));
}
