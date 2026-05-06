#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "stm32_protocol.h"

/**
 * @brief 协议字段数上限与各帧类型的期望字段数。
 *
 * 帧类型标识:
 *  - M (Measurement): 测量帧，包含红光/红外/心率/血氧等传感器数据
 *  - T (Time Ack):     RTC 时间设置应答帧
 */
enum {
    UART_FIELD_MAX = 16,       /**< 单帧允许的最大字段数（含帧类型标识） */
    UART_M_FIELD_COUNT = 12,   /**< 测量帧 M 的期望字段数 */
    UART_T_FIELD_COUNT = 6,    /**< 时间应答帧 T 的期望字段数 */
};

/**
 * @brief 测量帧 M 解析后的结构化数据。
 *
 * 来自 STM32 的 CSV 文本格式: M,rtc_valid,YYYYMMDD,HHMMSS,red,ir,baseline_ir,finger,bpm_valid,bpm,spo2_valid,spo2
 */
typedef struct {
    bool rtc_valid;      /**< RTC 时钟是否有效 */
    char date[9];        /**< 日期 YYYYMMDD（含结尾 '\0'） */
    char time[7];        /**< 时间 HHMMSS（含结尾 '\0'） */
    uint32_t red;        /**< 红光 ADC 原始值 */
    uint32_t ir;         /**< 红外 ADC 原始值 */
    uint32_t baseline_ir;/**< 红外基线值 */
    bool finger;         /**< 手指是否在传感器上（1=在, 0=不在） */
    bool bpm_valid;      /**< 心率值是否有效 */
    uint32_t bpm;        /**< 心率值（次/分钟） */
    bool spo2_valid;     /**< 血氧值是否有效 */
    uint32_t spo2;       /**< 血氧饱和度值（%） */
} stm32_measurement_t;

/**
 * @brief RTC 时间设置应答帧 T 解析后的结构化数据。
 *
 * 来自 STM32 的 CSV 文本格式: T,set_ok,rtc_valid,YYYYMMDD,HHMMSS,reason
 */
typedef struct {
    bool set_ok;         /**< RTC 设置是否成功 */
    bool rtc_valid;      /**< 设置后 RTC 是否有效 */
    char date[9];        /**< 日期 YYYYMMDD（含结尾 '\0'） */
    char time[7];        /**< 时间 HHMMSS（含结尾 '\0'） */
    const char *reason;  /**< 设置结果/失败原因文本（直接引用拆分后的字段，不复制） */
} stm32_time_ack_t;

static const char *json_bool(bool value);
static size_t json_escaped_length(const char *text);
static char *append_json_escaped(char *dst, const char *text);
static char *copy_json_escaped(const char *text);
static bool parse_bool_token(const char *text, bool *value);
static bool parse_u32_token(const char *text, uint32_t *value);
static bool copy_fixed_digits(char *dst, size_t dst_size, const char *src, size_t digits);
static size_t split_csv_fields(char *text, char *fields[], size_t max_fields);
static bool parse_measurement_frame(char *fields[], size_t field_count, stm32_measurement_t *msg);
static bool parse_time_ack_frame(char *fields[], size_t field_count, stm32_time_ack_t *ack);
static char *build_measurement_json(const stm32_measurement_t *msg);
static char *build_time_ack_json(const stm32_time_ack_t *ack);
static char *build_parse_error_json(const char *line, const char *error);

/**
 * @brief 解析 STM32 文本帧并生成统一 JSON。
 *
 * 函数会先复制输入文本并按逗号拆字段，然后根据首字段判断是测量帧 `M`、
 * 时间设置应答帧 `T` 还是非法输入，最后构造对应 JSON 或解析错误 JSON。
 *
 * @param line 来自 STM32 的原始单行文本帧。
 *
 * @return 成功时返回新分配的 JSON 字符串；内存不足或输入非法时返回 NULL 或错误 JSON。
 */
