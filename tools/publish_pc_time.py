#!/usr/bin/env python3
"""
Publish the current PC time to the STM32 through the existing MQTT bridge.

This script uses MQTT 3.1.1 over plain TCP and only depends on the Python
standard library. The default payload template assumes the STM32 time-set
command is:

    SETTIME YYYYMMDD HHMMSS

If the MCU expects a different frame, pass --template to override it.
"""

from __future__ import annotations

import argparse
import datetime as dt
import os
import pathlib
import re
import socket
import sys
from typing import Dict
from urllib.parse import urlparse


DEFAULT_BROKER_URI = "mqtt://172.20.10.4"
DEFAULT_TOPIC = "pulseox/cmd"
DEFAULT_TEMPLATE = "SETTIME {date} {time}"

DEFINE_RE = re.compile(
    r'#define\s+(MQTT_BROKER_URI|MQTT_COMMAND_TOPIC)\s+"([^"]+)"'
)

CONNACK_ERRORS = {
    1: "unacceptable protocol version",
    2: "identifier rejected",
    3: "server unavailable",
    4: "bad username or password",
    5: "not authorized",
}


def discover_firmware_defaults(repo_root: pathlib.Path) -> Dict[str, str]:
    defaults = {
        "broker_uri": DEFAULT_BROKER_URI,
        "topic": DEFAULT_TOPIC,
    }
    source_path = repo_root / "main" / "app_config.h"

    try:
        source_text = source_path.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return defaults

    for key, value in DEFINE_RE.findall(source_text):
        if key == "MQTT_BROKER_URI":
            defaults["broker_uri"] = value
        elif key == "MQTT_COMMAND_TOPIC":
            defaults["topic"] = value

    return defaults


def parse_args(argv: list[str]) -> argparse.Namespace:
    repo_root = pathlib.Path(__file__).resolve().parents[1]
    firmware_defaults = discover_firmware_defaults(repo_root)

    parser = argparse.ArgumentParser(
        description="Read local PC time and publish a STM32 set-time command over MQTT."
    )
    parser.add_argument(
        "--broker",
        default=firmware_defaults["broker_uri"],
        help="MQTT broker URI. Only plain mqtt://host[:port] is supported.",
    )
    parser.add_argument(
        "--topic",
        default=firmware_defaults["topic"],
        help="MQTT command topic forwarded by the ESP bridge.",
    )
    parser.add_argument(
        "--template",
        default=DEFAULT_TEMPLATE,
        help=(
            "Payload template. Supported placeholders: "
            "{date}, {date_dashed}, {time}, {time_colon}, {iso}."
        ),
    )
    parser.add_argument(
        "--date",
        help="Override date in YYYYMMDD format. Default: local PC date.",
    )
    parser.add_argument(
        "--time",
        dest="time_value",
        help="Override time in HHMMSS format. Default: local PC time.",
    )
    parser.add_argument(
        "--client-id",
        default=f"pc-time-sync-{os.getpid()}",
        help="MQTT client id used by this script.",
    )
    parser.add_argument(
        "--username",
        help="Optional MQTT username.",
    )
    parser.add_argument(
        "--password",
        help="Optional MQTT password.",
    )
    parser.add_argument(
        "--keepalive",
        type=int,
        default=30,
        help="MQTT keepalive seconds.",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=5.0,
        help="Socket timeout seconds.",
    )
    parser.add_argument(
        "--retain",
        action="store_true",
        help="Publish with the retain flag set.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print the payload and exit without connecting.",
    )
    return parser.parse_args(argv)


def require_fixed_digits(label: str, value: str, digits: int) -> str:
    if not re.fullmatch(rf"\d{{{digits}}}", value or ""):
        raise ValueError(f"{label} must be exactly {digits} digits, got: {value!r}")
    return value


def build_time_tokens(args: argparse.Namespace) -> Dict[str, str]:
    now = dt.datetime.now().astimezone()
    date_value = require_fixed_digits("date", args.date or now.strftime("%Y%m%d"), 8)
    time_value = require_fixed_digits(
        "time", args.time_value or now.strftime("%H%M%S"), 6
    )

    return {
        "date": date_value,
        "date_dashed": f"{date_value[:4]}-{date_value[4:6]}-{date_value[6:8]}",
        "time": time_value,
        "time_colon": f"{time_value[:2]}:{time_value[2:4]}:{time_value[4:6]}",
        "iso": now.isoformat(timespec="seconds"),
    }


def render_payload(template: str, tokens: Dict[str, str]) -> str:
    try:
        return template.format(**tokens)
    except KeyError as exc:
        raise ValueError(
            f"unknown template placeholder: {exc.args[0]!r}; "
            "supported placeholders are {date}, {date_dashed}, {time}, {time_colon}, {iso}"
        ) from exc


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

    connect_flags = 0x02
    payload = bytearray(encode_utf8_field(client_id))

    if username is not None:
        connect_flags |= 0x80
        payload.extend(encode_utf8_field(username))

    if password is not None:
        connect_flags |= 0x40
        payload.extend(encode_utf8_field(password))

    variable_header = (
        encode_utf8_field("MQTT")
        + bytes([0x04, connect_flags])
        + keepalive.to_bytes(2, "big")
    )
    remaining = variable_header + bytes(payload)
    return b"\x10" + encode_remaining_length(len(remaining)) + remaining


def build_publish_packet(topic: str, payload: str, retain: bool) -> bytes:
    fixed_header = bytes([0x31 if retain else 0x30])
    variable_header = encode_utf8_field(topic)
    payload_bytes = payload.encode("utf-8")
    remaining = variable_header + payload_bytes
    return fixed_header + encode_remaining_length(len(remaining)) + remaining


def publish_once(args: argparse.Namespace, payload: str) -> None:
    broker = urlparse(args.broker)
    if broker.scheme != "mqtt":
        raise ValueError("only plain mqtt:// broker URIs are supported")
    if not broker.hostname:
        raise ValueError(f"broker host is missing in URI: {args.broker!r}")

    port = broker.port or 1883
    username = args.username if args.username is not None else broker.username
    password = args.password if args.password is not None else broker.password
    connect_packet = build_connect_packet(
        client_id=args.client_id,
        keepalive=args.keepalive,
        username=username,
        password=password,
    )
    publish_packet = build_publish_packet(args.topic, payload, args.retain)

    with socket.create_connection((broker.hostname, port), timeout=args.timeout) as sock:
        sock.settimeout(args.timeout)
        sock.sendall(connect_packet)

        packet_type, packet_body = recv_packet(sock)
        if packet_type != 0x20 or len(packet_body) != 2:
            raise ConnectionError("expected MQTT CONNACK from broker")

        return_code = packet_body[1]
        if return_code != 0:
            reason = CONNACK_ERRORS.get(return_code, f"return code {return_code}")
            raise ConnectionError(f"broker rejected connection: {reason}")

        sock.sendall(publish_packet)
        sock.sendall(b"\xE0\x00")


def main(argv: list[str]) -> int:
    try:
        args = parse_args(argv)
        tokens = build_time_tokens(args)
        payload = render_payload(args.template, tokens)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    print(f"broker  : {args.broker}")
    print(f"topic   : {args.topic}")
    print(f"payload : {payload}")
    print(f"pc time : {tokens['iso']}")

    if args.dry_run:
        return 0

    try:
        publish_once(args, payload)
    except (OSError, ValueError, ConnectionError) as exc:
        print(f"publish failed: {exc}", file=sys.stderr)
        return 1

    print("publish ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
