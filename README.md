# ESP32-C3 Pulse Oximeter Bridge / 脉搏血氧仪桥接固件

[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.4-blue)](https://docs.espressif.com/projects/esp-idf/)
[![MCU](https://img.shields.io/badge/MCU-ESP32--C3-green)](https://www.espressif.com/en/products/socs/esp32-c3)

A firmware for ESP32-C3 that bridges an STM32-based pulse oximeter sensor to MQTT cloud.

一款运行在 ESP32-C3 上的桥接固件，将 STM32 脉搏血氧传感器的串口数据上报 MQTT 云端，支持下行命令反向透传。

---

## Architecture / 系统架构

```
STM32 (sensor)
    │ UART 115200 8N1
    │ CSV frames: M,... (measurement) / T,... (time ack)
    ▼
ESP32-C3 (this firmware)
    ├── UART RX → protocol parser → MQTT publish (pulseox/data)
    ├── MQTT subscribe (pulseox/cmd) → UART TX → STM32
    └── Health status report every 10s via ESP_LOG
             ▲
MQTT Broker ◄┘
```

## Hardware Pinout / 硬件接线

| ESP32-C3 | STM32 UART |
|----------|------------|
| GPIO7    | TX (ESP→STM32) |
| GPIO3    | RX (STM32→ESP) |

## Features / 功能特性

- **UART Bridge / 串口桥接**: Receives line-delimited CSV frames from STM32, auto-strips `\r`, handles buffer overflow with line discarding. / 接收 STM32 发出的换行分隔 CSV 帧，自动去除 `\r`，超长行自动丢弃。
- **Protocol Parser / 协议解析**: Parses 102-field measurement frames (`M`) and 6-field RTC time-ack frames (`T`). Strict field validation, JSON escaping for arbitrary text. Generates structured JSON with bridge/source/channel/protocol metadata and schema_version. / 解析 102 字段测量帧（`M`）和 6 字段时间应答帧（`T`），严格字段校验，自由文本 JSON 转义，生成带元信息和 schema_version 的结构化 JSON。
- **MQTT Uplink / MQTT 上行**: Publishes parsed JSON to `pulseox/data` topic. Skips publish when disconnected. / 将解析后的 JSON 发布到 `pulseox/data` 主题，断连时自动跳过。
- **MQTT Downlink / MQTT 下行**: Subscribes to `pulseox/cmd`, forwards payloads to STM32 over UART. Auto-appends newline if missing. Rejects fragmented/large payloads. / 订阅 `pulseox/cmd` 主题，将命令通过 UART 转发给 STM32，自动补换行，拒绝分片/超大负载。
- **Health Monitor / 健康监控**: Thread-safe runtime status snapshot across WiFi/MQTT/UART/Protocol subsystems. Periodic report every 10s + event-driven snapshot on errors. / 线程安全的运行时状态快照，覆盖 4 个子系统。每 10 秒周期性上报 + 异常事件即时快照。

## Project Structure / 项目结构

```
├── CMakeLists.txt           # Project root / 项目根 CMake
├── dependencies.lock        # Managed component lock / 组件版本锁
├── sdkconfig                # ESP-IDF Kconfig (generated, in .gitignore)
├── main/
│   ├── CMakeLists.txt       # Component registration / 模块注册
│   ├── app_config.h         # All compile-time constants / 全局编译时常量
│   ├── hello_world.c        # app_main entry point
│   ├── app_status.c/.h      # Runtime health snapshot & periodic report
│   ├── network.c/.h         # WiFi STA + MQTT client (uplink & downlink)
│   ├── uart_bridge.c/.h     # UART driver, RX line assembly, TX queue
│   ├── stm32_protocol.c/.h  # CSV frame parser, JSON builder
│   └── idf_component.yml    # ESP-IDF component manifest
└── tools/
    ├── README.md
    └── publish_pc_time.py   # PC-side script to send RTC time via MQTT
```

## Configuration / 配置

Edit `main/app_config.h` before building:

| Macro | Default | Description |
|-------|---------|-------------|
| `WIFI_SSID` | `"jiamdaole"` | WiFi STA SSID |
| `WIFI_PASS` | `"12345678"` | WiFi STA password |
| `MQTT_BROKER_URI` | `"mqtt://172.20.10.4"` | MQTT broker address |
| `MQTT_PUBLISH_TOPIC` | `"pulseox/data"` | Uplink topic for sensor data |
| `MQTT_COMMAND_TOPIC` | `"pulseox/cmd"` | Downlink topic for commands |
| `UART_PORT` | `UART_NUM_1` | UART port number |
| `UART_TX_PIN` / `UART_RX_PIN` | `7` / `3` | UART pin assignments |
| `UART_BAUD` | `115200` | UART baud rate |

## Protocol Frame Format / 协议帧格式

### Measurement Frame (M) / 测量帧

The STM32 outputs a 102-column CSV frame with the following fields in order:

```
M,rtc_valid,YYYYMMDD,HHMMSS,red,ir,baseline_ir,finger_present,bpm_valid,bpm,spo2_valid,spo2,rr_valid,rr,ibi_valid,ibi,hrv_valid,mean_ibi,sdnn,rmssd,motion_artifact,motion_score,sd1,sd2,sd1_sd2_x100,rhythm_irregular,hrv_freq_valid,lf_power_x100,hf_power_x100,lf_hf_x100,signal_quality,raw_signal_present,signal_ir_pi_x1000,signal_red_pi_x1000,signal_ir_ac_rms,signal_red_ac_rms,spo2_ratio_valid,spo2_ratio_x1000,spo2_balance_status,baseline_range_ir,adaptive_finger_on_delta,adaptive_finger_off_delta,ir_signal_delta,ir_signal_span,red_signal_span,finger_on_confirm_count,finger_off_confirm_count,sensor_last_read_status,sensor_error_streak,sensor_fifo_write_ptr,sensor_fifo_read_ptr,sensor_fifo_overflow_count,sensor_fifo_available_samples,sensor_read_ok_count,sensor_read_busy_count,sensor_read_error_count,sensor_recover_count,sensor_last_sample_tick,sensor_sample_change_count,sensor_sample_same_count,sensor_last_i2c_error,rtc_read_ok,uart_rx_message_valid,uart_tx_message_valid,sd_log_active,sd_state,sd_error,sd_total_written,display_refresh_count,display_last_refresh_tick,debug_mode,current_page,ecg_valid,ecg_hr,ecg_rr_ms,ecg_lead_off,ecg_r_peak_ms,ecg_filtered,ptt_valid,ptt_ms,ecg_sample_count,ecg_adc_sat_count,ecg_dma_overflow_count,ecg_lead_off_count,ecg_no_r_peak_timeout_count,crash_flag,crash_source,crash_task,crash_phase,crash_tick,reboot_count,reset_flags,max_task_phase,ui_task_phase,sd_task_phase,wdt_task_phase,max_task_stack_hwm,ui_task_stack_hwm,sd_task_stack_hwm,wdt_task_stack_hwm,max_task_heartbeat,ui_task_heartbeat
```

Key field positions:
| Col | Field | Type | Description |
|-----|-------|------|-------------|
| 0 | M | char | Frame type identifier |
| 1 | rtc_valid | 0/1 | RTC clock validity |
| 2 | YYYYMMDD | 8 digits | Date |
| 3 | HHMMSS | 6 digits | Time |
| 4 | red | uint | Red LED ADC raw value |
| 5 | ir | uint | IR LED ADC raw value |
| 7 | finger_present | 0/1 | Finger on sensor |
| 8-9 | bpm_valid, bpm | 0/1, uint | Heart rate validity and value |
| 10-11 | spo2_valid, spo2 | 0/1, uint | SpO2 validity and value |
| 30 | signal_quality | uint | Signal quality score (0-100) |
| 36-38 | spo2_ratio | — | SpO2 ratio and balance |
| 61-71 | system diag | — | RTC, UART, SD, display, debug status |
| 72-77 | ecg | — | ECG valid, HR, RR, lead-off, R-peak, filtered |
| 78-79 | ptt | — | PTT validity and value (ms) |
| 80-84 | ecg diag | — | ECG sample/error counters |
| 85-91 | crash | — | Crash flag, source, task, phase, tick, reboot, reset |
| 92-101 | task diag | — | Task phases, stack HWM, heartbeats |

### Time-set Command / 时间设置命令

The STM32 recognizes the following command sent via MQTT → ESP32 → UART:

```
SETTIME YYYY-MM-DD HH:MM:SS
```

Example: `SETTIME 2026-04-14 12:34:56`

The STM32 responds with a time-ack frame (`T`). The ESP32 also accepts `TIME YYYY-MM-DD HH:MM:SS` as a compatible alternative.

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
  "protocol": "stm32-compact-v2",
  "schema_version": 2,
  "message": "measurement",
  "frame": "M",
  "rx_ms": 12345,
  "parse_ok": true,
  "raw_line": "M,1,20260401,143025,123456,234567,...",
  "field_count": 102,
  "extra_field_count": 0,
  "parse_warnings": [],
  "extra_fields": [],
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
  "spo2": 98,
  ...
  "ecg_valid": true,
  "ecg_hr": 71,
  "ecg_lead_off": 0,
  "ecg_filtered": -12,
  "ptt_valid": true,
  "ptt_ms": 245,
  "signal_quality": 85,
  "raw_signal_present": true,
  ...
  "modules": {
    "timestamp": { ... },
    "raw_ppg": { ... },
    "bpm": { ... },
    "ecg": {
      "available": true,
      "valid": true,
      "hr": 71,
      "lead_off": 0,
      "filtered": -12,
      "sample_count": 1000,
      ...
    },
    "crash": { ... },
    "task_phase": { ... },
    "task_stack": { ... },
    "task_heartbeat": { ... }
  }
}
```

### Time-ack / 时间应答

```json
{
  "bridge": "esp32c3",
  "source": "stm32",
  "channel": "uart1",
  "protocol": "stm32-compact-v2",
  "schema_version": 2,
  "message": "rtc_set_ack",
  "frame": "T",
  "rx_ms": 12345,
  "parse_ok": true,
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
  "protocol": "stm32-compact-v2",
  "schema_version": 2,
  "message": "parse_error",
  "frame": "?",
  "parse_ok": false,
  "error": "empty_frame",
  "raw_line": ""
}
```

## Log Output / 日志输出

All modules share the tag `PULSEOX_ESP`. Health status is reported every 10 seconds:

```
I (12345) PULSEOX_ESP: STATUS[periodic] wifi=ok mqtt=ok uart=ok protocol=ok(last=M)
I (12345) PULSEOX_ESP: STATUS[periodic] uart_rx=150 uart_drop=0 uart_tx_queued=2 uart_tx_ok=2 uart_tx_fail=0
I (12345) PULSEOX_ESP: STATUS[periodic] mqtt_cmd=2 mqtt_pub_ok=150 mqtt_pub_fail=0 protocol_ok=150 protocol_err=0
```

## Tools / 工具

`tools/publish_pc_time.py` — Publishes the local PC system time to the MQTT command topic so the STM32 can sync its RTC. The default payload is `SETTIME YYYYMMDD HHMMSS`.

```bash
python tools/publish_pc_time.py --broker mqtt://172.20.10.4 --topic pulseox/cmd
```

To use a different command format:
```bash
python tools/publish_pc_time.py --template "TIME {date_dashed} {time_colon}"
```

## License / 许可证

MIT
