#include "tic_wifi.h"
#include "tic_serial.h"  // For frame header format
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_crc.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include <string.h>

static const char *TAG = "tic_wifi";

// Connection state
static bool s_wifi_connected = false;
static bool s_mqtt_connected = false;
static esp_mqtt_client_handle_t s_mqtt_client = NULL;

// Frame sequence number (separate from USB CDC)
static uint32_t s_seq = 0;

// MQTT topic (built from config)
static char s_topic[64];

// Ring buffer for frames during WiFi/MQTT outage
#define RING_SIZE CONFIG_TIC_WIFI_RING_SIZE
// Max ~2100 pairs per frame (2kHz * 1s + headroom) = 16800 bytes pairs + 74 header + 4 CRC ≈ 17KB
#define MAX_PAIRS_PER_FRAME 2100
#define MAX_FRAME_SIZE (sizeof(tic_frame_header_t) + MAX_PAIRS_PER_FRAME * sizeof(tic_matched_pair_t) + 4)

typedef struct {
    uint8_t *data;
    size_t len;
    bool valid;
} frame_slot_t;

static frame_slot_t s_ring[RING_SIZE];
static int s_ring_head = 0;  // Next slot to write
static int s_ring_tail = 0;  // Next slot to send
static uint32_t s_frames_dropped = 0;
static SemaphoreHandle_t s_ring_mutex = NULL;

// Forward declarations
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data);
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data);
static void flush_ring_buffer(void);
static bool ring_add_frame(const uint8_t *data, size_t len);

esp_err_t tic_wifi_init(void)
{
    ESP_LOGI(TAG, "Initializing WiFi MQTT transport");
    ESP_LOGI(TAG, "  SSID: %s", CONFIG_TIC_WIFI_SSID);
    ESP_LOGI(TAG, "  Broker: %s:%d", CONFIG_TIC_MQTT_BROKER, CONFIG_TIC_MQTT_PORT);
    ESP_LOGI(TAG, "  Device: %s", CONFIG_TIC_DEVICE_NAME);

    // Build MQTT topic
    snprintf(s_topic, sizeof(s_topic), "%s/%s/frames",
             CONFIG_TIC_MQTT_TOPIC, CONFIG_TIC_DEVICE_NAME);
    ESP_LOGI(TAG, "  Topic: %s", s_topic);

    // Initialize ring buffer
    s_ring_mutex = xSemaphoreCreateMutex();
    if (!s_ring_mutex) {
        ESP_LOGE(TAG, "Failed to create ring buffer mutex");
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < RING_SIZE; i++) {
        s_ring[i].data = malloc(MAX_FRAME_SIZE);
        if (!s_ring[i].data) {
            ESP_LOGE(TAG, "Failed to allocate ring buffer slot %d", i);
            return ESP_ERR_NO_MEM;
        }
        s_ring[i].len = 0;
        s_ring[i].valid = false;
    }
    ESP_LOGI(TAG, "Ring buffer: %d slots, %zu bytes each (~%dKB total)",
             RING_SIZE, MAX_FRAME_SIZE, (RING_SIZE * MAX_FRAME_SIZE) / 1024);

    // Initialize NVS (required for WiFi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    // Initialize WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));

    // Configure WiFi
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_TIC_WIFI_SSID,
            .password = CONFIG_TIC_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Disable WiFi power save for stable streaming
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "WiFi initialized, connecting...");
    return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                esp_wifi_connect();
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                ESP_LOGW(TAG, "WiFi disconnected, reconnecting...");
                s_wifi_connected = false;
                s_mqtt_connected = false;
                esp_wifi_connect();
                break;
            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "WiFi connected");
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_connected = true;

        // Start MQTT client
        if (!s_mqtt_client) {
            char uri[64];
            snprintf(uri, sizeof(uri), "mqtt://%s:%d",
                     CONFIG_TIC_MQTT_BROKER, CONFIG_TIC_MQTT_PORT);

            esp_mqtt_client_config_t mqtt_cfg = {
                .broker.address.uri = uri,
                .network.reconnect_timeout_ms = 1000,
                .network.timeout_ms = 30000,  // 30s network timeout
            };
            s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
            if (!s_mqtt_client) {
                ESP_LOGE(TAG, "Failed to init MQTT client (out of memory?)");
                return;
            }
            esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID,
                                           mqtt_event_handler, NULL);
            esp_mqtt_client_start(s_mqtt_client);
        }
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected");
            s_mqtt_connected = true;
            // Flush any buffered frames
            flush_ring_buffer();
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT disconnected");
            s_mqtt_connected = false;
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error");
            if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                ESP_LOGE(TAG, "  TCP error: %d", event->error_handle->esp_tls_last_esp_err);
            }
            break;
        default:
            break;
    }
}

bool tic_wifi_is_connected(void)
{
    return s_wifi_connected && s_mqtt_connected;
}

