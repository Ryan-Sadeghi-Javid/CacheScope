#!/usr/bin/env python3
"""Produce the analysis figures for cachescope.

Four figures, each answering one question:

  miss_rate_vs_size   How far does enlarging L1D get you, and does associativity
                      substitute for capacity? (line chart, one line per way count)
  mpki_over_time      Does the workload have phases, or is one average honest?
                      (interval time series, one line per cache level)
  cpi_stack           Where do the cycles actually go, per configuration?
                      (stacked bar: base work plus each stall bucket)
  predictor_accuracy  Which predictor earns its storage? (bar chart plus a
                      state-cost annotation)

Every figure is rendered twice, for light and dark surfaces, from the same
validated categorical palette stepped per mode. Each also writes the exact
numbers it plots to a sibling CSV: three of the palette slots sit below 3:1
contrast on the light surface, so the design rule is that direct labels or a
table view must accompany them. The CSVs are that table view.

Usage:
  python3 scripts/analyze.py --sweep-l1 analysis/sweep_l1_gcc.csv \\
      --telemetry analysis/telemetry_swim.csv \\
      --cpi-stack analysis/cpi_stack_inputs.csv \\
      --predictors analysis/sweep_predictor.csv \\
      --outdir analysis/figures
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")  # no display in CI or WSL
import matplotlib.pyplot as plt
import pandas as pd
from matplotlib.ticker import FuncFormatter

# ---------------------------------------------------------------- design tokens
# Categorical slots in fixed order, never cycled. Validated with the skill's
# validator for both surfaces: worst adjacent CVD delta-E 9.1 light / 8.4 dark.
SERIES_LIGHT = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100",
                "#e87ba4", "#008300", "#4a3aa7", "#e34948"]
SERIES_DARK = ["#3987e5", "#d95926", "#199e70", "#c98500",
               "#d55181", "#008300", "#9085e9", "#e66767"]

THEMES = {
    "light": {
        "series": SERIES_LIGHT,
        "surface": "#fcfcfb",
        "ink": "#0b0b0b",
        "ink2": "#52514e",
        "muted": "#898781",
        "grid": "#e1e0d9",
        "axis": "#c3c2b7",
    },
    "dark": {
        "series": SERIES_DARK,
        "surface": "#1a1a19",
        "ink": "#ffffff",
        "ink2": "#c3c2b7",
        "muted": "#898781",
        "grid": "#2c2c2a",
        "axis": "#383835",
    },
}

FONTS = ["DejaVu Sans", "system-ui", "Segoe UI", "sans-serif"]


def style(theme: dict) -> None:
    """Applies the recessive-chrome rules once per figure."""
    plt.rcParams.update({
        "font.family": "sans-serif",
        "font.sans-serif": FONTS,
        "figure.facecolor": theme["surface"],
        "axes.facecolor": theme["surface"],
        "savefig.facecolor": theme["surface"],
        "text.color": theme["ink"],
        "axes.labelcolor": theme["ink2"],
        "axes.edgecolor": theme["axis"],
        "xtick.color": theme["muted"],
        "ytick.color": theme["muted"],
        "xtick.labelcolor": theme["ink2"],
        "ytick.labelcolor": theme["ink2"],
        "grid.color": theme["grid"],
        "grid.linewidth": 0.8,
        "axes.grid": True,
        "axes.grid.axis": "y",
        "axes.spines.top": False,
        "axes.spines.right": False,
        "legend.frameon": False,
        "figure.dpi": 150,
        "font.size": 10,
    })


def finish(fig, ax, title: str, subtitle: str, theme: dict) -> None:
    ax.set_title(title, color=theme["ink"], fontsize=13, fontweight="bold", loc="left", pad=18)
    if subtitle:
        ax.text(0.0, 1.02, subtitle, transform=ax.transAxes, color=theme["ink2"],
                fontsize=9.5, va="bottom", ha="left")
    ax.set_axisbelow(True)
    fig.tight_layout()


def save(fig, outdir: Path, name: str, mode: str) -> Path:
    outdir.mkdir(parents=True, exist_ok=True)
    path = outdir / f"{name}_{mode}.png"
    fig.savefig(path, bbox_inches="tight")
    plt.close(fig)
    return path


def kib(v: float) -> str:
    v = int(v)
    return f"{v // 1024} MiB" if v >= 1024 and v % 1024 == 0 else f"{v} KiB"


# ---------------------------------------------------------------- figure 1
def fig_miss_rate_vs_size(csv: Path, outdir: Path, written: list[Path]) -> None:
    df = pd.read_csv(csv)
    df = df[df["status"] == "ok"].copy()
    if df.empty:
        print(f"  skip miss_rate_vs_size: no valid rows in {csv}", file=sys.stderr)
        return

    size_col = "levels.0.size_kb"
    assoc_col = "levels.0.assoc"
    if size_col not in df or assoc_col not in df:
        print(f"  skip miss_rate_vs_size: {csv} is not an L1 size x assoc sweep", file=sys.stderr)
        return

    df[size_col] = df[size_col].astype(int)
    df[assoc_col] = df[assoc_col].astype(int)
    df["miss_pct"] = df["l1d.miss_rate"].astype(float) * 100.0
    df = df.sort_values([assoc_col, size_col])
    df[[assoc_col, size_col, "miss_pct", "l1d.mpki", "sim.cpi"]].to_csv(
        outdir / "miss_rate_vs_size.csv", index=False)

    for mode, theme in THEMES.items():
        style(theme)
        fig, ax = plt.subplots(figsize=(7.4, 4.6))
        assocs = sorted(df[assoc_col].unique())
        for i, a in enumerate(assocs):
            sub = df[df[assoc_col] == a]
            color = theme["series"][i % len(theme["series"])]
            ax.plot(sub[size_col], sub["miss_pct"], color=color, linewidth=2.0,
                    marker="o", markersize=5, markeredgecolor=theme["surface"],
                    markeredgewidth=1.2, label=f"{a}-way", zorder=3)
            # Direct labels go at the LEFT edge, not the right. These series
            # converge as capacity grows, so end labels would pile up on top of
            # each other; the first point is where they are furthest apart.
            first = sub.iloc[0]
            ax.annotate(f"{a}-way", (first[size_col], first["miss_pct"]),
                        textcoords="offset points", xytext=(-10, 0), va="center",
                        ha="right", color=theme["ink2"], fontsize=9)

        ax.set_xscale("log", base=2)
        ax.set_xticks(sorted(df[size_col].unique()))
        ax.xaxis.set_major_formatter(FuncFormatter(lambda v, _: kib(v)))
        ax.set_xlabel("L1D capacity")
        ax.set_ylabel("L1D demand miss rate")
        ax.yaxis.set_major_formatter(FuncFormatter(lambda v, _: f"{v:g}%"))
        ax.set_ylim(bottom=0)
        ax.margins(x=0.22)
        ax.legend(loc="upper right", ncols=len(assocs), fontsize=9,
                  labelcolor=theme["ink2"])
        finish(fig, ax, "Associativity matters most when capacity is scarce",
               "L1D miss rate on gcc.trace, 64-byte lines, LRU; the ways converge "
               "once the working set fits", theme)
        written.append(save(fig, outdir, "miss_rate_vs_size", mode))


# ---------------------------------------------------------------- figure 2
def fig_mpki_over_time(csv: Path, outdir: Path, written: list[Path]) -> None:
    df = pd.read_csv(csv)
    if df.empty or "interval.index" not in df:
        print(f"  skip mpki_over_time: {csv} has no interval rows", file=sys.stderr)
        return

    # Per-interval MPKI for each level, derived from the monotonic counters so
    # the series shows phase behaviour rather than a running average.
    levels = [c.split(".")[0] for c in df.columns if c.endswith(".misses")]
    seen: list[str] = []
    for lv in levels:
        if lv not in seen and lv not in ("branch",):
            seen.append(lv)
    if not seen:
        print(f"  skip mpki_over_time: no per-level miss counters in {csv}", file=sys.stderr)
        return

    out = pd.DataFrame({"interval": df["interval.index"].astype(int),
                        "accesses": df["sim.accesses"].astype(float)})
    d_instr = df["sim.instructions"].astype(float).diff().fillna(
        df["sim.instructions"].astype(float))
    for lv in seen:
        d_miss = df[f"{lv}.misses"].astype(float).diff().fillna(df[f"{lv}.misses"].astype(float))
        out[f"{lv}_mpki"] = (1000.0 * d_miss / d_instr).where(d_instr > 0, 0.0)
    out["dram_bw_utilization_pct"] = df["interval.dram_bw_utilization"].astype(float) * 100.0
    out.to_csv(outdir / "mpki_over_time.csv", index=False)

    # Series whose final values sit on top of each other cannot all carry an end
    # label. Decide which ones can, before drawing anything.
    finals = {lv: float(out[f"{lv}_mpki"].iloc[-1]) for lv in seen}
    full_span = max(max(out[f"{lv}_mpki"].max() for lv in seen), 1e-9)
    # Track which series were labelled by NAME. Testing membership by value
    # fails exactly when it matters most: coincident series have identical
    # finals, so every one of them would match the single recorded value.
    labelled_y: list[float] = []
    can_label: set[str] = set()
    for lv in seen:
        if all(abs(finals[lv] - y) > 0.05 * full_span for y in labelled_y):
            labelled_y.append(finals[lv])
            can_label.add(lv)
    overlapping = len(can_label) < len(seen)

    for mode, theme in THEMES.items():
        style(theme)
        fig, ax = plt.subplots(figsize=(7.4, 4.6))
        for i, lv in enumerate(seen):
            color = theme["series"][i % len(theme["series"])]
            # Draw outer levels first and progressively thinner, so a level that
            # sits exactly on top of another does not erase it entirely.
            ax.plot(out["interval"], out[f"{lv}_mpki"], color=color,
                    linewidth=3.4 - 0.7 * i, marker="o", markersize=5.5 - 0.7 * i,
                    markeredgecolor=theme["surface"], markeredgewidth=1.0,
                    label=lv, zorder=3 + i, alpha=0.95)
            if lv in can_label:
                ax.annotate(lv, (out["interval"].iloc[-1], finals[lv]),
                            textcoords="offset points", xytext=(9, 0), va="center",
                            color=theme["ink2"], fontsize=9)

        ax.set_xlabel("sampling interval")
        ax.set_ylabel("misses per 1000 instructions")
        ax.set_ylim(bottom=0)
        ax.margins(x=0.10)
        ax.legend(loc="upper right", ncols=len(seen), fontsize=9, labelcolor=theme["ink2"])
        subtitle = "Per-interval MPKI by level; each point covers one sampling window"
        if overlapping:
            # Say it rather than let the reader wonder where a series went.
            subtitle += ". The levels coincide: nearly every L1 miss reaches DRAM"
        finish(fig, ax, "Miss behaviour is not constant across the run", subtitle, theme)
        written.append(save(fig, outdir, "mpki_over_time", mode))


# ---------------------------------------------------------------- figure 3
def fig_cpi_stack(csv: Path, outdir: Path, written: list[Path]) -> None:
    df = pd.read_csv(csv)
    if df.empty or "label" not in df:
        print(f"  skip cpi_stack: {csv} needs a 'label' column", file=sys.stderr)
        return

    # Absolute cycles-per-instruction contribution of each bucket, so bar length
    # is CPI and the segments are an exact decomposition of it.
    buckets = [
        ("base work", "topdown.base_pct"),
        ("L1 stall", "topdown.l1_stall_pct"),
        ("L2 stall", "topdown.l2_stall_pct"),
        ("LLC stall", "topdown.llc_stall_pct"),
        ("DRAM latency", "topdown.dram_latency_pct"),
        ("DRAM bandwidth", "topdown.dram_queue_pct"),
        ("branch", "topdown.branch_pct"),
    ]
    for _, col in buckets:
        if col not in df:
            print(f"  skip cpi_stack: missing column {col}", file=sys.stderr)
            return

    cpi = df["sim.cpi"].astype(float)
    table = pd.DataFrame({"label": df["label"], "cpi": cpi})
    for name, col in buckets:
        table[name] = cpi * df[col].astype(float) / 100.0
    table.to_csv(outdir / "cpi_stack.csv", index=False)

    for mode, theme in THEMES.items():
        style(theme)
        fig, ax = plt.subplots(figsize=(8.0, 0.62 * len(df) + 2.3))
        ax.grid(axis="x")
        ax.grid(axis="y", visible=False)

        y = range(len(df))
        left = [0.0] * len(df)
        for i, (name, _col) in enumerate(buckets):
            vals = table[name].tolist()
            if all(v <= 0 for v in vals):
                continue  # never draw an empty bucket into the legend
            color = theme["series"][i % len(theme["series"])]
            # A 2px surface gap between adjacent segments keeps the boundary
            # readable without a stroke.
            ax.barh(list(y), vals, left=left, height=0.58, color=color, label=name,
                    edgecolor=theme["surface"], linewidth=1.4, zorder=3)
            for j, v in enumerate(vals):
                # Selective direct labels: only segments wide enough to read.
                if v / max(cpi.max(), 1e-9) > 0.07:
                    ax.text(left[j] + v / 2, j, f"{v:.2f}", ha="center", va="center",
                            fontsize=8.5, color=theme["surface"], fontweight="bold")
                left[j] += v

        for j, total in enumerate(cpi):
            ax.text(total, j, f"  CPI {total:.3f}", va="center", ha="left",
                    fontsize=9.5, color=theme["ink"], fontweight="bold")

        ax.set_yticks(list(y))
        ax.set_yticklabels(df["label"])
        ax.invert_yaxis()
        ax.set_xlabel("cycles per instruction")
        ax.set_xlim(0, cpi.max() * 1.24)
        ax.legend(loc="upper center", bbox_to_anchor=(0.5, -0.16), ncols=4,
                  fontsize=8.5, labelcolor=theme["ink2"])
        finish(fig, ax, "Where the cycles go",
               "Each bar is total CPI, split into base work and stall sources", theme)
        written.append(save(fig, outdir, "cpi_stack", mode))


# ---------------------------------------------------------------- figure 4
def fig_predictor_accuracy(csv: Path, outdir: Path, written: list[Path]) -> None:
    df = pd.read_csv(csv)
    df = df[df["status"] == "ok"].copy() if "status" in df else df
    col = "branch.predictor" if "branch.predictor" in df else "predictor"
    if df.empty or col not in df:
        print(f"  skip predictor_accuracy: {csv} is not a predictor sweep", file=sys.stderr)
        return

    df["accuracy_pct"] = df["branch.accuracy"].astype(float) * 100.0
    df["state_kib"] = df["branch.predictor_state_bits"].astype(float) / 8192.0
    df = df.sort_values("accuracy_pct")
    df[[col, "accuracy_pct", "branch.mpki", "sim.cpi", "branch.predictor_state_bits"]].to_csv(
        outdir / "predictor_accuracy.csv", index=False)

    for mode, theme in THEMES.items():
        style(theme)
        fig, ax = plt.subplots(figsize=(7.4, 0.66 * len(df) + 2.4))
        ax.grid(axis="x")
        ax.grid(axis="y", visible=False)

        # One measure across four categories, so one hue. Colouring each bar
        # differently would imply an encoding that is not there.
        color = theme["series"][0]
        bars = ax.barh(df[col], df["accuracy_pct"], height=0.58, color=color, zorder=3)
        # Bars encode magnitude by length, so the baseline is zero. Cropping the
        # axis to the data range would exaggerate a 14-point spread into a
        # four-fold-looking difference.
        ax.set_xlim(0, 118)

        for bar, (_, row) in zip(bars, df.iterrows()):
            ax.text(row["accuracy_pct"] + 0.4, bar.get_y() + bar.get_height() / 2,
                    f"{row['accuracy_pct']:.2f}%   CPI {row['sim.cpi']:.3f}   "
                    f"{row['state_kib']:.1f} KiB state",
                    va="center", ha="left", fontsize=9, color=theme["ink2"])

        ax.set_xlabel("prediction accuracy")
        ax.set_xticks([0, 25, 50, 75, 100])
        ax.xaxis.set_major_formatter(FuncFormatter(lambda v, _: f"{v:g}%"))
        ax.set_ylabel("")
        finish(fig, ax, "Accuracy per bit of predictor state",
               "Higher is better; the annotation gives the storage each one costs", theme)
        written.append(save(fig, outdir, "predictor_accuracy", mode))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sweep-l1", type=Path, help="L1 size x assoc sweep CSV")
    ap.add_argument("--telemetry", type=Path, help="interval telemetry CSV")
    ap.add_argument("--cpi-stack", type=Path, help="CSV with a label column plus topdown.*_pct")
    ap.add_argument("--predictors", type=Path, help="predictor sweep CSV")
    ap.add_argument("--outdir", type=Path, default=Path("analysis/figures"))
    args = ap.parse_args()

    jobs = [
        ("miss_rate_vs_size", args.sweep_l1, fig_miss_rate_vs_size),
        ("mpki_over_time", args.telemetry, fig_mpki_over_time),
        ("cpi_stack", args.cpi_stack, fig_cpi_stack),
        ("predictor_accuracy", args.predictors, fig_predictor_accuracy),
    ]
    if all(path is None for _, path, _ in jobs):
        print("error: give at least one input CSV", file=sys.stderr)
        return 2

    written: list[Path] = []
    for name, path, fn in jobs:
        if path is None:
            continue
        if not path.exists():
            print(f"  skip {name}: {path} not found", file=sys.stderr)
            continue
        print(f"rendering {name} from {path}", file=sys.stderr)
        fn(path, args.outdir, written)

    if not written:
        print("no figures produced", file=sys.stderr)
        return 1
    print(f"\nwrote {len(written)} figures to {args.outdir}:", file=sys.stderr)
    for p in written:
        print(f"  {p}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
