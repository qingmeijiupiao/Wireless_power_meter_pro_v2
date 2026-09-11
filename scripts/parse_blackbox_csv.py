#!/usr/bin/env python3
"""Convert a Web-exported blackbox CSV file into an Excel-friendly HTML report."""

from __future__ import annotations

import argparse
import csv
import html
import json
import re
import sys
from collections import Counter
from datetime import datetime, timedelta, timezone
from pathlib import Path


UINT32_MOD = 1 << 32
ROLLOVER_EDGE_MS = 24 * 60 * 60 * 1000
SYNC_RAW_RE = re.compile(r"\[[EWIDV]\]\[TimeService\].*unix_s=(\d+)\s+unix_us=(\d+)")
BOOT_RE = re.compile(r"system: boot_start\b")
RESET_RE = re.compile(r"^\[Blackbox\]: reset$")
SEVERITY_RE = re.compile(r"^\[([EW])\]\[")

ORIGINAL_COLUMNS = [
    "index", "timestamp_ms", "type", "fragments", "text", "payload_hex",
    "snapshot_version", "flags", "protect", "voltage_mv", "current_ua",
    "meter_mwh", "board_temp_c", "chip_temp_c",
]
REPORT_COLUMNS = [
    "run_id", "wall_time", "uptime_ms", "severity", "type_display", "text",
    "flags_decoded", "protect_decoded", "voltage_mv", "current_ua",
    "meter_mwh", "board_temp_c", "chip_temp_c", "index", "fragments",
    "snapshot_version", "flags", "protect", "type", "payload_hex",
]
REPORT_COLUMN_LABELS = {
    "run_id": "运行编号",
    "wall_time": "网络时间",
    "uptime_ms": "启动后时间(ms)",
    "severity": "级别",
    "type_display": "类型",
    "text": "日志文本",
    "flags_decoded": "全局标志位解析",
    "protect_decoded": "保护状态解析",
    "voltage_mv": "电压(mV)",
    "current_ua": "电流(uA)",
    "meter_mwh": "累计电量(mWh)",
    "board_temp_c": "板载温度(C)",
    "chip_temp_c": "芯片温度(C)",
    "index": "原始索引",
    "fragments": "文本分片数",
    "snapshot_version": "快照版本",
    "flags": "全局标志位原始值",
    "protect": "保护状态原始值",
    "type": "原始类型",
    "payload_hex": "原始载荷(hex)",
}
REPORT_COLUMN_WIDTHS = {
    "run_id": 72,
    "wall_time": 210,
    "uptime_ms": 120,
    "severity": 64,
    "type_display": 100,
    "text": 360,
    "flags_decoded": 280,
    "protect_decoded": 180,
    "voltage_mv": 88,
    "current_ua": 88,
    "meter_mwh": 110,
    "board_temp_c": 100,
    "chip_temp_c": 100,
    "index": 80,
    "fragments": 88,
    "snapshot_version": 80,
    "flags": 120,
    "protect": 120,
    "type": 88,
    "payload_hex": 360,
}
GLOBAL_FLAG_LABELS = {
    "output_enabled": "输出已开启",
    "can_resistor_enabled": "CAN终端电阻已开启",
    "protect_bypassed": "保护已旁路",
    "protect_initialized": "保护已初始化",
    "lp_core_running": "LP Core运行中",
    "lp_ina228_initialized": "LP INA228已初始化",
    "lp_i2c_error": "LP I2C错误",
    "lp_ina228_read_timeout": "LP INA228读取超时",
    "wifi_service_initialized": "WiFi服务已初始化",
    "wifi_enabled": "WiFi已开启",
    "wifi_sta_connected": "WiFi STA已连接",
    "wifi_ap_mode": "WiFi AP模式",
    "wifi_has_saved_sta": "已保存WiFi STA配置",
    "wifi_web_enabled_on_boot": "开机启用WiFi/Web",
    "web_backend_running": "Web后端运行中",
    "screen_initialized": "屏幕已初始化",
    "blackbox_enabled": "黑匣子已开启",
    "reserved": "保留位",
}
PROTECT_CHANNEL_LABELS = {
    "temperature": "过温保护",
    "high_voltage": "过压保护",
    "low_voltage": "欠压保护",
    "current": "过流保护",
}
PROTECT_STATE_LABELS = {
    "normal": "正常",
    "warning": "警告",
    "protect": "保护",
}
TYPE_LABELS = {
    "structured": "结构化快照",
    "string": "文本日志",
    "unknown": "未知类型",
    "invalid": "无效记录",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="将网页导出的黑匣子 CSV 转换为 HTML 表格报告。"
    )
    parser.add_argument("input", type=Path, help="网页导出的 CSV 源文件")
    parser.add_argument("-o", "--output", type=Path, help="输出 HTML 路径")
    parser.add_argument(
        "--project-root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="用于查找标志位定义的工程根目录（默认：脚本所在工程）",
    )
    parser.add_argument(
        "--timezone-offset",
        type=int,
        default=8,
        help="网络时间的 UTC 时区偏移小时数（默认：8）",
    )
    parser.add_argument(
        "--no-source-flags",
        action="store_true",
        help="不从工程源码解析标志位定义",
    )
    return parser.parse_args()


def read_csv(path: Path) -> tuple[list[dict[str, str]], list[str]]:
    with path.open("r", encoding="utf-8-sig", newline="") as source:
        reader = csv.DictReader(source)
        if not reader.fieldnames:
            raise ValueError("CSV 缺少表头")
        missing = [column for column in ("index", "timestamp_ms", "type", "text") if column not in reader.fieldnames]
        if missing:
            raise ValueError(f"CSV 缺少必要列：{', '.join(missing)}")
        rows = []
        for source_order, row in enumerate(reader):
            normalized = {column: row.get(column, "") or "" for column in reader.fieldnames}
            normalized["_source_order"] = source_order
            normalized["_uptime_raw"] = parse_int(normalized.get("timestamp_ms"))
            rows.append(normalized)
        return rows, list(reader.fieldnames)


def parse_int(value: str | None) -> int | None:
    if value is None or not value.strip():
        return None
    try:
        return int(value, 0)
    except ValueError:
        return None


def parse_float(value: str | None) -> float | None:
    if value is None or not value.strip():
        return None
    try:
        return float(value)
    except ValueError:
        return None


def parse_bitfield_body(body: str, owner_name: str) -> list[tuple[str, int, int]]:
    fields = []
    offset = 0
    for name, width_text in re.findall(r"\b\w+\s+(\w+)\s*:\s*(\d+)\s*;", body):
        width = int(width_text)
        fields.append((name, offset, width))
        offset += width
    if not fields:
        raise ValueError(f"未找到 {owner_name} 中的位域")
    return fields


def parse_flag_fields(path: Path, type_name: str, member_name: str) -> list[tuple[str, int, int]]:
    source = path.read_text(encoding="utf-8")
    nested_struct = re.search(
        rf"union\s+{re.escape(type_name)}\s*\{{.*?struct\s*\{{(?P<body>.*?)\}}\s*{re.escape(member_name)}\s*;",
        source,
        re.DOTALL,
    )
    if nested_struct:
        return parse_bitfield_body(nested_struct.group("body"), f"{type_name}.{member_name}")

    direct_struct = re.search(
        rf"struct\s+{re.escape(type_name)}\s*\{{(?P<body>.*?)\}}\s*(?:__attribute__\s*\(\(.*?\)\)\s*)?;",
        source,
        re.DOTALL,
    )
    if direct_struct:
        return parse_bitfield_body(direct_struct.group("body"), type_name)

    raise ValueError(f"未找到 {type_name}.{member_name} 或 struct {type_name}")


