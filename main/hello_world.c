#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/sdspi_host.h"
#include "driver/spi_master.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"

#include "sdmmc_cmd.h"

#include "app_config.h"

#if !APP_MODE_SD_WIRING_TEST
#include "app_status.h"
#include "network.h"
#include "sd_logger.h"
#include "uart_bridge.h"
#endif

static const char *TAG = APP_TAG;

#if APP_MODE_SD_WIRING_TEST

#define SD_TEST_FILE_PATH SD_MOUNT_POINT "/sd_probe.txt"

typedef struct {
    sdmmc_host_t host;
    sdspi_device_config_t slot_config;
    sdspi_dev_handle_t card_handle;
    bool bus_ready;
    bool host_ready;
} sd_spi_context_t;

/**
 * @brief 打印当前 SD 接线测试使用的引脚配置。
 *
 * 该信息用于启动时核对 ESP32-C3 与 SD 模块之间的 MOSI、MISO、SCK、CS 接线，
 * 以及测试阶段使用的 SPI 频率。
 */
static void sd_test_log_pins(void)
{
    ESP_LOGI(TAG,
             "SD test pins: MOSI=GPIO%d MISO=GPIO%d SCK=GPIO%d CS=GPIO%d freq=%d kHz",
             SD_PIN_MOSI,
             SD_PIN_MISO,
             SD_PIN_SCK,
             SD_PIN_CS,
             SD_TEST_SPI_FREQ_KHZ);
}

/**
 * @brief 输出 SD 接线和文件系统检查建议。
 *
 * 当原始卡检测或文件系统挂载失败时调用，提示优先检查供电、共地、SPI 引脚和卡格式。
 */
static void sd_test_log_wiring_hint(void)
{
    ESP_LOGW(TAG, "Check 3V3/GND, CS, SCK, MOSI, MISO, and ensure the card is inserted.");
    ESP_LOGW(TAG, "Most SD modules need stable 3.3V power and a common ground with the ESP32-C3.");
    ESP_LOGW(TAG, "If raw card detection passes but mount fails, the card may need FAT32/FAT16 formatting.");
}

/**
 * @brief 初始化 SD 接线测试所需的 SDSPI 原始访问上下文。
 *
 * 函数会初始化 SPI 总线、SDSPI host 和 SD 设备句柄，并把最终可用于 `sdmmc_card_init`
 * 的 host 信息保存到上下文中。调用失败后仍可安全调用 `sd_test_spi_end` 做清理。
 *
 * @param ctx SD 测试上下文，函数会清零并填充其中的 host、slot 和资源状态。
 *
 * @return
 *      - ESP_OK: SDSPI 原始访问链路初始化成功
 *      - 其他错误码: SPI 总线、host 或设备初始化失败
 */
static esp_err_t sd_test_spi_begin(sd_spi_context_t *ctx)
{
    esp_err_t ret;
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_PIN_MOSI,
        .miso_io_num = SD_PIN_MISO,
        .sclk_io_num = SD_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };

    memset(ctx, 0, sizeof(*ctx));
    ctx->card_handle = -1;
    ctx->host = (sdmmc_host_t)SDSPI_HOST_DEFAULT();
    ctx->host.max_freq_khz = SD_TEST_SPI_FREQ_KHZ;
    ctx->slot_config = (sdspi_device_config_t)SDSPI_DEVICE_CONFIG_DEFAULT();
    ctx->slot_config.gpio_cs = SD_PIN_CS;
    ctx->slot_config.host_id = ctx->host.slot;

    ret = spi_bus_initialize(ctx->slot_config.host_id, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ctx->bus_ready = true;

    ret = sdspi_host_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sdspi_host_init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ctx->host_ready = true;

    ret = sdspi_host_init_device(&ctx->slot_config, &ctx->card_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sdspi_host_init_device failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ctx->host.slot = ctx->card_handle;
    return ESP_OK;
}

/**
 * @brief 释放 SD 接线测试中初始化的 SDSPI 资源。
 *
 * 函数根据上下文中的状态标志反向释放 host 和 SPI 总线，
 * 因此可在初始化中途失败时作为统一清理路径使用。
 *
 * @param ctx SD 测试上下文。
 */
static void sd_test_spi_end(sd_spi_context_t *ctx)
{
    if (ctx->host_ready) {
        esp_err_t ret = sdspi_host_deinit();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "sdspi_host_deinit returned: %s", esp_err_to_name(ret));
        }
        ctx->host_ready = false;
    }

    if (ctx->bus_ready) {
        esp_err_t ret = spi_bus_free(ctx->slot_config.host_id);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "spi_bus_free returned: %s", esp_err_to_name(ret));
        }
        ctx->bus_ready = false;
    }
}

