#ifndef USB_TRANSPORT_H
#define USB_TRANSPORT_H

#include <stdbool.h>

#include "esp_err.h"

/**
 * @brief 初始化 USB Serial/JTAG 驱动和内部队列。
 *
 * 仅在 USB_TRANSPORT_ENABLED=1 时执行实际初始化，否则直接返回 ESP_OK。
 *
 * @return ESP_OK 成功，ESP_ERR_NO_MEM 队列创建失败，其他值驱动安装失败。
 */
esp_err_t usb_transport_init(void);

/**
 * @brief 启动 USB 发送任务（从队列取 JSON 写入 USB）。
 *
 * 重复调用安全。USB_TRANSPORT_ENABLED=0 时直接返回 ESP_OK。
 */
esp_err_t usb_transport_start_tx_task(void);

/**
 * @brief 启动 USB 接收任务（读取 PC 下行命令并处理）。
 *
 * 重复调用安全。USB_TRANSPORT_ENABLED=0 时直接返回 ESP_OK。
 */
esp_err_t usb_transport_start_rx_task(void);

/**
 * @brief 检测 USB 硬件连接状态（线缆插入 + 驱动就绪）。
 *
 * @return true 硬件已连接，false 未连接或模块未启用。
 */
bool usb_transport_is_connected(void);

/**
 * @brief 检测 USB 会话是否激活（GUI 已发送 START 且未超时）。
 *
 * 与 is_connected() 的区别：
 * - is_connected() 只看硬件（线缆插入）。
 * - is_active() 需要 GUI 主动发送 GUI_USB_START 激活会话，
 *   且会话在 USB_SESSION_TIMEOUT_MS 内收到 PING/START 续期。
 *
 * @return true 会话激活中，false 未激活或已超时。
 */
bool usb_transport_is_active(void);

/**
 * @brief 强制停用 USB 会话（由 STOP 命令或超时逻辑调用）。
 */
void usb_transport_deactivate(void);

/**
 * @brief 尝试将 JSON 负载排入 USB 发送队列。
 *
 * 仅在 is_active() 为 true 时才实际入队，否则直接返回 false。
 * 调用方在返回 false 时应 fallback 到 MQTT。
 *
 * @param json_payload 待发送的 JSON 字符串（'\0' 结尾）。
 * @return true 入队成功，false 会话未激活或队列满。
 */
bool usb_transport_queue_json(const char *json_payload);

#endif
