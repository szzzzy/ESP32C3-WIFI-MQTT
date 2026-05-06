#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/sdspi_host.h"
#include "driver/spi_master.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"

#include "sdmmc_cmd.h"

#include "app_config.h"
#include "app_status.h"
#include "sd_logger.h"

static const char *TAG = APP_TAG;

typedef struct {
    size_t len;
    char data[UART_BUF_SIZE];
} sd_log_message_t;

static SemaphoreHandle_t s_sd_mutex = NULL;
static QueueHandle_t s_sd_log_queue = NULL;
static TaskHandle_t s_sd_writer_task_handle = NULL;
static TaskHandle_t s_sd_monitor_task_handle = NULL;
static sdmmc_card_t *s_sd_card = NULL;
static FILE *s_sd_log_file = NULL;
static bool s_sd_ready = false;
static bool s_sd_bus_ready = false;
static bool s_sd_waiting_for_card_logged = false;
static uint32_t s_sd_mount_failures = 0;
static uint32_t s_sd_lines_since_flush = 0;
static TickType_t s_sd_last_flush_tick = 0;
static uint32_t s_sd_fast_path_drops = 0;

static esp_err_t sdcard_mount_locked(void);
static void sdcard_unmount_locked(void);
static TickType_t sdcard_retry_delay_ticks(uint32_t failure_count);
static void sdcard_monitor_task(void *arg);
static void sdcard_writer_task(void *arg);
static bool sdcard_open_log_file_locked(void);
static esp_err_t sdcard_flush_log_file_locked(void);
static void sdcard_close_log_file_locked(bool report_close_failure);
static void sdcard_handle_io_error_locked(const char *operation);
static void sdcard_report_fast_path_drops(void);

/**
 * @brief 初始化 SD 日志模块的运行资源。
 *
 * 该函数会创建 SD 互斥锁、初始化 SDSPI 总线、创建日志队列并启动后台写入任务。
 * 挂载 SD 卡由监控任务异步完成，因此初始化阶段不会因为未插卡而阻塞主流程。
 *
 * @return
 *      - ESP_OK: 初始化成功
 *      - ESP_ERR_NO_MEM: 互斥锁或队列创建失败
 *      - ESP_FAIL: 写入任务创建失败
 *      - 其他错误码: SPI 总线初始化失败
 */