/**
 * @brief 直接通过 SDSPI 协议检测 SD 卡是否响应。
 *
 * 该步骤不挂载文件系统，只验证 SD 卡是否能在 SPI 总线上完成基础初始化，
 * 用于区分接线/供电问题和后续 FAT 文件系统问题。
 *
 * @return
 *      - ESP_OK: SD 卡在 SPI 总线上有响应
 *      - 其他错误码: 原始卡初始化失败
 */
static esp_err_t sd_test_detect_card_raw(void)
{
    esp_err_t ret;
    sd_spi_context_t ctx;
    sdmmc_card_t card;

    memset(&card, 0, sizeof(card));
    ret = sd_test_spi_begin(&ctx);
    if (ret != ESP_OK) {
        sd_test_spi_end(&ctx);
        return ret;
    }

    ret = sdmmc_card_init(&ctx.host, &card);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Step 1/2 OK: SD card responded over SPI, wiring looks good.");
        sdmmc_card_print_info(stdout, &card);
    } else {
        ESP_LOGW(TAG, "Step 1/2 FAIL: sdmmc_card_init failed: %s", esp_err_to_name(ret));
    }

    sd_test_spi_end(&ctx);
    return ret;
}

/**
 * @brief 挂载 SD 文件系统并执行一次读写回读测试。
 *
 * 在原始卡检测通过后，该函数会挂载 FAT 文件系统，写入测试文件、读回内容并检查文件状态，
 * 用于确认接线正常之外，卡格式和文件 I/O 也可用。
 *
 * @return
 *      - ESP_OK: 挂载、写入、读取和 stat 检查全部通过
 *      - ESP_FAIL: 文件打开、读取或状态检查失败
 *      - 其他错误码: SPI 总线初始化或文件系统挂载失败
 */
static esp_err_t sd_test_mount_and_rw(void)
{
    esp_err_t ret;
    sdmmc_card_t *card = NULL;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_PIN_MOSI,
        .miso_io_num = SD_PIN_MISO,
        .sclk_io_num = SD_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 3,
        .allocation_unit_size = 0,
        .disk_status_check_enable = false,
    };
    FILE *fp = NULL;
    char readback[96] = {0};
    struct stat file_stat = {0};

    host.max_freq_khz = SD_TEST_SPI_FREQ_KHZ;
    slot_config.gpio_cs = SD_PIN_CS;
    slot_config.host_id = host.slot;

    ret = spi_bus_initialize(slot_config.host_id, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize for mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_cfg, &card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Step 2/2 FAIL: mount failed: %s", esp_err_to_name(ret));
        (void)spi_bus_free(slot_config.host_id);
        return ret;
    }

    fp = fopen(SD_TEST_FILE_PATH, "w");
    if (fp == NULL) {
        ret = ESP_FAIL;
        ESP_LOGW(TAG, "Open %s failed: errno=%d", SD_TEST_FILE_PATH, errno);
        goto cleanup;
    }

    fprintf(fp, "sd wiring ok via esp32-c3\n");
    fclose(fp);
    fp = NULL;

    fp = fopen(SD_TEST_FILE_PATH, "r");
    if (fp == NULL) {
        ret = ESP_FAIL;
        ESP_LOGW(TAG, "Read %s failed: errno=%d", SD_TEST_FILE_PATH, errno);
        goto cleanup;
    }

    if (fgets(readback, sizeof(readback), fp) == NULL) {
        ret = ESP_FAIL;
        ESP_LOGW(TAG, "Readback from %s returned empty data", SD_TEST_FILE_PATH);
        goto cleanup;
    }

    fclose(fp);
    fp = NULL;

    if (stat(SD_TEST_FILE_PATH, &file_stat) != 0) {
        ret = ESP_FAIL;
        ESP_LOGW(TAG, "stat(%s) failed: errno=%d", SD_TEST_FILE_PATH, errno);
        goto cleanup;
    }

    ESP_LOGI(TAG,
             "Step 2/2 OK: mount/read/write passed. File=%s size=%ld",
             SD_TEST_FILE_PATH,
             (long)file_stat.st_size);
    ESP_LOGI(TAG, "Readback: %s", readback);
    ret = ESP_OK;

