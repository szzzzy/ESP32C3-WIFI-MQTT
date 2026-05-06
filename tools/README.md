# PC Time Sync

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
`main/hello_world.c`:

```powershell
python .\tools\publish_pc_time.py
```

## Default frame assumption

The default payload template is:

```text
T,{date},{time}
```

That becomes:

```text
T,20260407,153045
```

If the STM32 expects a different set-time frame, override it with `--template`:

```powershell
python .\tools\publish_pc_time.py --template "RTC,{date},{time}"
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
