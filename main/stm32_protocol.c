/**
 * ===========================================================================
 * STM32 协议解析与 JSON 构建模块
 * ===========================================================================
 *
 * 功能概述：
 *   本模块负责将 STM32 通过 UART 发送的 CSV 文本帧解析为结构化数据，并构建
 *   对应的 JSON 字符串用于 MQTT 发布。
 *
 * 支持的帧类型：
 *   - M 帧（测量帧）：102 个 CSV 字段，包含血氧/心率/HRV/ECG/PTT/传感器诊断/
 *     系统诊断/崩溃信息/任务栈/心跳等全部遥测数据。
 *   - T 帧（时间应答帧）：6 个 CSV 字段，STM32 对时间设置命令的应答。
 *
 * 解析容错策略：
 *   - 每个字段独立解析，单个字段失败（空值、格式错误、类型不匹配）不会丢弃整帧，
 *     而是将对应字段标记为 unavailable 并记录到 parse_warnings 中。
 *   - 超出 102 列的字段存入 extra_fields 数组，不透传丢弃。
 *   - 行过长（超过 LINE_COPY_SIZE）返回 parse_error 而非截断。
 *   - 半包/粘包由上层 UART 模块按 '\n' 成帧处理，本模块只接收完整行。
 *
 * JSON 输出特点：
 *   - 所有 JSON 均包含 bridge/source/channel/protocol/schema_version 元信息。
 *   - 测量帧 JSON 包含：顶层快捷字段 + modules 嵌套对象（含完整 available/valid 标记）。
 *   - 原始行保存在 raw_line 字段中，不丢失原始数据。
 *
 * 编译模式：
 *   - ESP-IDF 模式：包含 app_config.h，使用项目配置的 MQTT 元信息。
 *   - 主机测试模式（定义 STM32_PROTOCOL_HOST_TEST）：使用内联默认值，无需 ESP-IDF。
 *
 * 协议版本：stm32-compact-v2（schema_version=2），对应 STM32 102 列 M 帧。
 * ===========================================================================
 */

#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- ESP-IDF / 主机测试 编译模式选择 ---- */
#ifndef STM32_PROTOCOL_HOST_TEST
#include "app_config.h"       /* ESP-IDF 环境：MQTT 元信息和串口配置来自 Kconfig */
#else
#define MQTT_BRIDGE_NAME "esp32c3"
#define MQTT_SOURCE_NAME "stm32"
#define MQTT_CHANNEL_NAME "uart1"
#define MQTT_PROTOCOL_NAME "stm32-compact-v2"
#define UART_BUF_SIZE 512
#endif

#include "stm32_protocol.h"

/* ==========================================================================
 * 编译时常量
 * ========================================================================== */

/**
 * @brief 协议解析常量枚举
 *
 * UART_FIELD_MAX           - 指针数组容量，大于 schema 字段数以容纳超长帧
 * UART_MEASUREMENT_SCHEMA_FIELD_COUNT - M 帧的预定义字段数（列 0-101）
 * UART_T_FIELD_COUNT       - T 帧的固定字段数（T,set_ok,rtc_valid,date,time,reason）
 * PARSE_WARNING_MAX        - 单帧最多记录的解析警告条数
 * PARSE_WARNING_LEN        - 每条警告字符串的最大长度（含 '\0'）
 */
enum {
    UART_FIELD_MAX = 128,
    UART_MEASUREMENT_SCHEMA_FIELD_COUNT = 102,
    UART_T_FIELD_COUNT = 6,
    PARSE_WARNING_MAX = 48,
    PARSE_WARNING_LEN = 72,
};

/* ==========================================================================
 * 基础字段包装类型
 *
 * 每个类型都包含 available 标志，用于区分"字段缺失/未提供"
 * 和"字段存在但值为 0/false"这两种情况。
 * ========================================================================== */

/** @brief 布尔字段包装：available 指示该字段是否被成功解析 */
typedef struct {
    bool available;   /**< true = 字段有效，false = 字段缺失/空/格式错误 */
    bool value;       /**< 解析后的布尔值，仅在 available 为 true 时有效 */
} bool_field_t;

/** @brief 无符号 32 位整数字段包装 */
typedef struct {
    bool available;   /**< true = 字段有效，false = 字段缺失/空/格式错误 */
    uint32_t value;   /**< 解析后的数值，仅在 available 为 true 时有效 */
} u32_field_t;

/** @brief 有符号 32 位整数字段包装（用于 ecg_filtered 等可能为负的字段） */
typedef struct {
    bool available;
    int32_t value;
} i32_field_t;

