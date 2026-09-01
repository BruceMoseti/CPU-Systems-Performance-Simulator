#!/usr/bin/env python3
"""Sweeps architecture parameters and records the results.

Each sweep answers one question, and every row of the output is one simulated
machine running one workload.

    python3 experiments/run_experiments.py --list
    python3 experiments/run_experiments.py --all
    python3 experiments/run_experiments.py --sweep l2_capacity --sweep mlp
"""

from __future__ import annotations

import argparse
import math
import os
import sys
from collections.abc import Callable
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field

import perfsim

ARRAY_WORKLOADS = ["sequential", "random_access", "pointer_chase", "strided"]
MATRIX_WORKLOADS = ["matrix_naive", "matrix_blocked"]
ALL_WORKLOADS = ARRAY_WORKLOADS + MATRIX_WORKLOADS


def cache_latency_for(size_kb: int) -> int:
    """Rough access latency for a cache of a given size.

    Real caches get slower as they get bigger, roughly with the square root of
    capacity. Anchoring at the baseline's 32 KB / 4 cycle L1 gives 8 cycles at
    128 KB and 16 at 512 KB, which is the right order of magnitude and enough to
    show that capacity is not free.
    """
    return max(2, round(4 * math.sqrt(size_kb / 32)))


@dataclass
class Sweep:
    question: str
    knobs: list[str]  # dotted config keys, all set to the sweep value
    values: list
    workloads: list[str]
    preset: str = "baseline"
    fixed: dict = field(default_factory=dict)
    # Extra overrides derived from the sweep value, for knobs that cannot move
    # independently (a bigger cache is also a slower cache).
    derive: Callable[[int], dict] | None = None


SWEEPS = {
    "l1_capacity": Sweep(
        question="How much does L1 capacity matter, and where does it stop helping?",
        knobs=["l1.size_kb"],
        values=[8, 16, 32, 64, 128, 256, 512, 1024],
        workloads=ALL_WORKLOADS,
    ),
    "l1_capacity_latency": Sweep(
        question="With latency scaled to capacity, is a bigger L1 still worth it?",
        knobs=["l1.size_kb"],
        values=[8, 16, 32, 64, 128, 256, 512, 1024],
        workloads=ALL_WORKLOADS,
        derive=lambda size_kb: {"l1.latency_cycles": cache_latency_for(size_kb)},
    ),
    "l2_capacity": Sweep(
        question="When does additional L2 capacity stop improving performance?",
        knobs=["l2.size_kb"],
        values=[64, 128, 256, 512, 1024, 2048, 4096],
        workloads=ALL_WORKLOADS,
    ),
    "associativity": Sweep(
        question="How much do conflict misses cost?",
        knobs=["l1.associativity"],
        values=[1, 2, 4, 8, 16, 32],
        workloads=ALL_WORKLOADS,
    ),
    "associativity_l1_only": Sweep(
        question="Do L1 conflict misses matter when there is no L2 to absorb them?",
        knobs=["l1.associativity"],
        values=[1, 2, 4, 8, 16, 32],
        workloads=ALL_WORKLOADS,
        preset="l1_only",
    ),
    "line_size": Sweep(
        question="How does spatial locality interact with cache line size?",
        knobs=["l1.line_size", "l2.line_size"],
        values=[16, 32, 64, 128, 256],
        workloads=ALL_WORKLOADS,
    ),
    "dram_latency": Sweep(
        question="Which workloads are most sensitive to DRAM latency?",
        knobs=["memory.latency_cycles"],
        values=[60, 90, 120, 180, 240, 300],
        workloads=ALL_WORKLOADS,
    ),
    "mlp": Sweep(
        question="How much does memory-level parallelism (MSHR count) buy?",
        knobs=["cpu.mshrs"],
        values=[1, 2, 4, 8, 16, 32, 64],
        workloads=ALL_WORKLOADS,
    ),
    "bandwidth": Sweep(
        question="At what point does DRAM bandwidth become the limit?",
        knobs=["memory.bandwidth_gbps"],
        values=[3, 6, 12, 25, 50, 100, 200],
        workloads=ALL_WORKLOADS,
    ),
    "blocking": Sweep(
        question="How much can cache blocking recover, and when does it stop mattering?",
        knobs=["l2.size_kb"],
        values=[64, 128, 256, 512, 1024, 2048],
        workloads=MATRIX_WORKLOADS,
    ),
}


def run_sweep(name: str, sweep: Sweep, jobs: int) -> list[dict]:
    base = perfsim.load_config(sweep.preset)
    if sweep.fixed:
        base = perfsim.with_overrides(base, sweep.fixed)

    plan = [(workload, value) for workload in sweep.workloads for value in sweep.values]
    print(f"\n== {name}: {sweep.question}")
    print(f"   {len(plan)} runs over {sweep.knobs} = {sweep.values}")

    # Traces are generated up front because several runs share each one.
    for workload in sweep.workloads:
        perfsim.ensure_trace(workload)

    def execute(item):
        workload, value = item
        overrides = {knob: value for knob in sweep.knobs}
        if sweep.derive is not None:
            overrides.update(sweep.derive(value))
        config = perfsim.with_overrides(base, overrides)
        result = perfsim.simulate(workload, config)
        return perfsim.row(
            workload,
            result,
            experiment=name,
            sweep="+".join(sweep.knobs),
            value=value,
        )

    with ThreadPoolExecutor(max_workers=jobs) as pool:
        rows = list(pool.map(execute, plan))

    for workload in sweep.workloads:
        line = f"   {workload:>15}: "
        line += "  ".join(
            f"{r['value']}={r['ipc']:.2f}" for r in rows if r["workload"] == workload
        )
        print(line + "   (value=IPC)")
    return rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sweep", action="append", default=[], choices=sorted(SWEEPS))
    parser.add_argument("--all", action="store_true", help="run every sweep")
    parser.add_argument("--list", action="store_true", help="describe the sweeps and exit")
    parser.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 1))
    args = parser.parse_args()

    if args.list:
        for name, sweep in SWEEPS.items():
            runs = len(sweep.values) * len(sweep.workloads)
            print(f"{name:<14} {runs:>3} runs  {sweep.question}")
        return 0

    names = sorted(SWEEPS) if args.all or not args.sweep else args.sweep
    try:
        perfsim.ensure_built()
    except perfsim.BuildMissing as error:
        print(error, file=sys.stderr)
        return 1

    all_rows = []
    for name in names:
        rows = run_sweep(name, SWEEPS[name], args.jobs)
        path = perfsim.write_table(rows, name)
        print(f"   wrote {path.relative_to(perfsim.ROOT)}")
        all_rows += rows

    if len(names) > 1:
        path = perfsim.write_table(all_rows, "all_results")
        print(f"\nwrote {path.relative_to(perfsim.ROOT)} ({len(all_rows)} runs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
