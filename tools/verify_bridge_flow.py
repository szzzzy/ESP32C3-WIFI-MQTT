#!/usr/bin/env python3
"""Hardware-in-the-loop checks for the ESP32 STM32/MQTT bridge.

The MQTT client in this file intentionally uses only the Python standard
library. Serial-port access needs pyserial because Windows COM ports do not
have a practical cross-platform standard-library API.
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import socket
import sys
import time
from dataclasses import dataclass
from typing import Any
from urllib.parse import urlparse


ROOT = pathlib.Path(__file__).resolve().parents[1]
APP_CONFIG = ROOT / "main" / "app_config.h"

DEFAULT_BROKER_URI = "mqtt://172.20.10.4"
DEFAULT_PUBLISH_TOPIC = "pulseox/data"
DEFAULT_COMMAND_TOPIC = "pulseox/cmd"
DEFAULT_SELFTEST_COMMAND = "ESP_SELFTEST_MQTT"
DEFAULT_SETTIME_COMMAND = "SETTIME 2026-06-13 21:45:00"
DEFAULT_UART_BAUD = 115200

CONNACK_ERRORS = {
    1: "unacceptable protocol version",
    2: "identifier rejected",
    3: "server unavailable",
    4: "bad username or password",
    5: "not authorized",
}

DEFINE_RE = re.compile(
    r"#define\s+"
    r"(MQTT_BROKER_URI|MQTT_PUBLISH_TOPIC|MQTT_COMMAND_TOPIC|MQTT_SELFTEST_COMMAND|UART_BAUD)\s+"
    r'(?:"([^"]+)"|([0-9]+))'
)

SELFTEST_MEASUREMENT_LINE = (
    "M,1,20260613,120000,"
    "111111,222222,1800,1,"
    "1,72,"
    "1,98,"
    "1,18,"
    "1,833,"
    "1,830,42,35,"
    "0,5,"
    "30,40,75,0,"
    "1,1234,567,218,"
    "85,1,123,45,5000,3000,"
    "1,920,1,"
    "50000,1500,800,200,3000,2500,3,2,"
    "0,0,0,0,0,0,"
    "100,5,0,2,123456,50,10,0,"
    "1,1,1,1,3,0,1024000,"
    "500,123457,0,1,"
    "1,71,845,0,123458,-12,"
    "1,245,"
    "1000,0,0,0,0,"
    "0,0,0,0,0,5,1,"
    "3,2,1,0,"
    "512,256,128,64,"
    "999,500,"
    "80,0,1024,512,120,900,450,768"
)


@dataclass
class FirmwareDefaults:
    broker_uri: str = DEFAULT_BROKER_URI
    publish_topic: str = DEFAULT_PUBLISH_TOPIC
    command_topic: str = DEFAULT_COMMAND_TOPIC
    selftest_command: str = DEFAULT_SELFTEST_COMMAND
    uart_baud: int = DEFAULT_UART_BAUD


def discover_firmware_defaults() -> FirmwareDefaults:
    defaults = FirmwareDefaults()

    try:
        text = APP_CONFIG.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return defaults

    for key, string_value, number_value in DEFINE_RE.findall(text):
        value = string_value or number_value
        if key == "MQTT_BROKER_URI":
            defaults.broker_uri = value
        elif key == "MQTT_PUBLISH_TOPIC":
            defaults.publish_topic = value
        elif key == "MQTT_COMMAND_TOPIC":
            defaults.command_topic = value
        elif key == "MQTT_SELFTEST_COMMAND":
            defaults.selftest_command = value
        elif key == "UART_BAUD":
            defaults.uart_baud = int(value)

    return defaults


def build_measurement_frame() -> str:
    """Generate a full 110-field M frame with a per-run unique marker."""
    now = time.localtime()
    run_marker = int(time.time() * 1000) % 900000 + 100000
    date_text = time.strftime("%Y%m%d", now)
    time_text = time.strftime("%H%M%S", now)

    fields = [
        "M",
        "1", date_text, time_text,
        str(run_marker), str(run_marker + 1000), "1800", "1",
        "1", "72",
        "1", "98",
        "1", "18",
        "1", "833",
        "1", "830", "42", "35",
        "0", "5",
        "30", "40", "75", "0",
        "1", "1234", "567", "218",
        "85", "1", "123", "45", "5000", "3000",
        "1", "920", "1",
        "50000", "1500", "800", "200", "3000", "2500", "3", "2",
        "0", "0", "0", "0", "0", "0",
        "100", "5", "0", "2", str(run_marker), "50", "10", "0",
        "1", "1", "1", "1", "3", "0", "1024000",
        "500", str(run_marker + 1), "0", "1",
        "1", "71", "845", "0", str(run_marker + 2), "-12",
        "1", "245",
        "1000", "0", "0", "0", "0",
        "0", "0", "0", "0", "0", "5", "1",
        "3", "2", "1", "0",
        "512", "256", "128", "64",
        str(run_marker + 3), str(run_marker + 4),
        "80", "0", "1024", "512", "120", "900", "450", "768",
    ]
    if len(fields) != 110:
        raise AssertionError(f"sample frame field count changed: {len(fields)}")
    return ",".join(fields)


def build_time_ack_frame() -> str:
    return "T,1,1,20260515,143025,verify_bridge_flow"


def validate_bridge_payload(payload: dict[str, Any], expected_line: str) -> list[str]:
    errors: list[str] = []

    expected_pairs = {
        "schema_version": 3,
        "message": "measurement",
        "frame": "M",
        "parse_ok": True,
        "field_count": 110,
        "raw_line": expected_line,
        "bpm": 72,
        "spo2": 98,
        "signal_quality": 85,
        "ecg_filtered": -12,
        "ptt_ms": 245,
    }

    for key, expected in expected_pairs.items():
        if payload.get(key) != expected:
            errors.append(f"{key}: expected {expected!r}, got {payload.get(key)!r}")

    if payload.get("extra_field_count") != 0:
        errors.append(f"extra_field_count: expected 0, got {payload.get('extra_field_count')!r}")
    if payload.get("parse_warnings") != []:
        errors.append(f"parse_warnings: expected [], got {payload.get('parse_warnings')!r}")
    if payload.get("extra_fields") != []:
        errors.append(f"extra_fields: expected [], got {payload.get('extra_fields')!r}")

    modules = payload.get("modules")
    if not isinstance(modules, dict):
        errors.append("modules: missing object")
        return errors

    for module_name in ["timestamp", "raw_ppg", "bpm", "spo2", "ecg", "ptt"]:
        module = modules.get(module_name)
        if not isinstance(module, dict) or module.get("available") is not True:
            errors.append(f"modules.{module_name}.available: expected true")

    return errors


def validate_live_payload(payload: dict[str, Any], allow_parse_error: bool) -> list[str]:
    errors: list[str] = []

    if payload.get("schema_version") != 3:
        errors.append(f"schema_version: expected 3, got {payload.get('schema_version')!r}")

    message = payload.get("message")
    frame = payload.get("frame")
    parse_ok = payload.get("parse_ok")

    if allow_parse_error and message == "parse_error":
        if parse_ok is not False:
            errors.append("parse_error payload should have parse_ok=false")
        if not isinstance(payload.get("error"), str):
            errors.append("parse_error payload missing string error")
        if not isinstance(payload.get("raw_line"), str):
            errors.append("parse_error payload missing raw_line")
        if not isinstance(payload.get("field_count"), int):
            errors.append("parse_error payload missing integer field_count")
        if not isinstance(payload.get("parse_warnings"), list):
            errors.append("parse_error payload missing parse_warnings array")
        if not isinstance(payload.get("extra_fields"), list):
            errors.append("parse_error payload missing extra_fields array")
        return errors

    if parse_ok is not True:
        errors.append(f"parse_ok: expected true, got {parse_ok!r}")

    if message == "measurement":
        if frame != "M":
            errors.append(f"frame: expected 'M' for measurement, got {frame!r}")
        if not isinstance(payload.get("raw_line"), str) or not payload.get("raw_line"):
            errors.append("raw_line: missing non-empty string")
        if not isinstance(payload.get("field_count"), int):
            errors.append("field_count: missing integer")
        if not isinstance(payload.get("parse_warnings"), list):
            errors.append("parse_warnings: missing array")
        if not isinstance(payload.get("extra_fields"), list):
            errors.append("extra_fields: missing array")
        if not isinstance(payload.get("extra_field_count"), int):
            errors.append("extra_field_count: missing integer")

        modules = payload.get("modules")
        if not isinstance(modules, dict):
            errors.append("modules: missing object")
        else:
            for module_name in ["raw_ppg", "bpm", "spo2"]:
                module = modules.get(module_name)
                if module is not None and not isinstance(module, dict):
                    errors.append(f"modules.{module_name}: expected object when present")
    elif message == "rtc_set_ack":
        if frame != "T":
            errors.append(f"frame: expected 'T' for rtc_set_ack, got {frame!r}")
        if not isinstance(payload.get("raw_line"), str):
            errors.append("raw_line: missing string")
        if not isinstance(payload.get("field_count"), int):
            errors.append("field_count: missing integer")
        if not isinstance(payload.get("parse_warnings"), list):
            errors.append("parse_warnings: missing array")
        if not isinstance(payload.get("extra_fields"), list):
            errors.append("extra_fields: missing array")
        for key in ["set_ok", "rtc_valid"]:
            if not isinstance(payload.get(key), bool):
                errors.append(f"{key}: missing boolean")
    else:
        errors.append(f"message: expected measurement or rtc_set_ack, got {message!r}")

    if not isinstance(payload.get("rx_ms"), int):
        errors.append("rx_ms: missing integer")

    return errors


def encode_utf8_field(value: str) -> bytes:
    data = value.encode("utf-8")
    if len(data) > 0xFFFF:
        raise ValueError("MQTT UTF-8 field is too long")
    return len(data).to_bytes(2, "big") + data


def encode_remaining_length(value: int) -> bytes:
    if value < 0:
        raise ValueError("remaining length must be non-negative")

    encoded = bytearray()
    while True:
        digit = value % 128
        value //= 128
        if value > 0:
            digit |= 0x80
        encoded.append(digit)
        if value == 0:
            return bytes(encoded)


def recv_exact(sock: socket.socket, size: int) -> bytes:
    chunks = bytearray()
    while len(chunks) < size:
        chunk = sock.recv(size - len(chunks))
        if not chunk:
            raise ConnectionError("socket closed before packet was fully received")
        chunks.extend(chunk)
    return bytes(chunks)


def recv_packet(sock: socket.socket) -> tuple[int, bytes]:
    first_byte = recv_exact(sock, 1)[0]

    multiplier = 1
    remaining_length = 0
    for _ in range(4):
        digit = recv_exact(sock, 1)[0]
        remaining_length += (digit & 0x7F) * multiplier
        if (digit & 0x80) == 0:
            break
        multiplier *= 128
    else:
        raise ConnectionError("malformed MQTT remaining length")

    return first_byte, recv_exact(sock, remaining_length)


def build_connect_packet(
    client_id: str,
    keepalive: int,
    username: str | None,
    password: str | None,
) -> bytes:
    if password is not None and username is None:
        raise ValueError("password cannot be used without username")

    flags = 0x02
    payload = bytearray(encode_utf8_field(client_id))

    if username is not None:
        flags |= 0x80
        payload.extend(encode_utf8_field(username))
    if password is not None:
        flags |= 0x40
        payload.extend(encode_utf8_field(password))

    variable_header = (
        encode_utf8_field("MQTT")
        + bytes([0x04, flags])
        + keepalive.to_bytes(2, "big")
    )
    remaining = variable_header + bytes(payload)
    return b"\x10" + encode_remaining_length(len(remaining)) + remaining


def build_subscribe_packet(packet_id: int, topic: str, qos: int) -> bytes:
    variable_header = packet_id.to_bytes(2, "big")
    payload = encode_utf8_field(topic) + bytes([qos])
    remaining = variable_header + payload
    return b"\x82" + encode_remaining_length(len(remaining)) + remaining


def build_publish_packet(topic: str, payload: str, retain: bool) -> bytes:
    fixed_header = bytes([0x31 if retain else 0x30])
    payload_bytes = payload.encode("utf-8")
    remaining = encode_utf8_field(topic) + payload_bytes
    return fixed_header + encode_remaining_length(len(remaining)) + remaining


class TinyMqttClient:
    def __init__(
        self,
        broker_uri: str,
        client_id: str,
        username: str | None,
        password: str | None,
        keepalive: int,
        timeout: float,
    ) -> None:
        broker = urlparse(broker_uri)
        if broker.scheme != "mqtt":
            raise ValueError("only plain mqtt:// broker URIs are supported")
        if not broker.hostname:
            raise ValueError(f"broker host is missing in URI: {broker_uri!r}")

        self.host = broker.hostname
        self.port = broker.port or 1883
        self.username = username if username is not None else broker.username
        self.password = password if password is not None else broker.password
        self.client_id = client_id
        self.keepalive = keepalive
        self.timeout = timeout
        self.sock: socket.socket | None = None
        self.packet_id = 1

    def __enter__(self) -> "TinyMqttClient":
        self.connect()
        return self

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        self.close()

    def connect(self) -> None:
        sock = socket.create_connection((self.host, self.port), timeout=self.timeout)
        sock.settimeout(self.timeout)
        sock.sendall(
            build_connect_packet(
                self.client_id,
                self.keepalive,
                self.username,
                self.password,
            )
        )

        packet_type, packet_body = recv_packet(sock)
        if packet_type != 0x20 or len(packet_body) != 2:
            sock.close()
            raise ConnectionError("expected MQTT CONNACK from broker")
        if packet_body[1] != 0:
            reason = CONNACK_ERRORS.get(packet_body[1], f"return code {packet_body[1]}")
            sock.close()
            raise ConnectionError(f"broker rejected connection: {reason}")

        self.sock = sock

    def close(self) -> None:
        if self.sock is None:
            return
        try:
            self.sock.sendall(b"\xE0\x00")
        except OSError:
            pass
        self.sock.close()
        self.sock = None

    def subscribe(self, topic: str, qos: int = 0) -> None:
        sock = self._sock()
        packet_id = self._next_packet_id()
        sock.sendall(build_subscribe_packet(packet_id, topic, qos))

        deadline = time.monotonic() + self.timeout
        while True:
            remaining = max(0.1, deadline - time.monotonic())
            sock.settimeout(remaining)
            try:
                fixed_header, body = recv_packet(sock)
            except TimeoutError:
                raise
            except socket.timeout as exc:
                raise TimeoutError("timed out waiting for SUBACK") from exc
            packet_type = fixed_header >> 4
            if packet_type == 9:
                if len(body) < 3:
                    raise ConnectionError("malformed SUBACK")
                returned_id = int.from_bytes(body[:2], "big")
                if returned_id == packet_id:
                    if body[2] == 0x80:
                        raise ConnectionError(f"broker rejected subscription to {topic}")
                    return
            elif time.monotonic() >= deadline:
                raise TimeoutError("timed out waiting for SUBACK")

    def publish(self, topic: str, payload: str, retain: bool = False) -> None:
        self._sock().sendall(build_publish_packet(topic, payload, retain))

    def wait_for_json_payload(
        self,
        topic: str,
        expected_line: str,
        timeout: float,
    ) -> dict[str, Any]:
        deadline = time.monotonic() + timeout
        last_payload: str | None = None

        while time.monotonic() < deadline:
            remaining = max(0.1, deadline - time.monotonic())
            sock = self._sock()
            sock.settimeout(remaining)
            try:
                fixed_header, body = recv_packet(sock)
            except socket.timeout:
                break
            packet_type = fixed_header >> 4
            if packet_type != 3:
                continue

            publish_topic, payload_text = self._parse_publish(fixed_header, body)
            if publish_topic != topic:
                continue

            last_payload = payload_text
            try:
                payload = json.loads(payload_text)
            except json.JSONDecodeError:
                continue

            if payload.get("raw_line") == expected_line:
                return payload

        raise TimeoutError(
            "timed out waiting for matching MQTT payload"
            + (f"; last payload on topic was: {last_payload[:240]!r}" if last_payload else "")
        )

    def wait_for_next_json_payload(self, topic: str, timeout: float) -> dict[str, Any]:
        deadline = time.monotonic() + timeout
        last_payload: str | None = None

        while time.monotonic() < deadline:
            remaining = max(0.1, deadline - time.monotonic())
            sock = self._sock()
            sock.settimeout(remaining)
            try:
                fixed_header, body = recv_packet(sock)
            except socket.timeout:
                break

            packet_type = fixed_header >> 4
            if packet_type != 3:
                continue

            publish_topic, payload_text = self._parse_publish(fixed_header, body)
            if publish_topic != topic:
                continue

            last_payload = payload_text
            try:
                return json.loads(payload_text)
            except json.JSONDecodeError:
                continue

        raise TimeoutError(
            "timed out waiting for MQTT JSON payload"
            + (f"; last payload on topic was: {last_payload[:240]!r}" if last_payload else "")
        )

    def _parse_publish(self, fixed_header: int, body: bytes) -> tuple[str, str]:
        if len(body) < 2:
            raise ConnectionError("malformed PUBLISH packet")

        topic_len = int.from_bytes(body[:2], "big")
        if len(body) < 2 + topic_len:
            raise ConnectionError("malformed PUBLISH topic")

        topic = body[2 : 2 + topic_len].decode("utf-8", errors="replace")
        offset = 2 + topic_len
        qos = (fixed_header & 0x06) >> 1

        if qos:
            if len(body) < offset + 2:
                raise ConnectionError("malformed PUBLISH packet id")
            packet_id = body[offset : offset + 2]
            offset += 2
            if qos == 1:
                self._sock().sendall(b"\x40\x02" + packet_id)

        payload = body[offset:].decode("utf-8", errors="replace")
        return topic, payload

    def _next_packet_id(self) -> int:
        packet_id = self.packet_id
        self.packet_id += 1
        if self.packet_id > 0xFFFF:
            self.packet_id = 1
        return packet_id

    def _sock(self) -> socket.socket:
        if self.sock is None:
            raise ConnectionError("MQTT client is not connected")
        return self.sock


def import_serial_module() -> Any:
    try:
        import serial  # type: ignore[import-not-found]
    except ImportError as exc:
        raise SystemExit(
            "pyserial is required for serial-port checks. Install it with: "
            "python -m pip install pyserial"
        ) from exc
    return serial


def open_serial(port: str, baud: int, timeout: float) -> Any:
    serial = import_serial_module()
    return serial.Serial(
        port=port,
        baudrate=baud,
        timeout=timeout,
        write_timeout=timeout,
    )


def send_serial_line(port: str, baud: int, line: str, settle: float) -> None:
    with open_serial(port, baud, timeout=2.0) as ser:
        if settle > 0:
            time.sleep(settle)
        ser.write(line.encode("utf-8") + b"\n")
        ser.flush()


def run_mqtt_uplink(args: argparse.Namespace) -> int:
    line = args.line or build_measurement_frame()
    client_id = args.client_id or f"verify-uplink-{os.getpid()}"

    print(f"broker        : {args.broker}")
    print(f"publish topic : {args.publish_topic}")
    print(f"stimulus port : {args.stimulus_port} @ {args.baud}")
    print(f"frame bytes   : {len(line.encode('utf-8'))}")

    with TinyMqttClient(
        args.broker,
        client_id,
        args.username,
        args.password,
        args.keepalive,
        args.timeout,
    ) as mqtt:
        mqtt.subscribe(args.publish_topic, qos=0)
        send_serial_line(args.stimulus_port, args.baud, line, args.settle)
        payload = mqtt.wait_for_json_payload(args.publish_topic, line, args.timeout)

    errors = validate_bridge_payload(payload, line)
    if errors:
        print("FAIL: MQTT payload did not match expected bridge JSON")
        for error in errors:
            print(f"  - {error}")
        return 1

    print("PASS: UART stimulus was parsed and published to MQTT")
    print(f"rx_ms={payload.get('rx_ms')} bpm={payload.get('bpm')} spo2={payload.get('spo2')}")
    return 0


def run_mqtt_watch(args: argparse.Namespace) -> int:
    client_id = args.client_id or f"verify-watch-{os.getpid()}"
    valid_count = 0

    print(f"broker        : {args.broker}")
    print(f"publish topic : {args.publish_topic}")
    print(f"target count  : {args.count}")
    print("mode          : observe real STM32->ESP32->MQTT traffic")

    with TinyMqttClient(
        args.broker,
        client_id,
        args.username,
        args.password,
        args.keepalive,
        args.timeout,
    ) as mqtt:
        mqtt.subscribe(args.publish_topic, qos=0)
        while valid_count < args.count:
            payload = mqtt.wait_for_next_json_payload(args.publish_topic, args.timeout)
            errors = validate_live_payload(payload, args.allow_parse_error)
            if errors:
                print("FAIL: observed MQTT JSON does not match bridge schema")
                for error in errors:
                    print(f"  - {error}")
                print(f"payload message={payload.get('message')!r} frame={payload.get('frame')!r}")
                return 1

            valid_count += 1
            summary = (
                f"[{valid_count}/{args.count}] "
                f"message={payload.get('message')} frame={payload.get('frame')} "
                f"parse_ok={payload.get('parse_ok')} rx_ms={payload.get('rx_ms')}"
            )
            if payload.get("message") == "measurement":
                summary += (
                    f" fields={payload.get('field_count')} "
                    f"bpm={payload.get('bpm')} spo2={payload.get('spo2')}"
                )
            elif payload.get("message") == "rtc_set_ack":
                summary += (
                    f" set_ok={payload.get('set_ok')} "
                    f"rtc_valid={payload.get('rtc_valid')}"
                )
            print(summary)

    print(f"PASS: observed {valid_count} valid MQTT payload(s)")
    return 0


def run_mqtt_selftest(args: argparse.Namespace) -> int:
    client_id = args.client_id or f"verify-selftest-{os.getpid()}"
    command_payload = (
        args.selftest_command
        if args.plain_command
        else json.dumps({"cmd": args.selftest_command}, separators=(",", ":"))
    )

    print(f"broker        : {args.broker}")
    print(f"command topic : {args.command_topic}")
    print(f"publish topic : {args.publish_topic}")
    print(f"command       : {command_payload}")
    print("mode          : ESP32 synthetic measurement, no STM32/UART wiring required")

    with TinyMqttClient(
        args.broker,
        client_id,
        args.username,
        args.password,
        args.keepalive,
        args.timeout,
    ) as mqtt:
        mqtt.subscribe(args.publish_topic, qos=0)
        mqtt.publish(args.command_topic, command_payload)
        payload = mqtt.wait_for_json_payload(
            args.publish_topic,
            SELFTEST_MEASUREMENT_LINE,
            args.timeout,
        )

    errors = validate_bridge_payload(payload, SELFTEST_MEASUREMENT_LINE)
    if errors:
        print("FAIL: self-test MQTT payload did not match expected bridge JSON")
        for error in errors:
            print(f"  - {error}")
        return 1

    print("PASS: ESP32 MQTT self-test command produced the expected uplink JSON")
    print(f"rx_ms={payload.get('rx_ms')} bpm={payload.get('bpm')} spo2={payload.get('spo2')}")
    return 0


def read_json_line_from_serial(ser: Any, expected_line: str, timeout: float) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    last_line = ""

    while time.monotonic() < deadline:
        raw = ser.readline(65536)
        if not raw:
            continue

        text = raw.decode("utf-8", errors="replace").strip()
        if not text:
            continue

        last_line = text
        json_start = text.find("{")
        if json_start < 0:
            continue

        candidate = text[json_start:]
        try:
            payload = json.loads(candidate)
        except json.JSONDecodeError:
            continue

        if payload.get("raw_line") == expected_line:
            return payload

    raise TimeoutError(
        "timed out waiting for matching USB JSON line"
        + (f"; last serial line was: {last_line[:240]!r}" if last_line else "")
    )


def run_usb_loopback(args: argparse.Namespace) -> int:
    line = args.line or build_measurement_frame()

    print(f"usb port    : {args.usb_port} @ {args.baud}")
    print("wiring      : GPIO7(TX) -> GPIO3(RX), common GND")
    print(f"frame bytes : {len(line.encode('utf-8'))}")

    with open_serial(args.usb_port, args.baud, timeout=0.1) as ser:
        if args.settle > 0:
            time.sleep(args.settle)
        ser.reset_input_buffer()
        ser.write(b"GUI_USB_START\n")
        ser.flush()
        time.sleep(0.2)
        ser.write(line.encode("utf-8") + b"\n")
        ser.flush()
        payload = read_json_line_from_serial(ser, line, args.timeout)
        if args.stop_session:
            ser.write(b"GUI_USB_STOP\n")
            ser.flush()

    errors = validate_bridge_payload(payload, line)
    if errors:
        print("FAIL: USB loopback payload did not match expected bridge JSON")
        for error in errors:
            print(f"  - {error}")
        return 1

    print("PASS: USB->UART loopback was parsed and returned as USB JSON")
    print(f"rx_ms={payload.get('rx_ms')} bpm={payload.get('bpm')} spo2={payload.get('spo2')}")
    return 0


def run_mqtt_downlink(args: argparse.Namespace) -> int:
    command = args.command
    client_id = args.client_id or f"verify-downlink-{os.getpid()}"

    print(f"broker        : {args.broker}")
    print(f"command topic : {args.command_topic}")
    print(f"uart rx port  : {args.uart_rx_port} @ {args.baud}")
    print(f"command       : {command}")

    with open_serial(args.uart_rx_port, args.baud, timeout=0.1) as ser:
        ser.reset_input_buffer()
        with TinyMqttClient(
            args.broker,
            client_id,
            args.username,
            args.password,
            args.keepalive,
            args.timeout,
        ) as mqtt:
            mqtt.publish(args.command_topic, command)

        deadline = time.monotonic() + args.timeout
        last_line = ""
        while time.monotonic() < deadline:
            raw = ser.readline(4096)
            if not raw:
                continue
            last_line = raw.decode("utf-8", errors="replace").strip()
            if last_line == command:
                print("PASS: MQTT command was forwarded to UART TX")
                return 0

    print("FAIL: did not observe command on UART TX")
    if last_line:
        print(f"last line: {last_line!r}")
    return 1


def run_generate(args: argparse.Namespace) -> int:
    if args.kind == "measurement":
        line = build_measurement_frame()
    else:
        line = build_time_ack_frame()

    print(line)
    print(f"field_count: {line.count(',') + 1}")
    print(f"bytes      : {len(line.encode('utf-8'))}")
    return 0


def add_mqtt_options(parser: argparse.ArgumentParser, defaults: FirmwareDefaults) -> None:
    parser.add_argument("--broker", default=defaults.broker_uri, help="MQTT broker URI")
    parser.add_argument("--username", help="MQTT username")
    parser.add_argument("--password", help="MQTT password")
    parser.add_argument("--client-id", help="MQTT client id")
    parser.add_argument("--keepalive", type=int, default=30, help="MQTT keepalive seconds")
    parser.add_argument("--timeout", type=float, default=8.0, help="timeout seconds")


def parse_args(argv: list[str]) -> argparse.Namespace:
    defaults = discover_firmware_defaults()
    parser = argparse.ArgumentParser(
        description="Verify ESP32 STM32 UART bridge flows from a PC."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    generate = subparsers.add_parser("generate", help="print a sample STM32 frame")
    generate.add_argument(
        "--kind",
        choices=["measurement", "time-ack"],
        default="measurement",
        help="sample frame kind",
    )
    generate.set_defaults(func=run_generate)

    uplink = subparsers.add_parser(
        "mqtt-uplink",
        help="send a STM32 frame over UART and verify MQTT uplink JSON",
    )
    add_mqtt_options(uplink, defaults)
    uplink.add_argument("--publish-topic", default=defaults.publish_topic)
    uplink.add_argument("--stimulus-port", required=True, help="COM port wired to ESP32 UART RX")
    uplink.add_argument("--baud", type=int, default=defaults.uart_baud)
    uplink.add_argument("--settle", type=float, default=0.0, help="seconds to wait after opening serial")
    uplink.add_argument("--line", help="custom STM32 line to send instead of generated sample")
    uplink.set_defaults(func=run_mqtt_uplink)

    watch = subparsers.add_parser(
        "mqtt-watch",
        help="observe existing STM32->ESP32->MQTT traffic without rewiring",
    )
    add_mqtt_options(watch, defaults)
    watch.add_argument("--publish-topic", default=defaults.publish_topic)
    watch.add_argument("--count", type=int, default=1, help="valid payloads to observe")
    watch.add_argument(
        "--allow-parse-error",
        action="store_true",
        help="treat parse_error JSON as an acceptable observed payload",
    )
    watch.set_defaults(func=run_mqtt_watch)

    selftest = subparsers.add_parser(
        "mqtt-selftest",
        help="trigger ESP32 synthetic MQTT self-test without STM32 or rewiring",
    )
    add_mqtt_options(selftest, defaults)
    selftest.add_argument("--publish-topic", default=defaults.publish_topic)
    selftest.add_argument("--command-topic", default=defaults.command_topic)
    selftest.add_argument("--selftest-command", default=defaults.selftest_command)
    selftest.add_argument(
        "--plain-command",
        action="store_true",
        help="send the legacy plain-text self-test command instead of JSON",
    )
    selftest.set_defaults(func=run_mqtt_selftest)

    usb = subparsers.add_parser(
        "usb-loopback",
        help="activate USB session, send frame through UART loopback, verify USB JSON",
    )
    usb.add_argument("--usb-port", required=True, help="ESP32 USB Serial/JTAG COM port")
    usb.add_argument("--baud", type=int, default=defaults.uart_baud)
    usb.add_argument("--timeout", type=float, default=8.0, help="timeout seconds")
    usb.add_argument("--settle", type=float, default=1.0, help="seconds to wait after opening USB")
    usb.add_argument("--line", help="custom STM32 line to send instead of generated sample")
    usb.add_argument("--no-stop", dest="stop_session", action="store_false", help="leave USB session active")
    usb.set_defaults(func=run_usb_loopback, stop_session=True)

    downlink = subparsers.add_parser(
        "mqtt-downlink",
        help="publish MQTT command and verify it appears on ESP32 UART TX",
    )
    add_mqtt_options(downlink, defaults)
    downlink.add_argument("--command-topic", default=defaults.command_topic)
    downlink.add_argument("--uart-rx-port", required=True, help="COM port wired to ESP32 UART TX")
    downlink.add_argument("--baud", type=int, default=defaults.uart_baud)
    downlink.add_argument("--command", default=DEFAULT_SETTIME_COMMAND)
    downlink.set_defaults(func=run_mqtt_downlink)

    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        return args.func(args)
    except (OSError, ValueError, ConnectionError, TimeoutError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
