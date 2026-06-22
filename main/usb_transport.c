#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "driver/usb_serial_jtag.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "app_config.h"
#include "esp_status_report.h"
#include "line_reader.h"
#include "uart_bridge.h"
#include "usb_transport.h"

static const char *TAG = APP_TAG;

/* ---- USB 会话控制命令前缀 ---- */
#define GUI_USB_START  "GUI_USB_START"
#define GUI_USB_STOP   "GUI_USB_STOP"
#define GUI_USB_PING   "GUI_USB_PING"

/* ---- 发送队列消息：动态分配的 JSON 字符串 ---- */
typedef struct {
    char *data;
    size_t len;
} usb_tx_message_t;

/* ---- 模块内部状态 ---- */
static QueueHandle_t s_usb_tx_queue = NULL;
static TaskHandle_t s_usb_tx_task_handle = NULL;
static TaskHandle_t s_usb_rx_task_handle = NULL;

/** @brief USB 会话是否激活（GUI 已发 START 且未超时/STOP） */
static bool s_usb_active = false;

/** @brief 上次收到活动信号的时间戳（微秒），用于超时检测 */
static int64_t s_last_activity_us = 0;

/** @brief 保护 s_usb_active 和 s_last_activity_us 的自旋锁 */
static portMUX_TYPE s_session_lock = portMUX_INITIALIZER_UNLOCKED;

/* ---- 内部函数声明 ---- */
static void usb_rx_line_cb(const char *line, void *ctx);
static void usb_tx_task(void *arg);
static void usb_rx_task(void *arg);
static bool usb_write_all(const char *data, size_t len);
static void usb_session_activate(void);
static void usb_session_renew(void);
static bool usb_session_is_timed_out(void);

/* ==========================================================================
 * 公共接口
 * ========================================================================== */

