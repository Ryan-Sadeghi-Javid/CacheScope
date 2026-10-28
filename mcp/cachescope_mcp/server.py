"""MCP server exposing cachescope to an LLM agent.

Tools
-----
run_simulation(trace, config=None, overrides=None, branch_trace=None)
    Run one simulation. Returns a run_id plus the headline metrics, so the
    agent can keep iterating without re-reading the whole counter set.

sweep(param, values, trace, config=None, overrides=None, branch_trace=None)
    Run the same trace across a list of values for one config parameter.
    Returns one row per value, sorted by the parameter.

get_counters(run_id, prefix=None)
    Full PMU-style counter set for a previous run, optionally filtered by
    name prefix (for example "l1d." or "topdown.").

diagnose(run_id)
    The top-down cycle breakdown and the plain-English verdict for a run.

list_traces() / list_configs()
    Discovery, so the agent does not have to guess at paths.

Design notes
------------
Runs are cached in-process and keyed by a content hash of the whole invocation,
so an agent that asks the same question twice pays for it once. Every result
carries the resolved config, because an agent reasoning about "the 1 MB L2 case"
needs to be able to confirm that is what actually ran.

Errors are returned as structured values rather than raised. A sweep point with
an invalid geometry is useful information for the agent ("that size is not a
multiple of ways x block"), not a reason to fail the whole call.
"""

from __future__ import annotations

import hashlib
import json
import os
import subprocess
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

# The Python MCP SDK renamed FastMCP to MCPServer in 2.0. Support both, so the
# server runs against whichever generation of the SDK is installed.
try:
    from mcp.server.mcpserver import MCPServer as _Server  # mcp >= 2.0
except ImportError:  # pragma: no cover - exercised only on older SDKs
    from mcp.server.fastmcp import FastMCP as _Server  # mcp 1.x

mcp = _Server("cachescope")

# Resolved from the environment so the server works from any working directory.
REPO_ROOT = Path(os.environ.get("CACHESCOPE_ROOT", Path(__file__).resolve().parents[2]))
BINARY = Path(os.environ.get("CACHESCOPE_BIN", REPO_ROOT / "build" / "cachescope"))
TRACE_DIRS = [Path(p) for p in os.environ.get(
    "CACHESCOPE_TRACE_DIRS",
    os.pathsep.join(str(REPO_ROOT / d) for d in ("traces", "traces/upstream",
                                                 "traces/bench", "traces/branch"))
).split(os.pathsep) if p]
CONFIG_DIR = Path(os.environ.get("CACHESCOPE_CONFIG_DIR", REPO_ROOT / "configs"))

# Metrics worth returning without being asked. Everything else is one
# get_counters call away.
HEADLINE = [
    "sim.cpi", "sim.ipc", "sim.amat", "sim.instructions", "sim.accesses",
    "l1d.miss_rate", "l1d.mpki", "l2.miss_rate", "l2.mpki",
    "llc.miss_rate", "llc.mpki", "dram.bw_utilization", "dram.avg_queue_delay",
    "branch.accuracy", "branch.mpki", "branch.cpi_contribution",
]


@dataclass
class Run:
    run_id: str
    command: list[str]
    summary: dict[str, Any] = field(default_factory=dict)


_RUNS: dict[str, Run] = {}


def _resolve(path_like: str, search_dirs: list[Path], suffixes: tuple[str, ...]) -> Path | None:
    """Accepts an absolute path, a repo-relative path, or a bare name."""
    p = Path(path_like)
    if p.is_absolute() and p.exists():
        return p
    candidate = REPO_ROOT / p
    if candidate.exists():
        return candidate
    for d in search_dirs:
        for suffix in ("",) + suffixes:
            c = d / (path_like + suffix)
            if c.exists():
                return c
    return None


def _resolve_trace(trace: str) -> Path | None:
    return _resolve(trace, TRACE_DIRS, (".trace",))


def _resolve_config(config: str | None) -> Path | None:
    if not config:
        return None
    return _resolve(config, [CONFIG_DIR], (".json",))


def _run_key(parts: list[str]) -> str:
    return hashlib.sha256("\x00".join(parts).encode()).hexdigest()[:16]


