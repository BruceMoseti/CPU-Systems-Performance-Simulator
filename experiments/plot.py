#!/usr/bin/env python3
"""Turns the sweep results into figures.

    python3 experiments/run_experiments.py --all
    python3 experiments/plot.py --all

Figures land in results/figures/.
"""

from __future__ import annotations

import argparse
import json
import sys

import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt
import matplotlib.ticker
import pandas as pd

import perfsim

FIGURES_DIR = perfsim.RESULTS / "figures"

# One colour and marker per workload, held constant across every figure so the
# lines can be compared between plots.
STYLE = {
    "sequential": ("#1f77b4", "o"),
    "random_access": ("#d62728", "s"),
    "pointer_chase": ("#2ca02c", "^"),
    "matrix_naive": ("#ff7f0e", "D"),
    "matrix_blocked": ("#9467bd", "v"),
}

STALL_COLOURS = {
    "compute": "#4c9f70",
    "l1": "#9ecae1",
    "l2": "#f4a261",
    "dram": "#c1121f",
    "bandwidth": "#7b2cbf",
    "mshr": "#6c757d",
}


def load(name: str) -> pd.DataFrame:
    path = perfsim.RESULTS / f"{name}.csv"
    if not path.exists():
        raise FileNotFoundError(
            f"{path} is missing; run: python3 experiments/run_experiments.py --sweep {name}"
        )
    return pd.read_csv(path)


def save(figure, name: str) -> None:
    FIGURES_DIR.mkdir(parents=True, exist_ok=True)
    path = FIGURES_DIR / f"{name}.png"
    figure.tight_layout()
    figure.savefig(path, dpi=140)
    plt.close(figure)
    print(f"wrote {path.relative_to(perfsim.ROOT)}")


def line_panel(axis, frame: pd.DataFrame, column: str, log_x: bool = True) -> None:
    for workload, group in frame.groupby("workload", sort=False):
        colour, marker = STYLE.get(workload, ("#333333", "o"))
        group = group.sort_values("value")
        axis.plot(group["value"], group[column], marker=marker, color=colour, label=workload)
    if log_x:
        axis.set_xscale("log", base=2)
        values = sorted(frame["value"].unique())
        axis.set_xticks(values)
        axis.xaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: f"{v:g}"))
        axis.xaxis.set_minor_formatter(matplotlib.ticker.NullFormatter())
    axis.grid(alpha=0.3)


def sweep_figure(name: str, x_label: str, title: str, log_x: bool = True) -> None:
    """Standard two-panel figure: IPC on the left, L1 miss rate on the right."""
    frame = load(name)
    figure, (left, right) = plt.subplots(1, 2, figsize=(11, 4.2))

    line_panel(left, frame, "ipc", log_x)
    left.set_xlabel(x_label)
    left.set_ylabel("IPC")
    left.set_title("Performance")
    left.legend(fontsize=8)

    frame = frame.assign(l1_miss_rate=100.0 * (1.0 - frame["l1.hit_rate"]))
    line_panel(right, frame, "l1_miss_rate", log_x)
    right.set_xlabel(x_label)
    right.set_ylabel("L1 miss rate (%)")
    right.set_title("L1 miss rate")

    figure.suptitle(title)
    save(figure, name)


def figure_l1_capacity() -> None:
    """Capacity at a fixed latency next to capacity with latency scaled to size.

    Sweeping capacity alone implies a cache that is free to enlarge, which is not
    how hardware works. Putting both panels side by side shows where the extra
    capacity stops paying for the latency it costs.
    """
    figure, (left, right) = plt.subplots(1, 2, figsize=(11, 4.2))

    line_panel(left, load("l1_capacity"), "ipc")
    left.set_xlabel("L1 size (KB)")
    left.set_ylabel("IPC")
    left.set_title("L1 latency fixed at 4 cycles")
    left.legend(fontsize=8)

    frame = load("l1_capacity_latency")
    line_panel(right, frame, "ipc")
    right.set_ylabel("IPC")
    right.set_title("L1 latency scaled with capacity")

    # Spell out the latency each capacity was given, since that is the whole
    # point of the second panel.
    latencies = frame.drop_duplicates("value").sort_values("value")
    right.set_xticks(list(latencies["value"]))
    right.set_xticklabels(
        [f"{int(r.value)}\n{int(r['config.l1.latency_cycles'])}cy" for _, r in latencies.iterrows()],
        fontsize=8,
    )
    right.set_xlabel("L1 size (KB) and its access latency")

    figure.suptitle("L1 capacity: bigger caches are also slower")
    save(figure, "l1_capacity")


