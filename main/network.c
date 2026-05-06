#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "mqtt_client.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "app_status.h"
#include "network.h"
#include "stm32_protocol.h"
#include "uart_bridge.h"

static const char *TAG = APP_TAG;

/**
 * @brief WiFi/MQTT 复合事件组。
 *
 * 同时承载 APP_WIFI_CONNECTED_BIT 和 APP_MQTT_CONNECTED_BIT 两个事件位，
 * 用于网络层各组件间的同步（如等待 WiFi 就绪后才启动 MQTT）。
 */
static EventGroupHandle_t s_wifi_event_group = NULL;

/** @brief MQTT 客户端全局句柄，初始为 NULL，由 network_start_mqtt() 创建 */
static esp_mqtt_client_handle_t s_mqtt_client = NULL;

/* ---- 内部前置声明 ---- */
static void init_nvs_flash(void);
static void wifi_init_sta(void);
static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data);
static void mqtt_event_handler(void *handler_args,
                               esp_event_base_t base,
                               int32_t event_id,
                               void *event_data);
static void log_mqtt_error_details(esp_mqtt_event_handle_t event);
static bool mqtt_event_matches_topic(esp_mqtt_event_handle_t event, const char *topic);
static const char *protocol_result_to_string(stm32_protocol_result_t result);

/**
 * @brief 初始化网络模块的基础能力。
 *
 * 当前实现先确保 NVS 可用，再启动 Wi-Fi STA。后续网络相关动作
 * 都建立在这里完成的基础设施之上。
 */
void network_init(void)
{
    init_nvs_flash();
    wifi_init_sta();
}

/**
 * @brief 等待 Wi-Fi 成功连上并获取 IP。
 *
 * 该函数通过事件组阻塞等待连接位被置位，适合作为 MQTT 启动前的同步点，
 * 避免 broker 连接在底层网络尚未就绪时立即失败。
 */
void network_wait_for_wifi(void)
{
    ESP_LOGI(TAG, "Waiting for WiFi...");
    xEventGroupWaitBits(s_wifi_event_group,
                        APP_WIFI_CONNECTED_BIT,
                        pdFALSE,
                        pdTRUE,
                        portMAX_DELAY);
}

/**
 * @brief 创建并启动 MQTT 客户端。
 *
 * 函数负责构造客户端配置、初始化实例、注册统一事件回调，并正式启动 MQTT。
 * 如果客户端之前已经启动过，会直接返回，避免重复创建实例。
 */
void network_start_mqtt(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
    };

    if (s_mqtt_client != NULL) {
        return;
    }

    ESP_LOGI(TAG,
             "Starting MQTT client, broker=%s, pub_topic=%s, cmd_topic=%s",
             MQTT_BROKER_URI,
             MQTT_PUBLISH_TOPIC,
             MQTT_COMMAND_TOPIC);

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_mqtt_client == NULL) {
        ESP_LOGE(TAG, "Failed to create MQTT client");
        abort();
    }
    app_status_set_mqtt_started(true);

    ESP_ERROR_CHECK(esp_mqtt_client_register_event(
        s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_mqtt_client));
}

/**
 * @brief 把一行 UART 文本发布到 MQTT 上行主题。
 *
 * 该函数会先检查输入内容、客户端实例和连接状态，随后调用协议模块把串口文本
 * 转换为 JSON，最后发送到预定义主题。任何一步失败都会记录日志并安全返回。
 *
 * @param line 已完成按行分包的串口原始文本。
 */
