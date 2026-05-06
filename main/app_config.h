#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"

/* ==========================================================================
 * 全局标签与编译模式
 * ========================================================================== */

/** @brief 全局日志标签，所有模块统一使用此标签输出日志 */
#define APP_TAG "PULSEOX_ESP"

/**
 * @brief SD 接线测试模式开关
 *
 * 设为 1 时，固件启动后只循环执行 SD 卡接线检测与文件读写验证，
 * 不会启动 WiFi/MQTT/UART 桥接等正常业务逻辑。
 *
 * 设为 0 时，固件以正常桥接模式运行（串口透传 + MQTT 上报 + SD 日志）。
 *
 * @note 正式部署前必须将此项改为 0，否则模块只会重复 SD 测试流程。
 */
#define APP_MODE_SD_WIRING_TEST 0

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

/**
 * @brief MQTT 上报 JSON 中的桥接标识字段
 *
 * 每一帧上报 JSON 都会携带 bridge/source/channel/protocol 四个元信息字段，
 * 便于云端区分数据来源、通道与协议版本。
 */
#define MQTT_BRIDGE_NAME    "esp32c3"
#define MQTT_SOURCE_NAME    "stm32"
#define MQTT_CHANNEL_NAME   "uart1"
#define MQTT_PROTOCOL_NAME  "stm32-compact-v1"

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
/** @brief UART 收发缓冲区大小（字节），同时作为单帧文本最大长度 */
#define UART_BUF_SIZE   512

/** @brief UART 发送队列长度（可缓存的待发送 MQTT 下行命令数） */
#define UART_TX_QUEUE_LENGTH 8
/** @brief UART 收发任务的栈大小（字节） */
#define UART_TASK_STACK_SIZE 4096
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
 * FreeRTOS 事件位定义
 * ========================================================================== */

/** @brief WiFi 已连接并获得 IP 的事件位 */
#define APP_WIFI_CONNECTED_BIT BIT0
/** @brief MQTT 已成功连接 broker 的事件位 */
#define APP_MQTT_CONNECTED_BIT BIT1

/* ==========================================================================
 * SD 卡硬件引脚与测试参数
 * ========================================================================== */

/** @brief SD 卡 FAT 文件系统挂载点路径 */
#define SD_MOUNT_POINT "/sdcard"
/** @brief SD 卡 SPI MOSI 引脚 */
#define SD_PIN_MOSI    4
/** @brief SD 卡 SPI MISO 引脚 */
#define SD_PIN_MISO    6
/** @brief SD 卡 SPI SCK 时钟引脚 */
#define SD_PIN_SCK     5
/** @brief SD 卡 SPI CS 片选引脚 */
#define SD_PIN_CS      10

/** @brief SD 接线测试模式下的 SPI 频率（kHz），较低频率可提高接线不良时的容错 */
#define SD_TEST_SPI_FREQ_KHZ     400
/**
 * @brief SD 接线测试模式下，检测失败后重试间隔（毫秒）
 *
 * 仅用于接线测试循环；正式桥接模式的 SD 重试策略由 SD 监控任务独立控制。
 */
#define SD_TEST_RETRY_DELAY_MS   3000
/**
 * @brief SD 接线测试模式下，全部检测通过后的成功展示等待时间（毫秒）
 *
 * 测试全部通过后不会立即进入下一次循环，而是等待此时间让开发者确认结果。
 */
#define SD_TEST_SUCCESS_DELAY_MS 10000

/* ==========================================================================
 * SD 卡日志模块参数
 * ========================================================================== */

/** @brief SD 卡监控任务的栈大小（字节） */
#define SD_MONITOR_TASK_STACK_SIZE 4096
/** @brief SD 卡监控任务的 FreeRTOS 优先级 */
#define SD_MONITOR_TASK_PRIORITY   4
/** @brief SD 日志写入任务的栈大小（字节） */
#define SD_LOG_TASK_STACK_SIZE     4096
/** @brief SD 日志写入任务的 FreeRTOS 优先级 */
#define SD_LOG_TASK_PRIORITY       4
/** @brief SD 日志写入队列长度（可缓存的待写入日志行数） */
#define SD_LOG_QUEUE_LENGTH        32

/**
 * @brief SD 日志文件批量刷盘的最大间隔（毫秒）
 *
 * 即使未达到刷盘行数阈值，达到此时间后也会强制 fflush 一次，
 * 避免长时间无新数据时已缓冲内容丢失。
 */
#define SD_LOG_FLUSH_INTERVAL_MS   1000
/**
 * @brief SD 日志文件批量刷盘的行数阈值
 *
 * 每写入这么多行 UART 数据后自动 fflush 一次，用于在写入频率和掉电安全之间平衡。
 */
#define SD_LOG_FLUSH_LINE_BATCH    8
/**
 * @brief SD 卡挂载失败后的首次重试等待时间（毫秒）
 *
 * 连续失败后按 2 倍退避增长，直至达到 SD_MOUNT_RETRY_MAX_MS。
 */
#define SD_MOUNT_RETRY_INITIAL_MS  5000
/** @brief SD 卡挂载失败重试的最大等待时间（毫秒） */
#define SD_MOUNT_RETRY_MAX_MS      60000
/**
 * @brief SD 卡已挂载时的状态轮询间隔（毫秒）
 *
 * 监控任务以此周期检查 SD 卡是否仍在线，发现移除后立即卸载文件系统。
 */
#define SD_STATUS_CHECK_MS         1000
/**
 * @brief SD 卡监控任务上电后的初始延迟（毫秒）
 *
 * 等待硬件电源稳定后再开始第一次挂载尝试，避免因上电瞬间的电压波动导致误判。
 */
#define SD_STARTUP_DELAY_MS        3000

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
 * 状态任务会以此周期输出包含 WiFi/MQTT/SD/UART/协议 等子系统状态的汇总日志。
 */
#define APP_STATUS_REPORT_PERIOD_MS 10000

#endif
