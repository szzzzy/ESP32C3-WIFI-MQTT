#!/usr/bin/env python3
"""Host-side regression tests for the STM32 measurement parser (110-field v3).

The runner compiles main/stm32_protocol.c with STM32_PROTOCOL_HOST_TEST so the
tests exercise the production parser without requiring ESP-IDF headers.
"""

from __future__ import annotations

import json
import os
import pathlib
import shlex
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass


ROOT = pathlib.Path(__file__).resolve().parents[1]
MAIN = ROOT / "main"
MEASUREMENT_RESULT = 1
TIME_ACK_RESULT = 2
PARSE_ERROR_RESULT = 3


@dataclass(frozen=True)
class Compiler:
    command: list[str]
    flavor: str


def split_command(value: str) -> list[str]:
    """Split a compiler command from CC while preserving Windows paths."""
    parts = shlex.split(value, posix=(os.name != "nt"))
    return [strip_wrapping_quotes(part) for part in parts]


def strip_wrapping_quotes(value: str) -> str:
    if len(value) >= 2 and value[0] == value[-1] and value[0] in {"'", '"'}:
        return value[1:-1]
    return value


def command_exists(command: list[str]) -> bool:
    if not command:
        return False
    executable = command[0]
    if pathlib.Path(executable).exists():
        return True
    return shutil.which(executable) is not None


def compiler_flavor(command: list[str]) -> str:
    name = pathlib.Path(command[0]).name.lower()
    return "msvc" if name in {"cl", "cl.exe"} else "gcc"


def find_c_compiler() -> Compiler | None:
    candidates: list[list[str]] = []
    if os.environ.get("CC"):
        candidates.append(split_command(os.environ["CC"]))
    candidates.extend([
        ["gcc"],
        ["clang"],
        ["cc"],
        ["zig", "cc"],
        ["cl"],
    ])

    for candidate in candidates:
        if command_exists(candidate):
            return Compiler(candidate, compiler_flavor(candidate))
    return None


def build_probe(tmp: pathlib.Path, compiler: Compiler) -> pathlib.Path:
    source = tmp / "stm32_protocol_host_probe.c"
    exe = tmp / ("stm32_protocol_host_probe.exe" if os.name == "nt" else "stm32_protocol_host_probe")

    source.write_text(
        r'''
#include <stdio.h>
#include <stdlib.h>

#include "stm32_protocol.h"

int main(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; ++i) {
        stm32_protocol_result_t result = STM32_PROTOCOL_RESULT_NONE;
        char *json = stm32_protocol_build_publish_json(argv[i], 0, &result);

        if (json == NULL) {
            printf("%d\t<null>\n", (int)result);
            return 2;
        }

        printf("%d\t%s\n", (int)result, json);
        free(json);
    }

    return 0;
}
''',
        encoding="utf-8",
    )

    if compiler.flavor == "msvc":
        command = [
            *compiler.command,
            "/nologo",
            "/std:c11",
            "/DSTM32_PROTOCOL_HOST_TEST",
            f"/I{MAIN}",
            str(source),
            str(MAIN / "stm32_protocol.c"),
            f"/Fe:{exe}",
        ]
    else:
        command = [
            *compiler.command,
            "-std=c99",
            "-DSTM32_PROTOCOL_HOST_TEST",
            "-I",
            str(MAIN),
            str(source),
            str(MAIN / "stm32_protocol.c"),
            "-o",
            str(exe),
        ]
    subprocess.run(command, check=True, cwd=ROOT)
    return exe


def m(indexed_fields: dict[int, object]) -> str:
    highest = max(indexed_fields.keys(), default=0)
    fields = [""] * (highest + 1)
    fields[0] = "M"
    for index, value in indexed_fields.items():
        fields[index] = str(value)
    return ",".join(fields)


def complete_fields() -> list[str]:
    """Generate all 110 fields of a full M packet per current STM32 v3 schema."""
    return [
        # 0: msg_type
        "M",
        # 1-3: timestamp
        "1", "20260515", "123456",
        # 4-7: raw PPG
        "1000", "2000", "1800", "1",
        # 8-9: BPM
        "1", "72",
        # 10-11: SpO2
        "1", "98",
        # 12-13: RR
        "1", "18",
        # 14-15: IBI
        "1", "833",
        # 16-19: HRV time
        "1", "830", "42", "35",
        # 20-21: motion
        "0", "5",
        # 22-25: HRV poincare
        "30", "40", "75", "0",
        # 26-29: HRV freq
        "1", "1234", "567", "218",
        # 30-35: signal quality
        "85", "1", "123", "45", "5000", "3000",
        # 36-38: SpO2 ratio
        "1", "920", "1",
        # 39-46: finger detect
        "50000", "1500", "800", "200", "3000", "2500", "3", "2",
        # 47-60: sensor diag (14 fields)
        "0", "0", "0", "0", "0", "0",
        "100", "5", "0", "2", "12345", "50", "10", "0",
        # 61-71: system diag (11 fields)
        "1", "1", "1", "1", "3", "0", "1024000",
        "500", "12345", "0", "1",
        # 72-77: ECG core (6 fields)
        "1", "71", "845", "0", "123456", "-12",
        # 78-79: PTT
        "1", "245",
        # 80-84: ECG diagnostics (5 fields)
        "1000", "0", "0", "0", "0",
        # 85-91: crash (7 fields)
        "0", "0", "0", "0", "0", "5", "1",
        # 92-95: task phase (4 fields)
        "3", "2", "1", "0",
        # 96-99: task stack HWM (4 fields)
        "512", "256", "128", "64",
        # 100-101: task heartbeat (2 fields)
        "999", "500",
        # 102-109: ECG quality (8 fields)
        "80", "0", "1024", "512", "120", "900", "450", "768",
    ]