cleanup:
    if (fp != NULL) {
        fclose(fp);
    }
    if (card != NULL) {
        (void)esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, card);
    }
    (void)spi_bus_free(slot_config.host_id);
    return ret;
}

/**
 * @brief SD 接线测试模式入口。
 *
 * 该模式会循环执行“原始卡响应检测”和“文件系统读写验证”两步测试，
 * 并按测试结果输出接线、供电或格式化相关提示，便于独立排查 SD 模块硬件链路。
 */
void app_main(void)
{
    ESP_LOGI(TAG, "SD wiring test mode enabled");
    sd_test_log_pins();

    while (true) {
        esp_err_t raw_ret = sd_test_detect_card_raw();

        if (raw_ret == ESP_OK) {
            esp_err_t fs_ret = sd_test_mount_and_rw();
            if (fs_ret == ESP_OK) {
                ESP_LOGI(TAG, "SD test result: PASS. Wiring, card response, and file IO all look good.");
            } else {
                ESP_LOGW(TAG, "SD test result: card responds, but filesystem verification failed.");
                sd_test_log_wiring_hint();
            }
            vTaskDelay(pdMS_TO_TICKS(SD_TEST_SUCCESS_DELAY_MS));
            continue;
        }

        ESP_LOGW(TAG, "SD test result: FAIL. Card was not detected on the SPI bus.");
        sd_test_log_wiring_hint();
        vTaskDelay(pdMS_TO_TICKS(SD_TEST_RETRY_DELAY_MS));
    }
}

#else

/**
 * @brief 处理 UART 接收到的一行 STM32 文本。
 *
 * 每一行会同时送入 SD 日志模块和 MQTT 上行模块，实现本地落盘与云端发布的并行处理。
 *
 * @param line UART 模块按换行符切分出的单行文本。
 * @param ctx 回调上下文，当前未使用。
 */
static void handle_uart_line(const char *line, void *ctx)
{
    (void)ctx;

    sd_logger_append_line(line);
    network_publish_uart_line(line);
}

/**
 * @brief 正常桥接模式入口。
 *
 * 主流程依次初始化运行状态、网络、SD 日志和 UART 桥接模块。
 * Wi-Fi 就绪后启动 MQTT，再启动 UART 接收任务，使 STM32 串口数据可以同时写入 SD 卡并发布到 MQTT。
 */
void app_main(void)
{
    ESP_LOGI(TAG,
             "Bridge config: uart_port=%d tx=%d rx=%d baud=%d mqtt_broker=%s pub_topic=%s cmd_topic=%s",
             (int)UART_PORT,
             UART_TX_PIN,
             UART_RX_PIN,
             UART_BAUD,
             MQTT_BROKER_URI,
             MQTT_PUBLISH_TOPIC,
             MQTT_COMMAND_TOPIC);

    if (app_status_init() != ESP_OK) {
        ESP_LOGW(TAG, "Runtime status init failed");
    } else if (app_status_start_task() != ESP_OK) {
        ESP_LOGW(TAG, "Runtime status task start failed");
    } else {
        app_status_log_snapshot("boot");
    }

    network_init();

    if (sd_logger_init() != ESP_OK) {
        ESP_LOGW(TAG, "SD subsystem init failed, continue without SD logging");
    } else if (sd_logger_start_monitor_task() != ESP_OK) {
        ESP_LOGW(TAG, "Failed to create SD monitor task, continue without hot-plug logging");
    }

    ESP_ERROR_CHECK(uart_bridge_init());

    if (uart_bridge_start_tx_task() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create UART TX task");
        abort();
    }

    network_wait_for_wifi();
    network_start_mqtt();

    if (uart_bridge_start_rx_task(handle_uart_line, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create UART RX task");
        abort();
    }

    app_status_log_snapshot("app_ready");
}

#endif
