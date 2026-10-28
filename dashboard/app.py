"""Streamlit dashboard for cachescope.

Pick a trace and a configuration, tweak the hierarchy, run the simulator, and
read the counters, the CPI stack and the time series without touching the CLI.

Run it with:
    streamlit run dashboard/app.py

Environment:
    CACHESCOPE_BIN   path to the built simulator (default: build/cachescope)
    CACHESCOPE_ROOT  repository root (default: inferred from this file)
"""

from __future__ import annotations

import json
import os
import subprocess
import tempfile
from pathlib import Path

import pandas as pd
import streamlit as st

REPO_ROOT = Path(os.environ.get("CACHESCOPE_ROOT", Path(__file__).resolve().parents[1]))
BINARY = Path(os.environ.get("CACHESCOPE_BIN", REPO_ROOT / "build" / "cachescope"))
CONFIG_DIR = REPO_ROOT / "configs"
TRACE_DIRS = [REPO_ROOT / "traces" / d for d in ("upstream", "bench", "branch")]

# Same validated categorical order used by scripts/analyze.py, so a chart here
# and a chart in the README mean the same thing by colour.
SERIES = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#4a3aa7"]

BUCKETS = [
    ("base work", "topdown.base_pct", SERIES[0]),
    ("L1 stall", "topdown.l1_stall_pct", SERIES[1]),
    ("L2 stall", "topdown.l2_stall_pct", SERIES[2]),
    ("LLC stall", "topdown.llc_stall_pct", SERIES[3]),
    ("DRAM latency", "topdown.dram_latency_pct", SERIES[4]),
    ("DRAM bandwidth", "topdown.dram_queue_pct", SERIES[5]),
    ("branch", "topdown.branch_pct", SERIES[6]),
]

st.set_page_config(page_title="cachescope", page_icon=":bar_chart:", layout="wide")


def find_traces(kind: str) -> dict[str, Path]:
    out: dict[str, Path] = {}
    for d in TRACE_DIRS:
        if not d.is_dir():
            continue
        is_branch = d.name == "branch"
        if (kind == "branch") != is_branch:
            continue
        for f in sorted(d.glob("*.trace")):
            out[f"{d.name}/{f.stem}"] = f
    return out


@st.cache_data(show_spinner=False)
def run_sim(binary: str, config: str, trace: str, overrides: tuple[str, ...],
            branch_trace: str, third_field: str, interval: int) -> dict:
    """Runs the simulator. Cached so moving an unrelated slider is instant."""
    with tempfile.TemporaryDirectory() as td:
        summary_path = Path(td) / "summary.json"
        csv_path = Path(td) / "telemetry.csv"
        cmd = [binary, "run", "-c", config, "-t", trace, "-q",
               "--json", str(summary_path), "--third-field", third_field,
               "--interval", str(interval)]
        if interval > 0:
            cmd += ["--csv", str(csv_path)]
        if branch_trace:
            cmd += ["--branch-trace", branch_trace]
        for o in overrides:
            cmd += ["--set", o]

        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0:
            detail = (proc.stderr or proc.stdout).strip().splitlines()
            return {"error": detail[-1] if detail else "cachescope failed",
                    "command": " ".join(cmd)}
        summary = json.loads(summary_path.read_text())
        telemetry = pd.read_csv(csv_path).to_dict("list") if csv_path.exists() else {}
    return {"summary": summary, "telemetry": telemetry, "command": " ".join(cmd)}


st.title("cachescope")
st.caption("Trace-driven memory hierarchy and branch prediction simulator")

if not BINARY.exists():
    st.error(f"Simulator binary not found at `{BINARY}`.\n\n"
             "Build it first:\n\n"
             "```\ncmake -S . -B build -DCMAKE_BUILD_TYPE=Release\n"
             "cmake --build build\n```\n\n"
             "or point `CACHESCOPE_BIN` at an existing build.")
    st.stop()

mem_traces = find_traces("memory")
branch_traces = find_traces("branch")
configs = {f.stem: f for f in sorted(CONFIG_DIR.glob("*.json"))} if CONFIG_DIR.is_dir() else {}

if not mem_traces:
    st.error("No memory traces found. Run `scripts/fetch_traces.sh` first.")
    st.stop()
if not configs:
    st.error(f"No configurations found in `{CONFIG_DIR}`.")
    st.stop()