esp_err_t usb_transport_init(void)
{
#if USB_TRANSPORT_ENABLED
    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = USB_TRANSPORT_TX_BUF_SIZE,
        .rx_buffer_size = USB_TRANSPORT_RX_BUF_SIZE,
    };
    esp_err_t err;

    if (!usb_serial_jtag_is_driver_installed()) {
        err = usb_serial_jtag_driver_install(&cfg);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "USB Serial/JTAG driver install failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    if (s_usb_tx_queue == NULL) {
        s_usb_tx_queue = xQueueCreate(USB_TRANSPORT_TX_QUEUE_LENGTH, sizeof(usb_tx_message_t));
        if (s_usb_tx_queue == NULL) {
            ESP_LOGE(TAG, "Failed to create USB TX queue");
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(TAG,
             "USB transport ready: connected=%s tx_buf=%d rx_buf=%d queue_len=%d timeout=%dms",
             usb_transport_is_connected() ? "yes" : "no",
             USB_TRANSPORT_TX_BUF_SIZE,
             USB_TRANSPORT_RX_BUF_SIZE,
             USB_TRANSPORT_TX_QUEUE_LENGTH,
             USB_SESSION_TIMEOUT_MS);
    return ESP_OK;
#else
    return ESP_OK;
#endif
}

esp_err_t usb_transport_start_tx_task(void)
{
#if USB_TRANSPORT_ENABLED
    if (s_usb_tx_task_handle != NULL) {
        return ESP_OK;
    }

    if (xTaskCreate(usb_tx_task,
                    "usb_tx_task",
                    USB_TRANSPORT_TASK_STACK_SIZE,
                    NULL,
                    USB_TRANSPORT_TASK_PRIORITY,
                    &s_usb_tx_task_handle) != pdPASS) {
        return ESP_FAIL;
    }
#endif
    return ESP_OK;
}

esp_err_t usb_transport_start_rx_task(void)
{
#if USB_TRANSPORT_ENABLED
    if (s_usb_rx_task_handle != NULL) {
        return ESP_OK;
    }

    if (xTaskCreate(usb_rx_task,
                    "usb_rx_task",
                    USB_TRANSPORT_TASK_STACK_SIZE,
                    NULL,
                    USB_TRANSPORT_TASK_PRIORITY,
                    &s_usb_rx_task_handle) != pdPASS) {
        return ESP_FAIL;
    }
#endif
    return ESP_OK;
}

bool usb_transport_is_connected(void)
{
#if USB_TRANSPORT_ENABLED
    return usb_serial_jtag_is_driver_installed() && usb_serial_jtag_is_connected();
#else
    return false;
#endif
}

bool usb_transport_is_active(void)
{
    bool active = false;
#if USB_TRANSPORT_ENABLED
    bool timed_out = false;
    portENTER_CRITICAL(&s_session_lock);
    if (s_usb_active && usb_session_is_timed_out()) {
        s_usb_active = false;
        timed_out = true;
    }
    active = s_usb_active;
    portEXIT_CRITICAL(&s_session_lock);
    if (timed_out) {
        ESP_LOGI(TAG, "USB session timed out (%d ms), switching to MQTT",
                 USB_SESSION_TIMEOUT_MS);
        esp_status_report_request();
    }
#else
    (void)active;
#endif
    return active;
}

void usb_transport_deactivate(void)
{
#if USB_TRANSPORT_ENABLED
    bool was_active = false;
    portENTER_CRITICAL(&s_session_lock);
    if (s_usb_active) {
        s_usb_active = false;
        was_active = true;
    }
    portEXIT_CRITICAL(&s_session_lock);
    if (was_active) {
        ESP_LOGI(TAG, "USB session deactivated, uplink now MQTT");
        esp_status_report_request();
    }
#endif
}

bool usb_transport_queue_json(const char *json_payload)
{
#if USB_TRANSPORT_ENABLED
    usb_tx_message_t msg = {0};
    size_t payload_len;

    if (json_payload == NULL || json_payload[0] == '\0') {
        return false;
    }

    if (!usb_transport_is_active()) {
        return false;
    }

    if (!usb_transport_is_connected()) {
        ESP_LOGW(TAG, "USB cable disconnected during active session, deactivating");
        usb_transport_deactivate();
        return false;
    }

    if (s_usb_tx_queue == NULL) {
        ESP_LOGW(TAG, "USB TX queue not ready");
        return false;
    }

    payload_len = strlen(json_payload);
    msg.data = malloc(payload_len + 2U);
    if (msg.data == NULL) {
        ESP_LOGW(TAG, "No memory for USB JSON payload copy");
        return false;
    }

    memcpy(msg.data, json_payload, payload_len);
    msg.data[payload_len] = '\n';
    msg.data[payload_len + 1U] = '\0';
    msg.len = payload_len + 1U;

    if (xQueueSend(s_usb_tx_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "USB TX queue full, fallback to MQTT");
        free(msg.data);
        return false;
    }

    ESP_LOGI_VERBOSE(TAG, "USB queued JSON (%u bytes)", (unsigned int)msg.len);
    return true;
#else
    (void)json_payload;
    return false;
#endif
}

/* ==========================================================================
 * 会话状态管理（内部函数）
 * ========================================================================== */

/**
 * @brief 激活 USB 会话。
 *
 * 由 GUI_USB_START 命令触发。如果已经在 active 状态，仅续期。
 * 日志会明确输出状态转换。
 */
static void usb_session_activate(void)
{
    bool just_activated = false;
    portENTER_CRITICAL(&s_session_lock);
    if (!s_usb_active) {
        s_usb_active = true;
        just_activated = true;
    }
    s_last_activity_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_session_lock);
    if (just_activated) {
        ESP_LOGI(TAG, "USB session ACTIVATED by GUI, uplink switching to USB");
        esp_status_report_request();
    }
}

/**
 * @brief 续期 USB 会话（由 PING 或重复 START 调用）。
 *
 * 仅更新最后活动时间戳，不输出日志（避免 PING 刷屏）。
 */
static void usb_session_renew(void)
{
    portENTER_CRITICAL(&s_session_lock);
    s_last_activity_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_session_lock);
}

/**
 * @brief 检查 USB 会话是否已超时。
 *
 * 调用方必须已持有 s_session_lock 或在 is_active() 中调用
 * （is_active 内部已加锁，此函数被其调用时锁已持有）。
 *
 * @return true 已超时应停用，false 仍在有效期内。
 */
static bool usb_session_is_timed_out(void)
{
    int64_t elapsed_ms;

    if (!s_usb_active) {
        return false;
    }

    if (s_last_activity_us == 0) {
        return false;
    }

    elapsed_ms = (esp_timer_get_time() - s_last_activity_us) / 1000;
    return elapsed_ms >= USB_SESSION_TIMEOUT_MS;
}

