#!/usr/bin/env python3
"""Diagnoses a workload on a given machine and measures what would fix it.

The simulator reports which resource a workload is waiting on. This script goes
one step further and actually runs the candidate architecture changes, so the
recommendation is measured rather than inferred from a heuristic.

    python3 experiments/analyze.py --workload pointer_chase --preset baseline
    python3 experiments/analyze.py --workload all --preset baseline
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

import perfsim

# Each candidate is one plausible, single-knob change to the machine. Values are
# multipliers on the current configuration so the same list applies to any preset.
CANDIDATES = [
    ("l1.size_kb", 2),
    ("l1.size_kb", 4),
    ("l1.associativity", 2),
    ("l1.latency_cycles", 0.5),
    ("l2.size_kb", 2),
    ("l2.size_kb", 4),
    ("l2.size_kb", 8),
    ("l2.associativity", 2),
    ("memory.latency_cycles", 0.5),
    ("memory.bandwidth_gbps", 2),
    ("cpu.mshrs", 2),
    ("cpu.mshrs", 4),
    ("cpu.issue_width", 2),
]

# Line size has to move at both levels at once: a hierarchy whose levels disagree
# about line size is not something the model handles.
LINE_SIZE_CANDIDATES = [2, 0.5]


def scale(value, factor):
    scaled = value * factor
    return int(scaled) if float(scaled).is_integer() else scaled


def build_candidates(config: dict) -> list[tuple[str, dict]]:
    """Returns (label, overrides) pairs for every candidate the preset supports."""
    candidates = []
    for dotted, factor in CANDIDATES:
        section, _, field = dotted.partition(".")
        if section not in config or field not in config[section]:
            continue  # e.g. an L1-only preset has no l2 section
        old = config[section][field]
        new = scale(old, factor)
        if new == old or new <= 0:
            continue
        candidates.append((f"{dotted}: {old} -> {new}", {dotted: new}))

    if "l2" in config:
        for factor in LINE_SIZE_CANDIDATES:
            old = config["l1"]["line_size"]
            new = scale(old, factor)
            if new < 4 or new == old:
                continue
            candidates.append(
                (
                    f"line_size: {old} -> {new}",
                    {"l1.line_size": new, "l2.line_size": new},
                )
            )
    return candidates


def measure(workload: str, config: dict, overrides: dict) -> dict | None:
    """Runs one candidate, returning None when the machine is not representable."""
    try:
        return perfsim.simulate(workload, perfsim.with_overrides(config, overrides))
    except subprocess.CalledProcessError as error:
        message = (error.stderr or "").strip().splitlines()
        return {"error": message[-1] if message else "simulation failed"}


def analyse(workload: str, preset: str, jobs: int) -> dict:
    config = perfsim.load_config(preset)
    trace = perfsim.ensure_trace(workload)

    # Show the simulator's own report so the diagnosis and the numbers behind it
    # stay in one place.
    completed = subprocess.run(
        [str(perfsim.PERFSIM), "--config", str(perfsim.CONFIGS / f"{preset}.json"),
         "--trace", str(trace)],
        check=True,
        capture_output=True,
        text=True,
    )
    print(completed.stdout.rstrip())

    baseline = perfsim.simulate(workload, config)
    candidates = build_candidates(config)

    with ThreadPoolExecutor(max_workers=jobs) as pool:
        measured = list(
            pool.map(lambda item: measure(workload, config, item[1]), candidates)
        )

    rows = []
    for (label, overrides), result in zip(candidates, measured):
        if "error" in result:
            rows.append({"label": label, "error": result["error"]})
            continue
        rows.append(
            {
                "label": label,
                "overrides": overrides,
                "ipc": result["ipc"],
                "delta": result["ipc"] / baseline["ipc"] - 1.0 if baseline["ipc"] else 0.0,
                "bottleneck": result["bottleneck"]["name"],
                "seconds": result["seconds"],
            }
        )

    usable = [r for r in rows if "error" not in r]
    usable.sort(key=lambda r: r["ipc"], reverse=True)

    print("\nCandidate modifications (each one simulated, not estimated)")
    print("----------------------------------------------------------")
    print(f"  {'change':<34}{'IPC':>8}{'delta':>10}   resulting bottleneck")
    print(f"  {'(unchanged)':<34}{baseline['ipc']:>8.3f}{'-':>10}   {baseline['bottleneck']['name']}")
    for entry in usable:
        print(
            f"  {entry['label']:<34}{entry['ipc']:>8.3f}{entry['delta'] * 100:>9.1f}%"
            f"   {entry['bottleneck']}"
        )
    for entry in (r for r in rows if "error" in r):
        print(f"  {entry['label']:<34}{'n/a':>8}{'':>10}   {entry['error']}")

    best = usable[0] if usable else None
    print("\nBest tested modification")
    print("------------------------")
    if best is None or best["delta"] < 0.01:
        print("  No single change tested improves performance by more than 1%.")
        print("  This machine is balanced for this workload; the workload itself")
        print("  would have to change (see the blocking experiment).")
    else:
        print(f"  {best['label']}")
        print(
            f"  IPC {baseline['ipc']:.3f} -> {best['ipc']:.3f} ({best['delta'] * 100:+.1f}%), "
            f"runtime {baseline['seconds'] * 1e3:.2f} ms -> {best['seconds'] * 1e3:.2f} ms"
        )
        if best["bottleneck"] != baseline["bottleneck"]["name"]:
            print(
                f"  The bottleneck moves from {baseline['bottleneck']['name']} "
                f"to {best['bottleneck']}."
            )

    return {
        "workload": workload,
        "preset": preset,
        "baseline": {
            "ipc": baseline["ipc"],
            "bottleneck": baseline["bottleneck"]["name"],
            "seconds": baseline["seconds"],
        },
        "candidates": [
            {k: v for k, v in row.items() if k != "overrides"} for row in rows
        ],
        "best": {k: v for k, v in best.items() if k != "overrides"} if best else None,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workload", default="pointer_chase",
                        help="workload name, or 'all' (see perfsim.WORKLOADS)")
    parser.add_argument("--preset", default="baseline", help="config preset name")
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--json", dest="json_path", help="also write the analysis as JSON")
    args = parser.parse_args()

    try:
        perfsim.ensure_built()
    except perfsim.BuildMissing as error:
        print(error, file=sys.stderr)
        return 1

    workloads = sorted(perfsim.WORKLOADS) if args.workload == "all" else [args.workload]
    analyses = []
    for index, workload in enumerate(workloads):
        if index:
            print("\n" + "=" * 78 + "\n")
        analyses.append(analyse(workload, args.preset, args.jobs))

    if args.json_path:
        perfsim.RESULTS.mkdir(parents=True, exist_ok=True)
        with open(args.json_path, "w") as handle:
            json.dump(analyses if len(analyses) > 1 else analyses[0], handle, indent=2)
        print(f"\nwrote {args.json_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
