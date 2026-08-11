#!/usr/bin/env python3

import json
import os
import re
import socket
import subprocess
import time
import urllib.request
from pathlib import Path


STATUS_URL = os.environ.get("VIBE_STATUS_URL", "http://127.0.0.1:8880/status")
GRAPHITE_HOST = os.environ.get("GRAPHITE_HOST", "db")
GRAPHITE_PORT = int(os.environ.get("GRAPHITE_PORT", "2003"))
STATE_PATH = Path(
    os.environ.get("VIBE_DIAGNOSTICS_STATE", "/var/lib/vibe-motion-diagnostics/state.json")
)
HOST_COMPONENT = re.sub(r"[^A-Za-z0-9_-]", "_", socket.gethostname())
PREFIX = os.environ.get("VIBE_DIAGNOSTICS_PREFIX", f"{HOST_COMPONENT}.vibe_motion")
CLK_TCK = os.sysconf("SC_CLK_TCK")


def metric_component(value):
    return re.sub(r"[^A-Za-z0-9_-]", "_", str(value))


def read_status():
    with urllib.request.urlopen(STATUS_URL, timeout=8) as response:
        return json.load(response)


def main_pid():
    result = subprocess.run(
        ["systemctl", "show", "vibe-motion", "--property", "MainPID", "--value"],
        check=True,
        capture_output=True,
        text=True,
        timeout=5,
    )
    pid = int(result.stdout.strip())
    if pid <= 0:
        raise RuntimeError("vibe-motion has no active MainPID")
    return pid


def process_stat(path):
    text = path.read_text()
    closing = text.rfind(")")
    if closing < 0:
        raise ValueError(f"invalid proc stat: {path}")
    fields = text[closing + 2 :].split()
    return int(fields[11]) + int(fields[12])


def process_snapshot(pid):
    proc = Path("/proc") / str(pid)
    groups = {}
    tasks = {}
    for task in (proc / "task").iterdir():
        try:
            name = task.joinpath("comm").read_text().strip()
            ticks = process_stat(task / "stat")
        except (FileNotFoundError, ProcessLookupError, PermissionError, ValueError):
            continue
        tasks[task.name] = {"name": name, "ticks": ticks}
        entry = groups.setdefault(name, {"threads": 0})
        entry["threads"] += 1

    rss_bytes = 0
    for line in (proc / "status").read_text().splitlines():
        if line.startswith("VmRSS:"):
            rss_bytes = int(line.split()[1]) * 1024
            break
    return {
        "ticks": process_stat(proc / "stat"),
        "rss_bytes": rss_bytes,
        "groups": groups,
        "tasks": tasks,
    }


def thread_cpu_ticks(snapshot, previous):
    """Attribute CPU only when the same TID kept the same name for the interval.

    A camera source thread is temporarily renamed while it submits a timelapse
    frame. Comparing totals grouped only by the name visible at each sample can
    therefore assign that thread's lifetime CPU to the wrong group. TID deltas
    avoid that inflation; intervals crossing a rename are reported separately
    because their CPU cannot be split reliably from two snapshots.
    """
    attributed = {}
    unattributed = 0
    old_tasks = previous.get("tasks", {})
    for tid, task in snapshot["tasks"].items():
        old = old_tasks.get(tid)
        if not old or task["ticks"] < old.get("ticks", 0):
            continue
        delta = task["ticks"] - old["ticks"]
        if task["name"] == old.get("name"):
            attributed[task["name"]] = attributed.get(task["name"], 0) + delta
        else:
            unattributed += delta
    return attributed, unattributed


def available_memory_bytes():
    for line in Path("/proc/meminfo").read_text().splitlines():
        if line.startswith("MemAvailable:"):
            return int(line.split()[1]) * 1024
    return 0


def load_state():
    try:
        return json.loads(STATE_PATH.read_text())
    except (FileNotFoundError, json.JSONDecodeError, PermissionError):
        return {}


def save_state(state):
    STATE_PATH.parent.mkdir(parents=True, exist_ok=True)
    temporary = STATE_PATH.with_suffix(".tmp")
    temporary.write_text(json.dumps(state, separators=(",", ":")))
    os.replace(temporary, STATE_PATH)


def add_metric(metrics, path, value):
    if isinstance(value, bool):
        value = int(value)
    if isinstance(value, (int, float)):
        metrics[f"{PREFIX}.{path}"] = value


def add_rate(metrics, path, current, previous, elapsed, scale=1.0):
    if elapsed > 0 and current >= previous:
        add_metric(metrics, path, (current - previous) * scale / elapsed)