/* ==========================================================================
 * 内部任务
 * ========================================================================== */

/**
 * @brief USB 发送任务。
 *
 * 从队列取出 JSON 消息，分片写入 USB Serial/JTAG，等待发送完成。
 * 发送期间若 USB 断开则丢弃当前消息并停用会话。
 */
static void usb_tx_task(void *arg)
{
    usb_tx_message_t msg;

    (void)arg;

    while (1) {
        if (xQueueReceive(s_usb_tx_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (!usb_transport_is_connected()) {
            ESP_LOGW(TAG, "USB disconnected before TX, drop queued JSON");
            usb_transport_deactivate();
            free(msg.data);
            continue;
        }

        if (!usb_write_all(msg.data, msg.len)) {
            ESP_LOGW(TAG, "USB TX failed, deactivating session, JSON not delivered");
            usb_transport_deactivate();
            free(msg.data);
            continue;
        }

        if (usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(USB_TRANSPORT_TX_DONE_TIMEOUT_MS)) != ESP_OK) {
            ESP_LOGW(TAG, "USB TX flush timeout, deactivating session, JSON not delivered");
            usb_transport_deactivate();
            free(msg.data);
            continue;
        }

        ESP_LOGI_VERBOSE(TAG, "USB TX done (%u bytes)", (unsigned int)msg.len);
        free(msg.data);
    }
}

/**
 * @brief line_reader callback: process a complete USB line.
 *
 * Handles USB session control commands (START/STOP/PING) locally;
 * forwards all other commands to uart_bridge_queue_command().
 */
static void usb_rx_line_cb(const char *line, void *ctx)
{
    (void)ctx;

    if (line[0] == '\0') {
        return;
    }

    if (strcmp(line, GUI_USB_START) == 0) {
        usb_session_activate();
    } else if (strcmp(line, GUI_USB_STOP) == 0) {
        usb_transport_deactivate();
    } else if (strcmp(line, GUI_USB_PING) == 0) {
        usb_session_renew();
    } else {
        ESP_LOGI(TAG, "USB->UART command (%u bytes): %s",
                 (unsigned int)strlen(line), line);
        uart_bridge_queue_command(line, (int)strlen(line));
    }
}

/**
 * @brief USB 接收任务。
 *
 * 持续读取 USB Serial/JTAG 字节流，由 line_reader 按换行切分为完整命令，
 * 经 usb_rx_line_cb 分发到会话控制或 UART 转发。
 */
static void usb_rx_task(void *arg)
{
    uint8_t buf[64];
    char line_buf[USB_TRANSPORT_LINE_BUF_SIZE];
    line_reader_t reader;

    (void)arg;

    line_reader_init(&reader, line_buf, USB_TRANSPORT_LINE_BUF_SIZE, usb_rx_line_cb, NULL);

    while (1) {
        int len = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(100));

        if (len <= 0) {
            continue;
        }

        for (int i = 0; i < len; i++) {
            if (line_reader_feed(&reader, buf[i]) == LINE_READER_EVENT_OVERFLOW) {
                ESP_LOGW(TAG,
                         "USB RX line too long (%d max), discarding until newline",
                         USB_TRANSPORT_LINE_BUF_SIZE);
            }
        }
    }
}

/* ==========================================================================
 * 硬件写入辅助
 * ========================================================================== */

/**
 * @brief 将数据分片写入 USB Serial/JTAG。
 *
 * 每次最多写 128 字节，循环直到全部写完或出错/断开。
 *
 * @param data 数据指针。
 * @param len  数据长度。
 * @return true 全部写入成功，false 写入失败或 USB 断开。
 */
static bool usb_write_all(const char *data, size_t len)
{
    size_t written = 0;

    while (written < len) {
        size_t chunk = len - written;
        int sent;

        if (chunk > 128U) {
            chunk = 128U;
        }

        if (!usb_transport_is_connected()) {
            return false;
        }

        sent = usb_serial_jtag_write_bytes(&data[written],
                                           chunk,
                                           pdMS_TO_TICKS(USB_TRANSPORT_WRITE_TIMEOUT_MS));
        if (sent <= 0) {
            return false;
        }

        written += (size_t)sent;
    }

    return true;
}
