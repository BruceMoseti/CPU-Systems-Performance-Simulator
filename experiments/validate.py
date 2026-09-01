#!/usr/bin/env python3
"""Compares the model against the machine it is running on.

A simulator that has never been checked against hardware is just a story about
performance. This script does three things:

  1. Reads the host's real cache geometry from sysfs.
  2. Measures the host's real access latency at each level of its hierarchy by
     timing a dependent pointer chase over growing working sets. This needs no
     performance counters, which matters because most virtual machines do not
     expose a PMU.
  3. Configures the model to match the host and compares predicted runtime
     against measured runtime for every workload.

    python3 experiments/validate.py
    python3 experiments/validate.py --repeats 3
"""

from __future__ import annotations

import argparse
import json
import platform
import re
import subprocess
import sys
from pathlib import Path

import perfsim

SYSFS_CACHE = Path("/sys/devices/system/cpu/cpu0/cache")

# Working sets for the latency sweep, in bytes. They straddle the level
# boundaries of any plausible host so the steps in the curve are visible.
LATENCY_WORKING_SETS = [
    4 << 10, 16 << 10, 32 << 10, 64 << 10, 128 << 10, 256 << 10,
    512 << 10, 1 << 20, 2 << 20, 4 << 20, 8 << 20, 16 << 20, 32 << 20, 64 << 20,
]
TARGET_HOPS = 8_000_000


def read_host_caches() -> list[dict]:
    caches = []
    if not SYSFS_CACHE.exists():
        return caches
    for index in sorted(SYSFS_CACHE.glob("index*")):
        def field(name: str) -> str:
            path = index / name
            return path.read_text().strip() if path.exists() else ""

        size = field("size")
        match = re.match(r"(\d+)([KM])?", size)
        if not match:
            continue
        kb = int(match.group(1)) * (1024 if match.group(2) == "M" else 1)
        caches.append(
            {
                "level": int(field("level") or 0),
                "type": field("type"),
                "size_kb": kb,
                "ways": int(field("ways_of_associativity") or 0),
                "line_size": int(field("coherency_line_size") or 64),
            }
        )
    return caches


def read_host_frequency_ghz() -> float | None:
    try:
        text = Path("/proc/cpuinfo").read_text()
    except OSError:
        return None
    match = re.search(r"cpu MHz\s*:\s*([\d.]+)", text)
    return float(match.group(1)) / 1000.0 if match else None


def host_model_name() -> str:
    try:
        text = Path("/proc/cpuinfo").read_text()
    except OSError:
        return platform.processor() or "unknown"
    match = re.search(r"model name\s*:\s*(.+)", text)
    return match.group(1).strip() if match else platform.processor() or "unknown"


def run_native(workload: str, params: dict, repeats: int) -> float:
    """Runs a native kernel and returns the best kernel-only time in seconds."""
    command = [str(perfsim.TRACEGEN), workload, "--mode", "native"]
    for key, value in params.items():
        command += [f"--{key}", str(value)]
    best = float("inf")
    for _ in range(repeats):
        completed = subprocess.run(command, check=True, capture_output=True, text=True)
        best = min(best, json.loads(completed.stdout)["seconds"])
    return best


