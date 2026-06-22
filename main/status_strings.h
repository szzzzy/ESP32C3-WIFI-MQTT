#ifndef STATUS_STRINGS_H
#define STATUS_STRINGS_H

/**
 * @brief Shared Wi-Fi state string: "ok" / "connecting" / "stopped".
 *
 * @param snap  Pointer to a struct with bool fields wifi_connected, wifi_started.
 */
#define WIFI_STATE_STR(snap) \
    ((snap)->wifi_connected ? "ok" : (snap)->wifi_started ? "connecting" : "stopped")

/**
 * @brief Shared MQTT state string: "ok" / "connected" / "starting" / "stopped".
 *
 * @param snap  Pointer to a struct with bool fields mqtt_connected, mqtt_subscribed, mqtt_started.
 */
#define MQTT_STATE_STR(snap) \
    ((snap)->mqtt_connected && (snap)->mqtt_subscribed ? "ok" : \
     (snap)->mqtt_connected ? "connected" : \
     (snap)->mqtt_started ? "starting" : "stopped")

/**
 * @brief Shared UART state string: "ok" / "partial" / "stopped".
 *
 * @param snap  Pointer to a struct with bool fields uart_ready, uart_tx_task_running, uart_rx_task_running.
 */
#define UART_STATE_STR(snap) \
    ((snap)->uart_ready && (snap)->uart_tx_task_running && (snap)->uart_rx_task_running ? "ok" : \
     (snap)->uart_ready ? "partial" : "stopped")

/**
 * @brief Shared protocol state string: "ok" / "error" / "idle".
 *
 * @param snap  Pointer to a struct with uint32_t fields protocol_ok, protocol_error and bool field last_protocol_ok.
 */
#define PROTOCOL_STATE_STR(snap) \
    ((snap)->protocol_ok == 0U && (snap)->protocol_error == 0U ? "idle" : \
     (snap)->last_protocol_ok ? "ok" : "error")

#endif
