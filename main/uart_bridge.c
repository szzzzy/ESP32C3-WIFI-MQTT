#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "driver/uart.h"
#include "esp_timer.h"

#include "esp_err.h"
#include "esp_log.h"

#include "app_config.h"
#include "app_status.h"
#include "line_reader.h"
#include "uart_bridge.h"

static const char *TAG = APP_TAG;

/**
 * @brief UART 发送队列消息结构体。
 *
 * 每条消息封装了待发送的 MQTT 命令内容、长度，以及是否需要尾部补换行的标志。
 * 消息在 uart_bridge_queue_command() 中填充，在 uart_tx_task() 中消费。
 */
typedef struct {
    size_t len;            /**< 有效数据长度（不含结尾 '\0'） */
    bool append_newline;   /**< 写入数据后是否需要追加一个 '\n' */
    char data[UART_BUF_SIZE]; /**< 命令内容缓冲区 */
} uart_tx_message_t;

/** @brief UART 发送队列句柄，承载 MQTT→UART 的下行命令 */
static QueueHandle_t s_uart_tx_queue = NULL;
/** @brief UART 发送任务句柄，用于判断任务是否已创建 */
static TaskHandle_t s_uart_tx_task_handle = NULL;
/** @brief UART 接收任务句柄，用于判断任务是否已创建 */
static TaskHandle_t s_uart_rx_task_handle = NULL;
/** @brief UART 接收行回调函数指针，由 uart_bridge_start_rx_task() 设置 */
static uart_bridge_line_handler_t s_line_handler = NULL;
/** @brief 接收行回调的透传上下文，由 uart_bridge_start_rx_task() 保存 */
static void *s_line_handler_ctx = NULL;

/* ---- 内部任务入口前置声明 ---- */
static void uart_rx_line_cb(const char *line, void *ctx);
static void uart_rx_task(void *arg);
static void uart_tx_task(void *arg);

/**
 * @brief 初始化串口桥接模块。
 *
 * 该函数会安装 ESP-IDF UART 驱动、设置串口参数和引脚映射，并创建用于
 * MQTT 下行命令异步发送的内部队列。
 *
 * @return
 *      - ESP_OK: 初始化成功
 *      - ESP_ERR_NO_MEM: 发送队列创建失败
 */