char *stm32_protocol_build_publish_json(const char *line, stm32_protocol_result_t *result)
{
    char line_copy[UART_BUF_SIZE];
    char *fields[UART_FIELD_MAX];
    stm32_measurement_t measurement;
    stm32_time_ack_t time_ack;
    size_t field_count;
    size_t line_len;
    char *json = NULL;

    if (result != NULL) {
        *result = STM32_PROTOCOL_RESULT_NONE;
    }

    if (line == NULL) {
        return NULL;
    }

    line_len = strlen(line);
    if (line_len >= sizeof(line_copy)) {
        json = build_parse_error_json(line, "line_too_long");
        if (result != NULL) {
            *result = (json != NULL) ? STM32_PROTOCOL_RESULT_PARSE_ERROR
                                     : STM32_PROTOCOL_RESULT_NO_MEMORY;
        }
        return json;
    }

    memcpy(line_copy, line, line_len + 1U);
    field_count = split_csv_fields(line_copy, fields, UART_FIELD_MAX);

    if (field_count == 0) {
        json = build_parse_error_json(line, "empty_frame");
        if (result != NULL) {
            *result = (json != NULL) ? STM32_PROTOCOL_RESULT_PARSE_ERROR
                                     : STM32_PROTOCOL_RESULT_NO_MEMORY;
        }
        return json;
    }

    if (field_count > UART_FIELD_MAX) {
        json = build_parse_error_json(line, "too_many_fields");
        if (result != NULL) {
            *result = (json != NULL) ? STM32_PROTOCOL_RESULT_PARSE_ERROR
                                     : STM32_PROTOCOL_RESULT_NO_MEMORY;
        }
        return json;
    }

    if (strcmp(fields[0], "M") == 0) {
        if (!parse_measurement_frame(fields, field_count, &measurement)) {
            json = build_parse_error_json(line, "bad_measurement_frame");
            if (result != NULL) {
                *result = (json != NULL) ? STM32_PROTOCOL_RESULT_PARSE_ERROR
                                         : STM32_PROTOCOL_RESULT_NO_MEMORY;
            }
            return json;
        }

        json = build_measurement_json(&measurement);
        if (result != NULL) {
            *result = (json != NULL) ? STM32_PROTOCOL_RESULT_MEASUREMENT
                                     : STM32_PROTOCOL_RESULT_NO_MEMORY;
        }
        return json;
    }

    if (strcmp(fields[0], "T") == 0) {
        if (!parse_time_ack_frame(fields, field_count, &time_ack)) {
            json = build_parse_error_json(line, "bad_time_ack_frame");
            if (result != NULL) {
                *result = (json != NULL) ? STM32_PROTOCOL_RESULT_PARSE_ERROR
                                         : STM32_PROTOCOL_RESULT_NO_MEMORY;
            }
            return json;
        }

        json = build_time_ack_json(&time_ack);
        if (result != NULL) {
            *result = (json != NULL) ? STM32_PROTOCOL_RESULT_TIME_ACK
                                     : STM32_PROTOCOL_RESULT_NO_MEMORY;
        }
        return json;
    }

    json = build_parse_error_json(line, "unknown_frame_type");
    if (result != NULL) {
        *result = (json != NULL) ? STM32_PROTOCOL_RESULT_PARSE_ERROR
                                 : STM32_PROTOCOL_RESULT_NO_MEMORY;
    }
    return json;
}

/**
 * @brief 把布尔值转换成 JSON 文本形式。
 *
 * 该辅助函数统一返回 JSON 需要的小写字面量，避免在多个格式化函数中重复写
 * 条件表达式。
 *
 * @param value 待转换的布尔值。
 *
 * @return `"true"` 或 `"false"` 常量字符串。
 */
static const char *json_bool(bool value)
{
    return value ? "true" : "false";
}

/**
 * @brief 计算字符串在 JSON 转义后的长度。
 *
 * 该长度用于提前分配足够的内存空间。函数会考虑引号、反斜杠、控制字符等
 * 需要额外转义的情况。
 *
 * @param text 原始文本，可为 NULL。
 *
 * @return 转义后不含结尾 `\0` 的字符长度。
 */
static size_t json_escaped_length(const char *text)
{
    size_t len = 0;

    while (text != NULL && *text != '\0') {
        unsigned char ch = (unsigned char)*text++;

        switch (ch) {
            case '\"':
            case '\\':
            case '\b':
            case '\f':
            case '\n':
            case '\r':
            case '\t':
                len += 2;
                break;

            default:
                len += (ch < 0x20) ? 6 : 1;
                break;
        }
    }

    return len;
}

