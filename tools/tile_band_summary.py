#!/usr/bin/env python3
"""Summarize what the repeated tile bands cost, from the Phase 3 counter file.

Reads the JSON lines rexgpu-nb writes when nb_phase3_counters_file is set
(src/gpu/vendored/include/rex/graphics/phase3_counters.h). Only frames that
entered a tile pass are summarized. Prints per-frame medians and which route
to stopping the double submission the numbers leave open:

- the guest predicates its draws and asks for screen extents: the SDK answers
  every extent query with the full screen, so honest extents would let the
  existing predication skip band-confined draws before any host work;
- the guest does not predicate its draws: every draw is replayed in every band
  unconditionally, and only single-band rendering or a host-side skip remains.

Usage: python tools/tile_band_summary.py <phase3_counters.jsonl>
"""

import json
import statistics
import sys

FIELDS = ("tile_pass_ordinal_max", "tiled_draws", "duplicated_draws_across_passes",
          "tiled_draw_indices", "repeat_pass_draw_indices", "predicated_packets",
          "predicated_packets_skipped", "predicated_draws", "predicated_draws_skipped",
          "screen_extent_queries", "cond_writes")


def load(lines):
    frames = []
    for line in lines:
        line = line.strip()
        if not line:
            continue
        record = json.loads(line)
        if record.get("tile_pass_ordinal_max", 0) > 0:
            frames.append(record)
    return frames


def summarize(frames):
    if not frames:
        return {"frames": 0, "verdict": "no tiled frames: was the run at a tiling resolution?"}
    medians = {k: statistics.median(f.get(k, 0) for f in frames) for k in FIELDS}
    tiled = medians["tiled_draws"]
    repeat = medians["duplicated_draws_across_passes"]
    first = tiled - repeat
    out = {"frames": len(frames), **medians}
    out["repeat_share_of_tiled_draws"] = repeat / tiled if tiled else 0.0
    out["repeat_share_of_tiled_indices"] = (
        medians["repeat_pass_draw_indices"] / medians["tiled_draw_indices"]
        if medians["tiled_draw_indices"] else 0.0)
    # The pass model holds when each later band repeats the first one's draws.
    passes = medians["tile_pass_ordinal_max"]
    out["pass_model_holds"] = bool(first > 0 and passes >= 2 and
                                   abs(repeat - first * (passes - 1)) <= 0.1 * first * (passes - 1))
    if medians["predicated_draws"] > 0 and medians["screen_extent_queries"] > 0:
        out["verdict"] = ("guest predicates draws on screen extents: honest extents from "
                          "EVENT_WRITE_EXT are the cheapest skip")
    elif medians["predicated_draws"] > 0:
        out["verdict"] = ("guest predicates draws but asks for no extents: read its bin masks "
                          "before building anything")
    else:
        out["verdict"] = ("guest replays every draw in every band unconditionally: only "
                          "single-band rendering or a host-side skip can remove the repeat")
    return out


def main(argv):
    if len(argv) != 2:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    with open(argv[1], encoding="utf-8") as f:
        result = summarize(load(f))
    for key, value in result.items():
        if isinstance(value, float):
            value = f"{value:.3f}"
        print(f"{key}: {value}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
