#ifndef STM32_PROTOCOL_H
#define STM32_PROTOCOL_H

/**
 * @brief STM32 协议帧解析结果的枚举。
 *
 * 每一帧来自 STM32 的 UART 文本在解析后都会产生以下结果之一，
 * 用于驱动运行状态计数器并帮助定位是"正常数据"还是"格式异常"。
 */
typedef enum {
    /** 尚未设置结果（函数入口初始值） */
    STM32_PROTOCOL_RESULT_NONE = 0,
    /** 成功解析一帧测量数据（帧类型 "M"）并生成了 JSON */
    STM32_PROTOCOL_RESULT_MEASUREMENT,
    /** 成功解析一帧 RTC 时间设置应答（帧类型 "T"）并生成了 JSON */
    STM32_PROTOCOL_RESULT_TIME_ACK,
    /** 帧格式不符合协议约定，已生成 parse_error JSON */
    STM32_PROTOCOL_RESULT_PARSE_ERROR,
    /** 内存不足，JSON 分配失败（malloc 返回 NULL） */
    STM32_PROTOCOL_RESULT_NO_MEMORY,
} stm32_protocol_result_t;

/**
 * @brief 解析 STM32 文本帧并生成对应的 MQTT 上报 JSON。
 *
 * 函数将输入的 CSV 文本帧按首字段判断类型，分派到测量帧解析或应答帧解析，
 * 最后生成包含 bridge/source/channel/protocol 等元信息的单行 JSON。
 *
 * @param line   来自 STM32 的原始单行文本帧（以 '\0' 结尾）。
 * @param result [可选输出] 解析结果枚举，用于调用方的统计/诊断。
 *               传入 NULL 表示不关心结果类型。
 *
 * @return 成功时返回新分配的 JSON 字符串（调用方需 free）；
 *         解析失败时返回 parse_error JSON（仍为有效 JSON）；
 *         内存不足时返回 NULL。
 */
char *stm32_protocol_build_publish_json(const char *line, stm32_protocol_result_t *result);

#endif
