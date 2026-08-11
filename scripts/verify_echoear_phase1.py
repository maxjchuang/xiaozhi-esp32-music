#!/usr/bin/env python3
"""Validate EchoEar phase-one behavior from an ESP-IDF serial log."""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path


ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
TIME_RE = re.compile(r"\b[IEWD] \((\d+)\)")
SRAM_RE = re.compile(r"SystemInfo: free sram: (\d+)")
EXPRESSION_RE = re.compile(r"Expression: .* -> ([a-z_]+)")


@dataclass(frozen=True)
class Check:
    name: str
    pattern: str
    description: str


CHECKS = (
    Check("idle", r"Application: STATE: idle", "设备进入待机"),
    Check("wake", r"Expression: .* -> wake_acknowledged", "唤醒确认表情"),
    Check("listening", r"Expression: .* -> listening", "聆听表情"),
    Check("thinking", r"Expression: .* -> thinking", "思考表情"),
    Check("speaking", r"Expression: .* -> speaking", "说话表情"),
    Check("tool_running", r"(?:-> tool_running source=mcp|source=mcp_overlay .*正在(?:处理|找歌|连接))",
          "MCP 处理中反馈"),
    Check("tool_result", r"(?:-> (?:success|recoverable_error) source=mcp|source=mcp_overlay .*?(?:完成了|处理失败))",
          "MCP 结果与恢复"),
    Check("music_buffering", r"-> music_buffering source=music", "音乐缓冲状态"),
    Check("music_playing", r"-> music_playing source=music", "音乐播放状态"),
    Check("network_connecting", r"-> connecting source=network", "网络连接中状态"),
    Check("network_result", r"-> (success|recoverable_error) source=network", "网络成功或失败状态"),
    Check("idle_blink", r"source=idle_blink", "普通眨眼"),
    Check("idle_slow_blink", r"source=idle_slow_blink", "慢眨眼"),
    Check("idle_double_blink", r"source=idle_double_blink", "快速双眨眼"),
    Check("idle_observe", r"source=idle_observe", "观察动作"),
    Check("idle_curious", r"source=idle_curious", "好奇动作"),
    Check("idle_sleep", r"source=timer_sleep", "五分钟困倦"),
)


def clean_log(raw: str) -> str:
    return ANSI_RE.sub("", raw).replace("\r", "")


def elapsed_hours(log: str) -> float:
    times = [int(value) for value in TIME_RE.findall(log)]
    if len(times) < 2:
        return 0.0
    # ESP-IDF timestamps restart at zero after reboot. Use the longest observed
    # monotonic segment rather than incorrectly spanning resets.
    longest = current_start = previous = times[0]
    longest_ms = 0
    for value in times[1:]:
        if value < previous:
            longest_ms = max(longest_ms, previous - current_start)
            current_start = value
        previous = value
    longest_ms = max(longest_ms, previous - current_start)
    return longest_ms / 3_600_000


def count(log: str, pattern: str) -> int:
    return len(re.findall(pattern, log))


def idle_sram_samples(log: str) -> list[int]:
    """Return SRAM samples taken while the rendered behavior is idle.

    Comparing global endpoints gives false failures when a log ends during a
    conversation or while the music decoder owns its playback buffer.
    """
    behavior = "unknown"
    samples: list[int] = []
    for line in log.splitlines():
        transition = EXPRESSION_RE.search(line)
        if transition:
            behavior = transition.group(1)
        sram = SRAM_RE.search(line)
        if sram and behavior == "idle":
            samples.append(int(sram.group(1)))
    return samples


def main() -> int:
    parser = argparse.ArgumentParser(
        description="检查 EchoEar 第一阶段串口日志中的状态覆盖和稳定性指标")
    parser.add_argument("logs", type=Path, nargs="+",
                        help="一个或多个 idf.py monitor 保存的日志文件")
    parser.add_argument("--full", action="store_true",
                        help="执行完整验收门槛：50 轮对话、20 次音乐、20 次网络恢复、8 小时")
    args = parser.parse_args()

    missing = [path for path in args.logs if not path.is_file()]
    if missing:
        parser.error("日志不存在：" + ", ".join(str(path) for path in missing))
    log = "\n".join(clean_log(path.read_text(encoding="utf-8", errors="replace"))
                    for path in args.logs)

    failures: list[str] = []
    print("EchoEar 第一阶段日志验收")
    for check in CHECKS:
        hits = count(log, check.pattern)
        passed = hits > 0
        print(f"[{'PASS' if passed else 'FAIL'}] {check.description}: {hits}")
        if not passed:
            failures.append(check.name)

    crashes = count(log, r"Guru Meditation|assert failed|abort\(\)|watchdog.*triggered")
    print(f"[{'PASS' if crashes == 0 else 'FAIL'}] 无崩溃/看门狗异常: {crashes}")
    if crashes:
        failures.append("crash_free")

    sram = idle_sram_samples(log)
    if len(sram) >= 2:
        drop = sram[0] - sram[-1]
        stable = drop <= 8192
        print(f"[{'PASS' if stable else 'FAIL'}] 待机 SRAM 首尾下降不超过 8 KiB: {drop} bytes")
        if not stable:
            failures.append("sram_stability")
    else:
        print("[FAIL] 待机 SRAM 样本不足: 需要至少 2 条待机 SystemInfo 日志")
        failures.append("sram_samples")

    if args.full:
        full_metrics = (
            ("dialog_cycles", r"Application: Wake word detected:", 50, "对话轮数"),
            ("music_cycles", r"-> music_playing source=music", 20, "音乐播放次数"),
            ("network_cycles", r"-> success source=network", 20,
             "网络恢复次数"),
        )
        for name, pattern, required, description in full_metrics:
            hits = count(log, pattern)
            passed = hits >= required
            print(f"[{'PASS' if passed else 'FAIL'}] {description}: {hits}/{required}")
            if not passed:
                failures.append(name)
        hours = elapsed_hours(log)
        passed = hours >= 8.0
        print(f"[{'PASS' if passed else 'FAIL'}] 最长连续运行时间: {hours:.2f}/8.00 小时")
        if not passed:
            failures.append("eight_hour_soak")

    if failures:
        print("\n未通过：" + ", ".join(failures))
        return 1
    print("\n全部检查通过。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
