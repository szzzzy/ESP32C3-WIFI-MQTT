# Tools

## Bridge verification

`verify_bridge_flow.py` provides repeatable checks for the current bridge
firmware:

- Generate a valid 110-field STM32 measurement frame.
- Trigger the ESP32 MQTT self-test without STM32 or rewiring.
- Observe the live STM32 -> ESP32 -> MQTT uplink without rewiring.
- Verify USB loopback: USB command -> UART TX/RX loopback -> parser -> USB JSON.
- Verify MQTT uplink: UART stimulus -> parser -> `pulseox/data`.
- Verify MQTT downlink: `pulseox/cmd` -> UART TX.

See the full workflow in [VERIFY_BRIDGE.md](VERIFY_BRIDGE.md).

Quick examples:

```powershell
python .\tools\verify_bridge_flow.py generate
python .\tools\verify_bridge_flow.py mqtt-selftest
python .\tools\verify_bridge_flow.py mqtt-watch --count 3
python .\tools\verify_bridge_flow.py usb-loopback --usb-port COM12
python .\tools\verify_bridge_flow.py mqtt-uplink --stimulus-port COM15
python .\tools\verify_bridge_flow.py mqtt-downlink --uart-rx-port COM15
```

`mqtt-selftest` publishes this JSON command to `pulseox/cmd`:

```json
{"cmd":"ESP_SELFTEST_MQTT"}
```

Serial-port modes require `pyserial`:

```powershell
python -m pip install pyserial
```

## PC Time Sync

`publish_pc_time.py` reads the current PC time, formats a STM32 set-time command,
and publishes it to the same MQTT command topic that the ESP bridge already
forwards to UART.

The script has no third-party dependencies. It only supports plain `mqtt://`
brokers and MQTT QoS 0 publishing.

## Quick start

Preview the command without sending it:

```powershell
python .\tools\publish_pc_time.py --dry-run
```

Send the current PC time using the firmware defaults discovered from
`main/app_config.h`:

```powershell
python .\tools\publish_pc_time.py
```

## Time command

The payload is always:

```text
SETTIME YYYY-MM-DD HH:MM:SS
```

For example:

```text
SETTIME 2026-04-07 15:30:45
```

## Useful options

Use a different broker or topic:

```powershell
python .\tools\publish_pc_time.py --broker mqtt://192.168.1.10:1883 --topic pulseox/cmd
```

Override the date or time for testing:

```powershell
python .\tools\publish_pc_time.py --date 20260407 --time 235959 --dry-run
```

Use broker credentials:

```powershell
python .\tools\publish_pc_time.py --username user --password secret
```