def _invoke(trace: Path, config: Path | None, overrides: list[str],
            branch_trace: Path | None, third_field: str | None) -> dict[str, Any]:
    """Runs cachescope once and returns the parsed summary, or an error dict."""
    if not BINARY.exists():
        return {"error": f"cachescope binary not found at {BINARY}. "
                         "Build it, or set CACHESCOPE_BIN."}

    cmd = [str(BINARY), "run", "-t", str(trace), "-q"]
    if config:
        cmd += ["-c", str(config)]
    if branch_trace:
        cmd += ["--branch-trace", str(branch_trace)]
    if third_field:
        cmd += ["--third-field", third_field]
    for o in overrides:
        cmd += ["--set", o]

    key = _run_key(cmd)
    if key in _RUNS:
        cached = _RUNS[key]
        return {"run_id": cached.run_id, "cached": True, **cached.summary}

    with tempfile.TemporaryDirectory() as td:
        out = Path(td) / "summary.json"
        proc = subprocess.run(cmd + ["--json", str(out)], capture_output=True, text=True)
        if proc.returncode != 0:
            detail = (proc.stderr or proc.stdout).strip().splitlines()
            return {"error": detail[-1][:400] if detail else "cachescope failed",
                    "command": " ".join(cmd)}
        summary = json.loads(out.read_text())

    _RUNS[key] = Run(run_id=key, command=cmd, summary=summary)
    return {"run_id": key, "cached": False, **summary}


def _headline(summary: dict[str, Any]) -> dict[str, Any]:
    counters = summary.get("counters", {})
    return {k: counters[k] for k in HEADLINE if k in counters}


@mcp.tool()
def list_traces() -> dict[str, Any]:
    """List the memory and branch traces available to the simulator."""
    found: list[dict[str, Any]] = []
    for d in TRACE_DIRS:
        if not d.is_dir():
            continue
        for f in sorted(d.glob("*.trace")):
            found.append({
                "name": f.stem,
                "path": str(f.relative_to(REPO_ROOT) if f.is_relative_to(REPO_ROOT) else f),
                "size_bytes": f.stat().st_size,
                "kind": "branch" if "branch" in f.parts[-2:][0] else "memory",
            })
    return {"traces": found, "searched": [str(d) for d in TRACE_DIRS]}


@mcp.tool()
def list_configs() -> dict[str, Any]:
    """List the simulator configuration files available."""
    if not CONFIG_DIR.is_dir():
        return {"configs": [], "error": f"{CONFIG_DIR} does not exist"}
    out = []
    for f in sorted(CONFIG_DIR.glob("*.json")):
        try:
            doc = json.loads(f.read_text())
            levels = [f"{lv.get('name')}:{lv.get('size_kb', lv.get('size_bytes', '?'))}"
                      for lv in doc.get("levels", [])]
        except (json.JSONDecodeError, OSError):
            levels = []
        out.append({"name": f.stem, "path": str(f.relative_to(REPO_ROOT)), "levels": levels})
    return {"configs": out}


@mcp.tool()
def run_simulation(trace: str, config: str | None = None,
                   overrides: list[str] | None = None,
                   branch_trace: str | None = None,
                   third_field: str | None = None) -> dict[str, Any]:
    """Run one simulation and return its headline metrics and diagnosis.

    Args:
        trace: memory trace name or path, e.g. "swim" or "traces/upstream/swim.trace".
        config: config name or path, e.g. "baseline". Defaults to the built-in config.
        overrides: dotted config assignments, e.g. ["levels.1.size_kb=512"].
        branch_trace: optional branch trace, needed for any branch-related metric.
        third_field: "size", "instr_gap" or "ignore"; overrides the config.

    Returns a run_id usable with get_counters and diagnose.
    """
    tp = _resolve_trace(trace)
    if tp is None:
        return {"error": f"trace {trace!r} not found",
                "hint": "call list_traces() to see what is available"}
    cp = _resolve_config(config)
    if config and cp is None:
        return {"error": f"config {config!r} not found",
                "hint": "call list_configs() to see what is available"}
    bp = _resolve_trace(branch_trace) if branch_trace else None
    if branch_trace and bp is None:
        return {"error": f"branch trace {branch_trace!r} not found"}

    result = _invoke(tp, cp, overrides or [], bp, third_field)
    if "error" in result:
        return result

    return {
        "run_id": result["run_id"],
        "cached": result["cached"],
        "trace": str(tp.name),
        "config": cp.name if cp else "built-in default",
        "overrides": overrides or [],
        "metrics": _headline(result),
        "diagnosis": result["topdown"]["diagnosis"]["headline"],
        "bottleneck": result["topdown"]["diagnosis"]["bottleneck"],
    }


