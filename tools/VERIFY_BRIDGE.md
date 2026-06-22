# ESP32 桥接链路验证方法

这套验证方法按当前 ESP32 程序实现来测，不依赖根目录 README 的协议说明。依据的代码路径是：

- `main/app_config.h`：UART 引脚、波特率、MQTT broker/topic。
- `main/uart_bridge.c`：UART 按 `\n` 成帧，MQTT 下行命令入队后从 UART TX 发出。
- `main/network.c`：UART 解析后的 JSON 发布到 `MQTT_PUBLISH_TOPIC`，`MQTT_COMMAND_TOPIC` 收到的数据转给 UART。
- `main/usb_transport.c`：`GUI_USB_START` 激活 USB 上行，其他 USB 行作为下行命令透传到 UART TX。
- `main/stm32_protocol.c`：解析 `M`/`T` 文本帧并生成 JSON。

这套验证方法按风险从低到高分三层：

1. PC 上验证协议解析，不需要 ESP32。
2. ESP32 内部生成测试帧，验证 MQTT 下行触发、协议解析、MQTT 上行，不依赖 STM32。
3. 不拆线观察真实 STM32 -> ESP32 -> MQTT 上行。
4. USB 回环验证 ESP32 的 UART 收发、协议解析和 USB 上行，不需要 WiFi/MQTT。
5. UART 注入 + MQTT 订阅验证真实上行链路，再用 MQTT 下行验证命令透传。

## 0. 准备

固件当前关键配置来自 `main/app_config.h`：

| 项目 | 当前值 |
| --- | --- |
| STM32 UART RX | GPIO3 |
| STM32 UART TX | GPIO7 |
| UART 波特率 | 115200 |
| MQTT broker | `mqtt://172.20.10.4` |
| MQTT 上行 topic | `pulseox/data` |
| MQTT 下行 topic | `pulseox/cmd` |

串口验证需要 Python 的 `pyserial`：

```powershell
python -m pip install pyserial
```

脚本会自动从 `main/app_config.h` 读取 broker、topic 和波特率。若现场配置不同，可以用命令行参数覆盖。

## 1. 协议解析单元测试

这一步只验证 `main/stm32_protocol.c`，适合每次改协议字段后先跑。

```powershell
python .\tools\test_stm32_protocol_parser.py
```

需要 PATH 中有 native C 编译器：`gcc`、`clang`、`cc`、`zig cc`，或在 VS Developer PowerShell 里使用 `cl.exe`。如果没有 C 编译器，脚本会提示跳过；这不代表固件失败，只代表本机缺少 PC 侧编译环境。

## 2. ESP32 MQTT 自检（不依赖 STM32）

用途：不拆线、不短接 RX/TX、不需要 STM32 发数据。PC 往 `pulseox/cmd` 发布 JSON 命令
`{"cmd":"ESP_SELFTEST_MQTT"}`，ESP32 收到后内部生成一条固定 110 字段 `M` 帧，调用当前协议解析逻辑，
再发布 JSON 到 `pulseox/data`。

这一步验证的是：

```text
PC -> MQTT command topic -> ESP32 MQTT RX -> 内部测试帧 -> 协议解析 -> MQTT publish topic -> PC
```

前提：

- ESP32 已连接 WiFi 和 MQTT broker。
- PC 能访问同一个 MQTT broker。
- 固件需要包含 `MQTT_SELFTEST_COMMAND` 这次改动，重新 build/flash 后才有。

运行：

```powershell
python .\tools\verify_bridge_flow.py mqtt-selftest
```

MQTTX 手动触发时：

```text
topic: pulseox/cmd
payload: {"cmd":"ESP_SELFTEST_MQTT"}
```

然后订阅 `pulseox/data` 查看 ESP32 返回的 JSON。

如果 broker 和 `main/app_config.h` 不同：

```powershell
python .\tools\verify_bridge_flow.py mqtt-selftest --broker mqtt://192.168.1.10
```

通过标准：

```text
PASS: ESP32 MQTT self-test command produced the expected uplink JSON
```

注意：这一步不经过 UART 硬件，所以不能证明 GPIO3/GPIO7 接线正确。它用来先确认 ESP32 的
WiFi、MQTT 订阅、协议解析、MQTT 发布这些软件链路是通的。

## 3. 在线观察测试（不拆线）

用途：保持 STM32 原接线不动，直接观察 ESP32 是否把真实 STM32 数据解析后发布到 MQTT。

前提：

- STM32 正在向 ESP32 发串口帧。
- ESP32 已连接 WiFi 和 MQTT broker。
- PC 能访问同一个 MQTT broker。

运行：

```powershell
python .\tools\verify_bridge_flow.py mqtt-watch --count 3
```

如果 broker 和 `main/app_config.h` 不同：

```powershell
python .\tools\verify_bridge_flow.py mqtt-watch --broker mqtt://192.168.1.10 --count 3
```

