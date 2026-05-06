#ifndef SD_LOGGER_H
#define SD_LOGGER_H

#include "esp_err.h"

/**
 * @brief 初始化 SD 日志模块的基础资源。
 *
 * 该函数会创建互斥锁并初始化 SDSPI 总线，但不会立即阻塞等待 SD 卡插入。
 * 真正的挂载动作由后续监控任务按重试策略异步完成。
 *
 * @return
 *      - ESP_OK: 初始化成功
 *      - ESP_ERR_NO_MEM: 互斥锁创建失败
 *      - 其他错误码: SPI 总线初始化失败
 */
esp_err_t sd_logger_init(void);

/**
 * @brief 启动 SD 卡插拔监控任务。
 *
 * 监控任务会周期性检测是否有卡可挂载，并在卡被拔出时自动卸载文件系统，
 * 使日志功能在热插拔场景下尽量自恢复。
 *
 * @return
 *      - ESP_OK: 任务已启动或已存在
 *      - ESP_ERR_INVALID_STATE: 模块尚未初始化
 *      - ESP_FAIL: 任务创建失败
 */
esp_err_t sd_logger_start_monitor_task(void);

/**
 * @brief 追加一行文本到 SD 卡日志文件。
 *
 * 当 SD 卡已挂载时，该函数会把传入的一行串口文本追加到日志文件末尾；
 * 如果 SD 尚未就绪、写入失败或检测到卡异常，会自动跳过或触发卸载流程。
 *
 * @param line 需要写入日志文件的文本内容。
 */
void sd_logger_append_line(const char *line);

#endif
