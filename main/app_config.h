#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"

/* ==========================================================================
 * 全局标签与编译模式
 * ========================================================================== */

/** @brief 全局日志标签，所有模块统一使用此标签输出日志 */
#define APP_TAG "PULSEOX_ESP"

/* ==========================================================================
 * 日志模式配置
 * ========================================================================== */

/**
 * @brief 详细日志编译开关。
 *
 * 0 (默认) = 安静模式：高频数据路径日志降级为 DEBUG，不再刷屏；周期性 STATUS 关闭。
 * 1        = 详细模式：恢复所有 INFO 日志，方便调试。
 */
#define APP_VERBOSE_LOGGING 0

#if APP_VERBOSE_LOGGING
/** 详细日志宏：verbose 模式时相当于 ESP_LOGI，安静模式时降级为 ESP_LOGD */
#define ESP_LOGI_VERBOSE(tag, format, ...) ESP_LOGI(tag, format, ##__VA_ARGS__)
#else
#define ESP_LOGI_VERBOSE(tag, format, ...) ESP_LOGD(tag, format, ##__VA_ARGS__)
#endif

/* ==========================================================================
 * WiFi 配置
 * ========================================================================== */

/** @brief WiFi STA 模式要连接的 SSID */
#define WIFI_SSID           "jiamdaole"
/** @brief WiFi STA 模式使用的密码 */
#define WIFI_PASS           "12345678"

/* ==========================================================================
 * MQTT 配置
 * ========================================================================== */

/** @brief MQTT broker 连接地址（格式: mqtt://IP 或 mqtts://域名） */
#define MQTT_BROKER_URI     "mqtt://172.20.10.4"
/** @brief MQTT 上行发布主题，STM32 传感器数据经 ESP32 解析后发布到此主题 */
#define MQTT_PUBLISH_TOPIC  "pulseox/data"
/** @brief MQTT 下行命令主题，ESP32 订阅此主题并将收到的命令转发给 STM32 */
#define MQTT_COMMAND_TOPIC  "pulseox/cmd"
/** @brief MQTT 下行自检命令；收到后 ESP32 内部生成一帧测试数据并发布到上行 topic */
#define MQTT_SELFTEST_COMMAND "ESP_SELFTEST_MQTT"
/** @brief MQTT 自检发布任务栈大小（协议解析 + JSON 构建比普通事件回调更吃栈） */
#define MQTT_SELFTEST_TASK_STACK_SIZE 16384

/**
 * @brief MQTT 上报 JSON 中的桥接标识字段
 *
 * 每一帧上报 JSON 都会携带 bridge/source/channel/protocol 四个元信息字段，
 * 便于云端区分数据来源、通道与协议版本。
 */
#define MQTT_BRIDGE_NAME    "esp32c3"
#define MQTT_SOURCE_NAME    "stm32"
#define MQTT_CHANNEL_NAME   "uart1"
#define MQTT_PROTOCOL_NAME  "stm32-compact-v2"

/* ==========================================================================
 * UART 串口配置
 * ========================================================================== */

/** @brief 使用的 UART 端口号（ESP32-C3 通常用 UART_NUM_1 连接 STM32） */
#define UART_PORT       UART_NUM_1
/** @brief UART TX 引脚（ESP32→STM32） */
#define UART_TX_PIN     7
/** @brief UART RX 引脚（STM32→ESP32） */
#define UART_RX_PIN     3
/** @brief UART 波特率 */
#define UART_BAUD       115200
/** @brief UART 驱动内部收发缓冲区大小（字节） */
#define UART_BUF_SIZE   1024
/** @brief UART RX 行缓冲区大小（字节），单帧文本最大长度（102-col CSV ~1.5KB，留余量） */
#define UART_LINE_BUF_SIZE 4096

/** @brief UART 发送队列长度（可缓存的待发送 MQTT 下行命令数） */
#define UART_TX_QUEUE_LENGTH 8
/** @brief UART 收发任务的栈大小（字节） */
#define UART_TX_TASK_STACK_SIZE 4096
#define UART_RX_TASK_STACK_SIZE 16384
/** @brief UART 收发任务的 FreeRTOS 优先级 */
#define UART_TASK_PRIORITY   5

/**
 * @brief MQTT 下行命令写入串口后是否自动补换行符
 *
 * 设为 1 时，如果 MQTT 收到的命令末尾不是 `\n`，会自动追加一个换行再发送给 STM32。
 * 设为 0 时原样透传，不做任何修改。
 */
#define UART_APPEND_NEWLINE_ON_MQTT_COMMAND 1

/* ==========================================================================
 * USB 传输配置
 * ========================================================================== */

/** @brief 启用 USB Serial/JTAG 作为优先的 PC 直连传输通道 */
#define USB_TRANSPORT_ENABLED 1
/** @brief USB Serial/JTAG 发送环形缓冲区大小（字节） */
#define USB_TRANSPORT_TX_BUF_SIZE 2048
/** @brief USB Serial/JTAG 接收环形缓冲区大小（字节） */
#define USB_TRANSPORT_RX_BUF_SIZE 512
/** @brief USB 发送队列长度（待发送 JSON 条数） */
#define USB_TRANSPORT_TX_QUEUE_LENGTH 4
/** @brief USB 接收 PC 下行命令的行缓冲区大小（字节） */
#define USB_TRANSPORT_LINE_BUF_SIZE 1024
/** @brief USB 收发任务栈大小（字节） */
#define USB_TRANSPORT_TASK_STACK_SIZE 4096
/** @brief USB 收发任务 FreeRTOS 优先级 */
#define USB_TRANSPORT_TASK_PRIORITY 5
/** @brief USB 分片写入超时时间（毫秒） */
#define USB_TRANSPORT_WRITE_TIMEOUT_MS 20
/** @brief USB 单条 JSON 发送完成等待超时时间（毫秒） */
#define USB_TRANSPORT_TX_DONE_TIMEOUT_MS 50
/** @brief USB 会话超时时间（毫秒），超时无 PING/START 则自动停用 USB 通道 */
#define USB_SESSION_TIMEOUT_MS 15000

/* ==========================================================================
 * FreeRTOS 事件位定义
 * ========================================================================== */

/** @brief WiFi 已连接并获得 IP 的事件位 */
#define APP_WIFI_CONNECTED_BIT BIT0
/** @brief MQTT 已成功连接 broker 的事件位 */
#define APP_MQTT_CONNECTED_BIT BIT1

/* ==========================================================================
 * 运行状态上报模块参数
 * ========================================================================== */

/** @brief 状态上报任务的栈大小（字节） */
#define APP_STATUS_TASK_STACK_SIZE 4096
/** @brief 状态上报任务的 FreeRTOS 优先级 */
#define APP_STATUS_TASK_PRIORITY   3
/**
 * @brief 状态定期上报间隔（毫秒）
 *
 * 状态任务会以此周期输出包含 WiFi/MQTT/UART/协议 等子系统状态的汇总日志。
 */
#define APP_STATUS_REPORT_PERIOD_MS 10000

#endif