def parse_protect_states(path: Path) -> dict[int, str]:
    source = path.read_text(encoding="utf-8")
    values = {}
    for name, value in re.findall(r"\bPROTECT_STATE_(\w+)\s*=\s*(\d+)", source):
        values[int(value)] = name.lower()
    if not values:
        raise ValueError("未找到 ProtectState_t 枚举值")
    return values


def load_source_definitions(project_root: Path, disabled: bool) -> tuple[list[tuple[str, int, int]], list[tuple[str, int, int]], dict[int, str], list[str]]:
    warnings = []
    if disabled:
        return [], [], {}, ["已通过 --no-source-flags 禁用源码标志位解析。"]
    global_path = project_root / "components/app/global_state/include/global_state.h"
    protect_path = project_root / "components/app/protect/include/protect.h"
    try:
        global_fields = parse_flag_fields(global_path, "GlobalStateFlags", "bits")
    except (OSError, ValueError) as error:
        global_fields = []
        warnings.append(f"GlobalStateFlags 源码解析失败：{error}")
    try:
        protect_fields = parse_flag_fields(protect_path, "protect_states_t", "states_bit")
        protect_states = parse_protect_states(protect_path)
    except (OSError, ValueError) as error:
        protect_fields = []
        protect_states = {}
        warnings.append(f"protect_states_t 源码解析失败：{error}")
    return global_fields, protect_fields, protect_states, warnings


def decode_flags(value_text: str, fields: list[tuple[str, int, int]]) -> str:
    value = parse_int(value_text)
    if value is None or not fields:
        return ""
    decoded = []
    for name, offset, width in fields:
        field_value = (value >> offset) & ((1 << width) - 1)
        label = GLOBAL_FLAG_LABELS.get(name, name)
        decoded.append(f"{label}={field_value}")
    return "\n".join(decoded)


def decode_protect(value_text: str, fields: list[tuple[str, int, int]], states: dict[int, str]) -> str:
    value = parse_int(value_text)
    if value is None or not fields or not states:
        return ""
    decoded = []
    for name, offset, width in fields:
        field_value = (value >> offset) & ((1 << width) - 1)
        channel = name.removesuffix("_protect_state")
        channel_label = PROTECT_CHANNEL_LABELS.get(channel, channel)
        state = states.get(field_value)
        state_label = PROTECT_STATE_LABELS.get(state, f"未知({field_value})")
        decoded.append(f"{channel_label}={state_label}")
    return "，".join(decoded)


def assign_runs(rows: list[dict[str, str]]) -> tuple[list[dict[str, str]], list[str]]:
    warnings = []
    chronological = list(reversed(rows))
    run_id = 0
    rollover_offset = 0
    previous_uptime = None
    for row in chronological:
        uptime = row["_uptime_raw"]
        if uptime is None:
            row["_run_id"] = run_id or 1
            row["_uptime_unwrapped"] = None
            continue
        if previous_uptime is None:
            run_id = 1
        elif uptime < previous_uptime:
            if previous_uptime >= UINT32_MOD - ROLLOVER_EDGE_MS and uptime <= ROLLOVER_EDGE_MS:
                rollover_offset += UINT32_MOD
                warnings.append(f"运行 {run_id}：检测到 uint32 启动后时间回绕。")
            else:
                run_id += 1
                rollover_offset = 0
        row["_run_id"] = run_id
        row["_uptime_unwrapped"] = uptime + rollover_offset
        previous_uptime = uptime
    return chronological, warnings


def enrich_rows(
    chronological: list[dict[str, str]],
    global_fields: list[tuple[str, int, int]],
    protect_fields: list[tuple[str, int, int]],
    protect_states: dict[int, str],
    tz: timezone,
) -> None:
    anchors: dict[int, list[tuple[int, float]]] = {}
    for row in chronological:
        uptime = row["_uptime_unwrapped"]
        match = SYNC_RAW_RE.search(row.get("text", ""))
        if uptime is not None and match:
            wall_epoch = int(match.group(1)) + int(match.group(2)) / 1_000_000
            anchors.setdefault(row["_run_id"], []).append((uptime, wall_epoch))

    for row in chronological:
        text = row.get("text", "")
        match = SEVERITY_RE.match(text)
        row["run_id"] = str(row["_run_id"])
        row["uptime_ms"] = str(row["_uptime_unwrapped"] if row["_uptime_unwrapped"] is not None else "")
        row["severity"] = {"E": "错误", "W": "警告"}.get(match.group(1), "") if match else ""
        row["type_display"] = TYPE_LABELS.get(row.get("type", ""), row.get("type", ""))
        row["flags_decoded"] = decode_flags(row.get("flags", ""), global_fields)
        row["protect_decoded"] = decode_protect(row.get("protect", ""), protect_fields, protect_states)
        row["wall_time"] = ""
        row["_wall_epoch_ms"] = None
        uptime = row["_uptime_unwrapped"]
        run_anchors = anchors.get(row["_run_id"], [])
        if uptime is not None and run_anchors:
            anchor_uptime, anchor_epoch = min(run_anchors, key=lambda item: abs(item[0] - uptime))
            estimated_epoch = anchor_epoch + (uptime - anchor_uptime) / 1000
            row["_wall_epoch_ms"] = int(estimated_epoch * 1000)
            wall = datetime.fromtimestamp(estimated_epoch, tz)
            row["wall_time"] = wall.strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]


def summarize_runs(chronological: list[dict[str, str]]) -> list[dict[str, object]]:
    grouped: dict[int, list[dict[str, str]]] = {}
    for row in chronological:
        grouped.setdefault(row["_run_id"], []).append(row)
    summaries = []
    for run_id in sorted(grouped, reverse=True):
        rows = grouped[run_id]
        uptimes = [row["_uptime_unwrapped"] for row in rows if row["_uptime_unwrapped"] is not None]
        wall_times = [row["wall_time"] for row in rows if row["wall_time"]]
        counts = Counter(row["severity"] for row in rows)
        boot_rows = [row for row in rows if BOOT_RE.search(row.get("text", ""))]
        summaries.append({
            "run_id": run_id,
            "records": len(rows),
            "duration_ms": max(uptimes) - min(uptimes) if uptimes else 0,
            "errors": counts["错误"],
            "warnings": counts["警告"],
            "anchored": bool(wall_times),
            "wall_range": f"{wall_times[0]} 至 {wall_times[-1]}" if wall_times else "无时间锚点",
            "boot": boot_rows[-1].get("text", "") if boot_rows else "",
        })
    return summaries


def summarize_overview(chronological: list[dict[str, str]], summaries: list[dict[str, object]]) -> dict[str, object]:
    first_text = chronological[0].get("text", "") if chronological else ""
    return {
        "total_duration_ms": sum(int(summary["duration_ms"]) for summary in summaries),
        "blackbox_reset": bool(RESET_RE.search(first_text)),
    }


def decode_bit_values(value_text: str, fields: list[tuple[str, int, int]]) -> dict[str, int]:
    value = parse_int(value_text)
    if value is None:
        return {}
    return {name: (value >> offset) & ((1 << width) - 1) for name, offset, width in fields}


def build_diagnostic_event(
    row: dict[str, str],
    category: str,
    level: str,
    title: str,
    detail: str,
) -> dict[str, object]:
    return {
        "run_id": row.get("run_id", ""),
        "wall_time": row.get("wall_time", ""),
        "uptime_ms": row.get("uptime_ms", ""),
        "index": row.get("index", ""),
        "category": category,
        "level": level,
        "title": title,
        "detail": detail,
    }