void network_publish_uart_line(const char *line)
{
    EventBits_t bits;
    char *json_payload;
    int msg_id;
    stm32_protocol_result_t protocol_result = STM32_PROTOCOL_RESULT_NONE;

    if (line == NULL || line[0] == '\0') {
        return;
    }

    ESP_LOGI(TAG, "UART->MQTT raw line (%u bytes): %s", (unsigned int)strlen(line), line);

    if (s_mqtt_client == NULL) {
        ESP_LOGW(TAG, "MQTT client not created yet");
        app_status_note_mqtt_publish(false);
        return;
    }

    bits = xEventGroupGetBits(s_wifi_event_group);
    if ((bits & APP_MQTT_CONNECTED_BIT) == 0) {
        ESP_LOGW(TAG, "MQTT not ready, drop UART line: %s", line);
        app_status_note_mqtt_publish(false);
        return;
    }

    json_payload = stm32_protocol_build_publish_json(line, &protocol_result);
    app_status_note_protocol_result(protocol_result);
    if (json_payload == NULL) {
        ESP_LOGW(TAG, "Failed to build JSON payload for UART line");
        app_status_note_mqtt_publish(false);
        return;
    }

    ESP_LOGI(TAG,
             "UART->MQTT parsed result=%s payload=%s",
             protocol_result_to_string(protocol_result),
             json_payload);

    msg_id = esp_mqtt_client_publish(s_mqtt_client, MQTT_PUBLISH_TOPIC, json_payload, 0, 1, 0);
    free(json_payload);

    if (msg_id >= 0) {
        ESP_LOGI(TAG, "Published to MQTT topic=%s, msg_id=%d", MQTT_PUBLISH_TOPIC, msg_id);
        app_status_note_mqtt_publish(true);
    } else {
        ESP_LOGW(TAG, "Failed to publish UART line to topic=%s", MQTT_PUBLISH_TOPIC);
        app_status_note_mqtt_publish(false);
    }
}

/**
 * @brief 初始化或修复 NVS Flash。
 *
 * ESP-IDF 的 Wi-Fi 和 MQTT 栈依赖 NVS 存储。若检测到无可用页或版本不兼容，
 * 这里会先擦除再重新初始化，确保后续网络组件能正常启动。
 */
static void init_nvs_flash(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    ESP_ERROR_CHECK(err);
}

/**
 * @brief 以 STA 模式初始化并启动 Wi-Fi。
 *
 * 该函数会创建事件组、初始化网络接口和默认事件循环，注册 Wi-Fi/IP 事件处理器，
 * 然后根据配置好的 SSID 和密码启动 Wi-Fi 站点模式。
 */
static void wifi_init_sta(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .failure_retry_cnt = 5,
        },
    };

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create event group");
        abort();
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    app_status_set_wifi_started(true);
}

/**
 * @brief 处理 Wi-Fi 与 IP 层的事件。
 *
 * 当 STA 启动时立即尝试连接；当连接断开时清除连接状态并发起重连；
 * 当成功获取 IP 后则通知上层网络已经可用。
 *
 * @param arg 事件处理器用户参数，当前未使用。
 * @param event_base 事件所属模块，例如 WIFI_EVENT 或 IP_EVENT。
 * @param event_id 具体事件编号。
 * @param event_data 事件附带数据，当前实现未使用。
 */
static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "WiFi start, connecting...");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, retrying...");
        xEventGroupClearBits(s_wifi_event_group, APP_WIFI_CONNECTED_BIT | APP_MQTT_CONNECTED_BIT);
        app_status_set_wifi_connected(false);
        app_status_set_mqtt_connected(false);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "WiFi connected, got IP");
        xEventGroupSetBits(s_wifi_event_group, APP_WIFI_CONNECTED_BIT);
        app_status_set_wifi_connected(true);
    }
}

/**
 * @brief 统一处理 MQTT 生命周期和数据事件。
 *
 * 连接建立时订阅下行命令主题；连接断开时清除连接标志；
 * 收到数据时会校验主题和分片状态，并把命令转交 UART 模块。
 *
 * @param handler_args 注册事件时传入的用户参数，当前未使用。
 * @param base 事件基类，当前实现未使用。
 * @param event_id MQTT 事件编号。
 * @param event_data MQTT 事件上下文。
 */
