#!/usr/bin/env python3
"""Generate named seeds and run every device-free libFuzzer harness."""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

TARGETS = ("config", "lifecycle", "queue", "faults")


def seeds(target):
    if target == "config":
        result = {"valid": bytes(5), "context-oom": bytes(4) + b"\x01",
                  "stream-oom": bytes(4) + b"\x02",
                  "stereo-f32": struct.pack("<IB", 65536, 0),
                  "maximum-quantum": struct.pack("<IB", 65536 | 131072, 0)}
        for mask in (1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 262144):
            for value in (0, 1, 2, 32, 8192, 8193, 44100, 192000, 0xffffffff):
                result[f"field-{mask}-{value}"] = struct.pack("<IBI", mask, 0, value)
        return result
    if target == "lifecycle":
        def sequence(*ops):
            return bytes([31] + [v for op in ops for v in op])
        return {
            "duplex-flush-stop-restart-close": sequence((0, 0), (2, 0), (5, 32), (7, 31), (3, 7),
                (4, 7), (6, 1), (5, 128), (2, 0), (7, 31), (1, 0), (2, 0),
                (0, 0), (2, 0), (8, 0), (9, 0), (2, 0), (9, 0)),
            "delays": sequence((0, 63), *[(2, 0)] * 5, (6, 0), *[(2, 0)] * 5,
                (1, 0), *[(2, 0)] * 5),
            "hold-release": sequence((0, 0), (2, 0), (10, 8), (8, 0), (2, 0),
                (9, 0), (10, 0), (2, 0), (9, 0)),
            "regression-clock-config": bytes.fromhex("063030ce41df2a"),
        }
    if target == "queue":
        # Header: slots, quantum, bytes/frame, initial cursor distance to wrap.
        push = bytes([1, 96]) + struct.pack("<II", 100, 200) + bytes([2, 3]) + struct.pack("<I", 4000) + bytes([17])
        read = bytes([2, 7])
        return {"wrap-partial-full-reset": bytes([3, 31, 2, 0]) + (push + read * 7) * 6 + bytes([0, 0]),
                "timestamp-overflow": bytes([1, 63, 0, 1]) + bytes([3, 64]) + bytes(10) + bytes([0, 11]) + read * 12}
    return {f"phase-{phase}-mode-{mode}-schedule-{schedule}": bytes([31, mode, schedule, phase, 3])
            for phase in range(6) for mode in range(2) for schedule in range(8)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build-fuzz"))
    parser.add_argument("--output-dir", type=Path, default=Path("build-fuzz/results"))
    parser.add_argument("--runs", type=int, default=30000)
    parser.add_argument("--seed", type=int, default=73)
    parser.add_argument("--timeout", type=int, default=180, help="wall-clock limit per target, seconds")
    args = parser.parse_args()
    if args.runs < 1 or args.timeout < 1:
        parser.error("runs and timeout must be positive")
    results = []
    for target in TARGETS:
        directory = args.output_dir.resolve() / target
        corpus = directory / "corpus"
        artifacts = directory / "artifacts"
        corpus.mkdir(parents=True, exist_ok=True)
        artifacts.mkdir(exist_ok=True)
        for name, content in seeds(target).items():
            (corpus / name).write_bytes(content)
        binary = args.build_dir.resolve() / f"hail_fuzz_{target}"
        command = [str(binary), str(corpus), f"-runs={args.runs}", f"-seed={args.seed}",
                   "-max_len=1024", "-timeout=10", "-rss_limit_mb=1024", "-print_final_stats=1",
                   f"-artifact_prefix={artifacts}{os.sep}"]
        env = dict(os.environ, UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
        log_path = directory / "fuzzer.log"
        try:
            with log_path.open("w") as log:
                completed = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                           env=env, timeout=args.timeout, check=False)
                code = completed.returncode
        except subprocess.TimeoutExpired:
            code = "timeout"
        except OSError as error:
            code = str(error)
        result = {"target": target, "command": command, "returncode": code,
                  "log": str(log_path), "passed": code == 0}
        results.append(result)
        print(f"{target}: {'PASS' if result['passed'] else 'FAIL'} ({log_path})", flush=True)
    (args.output_dir / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
    return 0 if all(result["passed"] for result in results) else 1


if __name__ == "__main__":
    sys.exit(main())