static bool ring_add_frame(const uint8_t *data, size_t len)
{
    if (xSemaphoreTake(s_ring_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return false;
    }

    // Check if ring is full
    int next_head = (s_ring_head + 1) % RING_SIZE;
    if (next_head == s_ring_tail && s_ring[s_ring_head].valid) {
        // Ring full, drop oldest frame
        s_ring_tail = (s_ring_tail + 1) % RING_SIZE;
        s_frames_dropped++;
    }

    // Copy frame to ring buffer
    memcpy(s_ring[s_ring_head].data, data, len);
    s_ring[s_ring_head].len = len;
    s_ring[s_ring_head].valid = true;
    s_ring_head = next_head;

    xSemaphoreGive(s_ring_mutex);
    return true;
}

static void flush_ring_buffer(void)
{
    if (!s_mqtt_connected || !s_mqtt_client) {
        return;
    }

    if (xSemaphoreTake(s_ring_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    int flushed = 0;
    while (s_ring[s_ring_tail].valid) {
        // Publish buffered frame with QoS 0 (fire-and-forget)
        int ret = esp_mqtt_client_publish(s_mqtt_client, s_topic,
                                          (const char *)s_ring[s_ring_tail].data,
                                          s_ring[s_ring_tail].len, 0, 0);
        if (ret < 0) {
            ESP_LOGW(TAG, "Failed to flush buffered frame");
            break;
        }

        s_ring[s_ring_tail].valid = false;
        s_ring_tail = (s_ring_tail + 1) % RING_SIZE;
        flushed++;
    }

    xSemaphoreGive(s_ring_mutex);

    if (flushed > 0) {
        ESP_LOGI(TAG, "Flushed %d buffered frames", flushed);
    }
}

void tic_wifi_send_frame(const tic_matched_pair_t *pairs, uint16_t pair_count,
                         const tic_stats_t *stats, const tic_cpu_stats_t *cpu_stats,
                         uint32_t resolution_hz, uint64_t base_ts, bool overflow)
{
    // Build frame (same format as USB CDC)
    static uint8_t frame_buf[MAX_FRAME_SIZE];

    tic_frame_header_t header = {
        .magic = TIC_FRAME_MAGIC,
        .seq = s_seq++,
        .resolution_hz = resolution_hz,
        .base_ts = base_ts,
        .pair_count = pair_count,
        .edges_a = (uint16_t)stats->ch_a.edge_count,
        .edges_b = (uint16_t)stats->ch_b.edge_count,
        .miss_a = (uint16_t)stats->delay.missed_a,
        .miss_b = (uint16_t)stats->delay.missed_b,
        .flags = overflow ? TIC_FRAME_FLAG_OVERFLOW : 0,
        .delay_mean_ns = (float)stats->delay.mean_ns,
        .delay_min_ns = (float)stats->delay.min_ns,
        .delay_max_ns = (float)stats->delay.max_ns,
        .delay_stddev_ns = (float)stats->delay.stddev_ns,
        .cpu0_pct = cpu_stats ? (uint8_t)cpu_stats->util_pct_cpu0 : 0,
        .cpu1_pct = cpu_stats ? (uint8_t)cpu_stats->util_pct_cpu1 : 0,
        .period_a_mean_ns = (uint32_t)stats->ch_a.mean_ns,
        .period_a_min_ns = (uint32_t)stats->ch_a.min_ns,
        .period_a_max_ns = (uint32_t)stats->ch_a.max_ns,
        .period_b_mean_ns = (uint32_t)stats->ch_b.mean_ns,
        .period_b_min_ns = (uint32_t)stats->ch_b.min_ns,
        .period_b_max_ns = (uint32_t)stats->ch_b.max_ns,
    };

    // Copy header
    memcpy(frame_buf, &header, sizeof(header));
    size_t frame_len = sizeof(header);

    // Copy pairs
    if (pair_count > 0) {
        size_t pairs_size = pair_count * sizeof(tic_matched_pair_t);
        memcpy(frame_buf + frame_len, pairs, pairs_size);
        frame_len += pairs_size;
    }

    // Calculate CRC32 over header + pairs
    uint32_t crc = esp_crc32_le(0, frame_buf, frame_len);

    // Append CRC
    memcpy(frame_buf + frame_len, &crc, sizeof(crc));
    frame_len += sizeof(crc);

    // Try to send directly if connected
    if (s_mqtt_connected && s_mqtt_client) {
        // First flush any buffered frames
        flush_ring_buffer();

        // Send current frame with QoS 0 (fire-and-forget, ring buffer handles reliability)
        int ret = esp_mqtt_client_publish(s_mqtt_client, s_topic,
                                          (const char *)frame_buf, frame_len, 0, 0);
        if (ret >= 0) {
            return;  // Success
        }
        ESP_LOGW(TAG, "MQTT publish failed, buffering frame");
    }

    // Not connected or send failed - buffer the frame
    if (!ring_add_frame(frame_buf, frame_len)) {
        ESP_LOGW(TAG, "Failed to buffer frame");
    }
}

void tic_wifi_get_buffer_stats(uint32_t *buffered, uint32_t *dropped)
{
    if (xSemaphoreTake(s_ring_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        int count = 0;
        for (int i = 0; i < RING_SIZE; i++) {
            if (s_ring[i].valid) count++;
        }
        if (buffered) *buffered = count;
        if (dropped) *dropped = s_frames_dropped;
        xSemaphoreGive(s_ring_mutex);
    }
}
