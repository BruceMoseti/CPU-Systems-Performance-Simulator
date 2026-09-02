#!/usr/bin/env python3
"""Measures how fast the simulator itself runs.

There are two performance levels in this project: the performance of the
simulated machine, and the performance of the simulator. This measures the
second one, which is what the optimisation work in the README is judged against.

    python3 experiments/benchmark.py
    python3 experiments/benchmark.py --accesses 20000000 --label after
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys

import perfsim

PATTERNS = ["sequential", "random", "pointer_chase"]


def run_engine(pattern: str, accesses: int, repeats: int) -> dict:
    """Benchmark mode feeds records straight in, so no trace parsing is measured."""
    best = None
    for _ in range(repeats):
        completed = subprocess.run(
            [str(perfsim.PERFSIM), "--benchmark", "--pattern", pattern,
             "--accesses", str(accesses), "--config", str(perfsim.CONFIGS / "baseline.json"),
             "--quiet", "--json", "-"],
            check=True, capture_output=True, text=True,
        )
        payload = json.loads(completed.stdout)["simulator"]
        if best is None or payload["records_per_second"] > best["records_per_second"]:
            best = payload
    return best


def run_replay(workload: str, repeats: int) -> dict:
    """Replaying a trace from disk, which is how the simulator is actually used."""
    trace = perfsim.ensure_trace(workload)
    best = None
    for _ in range(repeats):
        completed = subprocess.run(
            [str(perfsim.PERFSIM), "--config", str(perfsim.CONFIGS / "baseline.json"),
             "--trace", str(trace), "--quiet", "--json", "-"],
            check=True, capture_output=True, text=True,
        )
        payload = json.loads(completed.stdout)["simulator"]
        if best is None or payload["records_per_second"] > best["records_per_second"]:
            best = payload
    return best


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--accesses", type=int, default=10_000_000)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--label", default="current", help="name for this measurement")
    args = parser.parse_args()

    try:
        perfsim.ensure_built()
    except perfsim.BuildMissing as error:
        print(error, file=sys.stderr)
        return 1

    print(f"Simulator throughput ({args.label}, best of {args.repeats})")
    print("=" * 60)
    print("\nEngine only (records fed directly to the model)")
    print(f"  {'pattern':<18}{'M records/s':>14}{'seconds':>10}")
    engine = {}
    for pattern in PATTERNS:
        payload = run_engine(pattern, args.accesses, args.repeats)
        engine[pattern] = payload
        print(f"  {pattern:<18}{payload['records_per_second'] / 1e6:>14.2f}"
              f"{payload['wall_seconds']:>10.2f}")

    print("\nReplaying traces from disk (includes parsing)")
    print(f"  {'workload':<18}{'M records/s':>14}{'seconds':>10}{'records':>14}{'peak MB':>10}")
    replay = {}
    for workload in perfsim.WORKLOADS:
        payload = run_replay(workload, args.repeats)
        replay[workload] = payload
        print(f"  {workload:<18}{payload['records_per_second'] / 1e6:>14.2f}"
              f"{payload['wall_seconds']:>10.2f}{int(payload['records']):>14,}"
              f"{payload.get('peak_memory_bytes', 0) / 1e6:>10.1f}")

    total_records = sum(p["records"] for p in replay.values())
    total_seconds = sum(p["wall_seconds"] for p in replay.values())
    aggregate = total_records / total_seconds
    print(f"\n  {'all traces':<18}{aggregate / 1e6:>14.2f}{total_seconds:>10.2f}"
          f"{int(total_records):>14,}")

    perfsim.RESULTS.mkdir(parents=True, exist_ok=True)
    path = perfsim.RESULTS / f"benchmark_{args.label}.json"
    path.write_text(json.dumps(
        {"label": args.label, "accesses": args.accesses, "engine": engine, "replay": replay,
         "aggregate_records_per_second": aggregate}, indent=2) + "\n")
    print(f"\nwrote {path.relative_to(perfsim.ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
