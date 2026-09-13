#!/usr/bin/env python3
"""Bounded EchoEar debug test runner (requires pyserial and opt-in firmware)."""
import argparse
import json
import re
import time
from pathlib import Path

import serial

SCENE = re.compile(r"scene=(\d+) frames=(\d+) elapsed_ms=(\d+) avg_us=(\d+) max_us=(\d+) over_budget=(\d+) stack_free=(\d+)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--rounds", type=int, default=10, choices=range(1, 21))
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    # Exclusive creation: never overwrite an earlier measurement.
    report = args.output.open("x", encoding="utf-8")
    conn = serial.Serial(args.port, 115200, timeout=0.2, dsrdtr=True, rtscts=True)
    deadline = time.monotonic() + args.rounds * 35 + 150
    results = []

    def record(value):
        report.write(json.dumps(value, ensure_ascii=False) + "\n")
        report.flush()

    def send(command):
        conn.write(("character-test " + command + "\n").encode())
        conn.flush()

    def read():
        if time.monotonic() > deadline:
            raise TimeoutError("Suite deadline exceeded")
        line = conn.readline().decode("utf-8", "replace").strip()
        if any(x in line for x in ("Guru Meditation", "watchdog", "Stack canary", "Brownout")):
            raise RuntimeError("Device fault: " + line)
        if "Character " in line:
            record({"log": line})
            print(line, flush=True)
        return line

    def wait_idle():
        end = time.monotonic() + 5
        send("status")
        while time.monotonic() < end:
            line = read()
            if "Character serial status running=0" in line:
                return
            if "Character serial status running=1" in line:
                time.sleep(0.1)
                send("status")
        raise TimeoutError("No idle acknowledgement; verify debug firmware and serial port")

    def run(label, interrupt=None):
        wait_idle()
        send("start")
        end = time.monotonic() + 30
        started = None
        cancelled_sent = False
        scenes = []
        before = after = None
        cancelled = None
        cache = False
        while time.monotonic() < end:
            if interrupt is not None and started is not None and not cancelled_sent and time.monotonic()-started >= interrupt:
                send("cancel")
                cancelled_sent = True
            line = read()
            if "start accepted=0" in line:
                raise RuntimeError("Device rejected start")
            if "guitar_cache=1" in line:
                cache = True
            if "Character preview begin" in line:
                started = time.monotonic()
                before = int(re.search(r"spiram_before=(\d+)", line)[1])
            match = SCENE.search(line)
            if match:
                values = list(map(int, match.groups()))
                scenes.append(dict(zip(("scene", "frames", "elapsed_ms", "avg_us", "max_us", "over_budget", "stack_free"), values)))
            if "Character preview end" in line:
                cancelled = int(re.search(r"cancelled=(\d+)", line)[1])
                after = int(re.search(r"spiram_after=(\d+)", line)[1])
            if "Character preview result" in line:
                completed, failed = map(int, re.search(r"completed=(\d+) failed=(\d+)", line).groups())
                expected = 3 if interrupt is None else int(interrupt // 6)
                if failed or completed != expected or cancelled != int(interrupt is not None) or not cache:
                    raise RuntimeError(f"Unexpected completion: {label}: {line}, cancelled={cancelled}, cache={cache}")
                if [s["scene"] for s in scenes] != list(range(3 if interrupt is None else expected+1)):
                    raise RuntimeError("Missing or reordered scene metrics")
                result = {"test": label, "completed": completed, "cancelled": cancelled,
                          "spiram_before": before, "spiram_after": after, "scenes": scenes}
                record(result)
                results.append(result)
                wait_idle()
                time.sleep(1)
                return
        raise TimeoutError("Round deadline exceeded: " + label)

    try:
        # Synchronize without resetting the device or consuming unrelated data.
        conn.reset_input_buffer()
        for index in range(args.rounds):
            run(f"full-{index+1}")
        for scene in range(3):
            run(f"software-interrupt-{scene}", scene * 6 + 1)
        run("recovery-full")
        record({"suite": "PASS", "tests": len(results)})
        print(f"PASS: {len(results)} tests. Report: {args.output}", flush=True)
    except BaseException as exc:
        send("cancel")
        record({"suite": "FAIL", "error": str(exc), "tests_completed": len(results)})
        raise
    finally:
        conn.close()
        report.close()


if __name__ == "__main__":
    main()
