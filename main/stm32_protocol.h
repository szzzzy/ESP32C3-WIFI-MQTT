#ifndef STM32_PROTOCOL_H
#define STM32_PROTOCOL_H

#include <stdint.h>

/**
 * @brief STM32 协议帧解析结果枚举。
 *
 * 主入口 stm32_protocol_build_publish_json() 解析完一行串口文本后，
 * 通过该枚举告知调用方当前帧的类型或失败原因，便于上层统计和日志分类。
 */
typedef enum {
    STM32_PROTOCOL_RESULT_NONE = 0,        /**< 未解析或无效输入 */
    STM32_PROTOCOL_RESULT_MEASUREMENT,     /**< 成功解析测量帧（M 帧），最多 110 个字段 */
    STM32_PROTOCOL_RESULT_TIME_ACK,        /**< 成功解析时间应答帧（T 帧），6 个字段 */
    STM32_PROTOCOL_RESULT_PARSE_ERROR,     /**< 解析失败：格式错误、字段非法或未知帧类型 */
    STM32_PROTOCOL_RESULT_NO_MEMORY,       /**< 内存分配失败（malloc/realloc 返回 NULL） */
} stm32_protocol_result_t;

/**
 * @brief 将一行 STM32 串口文本构建为 MQTT 发布的 JSON 负载。
 *
 * 这是协议模块的主入口函数。它接收从串口收到的完整文本行（已去除 \\r\\n），
 * 根据首字段判断帧类型并走不同的解析路径：
 *
 * - M 帧（测量帧）：逐字段解析 110 个预定义位置，各字段独立报错、独立 available/valid 标记，
 *   最后构建包含所有模块数据的完整 JSON。某个字段缺失或格式错误不会导致整帧失败，
 *   而是记录在 parse_warnings 中。
 *
 * - T 帧（时间应答帧）：严格校验 6 字段格式，任一字段格式不符即返回 parse_error。
 *
 * - 其他帧类型或空帧：返回包含原始行和错误原因的 parse_error JSON。
 *
 * 返回的 JSON 字符串由 malloc 分配，调用方必须在用完后 free。
 *
 * @param line    串口接收到的原始文本行（以 '\0' 结尾，不含 \\r\\n）。
 * @param rx_ms   该行被接收时的 ESP32 系统毫秒时间戳。
 * @param result  输出参数，返回协议解析结果分类。可为 NULL（不关心结果时）。
 * @return        malloc 分配的 JSON 字符串，内存不足时返回 NULL。
 */
char *stm32_protocol_build_publish_json(const char *line, uint32_t rx_ms,
                                         stm32_protocol_result_t *result);

#endif