esp_err_t sd_logger_init(void)
{
    esp_err_t ret = ESP_OK;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_PIN_MOSI,
        .miso_io_num = SD_PIN_MISO,
        .sclk_io_num = SD_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };

    if (s_sd_mutex == NULL) {
        s_sd_mutex = xSemaphoreCreateMutex();
        if (s_sd_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create SD mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    if (!s_sd_bus_ready) {
        ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
        if (ret == ESP_ERR_INVALID_STATE) {
            ret = ESP_OK;
        } else if (ret != ESP_OK) {
            ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
            return ret;
        }

        s_sd_bus_ready = true;
        app_status_set_sd_bus_ready(true);
    }

    if (s_sd_log_queue == NULL) {
        s_sd_log_queue = xQueueCreate(SD_LOG_QUEUE_LENGTH, sizeof(sd_log_message_t));
        if (s_sd_log_queue == NULL) {
            ESP_LOGE(TAG, "Failed to create SD log queue");
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_sd_writer_task_handle == NULL) {
        if (xTaskCreate(sdcard_writer_task,
                        "sdcard_writer_task",
                        SD_LOG_TASK_STACK_SIZE,
                        NULL,
                        SD_LOG_TASK_PRIORITY,
                        &s_sd_writer_task_handle) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create SD writer task");
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

/**
 * @brief 启动 SD 卡热插拔监控任务。
 *
 * 监控任务负责按重试策略尝试挂载 SD 卡，并在卡移除或状态异常时自动卸载。
 * 如果任务已经存在，则直接刷新运行状态并返回成功。
 *
 * @return
 *      - ESP_OK: 任务已启动或已存在
 *      - ESP_ERR_INVALID_STATE: SD 日志模块尚未初始化
 *      - ESP_FAIL: 任务创建失败
 */
esp_err_t sd_logger_start_monitor_task(void)
{
    if (s_sd_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_sd_monitor_task_handle != NULL) {
        app_status_set_sd_monitor_running(true);
        return ESP_OK;
    }

    if (xTaskCreate(sdcard_monitor_task,
                    "sdcard_monitor_task",
                    SD_MONITOR_TASK_STACK_SIZE,
                    NULL,
                    SD_MONITOR_TASK_PRIORITY,
                    &s_sd_monitor_task_handle) != pdPASS) {
        return ESP_FAIL;
    }

    app_status_set_sd_monitor_running(true);
    return ESP_OK;
}

/**
 * @brief 将一行串口文本排入 SD 日志写入队列。
 *
 * 该接口位于串口接收链路的快速路径中，只做长度检查和非阻塞入队。
 * 队列未就绪、内容过长或队列已满时会累计丢弃计数，由后台任务统一上报状态。
 *
 * @param line 需要追加到 SD 日志文件的单行文本。
 */
void sd_logger_append_line(const char *line)
{
    sd_log_message_t msg = {0};
    size_t len;

    if (line == NULL || line[0] == '\0') {
        return;
    }

    if (s_sd_log_queue == NULL) {
        __atomic_add_fetch(&s_sd_fast_path_drops, 1U, __ATOMIC_RELAXED);
        return;
    }

    len = strnlen(line, UART_BUF_SIZE - 1U);
    if (len == 0U) {
        return;
    }

    if (len == (UART_BUF_SIZE - 1U) && line[len] != '\0') {
        __atomic_add_fetch(&s_sd_fast_path_drops, 1U, __ATOMIC_RELAXED);
        return;
    }

    memcpy(msg.data, line, len);
    msg.data[len] = '\0';
    msg.len = len;

    if (xQueueSend(s_sd_log_queue, &msg, 0) != pdTRUE) {
        __atomic_add_fetch(&s_sd_fast_path_drops, 1U, __ATOMIC_RELAXED);
    }
}

/**
 * @brief 在持有 SD 互斥锁时尝试挂载文件系统。
 *
 * 函数会配置 SDSPI 主机和片选引脚，临时关闭底层 SD/FAT 挂载日志以减少未插卡时的刷屏，
 * 挂载成功后更新模块状态并打印卡信息。
 *
 * @return
 *      - ESP_OK: SD 卡已挂载或原本已经就绪
 *      - ESP_ERR_INVALID_STATE: SPI 总线尚未初始化
 *      - 其他错误码: SD/FAT 挂载失败
 */
static esp_err_t sdcard_mount_locked(void)
{
    esp_err_t ret;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = true,
    };
    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    esp_log_level_t sdmmc_sd_level;
    esp_log_level_t vfs_fat_sdmmc_level;

    if (!s_sd_bus_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_sd_ready && s_sd_card != NULL) {
        return ESP_OK;
    }

    host.max_freq_khz = 5000;
    slot_cfg.gpio_cs = SD_PIN_CS;
    slot_cfg.host_id = host.slot;

    sdmmc_sd_level = esp_log_level_get("sdmmc_sd");
    vfs_fat_sdmmc_level = esp_log_level_get("vfs_fat_sdmmc");

    esp_log_level_set("sdmmc_sd", ESP_LOG_NONE);
    esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_NONE);
    ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_cfg, &mount_cfg, &s_sd_card);
    esp_log_level_set("sdmmc_sd", sdmmc_sd_level);
    esp_log_level_set("vfs_fat_sdmmc", vfs_fat_sdmmc_level);

    if (ret != ESP_OK) {
        s_sd_card = NULL;
        s_sd_ready = false;
        return ret;
    }

    s_sd_ready = true;
    s_sd_waiting_for_card_logged = false;
    s_sd_mount_failures = 0;
    app_status_set_sd_mounted(true);
    ESP_LOGI(TAG, "SD card mounted, file logging enabled");
    sdmmc_card_print_info(stdout, s_sd_card);
    return ESP_OK;
}

/**
 * @brief 在持有 SD 互斥锁时卸载 SD 文件系统。
 *
 * 卸载前会先关闭当前日志文件，然后释放 VFS/FAT 挂载并清理卡句柄、
 * 等待插卡提示和重试计数，使后续监控循环可以重新开始挂载流程。
 */
static void sdcard_unmount_locked(void)
{
    sdcard_close_log_file_locked(false);

    if (!s_sd_ready || s_sd_card == NULL) {
        s_sd_ready = false;
        s_sd_card = NULL;
        return;
    }

    if (esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_sd_card) != ESP_OK) {
        ESP_LOGW(TAG, "SD unmount returned error after card removal");
    }

    s_sd_ready = false;
    s_sd_card = NULL;
    s_sd_waiting_for_card_logged = false;
    s_sd_mount_failures = 0;
    app_status_set_sd_mounted(false);
    ESP_LOGW(TAG, "SD card logging disabled");
}

/**
 * @brief 根据连续挂载失败次数计算下一次重试间隔。
 *
 * 重试间隔从初始值开始按 2 倍退避增长，达到上限后保持最大等待时间，
 * 避免未插卡时持续占用 CPU 和日志带宽。
 *
 * @param failure_count 连续挂载失败次数。
 *
 * @return 下一次重试前需要等待的 FreeRTOS tick 数。
 */
static TickType_t sdcard_retry_delay_ticks(uint32_t failure_count)
{
    uint32_t delay_ms = SD_MOUNT_RETRY_INITIAL_MS;

    while (failure_count > 1U && delay_ms < SD_MOUNT_RETRY_MAX_MS) {
        if (delay_ms > (SD_MOUNT_RETRY_MAX_MS / 2U)) {
            delay_ms = SD_MOUNT_RETRY_MAX_MS;
        } else {
            delay_ms *= 2U;
        }
        failure_count--;
    }

    return pdMS_TO_TICKS(delay_ms);
}

/**
 * @brief SD 卡监控任务，负责挂载、状态检测和拔卡恢复。
 *
 * 任务启动后先等待硬件上电稳定，再循环尝试挂载 SD 卡。
 * 已挂载时会周期性检查卡状态，发现卡移除或无响应后立即卸载并回到等待插卡状态。
 *
 * @param arg 任务参数，当前未使用。
 */
static void sdcard_monitor_task(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(SD_STARTUP_DELAY_MS));

    while (1) {
        TickType_t delay_ticks = sdcard_retry_delay_ticks(s_sd_mount_failures + 1U);

        if (s_sd_mutex != NULL && xSemaphoreTake(s_sd_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            if (!s_sd_ready || s_sd_card == NULL) {
                esp_err_t ret = sdcard_mount_locked();
                if (ret == ESP_OK) {
                    delay_ticks = pdMS_TO_TICKS(SD_STATUS_CHECK_MS);
                } else {
                    s_sd_mount_failures++;
                    delay_ticks = sdcard_retry_delay_ticks(s_sd_mount_failures);

                    if (!s_sd_waiting_for_card_logged) {
                        ESP_LOGI(TAG, "SD card not detected, continue without file logging");
                        s_sd_waiting_for_card_logged = true;
                    } else if ((s_sd_mount_failures & (s_sd_mount_failures - 1U)) == 0U) {
                        ESP_LOGI(TAG,
                                 "SD card still unavailable (%s), next retry in %lu ms",
                                 esp_err_to_name(ret),
                                 (unsigned long)pdTICKS_TO_MS(delay_ticks));
                    }
                }
            } else if (sdmmc_get_status(s_sd_card) != ESP_OK) {
                ESP_LOGW(TAG, "SD card removed or not responding");
                sdcard_unmount_locked();
            } else {
                delay_ticks = pdMS_TO_TICKS(SD_STATUS_CHECK_MS);
            }

            xSemaphoreGive(s_sd_mutex);
        }

        vTaskDelay(delay_ticks);
    }
}

/**
 * @brief SD 日志写入任务，负责把队列中的文本落盘。
 *
 * 任务从日志队列取出串口行，在 SD 已挂载时按追加模式写入日志文件。
 * 为降低频繁写盘开销，写入后按行数批量或定时刷新；无卡或写入异常时会记录丢弃/失败状态。
 *
 * @param arg 任务参数，当前未使用。
 */
static void sdcard_writer_task(void *arg)
{
    sd_log_message_t msg;

    (void)arg;

    while (1) {
        BaseType_t got_line;

        got_line = (s_sd_log_queue != NULL) &&
                   (xQueueReceive(s_sd_log_queue, &msg, pdMS_TO_TICKS(SD_LOG_FLUSH_INTERVAL_MS)) == pdTRUE);

        sdcard_report_fast_path_drops();

        if (s_sd_mutex == NULL || xSemaphoreTake(s_sd_mutex, portMAX_DELAY) != pdTRUE) {
            if (got_line) {
                app_status_note_sd_drop(1U);
            }
            continue;
        }

        if (!got_line) {
            (void)sdcard_flush_log_file_locked();
            xSemaphoreGive(s_sd_mutex);
            continue;
        }

        if (!s_sd_ready || s_sd_card == NULL) {
            app_status_note_sd_drop(1U);
            xSemaphoreGive(s_sd_mutex);
            continue;
        }

        if (!sdcard_open_log_file_locked()) {
            xSemaphoreGive(s_sd_mutex);
            continue;
        }

        if (fwrite(msg.data, 1, msg.len, s_sd_log_file) != msg.len || fputc('\n', s_sd_log_file) == EOF) {
            sdcard_handle_io_error_locked("write");
            xSemaphoreGive(s_sd_mutex);
            continue;
        }

        s_sd_lines_since_flush++;
        app_status_note_sd_write(true);

        if (s_sd_lines_since_flush >= SD_LOG_FLUSH_LINE_BATCH ||
            ((xTaskGetTickCount() - s_sd_last_flush_tick) >= pdMS_TO_TICKS(SD_LOG_FLUSH_INTERVAL_MS))) {
            (void)sdcard_flush_log_file_locked();
        }

        xSemaphoreGive(s_sd_mutex);
    }
}

/**
 * @brief 在持有 SD 互斥锁时打开日志文件。
 *
 * 如果日志文件已经打开则直接复用；否则以追加模式打开固定日志路径，
 * 并重置批量刷新计数和最近刷新时间。
 *
 * @return true 表示文件可写，false 表示 SD 未就绪或打开失败。
 */
static bool sdcard_open_log_file_locked(void)
{
    if (!s_sd_ready || s_sd_card == NULL) {
        return false;
    }

    if (s_sd_log_file != NULL) {
        return true;
    }

    s_sd_log_file = fopen(SD_MOUNT_POINT "/stm32_log.txt", "a");
    if (s_sd_log_file == NULL) {
        sdcard_handle_io_error_locked("open");
        return false;
    }

    s_sd_lines_since_flush = 0;
    s_sd_last_flush_tick = xTaskGetTickCount();
    return true;
}

/**
 * @brief 在持有 SD 互斥锁时刷新日志文件缓冲区。
 *
 * 当文件未打开或没有待刷新的新行时直接返回成功；实际刷新失败会进入统一 I/O 错误处理流程。
 *
 * @return
 *      - ESP_OK: 无需刷新或刷新成功
 *      - ESP_FAIL: fflush 失败
 */
static esp_err_t sdcard_flush_log_file_locked(void)
{
    if (s_sd_log_file == NULL || s_sd_lines_since_flush == 0U) {
        return ESP_OK;
    }

    if (fflush(s_sd_log_file) != 0) {
        sdcard_handle_io_error_locked("flush");
        return ESP_FAIL;
    }

    s_sd_lines_since_flush = 0;
    s_sd_last_flush_tick = xTaskGetTickCount();
    return ESP_OK;
}

/**
 * @brief 在持有 SD 互斥锁时关闭日志文件。
 *
 * 函数会清空文件句柄和刷新统计。调用方可通过 `report_close_failure`
 * 决定关闭失败时是否写入告警和状态计数。
 *
 * @param report_close_failure true 表示关闭失败时记录写入失败，false 表示静默清理。
 */
static void sdcard_close_log_file_locked(bool report_close_failure)
{
    if (s_sd_log_file == NULL) {
        s_sd_lines_since_flush = 0;
        s_sd_last_flush_tick = 0;
        return;
    }

    if (fclose(s_sd_log_file) != 0 && report_close_failure) {
        ESP_LOGW(TAG, "Failed to close SD log file");
        app_status_note_sd_write(false);
    }

    s_sd_log_file = NULL;
    s_sd_lines_since_flush = 0;
    s_sd_last_flush_tick = 0;
}

/**
 * @brief 统一处理 SD 日志文件 I/O 异常。
 *
 * 函数会记录写入失败状态，并检查 SD 卡是否仍然响应。
 * 如果卡已经失联则卸载文件系统；否则仅关闭当前文件，等待下一次写入重新打开。
 *
 * @param operation 发生异常的文件操作名称，用于日志输出。
 */
static void sdcard_handle_io_error_locked(const char *operation)
{
    ESP_LOGW(TAG, "Failed to %s SD log file", operation);
    app_status_note_sd_write(false);

    if (s_sd_card != NULL && sdmmc_get_status(s_sd_card) != ESP_OK) {
        sdcard_unmount_locked();
        return;
    }

    sdcard_close_log_file_locked(false);
}

/**
 * @brief 上报快速路径中累计的 SD 日志丢弃数量。
 *
 * 串口接收侧为了保持非阻塞，只用原子计数记录入队失败。
 * 写入任务会周期性取走该计数并同步到应用状态模块。
 */
static void sdcard_report_fast_path_drops(void)
{
    uint32_t drops = __atomic_exchange_n(&s_sd_fast_path_drops, 0U, __ATOMIC_RELAXED);

    if (drops > 0U) {
        app_status_note_sd_drop(drops);
    }
}
