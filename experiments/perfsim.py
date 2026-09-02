"""Shared plumbing for the experiment scripts: traces, configs and simulator runs."""

from __future__ import annotations

import copy
import json
import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
PERFSIM = BUILD / "perfsim"
TRACEGEN = BUILD / "tracegen"
PERFCOUNT = BUILD / "perfcount"
CONFIGS = ROOT / "configs"
TRACES = ROOT / "traces"
RESULTS = ROOT / "results"

# Named workloads with the sizes the experiments use.
#
# The working sets are chosen so that the interesting behaviour is visible on the
# baseline machine: 8 MB for the array workloads (well past a 512 KB L2), a
# 192x192 matrix multiply whose three 288 KB matrices do not fit in L2 together
# but whose 32x32 tiles do fit in a 32 KB L1, and 24 structures 4 KB apart, whose
# 1.5 KB of touched data fits in any L1 here but aliases onto a single set.
WORKLOADS = {
    "sequential": {"workload": "sequential", "n": 1_000_000},
    "random_access": {"workload": "random_access", "n": 1_000_000},
    "pointer_chase": {"workload": "pointer_chase", "n": 1_000_000},
    "strided": {"workload": "strided", "n": 24, "stride": 4096, "iterations": 41_666},
    "matrix_naive": {"workload": "matrix", "n": 192},
    "matrix_blocked": {"workload": "matrix", "n": 192, "block": 32},
}


class BuildMissing(RuntimeError):
    pass


def ensure_built() -> None:
    missing = [str(p.relative_to(ROOT)) for p in (PERFSIM, TRACEGEN) if not p.exists()]
    if missing:
        raise BuildMissing(
            "missing build artefacts: {}\nBuild first:\n"
            "  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++\n"
            "  cmake --build build -j".format(", ".join(missing))
        )


def load_config(preset: str) -> dict:
    """Loads configs/<preset>.json, or a path to a config file."""
    path = Path(preset)
    if not path.exists():
        path = CONFIGS / f"{preset}.json"
    if not path.exists():
        raise FileNotFoundError(f"no such config or preset: {preset}")
    with path.open() as handle:
        return json.load(handle)


def with_overrides(config: dict, overrides: dict) -> dict:
    """Returns a copy of config with dotted keys such as 'l2.size_kb' replaced."""
    updated = copy.deepcopy(config)
    for dotted, value in overrides.items():
        section, _, field = dotted.partition(".")
        if not field:
            raise ValueError(f"override {dotted!r} must be of the form section.field")
        if section not in updated:
            raise KeyError(f"config has no {section!r} section")
        if field not in updated[section]:
            # perfsim rejects unknown keys, so catch the typo here where the
            # error message can point at the override that caused it.
            raise KeyError(f"config section {section!r} has no field {field!r}")
        updated[section][field] = value
    return updated


def trace_path(name: str) -> Path:
    return TRACES / f"{name}.trace"


def ensure_trace(name: str, force: bool = False) -> Path:
    """Generates traces/<name>.trace on demand and caches it.

    A sidecar file records the parameters the trace was generated with, so that
    changing WORKLOADS regenerates it instead of silently reusing stale data.
    """
    ensure_built()
    if name not in WORKLOADS:
        raise KeyError(f"unknown workload {name!r}; known: {', '.join(sorted(WORKLOADS))}")
    spec = WORKLOADS[name]
    path = trace_path(name)
    stamp = path.with_suffix(".params")
    wanted = json.dumps(spec, sort_keys=True)

    if not force and path.exists() and stamp.exists() and stamp.read_text() == wanted:
        return path

    TRACES.mkdir(parents=True, exist_ok=True)
    command = [str(TRACEGEN), spec["workload"], "--out", str(path)]
    for key in ("n", "block", "stride", "iterations", "seed"):
        if key in spec:
            command += [f"--{key}", str(spec[key])]
    subprocess.run(command, check=True)
    stamp.write_text(wanted)
    return path


def run_simulation(config: dict, trace: Path) -> dict:
    """Runs one simulation and returns the parsed JSON result."""
    ensure_built()
    handle, config_path = tempfile.mkstemp(suffix=".json", prefix="perfsim_config_")
    try:
        with os.fdopen(handle, "w") as out:
            json.dump(config, out)
        completed = subprocess.run(
            [str(PERFSIM), "--config", config_path, "--trace", str(trace), "--quiet", "--json", "-"],
            check=True,
            capture_output=True,
            text=True,
        )
    finally:
        os.unlink(config_path)
    return json.loads(completed.stdout)


def simulate(workload: str, config: dict) -> dict:
    return run_simulation(config, ensure_trace(workload))


def flatten(result: dict, prefix: str = "") -> dict:
    """Flattens the nested result JSON into dotted columns for a DataFrame."""
    flat = {}
    for key, value in result.items():
        name = f"{prefix}{key}"
        if isinstance(value, dict):
            flat.update(flatten(value, prefix=f"{name}."))
        elif isinstance(value, list):
            flat[name] = "; ".join(str(item) for item in value)
        else:
            flat[name] = value
    return flat


def row(workload: str, result: dict, **extra) -> dict:
    """Builds one tidy result row: identity, knobs, metrics and stall shares."""
    flat = flatten(result)
    # How fast the simulator itself ran is not a property of the simulated
    # machine, and including it would make committed results churn on every
    # re-run. benchmark.py is where that belongs.
    flat = {k: v for k, v in flat.items() if not k.startswith("simulator.")}
    cycles = flat.get("cycles") or 1
    for bucket in ("l1", "l2", "dram", "bandwidth", "mshr"):
        flat[f"stall_share.{bucket}"] = flat.get(f"stall_cycles.{bucket}", 0) / cycles
    flat["stall_share.compute"] = flat.get("compute_cycles", 0) / cycles
    flat["workload"] = workload
    flat.update(extra)
    return flat


def write_table(rows: list[dict], name: str) -> Path:
    """Writes results/<name>.csv, keeping identity columns first."""
    import pandas as pd

    RESULTS.mkdir(parents=True, exist_ok=True)
    frame = pd.DataFrame(rows)
    leading = [c for c in ("experiment", "workload", "sweep", "value") if c in frame.columns]
    ordered = leading + [c for c in frame.columns if c not in leading]
    path = RESULTS / f"{name}.csv"
    frame[ordered].to_csv(path, index=False)
    return path
