#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "app_config.h"
#include "app_status.h"

static const char *TAG = APP_TAG;

/**
 * @brief 全局运行状态快照结构体。
 *
 * 该结构体汇总了 WiFi/MQTT/UART/协议 四个子系统的布尔状态标志
 * 和运行计数器，统一由互斥锁 s_status_mutex 保护。
 *
 * 布尔字段由各模块的状态变化回调更新；计数器字段由"快速路径"中的
 * note_ 系列接口递增。后台状态任务周期性读取快照并以日志形式输出。
 */
typedef struct {
    /* ---- WiFi ---- */
    bool wifi_started;      /**< WiFi 栈是否已启动 */
    bool wifi_connected;    /**< WiFi 是否已获取 IP */

    /* ---- MQTT ---- */
    bool mqtt_started;      /**< MQTT 客户端是否已创建并启动 */
    bool mqtt_connected;    /**< MQTT 是否已连接 broker */
    bool mqtt_subscribed;   /**< MQTT 是否已成功订阅下行命令主题 */

    /* ---- UART ---- */
    bool uart_ready;         /**< UART 驱动是否已初始化 */
    bool uart_tx_task_running; /**< UART 发送任务是否在运行 */
    bool uart_rx_task_running; /**< UART 接收任务是否在运行 */

    /* ---- 协议解析 ---- */
    bool last_protocol_ok;  /**< 最近一帧协议数据是否解析成功 */
    char last_frame_type;   /**< 最近一帧的类型（M/T/?/!），初始化为 '-' */
    uint32_t last_frame_ms; /**< 最近一帧 UART 接收完成时的系统毫秒时间戳 */

    /* ---- 计数器 ---- */
    uint32_t uart_rx_lines;      /**< UART 接收到的完整文本行总数 */
    uint32_t uart_rx_drops;      /**< UART 接收溢出丢弃的行数 */
    uint32_t uart_rx_overflows;  /**< UART 行缓冲区溢出事件次数 */
    uint32_t uart_tx_queued;     /**< UART 发送队列已入队的消息数 */
    uint32_t uart_tx_ok;         /**< UART 发送成功次数 */
    uint32_t uart_tx_fail;       /**< UART 发送失败次数 */

    uint32_t mqtt_publish_ok;    /**< MQTT 发布成功次数 */
    uint32_t mqtt_publish_fail;  /**< MQTT 发布失败次数 */
    uint32_t mqtt_command_count; /**< MQTT 收到的下行命令总数 */

    uint32_t protocol_ok;        /**< 协议帧解析成功次数 */
    uint32_t protocol_error;     /**< 协议帧解析失败次数 */
    uint32_t forward_fail;       /**< 转发失败次数（JSON 构建或 MQTT 发布失败） */

} app_status_snapshot_t;

/** @brief 保护全局状态快照读写线程安全的 FreeRTOS 互斥锁 */
static SemaphoreHandle_t s_status_mutex = NULL;
/** @brief 周期性状态上报任务的句柄，用于判断任务是否已创建 */
static TaskHandle_t s_status_task_handle = NULL;
/** @brief 全局运行状态快照实例（静态初始化，last_frame_type 默认 '-'） */
static app_status_snapshot_t s_status = {
    .last_protocol_ok = false,
    .last_frame_type = '-',
};

/**
 * @brief 周期性状态上报任务入口。
 *
 * 任务按 APP_STATUS_REPORT_PERIOD_MS 间隔循环，每次输出全量状态快照日志。
 * 参数 arg 当前未使用。
 */
static void app_status_task(void *arg);

/**
 * @brief 在互斥锁保护下复制一份状态快照。
 *
 * 读取 s_status 到调用方提供的 snapshot 中，确保读到的是瞬时一致的副本。
 * 若 mutex 不存在或 snapshot 为 NULL 则安全返回。
 */
static void app_status_copy_snapshot(app_status_snapshot_t *snapshot);

/* ---- 以下四个函数将布尔状态字段映射为可读的短字符串，用于日志输出 ---- */
static const char *wifi_state_string(const app_status_snapshot_t *snapshot);
static const char *mqtt_state_string(const app_status_snapshot_t *snapshot);
static const char *uart_state_string(const app_status_snapshot_t *snapshot);
static const char *protocol_state_string(const app_status_snapshot_t *snapshot);