/**
 * @brief 把字符串按 JSON 规则转义并追加到目标缓冲区。
 *
 * 该函数假设调用方已经为 `dst` 预留了足够空间，会把 `text` 中需要转义的字符
 * 转成 JSON 可接受的形式，并在末尾补上字符串结束符。
 *
 * @param dst 目标写入位置。
 * @param text 待转义的原始字符串。
 *
 * @return 指向写入结束位置的指针，便于继续追加内容。
 */
static char *append_json_escaped(char *dst, const char *text)
{
    static const char hex[] = "0123456789abcdef";

    while (text != NULL && *text != '\0') {
        unsigned char ch = (unsigned char)*text++;

        switch (ch) {
            case '\"':
                *dst++ = '\\';
                *dst++ = '\"';
                break;

            case '\\':
                *dst++ = '\\';
                *dst++ = '\\';
                break;

            case '\b':
                *dst++ = '\\';
                *dst++ = 'b';
                break;

            case '\f':
                *dst++ = '\\';
                *dst++ = 'f';
                break;

            case '\n':
                *dst++ = '\\';
                *dst++ = 'n';
                break;

            case '\r':
                *dst++ = '\\';
                *dst++ = 'r';
                break;

            case '\t':
                *dst++ = '\\';
                *dst++ = 't';
                break;

            default:
                if (ch < 0x20) {
                    *dst++ = '\\';
                    *dst++ = 'u';
                    *dst++ = '0';
                    *dst++ = '0';
                    *dst++ = hex[(ch >> 4) & 0x0F];
                    *dst++ = hex[ch & 0x0F];
                } else {
                    *dst++ = (char)ch;
                }
                break;
        }
    }

    *dst = '\0';
    return dst;
}

/**
 * @brief 复制并返回一份 JSON 转义后的字符串。
 *
 * 这是对长度计算和实际转义的封装，适合在构造 JSON 时处理包含任意字符的
 * 原始字段内容。
 *
 * @param text 原始字符串。
 *
 * @return 成功返回新分配的转义字符串，失败返回 NULL。
 */
static char *copy_json_escaped(const char *text)
{
    size_t len = json_escaped_length(text);
    char *escaped = malloc(len + 1U);

    if (escaped == NULL) {
        return NULL;
    }

    append_json_escaped(escaped, text);
    return escaped;
}

/**
 * @brief 把单字符的 `0/1` 文本解析成布尔值。
 *
 * 协议里布尔字段使用单字符数字表示，这里做严格校验，避免把其他字符串误判为
 * 有效布尔值。
 *
 * @param text 待解析文本。
 * @param value 解析成功后输出结果。
 *
 * @return true 表示解析成功，false 表示格式不合法。
 */
static bool parse_bool_token(const char *text, bool *value)
{
    if (text == NULL || value == NULL || text[0] == '\0' || text[1] != '\0') {
        return false;
    }

    if (text[0] == '0') {
        *value = false;
        return true;
    }

    if (text[0] == '1') {
        *value = true;
        return true;
    }

    return false;
}

/**
 * @brief 把十进制无符号整数字符串解析为 `uint32_t`。
 *
 * 解析过程中会逐位累加，并显式检查非法字符和 32 位无符号溢出，
 * 防止异常输入污染后续业务数据。
 *
 * @param text 待解析文本。
 * @param value 解析成功后输出数值。
 *
 * @return true 表示解析成功，false 表示输入为空、含非数字或发生溢出。
 */
static bool parse_u32_token(const char *text, uint32_t *value)
{
    uint32_t parsed = 0;

    if (text == NULL || value == NULL || text[0] == '\0') {
        return false;
    }

    while (*text != '\0') {
        uint32_t digit;

        if (*text < '0' || *text > '9') {
            return false;
        }

        digit = (uint32_t)(*text - '0');
        if (parsed > (UINT32_MAX - digit) / 10U) {
            return false;
        }

        parsed = parsed * 10U + digit;
        ++text;
    }

    *value = parsed;
    return true;
}