/** @brief 日期字段包装：固定 8 位数字 YYYYMMDD */
typedef struct {
    bool available;
    char value[9];    /**< 8 位日期 + '\0' */
} date_field_t;

/** @brief 时间字段包装：固定 6 位数字 HHMMSS */
typedef struct {
    bool available;
    char value[7];    /**< 6 位时间 + '\0' */
} time_field_t;

/* ==========================================================================
 * 解析警告管理
 * ========================================================================== */

/**
 * @brief 解析警告列表
 *
 * 每个字段解析失败时通过 add_field_warning() 向此列表追加一条警告。
 * 警告格式为 "field_<索引>_<字段名>_<原因>"，如 "field_9_bpm_invalid"。
 * 超过 PARSE_WARNING_MAX 条后设置 truncated 标志，不再追加。
 */
typedef struct {
    char items[PARSE_WARNING_MAX][PARSE_WARNING_LEN]; /**< 警告字符串数组 */
    size_t count;        /**< 当前已记录的警告条数 */
    bool truncated;      /**< 警告列表是否被截断 */
} parse_warnings_t;

/* ==========================================================================
 * 功能模块数据结构
 *
 * 每个结构体对应 M 帧中的一组相关字段。
 * 顶层 available 标志表示该模块是否至少有一个字段被成功解析。
 * 带 valid 标志的模块（BPM/SpO2/RR/IBI/HRV/ECG/PTT/SpO2_Ratio）表示
 * 其对应的 _valid 标志字段是否为 1。
 * ========================================================================== */

/** @brief 时间戳模块（列 1-3）：RTC 有效标志 + 日期 + 时间 */
typedef struct {
    bool available;
    bool_field_t rtc_valid;
    date_field_t yyyymmdd;
    time_field_t hhmmss;
} timestamp_fields_t;

/** @brief 原始 PPG 模块（列 4-7）：红光/红外 ADC 值、基线、手指在位标志 */
typedef struct {
    bool available;
    u32_field_t red;
    u32_field_t ir;
    u32_field_t baseline_ir;
    bool_field_t finger;
} raw_ppg_fields_t;

/** @brief 带有效标志的测值指标通用类型
 *
 * 用于 BPM/SpO2/RR/IBI/PTT 等"先有 valid 标志再有值"的字段对。
 * valid 综合判断：available 为 true 且 valid_flag 存在且为 1。
 */
typedef struct {
    bool available;        /**< 值字段是否被成功解析 */
    bool valid;            /**< 综合有效判断：值存在且对应 valid_flag 为 1 */
    bool_field_t valid_flag; /**< 有效标志字段 */
    u32_field_t value;     /**< 数值字段 */
} metric_u32_t;

/** @brief HRV 时域模块（列 16-19）：有效标志 + mean_ibi/sdnn/rmssd */
typedef struct {
    bool available;
    bool valid;
    bool_field_t valid_flag;
    u32_field_t mean_ibi;
    u32_field_t sdnn;
    u32_field_t rmssd;
} hrv_time_fields_t;

/** @brief HRV 庞加莱图模块（列 22-25）：sd1/sd2/比值/节律不规则标志 */
typedef struct {
    bool available;
    u32_field_t sd1;
    u32_field_t sd2;
    u32_field_t sd1_sd2_x100;
    bool_field_t rhythm_irregular;
} hrv_poincare_fields_t;

/** @brief HRV 频域模块（列 26-29）：有效标志 + LF/HF/LF_HF 比值 */
typedef struct {
    bool available;
    bool valid;
    bool_field_t valid_flag;
    u32_field_t lf_power_x100;
    u32_field_t hf_power_x100;
    u32_field_t lf_hf_x100;
} hrv_freq_fields_t;

/** @brief 运动模块（列 20-21）：运动伪影标志 + 运动评分 */
typedef struct {
    bool available;
    bool_field_t motion_artifact;
    u32_field_t motion_score;
} motion_fields_t;

/**
 * @brief ECG 模块（列 72-77, 80-84）
 *
 * 核心 ECG 字段：valid_flag/hr/rr_ms/lead_off/r_peak_ms/filtered
 * 诊断计数器：sample_count/adc_sat_count/dma_overflow_count/
 *              lead_off_count/no_r_peak_timeout_count
 */