@mcp.tool()
def sweep(param: str, values: list[str], trace: str, config: str | None = None,
          overrides: list[str] | None = None, branch_trace: str | None = None,
          third_field: str | None = None) -> dict[str, Any]:
    """Run one trace across several values of a single config parameter.

    Args:
        param: dotted config path, e.g. "levels.1.size_kb" or "branch.predictor".
        values: the values to try, as strings.
        trace: memory trace name or path.
        config: base config name or path.
        overrides: fixed assignments applied to every point.
        branch_trace: required if param starts with "branch.", because a memory
            trace contains no branches and every point would be identical.

    Returns one row per value, each with its own run_id.
    """
    if param.startswith("branch.") and not branch_trace:
        return {"error": "sweeping a branch.* parameter needs branch_trace; without "
                         "branches every predictor costs zero cycles and all points "
                         "would be identical"}

    tp = _resolve_trace(trace)
    if tp is None:
        return {"error": f"trace {trace!r} not found"}
    cp = _resolve_config(config)
    if config and cp is None:
        return {"error": f"config {config!r} not found"}
    bp = _resolve_trace(branch_trace) if branch_trace else None
    if branch_trace and bp is None:
        return {"error": f"branch trace {branch_trace!r} not found"}

    rows: list[dict[str, Any]] = []
    for v in values:
        result = _invoke(tp, cp, (overrides or []) + [f"{param}={v}"], bp, third_field)
        if "error" in result:
            # A rejected geometry is a real answer about the design space.
            rows.append({param: v, "status": "rejected", "reason": result["error"]})
            continue
        rows.append({
            param: v,
            "status": "ok",
            "run_id": result["run_id"],
            "metrics": _headline(result),
            "bottleneck": result["topdown"]["diagnosis"]["bottleneck"],
        })

    ok = [r for r in rows if r["status"] == "ok"]
    best = min(ok, key=lambda r: r["metrics"].get("sim.cpi", float("inf"))) if ok else None
    return {
        "param": param,
        "trace": tp.name,
        "config": cp.name if cp else "built-in default",
        "results": rows,
        "best_by_cpi": {param: best[param], "cpi": best["metrics"].get("sim.cpi"),
                        "run_id": best["run_id"]} if best else None,
    }


@mcp.tool()
def get_counters(run_id: str, prefix: str | None = None) -> dict[str, Any]:
    """Return the full counter set for a previous run.

    Args:
        run_id: identifier returned by run_simulation or sweep.
        prefix: optional name filter, e.g. "l1d." or "topdown." or "dram.".
    """
    run = _RUNS.get(run_id)
    if run is None:
        return {"error": f"unknown run_id {run_id!r}",
                "known_run_ids": sorted(_RUNS)[:20]}
    counters = run.summary.get("counters", {})
    if prefix:
        counters = {k: v for k, v in counters.items() if k.startswith(prefix)}
        if not counters:
            return {"error": f"no counters start with {prefix!r}",
                    "available_prefixes": sorted({k.split(".")[0] + "."
                                                  for k in run.summary["counters"]})}
    return {"run_id": run_id, "count": len(counters), "counters": counters}


@mcp.tool()
def diagnose(run_id: str) -> dict[str, Any]:
    """Return the top-down cycle breakdown and verdict for a previous run.

    Args:
        run_id: identifier returned by run_simulation or sweep.
    """
    run = _RUNS.get(run_id)
    if run is None:
        return {"error": f"unknown run_id {run_id!r}",
                "known_run_ids": sorted(_RUNS)[:20]}
    td = run.summary["topdown"]
    intervals = run.summary.get("intervals", [])
    peak = max(intervals, key=lambda s: s["dram_bw_utilization"]) if intervals else None
    return {
        "run_id": run_id,
        "headline": td["diagnosis"]["headline"],
        "bottleneck": td["diagnosis"]["bottleneck"],
        "findings": td["diagnosis"]["findings"],
        "cpi": td["cpi"],
        "percent_of_cycles": td["percent_of_cycles"],
        "intervals_sampled": len(intervals),
        "peak_bandwidth_interval": peak,
    }


def main() -> None:
    mcp.run()


if __name__ == "__main__":
    main()
