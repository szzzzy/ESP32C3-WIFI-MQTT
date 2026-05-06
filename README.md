# ESP32-C3 Pulse Oximeter Bridge / 脉搏血氧仪桥接固件

[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.4-blue)](https://docs.espressif.com/projects/esp-idf/)
[![MCU](https://img.shields.io/badge/MCU-ESP32--C3-green)](https://www.espressif.com/en/products/socs/esp32-c3)

A firmware for ESP32-C3 that bridges an STM32-based pulse oximeter sensor to MQTT cloud, with local SD card logging as backup.

一款运行在 ESP32-C3 上的桥接固件，将 STM32 脉搏血氧传感器的串口数据同时上报 MQTT 云端并落盘 SD 卡，支持下行命令反向透传。

---

## Architecture / 系统架构

```
STM32 (sensor)
    │ UART 115200 8N1
    │ CSV frames: M,... (measurement) / T,... (time ack)
    ▼
ESP32-C3 (this firmware)
    ├── UART RX → protocol parser → MQTT publish (pulseox/data)
    │                            → SD card append  (/sdcard/stm32_log.txt)
    ├── MQTT subscribe (pulseox/cmd) → UART TX → STM32
    └── Health status report every 10s via ESP_LOG
             ▲
MQTT Broker ◄┘
```

## Hardware Pinout / 硬件接线

| ESP32-C3 | SD Card (SPI) | STM32 UART |
|----------|---------------|------------|
| GPIO4    | MOSI          | —          |
| GPIO5    | SCK           | —          |
| GPIO6    | MISO          | —          |
| GPIO10   | CS            | —          |
| GPIO7    | —             | TX (ESP→STM32) |
| GPIO3    | —             | RX (STM32→ESP) |

## Features / 功能特性

- **UART Bridge / 串口桥接**: Receives line-delimited CSV frames from STM32, auto-strips `\r`, handles buffer overflow with line discarding. / 接收 STM32 发出的换行分隔 CSV 帧，自动去除 `\r`，超长行自动丢弃。
- **Protocol Parser / 协议解析**: Parses 12-field measurement frames (`M`) and 6-field RTC time-ack frames (`T`). Strict field validation, JSON escaping for arbitrary text. Generates structured JSON with bridge/source/channel/protocol metadata. / 解析 12 字段测量帧（`M`）和 6 字段时间应答帧（`T`），严格字段校验，自由文本 JSON 转义，生成带元信息的结构化 JSON。
- **MQTT Uplink / MQTT 上行**: Publishes parsed JSON to `pulseox/data` topic. Skips publish when disconnected. / 将解析后的 JSON 发布到 `pulseox/data` 主题，断连时自动跳过。
- **MQTT Downlink / MQTT 下行**: Subscribes to `pulseox/cmd`, forwards payloads to STM32 over UART. Auto-appends newline if missing. Rejects fragmented/large payloads. / 订阅 `pulseox/cmd` 主题，将命令通过 UART 转发给 STM32，自动补换行，拒绝分片/超大负载。
- **SD Card Logger / SD 卡日志**: Hot-plug tolerant. Exponential backoff retry (5s→10s→...→60s max). Batch-flush every 8 lines or 1 second. Atomic drop counter for non-blocking fast path. Auto-unmount on card removal. / 支持热插拔。指数退避重试（5s→10s→...→最大 60s）。每 8 行或 1 秒批量刷盘。非阻塞快速路径用原子计数记录丢弃。拔卡自动卸载。
- **Health Monitor / 健康监控**: Thread-safe runtime status snapshot across WiFi/MQTT/SD/UART/Protocol subsystems. Periodic report every 10s + event-driven snapshot on errors. / 线程安全的运行时状态快照，覆盖 5 个子系统。每 10 秒周期性上报 + 异常事件即时快照。
- **SD Wiring Test Mode / SD 接线测试模式**: Compile-time toggle (`APP_MODE_SD_WIRING_TEST`) to run a standalone 2-step SD detection and read/write verification loop, independent of the bridge logic. / 编译时开关，可切换为独立的 SD 接线 2 步检测循环，不依赖桥接逻辑。

## Project Structure / 项目结构

```
├── CMakeLists.txt           # Project root / 项目根 CMake
├── dependencies.lock        # Managed component lock / 组件版本锁
├── sdkconfig                # ESP-IDF Kconfig (generated, in .gitignore)
├── main/
│   ├── CMakeLists.txt       # Component registration / 模块注册
│   ├── app_config.h         # All compile-time constants / 全局编译时常量
│   ├── hello_world.c        # app_main entry (bridge mode / SD test mode)
│   ├── app_status.c/.h      # Runtime health snapshot & periodic report
│   ├── network.c/.h         # WiFi STA + MQTT client (uplink & downlink)
│   ├── uart_bridge.c/.h     # UART driver, RX line assembly, TX queue
│   ├── stm32_protocol.c/.h  # CSV frame parser, JSON builder
│   ├── sd_logger.c/.h       # SD SPI mount, hot-plug monitor, async writer
│   └── idf_component.yml    # ESP-IDF component manifest
└── tools/
    ├── README.md
    └── publish_pc_time.py   # PC-side script to send RTC time via MQTT
```

## Configuration / 配置

Edit `main/app_config.h` before building:

| Macro | Default | Description |
|-------|---------|-------------|
| `APP_MODE_SD_WIRING_TEST` | `0` | Set to `1` for standalone SD wiring test mode |
| `WIFI_SSID` | `"jiamdaole"` | WiFi STA SSID |
| `WIFI_PASS` | `"12345678"` | WiFi STA password |
| `MQTT_BROKER_URI` | `"mqtt://172.20.10.4"` | MQTT broker address |
| `MQTT_PUBLISH_TOPIC` | `"pulseox/data"` | Uplink topic for sensor data |
| `MQTT_COMMAND_TOPIC` | `"pulseox/cmd"` | Downlink topic for commands |
| `UART_PORT` | `UART_NUM_1` | UART port number |
| `UART_TX_PIN` / `UART_RX_PIN` | `7` / `3` | UART pin assignments |
| `UART_BAUD` | `115200` | UART baud rate |
| `SD_PIN_MOSI` / `MISO` / `SCK` / `CS` | `4` / `6` / `5` / `10` | SD SPI pins |

## Protocol Frame Format / 协议帧格式

### Measurement Frame (M) / 测量帧

```
M,rtc_valid,YYYYMMDD,HHMMSS,red,ir,baseline_ir,finger,bpm_valid,bpm,spo2_valid,spo2
```

| Field | Type | Description |
|-------|------|-------------|
| M | char | Frame type identifier |
| rtc_valid | 0/1 | RTC clock validity |
| YYYYMMDD | 8 digits | Date |
| HHMMSS | 6 digits | Time |
| red | uint | Red LED ADC raw value |
| ir | uint | IR LED ADC raw value |
| baseline_ir | uint | IR baseline |
| finger | 0/1 | Finger on sensor |
| bpm_valid | 0/1 | Heart rate validity |
| bpm | uint | Heart rate (bpm) |
| spo2_valid | 0/1 | SpO2 validity |
| spo2 | uint | SpO2 (%) |

### Time-ack Frame (T) / 时间应答帧

```
T,set_ok,rtc_valid,YYYYMMDD,HHMMSS,reason
```

## Build & Flash / 编译与烧录

```bash
# Set up ESP-IDF environment
. ~/esp/esp-idf/export.sh

# Build
idf.py build

# Flash to ESP32-C3 (adjust port as needed)
idf.py -p /dev/ttyUSB0 flash monitor
```

## MQTT JSON Payload Examples / MQTT 消息示例

### Measurement / 测量数据

```json
{
  "bridge": "esp32c3",
  "source": "stm32",
  "channel": "uart1",
  "protocol": "stm32-compact-v1",
  "message": "measurement",
  "frame": "M",
  "rtc_valid": true,
  "date": "20260401",
  "time": "143025",
  "red": 123456,
  "ir": 234567,
  "baseline_ir": 100000,
  "finger": true,
  "bpm_valid": true,
  "bpm": 72,
  "spo2_valid": true,
  "spo2": 98
}
```

### Time-ack / 时间应答

```json
{
  "bridge": "esp32c3",
  "source": "stm32",
  "channel": "uart1",
  "protocol": "stm32-compact-v1",
  "message": "rtc_set_ack",
  "frame": "T",
  "set_ok": true,
  "rtc_valid": true,
  "date": "20260401",
  "time": "143025",
  "reason": "ntp_synced"
}
```

### Parse Error / 解析错误

```json
{
  "bridge": "esp32c3",
  "source": "stm32",
  "channel": "uart1",
  "protocol": "stm32-compact-v1",
  "message": "parse_error",
  "frame": "?",
  "error": "bad_measurement_frame",
  "raw": "M,1,20260401,xyz,..."
}
```

## Log Output / 日志输出

All modules share the tag `PULSEOX_ESP`. Health status is reported every 10 seconds:

```
I (12345) PULSEOX_ESP: STATUS[periodic] wifi=ok mqtt=ok sd=ok uart=ok protocol=ok(last=M)
I (12345) PULSEOX_ESP: STATUS[periodic] uart_rx=150 uart_drop=0 uart_tx_queued=2 uart_tx_ok=2 uart_tx_fail=0
I (12345) PULSEOX_ESP: STATUS[periodic] mqtt_cmd=2 mqtt_pub_ok=150 mqtt_pub_fail=0 protocol_ok=150 protocol_err=0 sd_write_ok=150 sd_write_fail=0 sd_drop=0
```

## Tools / 工具

`tools/publish_pc_time.py` — Publishes the local PC system time to the MQTT command topic so the STM32 can sync its RTC.

```bash
python tools/publish_pc_time.py --broker 172.20.10.4 --topic pulseox/cmd
```

## License / 许可证

MIT