# --- controls, all in one row above the charts ----------------------------
with st.sidebar:
    st.header("Workload")
    trace_name = st.selectbox("Memory trace", list(mem_traces), index=0)
    config_name = st.selectbox("Configuration", list(configs),
                               index=list(configs).index("baseline")
                               if "baseline" in configs else 0)
    third_field = st.selectbox(
        "Third trace field", ["size", "instr_gap", "ignore"], index=0,
        help="Traces captured with scripts/capture_valgrind.sh carry a measured "
             "instruction count; use instr_gap for those.")
    use_branches = st.checkbox("Include a branch trace", value=bool(branch_traces))
    branch_name = (st.selectbox("Branch trace", list(branch_traces))
                   if use_branches and branch_traces else "")
    interval = st.number_input("Sample every N accesses", min_value=0, value=20000,
                               step=5000,
                               help="0 disables the time series.")

    st.header("Overrides")
    st.caption("Applied on top of the chosen configuration.")
    base_cfg = json.loads(configs[config_name].read_text())
    overrides: list[str] = []
    for i, level in enumerate(base_cfg.get("levels", [])):
        name = level.get("name", f"level{i}")
        with st.expander(f"{name}", expanded=(i == 0)):
            cur_kb = int(level.get("size_kb", level.get("size_bytes", 32768) // 1024))
            sizes = [4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192]
            kb = st.select_slider(f"{name} size", options=sizes,
                                  value=cur_kb if cur_kb in sizes else min(
                                      sizes, key=lambda s: abs(s - cur_kb)),
                                  key=f"size{i}")
            assoc = st.select_slider(f"{name} associativity", options=[1, 2, 4, 8, 16],
                                     value=int(level.get("assoc", 8)), key=f"assoc{i}")
            repl = st.selectbox(f"{name} replacement", ["lru", "fifo", "random"],
                                index=["lru", "fifo", "random"].index(
                                    level.get("replacement", "lru")), key=f"repl{i}")
            pf_cur = level.get("prefetcher", {})
            pf_cur = pf_cur.get("kind", "none") if isinstance(pf_cur, dict) else pf_cur
            pf = st.selectbox(f"{name} prefetcher", ["none", "next-line", "stride"],
                              index=["none", "next-line", "stride"].index(pf_cur),
                              key=f"pf{i}")
            overrides += [f"levels.{i}.size_kb={kb}", f"levels.{i}.assoc={assoc}",
                          f"levels.{i}.replacement={repl}",
                          f"levels.{i}.prefetcher.kind={pf}"]

    with st.expander("Memory and core"):
        dram_lat = st.slider("DRAM latency (cycles)", 20, 400,
                             int(base_cfg.get("dram", {}).get("latency_cycles", 200)), 10)
        dram_bw = st.slider("DRAM bandwidth (bytes/cycle)", 0.25, 32.0,
                            float(base_cfg.get("dram", {}).get("bytes_per_cycle", 8.0)), 0.25)
        mlp = st.slider("MLP overlap", 0.0, 0.95,
                        float(base_cfg.get("timing", {}).get("mlp_overlap", 0.0)), 0.05,
                        help="Fraction of post-L1 latency hidden by overlapping misses. "
                             "0 models a fully blocking core, which can never be "
                             "bandwidth-bound.")
        predictor = st.selectbox("Branch predictor",
                                 ["bimodal", "gshare", "tournament", "perceptron"],
                                 index=["bimodal", "gshare", "tournament", "perceptron"].index(
                                     base_cfg.get("branch", {}).get("predictor", "gshare")))
        overrides += [f"dram.latency_cycles={dram_lat}", f"dram.bytes_per_cycle={dram_bw}",
                      f"timing.mlp_overlap={mlp}", f"branch.predictor={predictor}"]

result = run_sim(str(BINARY), str(configs[config_name]), str(mem_traces[trace_name]),
                 tuple(overrides), str(branch_traces[branch_name]) if branch_name else "",
                 third_field, int(interval))

if "error" in result:
    st.error(f"Simulation failed: {result['error']}")
    st.code(result["command"], language="bash")
    st.stop()

summary = result["summary"]
counters = summary["counters"]
topdown = summary["topdown"]

# --- headline -------------------------------------------------------------
cols = st.columns(5)
cols[0].metric("CPI", f"{counters['sim.cpi']:.4f}")
cols[1].metric("IPC", f"{counters['sim.ipc']:.4f}")
cols[2].metric("AMAT", f"{counters['sim.amat']:.2f} cyc")
cols[3].metric("L1D MPKI", f"{counters.get('l1d.mpki', 0):.2f}")
cols[4].metric("DRAM bandwidth", f"{100 * counters['dram.bw_utilization']:.1f}%")

verdict = topdown["diagnosis"]["headline"]
(st.warning if topdown["diagnosis"]["bottleneck"] != "base" else st.success)(verdict)

tab_stack, tab_levels, tab_series, tab_counters = st.tabs(
    ["CPI stack", "Cache levels", "Time series", "All counters"])

with tab_stack:
    cpi = counters["sim.cpi"]
    rows = [{"bucket": label, "cycles per instruction": cpi * counters[col] / 100.0,
             "share of cycles": f"{counters[col]:.2f}%"}
            for label, col, _ in BUCKETS if counters.get(col, 0) > 0]
    df = pd.DataFrame(rows)
    st.bar_chart(df.set_index("bucket")["cycles per instruction"], horizontal=True,
                 color=SERIES[0])
    # The table is the accessible view of the same numbers.
    st.dataframe(df, hide_index=True, width="stretch")
    for finding in topdown["diagnosis"]["findings"]:
        st.caption(f"- {finding}")

with tab_levels:
    levels = [lv["name"] for lv in summary["config"]["levels"]]
    rows = []
    for name in levels:
        rows.append({
            "level": name,
            "size": f"{next(l['size_bytes'] for l in summary['config']['levels'] if l['name'] == name) // 1024} KiB",
            "accesses": counters.get(f"{name}.accesses", 0),
            "misses": counters.get(f"{name}.misses", 0),
            "miss rate": f"{100 * counters.get(f'{name}.miss_rate', 0):.2f}%",
            "MPKI": round(counters.get(f"{name}.mpki", 0), 3),
            "writebacks": counters.get(f"{name}.dirty_evictions", 0),
            "prefetch useful": counters.get(f"{name}.prefetch.useful", 0),
        })
    st.dataframe(pd.DataFrame(rows), hide_index=True, width="stretch")

    st.subheader("DRAM")
    st.dataframe(pd.DataFrame([{
        "reads": counters["dram.reads"], "writes": counters["dram.writes"],
        "bytes read": counters["dram.bytes_read"],
        "bytes written": counters["dram.bytes_written"],
        "bandwidth used": f"{100 * counters['dram.bw_utilization']:.1f}%",
        "avg queue delay": f"{counters['dram.avg_queue_delay']:.2f} cyc",
    }]), hide_index=True, width="stretch")

with tab_series:
    tel = result["telemetry"]
    if not tel or "interval.index" not in tel:
        st.info("Set a non-zero sampling interval in the sidebar to collect a time series.")
    else:
        tdf = pd.DataFrame(tel)
        mpki_cols = [c for c in tdf.columns if c.endswith(".mpki") and not c.startswith("branch")]
        st.subheader("MPKI per level, per interval")
        st.line_chart(tdf.set_index("interval.index")[mpki_cols], color=SERIES[:len(mpki_cols)])
        st.subheader("Interval CPI and DRAM bandwidth")
        c1, c2 = st.columns(2)
        # Two measures of different scale get two charts, never two y-axes.
        c1.line_chart(tdf.set_index("interval.index")[["interval.cpi"]], color=SERIES[0])
        c2.line_chart(tdf.set_index("interval.index")[["interval.dram_bw_utilization"]],
                      color=SERIES[4])
        st.dataframe(tdf, hide_index=True, width="stretch")

with tab_counters:
    cdf = pd.DataFrame(sorted(counters.items()), columns=["counter", "value"])
    query = st.text_input("Filter", "", placeholder="l1d. or topdown. or branch.")
    if query:
        cdf = cdf[cdf["counter"].str.contains(query, case=False)]
    st.dataframe(cdf, hide_index=True, width="stretch", height=520)
    st.download_button("Download summary JSON", json.dumps(summary, indent=2),
                       file_name="cachescope_summary.json", mime="application/json")

with st.expander("Command that produced these numbers"):
    st.code(result["command"], language="bash")