def build_diagnostic_events(
    chronological: list[dict[str, str]],
    global_fields: list[tuple[str, int, int]],
    protect_fields: list[tuple[str, int, int]],
    protect_states: dict[int, str],
) -> list[dict[str, object]]:
    events: list[dict[str, object]] = []
    previous_flags: dict[int, dict[str, int]] = {}
    previous_protect: dict[int, dict[str, int]] = {}
    watched_flags = {
        "output_enabled",
        "protect_bypassed",
        "lp_core_running",
        "lp_ina228_initialized",
        "lp_i2c_error",
        "lp_ina228_read_timeout",
        "wifi_enabled",
        "wifi_sta_connected",
        "wifi_ap_mode",
        "web_backend_running",
        "blackbox_enabled",
    }
    warning_flags = {"lp_i2c_error", "lp_ina228_read_timeout"}

    for row in chronological:
        text = row.get("text", "")
        if row.get("severity"):
            events.append(build_diagnostic_event(row, "日志", row["severity"], row["severity"], text))
        if BOOT_RE.search(text):
            events.append(build_diagnostic_event(row, "系统", "信息", "系统启动", text))
        if RESET_RE.search(text):
            events.append(build_diagnostic_event(row, "黑匣子", "信息", "黑匣子重置", text))

        run_id = row.get("_run_id", 0)
        flag_values = decode_bit_values(row.get("flags", ""), global_fields)
        if flag_values:
            before = previous_flags.get(run_id)
            if before is not None:
                for name, value in flag_values.items():
                    if name not in watched_flags or before.get(name) == value:
                        continue
                    label = GLOBAL_FLAG_LABELS.get(name, name)
                    level = "警告" if name in warning_flags and value else "状态"
                    events.append(build_diagnostic_event(row, "标志位", level, label, f"{label}: {before.get(name, 0)} -> {value}"))
            previous_flags[run_id] = flag_values

        protect_values = decode_bit_values(row.get("protect", ""), protect_fields)
        if protect_values:
            before = previous_protect.get(run_id)
            if before is not None:
                for name, value in protect_values.items():
                    if before.get(name) == value:
                        continue
                    channel = name.removesuffix("_protect_state")
                    channel_label = PROTECT_CHANNEL_LABELS.get(channel, channel)
                    state = protect_states.get(value, str(value))
                    state_label = PROTECT_STATE_LABELS.get(state, f"未知({value})")
                    level = "错误" if state == "protect" else ("警告" if state == "warning" else "状态")
                    events.append(
                        build_diagnostic_event(
                            row,
                            "保护",
                            level,
                            channel_label,
                            f"{channel_label}: {before.get(name, 0)} -> {state_label}",
                        )
                    )
            previous_protect[run_id] = protect_values
    return events


def build_interactive_model(
    chronological: list[dict[str, str]],
    summaries: list[dict[str, object]],
    global_fields: list[tuple[str, int, int]],
    protect_fields: list[tuple[str, int, int]],
    protect_states: dict[int, str],
) -> dict[str, object]:
    flag_meta = [
        {
            "name": name,
            "label": GLOBAL_FLAG_LABELS.get(name, name),
            "offset": offset,
            "width": width,
        }
        for name, offset, width in global_fields
        if name != "reserved"
    ]
    protect_meta = []
    for name, offset, width in protect_fields:
        channel = name.removesuffix("_protect_state")
        protect_meta.append({
            "name": name,
            "channel": channel,
            "label": PROTECT_CHANNEL_LABELS.get(channel, channel),
            "offset": offset,
            "width": width,
        })

    points = []
    for row in chronological:
        uptime = row.get("_uptime_unwrapped")
        if uptime is None:
            continue
        values = {
            "voltage_mv": parse_float(row.get("voltage_mv")),
            "current_ua": parse_float(row.get("current_ua")),
            "meter_mwh": parse_float(row.get("meter_mwh")),
            "board_temp_c": parse_float(row.get("board_temp_c")),
            "chip_temp_c": parse_float(row.get("chip_temp_c")),
        }
        if not any(value is not None for value in values.values()) and not row.get("flags") and not row.get("protect"):
            continue
        points.append({
            "run_id": row.get("run_id", ""),
            "uptime_ms": uptime,
            "wall_epoch_ms": row.get("_wall_epoch_ms"),
            "wall_time": row.get("wall_time", ""),
            "index": row.get("index", ""),
            "severity": row.get("severity", ""),
            "flags": parse_int(row.get("flags")),
            "protect": parse_int(row.get("protect")),
            **values,
        })

    state_labels = {
        str(value): PROTECT_STATE_LABELS.get(name, name)
        for value, name in protect_states.items()
    }
    return {
        "runs": summaries,
        "points": points,
        "flag_fields": flag_meta,
        "protect_fields": protect_meta,
        "protect_state_labels": state_labels,
        "diagnostic_events": build_diagnostic_events(chronological, global_fields, protect_fields, protect_states),
    }


def json_for_script(value: object) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).replace("<", "\\u003c")


def esc(value: object) -> str:
    return html.escape(str(value), quote=True)


def render_flags_cell(row: dict[str, str]) -> str:
    decoded = row.get("flags_decoded", "")
    if not decoded:
        return "<td></td>"
    lines = decoded.splitlines()
    active = []
    for line in lines:
        _, _, value = line.rpartition("=")
        if value != "0":
            active.append(line)
    raw = row.get("flags", "")
    summary = f"已置位 {len(active)} 项 · 原始值 {raw}"
    details = "".join(f"<span>{esc(line)}</span>" for line in lines)
    return (
        '<td class="flags-cell"><details class="flags-details">'
        f"<summary>{esc(summary)}</summary>"
        f'<div class="flags-grid">{details}</div>'
        "</details></td>"
    )


def render_report_cells(row: dict[str, str]) -> str:
    cells = []
    for column in REPORT_COLUMNS:
        if column == "flags_decoded":
            cells.append(render_flags_cell(row))
        else:
            cells.append(f"<td>{esc(row.get(column, ''))}</td>")
    return "".join(cells)


def render_html(
    rows: list[dict[str, str]],
    summaries: list[dict[str, object]],
    overview: dict[str, object],
    warnings: list[str],
    source_name: str,
    timezone_label: str,
    model: dict[str, object],
) -> str:
    run_options = "".join(f'<option value="{summary["run_id"]}">运行 {summary["run_id"]}</option>' for summary in summaries)
    warning_html = "".join(f"<li>{esc(warning)}</li>" for warning in warnings) or "<li>无</li>"
    summary_rows = []
    for summary in summaries:
        summary_rows.append(
            "<tr>"
            f'<td>运行 {summary["run_id"]}</td><td>{summary["records"]}</td>'
            f'<td>{summary["duration_ms"] / 1000:.3f} 秒</td>'
            f'<td>{summary["errors"]}</td><td>{summary["warnings"]}</td>'
            f'<td>{"是" if summary["anchored"] else "否"}</td>'
            f'<td>{esc(summary["wall_range"])}</td>'
            f'<td>{esc(summary["boot"])}</td></tr>'
        )
    diagnostic_rows = []
    for event in reversed(model["diagnostic_events"]):
        diagnostic_rows.append(
            "<tr>"
            f'<td>{esc(event["run_id"])}</td>'
            f'<td>{esc(event["wall_time"])}</td>'
            f'<td>{esc(event["uptime_ms"])}</td>'
            f'<td>{esc(event["level"])}</td>'
            f'<td>{esc(event["category"])}</td>'
            f'<td>{esc(event["title"])}</td>'
            f'<td>{esc(event["detail"])}</td>'
            f'<td>{esc(event["index"])}</td>'
            "</tr>"
        )
    table_rows = []
    for row in reversed(rows):
        severity = {"错误": "error", "警告": "warn"}.get(row["severity"], "")
        cells = render_report_cells(row)
        searchable = " ".join(str(row.get(column, "")) for column in REPORT_COLUMNS).lower()
        table_rows.append(
            f'<tr class="{severity}" data-run="{esc(row["run_id"])}" '
            f'data-severity="{esc(row["severity"])}" data-search="{esc(searchable)}">{cells}</tr>'
        )
    colgroup = "".join(f'<col style="width:{REPORT_COLUMN_WIDTHS.get(column, 120)}px">' for column in REPORT_COLUMNS)
    headers = "".join(
        f'<th>{esc(REPORT_COLUMN_LABELS.get(column, column))}<span class="resizer" title="拖拽调整列宽"></span></th>'
        for column in REPORT_COLUMNS
    )
    latest_run = summaries[0]["run_id"] if summaries else ""
    model_json = json_for_script(model)
    return f"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<title>黑匣子日志报告</title>
