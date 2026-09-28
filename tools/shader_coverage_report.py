#!/usr/bin/env python3
"""Weight native shader refusals by the draws they cost.

ucode2hlsl.py's histogram counts shader stages, but one refused vertex shader can cost thousands of draws a
frame while fifty refused pixel shaders cost none. This joins the translator's census with the draws a
logged frame could not find in the native library, so the refusal worth implementing next is the one that
costs the most draws, not the one that is most common in the dump.

Inputs:
  census   ucode2hlsl.py <dump_dir> -o <scratch_dir> --census census.csv
  log      a run with nb_log_frame = <frame>, which logs that frame's native draws ("frameseq ... pair")
           and every draw whose shader pair is missing from the library ("native library missing").
  library  optional: the installed native_shaders directory. Without it, a stage the census translated is
           assumed installed, and only a draw with no refused stage is put down to a stale library.

Usage:
  shader_coverage_report.py --census census.csv [--library <native_shaders>] <log> [<log> ...]

Nothing game-derived goes in the output beyond shader hashes and counts.
"""

import argparse
import collections
import csv
import os
import re
import sys

MISSING_RE = re.compile(r"rexgpu-nb: native library missing at frame (\d+): vs ([0-9A-Fa-f]{16}) "
                        r"ps ([0-9A-Fa-f]{16}), prim (\d+) x(\d+)")
NATIVE_RE = re.compile(r"rexgpu-nb: frameseq \d+ pair ([0-9A-Fa-f]{16})_([0-9A-Fa-f]{16}) prim (\d+) x(\d+)")
NO_PIXEL_SHADER = "0" * 16

NOT_DUMPED = "not in the shader dump"
NOT_INSTALLED = "translated, not in the library"


def read_census(path):
    """{(stage, HASH): (status, category)} from ucode2hlsl.py --census."""
    census = {}
    with open(path, encoding="utf-8", newline="") as f:
        for row in csv.DictReader(f):
            census[(row["stage"], row["hash"].upper())] = (row["status"], row["category"])
    return census


def read_library(path):
    """{(stage, HASH)} for every installed stage sidecar."""
    stages = set()
    for name in os.listdir(path):
        m = re.match(r"(vs|ps)_([0-9A-Fa-f]{16})\.meta$", name)
        if m:
            stages.add((m.group(1), m.group(2).upper()))
    return stages


def read_draws(paths):
    """(missing, native): lists of (vs, ps, indices) from the logged frame(s)."""
    missing, native = [], []
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = MISSING_RE.search(line)
                if m:
                    missing.append((m.group(2).upper(), m.group(3).upper(), int(m.group(5))))
                    continue
                m = NATIVE_RE.search(line)
                if m:
                    native.append((m.group(1).upper(), m.group(2).upper(), int(m.group(4))))
    return missing, native


def stage_cause(stage, shader_hash, census, library):
    """Why one stage is absent from the library, or None when it is present."""
    if library is not None and (stage, shader_hash) in library:
        return None
    entry = census.get((stage, shader_hash))
    if entry is None:
        return NOT_DUMPED
    status, category = entry
    if status == "ok":
        return NOT_INSTALLED if library is not None else None
    if status == "unsupported":
        return category
    if status == "compile_failed":
        return "fxc"
    return f"translator error {category}"


def draw_causes(vs, ps, census, library):
    """The stages that keep one missing draw emulated, as {(stage, hash): cause}."""
    causes = {}
    for stage, shader_hash in (("vs", vs), ("ps", ps)):
        if stage == "ps" and shader_hash == NO_PIXEL_SHADER:
            continue
        cause = stage_cause(stage, shader_hash, census, library)
        if cause is not None:
            causes[(stage, shader_hash)] = cause
    if not causes:
        # Every stage translated and none is known missing: the library predates the dump or filters it.
        causes[("vs", vs)] = NOT_INSTALLED
    return causes


def build_report(missing, native, census, library):
    category_draws = collections.Counter()      # draws a category blocks, alone or with another
    category_indices = collections.Counter()
    category_sole = collections.Counter()       # draws that go native once this category alone is lifted
    category_sole_indices = collections.Counter()
    category_stages = collections.defaultdict(set)
    stage_draws = collections.Counter()
    stage_indices = collections.Counter()
    stage_pairs = collections.defaultdict(set)
    stage_category = {}
    for vs, ps, indices in missing:
        causes = draw_causes(vs, ps, census, library)
        keys = {f"{stage}: {cause}" for (stage, _), cause in causes.items()}
        for key in keys:
            category_draws[key] += 1
            category_indices[key] += indices
        if len(keys) == 1:
            key = next(iter(keys))
            category_sole[key] += 1
            category_sole_indices[key] += indices
        for (stage, shader_hash), cause in causes.items():
            category_stages[f"{stage}: {cause}"].add(shader_hash)
            stage_draws[(stage, shader_hash)] += 1
            stage_indices[(stage, shader_hash)] += indices
            stage_pairs[(stage, shader_hash)].add((vs, ps))
            stage_category[(stage, shader_hash)] = cause

    lines = []
    total_missing = len(missing)
    total_native = len(native)
    lines.append(f"logged draws: {total_native} native, {total_missing} missing from the library "
                 f"({sum(i for *_, i in native)} / {sum(i for *_, i in missing)} indices)")
    if total_native + total_missing:
        lines.append(f"missing share of native-or-missing draws: "
                     f"{100.0 * total_missing / (total_native + total_missing):.1f}%")
    lines.append("")
    lines.append("By refusal category (a draw with two refused stages counts under both):")
    lines.append(f"  {'draws':>7} {'indices':>10} {'alone':>7} {'alone idx':>10} {'stages':>6}  category")
    for key, draws in sorted(category_draws.items(), key=lambda kv: (-kv[1], kv[0])):
        lines.append(f"  {draws:7} {category_indices[key]:10} {category_sole[key]:7} "
                     f"{category_sole_indices[key]:10} {len(category_stages[key]):6}  {key}")
    lines.append("  ('alone' = draws that go native once that category alone is implemented)")
    lines.append("")
    lines.append("Stages costing the most draws:")
    lines.append(f"  {'draws':>7} {'indices':>10} {'pairs':>5}  stage hash               cause")
    ranked = sorted(stage_draws.items(), key=lambda kv: (-kv[1], kv[0]))
    for (stage, shader_hash), draws in ranked[:25]:
        lines.append(f"  {draws:7} {stage_indices[(stage, shader_hash)]:10} "
                     f"{len(stage_pairs[(stage, shader_hash)]):5}  {stage}_{shader_hash}  "
                     f"{stage_category[(stage, shader_hash)]}")
    return "\n".join(lines) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--census", required=True, help="ucode2hlsl.py --census output")
    ap.add_argument("--library", default=None, help="installed native_shaders directory (optional)")
    ap.add_argument("logs", nargs="+", help="run logs covering an nb_log_frame frame")
    args = ap.parse_args(argv)
    census = read_census(args.census)
    library = read_library(args.library) if args.library else None
    missing, native = read_draws(args.logs)
    if not missing and not native:
        print("No logged frame found: set nb_log_frame to a gameplay frame number and rerun.", file=sys.stderr)
        return 1
    sys.stdout.write(build_report(missing, native, census, library))
    return 0


if __name__ == "__main__":
    sys.exit(main())