def camera_metrics(metrics, status, previous, elapsed):
    current_state = {}
    counter_fields = {
        "frames": "decoded_frames_total",
        "input_packets": "input_packets_total",
        "input_bytes": "input_bytes_total",
        "reconnects": "reconnects_total",
        "timelapse_frames": "timelapse_frames_total",
        "timelapse_packets": "timelapse_packets_total",
        "timelapse_bytes": "timelapse_bytes_total",
        "timelapse_keyframes": "timelapse_keyframes_total",
        "timelapse_write_us_total": "timelapse_write_us_total",
    }
    gauge_fields = {
        "keyframe_interval_ms": "keyframe_interval_ms",
        "timelapse_write_us_last": "timelapse_write_ms_last",
        "timelapse_write_us_max": "timelapse_write_ms_max",
    }

    for camera in status.get("cameras", []):
        camera_id = metric_component(camera.get("id", "unknown"))
        base = f"camera.{camera_id}"
        counters = {}
        add_metric(metrics, f"{base}.connected", camera.get("connected", False))
        add_metric(metrics, f"{base}.timelapse_healthy", not camera.get("timelapse_error", ""))
        for source, target in counter_fields.items():
            value = camera.get(source)
            if isinstance(value, (int, float)):
                counters[source] = value
                add_metric(metrics, f"{base}.{target}", value)
        for source, target in gauge_fields.items():
            value = camera.get(source)
            if isinstance(value, (int, float)):
                if source.startswith("timelapse_write_us_"):
                    value /= 1000.0
                add_metric(metrics, f"{base}.{target}", value)

        old = previous.get(str(camera.get("id")), {})
        if old and elapsed > 0:
            if "input_bytes" in counters and "input_bytes" in old:
                add_rate(
                    metrics,
                    f"{base}.input_mbps",
                    counters["input_bytes"],
                    old["input_bytes"],
                    elapsed,
                    8.0 / 1_000_000.0,
                )
            if "timelapse_bytes" in counters and "timelapse_bytes" in old:
                add_rate(
                    metrics,
                    f"{base}.timelapse_output_kbps",
                    counters["timelapse_bytes"],
                    old["timelapse_bytes"],
                    elapsed,
                    8.0 / 1000.0,
                )
            if "timelapse_frames" in counters and "timelapse_frames" in old:
                add_rate(
                    metrics,
                    f"{base}.timelapse_fps",
                    counters["timelapse_frames"],
                    old["timelapse_frames"],
                    elapsed,
                )
            if (
                "timelapse_write_us_total" in counters
                and "timelapse_write_us_total" in old
                and "timelapse_frames" in counters
                and "timelapse_frames" in old
            ):
                write_us = (
                    counters["timelapse_write_us_total"] - old["timelapse_write_us_total"]
                )
                frames = counters["timelapse_frames"] - old["timelapse_frames"]
                if write_us >= 0:
                    add_metric(
                        metrics,
                        f"{base}.timelapse_write_busy_percent",
                        write_us / elapsed / 10_000.0,
                    )
                if write_us >= 0 and frames > 0:
                    add_metric(
                        metrics,
                        f"{base}.timelapse_write_ms_avg",
                        write_us / frames / 1000.0,
                    )
        current_state[str(camera.get("id"))] = counters
    return current_state


def send_metrics(metrics, timestamp):
    payload = "".join(f"{name} {value} {timestamp}\n" for name, value in sorted(metrics.items()))
    if os.environ.get("VIBE_DIAGNOSTICS_DRY_RUN"):
        print(payload, end="")
        return
    with socket.create_connection((GRAPHITE_HOST, GRAPHITE_PORT), timeout=8) as connection:
        connection.sendall(payload.encode())


def main():
    now = int(time.time())
    pid = main_pid()
    status = read_status()
    snapshot = process_snapshot(pid)
    previous = load_state()
    elapsed = now - previous.get("timestamp", now)
    same_process = previous.get("pid") == pid and elapsed > 0
    metrics = {}

    add_metric(metrics, "diagnostics.heartbeat", 1)
    add_metric(metrics, "process.rss_bytes", snapshot["rss_bytes"])
    add_metric(metrics, "process.available_memory_bytes", available_memory_bytes())
    add_metric(
        metrics,
        "process.threads",
        sum(group["threads"] for group in snapshot["groups"].values()),
    )
    if same_process:
        add_rate(
            metrics,
            "process.cpu_percent",
            snapshot["ticks"],
            previous.get("process_ticks", 0),
            elapsed,
            100.0 / CLK_TCK,
        )

    attributed_ticks = {}
    unattributed_ticks = 0
    if same_process and previous.get("tasks"):
        attributed_ticks, unattributed_ticks = thread_cpu_ticks(snapshot, previous)
        add_metric(
            metrics,
            "process.thread_cpu_unattributed_percent",
            unattributed_ticks * 100.0 / CLK_TCK / elapsed,
        )
    for name, group in snapshot["groups"].items():
        component = metric_component(name)
        add_metric(metrics, f"thread_group.{component}.threads", group["threads"])
        if name in attributed_ticks:
            add_metric(
                metrics,
                f"thread_group.{component}.cpu_percent",
                attributed_ticks[name] * 100.0 / CLK_TCK / elapsed,
            )

    camera_state = camera_metrics(
        metrics, status, previous.get("cameras", {}) if same_process else {}, elapsed
    )
    send_metrics(metrics, now)
    save_state(
        {
            "timestamp": now,
            "pid": pid,
            "process_ticks": snapshot["ticks"],
            "tasks": snapshot["tasks"],
            "cameras": camera_state,
        }
    )


if __name__ == "__main__":
    main()
