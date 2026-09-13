#!/usr/bin/env python3
"""Summarize the plugin's CP records (nb_records/cp_records.jsonl, written by src/gpu/nb_cp_records.cpp).

Per recorded frame: draw and resolve counts, the render-target bindings in order, and every draw grouped
by pixel shader. Then a ranked list of "fullscreen candidates": draws whose viewport covers the whole
surface and whose geometry is a rectangle list or a handful of indices, with the textures they sample.
Those are the passes a native fullscreen pass can take over first (docs/plan.md, Phase 6).

Usage: python tools/cp_records_summary.py <cp_records.jsonl> [--frame N] [--json out.json]
"""

import argparse
import collections
import json
import sys

PRIM_NAMES = {1: "points", 2: "lines", 3: "linestrip", 4: "tris", 5: "trifan", 6: "tristrip",
              7: "tris_w", 8: "rectlist", 12: "lineloop", 13: "quadlist", 17: "tripatch", 18: "quadpatch"}
COLOR_FORMATS = {0: "8888", 1: "8888_GAMMA", 2: "2_10_10_10", 3: "2_10_10_10_FLOAT", 4: "16_16",
                 5: "16_16_16_16", 6: "16_16_FLOAT", 7: "16_16_16_16_FLOAT", 10: "2_10_10_10_AS_10_10_10_10",
                 12: "2_10_10_10_FLOAT_AS_16_16_16_16", 14: "32_FLOAT", 15: "32_32_FLOAT"}
DEPTH_FORMATS = {0: "D24S8", 1: "D24FS8"}


def load(path):
    frames = collections.OrderedDict()
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                continue
            frames.setdefault(rec["frame"], []).append(rec)
    return frames


def describe_rt(rb):
    color = rb["color"]
    parts = []
    for i, (base, fmt) in enumerate(color):
        if i == 0 or base != 0:
            parts.append(f"c{i}@{base}:{COLOR_FORMATS.get(fmt, fmt)}")
    depth_base, depth_fmt = rb["depth"]
    parts.append(f"d@{depth_base}:{DEPTH_FORMATS.get(depth_fmt, depth_fmt)}")
    return f"pitch {rb['pitch']} msaa {rb['msaa']} mode {rb['edram_mode']} " + " ".join(parts)


def viewport_wh(rec):
    xs, xo, ys, yo = rec["viewport"]
    return abs(xs) * 2.0, abs(ys) * 2.0


def is_fullscreen_candidate(rec, surface_w, surface_h):
    if rec["type"] != "cp_draw":
        return False
    w, h = viewport_wh(rec)
    covers = w >= surface_w * 0.98 and h >= surface_h * 0.98
    few_verts = rec["prim"] == 8 or rec["index_count"] <= 6
    return covers and few_verts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--frame", type=int, default=None, help="detail only this frame index")
    ap.add_argument("--json", default=None, help="write the per-shader summary as JSON")
    ap.add_argument("--surface", default="1280x720", help="frontbuffer size used for the fullscreen test")
    args = ap.parse_args()
    surface_w, surface_h = (int(v) for v in args.surface.lower().split("x"))

    frames = load(args.path)
    if not frames:
        sys.exit(f"no records in {args.path}")
    print(f"{len(frames)} recorded frames: {', '.join(str(k) for k in frames)}")

    per_ps = collections.defaultdict(lambda: {"draws": 0, "frames": set(), "prims": collections.Counter(),
                                              "index_counts": collections.Counter(), "rts": collections.Counter(),
                                              "tfetch": collections.Counter(), "vfetch_strides": collections.Counter(),
                                              "fullscreen": 0, "sample": None, "vs": collections.Counter()})
    for frame, recs in frames.items():
        draws = [r for r in recs if r["type"] == "cp_draw"]
        copies = [r for r in recs if r["type"] == "cp_copy"]
        if args.frame is None or args.frame == frame:
            print(f"\n=== frame {frame}: {len(draws)} draws, {len(copies)} resolves ===")
            last_rt = None
            for r in recs:
                if r["type"] == "cp_copy":
                    print(f"  [{r['seq']:3}] RESOLVE   {describe_rt(r['rb'])}")
                    continue
                if r["type"] != "cp_draw":
                    continue
                rt = describe_rt(r["rb"])
                if rt != last_rt:
                    print(f"  -- targets: {rt}")
                    last_rt = rt
                w, h = viewport_wh(r)
                tf = " ".join(f"t{t['fc']}:{t['w']}x{t['h']}/{t['format']}@{t['base']}" for t in r["tfetch"])
                flag = " FULLSCREEN" if is_fullscreen_candidate(r, surface_w, surface_h) else ""
                print(f"  [{r['seq']:3}] {PRIM_NAMES.get(r['prim'], r['prim']):9} n={r['index_count']:6} vp {w:.0f}x{h:.0f} "
                      f"vs {r['vs_ucode']} ps {r['ps_ucode']} {tf}{flag}")
        for r in draws:
            e = per_ps[r["ps_ucode"]]
            e["draws"] += 1
            e["frames"].add(frame)
            e["prims"][PRIM_NAMES.get(r["prim"], r["prim"])] += 1
            e["index_counts"][r["index_count"]] += 1
            e["rts"][describe_rt(r["rb"])] += 1
            e["vs"][r["vs_ucode"]] += 1
            for t in r["tfetch"]:
                e["tfetch"][f"{t['w']}x{t['h']}/{t['format']}"] += 1
            for v in r["vfetch"]:
                e["vfetch_strides"][v["stride_words"]] += 1
            if is_fullscreen_candidate(r, surface_w, surface_h):
                e["fullscreen"] += 1
                if e["sample"] is None:
                    e["sample"] = r

    print("\n=== fullscreen candidates (pixel shader, draws/frame, geometry, textures) ===")
    ranked = sorted(((k, v) for k, v in per_ps.items() if v["fullscreen"]),
                    key=lambda kv: (-kv[1]["fullscreen"] / max(1, len(kv[1]["frames"])), kv[0]))
    for ps, e in ranked:
        per_frame = e["fullscreen"] / max(1, len(e["frames"]))
        print(f"  ps {ps}: {per_frame:.1f} fullscreen draws/frame over {len(e['frames'])} frames; "
              f"prims {dict(e['prims'])}; textures {dict(e['tfetch'])}; vs {list(e['vs'])[:2]}")
        s = e["sample"]
        print(f"      sample: frame {s['frame']} seq {s['seq']} {describe_rt(s['rb'])} viewport {s['viewport']}")

    print("\n=== all pixel shaders ===")
    for ps, e in sorted(per_ps.items(), key=lambda kv: -kv[1]["draws"]):
        print(f"  ps {ps}: {e['draws']} draws in {len(e['frames'])} frames; prims {dict(e['prims'])}; "
              f"textures {dict(e['tfetch'])}; vfetch strides {dict(e['vfetch_strides'])}")

    if args.json:
        out = {ps: {"draws": e["draws"], "frames": sorted(e["frames"]), "prims": dict(e["prims"]),
                    "index_counts": {str(k): v for k, v in e["index_counts"].items()}, "rts": dict(e["rts"]),
                    "tfetch": dict(e["tfetch"]), "fullscreen": e["fullscreen"], "vs": dict(e["vs"])}
               for ps, e in per_ps.items()}
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(out, f, indent=1)
        print(f"\nwrote {args.json}")


if __name__ == "__main__":
    main()
