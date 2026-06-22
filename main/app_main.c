#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"

#include "app_config.h"
#include "app_status.h"
#include "esp_status_report.h"
#include "network.h"
#include "stm32_protocol.h"
#include "uart_bridge.h"
#include "usb_transport.h"

static const char *TAG = APP_TAG;

/**
 * @brief 处理 UART 接收到的一行 STM32 文本。
 *
 * 根据 USB 会话状态选择上行通道：
 *   - USB 会话激活（GUI 已发 GUI_USB_START 且未超时）→ 走 USB 直连
 *   - USB 会话未激活 → 走 MQTT broker
 *
 * 协议解析（CSV→JSON）在此处统一完成，两种通道共用同一份 JSON。
 *
 * @param line  UART 模块按换行符切分出的单行文本。
 * @param rx_ms 该行被接收时的系统毫秒时间戳。
 * @param ctx   回调上下文，当前未使用。
 */
static void handle_uart_line(const char *line, uint32_t rx_ms, void *ctx)
{
    char *json_payload;
    stm32_protocol_result_t protocol_result = STM32_PROTOCOL_RESULT_NONE;

    (void)ctx;

    if (line == NULL || line[0] == '\0') {
        return;
    }

    json_payload = stm32_protocol_build_publish_json(line, rx_ms, &protocol_result);
    app_status_note_protocol_result(protocol_result);
    if (json_payload == NULL) {
        ESP_LOGW(TAG, "Failed to build JSON payload for UART line");
        app_status_note_forward_fail();
        return;
    }

    if (usb_transport_is_active()) {
        if (usb_transport_queue_json(json_payload)) {
            ESP_LOGI_VERBOSE(TAG, "UPLINK[USB] frame forwarded via USB, MQTT skipped");
        } else {
            ESP_LOGW(TAG, "UPLINK[USB] queue failed, fallback to MQTT");
            (void)network_publish_json_payload(json_payload);
            ESP_LOGI_VERBOSE(TAG, "UPLINK[MQTT] frame forwarded via MQTT fallback");
        }
    } else {
        (void)network_publish_json_payload(json_payload);
        ESP_LOGI_VERBOSE(TAG, "UPLINK[MQTT] frame forwarded via MQTT");
    }

    free(json_payload);
}

/**
 * @brief 正常桥接模式入口。
 *
 * 主流程依次初始化运行状态、网络和 UART 桥接模块。
 * Wi-Fi 就绪后启动 MQTT，再启动 UART 接收任务，使 STM32 串口数据可以发布到 MQTT。
 */
void app_main(void)
{
    ESP_LOGI(TAG,
             "Bridge config: uart_port=%d tx=%d rx=%d baud=%d mqtt_broker=%s pub_topic=%s status_topic=%s cmd_topic=%s",
             (int)UART_PORT,
             UART_TX_PIN,
             UART_RX_PIN,
             UART_BAUD,
             MQTT_BROKER_URI,
             MQTT_PUBLISH_TOPIC,
             MQTT_STATUS_TOPIC,
             MQTT_COMMAND_TOPIC);

    if (app_status_init() != ESP_OK) {
        ESP_LOGW(TAG, "Runtime status init failed");
    } else if (app_status_start_task() != ESP_OK) {
        ESP_LOGW(TAG, "Runtime status task start failed");
    } else {
        app_status_log_snapshot("boot");
    }

    network_init();

    ESP_ERROR_CHECK(uart_bridge_init());
    if (usb_transport_init() != ESP_OK) {
        ESP_LOGW(TAG, "USB transport unavailable, MQTT fallback remains active");
    }

    if (uart_bridge_start_tx_task() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create UART TX task");
        abort();
    }

    if (usb_transport_start_tx_task() != ESP_OK) {
        ESP_LOGW(TAG, "Failed to create USB TX task, MQTT fallback remains active");
    }

    if (usb_transport_start_rx_task() != ESP_OK) {
        ESP_LOGW(TAG, "Failed to create USB RX task, USB commands will be unavailable");
    }

    if (uart_bridge_start_rx_task(handle_uart_line, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create UART RX task");
        abort();
    }

    app_status_log_snapshot("local_io_ready");
    if (esp_status_report_start_task() != ESP_OK) {
        ESP_LOGW(TAG, "ESP status report task start failed");
    } else {
        esp_status_report_request();
    }

    if (!network_wait_for_wifi(WIFI_WAIT_BEFORE_MQTT_MS)) {
        ESP_LOGW(TAG, "Starting MQTT before WiFi is fully ready");
    }
    network_start_mqtt();

    app_status_log_snapshot("app_ready");
    esp_status_report_request();
}