/**
 * @brief 复制并校验固定长度数字字段。
 *
 * 该函数主要用于日期和时间字段，要求源字符串长度必须与 `digits` 完全一致，
 * 且每一位都必须是数字字符。
 *
 * @param dst 目标缓冲区。
 * @param dst_size 目标缓冲区大小。
 * @param src 源字符串。
 * @param digits 期望的数字位数。
 *
 * @return true 表示复制成功，false 表示参数非法或格式不匹配。
 */
static bool copy_fixed_digits(char *dst, size_t dst_size, const char *src, size_t digits)
{
    size_t i;

    if (dst == NULL || src == NULL || dst_size < digits + 1U) {
        return false;
    }

    for (i = 0; i < digits; ++i) {
        if (src[i] < '0' || src[i] > '9') {
            return false;
        }

        dst[i] = src[i];
    }

    if (src[digits] != '\0') {
        return false;
    }

    dst[digits] = '\0';
    return true;
}

/**
 * @brief 原地拆分逗号分隔的文本字段。
 *
 * 函数会把原始文本中的逗号替换为 `\0`，并把每个字段起始地址填入 `fields` 数组。
 * 如果字段数量超过上限，会返回 `max_fields + 1` 供上层识别“字段过多”。
 *
 * @param text 待拆分文本，内容会被原地修改。
 * @param fields 输出字段指针数组。
 * @param max_fields 允许的最大字段数。
 *
 * @return 拆分得到的字段数；若超限则返回 `max_fields + 1`。
 */
static size_t split_csv_fields(char *text, char *fields[], size_t max_fields)
{
    size_t count = 0;
    char *cursor = text;

    if (text == NULL || fields == NULL || max_fields == 0 || text[0] == '\0') {
        return 0;
    }

    fields[count++] = cursor;

    while (*cursor != '\0') {
        if (*cursor == ',') {
            *cursor = '\0';

            if (count >= max_fields) {
                return max_fields + 1U;
            }

            fields[count++] = cursor + 1;
        }

        ++cursor;
    }

    return count;
}

/**
 * @brief 解析测量帧 `M` 的字段内容。
 *
 * 测量帧包含 RTC 状态、日期时间、红光/红外值、手指检测、心率和血氧等信息。
 * 只有字段数和每个字段内容都完全合法时才会返回成功。
 *
 * @param fields 已拆分的字段数组。
 * @param field_count 字段数量。
 * @param msg 解析成功后的输出结构体。
 *
 * @return true 表示帧格式正确，false 表示字段数或字段内容不合法。
 */
static bool parse_measurement_frame(char *fields[], size_t field_count, stm32_measurement_t *msg)
{
    if (fields == NULL || msg == NULL || field_count != UART_M_FIELD_COUNT) {
        return false;
    }

    return parse_bool_token(fields[1], &msg->rtc_valid) &&
           copy_fixed_digits(msg->date, sizeof(msg->date), fields[2], 8U) &&
           copy_fixed_digits(msg->time, sizeof(msg->time), fields[3], 6U) &&
           parse_u32_token(fields[4], &msg->red) &&
           parse_u32_token(fields[5], &msg->ir) &&
           parse_u32_token(fields[6], &msg->baseline_ir) &&
           parse_bool_token(fields[7], &msg->finger) &&
           parse_bool_token(fields[8], &msg->bpm_valid) &&
           parse_u32_token(fields[9], &msg->bpm) &&
           parse_bool_token(fields[10], &msg->spo2_valid) &&
           parse_u32_token(fields[11], &msg->spo2);
}

/**
 * @brief 解析 RTC 设置应答帧 `T`。
 *
 * 应答帧包含设置是否成功、RTC 当前是否有效、日期时间以及失败原因等字段，
 * 解析成功后不会复制 `reason` 文本，而是直接引用拆分后的原始字段。
 *
 * @param fields 已拆分的字段数组。
 * @param field_count 字段数量。
 * @param ack 解析结果输出结构体。
 *
 * @return true 表示应答帧格式正确，false 表示内容不合法。
 */
