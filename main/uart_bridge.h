#ifndef UART_BRIDGE_H
#define UART_BRIDGE_H

#include "esp_err.h"

typedef void (*uart_bridge_line_handler_t)(const char *line, uint32_t rx_ms, void *ctx);

/**
 * @brief 初始化 UART 驱动和发送队列。
 *
 * 该接口会根据配置项完成串口参数设置、引脚绑定以及发送队列创建，
 * 是启动收发任务之前必须先调用的入口。
 *
 * @return
 *      - ESP_OK: 初始化成功
 *      - ESP_ERR_NO_MEM: 队列创建失败
 *      - 其他错误码: 底层 UART 驱动初始化失败
 */
esp_err_t uart_bridge_init(void);

/**
 * @brief 启动 UART 发送任务。
 *
 * 发送任务会持续从内部队列中取出待发送命令，并顺序写入 STM32 串口。
 * 重复调用是安全的；如果任务已经存在，函数会直接返回成功。
 *
 * @return
 *      - ESP_OK: 任务已启动或已存在
 *      - ESP_FAIL: 任务创建失败
 */
esp_err_t uart_bridge_start_tx_task(void);

/**
 * @brief 启动 UART 接收任务并注册按行回调。
 *
 * 接收任务会持续读取串口字节流，按换行符切分成完整文本行，随后调用
 * 传入的处理回调，供上层执行日志记录或网络转发。
 *
 * @param handler 收到完整文本行后的回调函数。
 * @param ctx 传递给回调的用户上下文。
 *
 * @return
 *      - ESP_OK: 任务已启动或已存在
 *      - ESP_FAIL: 任务创建失败
 */
esp_err_t uart_bridge_start_rx_task(uart_bridge_line_handler_t handler, void *ctx);

/**
 * @brief 把 MQTT 下发的命令压入 UART 发送队列。
 *
 * 该接口主要给网络层调用，会把收到的 MQTT 数据复制到内部消息结构中，
 * 必要时补上换行符，再交给发送任务异步发往 STM32。
 *
 * @param payload 待发送的命令内容。
 * @param payload_len 命令字节长度。
 */
void uart_bridge_queue_command(const char *payload, int payload_len);

#endif
