#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "cJSON.h"

#include "app_config.h"
#include "app_status.h"
#include "esp_status_report.h"
#include "network.h"
#include "status_strings.h"
#include "usb_transport.h"

static const char *TAG = APP_TAG;

static TaskHandle_t s_status_report_task_handle = NULL;

static void esp_status_report_task(void *arg);
static cJSON *build_status_json(const app_status_public_snapshot_t *snapshot,
                                bool usb_connected,
                                bool usb_active,
                                uint32_t uptime_ms);
static cJSON *build_transport_section(const app_status_public_snapshot_t *snapshot,
                                      bool usb_connected, bool usb_active);
static cJSON *build_usb_section(bool usb_connected, bool usb_active);
static cJSON *build_wifi_section(const app_status_public_snapshot_t *snapshot);
static cJSON *build_mqtt_section(const app_status_public_snapshot_t *snapshot);
static cJSON *build_uart_section(const app_status_public_snapshot_t *snapshot);
static cJSON *build_stm32_section(const app_status_public_snapshot_t *snapshot);
static cJSON *build_counters_section(const app_status_public_snapshot_t *snapshot);
static const char *active_transport_string(const app_status_public_snapshot_t *snapshot,
                                           bool usb_connected,
                                           bool usb_active);

esp_err_t esp_status_report_start_task(void)
{
    if (s_status_report_task_handle != NULL) {
        return ESP_OK;
    }

    if (xTaskCreate(esp_status_report_task,
                    "esp_status_report",
                    ESP_STATUS_REPORT_TASK_STACK_SIZE,
                    NULL,
                    ESP_STATUS_REPORT_TASK_PRIORITY,
                    &s_status_report_task_handle) != pdPASS) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

void esp_status_report_request(void)
{
    if (s_status_report_task_handle != NULL) {
        xTaskNotifyGive(s_status_report_task_handle);
    }
}

void esp_status_report_publish_once(void)
{
    app_status_public_snapshot_t snapshot = {
        .last_frame_type = '-',
    };
    cJSON *root;
    char *json_payload;
    bool usb_connected;
    bool usb_active;
    uint32_t uptime_ms;

    app_status_read_snapshot(&snapshot);

    usb_connected = usb_transport_is_connected();
    usb_active = usb_transport_is_active();
    if (usb_active && !usb_connected) {
        usb_transport_deactivate();
        usb_active = false;
    }

    uptime_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    root = build_status_json(&snapshot, usb_connected, usb_active, uptime_ms);
    if (root == NULL) {
        ESP_LOGW(TAG, "Failed to build ESP status JSON");
        return;
    }

    json_payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json_payload == NULL) {
        ESP_LOGW(TAG, "Failed to serialize ESP status JSON");
        return;
    }

    if (usb_active) {
        (void)usb_transport_queue_json(json_payload);
    }
    (void)network_publish_status_json(json_payload);

    cJSON_free(json_payload);
}

static void esp_status_report_task(void *arg)
{
    (void)arg;

    while (1) {
        esp_status_report_publish_once();
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ESP_STATUS_REPORT_PERIOD_MS));
    }
}

static cJSON *build_status_json(const app_status_public_snapshot_t *snapshot,
                                bool usb_connected,
                                bool usb_active,
                                uint32_t uptime_ms)
{
    cJSON *root;

    root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    cJSON_AddStringToObject(root, "bridge", MQTT_BRIDGE_NAME);
    cJSON_AddStringToObject(root, "source", "esp32");
    cJSON_AddStringToObject(root, "channel", "status");
    cJSON_AddStringToObject(root, "protocol", "esp-status-v1");
    cJSON_AddNumberToObject(root, "schema_version", 1);
    cJSON_AddStringToObject(root, "message", "esp_status");
    cJSON_AddBoolToObject(root, "online", true);
    cJSON_AddNumberToObject(root, "uptime_ms", (double)uptime_ms);

    cJSON_AddItemToObject(root, "transport",
        build_transport_section(snapshot, usb_connected, usb_active));
    cJSON_AddItemToObject(root, "usb",
        build_usb_section(usb_connected, usb_active));
    cJSON_AddItemToObject(root, "wifi",
        build_wifi_section(snapshot));
    cJSON_AddItemToObject(root, "mqtt",
        build_mqtt_section(snapshot));
    cJSON_AddItemToObject(root, "uart",
        build_uart_section(snapshot));
    cJSON_AddItemToObject(root, "stm32",
        build_stm32_section(snapshot));
    cJSON_AddItemToObject(root, "counters",
        build_counters_section(snapshot));

    return root;
}

static cJSON *build_transport_section(const app_status_public_snapshot_t *snapshot,
                                      bool usb_connected, bool usb_active)
{
    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) return NULL;

    cJSON_AddStringToObject(obj, "active",
        active_transport_string(snapshot, usb_connected, usb_active));
    cJSON_AddBoolToObject(obj, "usb_connected", usb_connected);
    cJSON_AddBoolToObject(obj, "usb_active", usb_active);
    cJSON_AddBoolToObject(obj, "mqtt_connected", snapshot->mqtt_connected);
    return obj;
}