esp_err_t uart_bridge_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, UART_BUF_SIZE, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT,
                                 UART_TX_PIN,
                                 UART_RX_PIN,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));

    if (s_uart_tx_queue == NULL) {
        s_uart_tx_queue = xQueueCreate(UART_TX_QUEUE_LENGTH, sizeof(uart_tx_message_t));
        if (s_uart_tx_queue == NULL) {
            ESP_LOGE(TAG, "Failed to create UART TX queue");
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(TAG,
             "UART ready: port=%d tx=%d rx=%d baud=%d buf=%d queue_len=%d",
             (int)UART_PORT,
             UART_TX_PIN,
             UART_RX_PIN,
             UART_BAUD,
             UART_BUF_SIZE,
             UART_TX_QUEUE_LENGTH);
    app_status_set_uart_ready(true);
    return ESP_OK;
}

/**
 * @brief 启动 UART 发送任务。
 *
 * 发送任务会长期阻塞在发送队列上，收到消息后按顺序写入串口并等待发送完成。
 * 如果任务已经存在，则直接返回成功，便于上层重复调用。
 *
 * @return
 *      - ESP_OK: 任务已启动或已存在
 *      - ESP_FAIL: 任务创建失败
 */
esp_err_t uart_bridge_start_tx_task(void)
{
    if (s_uart_tx_task_handle != NULL) {
        return ESP_OK;
    }

    if (xTaskCreate(uart_tx_task,
                    "uart_tx_task",
                    UART_TX_TASK_STACK_SIZE,
                    NULL,
                    UART_TASK_PRIORITY,
                    &s_uart_tx_task_handle) != pdPASS) {
        return ESP_FAIL;
    }

    app_status_set_uart_tx_task_running(true);
    return ESP_OK;
}

/**
 * @brief 启动 UART 接收任务并保存上层回调。
 *
 * 接收任务会持续读取串口字节流，按换行符拼接成完整文本行后交给回调函数，
 * 供日志和 MQTT 层进一步处理。
 *
 * @param handler 收到完整文本行后的回调函数。
 * @param ctx 透传给回调的用户上下文。
 *
 * @return
 *      - ESP_OK: 任务已启动或已存在
 *      - ESP_FAIL: 任务创建失败
 */
esp_err_t uart_bridge_start_rx_task(uart_bridge_line_handler_t handler, void *ctx)
{
    s_line_handler = handler;
    s_line_handler_ctx = ctx;

    if (s_uart_rx_task_handle != NULL) {
        return ESP_OK;
    }

    if (xTaskCreate(uart_rx_task,
                    "uart_rx_task",
                    UART_RX_TASK_STACK_SIZE,
                    NULL,
                    UART_TASK_PRIORITY,
                    &s_uart_rx_task_handle) != pdPASS) {
        return ESP_FAIL;
    }

    app_status_set_uart_rx_task_running(true);
    return ESP_OK;
}

/**
 * @brief 把 MQTT 下行命令排入 UART 发送队列。
 *
 * 该函数会对输入做基本合法性检查，把 MQTT 负载复制到固定大小的消息缓冲区中，
 * 必要时记录“发送后补换行”的标志，然后交给发送任务异步处理。
 *
 * @param payload MQTT 收到的命令内容。
 * @param payload_len 命令的字节长度。
 */
void uart_bridge_queue_command(const char *payload, int payload_len)
{
    uart_tx_message_t msg = {0};

    if (payload == NULL || payload_len <= 0) {
        ESP_LOGW(TAG, "Ignore empty MQTT command");
        return;
    }

    if (s_uart_tx_queue == NULL) {
        ESP_LOGW(TAG, "UART TX queue not ready");
        app_status_note_uart_tx_result(false);
        return;
    }

    if (payload_len >= UART_BUF_SIZE) {
        ESP_LOGW(TAG,
                 "MQTT command too long for UART buffer, drop (len=%d max=%d)",
                 payload_len,
                 UART_BUF_SIZE - 1);
        app_status_note_uart_tx_result(false);
        return;
    }

    memcpy(msg.data, payload, (size_t)payload_len);
    msg.len = (size_t)payload_len;

#if UART_APPEND_NEWLINE_ON_MQTT_COMMAND
    msg.append_newline = payload[payload_len - 1] != '\n';
#endif

    if (xQueueSend(s_uart_tx_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "UART TX queue full, drop MQTT command");
        app_status_note_uart_tx_result(false);
        return;
    }

    app_status_note_uart_tx_queued();
    ESP_LOGI_VERBOSE(TAG,
             "MQTT->UART queued (%d bytes%s): %.*s",
             payload_len,
             msg.append_newline ? " + newline" : "",
             payload_len,
             payload);
}

/**
 * @brief line_reader callback: invoked for each complete UART line.
 *
 * Records rx_ms, logs the line, updates counters, and forwards to the
 * upper-layer handler registered via uart_bridge_start_rx_task().
 */
static void uart_rx_line_cb(const char *line, void *ctx)
{
    uint32_t rx_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    (void)ctx;

    ESP_LOGI_VERBOSE(TAG,
             "UART RX line (%u bytes) @ %lu ms: %s",
             (unsigned int)strlen(line),
             (unsigned long)rx_ms,
             line);
    app_status_note_uart_rx_line();
    app_status_set_last_frame_ms(rx_ms);

    if (s_line_handler != NULL) {
        s_line_handler(line, rx_ms, s_line_handler_ctx);
    }
}

/**
 * @brief UART 接收任务，逐字节读取并交由 line_reader 切分成文本行。
 *
 * @param arg 任务参数，当前未使用。
 */
static void uart_rx_task(void *arg)
{
    uint8_t ch;
    char *line_buf = malloc(UART_LINE_BUF_SIZE);
    line_reader_t reader;

    (void)arg;

    if (line_buf == NULL) {
        ESP_LOGE(TAG, "No memory for UART RX line buffer (%d bytes)", UART_LINE_BUF_SIZE);
        app_status_note_uart_rx_overflow();
        app_status_set_uart_rx_task_running(false);
        vTaskDelete(NULL);
        return;
    }

    line_reader_init(&reader, line_buf, UART_LINE_BUF_SIZE, uart_rx_line_cb, NULL);

    while (1) {
        int len = uart_read_bytes(UART_PORT, &ch, 1, pdMS_TO_TICKS(100));

        if (len <= 0) {
            continue;
        }

        if (line_reader_feed(&reader, ch) == LINE_READER_EVENT_OVERFLOW) {
            ESP_LOGW(TAG, "UART RX line too long (%d max), discarding until newline",
                     UART_LINE_BUF_SIZE);
            app_status_note_uart_rx_drop();
            app_status_note_uart_rx_overflow();
        }
    }
}

/**
 * @brief UART 发送任务，负责把排队命令写给 STM32。
 *
 * 任务会一直等待队列消息，收到后先写入主数据，再根据配置决定是否补发换行，
 * 最后等待硬件发送完成，确保 MQTT 下行命令按顺序可靠转发。
 *
 * @param arg 任务参数，当前未使用。
 */
static void uart_tx_task(void *arg)
{
    uart_tx_message_t msg;

    (void)arg;

    while (1) {
        if (xQueueReceive(s_uart_tx_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (uart_write_bytes(UART_PORT, msg.data, msg.len) < 0) {
            ESP_LOGW(TAG, "Failed to write MQTT command to UART");
            app_status_note_uart_tx_result(false);
            continue;
        }

#if UART_APPEND_NEWLINE_ON_MQTT_COMMAND
        if (msg.append_newline && uart_write_bytes(UART_PORT, "\n", 1) < 0) {
            ESP_LOGW(TAG, "Failed to append newline to UART command");
            app_status_note_uart_tx_result(false);
            continue;
        }
#endif

        if (uart_wait_tx_done(UART_PORT, pdMS_TO_TICKS(1000)) != ESP_OK) {
            ESP_LOGW(TAG, "UART TX did not finish before timeout");
            app_status_note_uart_tx_result(false);
            continue;
        }

        app_status_note_uart_tx_result(true);
        ESP_LOGI_VERBOSE(TAG,
                 "UART TX done (%u bytes%s): %.*s",
                 (unsigned int)msg.len,
#if UART_APPEND_NEWLINE_ON_MQTT_COMMAND
                 msg.append_newline ? " + newline" : "",
#else
                 "",
#endif
                 (int)msg.len,
                 msg.data);
    }
}
