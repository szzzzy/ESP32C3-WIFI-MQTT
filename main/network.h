#ifndef NETWORK_H
#define NETWORK_H

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
void network_wait_for_wifi(void);

/**
 * @brief 创建并启动 MQTT 客户端。
 *
 * 函数会根据配置的 broker 地址创建客户端实例、注册事件回调，并启动后台任务。
 * 如果客户端已经创建，则直接返回，避免重复启动。
 */
void network_start_mqtt(void);

/**
 * @brief 把一行串口文本包装成 JSON 并发布到 MQTT。
 *
 * 该接口会先检查 MQTT 是否已连接，再调用协议模块把串口文本转换为统一的
 * JSON 结构，最后将结果发布到上行主题。
 *
 * @param line 已经完成一行组包的串口原始文本。
 */
void network_publish_uart_line(const char *line);

#endif