通过标准：

```text
PASS: observed 3 valid MQTT payload(s)
```

这一步不主动给 ESP32 注入数据，所以它验证的是“当前真实系统正在上报且 JSON 结构正确”。如果 STM32 此刻没有发送数据，脚本会超时。

## 4. USB 回环快速验证

用途：不接 STM32，不接 broker，快速验证 ESP32 固件里的这条路径：

```text
PC USB -> ESP32 USB RX -> UART TX -> GPIO7/GPIO3 回环 -> UART RX -> 协议解析 -> USB JSON
```

接线：

```text
ESP32 GPIO7(TX)  ->  ESP32 GPIO3(RX)
GND              ->  GND
```

运行：

```powershell
python .\tools\verify_bridge_flow.py usb-loopback --usb-port COM12
```

把 `COM12` 换成 ESP32 的 USB Serial/JTAG 端口。运行时不要同时打开 `idf.py monitor` 占用同一个端口。

通过标准：

```text
PASS: USB->UART loopback was parsed and returned as USB JSON
```

这一步能发现：

- UART TX/RX 引脚或波特率错误
- UART 按行收包异常
- 110 字段 M 帧解析异常
- USB 会话上行 JSON 异常

注意：这个模式会先发送 `GUI_USB_START` 激活 USB 上行。测试结束默认发送 `GUI_USB_STOP` 切回 MQTT。

## 5. MQTT 上行验证

用途：模拟 STM32 往 ESP32 发一帧，检查 ESP32 是否发布正确 JSON 到 `pulseox/data`。

接线建议用一个 USB-TTL 模块：

```text
USB-TTL TX  ->  ESP32 GPIO3(RX)
USB-TTL GND ->  ESP32 GND
```

ESP32 需要已经烧录固件，并能连接 WiFi 和 MQTT broker。

运行：

```powershell
python .\tools\verify_bridge_flow.py mqtt-uplink --stimulus-port COM15
```

如果 broker 地址和固件不同：

```powershell
python .\tools\verify_bridge_flow.py mqtt-uplink --stimulus-port COM15 --broker mqtt://192.168.1.10
```

通过标准：

```text
PASS: UART stimulus was parsed and published to MQTT
```

脚本会订阅上行 topic，发送一条带唯一标记的 110 字段 M 帧，并只接受 `raw_line` 完全匹配的 JSON，避免误判旧消息。

## 6. MQTT 下行验证

用途：检查 `pulseox/cmd` 上的任意 payload 是否被 ESP32 透传到 UART TX。ESP32 当前不解析
`SETTIME` 语义，时间命令格式属于 STM32 侧协议。

接线：

```text
USB-TTL RX  ->  ESP32 GPIO7(TX)
USB-TTL GND ->  ESP32 GND
```

运行：

```powershell
python .\tools\verify_bridge_flow.py mqtt-downlink --uart-rx-port COM15
```

自定义命令：

```powershell
python .\tools\verify_bridge_flow.py mqtt-downlink --uart-rx-port COM15 --command "SETTIME 2026-06-13 21:45:00"
```

通过标准：

```text
PASS: MQTT command was forwarded to UART TX
```

固件当前配置 `UART_APPEND_NEWLINE_ON_MQTT_COMMAND=1`，所以下行命令没有换行时，ESP32 会在 UART 发送时自动补 `\n`。

## 7. 失败时怎么定位

| 现象 | 优先检查 |
| --- | --- |
| `mqtt-selftest` 超时 | ESP32 是否已重新烧录新固件、WiFi/MQTT 是否 connected、PC 和 ESP32 是否连同一个 broker |
| `mqtt-watch` 超时 | STM32 是否正在发帧、ESP32 是否连上 WiFi/MQTT、PC 是否能访问 broker |
| `usb-loopback` 超时 | GPIO7 到 GPIO3 是否短接、USB 端口是否被 monitor 占用、固件是否启用 `USB_TRANSPORT_ENABLED` |
| `mqtt-uplink` 收不到消息 | ESP32 WiFi/MQTT 是否 connected、broker IP 是否正确、USB-TTL TX 是否接到 GPIO3 |
| 收到 MQTT 但校验失败 | 协议字段顺序、JSON 字段名、schema_version 是否被改动 |
| `mqtt-downlink` 没有 UART 输出 | ESP32 是否订阅 `pulseox/cmd`、USB-TTL RX 是否接到 GPIO7、broker 是否同一个 |
| PC 打不开 COM 口 | 端口被 `idf.py monitor` 或其他串口工具占用 |

推荐日常顺序：

```text
协议字段变更 -> 跑 test_stm32_protocol_parser.py
不依赖 STM32 -> 跑 mqtt-selftest
不想拆线     -> 跑 mqtt-watch
固件改动     -> 跑 usb-loopback
联调现场     -> 跑 mqtt-uplink 和 mqtt-downlink
```