static cJSON *build_usb_section(bool usb_connected, bool usb_active)
{
    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) return NULL;

    cJSON_AddBoolToObject(obj, "connected", usb_connected);
    cJSON_AddBoolToObject(obj, "active", usb_active);
    cJSON_AddNumberToObject(obj, "session_timeout_ms", USB_SESSION_TIMEOUT_MS);
    return obj;
}

static cJSON *build_wifi_section(const app_status_public_snapshot_t *snapshot)
{
    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) return NULL;

    cJSON_AddBoolToObject(obj, "started", snapshot->wifi_started);
    cJSON_AddBoolToObject(obj, "connected", snapshot->wifi_connected);
    cJSON_AddStringToObject(obj, "state", WIFI_STATE_STR(snapshot));
    return obj;
}

static cJSON *build_mqtt_section(const app_status_public_snapshot_t *snapshot)
{
    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) return NULL;

    cJSON_AddBoolToObject(obj, "started", snapshot->mqtt_started);
    cJSON_AddBoolToObject(obj, "connected", snapshot->mqtt_connected);
    cJSON_AddBoolToObject(obj, "subscribed", snapshot->mqtt_subscribed);
    cJSON_AddStringToObject(obj, "state", MQTT_STATE_STR(snapshot));
    cJSON_AddStringToObject(obj, "status_topic", MQTT_STATUS_TOPIC);
    cJSON_AddStringToObject(obj, "data_topic", MQTT_PUBLISH_TOPIC);
    cJSON_AddStringToObject(obj, "command_topic", MQTT_COMMAND_TOPIC);
    return obj;
}

static cJSON *build_uart_section(const app_status_public_snapshot_t *snapshot)
{
    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) return NULL;

    cJSON_AddBoolToObject(obj, "ready", snapshot->uart_ready);
    cJSON_AddBoolToObject(obj, "tx_task", snapshot->uart_tx_task_running);
    cJSON_AddBoolToObject(obj, "rx_task", snapshot->uart_rx_task_running);
    cJSON_AddStringToObject(obj, "state", UART_STATE_STR(snapshot));
    return obj;
}

static cJSON *build_stm32_section(const app_status_public_snapshot_t *snapshot)
{
    cJSON *obj = cJSON_CreateObject();
    char last_frame_buf[2];
    if (obj == NULL) return NULL;

    cJSON_AddStringToObject(obj, "protocol_state", PROTOCOL_STATE_STR(snapshot));
    last_frame_buf[0] = snapshot->last_frame_type != '\0' ? snapshot->last_frame_type : '-';
    last_frame_buf[1] = '\0';
    cJSON_AddStringToObject(obj, "last_frame", last_frame_buf);
    cJSON_AddNumberToObject(obj, "last_frame_ms", (double)snapshot->last_frame_ms);
    return obj;
}

static cJSON *build_counters_section(const app_status_public_snapshot_t *snapshot)
{
    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) return NULL;

    cJSON_AddNumberToObject(obj, "uart_rx_lines",       (double)snapshot->uart_rx_lines);
    cJSON_AddNumberToObject(obj, "uart_rx_drops",       (double)snapshot->uart_rx_drops);
    cJSON_AddNumberToObject(obj, "uart_rx_overflows",   (double)snapshot->uart_rx_overflows);
    cJSON_AddNumberToObject(obj, "uart_tx_queued",      (double)snapshot->uart_tx_queued);
    cJSON_AddNumberToObject(obj, "uart_tx_ok",          (double)snapshot->uart_tx_ok);
    cJSON_AddNumberToObject(obj, "uart_tx_fail",        (double)snapshot->uart_tx_fail);
    cJSON_AddNumberToObject(obj, "mqtt_publish_ok",     (double)snapshot->mqtt_publish_ok);
    cJSON_AddNumberToObject(obj, "mqtt_publish_fail",   (double)snapshot->mqtt_publish_fail);
    cJSON_AddNumberToObject(obj, "mqtt_command_count",  (double)snapshot->mqtt_command_count);
    cJSON_AddNumberToObject(obj, "protocol_ok",         (double)snapshot->protocol_ok);
    cJSON_AddNumberToObject(obj, "protocol_error",      (double)snapshot->protocol_error);
    cJSON_AddNumberToObject(obj, "forward_fail",        (double)snapshot->forward_fail);
    return obj;
}

static const char *active_transport_string(const app_status_public_snapshot_t *snapshot,
                                           bool usb_connected,
                                           bool usb_active)
{
    if (usb_active) {
        return "usb";
    }

    if (snapshot->mqtt_connected) {
        return "mqtt";
    }

    if (usb_connected) {
        return "usb_idle";
    }

    return "offline";
}