static bool parse_time_ack_frame(char *fields[], size_t field_count, stm32_time_ack_t *ack)
{
    if (fields == NULL || ack == NULL || field_count != UART_T_FIELD_COUNT) {
        return false;
    }

    if (!parse_bool_token(fields[1], &ack->set_ok) ||
        !parse_bool_token(fields[2], &ack->rtc_valid) ||
        !copy_fixed_digits(ack->date, sizeof(ack->date), fields[3], 8U) ||
        !copy_fixed_digits(ack->time, sizeof(ack->time), fields[4], 6U) ||
        fields[5] == NULL || fields[5][0] == '\0') {
        return false;
    }

    ack->reason = fields[5];
    return true;
}

/**
 * @brief 根据测量结构体构造上报 JSON。
 *
 * 函数会把公共桥接元信息和测量字段一起格式化成单行 JSON，
 * 供 MQTT 上行直接发布。
 *
 * @param msg 已解析完成的测量数据。
 *
 * @return 成功返回新分配的 JSON 字符串，失败返回 NULL。
 */
static char *build_measurement_json(const stm32_measurement_t *msg)
{
    int needed;
    char *json;

    if (msg == NULL) {
        return NULL;
    }

    needed = snprintf(NULL,
                      0,
                      "{\"bridge\":\"%s\",\"source\":\"%s\",\"channel\":\"%s\","
                      "\"protocol\":\"%s\",\"message\":\"measurement\",\"frame\":\"M\","
                      "\"rtc_valid\":%s,\"date\":\"%s\",\"time\":\"%s\","
                      "\"red\":%lu,\"ir\":%lu,\"baseline_ir\":%lu,"
                      "\"finger\":%s,\"bpm_valid\":%s,\"bpm\":%lu,"
                      "\"spo2_valid\":%s,\"spo2\":%lu}",
                      MQTT_BRIDGE_NAME,
                      MQTT_SOURCE_NAME,
                      MQTT_CHANNEL_NAME,
                      MQTT_PROTOCOL_NAME,
                      json_bool(msg->rtc_valid),
                      msg->date,
                      msg->time,
                      (unsigned long)msg->red,
                      (unsigned long)msg->ir,
                      (unsigned long)msg->baseline_ir,
                      json_bool(msg->finger),
                      json_bool(msg->bpm_valid),
                      (unsigned long)msg->bpm,
                      json_bool(msg->spo2_valid),
                      (unsigned long)msg->spo2);
    if (needed < 0) {
        return NULL;
    }

    json = malloc((size_t)needed + 1U);
    if (json == NULL) {
        return NULL;
    }

    snprintf(json,
             (size_t)needed + 1U,
             "{\"bridge\":\"%s\",\"source\":\"%s\",\"channel\":\"%s\","
             "\"protocol\":\"%s\",\"message\":\"measurement\",\"frame\":\"M\","
             "\"rtc_valid\":%s,\"date\":\"%s\",\"time\":\"%s\","
             "\"red\":%lu,\"ir\":%lu,\"baseline_ir\":%lu,"
             "\"finger\":%s,\"bpm_valid\":%s,\"bpm\":%lu,"
             "\"spo2_valid\":%s,\"spo2\":%lu}",
             MQTT_BRIDGE_NAME,
             MQTT_SOURCE_NAME,
             MQTT_CHANNEL_NAME,
             MQTT_PROTOCOL_NAME,
             json_bool(msg->rtc_valid),
             msg->date,
             msg->time,
             (unsigned long)msg->red,
             (unsigned long)msg->ir,
             (unsigned long)msg->baseline_ir,
             json_bool(msg->finger),
             json_bool(msg->bpm_valid),
             (unsigned long)msg->bpm,
             json_bool(msg->spo2_valid),
             (unsigned long)msg->spo2);

    return json;
}

/**
 * @brief 根据 RTC 应答结构体构造 JSON。
 *
 * 在格式化前会先对 `reason` 做 JSON 转义，确保其中即使包含引号、换行或其他
 * 特殊字符，也不会破坏最终生成的 JSON。
 *
 * @param ack 已解析完成的 RTC 设置应答数据。
 *
 * @return 成功返回新分配的 JSON 字符串，失败返回 NULL。
 */
