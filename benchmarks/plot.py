#!/usr/bin/env python3
"""Plot complete per-frame MangoHud logs from an explicit comparison manifest."""

import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import statistics


def read_frames(path):
    with Path(path).open(newline="") as stream:
        rows = list(csv.reader(stream))
    header_index = next((i for i, row in enumerate(rows[:40])
                         if "frametime" in row or "frametime_ms" in row), None)
    if header_index is None:
        raise ValueError(f"{path}: no per-frame CSV header")
    header = rows[header_index]
    ft_key = "frametime_ms" if "frametime_ms" in header else "frametime"
    time_key = "elapsed_ns" if "elapsed_ns" in header else "elapsed"
    if time_key not in header:
        raise ValueError(f"{path}: missing nanosecond elapsed column")
    frames, elapsed = [], []
    for row in rows[header_index + 1:]:
        if not row:
            continue
        if len(row) != len(header):
            raise ValueError(f"{path}: incomplete CSV row")
        item = dict(zip(header, row))
        frames.append(float(item[ft_key]))
        elapsed.append(int(item[time_key]))
    if len(frames) < 2 or any(not math.isfinite(x) or x <= 0 for x in frames):
        raise ValueError(f"{path}: invalid frame intervals")
    if any(b <= a for a, b in zip(elapsed, elapsed[1:])):
        raise ValueError(f"{path}: elapsed timestamps are not increasing")
    span = (elapsed[-1] - elapsed[0]) / 1e9
    interval_span = sum(frames[1:]) / 1000
    if abs(span - interval_span) > max(0.01, span * 0.01):
        raise ValueError(f"{path}: timestamps disagree with per-frame intervals; "
                         "check log_interval=0 and elapsed units")
    return frames, [(x - elapsed[0]) / 1e9 for x in elapsed]


def percentile(values, fraction):
    ordered = sorted(values)
    index = (len(ordered) - 1) * fraction
    low = math.floor(index)
    return ordered[low] + (ordered[math.ceil(index)] - ordered[low]) * (index - low)


def metrics(frames):
    seconds = sum(frames) / 1000
    return {
        "frames": len(frames), "duration_s": seconds,
        "average_fps": len(frames) / seconds,
        **{f"p{p:g}_ms": percentile(frames, p / 100) for p in (50, 95, 99, 99.9)},
        "max_ms": max(frames),
        "over_25_ms": sum(x > 25 for x in frames),
        "over_33_333_ms": sum(x > 100 / 3 for x in frames),
        "over_50_ms": sum(x > 50 for x in frames),
        "over_33_333_ms_per_minute": sum(x > 100 / 3 for x in frames) * 60 / seconds,
    }


