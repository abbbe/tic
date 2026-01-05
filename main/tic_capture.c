#include "tic_capture.h"
#include "driver/mcpwm_cap.h"
#include "esp_log.h"
#include "esp_attr.h"
#include <string.h>

static const char *TAG = "tic_capture";

// Buffer configuration - will be set at init
#define MAX_BUFFER_SIZE 8192  // Maximum events per buffer
static size_t s_edges_per_buffer = 1000;  // Default, configurable

// Double buffer
static tic_event_t s_buffer_a[MAX_BUFFER_SIZE];
static tic_event_t s_buffer_b[MAX_BUFFER_SIZE];
static tic_event_t *s_active_buffer = s_buffer_a;
static tic_event_t *s_ready_buffer = s_buffer_b;
static volatile size_t s_active_count = 0;
static size_t s_ready_count = 0;

// MCPWM handles
static mcpwm_cap_timer_handle_t s_cap_timer = NULL;
static mcpwm_cap_channel_handle_t s_cap_channel = NULL;

// Timer resolution
static uint32_t s_resolution_hz = 0;

// Event group for signaling buffer ready
static EventGroupHandle_t s_event_group = NULL;

// Spinlock for protecting buffer swap from both ISR and task context
static portMUX_TYPE spinlock = portMUX_INITIALIZER_UNLOCKED;

// Capture callback - runs in ISR context
static bool IRAM_ATTR capture_callback(mcpwm_cap_channel_handle_t cap_chan,
                                        const mcpwm_capture_event_data_t *edata,
                                        void *user_ctx)
{
    BaseType_t high_task_woken = pdFALSE;

    // Only process rising edges
    if (edata->cap_edge != MCPWM_CAP_EDGE_POS) {
        return false;
    }

    portENTER_CRITICAL_ISR(&spinlock);

    size_t count = s_active_count;

    // Store edge event
    if (count < MAX_BUFFER_SIZE) {
        tic_event_t *event = &s_active_buffer[count];
        event->type = TIC_EVENT_EDGE;
        event->reserved = 0;
        event->reserved2 = 0;
        event->value = edata->cap_value;
        s_active_count = count + 1;
    }

    // Check if we've reached the threshold for buffer swap
    if (s_active_count >= s_edges_per_buffer) {
        // Swap buffers
        tic_event_t *old_active = s_active_buffer;
        size_t old_count = s_active_count;

        // Switch to the other buffer
        if (s_active_buffer == s_buffer_a) {
            s_active_buffer = s_buffer_b;
        } else {
            s_active_buffer = s_buffer_a;
        }
        s_active_count = 0;

        // The old active buffer is now the ready buffer
        s_ready_buffer = old_active;
        s_ready_count = old_count;

        portEXIT_CRITICAL_ISR(&spinlock);

        // Signal main loop that buffer is ready
        if (s_event_group) {
            xEventGroupSetBitsFromISR(s_event_group, TIC_BUFFER_READY_BIT, &high_task_woken);
        }
    } else {
        portEXIT_CRITICAL_ISR(&spinlock);
    }

    return high_task_woken == pdTRUE;
}

esp_err_t tic_capture_init(int gpio_num, bool loopback, size_t edges_per_buffer)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "Initializing capture on GPIO %d (loopback=%d, edges_per_buffer=%zu)",
             gpio_num, loopback, edges_per_buffer);

    // Validate and set edges per buffer
    if (edges_per_buffer == 0 || edges_per_buffer > MAX_BUFFER_SIZE) {
        ESP_LOGE(TAG, "Invalid edges_per_buffer: %zu (max: %d)", edges_per_buffer, MAX_BUFFER_SIZE);
        return ESP_ERR_INVALID_ARG;
    }
    s_edges_per_buffer = edges_per_buffer;

    // Create event group
    s_event_group = xEventGroupCreate();
    if (!s_event_group) {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_ERR_NO_MEM;
    }

    // Create capture timer with maximum resolution
    mcpwm_capture_timer_config_t cap_timer_config = {
        .group_id = 0,
        .clk_src = MCPWM_CAPTURE_CLK_SRC_DEFAULT,
    };
    ret = mcpwm_new_capture_timer(&cap_timer_config, &s_cap_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create capture timer: %s", esp_err_to_name(ret));
        return ret;
    }

    // Get actual resolution
    ret = mcpwm_capture_timer_get_resolution(s_cap_timer, &s_resolution_hz);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get timer resolution: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Capture timer resolution: %lu Hz (%.2f ns/tick)",
             (unsigned long)s_resolution_hz, 1e9 / s_resolution_hz);

    // Create capture channel
    mcpwm_capture_channel_config_t cap_ch_config = {
        .gpio_num = gpio_num,
        .prescale = 1,
        .flags.pos_edge = true,      // Capture on rising edge
        .flags.neg_edge = false,
        .flags.pull_up = false,
        .flags.pull_down = false,
        .flags.io_loop_back = loopback,
    };
    ret = mcpwm_new_capture_channel(s_cap_timer, &cap_ch_config, &s_cap_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create capture channel: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register capture callback
    mcpwm_capture_event_callbacks_t cbs = {
        .on_cap = capture_callback,
    };
    ret = mcpwm_capture_channel_register_event_callbacks(s_cap_channel, &cbs, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register callbacks: %s", esp_err_to_name(ret));
        return ret;
    }

    // Enable capture channel
    ret = mcpwm_capture_channel_enable(s_cap_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable capture channel: %s", esp_err_to_name(ret));
        return ret;
    }

    // Enable capture timer
    ret = mcpwm_capture_timer_enable(s_cap_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable capture timer: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Capture module initialized");

    return ESP_OK;
}

esp_err_t tic_capture_start(void)
{
    if (!s_cap_timer) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Starting capture");

    // Clear buffers and event group
    s_active_count = 0;
    s_ready_count = 0;
    xEventGroupClearBits(s_event_group, TIC_BUFFER_READY_BIT);

    return mcpwm_capture_timer_start(s_cap_timer);
}

esp_err_t tic_capture_stop(void)
{
    if (!s_cap_timer) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Stopping capture");
    return mcpwm_capture_timer_stop(s_cap_timer);
}

EventGroupHandle_t tic_capture_get_event_group(void)
{
    return s_event_group;
}

tic_event_t* tic_capture_get_ready_buffer(size_t *count)
{
    *count = s_ready_count;
    return s_ready_buffer;
}

uint32_t tic_capture_get_resolution(void)
{
    return s_resolution_hz;
}

void tic_capture_force_swap(void)
{
    // Disable interrupts to safely swap buffers
    portENTER_CRITICAL(&spinlock);

    // Only swap if there's anything in the active buffer or if ready buffer is empty
    if (s_active_count > 0 || s_ready_count == 0) {
        // Swap buffers
        tic_event_t *old_active = s_active_buffer;
        size_t old_count = s_active_count;

        // Switch to the other buffer
        if (s_active_buffer == s_buffer_a) {
            s_active_buffer = s_buffer_b;
        } else {
            s_active_buffer = s_buffer_a;
        }
        s_active_count = 0;

        // The old active buffer is now the ready buffer
        s_ready_buffer = old_active;
        s_ready_count = old_count;
    }

    portEXIT_CRITICAL(&spinlock);

    // Signal main loop that buffer is ready
    if (s_event_group) {
        xEventGroupSetBits(s_event_group, TIC_BUFFER_READY_BIT);
    }
}