typedef struct {
    bool available;
    bool valid;
    bool_field_t valid_flag;           /**< 列 72: ecg_valid */
    u32_field_t hr;                    /**< 列 73: ecg_hr 心率(bpm) */
    u32_field_t rr_ms;                 /**< 列 74: ecg_rr_ms RR间期(ms) */
    u32_field_t lead_off;              /**< 列 75: ecg_lead_off 导联脱落位掩码 */
    u32_field_t r_peak_ms;             /**< 列 76: ecg_r_peak_ms R波时间戳(ms) */
    i32_field_t filtered;              /**< 列 77: ecg_filtered 滤波后ECG值(可为负) */
    u32_field_t sample_count;          /**< 列 80: ECG 采样总数 */
    u32_field_t adc_sat_count;         /**< 列 81: ADC 饱和次数 */
    u32_field_t dma_overflow_count;    /**< 列 82: DMA 溢出次数 */
    u32_field_t lead_off_count;        /**< 列 83: 导联脱落事件计数 */
    u32_field_t no_r_peak_timeout_count; /**< 列 84: R波检测超时次数 */
} ecg_fields_t;

/** @brief PTT 脉搏传导时间模块（列 78-79）：有效标志 + 时间值(ms) */
typedef struct {
    bool available;
    bool valid;
    bool_field_t valid_flag;
    u32_field_t value;
} ptt_fields_t;

/** @brief 信号质量模块（列 30-35）：信号质量评分 + PI 灌注指数 + AC RMS */
typedef struct {
    bool available;
    u32_field_t signal_quality;        /**< 列 30: 信号质量评分(0-100) */
    bool_field_t raw_signal_present;   /**< 列 31: 是否有原始信号 */
    u32_field_t signal_ir_pi_x1000;    /**< 列 32: IR 灌注指数×1000 */
    u32_field_t signal_red_pi_x1000;   /**< 列 33: RED 灌注指数×1000 */
    u32_field_t signal_ir_ac_rms;      /**< 列 34: IR 交流分量 RMS */
    u32_field_t signal_red_ac_rms;     /**< 列 35: RED 交流分量 RMS */
} signal_quality_fields_t;

/** @brief SpO2 比率模块（列 36-38）：有效标志 + 比率×1000 + 平衡状态 */
typedef struct {
    bool available;
    bool valid;
    bool_field_t valid_flag;
    u32_field_t ratio_x1000;
    u32_field_t balance_status;
} spo2_ratio_fields_t;

/** @brief 手指检测诊断模块（列 39-46）：基线范围、自适应阈值、信号跨度、确认计数 */
typedef struct {
    bool available;
    u32_field_t baseline_range_ir;
    u32_field_t adaptive_finger_on_delta;
    u32_field_t adaptive_finger_off_delta;
    u32_field_t ir_signal_delta;
    u32_field_t ir_signal_span;
    u32_field_t red_signal_span;
    u32_field_t finger_on_confirm_count;
    u32_field_t finger_off_confirm_count;
} finger_detect_fields_t;

/** @brief 传感器诊断模块（列 47-60）：MAX30102 状态、FIFO、读写统计、恢复计数 */
typedef struct {
    bool available;
    u32_field_t last_read_status;
    u32_field_t error_streak;
    u32_field_t fifo_write_ptr;
    u32_field_t fifo_read_ptr;
    u32_field_t fifo_overflow_count;
    u32_field_t fifo_available_samples;
    u32_field_t read_ok_count;
    u32_field_t read_busy_count;
    u32_field_t read_error_count;
    u32_field_t recover_count;
    u32_field_t last_sample_tick;
    u32_field_t sample_change_count;
    u32_field_t sample_same_count;
    u32_field_t last_i2c_error;
} sensor_diag_fields_t;

/** @brief 系统诊断模块（列 61-71）：RTC/UART/SD/显示/调试状态 */
typedef struct {
    bool available;
    bool_field_t rtc_read_ok;           /**< 列 61: RTC 读取成功 */
    bool_field_t uart_rx_message_valid; /**< 列 62: UART 接收消息有效 */
    bool_field_t uart_tx_message_valid; /**< 列 63: UART 发送消息有效 */
    bool_field_t sd_log_active;         /**< 列 64: SD 日志是否开启 */
    u32_field_t sd_state;               /**< 列 65: SD 卡状态码 */
    u32_field_t sd_error;               /**< 列 66: SD 卡错误码 */
    u32_field_t sd_total_written;       /**< 列 67: SD 累计写入字节数 */
    u32_field_t display_refresh_count;  /**< 列 68: 显示器刷新次数 */
    u32_field_t display_last_refresh_tick; /**< 列 69: 上次刷新 tick */
    u32_field_t debug_mode;             /**< 列 70: 调试模式标志 */
    u32_field_t current_page;           /**< 列 71: 当前显示页面 */
} system_diag_fields_t;