static void mqtt_event_handler(void *handler_args,
                               esp_event_base_t base,
                               int32_t event_id,
                               void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    int msg_id;

    (void)handler_args;
    (void)base;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected");
            xEventGroupSetBits(s_wifi_event_group, APP_MQTT_CONNECTED_BIT);
            app_status_set_mqtt_connected(true);

            msg_id = esp_mqtt_client_subscribe(s_mqtt_client, MQTT_COMMAND_TOPIC, 1);
            if (msg_id >= 0) {
                ESP_LOGI(TAG, "Subscribed to command topic, msg_id=%d", msg_id);
            } else {
                ESP_LOGW(TAG, "Failed to subscribe to command topic");
            }
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT disconnected");
            xEventGroupClearBits(s_wifi_event_group, APP_MQTT_CONNECTED_BIT);
            app_status_set_mqtt_connected(false);
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error");
            log_mqtt_error_details(event);
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT subscription acknowledged, msg_id=%d", event->msg_id);
            app_status_set_mqtt_subscribed(true);
            break;

        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG,
                     "MQTT RX topic=%.*s payload=(%d bytes) %.*s",
                     event->topic_len,
                     event->topic != NULL ? event->topic : "",
                     event->data_len,
                     event->data_len,
                     event->data != NULL ? event->data : "");

            if (!mqtt_event_matches_topic(event, MQTT_COMMAND_TOPIC)) {
                ESP_LOGW(TAG,
                         "Ignored MQTT data on unexpected topic=%.*s",
                         event->topic_len,
                         event->topic != NULL ? event->topic : "");
                break;
            }

            if (event->current_data_offset != 0 || event->data_len != event->total_data_len) {
                ESP_LOGW(TAG,
                         "Fragmented MQTT payload is not supported for UART forwarding "
                         "(offset=%d data_len=%d total_len=%d)",
                         event->current_data_offset,
                         event->data_len,
                         event->total_data_len);
                break;
            }

            app_status_note_mqtt_command();
            ESP_LOGI(TAG, "MQTT->UART forwarding command");
            uart_bridge_queue_command(event->data, event->data_len);
            break;

        default:
            break;
    }
}

/**
 * @brief 输出更具体的 MQTT 错误诊断信息。
 *
 * 该函数把 MQTT 事件里携带的错误结构展开成可读日志，方便区分
 * TLS 传输错误、连接拒绝和其他未知错误。
 *
 * @param event 当前 MQTT 错误事件。
 */
static void log_mqtt_error_details(esp_mqtt_event_handle_t event)
{
    if (event == NULL || event->error_handle == NULL) {
        ESP_LOGW(TAG, "MQTT error detail unavailable");
        return;
    }

    switch (event->error_handle->error_type) {
        case MQTT_ERROR_TYPE_NONE:
            ESP_LOGW(TAG, "MQTT error type: none");
            break;

        case MQTT_ERROR_TYPE_TCP_TRANSPORT:
            ESP_LOGE(TAG,
                     "MQTT transport error: tls_esp_err=0x%x tls_stack_err=0x%x sock_errno=%d (%s)",
                     event->error_handle->esp_tls_last_esp_err,
                     event->error_handle->esp_tls_stack_err,
                     event->error_handle->esp_transport_sock_errno,
                     strerror(event->error_handle->esp_transport_sock_errno));
            break;

        case MQTT_ERROR_TYPE_CONNECTION_REFUSED:
            ESP_LOGE(TAG,
                     "MQTT connection refused, return_code=0x%x",
                     event->error_handle->connect_return_code);
            break;

        default:
            ESP_LOGE(TAG,
                     "MQTT unknown error type=0x%x, return_code=0x%x",
                     event->error_handle->error_type,
                     event->error_handle->connect_return_code);
            break;
    }
}

/**
 * @brief 判断 MQTT 数据事件是否来自指定主题。
 *
 * MQTT 事件中的主题名不是以 `\0` 结尾的标准 C 字符串，因此这里需要同时比较
 * 长度和内容，避免把其他主题的数据误转发到串口。
 *
 * @param event MQTT 数据事件。
 * @param topic 期望匹配的主题字符串。
 *
 * @return true 表示主题完全匹配，false 表示不匹配或输入非法。
 */
static bool mqtt_event_matches_topic(esp_mqtt_event_handle_t event, const char *topic)
{
    size_t topic_len;

    if (event == NULL || topic == NULL || event->topic == NULL) {
        return false;
    }

    topic_len = strlen(topic);
    return event->topic_len == (int)topic_len && strncmp(event->topic, topic, topic_len) == 0;
}

/**
 * @brief 将 STM32 协议解析结果枚举转为可读的字符串标签。
 *
 * 用于 MQTT 上行发布时的日志输出，方便在串口日志中快速区分消息类型。
 */
static const char *protocol_result_to_string(stm32_protocol_result_t result)
{
    switch (result) {
        case STM32_PROTOCOL_RESULT_MEASUREMENT:
            return "measurement";

        case STM32_PROTOCOL_RESULT_TIME_ACK:
            return "time_ack";

        case STM32_PROTOCOL_RESULT_PARSE_ERROR:
            return "parse_error";

        case STM32_PROTOCOL_RESULT_NO_MEMORY:
            return "no_memory";

        case STM32_PROTOCOL_RESULT_NONE:
        default:
            return "none";
    }
}