def figure_l2_capacity() -> None:
    frame = load("l2_capacity")
    figure, (left, right) = plt.subplots(1, 2, figsize=(11, 4.2))

    line_panel(left, frame, "ipc")
    left.set_xlabel("L2 size (KB)")
    left.set_ylabel("IPC")
    left.set_title("Performance")
    left.legend(fontsize=8)

    frame = frame.assign(dram_per_1k=1000.0 * frame["memory.reads"] / frame["instructions"])
    line_panel(right, frame, "dram_per_1k")
    right.set_xlabel("L2 size (KB)")
    right.set_ylabel("DRAM reads per 1000 instructions")
    right.set_yscale("log")
    right.set_title("Traffic reaching DRAM")

    figure.suptitle("L2 capacity: when does additional cache stop improving performance?")
    save(figure, "l2_capacity")


def figure_associativity() -> None:
    """Conflict misses, and how much they actually cost.

    The L1 miss rate shows the conflicts directly. Whether they cost any
    performance depends on what sits behind L1, so the right panel repeats the
    sweep on a machine with no L2 at all.
    """
    figure, (left, right) = plt.subplots(1, 2, figsize=(11, 4.2))

    frame = load("associativity")
    frame = frame.assign(l1_miss_rate=100.0 * (1.0 - frame["l1.hit_rate"]))
    line_panel(left, frame, "l1_miss_rate")
    left.set_xlabel("L1 associativity (ways)")
    left.set_ylabel("L1 miss rate (%)")
    left.set_title("L1 miss rate (32 KB L1, 512 KB L2)")
    left.legend(fontsize=8)

    line_panel(right, load("associativity_l1_only"), "ipc")
    right.set_xlabel("L1 associativity (ways)")
    right.set_ylabel("IPC")
    right.set_title("Performance with no L2 to absorb the misses")

    figure.suptitle("Associativity: conflict misses cost nothing until the next level is far away")
    save(figure, "associativity")


def figure_line_size() -> None:
    sweep_figure(
        "line_size",
        "Cache line size (bytes)",
        "Line size: spatial locality against wasted bandwidth",
    )


def figure_dram_latency() -> None:
    frame = load("dram_latency")
    figure, (left, right) = plt.subplots(1, 2, figsize=(11, 4.2))

    line_panel(left, frame, "seconds", log_x=False)
    left.set_xlabel("DRAM latency (cycles)")
    left.set_ylabel("Simulated runtime (s)")
    left.set_yscale("log")
    left.set_title("Runtime")
    left.legend(fontsize=8)

    # Normalising against each workload's own fastest memory shows sensitivity
    # rather than absolute cost, which differs by orders of magnitude here.
    parts = []
    for workload, group in frame.groupby("workload", sort=False):
        group = group.sort_values("value").copy()
        group["relative"] = group["seconds"] / group["seconds"].iloc[0]
        parts.append(group)
    line_panel(right, pd.concat(parts), "relative", log_x=False)
    right.set_xlabel("DRAM latency (cycles)")
    right.set_ylabel("Runtime relative to 60-cycle DRAM")
    right.set_title("Sensitivity to DRAM latency")

    figure.suptitle("DRAM latency: which workloads care?")
    save(figure, "dram_latency")


def figure_mlp() -> None:
    frame = load("mlp")
    figure, (left, right) = plt.subplots(1, 2, figsize=(11, 4.2))

    line_panel(left, frame, "ipc")
    left.set_xlabel("MSHRs (misses allowed in flight)")
    left.set_ylabel("IPC")
    left.set_title("Performance")
    left.legend(fontsize=8)

    line_panel(right, frame, "average_outstanding_misses")
    right.set_xlabel("MSHRs (misses allowed in flight)")
    right.set_ylabel("Average misses in flight")
    right.set_title("Achieved memory-level parallelism")

    figure.suptitle("Memory-level parallelism: a dependent chain cannot use it")
    save(figure, "mlp")


def figure_bandwidth() -> None:
    frame = load("bandwidth")
    figure, (left, right) = plt.subplots(1, 2, figsize=(11, 4.2))

    line_panel(left, frame, "ipc")
    left.set_xlabel("DRAM bandwidth (GB/s)")
    left.set_ylabel("IPC")
    left.set_title("Performance")
    left.legend(fontsize=8)

    frame = frame.assign(utilisation=100.0 * frame["bandwidth_utilization"])
    line_panel(right, frame, "utilisation")
    right.axhline(80, color="#c1121f", linestyle="--", linewidth=1, label="saturation threshold")
    right.set_xlabel("DRAM bandwidth (GB/s)")
    right.set_ylabel("Bandwidth used (%)")
    right.set_title("Bus utilisation")
    right.legend(fontsize=8)

    figure.suptitle("DRAM bandwidth: only the streaming workloads notice")
    save(figure, "bandwidth")