def short_old_m_fields() -> list[str]:
    """Generate a short 12-field frame like old firmware."""
    return [
        "M",
        "1", "20260515", "143025",
        "1000", "2000", "1800", "1",
        "1", "72",
        "1", "98",
    ]


def t_ack_fields() -> list[str]:
    return ["T", "1", "1", "20260515", "143025", "ntp_synced"]


class Probe:
    def __init__(self, exe: pathlib.Path):
        self.exe = exe

    def run(self, line: str) -> tuple[int, dict]:
        completed = subprocess.run(
            [str(self.exe), line],
            check=True,
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        result_text, payload_text = completed.stdout.rstrip("\n").split("\t", 1)
        return int(result_text), json.loads(payload_text)


def assert_measurement(result: int, payload: dict) -> None:
    assert result == MEASUREMENT_RESULT, payload
    assert payload["message"] == "measurement", payload
    assert payload["frame"] == "M", payload
    assert "rx_ms" in payload, "rx_ms missing in payload"
    assert "raw_line" in payload, "raw_line missing in payload"
    assert isinstance(payload.get("field_count"), int), payload
    assert isinstance(payload.get("parse_warnings"), list), payload
    assert isinstance(payload.get("extra_fields"), list), payload
    assert isinstance(payload.get("extra_field_count"), int), payload


def assert_parse_ok(payload: dict, expected: bool) -> None:
    assert payload["parse_ok"] is expected, f"expected parse_ok={expected}, got {payload.get('parse_ok')}"


def assert_parse_error(result: int, payload: dict) -> None:
    assert result == PARSE_ERROR_RESULT, payload
    assert payload["message"] == "parse_error", payload
    assert payload["parse_ok"] is False, payload
    assert "raw_line" in payload, payload
    assert isinstance(payload.get("field_count"), int), payload
    assert isinstance(payload.get("parse_warnings"), list), payload
    assert isinstance(payload.get("extra_fields"), list), payload


# ---------------------------------------------------------------------------
# Core parsing tests
# ---------------------------------------------------------------------------

def test_sparse_red_ir(probe: Probe) -> None:
    line = "M,,,,12345,67890"
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert payload["raw_line"] == line
    assert payload["field_count"] == 6
    assert payload["red"] == 12345
    assert payload["ir"] == 67890
    assert payload["modules"]["raw_ppg"]["available"] is True
    assert payload["modules"]["raw_ppg"]["red_available"] is True
    assert payload["modules"]["raw_ppg"]["ir_available"] is True


def test_bpm_without_later_modules(probe: Probe) -> None:
    line = m({4: 1000, 5: 2000, 7: 1, 8: 1, 9: 72})
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert payload["modules"]["bpm"]["available"] is True
    assert payload["modules"]["bpm"]["valid"] is True
    assert payload["modules"]["finger_status"]["available"] is True
    assert payload["modules"]["spo2"]["available"] is False
    assert payload["modules"]["rr"]["available"] is False
    assert payload["modules"]["ecg"]["available"] is False


def test_spo2_without_rr(probe: Probe) -> None:
    line = m({4: 1000, 5: 2000, 10: 1, 11: 98})
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert payload["modules"]["spo2"]["available"] is True
    assert payload["modules"]["spo2"]["valid"] is True
    assert payload["modules"]["rr"]["available"] is False


def test_rr_without_ecg(probe: Probe) -> None:
    line = m({12: 1, 13: 18})
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert payload["modules"]["rr"]["available"] is True
    assert payload["modules"]["rr"]["valid"] is True
    assert payload["modules"]["ecg"]["available"] is False


def test_complete_110_column_frame(probe: Probe) -> None:
    line = ",".join(complete_fields())
    assert len(complete_fields()) == 110, f"expected 110 fields, got {len(complete_fields())}"
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert_parse_ok(payload, True)
    assert payload["field_count"] == 110
    assert payload["schema_version"] == 3
    assert payload["modules"]["timestamp"]["available"] is True
    assert payload["modules"]["bpm"]["valid"] is True
    assert payload["modules"]["spo2"]["valid"] is True
    assert payload["modules"]["rr"]["valid"] is True
    assert payload["modules"]["hrv_time"]["valid"] is True
    assert payload["modules"]["hrv_freq"]["valid"] is True
    # ECG: filtered at column 77, should be i32 (-12)
    assert payload["modules"]["ecg"]["filtered"] == -12
    assert payload["modules"]["ecg"]["valid"] is True
    assert payload["ecg_filtered"] == -12
    assert payload["modules"]["ptt"]["valid"] is True
    # Signal quality at column 30
    assert payload["signal_quality"] == 85
    assert payload["raw_signal_present"] is True
    assert payload["signal_ir_pi_x1000"] == 123
    assert payload["signal_red_pi_x1000"] == 45
    # SpO2 ratio at 36-38
    assert payload["spo2_ratio_valid"] is True
    assert payload["spo2_ratio_x1000"] == 920
    assert payload["spo2_balance_status"] == 1
    # Sensor diag
    assert payload["sensor_read_error_count"] == 0
    assert payload["sensor_recover_count"] == 2
    assert payload["sensor_last_i2c_error"] == 0
    # System diag
    assert payload["modules"]["system_diag"]["available"] is True
    assert payload["modules"]["system_diag"]["sd_log_active"] is True
    assert payload["modules"]["system_diag"]["sd_state"] == 3
    assert payload["modules"]["system_diag"]["sd_error"] == 0
    assert payload["modules"]["system_diag"]["debug_mode"] == 0
    assert payload["modules"]["system_diag"]["current_page"] == 1
    # New modules
    assert payload["modules"]["crash"]["available"] is True
    assert payload["modules"]["task_phase"]["available"] is True
    assert payload["modules"]["task_stack"]["available"] is True
    assert payload["modules"]["task_heartbeat"]["available"] is True
    # ECG quality (cols 102-109)
    assert payload["modules"]["ecg_quality"]["available"] is True
    assert payload["ecg_signal_quality"] == 80
    assert payload["ecg_invalid_reason"] == 0
    assert payload["ecg_raw_span"] == 1024
    assert payload["ecg_filtered_span"] == 512
    assert payload["ecg_noise_level"] == 120
    assert payload["ecg_qrs_threshold"] == 900
    assert payload["ecg_peak_snr_x100"] == 450
    assert payload["ecg_dma_available_high_watermark"] == 768


# ---------------------------------------------------------------------------
# Field position verification
# ---------------------------------------------------------------------------

def test_signal_quality_at_column_30(probe: Probe) -> None:
    """signal_quality must be parsed from column 30 (field index 30)."""
    line = m({30: 95, 31: 1})  # signal_quality=95, raw_signal_present=1
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert payload["signal_quality"] == 95
    assert payload["raw_signal_present"] is True


def test_ecg_valid_at_column_72(probe: Probe) -> None:
    """ecg_valid must be parsed from column 72, not the old column 33."""
    line = m({72: 1, 73: 75, 74: 800})  # ecg_valid=1, ecg_hr=75, ecg_rr_ms=800
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert payload["ecg_valid"] is True
    assert payload["ecg_hr"] == 75
    assert payload["ecg_rr_ms"] == 800


def test_ptt_ms_at_column_79(probe: Probe) -> None:
    """ptt_ms must be parsed from column 79, not the old column 38."""
    line = m({78: 1, 79: 260})  # ptt_valid=1, ptt_ms=260
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert payload["ptt_valid"] is True
    assert payload["ptt_ms"] == 260


def test_named_critical_columns_in_110_frame(probe: Probe) -> None:
    """Verify the current STM32 110-column critical positions."""
    fields = complete_fields()
    fields[30] = "91"
    fields[64] = "0"
    fields[65] = "7"
    fields[66] = "3"
    fields[72] = "1"
    fields[73] = "88"
    fields[78] = "1"
    fields[79] = "301"
    fields[96] = "4096"
    fields[97] = "2048"
    fields[98] = "1024"
    fields[99] = "512"
    fields[100] = "12345"
    fields[101] = "67890"
    fields[102] = "95"
    fields[103] = "1"
    fields[104] = "2048"
    fields[105] = "1024"
    fields[108] = "600"
    fields[109] = "512"
    line = ",".join(fields)
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert_parse_ok(payload, True)
    assert payload["signal_quality"] == 91
    assert payload["modules"]["system_diag"]["sd_log_active"] is False
    assert payload["modules"]["system_diag"]["sd_state"] == 7
    assert payload["modules"]["system_diag"]["sd_error"] == 3
    assert payload["ecg_valid"] is True
    assert payload["ecg_hr"] == 88
    assert payload["ptt_valid"] is True
    assert payload["ptt_ms"] == 301
    assert payload["modules"]["task_stack"]["max_task_stack_hwm"] == 4096
    assert payload["modules"]["task_stack"]["ui_task_stack_hwm"] == 2048
    assert payload["modules"]["task_stack"]["sd_task_stack_hwm"] == 1024
    assert payload["modules"]["task_stack"]["wdt_task_stack_hwm"] == 512
    assert payload["modules"]["task_heartbeat"]["max_task_heartbeat"] == 12345
    assert payload["modules"]["task_heartbeat"]["ui_task_heartbeat"] == 67890
    # ECG quality new fields
    assert payload["ecg_signal_quality"] == 95
    assert payload["ecg_invalid_reason"] == 1
    assert payload["ecg_raw_span"] == 2048
    assert payload["ecg_filtered_span"] == 1024
    assert payload["ecg_peak_snr_x100"] == 600
    assert payload["ecg_dma_available_high_watermark"] == 512


def test_old_ecg_column_33_is_now_signal_red_pi(probe: Probe) -> None:
    """Old ecg_valid at column 33 is now signal_red_pi_x1000."""
    line = m({33: 99})  # signal_red_pi_x1000=99
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert payload["signal_red_pi_x1000"] == 99
    # ECG should NOT be valid from this field
    assert payload["modules"]["ecg"]["available"] is False


# ---------------------------------------------------------------------------
# ECG quality fields (cols 102-109)
# ---------------------------------------------------------------------------

def test_ecg_quality_at_top_level(probe: Probe) -> None:
    """New ECG quality fields at cols 102-109 must appear at JSON top level."""
    line = m({102: 75, 103: 2, 104: 2048, 105: 1800,
              106: 95, 107: 850, 108: 400, 109: 256})
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert payload["ecg_signal_quality"] == 75
    assert payload["ecg_invalid_reason"] == 2
    assert payload["ecg_raw_span"] == 2048
    assert payload["ecg_filtered_span"] == 1800
    assert payload["ecg_noise_level"] == 95
    assert payload["ecg_qrs_threshold"] == 850
    assert payload["ecg_peak_snr_x100"] == 400
    assert payload["ecg_dma_available_high_watermark"] == 256


def test_ecg_quality_module(probe: Probe) -> None:
    """New ECG quality fields must appear in modules.ecg_quality."""
    line = m({102: 88, 104: 4096, 107: 1024})
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert payload["modules"]["ecg_quality"]["available"] is True
    assert payload["modules"]["ecg_quality"]["signal_quality"] == 88
    assert payload["modules"]["ecg_quality"]["raw_span"] == 4096
    assert payload["modules"]["ecg_quality"]["qrs_threshold"] == 1024
    # Fields not provided should be null
    assert payload["modules"]["ecg_quality"]["noise_level"] is None


def test_ecg_quality_available_false_when_none_parsed(probe: Probe) -> None:
    """modules.ecg_quality.available must be false when no ECG quality fields present."""
    line = m({4: 1000, 5: 2000})
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert payload["modules"]["ecg_quality"]["available"] is False


# ---------------------------------------------------------------------------
# Short old firmware tests
# ---------------------------------------------------------------------------

def test_short_old_12_field_m_frame(probe: Probe) -> None:
    """Very old short firmware frames are forwarded but not schema-clean."""
    line = ",".join(short_old_m_fields())
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert_parse_ok(payload, False)
    assert payload["field_count"] == 12
    assert payload["raw_line"] == line
    assert "field_count_12_expected_110" in payload["parse_warnings"]
    assert payload["finger"] is True
    assert payload["bpm"] == 72
    assert payload["bpm_valid"] is True
    assert payload["spo2"] == 98
    assert payload["spo2_valid"] is True
    # Later modules should be unavailable
    assert payload["modules"]["rr"]["available"] is False
    assert payload["modules"]["ecg"]["available"] is False
    assert payload["modules"]["ptt"]["available"] is False
    assert payload["modules"]["signal_quality"]["available"] is False
    assert payload["modules"]["ecg_quality"]["available"] is False


def test_even_shorter_m_frame(probe: Probe) -> None:
    """Only a few fields — just M and raw PPG values with sparse commas."""
    line = "M,,,20260515,12345,67890"
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert_parse_ok(payload, False)
    assert payload["field_count"] == 6
    assert payload["red"] == 12345
    assert payload["ir"] == 67890
    assert "field_count_6_expected_110" in payload["parse_warnings"]


# ---------------------------------------------------------------------------
# Legacy 102-column compatibility
# ---------------------------------------------------------------------------

def test_legacy_102_field_frame_is_schema_mismatch(probe: Probe) -> None:
    """Old 102-column v2 frames must parse_ok=false with schema warning."""
    fields = [
        "M",
        "1", "20260515", "123456",
        "1000", "2000", "1800", "1",
        "1", "72", "1", "98", "1", "18", "1", "833",
        "1", "830", "42", "35",
        "0", "5",
        "30", "40", "75", "0",
        "1", "1234", "567", "218",
        "85", "1", "123", "45", "5000", "3000",
        "1", "920", "1",
        "50000", "1500", "800", "200", "3000", "2500", "3", "2",
        "0", "0", "0", "0", "0", "0", "100", "5", "0", "2", "12345", "50", "10", "0",
        "1", "1", "1", "1", "3", "0", "1024000", "500", "12345", "0", "1",
        "1", "71", "845", "0", "123456", "-12",
        "1", "245",
        "1000", "0", "0", "0", "0",
        "0", "0", "0", "0", "0", "5", "1",
        "3", "2", "1", "0",
        "512", "256", "128", "64",
        "999", "500",
    ]
    assert len(fields) == 102, f"expected 102 fields, got {len(fields)}"
    line = ",".join(fields)
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert_parse_ok(payload, False)
    assert payload["field_count"] == 102
    assert payload["schema_version"] == 3
    assert "field_count_102_expected_110" in payload["parse_warnings"]
    # First 102 fields must still parse correctly
    assert payload["bpm"] == 72
    assert payload["spo2"] == 98
    assert payload["modules"]["ecg"]["filtered"] == -12
    assert payload["modules"]["task_heartbeat"]["ui_task_heartbeat"] == 500
    # ECG quality fields should NOT be available (they don't exist in 102-col)
    assert payload["modules"]["ecg_quality"]["available"] is False
    # extra_fields must be empty since there are no fields beyond 109
    assert payload["extra_fields"] == []
    assert payload["extra_field_count"] == 0


# ---------------------------------------------------------------------------
# T ack frame tests
# ---------------------------------------------------------------------------

def test_t_ack_frame(probe: Probe) -> None:
    line = ",".join(t_ack_fields())
    result, payload = probe.run(line)
    assert result == TIME_ACK_RESULT, payload
    assert payload["message"] == "rtc_set_ack"
    assert payload["frame"] == "T"
    assert payload["parse_ok"] is True
    assert "rx_ms" in payload
    assert payload["raw_line"] == line
    assert payload["field_count"] == 6
    assert payload["parse_warnings"] == []
    assert payload["extra_fields"] == []
    assert payload["extra_field_count"] == 0
    assert payload["set_ok"] is True
    assert payload["rtc_valid"] is True
    assert payload["date"] == "20260515"
    assert payload["time"] == "143025"
    assert payload["reason"] == "ntp_synced"


def test_t_ack_bad_field_count(probe: Probe) -> None:
    line = "T,1,1,20260515"
    result, payload = probe.run(line)
    assert_parse_error(result, payload)
    assert payload["error"] == "bad_time_ack_frame"
    assert payload["field_count"] == 4
    assert "bad_time_ack_frame" in payload["parse_warnings"]


# ---------------------------------------------------------------------------
# Invalid / bad format tests
# ---------------------------------------------------------------------------

def test_invalid_field_warns_without_dropping_frame(probe: Probe) -> None:
    line = m({4: 1000, 5: 2000, 8: 1, 9: "not-a-number"})
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert_parse_ok(payload, False)
    assert payload["modules"]["raw_ppg"]["available"] is True
    assert payload["modules"]["bpm"]["available"] is False
    assert payload["modules"]["bpm"]["valid"] is False
    assert "field_9_bpm_invalid" in payload["parse_warnings"]


def test_empty_value_warns_without_dropping_frame(probe: Probe) -> None:
    fields = complete_fields()
    fields[9] = ""
    line = ",".join(fields)
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert_parse_ok(payload, False)
    assert payload["field_count"] == 110
    assert payload["bpm"] is None
    assert "field_9_bpm_empty" in payload["parse_warnings"]


def test_dash_dash_warns_without_dropping_frame(probe: Probe) -> None:
    fields = complete_fields()
    fields[9] = "--"
    line = ",".join(fields)
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert_parse_ok(payload, False)
    assert payload["field_count"] == 110
    assert payload["bpm"] is None
    assert "field_9_bpm_invalid" in payload["parse_warnings"]


def test_extra_fields_do_not_fail(probe: Probe) -> None:
    line = ",".join(complete_fields() + ["future_a", "future_b"])
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert_parse_ok(payload, False)
    assert payload["raw_line"] == line
    assert payload["field_count"] == 112
    assert payload["extra_field_count"] == 2
    assert payload["extra_fields"] == ["future_a", "future_b"]
    assert "field_count_112_expected_110" in payload["parse_warnings"]


def test_fields_beyond_internal_warning_limit_are_preserved(probe: Probe) -> None:
    extras = [f"extra_{i}" for i in range(30)]
    line = ",".join(complete_fields() + extras)
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert_parse_ok(payload, False)
    assert payload["field_count"] == 140
    assert payload["extra_field_count"] == 30
    assert payload["extra_fields"] == extras
    assert "field_count_140_expected_110" in payload["parse_warnings"]


def test_invalid_flags_are_not_transport_errors(probe: Probe) -> None:
    fields = complete_fields()
    # Set all _valid flags to 0
    fields[1] = "0"   # rtc_valid
    fields[8] = "0"   # bpm_valid
    fields[10] = "0"  # spo2_valid
    fields[12] = "0"  # rr_valid
    fields[14] = "0"  # ibi_valid
    fields[16] = "0"  # hrv_valid
    fields[26] = "0"  # hrv_freq_valid
    fields[36] = "0"  # spo2_ratio_valid
    fields[72] = "0"  # ecg_valid
    fields[78] = "0"  # ptt_valid
    line = ",".join(fields)
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert payload["modules"]["bpm"]["valid"] is False
    assert payload["modules"]["spo2"]["valid"] is False
    assert payload["modules"]["rr"]["valid"] is False
    assert payload["modules"]["hrv_time"]["valid"] is False
    assert payload["modules"]["hrv_freq"]["valid"] is False
    assert payload["modules"]["ecg"]["valid"] is False
    assert payload["modules"]["ptt"]["valid"] is False
    assert payload["modules"]["spo2_ratio"]["valid"] is False
    # Values still available even when invalid
    assert payload["modules"]["spo2"]["available"] is True
    assert payload["modules"]["rr"]["available"] is True
    assert payload["modules"]["ecg"]["available"] is True


def test_empty_fields_are_null(probe: Probe) -> None:
    line = m({4: 1000, 5: 2000, 7: "", 8: "", 9: ""})
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert payload["finger"] is None
    assert payload["bpm"] is None
    assert payload["bpm_valid"] is False


def test_unknown_frame_type(probe: Probe) -> None:
    line = "X,1,2,3"
    result, payload = probe.run(line)
    assert_parse_error(result, payload)
    assert payload["error"] == "unknown_frame_type"
    assert payload["raw_line"] == line
    assert payload["field_count"] == 4
    assert "unknown_frame_type" in payload["parse_warnings"]


def test_empty_frame(probe: Probe) -> None:
    line = ""
    result, payload = probe.run(line)
    assert_parse_error(result, payload)
    assert payload["error"] == "empty_frame"
    assert payload["field_count"] == 0
    assert "empty_frame" in payload["parse_warnings"]


def test_line_too_long(probe: Probe) -> None:
    line = "M," + "x" * 4200
    result, payload = probe.run(line)
    assert_parse_error(result, payload)
    assert payload["error"] == "line_too_long"
    assert payload["field_count"] == 2
    assert payload["raw_line"] == line
    assert "line_too_long" in payload["parse_warnings"]


# ---------------------------------------------------------------------------
# Semantic tests
# ---------------------------------------------------------------------------

def test_motion_artifact_freeze_semantics(probe: Probe) -> None:
    line = m({4: 1000, 5: 2000, 7: 1, 8: 1, 9: 72, 10: 1, 11: 98, 20: 1, 21: 90})
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert payload["motion_artifact"] is True
    assert payload["motion_score"] == 90
    assert payload["bpm"] == 72
    assert payload["spo2"] == 98


def test_signal_quality_sq_parsing(probe: Probe) -> None:
    """Signal quality at column 30 should parse as SQ 0-100."""
    line = m({30: 95, 31: 1, 32: 50, 33: 20})
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert payload["signal_quality"] == 95
    assert payload["raw_signal_present"] is True
    assert payload["signal_ir_pi_x1000"] == 50
    assert payload["signal_red_pi_x1000"] == 20


def test_spo2_ratio_and_balance(probe: Probe) -> None:
    """spo2_ratio/balance fields at columns 36-38 should parse."""
    line = m({36: 1, 37: 920, 38: 2})
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert payload["spo2_ratio_valid"] is True
    assert payload["spo2_ratio_x1000"] == 920
    assert payload["spo2_balance_status"] == 2


def test_finger_detect_fields(probe: Probe) -> None:
    """Finger detection diagnostic fields at columns 39-46 should parse."""
    line = m({39: 50000, 40: 1500, 41: 800, 42: 200, 43: 3000, 44: 2500, 45: 3, 46: 2})
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert payload["modules"]["finger_detect"]["available"] is True
    assert payload["modules"]["finger_detect"]["baseline_range_ir"] == 50000
    assert payload["modules"]["finger_detect"]["finger_on_confirm"] == 3
    assert payload["modules"]["finger_detect"]["finger_off_confirm"] == 2


def test_sensor_diag_fields(probe: Probe) -> None:
    """Sensor diagnostic fields at columns 47-60 should parse."""
    line = m({53: 100, 55: 3, 56: 1, 60: 42})  # read_ok=100, read_error=3, recover=1, i2c_error=42
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert payload["sensor_read_error_count"] == 3
    assert payload["sensor_recover_count"] == 1
    assert payload["sensor_last_i2c_error"] == 42
    assert payload["modules"]["sensor_diag"]["read_ok"] == 100


def test_system_diag_fields(probe: Probe) -> None:
    """System diagnostic fields at columns 61-71 with updated names."""
    line = m({61: 1, 62: 1, 63: 1, 64: 1, 65: 3, 66: 0, 67: 1024000,
              68: 500, 70: 0, 71: 1})
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert payload["modules"]["system_diag"]["available"] is True
    assert payload["modules"]["system_diag"]["sd_log_active"] is True
    assert payload["modules"]["system_diag"]["sd_state"] == 3
    assert payload["modules"]["system_diag"]["sd_error"] == 0
    assert payload["modules"]["system_diag"]["sd_total_written"] == 1024000
    assert payload["modules"]["system_diag"]["display_refresh"] == 500
    assert payload["modules"]["system_diag"]["debug_mode"] == 0
    assert payload["modules"]["system_diag"]["current_page"] == 1


def test_ecg_lead_off_bits(probe: Probe) -> None:
    """ecg_lead_off uses bit0=LD-, bit1=LD+. Now at column 75."""
    line = m({75: 3})
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert payload["ecg_lead_off"] == 3
    assert payload["modules"]["ecg"]["lead_off"] == 3


def test_ecg_diagnostic_fields(probe: Probe) -> None:
    """ECG diagnostic fields at columns 80-84."""
    line = m({80: 5000, 81: 2, 82: 0, 83: 1, 84: 3})
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert payload["modules"]["ecg"]["sample_count"] == 5000
    assert payload["modules"]["ecg"]["adc_sat_count"] == 2
    assert payload["modules"]["ecg"]["dma_overflow_count"] == 0
    assert payload["modules"]["ecg"]["lead_off_count"] == 1
    assert payload["modules"]["ecg"]["no_r_peak_timeout_count"] == 3


def test_field_order_is_preserved(probe: Probe) -> None:
    """Fields must remain in original order for GUI compatibility."""
    line = ",".join(complete_fields())
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    # First few fields
    assert payload["rtc_valid"] is True
    assert payload["date"] == "20260515"
    assert payload["time"] == "123456"
    # BPM before SpO2 before RR before ECG
    bpm_idx = list(payload.keys()).index("bpm")
    spo2_idx = list(payload.keys()).index("spo2")
    rr_idx = list(payload.keys()).index("rr")
    ecg_hr_idx = list(payload.keys()).index("ecg_hr")
    assert bpm_idx < spo2_idx < rr_idx < ecg_hr_idx


# ---------------------------------------------------------------------------
# New module tests: crash, task_phase, task_stack, task_heartbeat
# ---------------------------------------------------------------------------

def test_crash_module(probe: Probe) -> None:
    """Crash diagnostic fields at columns 85-91."""
    line = m({85: 1, 86: 3, 87: 1, 88: 5, 89: 123456, 90: 10, 91: 0x07})
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert payload["modules"]["crash"]["available"] is True
    assert payload["modules"]["crash"]["crash_flag"] is True
    assert payload["modules"]["crash"]["crash_source"] == 3
    assert payload["modules"]["crash"]["crash_task"] == 1
    assert payload["modules"]["crash"]["crash_phase"] == 5
    assert payload["modules"]["crash"]["crash_tick"] == 123456
    assert payload["modules"]["crash"]["reboot_count"] == 10
    assert payload["modules"]["crash"]["reset_flags"] == 7


def test_task_phase_module(probe: Probe) -> None:
    """Task phase fields at columns 92-95."""
    line = m({92: 4, 93: 3, 94: 2, 95: 1})
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert payload["modules"]["task_phase"]["available"] is True
    assert payload["modules"]["task_phase"]["max_task_phase"] == 4
    assert payload["modules"]["task_phase"]["ui_task_phase"] == 3
    assert payload["modules"]["task_phase"]["sd_task_phase"] == 2
    assert payload["modules"]["task_phase"]["wdt_task_phase"] == 1


def test_task_stack_module(probe: Probe) -> None:
    """Task stack HWM fields at columns 96-99."""
    line = m({96: 2048, 97: 1024, 98: 512, 99: 256})
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert payload["modules"]["task_stack"]["available"] is True
    assert payload["modules"]["task_stack"]["max_task_stack_hwm"] == 2048
    assert payload["modules"]["task_stack"]["ui_task_stack_hwm"] == 1024
    assert payload["modules"]["task_stack"]["sd_task_stack_hwm"] == 512
    assert payload["modules"]["task_stack"]["wdt_task_stack_hwm"] == 256


def test_task_heartbeat_module(probe: Probe) -> None:
    """Task heartbeat fields at columns 100-101."""
    line = m({100: 10000, 101: 5000})
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert payload["modules"]["task_heartbeat"]["available"] is True
    assert payload["modules"]["task_heartbeat"]["max_task_heartbeat"] == 10000
    assert payload["modules"]["task_heartbeat"]["ui_task_heartbeat"] == 5000


def test_schema_version_present(probe: Probe) -> None:
    """All output types must include schema_version=3."""
    # Measurement
    line = m({4: 1000, 5: 2000})
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert payload["schema_version"] == 3

    # Time ack
    line = ",".join(t_ack_fields())
    result, payload = probe.run(line)
    assert payload["schema_version"] == 3

    # Parse error
    line = "X,1,2"
    result, payload = probe.run(line)
    assert payload["schema_version"] == 3


def test_110_field_tail_not_lost(probe: Probe) -> None:
    """Fields at columns 100-109 must not be truncated."""
    fields = complete_fields()
    assert len(fields) == 110
    line = ",".join(fields)
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert payload["field_count"] == 110
    assert payload["modules"]["task_heartbeat"]["available"] is True
    assert payload["modules"]["task_heartbeat"]["max_task_heartbeat"] == 999
    assert payload["modules"]["task_heartbeat"]["ui_task_heartbeat"] == 500
    assert payload["modules"]["ecg_quality"]["available"] is True
    assert payload["ecg_signal_quality"] == 80
    assert payload["ecg_dma_available_high_watermark"] == 768


def test_ecg_filtered_negative(probe: Probe) -> None:
    """ecg_filtered at column 77 can be negative (i32)."""
    line = m({77: -42})
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert payload["ecg_filtered"] == -42
    assert payload["modules"]["ecg"]["filtered"] == -42


# ---------------------------------------------------------------------------
# parse_ok boundary tests (parse_ok=false when field_count != 110 or warnings present)
# ---------------------------------------------------------------------------

def test_109_field_partial_frame_parse_ok_false(probe: Probe) -> None:
    """109-field frame (one column short) must report parse_ok=false."""
    fields = complete_fields()[:109]  # drop column 109
    line = ",".join(fields)
    assert len(fields) == 109
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert_parse_ok(payload, False)
    assert payload["field_count"] == 109
    assert "raw_line" in payload
    assert "field_count" in payload
    assert "field_count_109_expected_110" in payload["parse_warnings"]


def test_81_field_partial_frame_parse_ok_false(probe: Probe) -> None:
    """Old 81-field frame must report parse_ok=false with an explicit warning."""
    fields = complete_fields()[:81]
    line = ",".join(fields)
    assert len(fields) == 81
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert_parse_ok(payload, False)
    assert payload["field_count"] == 81
    assert "field_count_81_expected_110" in payload["parse_warnings"]


def test_parse_warnings_cause_parse_ok_false(probe: Probe) -> None:
    """A 110-field frame with a malformed field must report parse_ok=false."""
    fields = complete_fields()
    fields[9] = "not-a-number"  # corrupt bpm field
    line = ",".join(fields)
    assert len(fields) == 110
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert_parse_ok(payload, False)
    assert len(payload["parse_warnings"]) > 0
    assert "field_9_bpm_invalid" in payload["parse_warnings"]


def test_111_extra_fields_parse_ok_false(probe: Probe) -> None:
    """111-field frame (one extra column) must report parse_ok=false."""
    fields = complete_fields() + ["extra_110"]
    line = ",".join(fields)
    assert len(fields) == 111
    result, payload = probe.run(line)
    assert_measurement(result, payload)
    assert_parse_ok(payload, False)
    assert payload["field_count"] == 111
    assert payload["extra_field_count"] == 1
    assert payload["extra_fields"] == ["extra_110"]
    assert "field_count_111_expected_110" in payload["parse_warnings"]


def main() -> int:
    compiler = find_c_compiler()
    if compiler is None:
        print(
            "SKIP: no native C compiler found; install MinGW/LLVM/Zig, "
            "run from a VS Developer PowerShell, or set CC to run host parser tests"
        )
        return 77

    with tempfile.TemporaryDirectory() as tmp_name:
        tmp = pathlib.Path(tmp_name)
        probe = Probe(build_probe(tmp, compiler))
        for test in [
            # Core parsing
            test_sparse_red_ir,
            test_bpm_without_later_modules,
            test_spo2_without_rr,
            test_rr_without_ecg,
            test_complete_110_column_frame,
            # Field position verification
            test_signal_quality_at_column_30,
            test_ecg_valid_at_column_72,
            test_ptt_ms_at_column_79,
            test_named_critical_columns_in_110_frame,
            test_old_ecg_column_33_is_now_signal_red_pi,
            # ECG quality fields
            test_ecg_quality_at_top_level,
            test_ecg_quality_module,
            test_ecg_quality_available_false_when_none_parsed,
            # Old firmware compatibility
            test_short_old_12_field_m_frame,
            test_even_shorter_m_frame,
            # Legacy 102-column compatibility
            test_legacy_102_field_frame_is_schema_mismatch,
            # T ack
            test_t_ack_frame,
            test_t_ack_bad_field_count,
            # Invalid / edge cases
            test_invalid_field_warns_without_dropping_frame,
            test_empty_value_warns_without_dropping_frame,
            test_dash_dash_warns_without_dropping_frame,
            test_extra_fields_do_not_fail,
            test_fields_beyond_internal_warning_limit_are_preserved,
            test_invalid_flags_are_not_transport_errors,
            test_empty_fields_are_null,
            test_unknown_frame_type,
            test_empty_frame,
            test_line_too_long,
            # Semantic tests
            test_motion_artifact_freeze_semantics,
            test_signal_quality_sq_parsing,
            test_spo2_ratio_and_balance,
            test_finger_detect_fields,
            test_sensor_diag_fields,
            test_system_diag_fields,
            test_ecg_lead_off_bits,
            test_ecg_diagnostic_fields,
            test_field_order_is_preserved,
            # New modules
            test_crash_module,
            test_task_phase_module,
            test_task_stack_module,
            test_task_heartbeat_module,
            test_schema_version_present,
            test_110_field_tail_not_lost,
            test_ecg_filtered_negative,
            # parse_ok boundary tests
            test_109_field_partial_frame_parse_ok_false,
            test_81_field_partial_frame_parse_ok_false,
            test_parse_warnings_cause_parse_ok_false,
            test_111_extra_fields_parse_ok_false,
        ]:
            test(probe)

    print("stm32_protocol parser tests passed (110-field v3 schema)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
