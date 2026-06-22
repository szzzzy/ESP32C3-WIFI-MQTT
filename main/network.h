#ifndef NETWORK_H
#define NETWORK_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief 初始化网络子系统。
 *
 * 该接口会完成 NVS 准备、TCP/IP 栈初始化、事件循环创建以及 Wi-Fi STA 启动。
 * 调用成功返回后，系统会开始异步尝试连接到配置好的无线网络。
 */
void network_init(void);

/**
 * @brief 阻塞等待 Wi-Fi 连接建立。
 *
 * 该函数会等待到系统成功拿到 IP 地址后才返回，适合在启动 MQTT 之前调用，
 * 以保证后续网络相关组件启动时基础网络已经就绪。
 */
bool network_wait_for_wifi(uint32_t timeout_ms);

/**
 * @brief 创建并启动 MQTT 客户端。
 *
 * 函数会根据配置的 broker 地址创建客户端实例、注册事件回调，并启动后台任务。
 * 如果客户端已经创建，则直接返回，避免重复启动。
 */
void network_start_mqtt(void);

/**
 * @brief 将已构建好的 JSON 字符串直接发布到 MQTT 上行主题。
 *
 * 调用方需自行完成协议解析和 JSON 构建（通过 stm32_protocol_build_publish_json()），
 * 然后将结果传入本函数发布。适用于调用方选择上行通道（USB/MQTT）后再发布的场景。
 *
 * @param json_payload 已构建好的 JSON 字符串（'\0' 结尾）。
 * @return true 发布成功，false MQTT 未就绪或发送失败。
 */
bool network_publish_json_payload(const char *json_payload);

/**
 * @brief Publish an ESP bridge status JSON payload to MQTT_STATUS_TOPIC.
 *
 * Status publishing does not update sensor uplink counters, so GUI heartbeats
 * do not distort data-path diagnostics.
 */
bool network_publish_status_json(const char *json_payload);

#endif