def figure_blocking() -> None:
    frame = load("blocking")
    figure, (left, right) = plt.subplots(1, 2, figsize=(11, 4.2))

    line_panel(left, frame, "ipc")
    left.set_xlabel("L2 size (KB)")
    left.set_ylabel("IPC")
    left.set_title("Same arithmetic, different access order")
    left.legend(fontsize=8)

    frame = frame.assign(l1_miss_rate=100.0 * (1.0 - frame["l1.hit_rate"]))
    line_panel(right, frame, "l1_miss_rate")
    right.set_xlabel("L2 size (KB)")
    right.set_ylabel("L1 miss rate (%)")
    right.set_title("L1 miss rate")

    figure.suptitle("Cache blocking: a software fix for a hardware bottleneck")
    save(figure, "blocking")


def figure_stall_breakdown() -> None:
    """Runs the baseline machine once per workload and stacks where cycles go."""
    config = perfsim.load_config("baseline")
    workloads = list(perfsim.WORKLOADS)
    shares = {bucket: [] for bucket in STALL_COLOURS}
    ipcs = []

    for workload in workloads:
        result = perfsim.simulate(workload, config)
        cycles = result["cycles"]
        ipcs.append(result["ipc"])
        shares["compute"].append(100.0 * result["compute_cycles"] / cycles)
        for bucket in ("l1", "l2", "dram", "bandwidth", "mshr"):
            shares[bucket].append(100.0 * result["stall_cycles"][bucket] / cycles)

    figure, axis = plt.subplots(figsize=(9, 4.6))
    bottom = [0.0] * len(workloads)
    labels = {
        "compute": "compute (issuing)",
        "l1": "L1 latency (dependent)",
        "l2": "L2 latency",
        "dram": "DRAM latency",
        "bandwidth": "DRAM bandwidth",
        "mshr": "no free MSHR",
    }
    for bucket, colour in STALL_COLOURS.items():
        axis.bar(workloads, shares[bucket], bottom=bottom, color=colour, label=labels[bucket])
        bottom = [b + s for b, s in zip(bottom, shares[bucket])]

    for index, ipc in enumerate(ipcs):
        axis.text(index, 101, f"IPC {ipc:.2f}", ha="center", fontsize=9)

    axis.set_ylabel("Share of all cycles (%)")
    axis.set_ylim(0, 112)
    axis.set_title("Where the cycles go on the baseline machine")
    axis.legend(fontsize=8, ncol=3, loc="lower center", bbox_to_anchor=(0.5, -0.32))
    axis.grid(alpha=0.3, axis="y")
    save(figure, "stall_breakdown")


def figure_latency_curve() -> None:
    """The host's real memory hierarchy, measured by a dependent pointer chase."""
    path = perfsim.RESULTS / "latency_curve.csv"
    if not path.exists():
        raise FileNotFoundError(
            f"{path} is missing; run: python3 experiments/validate.py"
        )
    frame = pd.read_csv(path)
    validation = json.loads((perfsim.RESULTS / "validation.json").read_text())

    figure, axis = plt.subplots(figsize=(8, 4.4))
    axis.plot(frame["working_set_kb"], frame["cycles_per_hop"], marker="o", color="#1f77b4")
    axis.set_xscale("log", base=2)
    axis.xaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: f"{v:g}"))
    axis.set_xlabel("Working set (KB)")
    axis.set_ylabel("Cycles per dependent load")
    axis.grid(alpha=0.3)

    # Mark where the host says each cache level ends; the steps should line up.
    for cache in validation["host"]["caches"]:
        if cache["type"] == "Instruction":
            continue
        axis.axvline(cache["size_kb"], color="#c1121f", linestyle="--", linewidth=1)
        axis.text(
            cache["size_kb"], axis.get_ylim()[1] * 0.95,
            f" L{cache['level']} {cache['size_kb']} KB", fontsize=8, color="#c1121f",
            ha="left", va="top", rotation=90,
        )

    axis.set_title(
        f"Measured memory hierarchy of {validation['host']['model_name']}\n"
        f"(each step is a level of the real cache hierarchy)"
    )
    save(figure, "latency_curve")


FIGURES = {
    "latency_curve": figure_latency_curve,
    "l1_capacity": figure_l1_capacity,
    "l2_capacity": figure_l2_capacity,
    "associativity": figure_associativity,
    "line_size": figure_line_size,
    "dram_latency": figure_dram_latency,
    "mlp": figure_mlp,
    "bandwidth": figure_bandwidth,
    "blocking": figure_blocking,
    "stall_breakdown": figure_stall_breakdown,
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--figure", action="append", default=[], choices=sorted(FIGURES))
    parser.add_argument("--all", action="store_true")
    args = parser.parse_args()

    names = sorted(FIGURES) if args.all or not args.figure else args.figure
    failures = 0
    for name in names:
        try:
            FIGURES[name]()
        except FileNotFoundError as error:
            print(f"skipping {name}: {error}", file=sys.stderr)
            failures += 1
    return 1 if failures == len(names) else 0


if __name__ == "__main__":
    sys.exit(main())