static char *build_time_ack_json(const stm32_time_ack_t *ack)
{
    char *reason_escaped;
    char *json;
    int needed;

    if (ack == NULL || ack->reason == NULL) {
        return NULL;
    }

    reason_escaped = copy_json_escaped(ack->reason);
    if (reason_escaped == NULL) {
        return NULL;
    }

    needed = snprintf(NULL,
                      0,
                      "{\"bridge\":\"%s\",\"source\":\"%s\",\"channel\":\"%s\","
                      "\"protocol\":\"%s\",\"message\":\"rtc_set_ack\",\"frame\":\"T\","
                      "\"set_ok\":%s,\"rtc_valid\":%s,\"date\":\"%s\",\"time\":\"%s\","
                      "\"reason\":\"%s\"}",
                      MQTT_BRIDGE_NAME,
                      MQTT_SOURCE_NAME,
                      MQTT_CHANNEL_NAME,
                      MQTT_PROTOCOL_NAME,
                      json_bool(ack->set_ok),
                      json_bool(ack->rtc_valid),
                      ack->date,
                      ack->time,
                      reason_escaped);
    if (needed < 0) {
        free(reason_escaped);
        return NULL;
    }

    json = malloc((size_t)needed + 1U);
    if (json == NULL) {
        free(reason_escaped);
        return NULL;
    }

    snprintf(json,
             (size_t)needed + 1U,
             "{\"bridge\":\"%s\",\"source\":\"%s\",\"channel\":\"%s\","
             "\"protocol\":\"%s\",\"message\":\"rtc_set_ack\",\"frame\":\"T\","
             "\"set_ok\":%s,\"rtc_valid\":%s,\"date\":\"%s\",\"time\":\"%s\","
             "\"reason\":\"%s\"}",
             MQTT_BRIDGE_NAME,
             MQTT_SOURCE_NAME,
             MQTT_CHANNEL_NAME,
             MQTT_PROTOCOL_NAME,
             json_bool(ack->set_ok),
             json_bool(ack->rtc_valid),
             ack->date,
             ack->time,
             reason_escaped);

    free(reason_escaped);
    return json;
}

/**
 * @brief 构造解析失败时的错误 JSON。
 *
 * 当协议帧无法识别或字段内容非法时，该函数会把错误原因和原始文本一起打包，
 * 方便云端观察串口异常输入。
 *
 * @param line 原始串口文本。
 * @param error 解析失败原因标识。
 *
 * @return 成功返回新分配的 JSON 字符串，失败返回 NULL。
 */
static char *build_parse_error_json(const char *line, const char *error)
{
    char *raw_escaped;
    char *error_escaped;
    char *json;
    int needed;

    if (line == NULL || error == NULL) {
        return NULL;
    }

    raw_escaped = copy_json_escaped(line);
    error_escaped = copy_json_escaped(error);
    if (raw_escaped == NULL || error_escaped == NULL) {
        free(raw_escaped);
        free(error_escaped);
        return NULL;
    }

    needed = snprintf(NULL,
                      0,
                      "{\"bridge\":\"%s\",\"source\":\"%s\",\"channel\":\"%s\","
                      "\"protocol\":\"%s\",\"message\":\"parse_error\",\"frame\":\"?\","
                      "\"error\":\"%s\",\"raw\":\"%s\"}",
                      MQTT_BRIDGE_NAME,
                      MQTT_SOURCE_NAME,
                      MQTT_CHANNEL_NAME,
                      MQTT_PROTOCOL_NAME,
                      error_escaped,
                      raw_escaped);
    if (needed < 0) {
        free(raw_escaped);
        free(error_escaped);
        return NULL;
    }

    json = malloc((size_t)needed + 1U);
    if (json == NULL) {
        free(raw_escaped);
        free(error_escaped);
        return NULL;
    }

    snprintf(json,
             (size_t)needed + 1U,
             "{\"bridge\":\"%s\",\"source\":\"%s\",\"channel\":\"%s\","
             "\"protocol\":\"%s\",\"message\":\"parse_error\",\"frame\":\"?\","
             "\"error\":\"%s\",\"raw\":\"%s\"}",
             MQTT_BRIDGE_NAME,
             MQTT_SOURCE_NAME,
             MQTT_CHANNEL_NAME,
             MQTT_PROTOCOL_NAME,
             error_escaped,
             raw_escaped);

    free(raw_escaped);
    free(error_escaped);
    return json;
}
