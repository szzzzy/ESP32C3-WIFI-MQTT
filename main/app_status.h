#ifndef APP_STATUS_H
#define APP_STATUS_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "stm32_protocol.h"

/* ==========================================================================
 * 模块: 运行状态监控
 *
 * 本模块提供线程安全的全局运行状态记录与周期性上报功能。
 *
 * 核心职责:
 *   1. 各子系统在状态变化时通过 set_/note_ 系列接口更新快照
 *   2. 内置 FreeRTOS 后台任务，按 APP_STATUS_REPORT_PERIOD_MS 周期输出全量状态日志
 *   3. 发生协议错误、MQTT 断连等关键事件时立即触发状态快照输出
 *
 * 线程安全:
 *   所有 set_/note_ 接口内部使用互斥锁保护共享状态，可在中断/任务/回调中安全调用。
 *   copy_snapshot 使用"持锁复制"模式，读出的快照是瞬时一致的副本。
 *
 * 依赖:
 *   - app_config.h: 任务栈大小、优先级和上报周期配置
 *   - stm32_protocol.h: stm32_protocol_result_t 枚举定义
 * ========================================================================== */

/* --------------------------------------------------------------------------
 * 生命周期管理
 * -------------------------------------------------------------------------- */

/**
 * @brief 初始化状态监控模块。
 *
 * 创建保护全局状态快照的互斥锁。在创建状态后台任务之前必须先调用此接口。
 *
 * @return
 *      - ESP_OK: 互斥锁创建成功
 *      - ESP_ERR_NO_MEM: FreeRTOS 互斥锁创建失败（内存不足）
 */
esp_err_t app_status_init(void);

/**
 * @brief 启动状态定期打印后台任务。
 *
 * 任务按 APP_STATUS_REPORT_PERIOD_MS 间隔循环输出全量状态快照。
 * 重复调用安全；如果任务已存在则直接返回成功。
 *
 * @return
 *      - ESP_OK: 任务已启动或已存在
 *      - ESP_ERR_INVALID_STATE: 模块尚未初始化（互斥锁不存在）
 *      - ESP_FAIL: 任务创建失败
 */
esp_err_t app_status_start_task(void);

/**
 * @brief 立即输出一次完整的状态快照日志。
 *
 * 该接口不需要先持有互斥锁，内部会自行加锁复制一份快照后输出。
 * 适合在系统启动完成、关键事件发生后调用，作为"检查点"记录。
 *
 * @param reason 快照原因标签，会嵌入日志以区分调用来源（如 "boot", "app_ready", "protocol_error"）。
 *               为 NULL 或空字符串时默认显示 "snapshot"。
 */
void app_status_log_snapshot(const char *reason);

/* --------------------------------------------------------------------------
 * WiFi 状态更新
 * -------------------------------------------------------------------------- */

/** @brief 记录 WiFi STA 是否已启动（WiFi 栈已开始运行） */
void app_status_set_wifi_started(bool started);
/** @brief 记录 WiFi 是否已成功连接到 AP 并获取 IP */
void app_status_set_wifi_connected(bool connected);

/* --------------------------------------------------------------------------
 * MQTT 状态更新
 * -------------------------------------------------------------------------- */

/** @brief 记录 MQTT 客户端实例是否已创建并启动 */
void app_status_set_mqtt_started(bool started);
/**
 * @brief 记录 MQTT 是否已连接到 broker。
 *
 * 断开连接时会自动将 mqtt_subscribed 也置为 false，
 * 因为重连后需要重新订阅主题。
 */
void app_status_set_mqtt_connected(bool connected);
/** @brief 记录 MQTT 是否已成功订阅下行命令主题 */
void app_status_set_mqtt_subscribed(bool subscribed);

/* --------------------------------------------------------------------------
 * SD 卡状态更新
 * -------------------------------------------------------------------------- */

/** @brief 记录 SD SPI 总线是否初始化成功 */
void app_status_set_sd_bus_ready(bool ready);
/** @brief 记录 SD 卡热插拔监控任务是否已运行 */
void app_status_set_sd_monitor_running(bool running);
/** @brief 记录 SD 卡文件系统是否已挂载并可读写 */
void app_status_set_sd_mounted(bool mounted);

/* --------------------------------------------------------------------------
 * UART 状态更新
 * -------------------------------------------------------------------------- */

/** @brief 记录 UART 驱动是否已安装并配置完成 */
void app_status_set_uart_ready(bool ready);
/** @brief 记录 UART 发送任务是否已运行 */
void app_status_set_uart_tx_task_running(bool running);
/** @brief 记录 UART 接收任务是否已运行 */
void app_status_set_uart_rx_task_running(bool running);

/* --------------------------------------------------------------------------
 * UART 收发计数器（接收侧在快速路径中调用）
 * -------------------------------------------------------------------------- */

/** @brief 递增 UART 接收到的完整文本行计数 */
void app_status_note_uart_rx_line(void);
/** @brief 递增 UART 接收行丢弃计数（单行过长，超过 UART_BUF_SIZE） */
void app_status_note_uart_rx_drop(void);
/** @brief 递增 UART 发送队列入队成功计数 */
void app_status_note_uart_tx_queued(void);
/**
 * @brief 记录一次 UART 发送结果。
 *
 * @param ok true 表示发送 + 等待 TX 完成全部成功，false 表示任一步骤失败。
 */
void app_status_note_uart_tx_result(bool ok);

/* --------------------------------------------------------------------------
 * MQTT 收发计数器
 * -------------------------------------------------------------------------- */

/**
 * @brief 记录一次 MQTT 发布结果。
 *
 * @param ok true 表示发布成功（得到非负 msg_id），false 表示发布失败或连接未就绪。
 */
void app_status_note_mqtt_publish(bool ok);
/** @brief 递增 MQTT 下行命令收到计数（每次收到匹配 topic 的数据帧） */
void app_status_note_mqtt_command(void);

/* --------------------------------------------------------------------------
 * SD 卡日志计数器
 * -------------------------------------------------------------------------- */

/**
 * @brief 记录一次 SD 日志写入结果。
 *
 * @param ok true 表示 fwrite 成功，false 表示写入、flush、打开文件等 I/O 操作失败。
 */
void app_status_note_sd_write(bool ok);
/**
 * @brief 递增 SD 日志丢弃计数。
 *
 * 该接口用于上报"快速路径"中的入队丢弃数量——串口接收侧为了非阻塞，
 * 先用原子计数累计，再由 SD 写入任务批量同步到本模块。
 *
 * @param count 需要累加的丢弃条数。
 */
void app_status_note_sd_drop(uint32_t count);

/* --------------------------------------------------------------------------
 * 协议解析计数器
 * -------------------------------------------------------------------------- */

/**
 * @brief 记录一次 STM32 协议帧的解析结果。
 *
 * 测量帧（M）和应答帧（T）记为成功；解析错误或内存不足记为失败。
 * 遇到解析错误或内存不足时会立即触发一次状态快照输出，便于尽早发现问题。
 *
 * @param result 协议解析结果枚举。
 */
void app_status_note_protocol_result(stm32_protocol_result_t result);

#endif