<style>
body{{font-family:Arial,"Microsoft YaHei",sans-serif;margin:16px;color:#1f2937;background:#f8fafc}}
h1,h2,h3{{margin:12px 0 8px}} .meta{{color:#475569}} .controls{{display:flex;gap:8px;flex-wrap:wrap;margin:12px 0;align-items:center}}
input,select,button{{padding:6px 8px}} table{{border-collapse:collapse;width:100%;font-size:12px;background:white}}
th,td{{border:1px solid #cbd5e1;padding:4px 6px;text-align:left;vertical-align:top;white-space:pre-wrap;word-break:break-word}}
th{{background:#e2e8f0;position:sticky;top:0}} tr.error td{{background:#fecaca}} tr.warn td{{background:#fef3c7}}
.summary{{margin-bottom:14px}} .summary th{{position:static}} .hidden{{display:none}}
.table-wrap{{max-width:100%;overflow-x:auto}} #log-table{{table-layout:fixed;width:max-content;min-width:100%}}
#log-table th{{position:sticky}} #log-table th .resizer{{position:absolute;top:0;right:-3px;width:7px;height:100%;cursor:col-resize;user-select:none}}
#log-table th .resizer:hover,#log-table th .resizer.active{{background:#64748b}}
.flags-details summary{{cursor:pointer;white-space:nowrap}} .flags-grid{{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:2px 10px;margin-top:4px;white-space:normal}}
.flags-grid span{{overflow-wrap:anywhere}} button{{cursor:pointer}}
.tabs{{display:flex;gap:6px;flex-wrap:wrap;margin:16px 0 10px}}
.tab-button{{border:1px solid #94a3b8;background:#e2e8f0;border-radius:8px;color:#0f172a}}
.tab-button.active{{background:#2563eb;color:white;border-color:#1d4ed8}}
.tab-panel{{display:none;background:white;border:1px solid #cbd5e1;border-radius:10px;padding:12px;margin-bottom:14px;box-shadow:0 1px 2px #0001}}
.tab-panel.active{{display:block}}
.cards{{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:10px;margin:12px 0}}
.card{{border:1px solid #cbd5e1;border-radius:10px;padding:10px;background:#f8fafc}}
.card .value{{font-size:22px;font-weight:700;margin-top:4px}} .card .hint{{color:#64748b;font-size:12px}}
.chart-grid{{display:grid;grid-template-columns:repeat(auto-fit,minmax(420px,1fr));gap:12px}}
.chart-card{{border:1px solid #cbd5e1;border-radius:10px;padding:10px;background:#fff}}
.chart-card canvas{{width:100%;height:260px;border:1px solid #e2e8f0;border-radius:6px;background:#ffffff}}
.state-canvas{{width:100%;height:520px;border:1px solid #e2e8f0;border-radius:6px;background:#ffffff}}
.note{{color:#64748b;font-size:12px;margin:6px 0}}
.excel-friendly{{margin-top:18px}}
.pill{{display:inline-block;border-radius:999px;background:#e2e8f0;padding:2px 8px;margin:2px;color:#334155}}
.chart-tooltip{{position:fixed;z-index:9999;display:none;max-width:360px;padding:8px 10px;border:1px solid #94a3b8;border-radius:8px;background:#0f172a;color:#f8fafc;font-size:12px;line-height:1.45;box-shadow:0 8px 24px #0003;pointer-events:none;white-space:pre-wrap}}
</style>
</head>
<body>
<h1>黑匣子日志报告</h1>
<div class="meta">源文件：{esc(source_name)} | 记录数：{len(rows)} | 运行次数：{len(summaries)} | 黑匣子总记录运行时长：{overview["total_duration_ms"] / 1000:.3f} 秒 | 黑匣子已重置：{"是" if overview["blackbox_reset"] else "否"} | 网络时间时区：{esc(timezone_label)}</div>

<div class="tabs">
<button class="tab-button active" type="button" data-tab="overview">总览</button>
<button class="tab-button" type="button" data-tab="charts">曲线</button>
<button class="tab-button" type="button" data-tab="flags">标志位</button>
<button class="tab-button" type="button" data-tab="protect">保护状态</button>
<button class="tab-button" type="button" data-tab="diagnostics">故障时间线</button>
<button class="tab-button" type="button" data-tab="logs">原始日志表</button>
</div>

<section id="tab-overview" class="tab-panel active">
<h2>总览</h2>
<div class="cards">
<div class="card"><div>记录数</div><div class="value">{len(rows)}</div><div class="hint">CSV 原始记录</div></div>
<div class="card"><div>运行次数</div><div class="value">{len(summaries)}</div><div class="hint">按启动后时间回退切分</div></div>
<div class="card"><div>诊断事件</div><div class="value">{len(model["diagnostic_events"])}</div><div class="hint">错误、警告、状态变化</div></div>
<div class="card"><div>总运行时长</div><div class="value">{overview["total_duration_ms"] / 1000:.1f}s</div><div class="hint">各运行片段累计</div></div>
</div>
<h2>运行摘要</h2>
<table class="summary"><thead><tr><th>运行</th><th>记录数</th><th>持续时间</th><th>错误数</th><th>警告数</th><th>包含网络时间</th><th>网络时间段</th><th>启动标记</th></tr></thead>
<tbody>{''.join(summary_rows)}</tbody></table>
<h2>解析提示</h2><ul>{warning_html}</ul>
</section>

<section id="tab-charts" class="tab-panel">
<h2>曲线</h2>
<div class="note">默认查看最新运行；有网络时间锚点时自动使用真实时间，没有锚点时使用启动后时间。选择“全部运行 + 启动后时间”可叠加比较多次启动后的行为。图表支持滚轮缩放、拖拽平移、双击重置、鼠标悬浮查看值。</div>
<div class="controls">
<label>运行 <select id="chart-run"><option value="{esc(latest_run)}">最新运行 {esc(latest_run)}</option><option value="">全部运行</option>{run_options}</select></label>
<label>时间轴 <select id="chart-time"><option value="auto">自动</option><option value="wall">网络时间</option><option value="uptime">启动后时间</option></select></label>
<span id="chart-note" class="note"></span>
</div>
<div class="chart-grid">
<div class="chart-card"><h3>电压</h3><canvas id="chart-voltage"></canvas></div>
<div class="chart-card"><h3>电流</h3><canvas id="chart-current"></canvas></div>
<div class="chart-card"><h3>累计能量</h3><canvas id="chart-meter"></canvas></div>
<div class="chart-card"><h3>温度</h3><canvas id="chart-temp"></canvas></div>
</div>
</section>

<section id="tab-flags" class="tab-panel">
<h2>标志位状态曲线</h2>
<div class="note">支持滚轮缩放、拖拽平移、双击重置、鼠标悬浮查看 bit 状态。</div>
<div class="controls">
<label>运行 <select id="flag-run"><option value="{esc(latest_run)}">最新运行 {esc(latest_run)}</option><option value="">全部运行</option>{run_options}</select></label>
<label>时间轴 <select id="flag-time"><option value="auto">自动</option><option value="wall">网络时间</option><option value="uptime">启动后时间</option></select></label>
<span class="pill">高电平/非 0 表示状态置位</span>
</div>
<canvas id="flags-canvas" class="state-canvas"></canvas>
</section>

<section id="tab-protect" class="tab-panel">
<h2>保护状态曲线</h2>
<div class="note">支持滚轮缩放、拖拽平移、双击重置、鼠标悬浮查看保护通道状态。</div>
<div class="controls">
<label>运行 <select id="protect-run"><option value="{esc(latest_run)}">最新运行 {esc(latest_run)}</option><option value="">全部运行</option>{run_options}</select></label>
<label>时间轴 <select id="protect-time"><option value="auto">自动</option><option value="wall">网络时间</option><option value="uptime">启动后时间</option></select></label>
<span class="pill">0 正常</span><span class="pill">1 警告</span><span class="pill">2 保护</span>
</div>
<canvas id="protect-canvas" class="state-canvas"></canvas>
</section>

<section id="tab-diagnostics" class="tab-panel">
<h2>故障时间线</h2>
<div class="controls">
<input id="diag-search" placeholder="筛选故障/状态变化">
<select id="diag-run"><option value="">全部运行</option>{run_options}</select>
<select id="diag-level"><option value="">全部级别</option><option>错误</option><option>警告</option><option>状态</option><option>信息</option></select>
<span id="diag-visible"></span>
</div>
<div class="table-wrap"><table id="diagnostic-table"><thead><tr><th>运行</th><th>网络时间</th><th>启动后时间(ms)</th><th>级别</th><th>类别</th><th>标题</th><th>详情</th><th>原始索引</th></tr></thead>
<tbody>{''.join(diagnostic_rows)}</tbody></table></div>
</section>

<section id="tab-logs" class="tab-panel">
<h2>原始日志表</h2>
<div class="controls">
<input id="search" placeholder="筛选文本">
<select id="run"><option value="">全部运行</option>{run_options}</select>
<select id="severity"><option value="">全部级别</option><option>错误</option><option>警告</option></select>
<button id="expand-flags" type="button">展开全部标志位</button>
<button id="collapse-flags" type="button">收起全部标志位</button>
<span id="visible"></span>
</div>
<div class="table-wrap"><table id="log-table"><colgroup>{colgroup}</colgroup><thead><tr>{headers}</tr></thead><tbody id="logs">{''.join(table_rows)}</tbody></table></div>
</section>

<section class="excel-friendly">
<h2>Excel 友好表格说明</h2>
<div class="note">Excel 打开本 HTML 时通常会忽略上方交互脚本，但仍能解析这些静态表格：运行摘要、故障时间线、原始日志表。</div>
</section>
<div id="chart-tooltip" class="chart-tooltip"></div>

<script>
const model={model_json};
const latestRun=String({json.dumps(str(latest_run), ensure_ascii=False)});
const rows=[...document.querySelectorAll('#logs tr')];
document.querySelectorAll('.tab-button').forEach(button=>{{
  button.addEventListener('click',()=>{{
    document.querySelectorAll('.tab-button').forEach(node=>node.classList.toggle('active',node===button));
    document.querySelectorAll('.tab-panel').forEach(panel=>panel.classList.toggle('active',panel.id==='tab-'+button.dataset.tab));
    setTimeout(renderInteractive,0);
  }});
}});
function applyFilter(){{
  const search=document.querySelector('#search').value.toLowerCase();
  const run=document.querySelector('#run').value;
  const severity=document.querySelector('#severity').value;
  let visible=0;
  rows.forEach(row=>{{
    const show=(!search||row.dataset.search.includes(search))&&(!run||row.dataset.run===run)&&(!severity||row.dataset.severity===severity);
    row.classList.toggle('hidden',!show); if(show) visible++;
  }});
  document.querySelector('#visible').textContent=`显示：${{visible}} / ${{rows.length}}`;
}}
document.querySelectorAll('input,select').forEach(node=>node.addEventListener('input',applyFilter));
document.querySelector('#expand-flags').addEventListener('click',()=>document.querySelectorAll('.flags-details').forEach(node=>node.open=true));
document.querySelector('#collapse-flags').addEventListener('click',()=>document.querySelectorAll('.flags-details').forEach(node=>node.open=false));
const diagRows=[...document.querySelectorAll('#diagnostic-table tbody tr')];
function applyDiagFilter(){{
  const search=document.querySelector('#diag-search').value.toLowerCase();
  const run=document.querySelector('#diag-run').value;
  const level=document.querySelector('#diag-level').value;
  let visible=0;
  diagRows.forEach(row=>{{
    const cells=[...row.cells].map(cell=>cell.textContent);
    const show=(!search||cells.join(' ').toLowerCase().includes(search))&&(!run||cells[0]===run)&&(!level||cells[3]===level);
    row.classList.toggle('hidden',!show); if(show) visible++;
  }});
  document.querySelector('#diag-visible').textContent='显示：'+visible+' / '+diagRows.length;
}}
['diag-search','diag-run','diag-level'].forEach(id=>document.querySelector('#'+id).addEventListener('input',applyDiagFilter));
const columns=[...document.querySelectorAll('#log-table col')];
document.querySelectorAll('#log-table .resizer').forEach((resizer,index)=>{{
  resizer.addEventListener('mousedown',event=>{{
    event.preventDefault();
    const startX=event.clientX;
    const startWidth=columns[index].getBoundingClientRect().width;
    resizer.classList.add('active');
    const move=moveEvent=>{{
      columns[index].style.width=`${{Math.max(48,startWidth+moveEvent.clientX-startX)}}px`;
    }};
    const stop=()=>{{
      resizer.classList.remove('active');
      document.removeEventListener('mousemove',move);
      document.removeEventListener('mouseup',stop);
    }};
    document.addEventListener('mousemove',move);
    document.addEventListener('mouseup',stop);
  }});
}});
function resizeCanvas(canvas){{
  const rect=canvas.getBoundingClientRect();
  const ratio=window.devicePixelRatio||1;
  const w=Math.max(320,Math.floor(rect.width*ratio));
  const h=Math.max(180,Math.floor(rect.height*ratio));
  if(canvas.width!==w||canvas.height!==h){{canvas.width=w;canvas.height=h;}}
  return ratio;
}}
function pickAxis(points, run, wanted){{
  if(wanted==='uptime') return 'uptime';
  if(wanted==='wall'){{
    const usable=points.filter(p=>p.wall_epoch_ms!==null&&p.wall_epoch_ms!==undefined);
    return usable.length?'wall':'uptime';
  }}
  if(run){{
    return points.some(p=>p.wall_epoch_ms!==null&&p.wall_epoch_ms!==undefined)?'wall':'uptime';
  }}
  return 'uptime';
}}
function filteredPoints(run, wanted){{
  let points=model.points.filter(p=>!run||String(p.run_id)===String(run));
  const axis=pickAxis(points,run,wanted);
  if(axis==='wall') points=points.filter(p=>p.wall_epoch_ms!==null&&p.wall_epoch_ms!==undefined);
  points.sort((a,b)=>(axis==='wall'?a.wall_epoch_ms-b.wall_epoch_ms:a.uptime_ms-b.uptime_ms)||String(a.run_id).localeCompare(String(b.run_id)));
  return {{points,axis}};
}}
function xValue(point,axis){{return axis==='wall'?point.wall_epoch_ms:point.uptime_ms;}}
function fmtTime(value,axis){{
  if(axis==='wall') return new Date(value).toLocaleString();
  const total=Math.max(0,Math.floor(value/1000));
  const ms=Math.floor(value%1000).toString().padStart(3,'0');
  const s=(total%60).toString().padStart(2,'0');
  const m=(Math.floor(total/60)%60).toString().padStart(2,'0');
  const h=Math.floor(total/3600).toString().padStart(2,'0');
  return '+'+h+':'+m+':'+s+'.'+ms;
}}
function fmtDuration(ms){{
  if(ms===null||ms===undefined||Number.isNaN(ms)) return '';
  const sign=ms<0?'-':'';
  ms=Math.abs(Math.round(ms));
  const total=Math.floor(ms/1000);
  const millis=(ms%1000).toString().padStart(3,'0');
  const seconds=(total%60).toString().padStart(2,'0');
  const minutes=(Math.floor(total/60)%60).toString().padStart(2,'0');
  const hours=Math.floor(total/3600);
  return sign+(hours?hours+':':'')+minutes+':'+seconds+'.'+millis;
}}
function drawEmpty(ctx,w,h,text){{ctx.clearRect(0,0,w,h);ctx.fillStyle='#64748b';ctx.font='14px sans-serif';ctx.fillText(text,20,32);}}
const chartViews={{}};
const tooltip=document.querySelector('#chart-tooltip');
function getView(canvasId){{return chartViews[canvasId]||(chartViews[canvasId]={{installed:false,dragging:false,items:[]}});}}
function resetView(view,minX,maxX){{view.minX=minX;view.maxX=maxX;view.fullMinX=minX;view.fullMaxX=maxX;}}
function ensureView(canvasId,signature,minX,maxX){{
  const view=getView(canvasId);
  if(view.signature!==signature||view.minX===undefined||view.maxX===undefined){{
    view.signature=signature;
    resetView(view,minX,maxX);
  }} else {{
    view.fullMinX=minX; view.fullMaxX=maxX;
    const fullSpan=Math.max(1,maxX-minX);
    let span=Math.max(1,view.maxX-view.minX);
    if(span>fullSpan) span=fullSpan;
    view.minX=Math.max(minX,Math.min(maxX-span,view.minX));
    view.maxX=view.minX+span;
  }}
  return view;
}}
function clampView(view){{
  const fullSpan=Math.max(1,view.fullMaxX-view.fullMinX);
  let span=Math.max(1,view.maxX-view.minX);
  if(span>fullSpan) span=fullSpan;
  if(view.minX<view.fullMinX){{view.minX=view.fullMinX;view.maxX=view.minX+span;}}
  if(view.maxX>view.fullMaxX){{view.maxX=view.fullMaxX;view.minX=view.maxX-span;}}
  if(view.minX<view.fullMinX) view.minX=view.fullMinX;
}}
function dataXFromCanvas(view,clientX,canvas){{
  const rect=canvas.getBoundingClientRect();
  const ratio=window.devicePixelRatio||1;
  const x=(clientX-rect.left)*ratio;
  const plotW=Math.max(1,canvas.width-view.pad.l-view.pad.r);
  return view.minX+(x-view.pad.l)/plotW*(view.maxX-view.minX);
}}
function showTooltip(event,text){{
  if(!text){{tooltip.style.display='none';return;}}
  tooltip.textContent=text;
  tooltip.style.display='block';
  const margin=14;
  const rect=tooltip.getBoundingClientRect();
  let left=event.clientX+margin;
  let top=event.clientY+margin;
  if(left+rect.width>window.innerWidth) left=event.clientX-rect.width-margin;
  if(top+rect.height>window.innerHeight) top=event.clientY-rect.height-margin;
  tooltip.style.left=Math.max(4,left)+'px';
  tooltip.style.top=Math.max(4,top)+'px';
}}
function nearestLineItem(view,event,canvas){{
  const rect=canvas.getBoundingClientRect();
  const ratio=window.devicePixelRatio||1;
  const mx=(event.clientX-rect.left)*ratio, my=(event.clientY-rect.top)*ratio;
  let best=null, bestDist=Infinity;
  for(const item of view.items||[]){{
    const dx=item.px-mx, dy=item.py-my;
    const dist=Math.sqrt(dx*dx+dy*dy);
    if(dist<bestDist){{best=item;bestDist=dist;}}
  }}
  return best&&bestDist<=18*ratio?best:null;
}}
function drawLineHover(canvas,item){{
  const view=getView(canvas.id);
  if(!item||!view.pad) return;
  const ctx=canvas.getContext('2d');
  const ratio=window.devicePixelRatio||1;
  ctx.save();
  ctx.setLineDash([4*ratio,4*ratio]);
  ctx.strokeStyle='#0f172a';
  ctx.lineWidth=1*ratio;
  ctx.beginPath();
  ctx.moveTo(item.px,view.pad.t);
  ctx.lineTo(item.px,canvas.height-view.pad.b);
  ctx.stroke();
  ctx.setLineDash([]);
  ctx.fillStyle='#ffffff';
  ctx.strokeStyle=item.color||'#0f172a';
  ctx.lineWidth=3*ratio;
  ctx.beginPath();
  ctx.arc(item.px,item.py,5*ratio,0,Math.PI*2);
  ctx.fill();
  ctx.stroke();
  ctx.restore();
}}
function drawStateHover(canvas,item){{
  const view=getView(canvas.id);
  if(!item||!view.pad) return;
  const ctx=canvas.getContext('2d');
  const ratio=window.devicePixelRatio||1;
  ctx.save();
  ctx.strokeStyle='#0f172a';
  ctx.lineWidth=2*ratio;
  ctx.strokeRect(item.x1,item.y1,Math.max(1,item.x2-item.x1),Math.max(1,item.y2-item.y1));
  ctx.setLineDash([4*ratio,4*ratio]);
  ctx.lineWidth=1*ratio;
  ctx.beginPath();
  ctx.moveTo(item.x1,view.pad.t);
  ctx.lineTo(item.x1,canvas.height-view.pad.b);
  ctx.moveTo(item.x2,view.pad.t);
  ctx.lineTo(item.x2,canvas.height-view.pad.b);
  ctx.stroke();
  ctx.restore();
}}
function stateItemAt(view,event,canvas){{
  const rect=canvas.getBoundingClientRect();
  const ratio=window.devicePixelRatio||1;
  const mx=(event.clientX-rect.left)*ratio, my=(event.clientY-rect.top)*ratio;
  return (view.items||[]).find(item=>mx>=item.x1&&mx<=item.x2&&my>=item.y1&&my<=item.y2);
}}
function installCanvasInteraction(canvasId){{
  const canvas=document.querySelector('#'+canvasId);
  const view=getView(canvasId);
  if(!canvas||view.installed) return;
  view.installed=true;
  canvas.addEventListener('wheel',event=>{{
    if(view.minX===undefined||!view.pad) return;
    event.preventDefault();
    const focus=dataXFromCanvas(view,event.clientX,canvas);
    const factor=event.deltaY<0?0.8:1.25;
    const minSpan=Math.max(1,(view.fullMaxX-view.fullMinX)/100000);
    let span=Math.max(minSpan,(view.maxX-view.minX)*factor);
    span=Math.min(span,view.fullMaxX-view.fullMinX);
    const ratio=(focus-view.minX)/(view.maxX-view.minX);
    view.minX=focus-span*ratio;
    view.maxX=view.minX+span;
    clampView(view);
    renderInteractive();
  }},{{passive:false}});
  canvas.addEventListener('mousedown',event=>{{
    if(view.minX===undefined||!view.pad) return;
    view.dragging=true; view.dragStartX=event.clientX; view.dragMinX=view.minX; view.dragMaxX=view.maxX;
    canvas.style.cursor='grabbing';
  }});
  window.addEventListener('mouseup',()=>{{view.dragging=false; canvas.style.cursor='';}});
  canvas.addEventListener('mousemove',event=>{{
    if(view.dragging){{
      const rect=canvas.getBoundingClientRect();
      const ratio=window.devicePixelRatio||1;
      const plotW=Math.max(1,canvas.width-view.pad.l-view.pad.r);
      const dx=(event.clientX-view.dragStartX)*ratio;
      const delta=dx/plotW*(view.dragMaxX-view.dragMinX);
      view.minX=view.dragMinX-delta; view.maxX=view.dragMaxX-delta;
      clampView(view);
      renderInteractive();
      return;
    }}
    const item=view.kind==='state'?stateItemAt(view,event,canvas):nearestLineItem(view,event,canvas);
    renderInteractive();
    if(item){{view.kind==='state'?drawStateHover(canvas,item):drawLineHover(canvas,item);}}
    showTooltip(event,item?item.tooltip:'');
  }});
  canvas.addEventListener('mouseleave',()=>{{tooltip.style.display='none'; view.dragging=false; canvas.style.cursor='';}});
  canvas.addEventListener('dblclick',()=>{{if(view.fullMinX!==undefined){{resetView(view,view.fullMinX,view.fullMaxX);renderInteractive();}}}});
}}
function drawLine(canvasId, configs, run, wanted){{
  const canvas=document.querySelector('#'+canvasId); if(!canvas) return;
  installCanvasInteraction(canvasId);
  const ratio=resizeCanvas(canvas), ctx=canvas.getContext('2d'), w=canvas.width, h=canvas.height;
  const result=filteredPoints(run,wanted), axis=result.axis;
  let series=[];
  configs.forEach(cfg=>{{
    const groups=new Map();
    result.points.forEach(p=>{{
      const value=p[cfg.key];
      if(value===null||value===undefined||Number.isNaN(value)) return;
      const key=run?'':String(p.run_id);
      if(!groups.has(key)) groups.set(key,[]);
      groups.get(key).push({{x:xValue(p,axis),y:value,run:p.run_id}});
    }});
    groups.forEach((pts,key)=>series.push({{label:cfg.label+(key?' · 运行 '+key:''),color:cfg.color,points:pts}}));
  }});
  const all=series.flatMap(s=>s.points);
  if(!all.length){{drawEmpty(ctx,w,h,'没有可绘制的数据');return;}}
  const pad={{l:58*ratio,r:18*ratio,t:20*ratio,b:36*ratio}};
  let fullMinX=Math.min(...all.map(p=>p.x)), fullMaxX=Math.max(...all.map(p=>p.x));
  if(fullMinX===fullMaxX){{fullMinX-=1;fullMaxX+=1;}}
  const signature=canvasId+'|'+String(run)+'|'+wanted+'|'+axis+'|'+all.length+'|'+fullMinX+'|'+fullMaxX;
  const view=ensureView(canvasId,signature,fullMinX,fullMaxX);
  view.kind='line'; view.axis=axis; view.pad=pad; view.items=[];
  let minX=view.minX, maxX=view.maxX;
  let minY=Math.min(...all.map(p=>p.y)), maxY=Math.max(...all.map(p=>p.y));
  if(minY===maxY){{minY-=1;maxY+=1;}}
  const yPad=(maxY-minY)*0.08; minY-=yPad; maxY+=yPad;
  const sx=x=>pad.l+(x-minX)/(maxX-minX)*(w-pad.l-pad.r);
  const sy=y=>h-pad.b-(y-minY)/(maxY-minY)*(h-pad.t-pad.b);
  ctx.clearRect(0,0,w,h);
  ctx.strokeStyle='#cbd5e1';ctx.lineWidth=1*ratio;ctx.beginPath();ctx.moveTo(pad.l,pad.t);ctx.lineTo(pad.l,h-pad.b);ctx.lineTo(w-pad.r,h-pad.b);ctx.stroke();
  ctx.fillStyle='#64748b';ctx.font=(11*ratio)+'px sans-serif';
  for(let i=0;i<=4;i++){{const y=minY+(maxY-minY)*i/4;const py=sy(y);ctx.strokeStyle='#e2e8f0';ctx.beginPath();ctx.moveTo(pad.l,py);ctx.lineTo(w-pad.r,py);ctx.stroke();ctx.fillText(y.toFixed(2),4*ratio,py+4*ratio);}}
  ctx.fillText(fmtTime(minX,axis),pad.l,h-10*ratio);ctx.textAlign='right';ctx.fillText(fmtTime(maxX,axis),w-pad.r,h-10*ratio);ctx.textAlign='left';
  const palette=['#2563eb','#dc2626','#16a34a','#9333ea','#ea580c','#0891b2','#4f46e5','#be123c'];
  series.forEach((s,idx)=>{{
    const color=run?s.color:palette[idx%palette.length];
    ctx.strokeStyle=color;ctx.lineWidth=1.8*ratio;ctx.beginPath();
    s.points.forEach((p,i)=>{{const px=sx(p.x),py=sy(p.y); if(i===0)ctx.moveTo(px,py); else ctx.lineTo(px,py);
      if(p.x>=minX&&p.x<=maxX) view.items.push({{px,py,color,tooltip:s.label+'\\n时间: '+fmtTime(p.x,axis)+'\\n值: '+p.y+'\\n运行: '+p.run}});
    }});
    ctx.stroke();
    const visible=s.points.filter(p=>p.x>=minX&&p.x<=maxX);
    const markerStep=Math.max(1,Math.ceil(visible.length/160));
    visible.forEach((p,i)=>{{
      if(i%markerStep!==0&&i!==visible.length-1) return;
      const px=sx(p.x), py=sy(p.y);
      ctx.fillStyle='#ffffff';
      ctx.strokeStyle=color;
      ctx.lineWidth=1.4*ratio;
      ctx.beginPath();
      ctx.arc(px,py,2.6*ratio,0,Math.PI*2);
      ctx.fill();
      ctx.stroke();
    }});
    if(visible.length){{
      const latest=visible[visible.length-1];
      const px=sx(latest.x), py=sy(latest.y);
      ctx.fillStyle=color;
      ctx.strokeStyle='#ffffff';
      ctx.lineWidth=2.5*ratio;
      ctx.beginPath();
      ctx.arc(px,py,6*ratio,0,Math.PI*2);
      ctx.fill();
      ctx.stroke();
    }}
  }});
  ctx.fillStyle='#334155';ctx.font=(12*ratio)+'px sans-serif';ctx.fillText((axis==='wall'?'网络时间':'启动后时间')+' · '+all.length+' 点 · 显示 '+fmtTime(minX,axis)+' 至 '+fmtTime(maxX,axis),pad.l,pad.t-6*ratio);
}}
function drawState(canvasId, fields, valueKey, run, wanted, labels){{
  const canvas=document.querySelector('#'+canvasId); if(!canvas) return;
  installCanvasInteraction(canvasId);
  const ratio=resizeCanvas(canvas), ctx=canvas.getContext('2d'), w=canvas.width, h=canvas.height;
  const result=filteredPoints(run,wanted), axis=result.axis;
  const points=result.points.filter(p=>p[valueKey]!==null&&p[valueKey]!==undefined);
  if(!points.length||!fields.length){{drawEmpty(ctx,w,h,'没有可绘制的数据');return;}}
  let fullMinX=Math.min(...points.map(p=>xValue(p,axis))), fullMaxX=Math.max(...points.map(p=>xValue(p,axis)));
  if(fullMinX===fullMaxX){{fullMinX-=1;fullMaxX+=1;}}
  const pad={{l:190*ratio,r:18*ratio,t:22*ratio,b:34*ratio}};
  const signature=canvasId+'|'+String(run)+'|'+wanted+'|'+axis+'|'+points.length+'|'+fullMinX+'|'+fullMaxX;
  const view=ensureView(canvasId,signature,fullMinX,fullMaxX);
  view.kind='state'; view.axis=axis; view.pad=pad; view.items=[];
  let minX=view.minX, maxX=view.maxX;
  const rowH=Math.max(22*ratio,(h-pad.t-pad.b)/fields.length);
  const sx=x=>pad.l+(x-minX)/(maxX-minX)*(w-pad.l-pad.r);
  ctx.clearRect(0,0,w,h);ctx.font=(11*ratio)+'px sans-serif';ctx.fillStyle='#334155';
  ctx.fillText((axis==='wall'?'网络时间':'启动后时间')+' · '+points.length+' 点 · 显示 '+fmtTime(minX,axis)+' 至 '+fmtTime(maxX,axis),pad.l,pad.t-8*ratio);
  fields.forEach((field,idx)=>{{
    const y=pad.t+idx*rowH;
    ctx.fillStyle='#334155';ctx.textAlign='right';ctx.fillText(field.label,pad.l-8*ratio,y+rowH*0.65);ctx.textAlign='left';
    ctx.strokeStyle='#e2e8f0';ctx.beginPath();ctx.moveTo(pad.l,y+rowH*0.5);ctx.lineTo(w-pad.r,y+rowH*0.5);ctx.stroke();
    let last=null;
    points.forEach((p,i)=>{{
      const raw=p[valueKey]; const value=(raw>>field.offset)&((1<<field.width)-1);
      const startValue=xValue(p,axis);
      const endValue=i+1<points.length?xValue(points[i+1],axis):maxX;
      const next=sx(endValue);
      const x=sx(startValue); const width=Math.max(1,next-x);
      if(next<pad.l||x>w-pad.r){{last=value;return;}}
      if(valueKey==='protect') ctx.fillStyle=value===2?'#dc2626':(value===1?'#f59e0b':'#22c55e');
      else ctx.fillStyle=value?'#2563eb':'#e2e8f0';
      const x1=Math.max(pad.l,x), x2=Math.min(w-pad.r,next);
      const y1=y+4*ratio, y2=y+Math.max(4*ratio,rowH-4*ratio);
      ctx.fillRect(x1,y1,Math.max(1,x2-x1),Math.max(4*ratio,rowH-8*ratio));
      const valueLabel=labels?(labels[String(value)]||String(value)):String(value);
      const endNote=i+1<points.length?'':'（可视结束）';
      view.items.push({{
        x1,x2,y1,y2,
        tooltip:field.label+'\\n开始: '+fmtTime(startValue,axis)+'\\n结束: '+fmtTime(endValue,axis)+endNote+'\\n持续: '+fmtDuration(endValue-startValue)+'\\n值: '+valueLabel+' ('+value+')\\n运行: '+p.run_id
      }});
      last=value;
    }});
    if(labels&&last!==null){{ctx.fillStyle='#475569';ctx.fillText(labels[String(last)]||String(last),w-pad.r-56*ratio,y+rowH*0.65);}}
  }});
  ctx.fillStyle='#64748b';ctx.fillText(fmtTime(minX,axis),pad.l,h-10*ratio);ctx.textAlign='right';ctx.fillText(fmtTime(maxX,axis),w-pad.r,h-10*ratio);ctx.textAlign='left';
}}
function selected(id){{return document.querySelector('#'+id).value;}}
function renderInteractive(){{
  drawLine('chart-voltage',[{{key:'voltage_mv',label:'电压(mV)',color:'#2563eb'}}],selected('chart-run'),selected('chart-time'));
  drawLine('chart-current',[{{key:'current_ua',label:'电流(uA)',color:'#dc2626'}}],selected('chart-run'),selected('chart-time'));
  drawLine('chart-meter',[{{key:'meter_mwh',label:'累计能量(mWh)',color:'#16a34a'}}],selected('chart-run'),selected('chart-time'));
  drawLine('chart-temp',[{{key:'board_temp_c',label:'板温(C)',color:'#ea580c'}},{{key:'chip_temp_c',label:'芯片温度(C)',color:'#9333ea'}}],selected('chart-run'),selected('chart-time'));
  drawState('flags-canvas',model.flag_fields,'flags',selected('flag-run'),selected('flag-time'),null);
  drawState('protect-canvas',model.protect_fields,'protect',selected('protect-run'),selected('protect-time'),model.protect_state_labels);
  const probe=filteredPoints(selected('chart-run'),selected('chart-time'));
  const baseCount=model.points.filter(p=>!selected('chart-run')||String(p.run_id)===String(selected('chart-run'))).length;
  const missing=selected('chart-time')==='wall'?baseCount-probe.points.length:0;
  document.querySelector('#chart-note').textContent=(probe.axis==='wall'?'当前使用网络时间':'当前使用启动后时间')+(missing>0?'；部分记录无网络时间未参与绘制':'');
}}
['chart-run','chart-time','flag-run','flag-time','protect-run','protect-time'].forEach(id=>document.querySelector('#'+id).addEventListener('input',renderInteractive));
window.addEventListener('resize',renderInteractive);
applyFilter();
applyDiagFilter();
renderInteractive();
</script>
</body></html>
"""


def main() -> int:
    args = parse_args()
    output = args.output or args.input.with_suffix(".html")
    try:
        rows, csv_columns = read_csv(args.input)
        global_fields, protect_fields, protect_states, warnings = load_source_definitions(
            args.project_root, args.no_source_flags
        )
        chronological, run_warnings = assign_runs(rows)
        warnings.extend(run_warnings)
        unknown_columns = [column for column in csv_columns if column not in ORIGINAL_COLUMNS]
        if unknown_columns:
            warnings.append(f"CSV 包含报告中未显示的额外列：{', '.join(unknown_columns)}")
        tz = timezone(timedelta(hours=args.timezone_offset))
        enrich_rows(chronological, global_fields, protect_fields, protect_states, tz)
        summaries = summarize_runs(chronological)
        overview = summarize_overview(chronological, summaries)
        model = build_interactive_model(chronological, summaries, global_fields, protect_fields, protect_states)
        sign = "+" if args.timezone_offset >= 0 else "-"
        timezone_label = f"UTC{sign}{abs(args.timezone_offset):02d}:00"
        report = render_html(chronological, summaries, overview, warnings, args.input.name, timezone_label, model)
        output.write_text("\ufeff" + report, encoding="utf-8")
    except (OSError, ValueError) as error:
        print(f"错误：{error}", file=sys.stderr)
        return 1
    print(f"已生成：{output}")
    print(f"记录数：{len(rows)}，运行次数：{len(summaries)}，解析提示：{len(warnings)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