def render(manifest_path, output):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    manifest_path = Path(manifest_path)
    manifest = json.loads(manifest_path.read_text())
    runs = manifest["runs"]
    groups = list(dict.fromkeys(run["group"] for run in runs))
    if len(groups) != 2 or not runs:
        raise ValueError("A comparison requires exactly two groups")
    datasets, results = [], []
    for run in runs:
        path = manifest_path.parent / run["csv"]
        frames, elapsed = read_frames(path)
        result = {**run, "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                  **metrics(frames)}
        duration = manifest.get("duration_s", 40)
        if not duration - 1 <= result["duration_s"] <= duration + 2:
            raise ValueError(f"{path}: duration {result['duration_s']:.3f}s is outside "
                             f"the expected {duration}s capture")
        datasets.append((run, frames, elapsed))
        results.append(result)
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    (output / "metrics.json").write_text(json.dumps({
        "title": manifest["title"], "notes": manifest["notes"], "runs": results,
        "method": "All frames, linear-interpolated percentiles; FPS = frame count / "
                  "sum of frame intervals. Runs are not pooled."
    }, indent=2) + "\n")
    colors = {groups[0]: "#b05a24", groups[1]: "#067a96"}
    plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 11,
                         "axes.spines.top": False, "axes.spines.right": False,
                         "svg.hashsalt": "ff7-sparse-vm-benchmarks", "path.simplify": False})
    figure, axes = plt.subplots(len(runs), 1, figsize=(12, 2.05 * len(runs) + 1.7),
                                sharex=True, sharey=True, squeeze=False)
    ymax = max(max(frames) for _, frames, _ in datasets) * 1.10
    xmax = max(elapsed[-1] for _, _, elapsed in datasets)
    for axis, (run, frames, elapsed), result in zip(axes[:, 0], datasets, results):
        axis.plot(elapsed, frames, color=colors[run["group"]], linewidth=0.65)
        axis.set_ylim(0, ymax)
        axis.set_xlim(0, xmax)
        axis.set_ylabel("Frame time (ms)")
        axis.grid(axis="y", alpha=0.2)
        axis.set_title(f"{run['label']}  ·  P99 {result['p99_ms']:.2f} ms  ·  "
                       f"maximum {result['max_ms']:.2f} ms", loc="left", fontsize=11)
    axes[-1, 0].set_xlabel("Seconds from first logged frame (runs are not camera-aligned)")
    figure.suptitle(manifest["title"], x=0.09, ha="left", fontsize=17, fontweight="bold")
    figure.text(0.09, 0.025, manifest["subtitle"] + "\nAll recorded frames; shared axes; lower frame times are better.", fontsize=10)
    figure.subplots_adjust(left=0.09, right=0.975, top=0.88, bottom=0.16, hspace=0.48)
    save(figure, output / "frame-times")
    plt.close(figure)

    figure, axis = plt.subplots(figsize=(11, 5.5))
    keys = ["p50_ms", "p95_ms", "p99_ms", "p99.9_ms"]
    for group_index, group in enumerate(groups):
        subset = [run for run in results if run["group"] == group]
        positions = [i + (group_index - 0.5) * 0.24 for i in range(len(keys))]
        medians = [statistics.median(run[key] for run in subset) for key in keys]
        lower = [medians[i] - min(run[key] for run in subset) for i, key in enumerate(keys)]
        upper = [max(run[key] for run in subset) - medians[i] for i, key in enumerate(keys)]
        axis.errorbar(positions, medians, yerr=[lower, upper], fmt="s", capsize=5,
                      color=colors[group], label=group, markersize=7)
        for run in subset:
            axis.scatter(positions, [run[key] for key in keys], color=colors[group],
                         s=20, alpha=0.65)
        for x, value in zip(positions, medians):
            axis.annotate(f"{value:.2f}", (x, value), xytext=(0, 10 + 10 * group_index),
                          textcoords="offset points", ha="center", fontsize=10,
                          color=colors[group])
    axis.set_xticks(range(4), ["Median", "95th percentile", "99th percentile", "99.9th percentile"])
    axis.set_ylim(bottom=0, top=max(run[key] for run in results for key in keys) * 1.28)
    axis.set_xlim(-0.5, 3.5)
    axis.set_ylabel("Frame time (ms) · lower is better")
    axis.grid(axis="y", alpha=0.2)
    axis.legend(loc="upper left", frameon=False)
    figure.suptitle(manifest["title"] + "\nFrame-time percentiles", x=0.09, ha="left",
                   fontsize=15, fontweight="bold")
    figure.text(0.09, 0.035, manifest["subtitle"] + "\nSquares: median across runs; whiskers: run-to-run range, not confidence intervals."
                if len(runs) > 2 else manifest["subtitle"] + "\nOne run per group; no repeatability estimate.", fontsize=10)
    figure.subplots_adjust(left=0.09, right=0.975, top=0.85, bottom=0.19)
    save(figure, output / "frame-time-percentiles")
    plt.close(figure)
    print(json.dumps({"output": str(output), "runs": results}, indent=2))


def save(figure, stem):
    svg = stem.with_suffix(".svg")
    figure.savefig(svg, facecolor="white", metadata={"Date": None})
    svg.write_text("\n".join(line.rstrip() for line in svg.read_text().splitlines()) + "\n")
    figure.savefig(stem.with_suffix(".png"), facecolor="white", dpi=160)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    render(args.manifest, args.output)