/** @brief 崩溃信息模块（列 85-91）：崩溃标志/来源/任务/阶段/tick + 重启计数 */
typedef struct {
    bool available;
    bool_field_t crash_flag;
    u32_field_t crash_source;
    u32_field_t crash_task;
    u32_field_t crash_phase;
    u32_field_t crash_tick;
    u32_field_t reboot_count;
    u32_field_t reset_flags;
} crash_fields_t;

/** @brief 任务阶段模块（列 92-95）：各任务的运行阶段编号 */
typedef struct {
    bool available;
    u32_field_t max_task_phase;
    u32_field_t ui_task_phase;
    u32_field_t sd_task_phase;
    u32_field_t wdt_task_phase;
} task_phase_fields_t;

/** @brief 任务栈高水位模块（列 96-99）：各任务栈使用峰值(字节) */
typedef struct {
    bool available;
    u32_field_t max_task_stack_hwm;
    u32_field_t ui_task_stack_hwm;
    u32_field_t sd_task_stack_hwm;
    u32_field_t wdt_task_stack_hwm;
} task_stack_fields_t;

/** @brief 任务心跳模块（列 100-101）：各任务的心跳计数器 */
typedef struct {
    bool available;
    u32_field_t max_task_heartbeat;
    u32_field_t ui_task_heartbeat;
} task_heartbeat_fields_t;

/* ==========================================================================
 * 解析中间结构与 JSON 构建器
 * ========================================================================== */

/**
 * @brief M 帧解析后的完整中间表示
 *
 * 解析阶段将每个 CSV 字段独立解析到对应的子模块中，各字段解析失败不影响
 * 其他字段。JSON 构建阶段再从此结构中读取并输出。
 *
 * raw_line 保存原始串口行指针（不复制，指向上层缓冲区的数据）。
 * fields 指向 split_csv_fields 原地切割后的字段指针数组。
 */
typedef struct {
    const char *raw_line;        /**< 原始串口行（指向上层缓冲区，不复制） */
    uint32_t rx_ms;              /**< 接收时刻的 ESP32 系统毫秒时间戳 */
    int field_count;             /**< CSV 字段总数（split_csv_fields 返回值） */
    int stored_field_count;      /**< 实际存入 fields[] 的字段数（受 UART_FIELD_MAX 限制） */
    const char **fields;         /**< CSV 字段指针数组（原地切割，不复制字符串） */

    /* ---- 各功能模块解析结果 ---- */
    timestamp_fields_t timestamp;
    raw_ppg_fields_t raw_ppg;
    metric_u32_t bpm;
    metric_u32_t spo2;
    metric_u32_t rr;
    metric_u32_t ibi;
    hrv_time_fields_t hrv_time;
    hrv_poincare_fields_t hrv_poincare;
    hrv_freq_fields_t hrv_freq;
    motion_fields_t motion;
    ecg_fields_t ecg;
    ptt_fields_t ptt;
    signal_quality_fields_t signal_quality;
    spo2_ratio_fields_t spo2_ratio;
    finger_detect_fields_t finger_detect;
    sensor_diag_fields_t sensor_diag;
    system_diag_fields_t system_diag;
    crash_fields_t crash;
    task_phase_fields_t task_phase;
    task_stack_fields_t task_stack;
    task_heartbeat_fields_t task_heartbeat;

    parse_warnings_t parse_warnings; /**< 解析过程中积累的警告列表 */
} measurement_frame_t;

/** @brief T 帧解析结果：时间设置应答 */
typedef struct {
    bool set_ok;        /**< 时间设置是否成功 */
    bool rtc_valid;     /**< 设置后 RTC 是否有效 */
    char date[9];       /**< RTC 当前日期(YYYYMMDD) */
    char time[7];       /**< RTC 当前时间(HHMMSS) */
    const char *reason; /**< 应答原因字符串（如 "ok"/"ntp_synced"） */
} stm32_time_ack_t;

/**
 * @brief 动态 JSON 字符串构建器
 *
 * 通过 realloc 自动扩容的字符串缓冲区，提供追加文本、格式化追加、
 * JSON 字符串转义追加等方法。构建完成后通过 json_writer_take 取出
 * 最终字符串并转移所有权。
 */
typedef struct {
    char *data;  /**< 动态分配的字符串缓冲区 */
    size_t len;  /**< 当前已写入长度（不含 '\0'） */
    size_t cap;  /**< 缓冲区总容量 */
    bool ok;     /**< 构建状态：true=正常，false=内存分配失败（缓冲区已释放） */
} json_writer_t;

#include "stm32_protocol_impl.inc"
