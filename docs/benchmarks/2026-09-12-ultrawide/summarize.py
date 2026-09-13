"""Recalculate this session's statistics; optionally redraw its SVG with Matplotlib."""
import argparse
import csv
import json
from pathlib import Path
import statistics

ROOT = Path(__file__).resolve().parent


def read(name):
    with (ROOT / name).open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def game_summary(rows):
    seconds = sum(float(row["interval_seconds"]) for row in rows)
    swaps = sum(int(row["swap_delta"]) for row in rows)
    return {
        "samples": len(rows),
        "observed_seconds": seconds,
        "completed_swaps": swaps,
        "average_game_swap_fps": swaps / seconds,
        "minimum_sample_window_fps": min(float(row["game_fps"]) for row in rows),
        "maximum_sample_window_fps": max(float(row["game_fps"]) for row in rows),
        "average_cpu_percent_total_capacity": sum(
            float(row["cpu_percent_total_capacity"]) * float(row["interval_seconds"])
            for row in rows
        ) / seconds,
        "peak_cpu_percent_total_capacity": max(float(row["cpu_percent_total_capacity"]) for row in rows),
        "peak_working_set_mib": max(float(row["working_set_mib"]) for row in rows),
        "peak_private_memory_mib": max(float(row["private_memory_mib"]) for row in rows),
    }


def plot(game, gpu):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.ticker import MultipleLocator

    plt.rcParams.update({
        "font.family": "DejaVu Sans", "font.size": 10,
        "svg.fonttype": "none", "svg.hashsalt": "boltsrebuilt-20260912",
        "path.simplify": False,
        "axes.spines.top": False, "axes.spines.right": False,
    })
    fig, (fps_ax, gpu_ax) = plt.subplots(
        2, 1, figsize=(11, 6.2), sharex=True, gridspec_kw={"height_ratios": [2, 1]},
    )
    fig.subplots_adjust(top=0.82, bottom=0.14, left=0.09, right=0.97, hspace=0.23)
    fig.text(0.09, 0.955, "BoltsRebuilt | Partial development playtest", fontsize=17, weight="bold")
    fig.text(0.09, 0.909, "12 September 2026  ·  Native 5120 × 1440  ·  Core Ultra 9 285K / RTX 5070 Ti", color="#444444")
    times = [float(row["elapsed_seconds"]) / 60 for row in game]
    fps_ax.plot(times, [float(row["game_fps"]) for row in game], color="#245e94", linewidth=0.85,
                label="Game swap cadence (~1 s samples)")
    mean = game_summary(game)["average_game_swap_fps"]
    fps_ax.axhline(mean, color="#8b4b22", linewidth=1.2, linestyle="--", label=f"Whole-session mean: {mean:.2f} FPS")
    # Focus is sampled at each interval endpoint; shading does not identify gameplay.
    start = None
    label = "Sampled out of focus"
    for row in game:
        end = float(row["elapsed_seconds"]) / 60
        left = end - float(row["interval_seconds"]) / 60
        if row["foreground"] == "0" and start is None:
            start = left
        if row["foreground"] == "1" and start is not None:
            fps_ax.axvspan(start, left, color="#dddddd", alpha=0.65, linewidth=0, label=label, zorder=0)
            start, label = None, None
    if start is not None:
        fps_ax.axvspan(start, times[-1], color="#dddddd", alpha=0.65, linewidth=0, label=label, zorder=0)
    fps_ax.set_ylim(0, 240)
    fps_ax.set_ylabel("Game swaps / second")
    fps_ax.legend(loc="upper right", frameon=False, fontsize=9)
    gpu_times = [float(row["elapsed_seconds"]) / 60 for row in gpu]
    gpu_ax.plot(gpu_times, [float(row["gpu_utilization_percent"]) for row in gpu], color="#30775c", linewidth=1,
                marker=".", markersize=2.2)
    gpu_ax.axvspan(0, gpu_times[0], color="#f0f0f0", linewidth=0)
    gpu_ax.text(gpu_times[0] / 2, 52, "No GPU\nsamples", ha="center", va="center", fontsize=9, color="#666666")
    gpu_ax.set_ylim(0, 105)
    gpu_ax.set_yticks([0, 50, 100])
    gpu_ax.set_ylabel("Whole GPU use (%)")
    gpu_ax.set_xlabel("Minutes since game telemetry baseline")
    gpu_ax.set_xlim(0, times[-1])
    gpu_ax.xaxis.set_major_locator(MultipleLocator(2))
    for ax in (fps_ax, gpu_ax):
        ax.grid(axis="y", color="#e4e4e4", linewidth=0.6)
        ax.set_axisbelow(True)
    fig.text(0.09, 0.035, "Includes startup, menus, loading and focus changes. No per-frame timings or mission labels. GPU samples ~5 s apart.",
             fontsize=9, color="#555555")
    output = ROOT / "timeline.svg"
    fig.savefig(output, metadata={"Date": None, "Creator": "Matplotlib"})
    plt.close(fig)
    output.write_text("\n".join(line.rstrip() for line in output.read_text(encoding="utf-8").splitlines()) + "\n",
                      encoding="utf-8", newline="\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plot", action="store_true", help="Regenerate timeline.svg (requires Matplotlib).")
    args = parser.parse_args()
    game, gpu = read("game-samples.csv"), read("gpu-samples.csv")
    fields = ("gpu_utilization_percent", "memory_used_mib", "temperature_c", "power_w", "graphics_clock_mhz")
    summary = {
        "all_game_samples": game_summary(game),
        "sampled_foreground": game_summary([row for row in game if row["foreground"] == "1"]),
        "gpu_samples": len(gpu),
        "gpu_first_elapsed_seconds": float(gpu[0]["elapsed_seconds"]),
        "gpu_last_elapsed_seconds": float(gpu[-1]["elapsed_seconds"]),
        "gpu": {field: {
            "sample_mean": statistics.mean(float(row[field]) for row in gpu),
            "sample_minimum": min(float(row[field]) for row in gpu),
            "sample_maximum": max(float(row[field]) for row in gpu),
        } for field in fields},
    }
    print(json.dumps(summary, indent=2))
    if args.plot:
        plot(game, gpu)


if __name__ == "__main__":
    main()
