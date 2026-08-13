#!/usr/bin/env python3
"""Validate EchoEar phase-two music behavior from ESP-IDF serial logs."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
TIME_RE = re.compile(r"\b[IEWD] \((\d+)\)")
METADATA_RE = re.compile(r"MUSIC_METRIC metadata .*?elapsed_ms=(\d+)")
ARTWORK_RE = re.compile(r"MUSIC_METRIC artwork .*?available=(\d).*?elapsed_ms=(\d+)")
AUDIO_RE = re.compile(
    r"MUSIC_METRIC audio_complete .*?underruns=(\d+).*?underrun_total_ms=(\d+)")
RESOURCE_RE = re.compile(
    r"MUSIC_METRIC resources released .*?elapsed_ms=(\d+).*?internal_delta=(-?\d+).*?psram_delta=(-?\d+)")
OVERLAY_RE = re.compile(r"MUSIC_METRIC overlay visible=(\d)")
BUFFER_RELEASE_RE = re.compile(r"MUSIC_METRIC audio_buffer_release bytes=(\d+) chunks=(\d+)")
WORKERS_REAPED_RE = re.compile(r"MUSIC_METRIC workers_reaped generation=(\d+)")


def clean_log(raw: str) -> str:
    return ANSI_RE.sub("", raw).replace("\r", "")


def elapsed_hours(log: str) -> float:
    times = [int(value) for value in TIME_RE.findall(log)]
    if len(times) < 2:
        return 0.0
    longest_ms = 0
    start = previous = times[0]
    for value in times[1:]:
        if value < previous:
            longest_ms = max(longest_ms, previous - start)
            start = value
        previous = value
    return max(longest_ms, previous - start) / 3_600_000


def report(passed: bool, description: str, detail: str, failures: list[str], name: str) -> None:
    print(f"[{'PASS' if passed else 'FAIL'}] {description}: {detail}")
    if not passed:
        failures.append(name)


def has_overlay_cycle(log: str) -> bool:
    states = [int(value) for value in OVERLAY_RE.findall(log)]
    return any(states[index] == 0 and 1 in states[index + 1:]
               for index in range(len(states)))


def main() -> int:
    parser = argparse.ArgumentParser(description="检查 EchoEar 第二阶段音乐场景与稳定性指标")
    parser.add_argument("logs", type=Path, nargs="+", help="idf.py monitor 保存的日志")
    parser.add_argument("--structured", action="store_true",
                        help="要求结构化元数据和可用封面均在 3 秒内完成")
    parser.add_argument("--interaction", action="store_true",
                        help="要求日志包含交互覆盖音乐并恢复的完整周期")
    parser.add_argument("--full", action="store_true",
                        help="执行完整门槛；同时启用 --structured 和 --interaction")
    args = parser.parse_args()

    missing = [path for path in args.logs if not path.is_file()]
    if missing:
        parser.error("日志不存在：" + ", ".join(str(path) for path in missing))
    log = "\n".join(clean_log(path.read_text(encoding="utf-8", errors="replace"))
                    for path in args.logs)
    failures: list[str] = []
    print("EchoEar 第二阶段日志验收")

    basic_checks = (
        ("scene_enter", r"MUSIC_METRIC scene enter", "进入音乐场景"),
        ("buffering", r"-> music_buffering source=music", "音乐缓冲状态"),
        ("playing", r"-> music_playing source=music", "音乐播放状态"),
        ("scene_exit", r"MUSIC_METRIC scene exit", "退出并释放音乐场景"),
    )
    for name, pattern, description in basic_checks:
        hits = len(re.findall(pattern, log))
        report(hits > 0, description, str(hits), failures, name)

    crashes = len(re.findall(r"Guru Meditation|assert failed|abort\(\)|watchdog.*triggered", log,
                             re.IGNORECASE))
    report(crashes == 0, "无崩溃/看门狗异常", str(crashes), failures, "crash_free")

    audio = [(int(count), int(duration)) for count, duration in AUDIO_RE.findall(log)]
    underruns = sum(item[0] for item in audio)
    underrun_ms = sum(item[1] for item in audio)
    report(bool(audio) and underruns == 0, "播放期间无音频欠载",
           f"sessions={len(audio)}, underruns={underruns}, wait={underrun_ms}ms",
           failures, "audio_underrun")

    resources = [(int(elapsed), int(internal), int(psram))
                 for elapsed, internal, psram in RESOURCE_RE.findall(log)]
    resource_ok = bool(resources) and all(elapsed <= 5500 and internal >= -8192 and psram >= -8192
                                          for elapsed, internal, psram in resources)
    worst_elapsed = max((item[0] for item in resources), default=0)
    worst_internal = min((item[1] for item in resources), default=0)
    worst_psram = min((item[2] for item in resources), default=0)
    report(resource_ok, "退出 5 秒后内存下降均不超过 8 KiB",
           f"sessions={len(resources)}, elapsed={worst_elapsed}ms, internal={worst_internal}, psram={worst_psram}",
           failures, "resource_release")

    buffer_releases = [(int(size), int(chunks))
                       for size, chunks in BUFFER_RELEASE_RE.findall(log)]
    if buffer_releases:
        released_bytes = sum(item[0] for item in buffer_releases)
        released_chunks = sum(item[1] for item in buffer_releases)
        report(released_bytes > 0 and released_chunks > 0,
               "主动停止释放未播放音频缓冲",
               f"events={len(buffer_releases)}, bytes={released_bytes}, chunks={released_chunks}",
               failures, "audio_buffer_release")
        report(bool(WORKERS_REAPED_RE.search(log)),
               "主动停止回收音乐工作线程",
               f"events={len(WORKERS_REAPED_RE.findall(log))}",
               failures, "music_workers_reaped")

    if args.structured or args.full:
        metadata = [int(value) for value in METADATA_RE.findall(log)]
        artwork = [(int(available), int(elapsed)) for available, elapsed in ARTWORK_RE.findall(log)]
        metadata_ok = bool(metadata) and max(metadata) <= 3000
        artwork_ok = bool(artwork) and all(available == 1 and elapsed <= 3000
                                           for available, elapsed in artwork)
        report(metadata_ok, "元数据在 3 秒内就绪",
               f"samples={metadata or 'none'}", failures, "metadata_latency")
        report(artwork_ok, "可用封面在 3 秒内就绪",
               f"samples={artwork or 'none'}", failures, "artwork_latency")

    if args.interaction or args.full:
        states = OVERLAY_RE.findall(log)
        report(has_overlay_cycle(log), "交互覆盖音乐并在空闲后恢复",
               " -> ".join(states) if states else "none", failures, "overlay_restore")

    if args.full:
        sessions = len(AUDIO_RE.findall(log))
        exits = len(RESOURCE_RE.findall(log))
        report(sessions >= 20, "完整播放/停止次数", f"{sessions}/20",
               failures, "music_sessions")
        report(exits >= 20, "音乐场景释放次数", f"{exits}/20",
               failures, "scene_exits")
        hours = elapsed_hours(log)
        report(hours >= 8.0, "最长连续运行时间", f"{hours:.2f}/8.00 小时",
               failures, "eight_hour_soak")

    print("\n" + ("未通过：" + ", ".join(failures) if failures else "全部自动检查通过。"))
    if not failures:
        print("歌词与声音 ±700 ms、旋转观感仍需按验收文档人工确认。")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