/**
 * @brief 线程安全地设置布尔字段，并在发生变更时自动触发快照日志。
 *
 * @param field  指向 s_status 中的布尔字段指针。
 * @param value  要设置的目标值。
 * @param reason 变更原因标签，仅当值确实改变时用于快照日志。
 */
static void set_flag_with_reason(bool *field, bool value, const char *reason);

esp_err_t app_status_init(void)
{
    if (s_status_mutex != NULL) {
        return ESP_OK;
    }

    s_status_mutex = xSemaphoreCreateMutex();
    if (s_status_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t app_status_start_task(void)
{
    if (s_status_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_status_task_handle != NULL) {
        return ESP_OK;
    }

    if (xTaskCreate(app_status_task,
                    "app_status_task",
                    APP_STATUS_TASK_STACK_SIZE,
                    NULL,
                    APP_STATUS_TASK_PRIORITY,
                    &s_status_task_handle) != pdPASS) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

void app_status_log_snapshot(const char *reason)
{
    app_status_snapshot_t snapshot;
    const char *label = (reason != NULL && reason[0] != '\0') ? reason : "snapshot";

    if (s_status_mutex == NULL) {
        return;
    }

    app_status_copy_snapshot(&snapshot);

    ESP_LOGI(TAG,
             "STATUS[%s] wifi=%s mqtt=%s uart=%s protocol=%s(last=%c)",
             label,
             wifi_state_string(&snapshot),
             mqtt_state_string(&snapshot),
             uart_state_string(&snapshot),
             protocol_state_string(&snapshot),
             snapshot.last_frame_type);
    ESP_LOGI(TAG,
             "STATUS[%s] uart_rx=%lu uart_drop=%lu uart_overflow=%lu uart_tx_queued=%lu uart_tx_ok=%lu uart_tx_fail=%lu",
             label,
             (unsigned long)snapshot.uart_rx_lines,
             (unsigned long)snapshot.uart_rx_drops,
             (unsigned long)snapshot.uart_rx_overflows,
             (unsigned long)snapshot.uart_tx_queued,
             (unsigned long)snapshot.uart_tx_ok,
             (unsigned long)snapshot.uart_tx_fail);
    ESP_LOGI(TAG,
             "STATUS[%s] mqtt_cmd=%lu mqtt_pub_ok=%lu mqtt_pub_fail=%lu forward_fail=%lu protocol_ok=%lu protocol_err=%lu last_frame_ms=%lu",
             label,
             (unsigned long)snapshot.mqtt_command_count,
             (unsigned long)snapshot.mqtt_publish_ok,
             (unsigned long)snapshot.mqtt_publish_fail,
             (unsigned long)snapshot.forward_fail,
             (unsigned long)snapshot.protocol_ok,
             (unsigned long)snapshot.protocol_error,
             (unsigned long)snapshot.last_frame_ms);
}

void app_status_set_wifi_started(bool started)
{
    set_flag_with_reason(&s_status.wifi_started, started, started ? "wifi_started" : "wifi_stopped");
}

void app_status_set_wifi_connected(bool connected)
{
    set_flag_with_reason(&s_status.wifi_connected,
                         connected,
                         connected ? "wifi_connected" : "wifi_disconnected");
}

void app_status_set_mqtt_started(bool started)
{
    set_flag_with_reason(&s_status.mqtt_started, started, started ? "mqtt_started" : "mqtt_stopped");
}

void app_status_set_mqtt_connected(bool connected)
{
    bool changed = false;

    if (s_status_mutex == NULL) {
        return;
    }

    if (xSemaphoreTake(s_status_mutex, portMAX_DELAY) == pdTRUE) {
        if (s_status.mqtt_connected != connected) {
            s_status.mqtt_connected = connected;
            changed = true;
        }

        if (!connected) {
            s_status.mqtt_subscribed = false;
        }

        xSemaphoreGive(s_status_mutex);
    }

    if (changed) {
        app_status_log_snapshot(connected ? "mqtt_connected" : "mqtt_disconnected");
    }
}

void app_status_set_mqtt_subscribed(bool subscribed)
{
    set_flag_with_reason(&s_status.mqtt_subscribed,
                         subscribed,
                         subscribed ? "mqtt_subscribed" : "mqtt_unsubscribed");
}

void app_status_set_uart_ready(bool ready)
{
    set_flag_with_reason(&s_status.uart_ready, ready, ready ? "uart_ready" : "uart_not_ready");
}

void app_status_set_uart_tx_task_running(bool running)
{
    set_flag_with_reason(&s_status.uart_tx_task_running,
                         running,
                         running ? "uart_tx_task_started" : "uart_tx_task_stopped");
}

void app_status_set_uart_rx_task_running(bool running)
{
    set_flag_with_reason(&s_status.uart_rx_task_running,
                         running,
                         running ? "uart_rx_task_started" : "uart_rx_task_stopped");
}

void app_status_note_uart_rx_line(void)
{
    if (s_status_mutex == NULL) {
        return;
    }

    if (xSemaphoreTake(s_status_mutex, portMAX_DELAY) == pdTRUE) {
        s_status.uart_rx_lines++;
        xSemaphoreGive(s_status_mutex);
    }
}

void app_status_note_uart_rx_drop(void)
{
    if (s_status_mutex == NULL) {
        return;
    }

    if (xSemaphoreTake(s_status_mutex, portMAX_DELAY) == pdTRUE) {
        s_status.uart_rx_drops++;
        xSemaphoreGive(s_status_mutex);
    }
}

void app_status_note_uart_rx_overflow(void)
{
    if (s_status_mutex == NULL) {
        return;
    }

    if (xSemaphoreTake(s_status_mutex, portMAX_DELAY) == pdTRUE) {
        s_status.uart_rx_overflows++;
        xSemaphoreGive(s_status_mutex);
    }
}

void app_status_set_last_frame_ms(uint32_t ms)
{
    if (s_status_mutex == NULL) {
        return;
    }

    if (xSemaphoreTake(s_status_mutex, portMAX_DELAY) == pdTRUE) {
        s_status.last_frame_ms = ms;
        xSemaphoreGive(s_status_mutex);
    }
}

void app_status_note_uart_tx_queued(void)
{
    if (s_status_mutex == NULL) {
        return;
    }

    if (xSemaphoreTake(s_status_mutex, portMAX_DELAY) == pdTRUE) {
        s_status.uart_tx_queued++;
        xSemaphoreGive(s_status_mutex);
    }
}

void app_status_note_uart_tx_result(bool ok)
{
    if (s_status_mutex == NULL) {
        return;
    }

    if (xSemaphoreTake(s_status_mutex, portMAX_DELAY) == pdTRUE) {
        if (ok) {
            s_status.uart_tx_ok++;
        } else {
            s_status.uart_tx_fail++;
        }
        xSemaphoreGive(s_status_mutex);
    }
}

void app_status_note_mqtt_publish(bool ok)
{
    if (s_status_mutex == NULL) {
        return;
    }

    if (xSemaphoreTake(s_status_mutex, portMAX_DELAY) == pdTRUE) {
        if (ok) {
            s_status.mqtt_publish_ok++;
        } else {
            s_status.mqtt_publish_fail++;
        }
        xSemaphoreGive(s_status_mutex);
    }
}

void app_status_note_mqtt_command(void)
{
    if (s_status_mutex == NULL) {
        return;
    }

    if (xSemaphoreTake(s_status_mutex, portMAX_DELAY) == pdTRUE) {
        s_status.mqtt_command_count++;
        xSemaphoreGive(s_status_mutex);
    }
}

void app_status_note_forward_fail(void)
{
    if (s_status_mutex == NULL) {
        return;
    }

    if (xSemaphoreTake(s_status_mutex, portMAX_DELAY) == pdTRUE) {
        s_status.forward_fail++;
        xSemaphoreGive(s_status_mutex);
    }
}

void app_status_note_protocol_result(stm32_protocol_result_t result)
{
    bool should_log = false;

    if (s_status_mutex == NULL) {
        return;
    }

    if (xSemaphoreTake(s_status_mutex, portMAX_DELAY) == pdTRUE) {
        switch (result) {
            case STM32_PROTOCOL_RESULT_MEASUREMENT:
                s_status.protocol_ok++;
                s_status.last_protocol_ok = true;
                s_status.last_frame_type = 'M';
                break;

            case STM32_PROTOCOL_RESULT_TIME_ACK:
                s_status.protocol_ok++;
                s_status.last_protocol_ok = true;
                s_status.last_frame_type = 'T';
                break;

            case STM32_PROTOCOL_RESULT_PARSE_ERROR:
                s_status.protocol_error++;
                s_status.last_protocol_ok = false;
                s_status.last_frame_type = '?';
                should_log = true;
                break;

            case STM32_PROTOCOL_RESULT_NO_MEMORY:
                s_status.protocol_error++;
                s_status.last_protocol_ok = false;
                s_status.last_frame_type = '!';
                should_log = true;
                break;

            default:
                break;
        }

        xSemaphoreGive(s_status_mutex);
    }

    if (should_log) {
        app_status_log_snapshot("protocol_error");
    }
}

/**
 * @brief 状态上报任务主循环。
 *
 * 按配置的周期定时输出全量状态快照，方便从串口日志中持续观察系统健康状况。
 * 与事件驱动的快照（如 MQTT 断连、协议错误）互补，提供"心跳式"背景信息。
 */
static void app_status_task(void *arg)
{
    (void)arg;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(APP_STATUS_REPORT_PERIOD_MS));
#if APP_VERBOSE_LOGGING
        app_status_log_snapshot("periodic");
#endif
    }
}

static void app_status_copy_snapshot(app_status_snapshot_t *snapshot)
{
    if (snapshot == NULL || s_status_mutex == NULL) {
        return;
    }

    if (xSemaphoreTake(s_status_mutex, portMAX_DELAY) == pdTRUE) {
        *snapshot = s_status;
        xSemaphoreGive(s_status_mutex);
    }
}

/**
 * @brief 将 WiFi 布尔状态转为单行可读字符串。
 *
 * 优先级: wifi_connected → "ok"; wifi_started → "connecting"; 否则 → "stopped"
 */
static const char *wifi_state_string(const app_status_snapshot_t *snapshot)
{
    if (snapshot->wifi_connected) {
        return "ok";
    }

    return snapshot->wifi_started ? "connecting" : "stopped";
}

/**
 * @brief 将 MQTT 布尔状态转为单行可读字符串。
 *
 * 优先级: connected+subscribed → "ok"; connected 仅 → "connected";
 *         started → "starting"; 否则 → "stopped"
 */
static const char *mqtt_state_string(const app_status_snapshot_t *snapshot)
{
    if (snapshot->mqtt_connected && snapshot->mqtt_subscribed) {
        return "ok";
    }

    if (snapshot->mqtt_connected) {
        return "connected";
    }

    return snapshot->mqtt_started ? "starting" : "stopped";
}

/**
 * @brief 将 UART 布尔状态转为单行可读字符串。
 *
 * 条件: 驱动就绪 + TX/RX 任务同时运行 → "ok";
 *       仅驱动就绪 → "partial"; 否则 → "stopped"
 */
static const char *uart_state_string(const app_status_snapshot_t *snapshot)
{
    if (snapshot->uart_ready && snapshot->uart_tx_task_running && snapshot->uart_rx_task_running) {
        return "ok";
    }

    if (snapshot->uart_ready) {
        return "partial";
    }

    return "stopped";
}

/**
 * @brief 将协议解析状态转为单行可读字符串。
 *
 * 当尚未收到任何帧时（ok/error 均为 0）返回 "idle"；
 * 否则根据最近一帧的解析结果返回 "ok" 或 "error"。
 */
static const char *protocol_state_string(const app_status_snapshot_t *snapshot)
{
    if (snapshot->protocol_ok == 0U && snapshot->protocol_error == 0U) {
        return "idle";
    }

    return snapshot->last_protocol_ok ? "ok" : "error";
}

/**
 * @brief 加锁设置布尔字段，值变更时自动输出快照。
 *
 * 这是所有 set_ 系列接口的通用实现：持锁比较新旧值，仅在确实改变时才更新
 * 并调用 app_status_log_snapshot 输出变更日志。若 mutex 尚未初始化或 field 为 NULL
 * 则安全返回。
 */
static void set_flag_with_reason(bool *field, bool value, const char *reason)
{
    bool changed = false;

    if (field == NULL || s_status_mutex == NULL) {
        return;
    }

    if (xSemaphoreTake(s_status_mutex, portMAX_DELAY) == pdTRUE) {
        if (*field != value) {
            *field = value;
            changed = true;
        }

        xSemaphoreGive(s_status_mutex);
    }

    if (changed) {
        app_status_log_snapshot(reason);
    }
}