def measure_latency_curve(frequency_ghz: float, repeats: int) -> list[dict]:
    """Times a dependent pointer chase over growing working sets.

    Each hop cannot start until the previous load returns, so seconds/hops is the
    host's real access latency for that working set.
    """
    curve = []
    for working_set in LATENCY_WORKING_SETS:
        nodes = working_set // 8
        iterations = max(1, TARGET_HOPS // nodes)
        hops = nodes * iterations
        seconds = run_native("pointer_chase", {"n": nodes, "iterations": iterations}, repeats)
        ns_per_hop = seconds / hops * 1e9
        curve.append(
            {
                "working_set_kb": working_set // 1024,
                "hops": hops,
                "ns_per_hop": ns_per_hop,
                "cycles_per_hop": ns_per_hop * frequency_ghz,
            }
        )
        print(
            f"  {working_set // 1024:>7} KB   {ns_per_hop:>7.2f} ns/hop"
            f"   {ns_per_hop * frequency_ghz:>7.1f} cycles"
        )
    return curve


def latency_at(curve: list[dict], working_set_kb: int) -> float:
    """Measured cycles per hop at the working set closest to the requested size."""
    closest = min(curve, key=lambda point: abs(point["working_set_kb"] - working_set_kb))
    return closest["cycles_per_hop"]


def build_host_config(caches: list[dict], curve: list[dict], frequency_ghz: float) -> dict:
    """Builds a model of the host from its geometry and measured latencies."""
    l1d = next((c for c in caches if c["level"] == 1 and c["type"] == "Data"), None)
    l2 = next((c for c in caches if c["level"] == 2), None)

    config = perfsim.load_config("baseline")
    config["cpu"]["frequency_ghz"] = round(frequency_ghz, 3)
    if l1d:
        config["l1"] = {
            "size_kb": l1d["size_kb"],
            "line_size": l1d["line_size"],
            "associativity": l1d["ways"],
            # A chase inside L1 measures the load latency plus the address
            # arithmetic between hops, so round to the nearest cycle and floor at 1.
            "latency_cycles": max(1, round(latency_at(curve, l1d["size_kb"] // 2))),
        }
    if l2:
        config["l2"] = {
            "size_kb": l2["size_kb"],
            "line_size": l2["line_size"],
            "associativity": l2["ways"],
            "latency_cycles": max(2, round(latency_at(curve, l2["size_kb"] // 2))),
        }
    # The model has two cache levels, so its "DRAM" stands for whatever serves
    # the workloads' 8 MB working set on this host. On a chip with a large shared
    # L3 that is the L3, not DRAM.
    config["memory"]["latency_cycles"] = max(3, round(latency_at(curve, 8192)))
    return config


def compare_workloads(config: dict, repeats: int) -> list[dict]:
    rows = []
    for name, spec in perfsim.WORKLOADS.items():
        params = {k: v for k, v in spec.items() if k != "workload"}
        native_seconds = run_native(spec["workload"], params, repeats)
        result = perfsim.simulate(name, config)
        rows.append(
            {
                "workload": name,
                "native_ms": native_seconds * 1e3,
                "model_ms": result["seconds"] * 1e3,
                "ratio": result["seconds"] / native_seconds if native_seconds else 0.0,
                "model_ipc": result["ipc"],
                "model_l1_hit_rate": result["l1"]["hit_rate"],
                "bottleneck": result["bottleneck"]["name"],
            }
        )
    return rows


def check_counters() -> dict:
    """Reports whether this host exposes hardware counters, via perfcount."""
    if not perfsim.PERFCOUNT.exists():
        return {"available": False, "reason": "perfcount was not built"}
    completed = subprocess.run(
        [str(perfsim.PERFCOUNT), "--", "true"], capture_output=True, text=True
    )
    try:
        payload = json.loads(completed.stdout)
    except json.JSONDecodeError:
        return {"available": False, "reason": "perfcount produced no JSON"}
    if payload.get("hardware_counters_available"):
        return {"available": True, "counters": sorted(payload.get("counters", {}))}
    unavailable = payload.get("unavailable", {})
    reason = unavailable.get("cycles", "hardware events could not be opened")
    return {"available": False, "reason": f"perf_event_open: {reason}"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repeats", type=int, default=2,
                        help="native timing repeats; the fastest run is used")
    parser.add_argument("--frequency-ghz", type=float, default=None,
                        help="override the host clock frequency")
    args = parser.parse_args()

    try:
        perfsim.ensure_built()
    except perfsim.BuildMissing as error:
        print(error, file=sys.stderr)
        return 1

    frequency = args.frequency_ghz or read_host_frequency_ghz() or 3.0
    caches = read_host_caches()
    counters = check_counters()

    print("Model against hardware")
    print("======================\n")
    print(f"Host CPU:   {host_model_name()}")
    print(f"Frequency:  {frequency:.2f} GHz (from /proc/cpuinfo unless overridden)")
    for cache in caches:
        print(
            f"  L{cache['level']} {cache['type']:<12} {cache['size_kb']:>7} KB"
            f"  {cache['ways']:>3}-way  {cache['line_size']} B lines"
        )
    if counters["available"]:
        print(f"Counters:   available ({', '.join(counters['counters'])})")
    else:
        print(f"Counters:   unavailable - {counters['reason']}")
        print("            Falling back to latency and runtime measurements, which")
        print("            need no PMU.")

    print("\nMeasured access latency (dependent pointer chase)")
    print("-------------------------------------------------")
    curve = measure_latency_curve(frequency, args.repeats)

    config = build_host_config(caches, curve, frequency)
    print("\nModel configured to match the host")
    print("----------------------------------")
    print(f"  L1:   {config['l1']['size_kb']} KB, {config['l1']['associativity']}-way, "
          f"{config['l1']['latency_cycles']} cycles")
    print(f"  L2:   {config['l2']['size_kb']} KB, {config['l2']['associativity']}-way, "
          f"{config['l2']['latency_cycles']} cycles")
    print(f"  DRAM: {config['memory']['latency_cycles']} cycles "
          f"(measured at an 8 MB working set)")

    print("\nPredicted against measured runtime")
    print("----------------------------------")
    rows = compare_workloads(config, args.repeats)
    print(f"  {'workload':<16}{'native (ms)':>13}{'model (ms)':>12}{'model/native':>14}"
          f"   model bottleneck")
    for entry in rows:
        print(
            f"  {entry['workload']:<16}{entry['native_ms']:>13.1f}{entry['model_ms']:>12.1f}"
            f"{entry['ratio']:>13.2f}x   {entry['bottleneck']}"
        )

    print("\nRelative comparisons (absolute modelling error cancels out)")
    print("-----------------------------------------------------------")
    by_name = {entry["workload"]: entry for entry in rows}

    def ratio_pair(slow: str, fast: str, label: str) -> dict | None:
        if slow not in by_name or fast not in by_name:
            return None
        native = by_name[slow]["native_ms"] / by_name[fast]["native_ms"]
        model = by_name[slow]["model_ms"] / by_name[fast]["model_ms"]
        print(f"  {label:<34} native {native:>6.2f}x   model {model:>6.2f}x")
        return {"comparison": label, "native": native, "model": model}

    comparisons = [
        ratio_pair("matrix_naive", "matrix_blocked", "naive / blocked matrix multiply"),
        ratio_pair("pointer_chase", "random_access", "pointer chase / random access"),
        ratio_pair("random_access", "sequential", "random access / sequential"),
    ]

    perfsim.RESULTS.mkdir(parents=True, exist_ok=True)
    payload = {
        "host": {
            "model_name": host_model_name(),
            "frequency_ghz": frequency,
            "caches": caches,
            "hardware_counters": counters,
        },
        "latency_curve": curve,
        "host_config": config,
        "workloads": rows,
        "comparisons": [c for c in comparisons if c],
    }
    (perfsim.RESULTS / "validation.json").write_text(json.dumps(payload, indent=2) + "\n")

    import pandas as pd

    pd.DataFrame(curve).to_csv(perfsim.RESULTS / "latency_curve.csv", index=False)
    print("\nwrote results/validation.json and results/latency_curve.csv")
    return 0


if __name__ == "__main__":
    sys.exit(main())
