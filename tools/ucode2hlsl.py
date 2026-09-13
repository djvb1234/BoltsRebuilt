#!/usr/bin/env python3
"""Translate individual SDK ucode disassemblies into HLSL for the nb native geometry pass.

Input: the SDK's `dump_shaders` text files (shader_<hash>.ucode.vert / .frag). Output: vs_<hash>.hlsl
or ps_<hash>.hlsl, each with a .meta sidecar describing texture slots and dimensions, vertex streams,
constant ranges and stage properties. NativeShaderLibrary combines stages at runtime and prepends
src/gpu/native/shaders/prelude.hlsl. Binary dump variants and cp_records inventories are not inputs.

Semantics follow the SDK's shader interpreter (src/graphics/pipeline/shader/interpreter.cpp): every ALU
opcode, the predicate model (setp_* set p0, (p0)/(!p0) prefixes), bool-constant control flow (cexec,
cjmp to a forward label), vertex formats and texture fetch options seen in this game's corpus. Loops,
unsupported texture options and relative loop addressing raise NotSupported so the pass stays
emulated. Basic 3D instructions use the runtime's bounded stacked-texture path; cube ALU and fetches
follow the SDK's face-space conversion. Texture sidecars use dimensions 2 (2D), 3 (stacked), 4 (cube).

Usage:
  ucode2hlsl.py <dump_dir> -o <out_dir> [--only <shader_hash>] [--fxc <path>]

The optional fxc check compiles each generated stage. Use gen_native_shaders.ps1 for machine locking,
staging, checking changed stages before installation, and retaining existing library files.
"""

import argparse
import collections
import json
import math
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PRELUDE = os.path.join(HERE, "..", "src", "gpu", "native", "shaders", "prelude.hlsl")


class NotSupported(Exception):
    pass


# ---------------------------------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------------------------------

ADDR_RE = re.compile(r"^\s*/\*\s*([0-9.]+)\s*\*/\s*(.*)$")
PRED_RE = re.compile(r"^\((!?)p0\)\s*(.*)$")
OPT_RE = re.compile(r"^([A-Za-z]+)=(.+)$")


class Instr:
    __slots__ = ("op", "sat", "dest", "srcs", "opts", "pred", "coissued")

    def __init__(self, op, dest, srcs, opts, pred, coissued):
        self.sat = op.endswith("_sat") and op not in ("kills_sat",)
        self.op = op[:-4] if self.sat else op
        self.dest = dest
        self.srcs = srcs
        self.opts = opts
        self.pred = pred          # None, "p0" or "!p0"
        self.coissued = coissued  # scalar half of a co-issued pair


def split_operands(text):
    parts, depth, cur = [], 0, ""
    for ch in text:
        if ch == "[":
            depth += 1
        elif ch == "]":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        parts.append(cur.strip())
    return parts


def parse_instruction(text, coissued):
    pred = None
    m = PRED_RE.match(text)
    if m:
        pred = "!p0" if m.group(1) else "p0"
        text = m.group(2)
    text = text.split("//")[0].strip()
    if not text:
        return None
    op, _, rest = text.partition(" ")
    ops = split_operands(rest) if rest.strip() else []
    opts = {}
    plain = []
    for o in ops:
        m = OPT_RE.match(o)
        if m and op.startswith(("vfetch", "tfetch", "getCompTexLOD", "setTexLOD")):
            opts[m.group(1)] = m.group(2)
        else:
            plain.append(o)
    dest = plain[0] if plain else None
    return Instr(op, dest, plain[1:], opts, pred, coissued)


CF_OPS = {"exec", "exece", "cexec", "cexece", "cjmp", "jmp", "label", "alloc", "cnop", "nop",
          "loop_start", "loop_end", "call", "ret", "exec_end"}


def parse_listing(text):
    """Returns a list of blocks: {"kind":..., "cond":..., "target":..., "instrs":[Instr]}."""
    blocks = []
    pending_serialize = False
    pending_coissue = False
    cur = None
    for raw in text.splitlines():
        line = raw.rstrip()
        if not line.strip():
            continue
        m = ADDR_RE.match(line)
        body = m.group(2) if m else line.strip()
        body = body.strip()
        if body.startswith("+"):
            pending_coissue = True
            body = body[1:].strip()
        if not body:
            continue
        # A control-flow instruction can itself be predicated: "(p0) exec" runs the block only when p0
        # holds at block entry; "(!p0) jmp" likewise.
        block_pred = None
        pm = PRED_RE.match(body)
        head = (pm.group(2) if pm else body).split()[0] if (pm.group(2) if pm else body).split() else ""
        if pm and head in CF_OPS:
            block_pred = "!p0" if pm.group(1) else "p0"
            body = pm.group(2)
        if head == "serialize":
            pending_serialize = True
            continue
        if head in CF_OPS or head.startswith("alloc"):
            cf = body.split("//")[0].strip()
            toks = cf.replace(",", " ").split()
            kind = toks[0]
            if kind in ("alloc", "cnop", "nop"):
                continue
            if kind in ("loop_start", "loop_end", "call", "ret"):
                raise NotSupported(f"control flow {kind}")
            blk = {"kind": kind, "cond": None, "target": None, "instrs": [], "pred": block_pred}
            if kind in ("cexec", "cexece"):
                blk["cond"] = toks[1]
            elif kind == "cjmp":
                blk["cond"] = toks[1]
                blk["target"] = toks[2]
            elif kind == "jmp":
                blk["target"] = toks[1]
            elif kind == "label":
                blk["target"] = toks[1]
            blocks.append(blk)
            cur = blk
            pending_coissue = False
            continue
        ins = parse_instruction(body, pending_coissue)
        pending_coissue = False
        pending_serialize = False
        if ins is None:
            continue
        if cur is None:
            cur = {"kind": "exec", "cond": None, "target": None, "instrs": [], "pred": None}
            blocks.append(cur)
        cur["instrs"].append(ins)
    return blocks


# ---------------------------------------------------------------------------------------------------
# Operands
# ---------------------------------------------------------------------------------------------------

REG_RE = re.compile(r"^(-?)(r_abs\[(\d+)\]|r(\d+)|c_abs\[(\d+)\]|c\[(\d+)\+a0\]|c(\d+)|o(\d+)|oPos|oPts|oC(\d)|oDepth)(?:\.([xyzw01_]+))?$")


def legacy_mul(a, b):
    """Xenos multiplies by the Shader Model 3 rule: a product with a zero or denormal multiplicand is +0
    whatever the other side holds, infinities included, where IEEE would hand back a NaN. Identical
    operands need no guard - 0 * 0 is +0 either way - and skipping them keeps the common squaring cheap."""
    if a == b:
        return f"({a} * {b})"
    return f"MulLegacy({a}, {b})"


def legacy_dot(a, b, width):
    """dot() with each term under the legacy multiply rule, summed left to right so a +0 term stays +0."""
    if a == b:
        return f"dot({a}, {b})"
    return f"DotLegacy{width}({a}, {b})"


def select_max(a, b):
    """max on this hardware is the SM3 select (a >= b) ? a : b, so max(a, NaN) is NaN - HLSL's max drops
    the NaN instead. Identical operands make the two forms agree, which covers the mov-by-max idiom."""
    return f"max({a}, {b})" if a == b else f"MaxLegacy({a}, {b})"


def select_min(a, b):
    return f"min({a}, {b})" if a == b else f"MinLegacy({a}, {b})"


class Translator:
    def __init__(self, stage, blocks, interpolator_count, const_map=None):
        self.stage = stage  # "vs" or "ps"
        # Absolute guest constant register -> packed register, or None on the collecting pass.
        self.const_map = const_map
        self.blocks = blocks
        self.interp_count = interpolator_count
        # Root-constant texture slots are split by stage so the two shaders translate independently:
        # a pixel shader's fetch constants land in slots 0..15, a vertex shader's in 16..19.
        self.slot_base = 16 if stage == "vs" else 0
        self.slot_limit = 4 if stage == "vs" else 16
        self.tex_slots = []               # fetch constants in slot order
        self.tex_dimensions = []          # instruction dimensions, parallel to tex_slots
        self.lines = []
        self.regs_used = set()
        self.exports = set()
        self.streams = []                 # vfN in order of first use
        self.last_vfetch = None           # (stream_slot, index_expr)
        self.uses_a0 = False
        self.max_const = -1               # highest cN read; 255 once relative addressing is used
        self.const_used = set()           # constant registers read outright
        self.const_all = False            # relative addressing: any register can be read
        self.uses_bool = False
        self.kills = False
        self.writes_depth = False
        self.color_outputs = set()
        self.jump_flags = []
        self.vfetch_addrs = []            # stream address temps, hoisted so a mini can outlive its full's scope
        self.writes_kill = False          # vertex shader writes oPts.z (vertex kill)
        self.writes_point_size = False    # vertex shader writes oPts.x (point sprite diameter)
        self.tmp = 0

    # --- helpers -----------------------------------------------------------------------------------
    def const(self, n_expr):
        n_text = str(n_expr)
        if n_text.isdigit():
            index = int(n_text)
            self.max_const = max(self.max_const, index)
            self.const_used.add(index)
            if self.const_map is not None:
                n_expr = self.const_map[index]
        else:
            self.max_const = 255
            self.const_all = True
        return f"{self.stage}_c[{n_expr}]"

    def tex_slot(self, fc, dimension=2):
        for index, key in enumerate(zip(self.tex_slots, self.tex_dimensions)):
            if key == (fc, dimension):
                return self.slot_base + index
        if len(self.tex_slots) >= self.slot_limit:
            raise NotSupported(f"more than {self.slot_limit} texture fetch constants in one {self.stage}")
        self.tex_slots.append(fc)
        self.tex_dimensions.append(dimension)
        return self.slot_base + len(self.tex_slots) - 1

    def stream_slot(self, vf, stride):
        if not 0 <= vf < 96 or not 0 <= stride <= 255:
            raise NotSupported("vertex stream fetch constant or stride outside SDK bounds")
        for i, s in enumerate(self.streams):
            if s["vf"] == vf:
                if s["stride"] != stride:
                    raise NotSupported(f"stream vf{vf} fetched with two strides")
                return i
        if len(self.streams) >= 16:
            raise NotSupported("more than 16 vertex streams")
        self.streams.append({"vf": vf, "stride": stride})
        return len(self.streams) - 1

    def operand(self, text, want, scalar_second=None):
        """Returns the HLSL expression of an operand.
        want: 4 -> float4 (swizzle padded to 4), 1 -> float, 2/3 -> tuple of component floats."""
        m = REG_RE.match(text)
        if not m:
            raise NotSupported(f"operand {text}")
        neg = m.group(1) == "-"
        swz = m.group(10)
        if m.group(3) is not None:
            base = f"abs(r{m.group(3)})"; self.regs_used.add(int(m.group(3)))
        elif m.group(4) is not None:
            base = f"r{m.group(4)}"; self.regs_used.add(int(m.group(4)))
        elif m.group(5) is not None:
            base = f"abs({self.const(m.group(5))})"
        elif m.group(6) is not None:
            base = self.const(f"{m.group(6)} + a0"); self.uses_a0 = True
        elif m.group(7) is not None:
            base = self.const(m.group(7))
        else:
            raise NotSupported(f"export as source {text}")
        # ALU source swizzles are two bits per channel, so only x/y/z/w can appear; 0 and 1 belong to fetch
        # destinations. Anything else would emit HLSL that fxc rejects, and the pass would silently fall
        # back to emulation instead of saying why.
        if swz:
            stripped = swz.replace("_", "")
            if not stripped or any(ch not in "xyzw" for ch in stripped):
                raise NotSupported(f"source swizzle {text}")
        if want == 4:
            if not swz:
                expr = base
            else:
                s = swz.replace("_", "")
                if len(s) < 4:
                    s = s + s[-1] * (4 - len(s))
                expr = f"{base}.{s}"
        elif want == 1:
            s = (swz or "x").replace("_", "")[0]
            expr = f"{base}.{s}"
        else:  # coordinate scalars from one operand ("r0.zw", "c8.xx", "r2.xzy")
            s = (swz or "xyz"[:want]).replace("_", "")
            if len(s) < want:
                s += s[-1] * (want - len(s))
            components = tuple(f"{base}.{ch}" for ch in s[:want])
            return tuple(f"(-{c})" for c in components) if neg else components
        return f"(-{expr})" if neg else expr

    def dest_parts(self, text):
        """Returns (name, mask_letters or None). mask letters: 4 chars of x/y/z/w/0/1/_ (None = all)."""
        m = REG_RE.match(text)
        if not m or m.group(1) == "-":
            raise NotSupported(f"dest {text}")
        if m.group(4) is not None:
            name = f"r{m.group(4)}"; self.regs_used.add(int(m.group(4)))
        elif m.group(8) is not None:
            name = f"o{m.group(8)}"; self.exports.add(int(m.group(8)))
        elif text.split(".")[0] == "oPos":
            name = "oPos"
        elif text.split(".")[0] == "oPts":
            # Point size / edge flag / vertex kill export: .z != 0 kills the vertex's primitives.
            name = "oPts"
            mask = m.group(10)
            if mask is None or (len(mask) > 2 and mask[2] != "_"):
                self.writes_kill = True
            if mask is None or mask[0] != "_":
                self.writes_point_size = True
        elif m.group(9) is not None:
            name = f"oC{m.group(9)}"; self.color_outputs.add(int(m.group(9)))
        elif text.split(".")[0] == "oDepth":
            name = "oDepth"; self.writes_depth = True
        else:
            raise NotSupported(f"dest {text}")
        return name, m.group(10)

    def emit(self, s):
        self.lines.append("  " * (1 + self.depth) + s)

    def new_tmp(self, prefix="t"):
        self.tmp += 1
        return f"{prefix}{self.tmp}"

    # --- ALU ---------------------------------------------------------------------------------------
    VECTOR_OPS = {"add", "mul", "max", "min", "seq", "sgt", "sge", "sne", "frc", "trunc", "floor", "mad",
                  "cndeq", "cndge", "cndgt", "dp4", "dp3", "dp2add", "cube", "max4", "setp_eq_push",
                  "setp_ne_push", "setp_gt_push", "setp_ge_push", "kill_eq", "kill_gt", "kill_ge", "kill_ne",
                  "dst", "maxa"}
    SCALAR_OPS = {"adds", "addsc", "muls", "mulsc", "subs", "subsc", "adds_prev", "muls_prev", "subs_prev",
                  "muls_prev2", "maxs", "mins", "seqs", "sgts", "sges", "snes", "frcs", "truncs", "floors",
                  "exp", "logc", "log", "rcpc", "rcpf", "rcp", "rsqc", "rsqf", "rsq", "maxas", "maxasf",
                  "setp_eq", "setp_ne", "setp_gt", "setp_ge", "setp_inv", "setp_pop", "setp_clr", "setp_rstr",
                  "kills_eq", "kills_gt", "kills_ge", "kills_ne", "kills_one", "sqrt", "sin", "cos",
                  "retain_prev"}

    def write_masked(self, name, mask, value_expr, replicate=False, sat=False):
        """Emits writes of value_expr (float4 or scalar) into name with mask."""
        v = value_expr
        if sat:
            v = f"saturate({v})"
        if mask is None or mask == "xyzw":
            if replicate:
                self.emit(f"{name} = float4({v}.xxxx);") if not v.startswith("float(") else self.emit(f"{name} = float4({v}, {v}, {v}, {v});")
            else:
                self.emit(f"{name} = {v};")
            return
        tmp = self.new_tmp()
        if replicate:
            self.emit(f"float {tmp} = {v};")
            for i, ch in enumerate(mask):
                comp = "xyzw"[i]
                if ch == "_":
                    continue
                if ch in "01":
                    self.emit(f"{name}.{comp} = {ch}.0;")
                else:
                    self.emit(f"{name}.{comp} = {tmp};")
        else:
            self.emit(f"float4 {tmp} = {v};")
            for i, ch in enumerate(mask):
                comp = "xyzw"[i]
                if ch == "_":
                    continue
                if ch in "01":
                    self.emit(f"{name}.{comp} = {ch}.0;")
                else:
                    self.emit(f"{name}.{comp} = {tmp}.{comp};")

    def write_scalar(self, name, mask, expr):
        if mask is None or mask == "xyzw":
            self.emit(f"{name} = float4({expr}, {expr}, {expr}, {expr});")
            return
        for i, ch in enumerate(mask):
            comp = "xyzw"[i]
            if ch == "_":
                continue
            if ch in "01":
                self.emit(f"{name}.{comp} = {ch}.0;")
            else:
                self.emit(f"{name}.{comp} = {expr};")

    def vector_expr(self, ins):
        op = ins.op
        s = ins.srcs
        a = self.operand(s[0], 4) if len(s) > 0 else None
        b = self.operand(s[1], 4) if len(s) > 1 else None
        c = self.operand(s[2], 4) if len(s) > 2 else None
        rep = False
        if op == "add": e = f"({a} + {b})"
        elif op == "mul": e = legacy_mul(a, b)
        elif op == "max": e = select_max(a, b)
        elif op == "min": e = select_min(a, b)
        elif op == "seq": e = f"float4({a} == {b})"
        elif op == "sgt": e = f"float4({a} > {b})"
        elif op == "sge": e = f"float4({a} >= {b})"
        elif op == "sne": e = f"float4({a} != {b})"
        elif op == "frc": e = f"frac({a})"
        elif op == "trunc": e = f"trunc({a})"
        elif op == "floor": e = f"floor({a})"
        elif op == "mad": e = f"({legacy_mul(a, b)} + {c})"
        elif op == "cndeq": e = f"(({a} == 0.0) ? {b} : {c})"
        elif op == "cndge": e = f"(({a} >= 0.0) ? {b} : {c})"
        elif op == "cndgt": e = f"(({a} > 0.0) ? {b} : {c})"
        elif op == "dp4": e = legacy_dot(a, b, 4); rep = True
        elif op == "dp3": e = legacy_dot(f"({a}).xyz", f"({b}).xyz", 3); rep = True
        elif op == "dp2add": e = f"({legacy_dot(f'({a}).xy', f'({b}).xy', 2)} + ({c}).x)"; rep = True
        elif op == "max4":
            t = self.new_tmp("m"); self.emit(f"float4 {t} = {a};")
            e = f"max(max({t}.x, {t}.y), max({t}.z, {t}.w))"; rep = True
        elif op in ("setp_eq_push", "setp_ne_push", "setp_gt_push", "setp_ge_push"):
            cmp = {"setp_eq_push": "==", "setp_ne_push": "!=", "setp_gt_push": ">", "setp_ge_push": ">="}[op]
            ta, tb = self.new_tmp("pa"), self.new_tmp("pb")
            self.emit(f"float4 {ta} = {a}; float4 {tb} = {b};")
            self.emit(f"p0 = ({ta}.w == 0.0) && ({tb}.w {cmp} 0.0);")
            e = f"((({ta}.x == 0.0) && ({tb}.x {cmp} 0.0)) ? 0.0 : {ta}.x + 1.0)"; rep = True
        elif op in ("kill_eq", "kill_gt", "kill_ge", "kill_ne"):
            cmp = {"kill_eq": "==", "kill_gt": ">", "kill_ge": ">=", "kill_ne": "!="}[op]
            t = self.new_tmp("k"); self.emit(f"bool4 {t} = ({a} {cmp} {b});")
            if self.stage == "ps":
                self.emit(f"if (any({t})) discard;"); self.kills = True
            e = f"(any({t}) ? 1.0 : 0.0)"; rep = True
        elif op == "dst":
            ta, tb = self.new_tmp("da"), self.new_tmp("db")
            self.emit(f"float4 {ta} = {a}; float4 {tb} = {b};")
            e = f"float4(1.0, {legacy_mul(f'{ta}.y', f'{tb}.y')}, {ta}.z, {tb}.w)"
        elif op == "maxa":
            t = self.new_tmp("ma"); self.emit(f"float4 {t} = {a};")
            self.emit(f"a0 = int(floor(clamp({t}.w, -256.0, 255.0) + 0.5));"); self.uses_a0 = True
            e = select_max(t, b)
        elif op == "cube":
            if len(s) != 2:
                raise NotSupported("cube operands")
            # The SDK loads the first source only: its z/w/x lanes are direction X/Y/Z.
            # The assembler's second source is the redundant complementary swizzle.
            e = f"XenosCube({a})"
        else:
            raise NotSupported(f"vector op {op}")
        return e, rep

    def clamp_infinite(self, pre, variant, expr):
        """rcp/rsq keep IEEE infinities. The 'c' (clamped) forms hand back +/-FLT_MAX instead and the 'f'
        (fixed-function) forms +/-0, so the guest's next multiply produces a finite number where ours would
        produce a NaN. That NaN is what a black surface looks like from here."""
        if not variant:
            return expr
        t = self.new_tmp("s")
        pre.append(f"float {t} = {expr};")
        if variant == "c":
            return f"(isinf({t}) ? (({t} > 0.0) ? 3.402823466e+38 : -3.402823466e+38) : {t})"
        return f"(isinf({t}) ? (({t} > 0.0) ? 0.0 : -0.0) : {t})"

    def scalar_expr(self, ins):
        op = ins.op
        s = ins.srcs
        # operand shapes
        if op in ("adds", "muls", "subs", "maxs", "mins", "maxas", "maxasf", "muls_prev2"):
            if len(s) == 1:
                a, b = self.operand(s[0], 2)
            else:
                a, b = self.operand(s[0], 1), self.operand(s[1], 1)
        elif op in ("addsc", "mulsc", "subsc"):
            a, b = self.operand(s[0], 1), self.operand(s[1], 1)
        elif op in ("setp_clr", "retain_prev"):
            a = b = None
        else:
            a = self.operand(s[0], 1) if s else None
            b = None
        pre = []
        if op in ("adds", "addsc"): e = f"({a} + {b})"
        elif op == "adds_prev": e = f"({a} + ps)"
        elif op in ("muls", "mulsc"): e = legacy_mul(a, b)
        elif op == "muls_prev": e = legacy_mul(a, "ps")
        elif op == "muls_prev2":
            # MUL_LIT: the lit() idiom and the predicate counter. Anything unusable in ps or a
            # non-positive guard component collapses the result to the -FLT_MAX sentinel.
            e = (f"((ps == -3.402823466e+38 || isinf(ps) || isnan(ps) || isinf({b}) || isnan({b}) || "
                 f"{b} <= 0.0) ? -3.402823466e+38 : {legacy_mul(a, 'ps')})")
        elif op in ("subs", "subsc"): e = f"({a} - {b})"
        elif op == "subs_prev": e = f"({a} - ps)"
        elif op == "maxs": e = select_max(a, b)
        elif op == "mins": e = select_min(a, b)
        elif op in ("maxas", "maxasf"):
            rnd = " + 0.5" if op == "maxas" else ""
            pre.append(f"a0 = int(floor(clamp({a}, -256.0, 255.0){rnd}));"); self.uses_a0 = True
            e = select_max(a, b)
        elif op == "seqs": e = f"(({a} == 0.0) ? 1.0 : 0.0)"
        elif op == "sgts": e = f"(({a} > 0.0) ? 1.0 : 0.0)"
        elif op == "sges": e = f"(({a} >= 0.0) ? 1.0 : 0.0)"
        elif op == "snes": e = f"(({a} != 0.0) ? 1.0 : 0.0)"
        elif op == "frcs": e = f"frac({a})"
        elif op == "truncs": e = f"trunc({a})"
        elif op == "floors": e = f"floor({a})"
        elif op == "exp": e = f"exp2({a})"
        elif op == "log": e = f"log2({a})"
        elif op == "logc":
            # log2 of +0 is -inf on the host; the clamped form returns -FLT_MAX. A negative input's NaN
            # passes through either way (NaN compares false).
            t = self.new_tmp("s"); pre.append(f"float {t} = log2({a});")
            e = f"(({t} < -3.402823466e+38) ? -3.402823466e+38 : {t})"
        elif op in ("rcp", "rcpc", "rcpf"):
            e = self.clamp_infinite(pre, op[3:], f"(1.0 / {a})")
        elif op in ("rsq", "rsqc", "rsqf"):
            e = self.clamp_infinite(pre, op[3:], f"rsqrt({a})")
        elif op == "sqrt": e = f"sqrt({a})"
        elif op == "sin": e = f"sin({a})"
        elif op == "cos": e = f"cos({a})"
        elif op in ("setp_eq", "setp_ne", "setp_gt", "setp_ge"):
            cmp = {"setp_eq": "==", "setp_ne": "!=", "setp_gt": ">", "setp_ge": ">="}[op]
            pre.append(f"p0 = ({a} {cmp} 0.0);")
            e = "(p0 ? 0.0 : 1.0)"
        elif op == "setp_inv":
            t = self.new_tmp("s"); pre.append(f"float {t} = {a};")
            pre.append(f"p0 = ({t} == 1.0);")
            e = f"(p0 ? 0.0 : (({t} == 0.0) ? 1.0 : {t}))"
        elif op == "setp_pop":
            t = self.new_tmp("s"); pre.append(f"float {t} = {a} - 1.0;")
            pre.append(f"p0 = ({t} <= 0.0);")
            e = f"(p0 ? 0.0 : {t})"
        elif op == "setp_clr":
            pre.append("p0 = false;"); e = "3.402823466e+38"
        elif op == "setp_rstr":
            t = self.new_tmp("s"); pre.append(f"float {t} = {a};")
            pre.append(f"p0 = ({t} == 0.0);")
            e = f"(p0 ? 0.0 : {t})"
        elif op in ("kills_eq", "kills_gt", "kills_ge", "kills_ne", "kills_one"):
            cmp = {"kills_eq": "== 0.0", "kills_gt": "> 0.0", "kills_ge": ">= 0.0", "kills_ne": "!= 0.0",
                   "kills_one": "== 1.0"}[op]
            t = self.new_tmp("k"); pre.append(f"bool {t} = ({a} {cmp});")
            # Kills are pixel-shader only: elsewhere the hardware ignores the discard but still
            # writes the condition to the destination and to ps.
            if self.stage == "ps":
                pre.append(f"if ({t}) discard;"); self.kills = True
            e = f"({t} ? 1.0 : 0.0)"
        elif op == "retain_prev": e = "ps"
        else:
            raise NotSupported(f"scalar op {op}")
        return pre, e

    def emit_alu_pair(self, vec, sca):
        """vec and/or sca (Instr) issued together: read all operands before any write."""
        # Vector half
        vexpr = None
        if vec is not None:
            vexpr, rep = self.vector_expr(vec)
        spre, sexpr = (None, None)
        if sca is not None:
            spre, sexpr = self.scalar_expr(sca)
        # Evaluate into temporaries first (both halves read the pre-write state).
        vt = st = None
        if vec is not None:
            vt = self.new_tmp("v")
            self.emit(f"{'float' if rep else 'float4'} {vt} = {vexpr};")
        if sca is not None:
            for p in spre:
                self.emit(p)
            st = self.new_tmp("s")
            self.emit(f"float {st} = {sexpr};")
        if vec is not None:
            name, mask = self.dest_parts(vec.dest)
            val = f"saturate({vt})" if vec.sat else vt
            if rep:
                self.write_scalar(name, mask, val)
            else:
                self.write_masked(name, mask, val)
        if sca is not None:
            name, mask = self.dest_parts(sca.dest)
            val = f"saturate({st})" if sca.sat else st
            self.write_scalar(name, mask, val)
            # _sat clamps the register store only: ps keeps the raw result, so a co-issued *_prev reading
            # it sees what the hardware saw. Saturating ps too turns a negative light term into a zero one
            # and the surface goes black.
            self.emit(f"ps = {st};")

    # --- fetch -------------------------------------------------------------------------------------
    FORMAT_FETCH = {
        "FMT_32_FLOAT": ("FetchF32", 1), "FMT_32_32_FLOAT": ("FetchF32", 2), "FMT_32_32_32_FLOAT": ("FetchF32", 3),
        "FMT_32_32_32_32_FLOAT": ("FetchF32", 4), "FMT_16_16_FLOAT": ("FetchF16x2", 0),
        "FMT_16_16_16_16_FLOAT": ("FetchF16x4", 0), "FMT_2_10_10_10": ("Fetch2_10_10_10", 0),
        "FMT_8_8_8_8": ("Fetch8_8_8_8", 0), "FMT_16_16": ("Fetch16_16", 0), "FMT_16_16_16_16": ("Fetch16_16_16_16", 0),
        "FMT_11_11_10": ("Fetch11_11_10", 0), "FMT_10_11_11": ("Fetch10_11_11", 0),
        "FMT_32": ("Fetch32x4Int", 1), "FMT_32_32": ("Fetch32x4Int", 2), "FMT_32_32_32_32": ("Fetch32x4Int", 4),
    }

    def emit_vfetch(self, ins):
        fmt = ins.opts.get("DataFormat")
        if fmt not in self.FORMAT_FETCH:
            raise NotSupported(f"vertex format {fmt}")
        fn, count = self.FORMAT_FETCH[fmt]
        signed = "true" if ins.opts.get("Signed", "false") == "true" else "false"
        normalized = "false" if ins.opts.get("NumFormat", "") == "integer" else "true"
        offset = int(ins.opts.get("Offset", "0"))
        if ins.op == "vfetch_full":
            if len(ins.srcs) < 2:
                raise NotSupported("vfetch_full operands")
            idx_expr = self.operand(ins.srcs[0], 1)
            # The SDK's disassembler prints vf{95 - fetch_constant} (translator_disasm.cpp), so undo that.
            vf = 95 - int(ins.srcs[1][2:])
            # Stride 0 is legal: every vertex then reads the same address, which StreamAddress already does.
            stride = int(ins.opts.get("Stride", "0"))
            stream = self.stream_slot(vf, stride)
            idx_tmp = self.new_tmp("vi")
            # The index floors unless the instruction asks for rounding, which the disassembler prints
            # only when the bit is set.
            rounded = " + 0.5" if ins.opts.get("RoundIndex", "false") == "true" else ""
            self.emit(f"uint {idx_tmp} = uint(floor({idx_expr}{rounded}));")
            base_tmp = self.new_tmp("va")
            # Declared at function scope: a mini after the end of the full's block still reads it, and a
            # full that was skipped leaves the stale address the hardware would have left.
            self.vfetch_addrs.append(base_tmp)
            self.emit(f"{base_tmp} = StreamAddress({stream}u, {idx_tmp});")
            self.last_vfetch = (stream, base_tmp, stride)
        else:
            if self.last_vfetch is None:
                raise NotSupported("vfetch_mini without vfetch_full")
            stream, base_tmp, stride = self.last_vfetch
        addr = f"({base_tmp} + {offset * 4}u)" if offset >= 0 else f"({base_tmp} - {-offset * 4}u)"
        endian = f"StreamEndian({stream}u)"
        if fn == "FetchF32":
            call = f"FetchF32({addr}, {endian}, {count}u, {stream}u)"
        elif fn in ("FetchF16x2", "FetchF16x4"):
            call = f"{fn}({addr}, {endian}, {stream}u)"
        elif fn == "Fetch32x4Int":
            call = f"Fetch32x4Int({addr}, {endian}, {count}u, {signed}, {normalized}, {stream}u)"
        else:
            call = f"{fn}({addr}, {endian}, {signed}, {normalized}, {stream}u)"
        # ExpAdjust scales every decoded component by 2^N. repr() of a power of two is exact.
        exp_adjust = int(ins.opts.get("ExpAdjust", "0"))
        if exp_adjust:
            call = f"({call} * {2.0 ** exp_adjust!r})"
        self.apply_fetch_dest(ins.dest, call)

    def apply_fetch_dest(self, dest, call):
        name, mask = self.dest_parts(dest)
        t = self.new_tmp("f")
        self.emit(f"float4 {t} = {call};")
        if mask is None:
            self.emit(f"{name} = {t};")
            return
        for i, ch in enumerate(mask):
            comp = "xyzw"[i]
            if ch == "_":
                continue
            if ch in "01":
                self.emit(f"{name}.{comp} = {ch}.0;")
            else:
                self.emit(f"{name}.{comp} = {t}.{ch};")

    def emit_tfetch(self, ins):
        if ins.op not in ("tfetch2D", "tfetch3D", "tfetchCube"):
            raise NotSupported(ins.op)
        if len(ins.srcs) < 2:
            raise NotSupported("tfetch operands")
        cube = ins.op == "tfetchCube"
        dimension = 3 if ins.op != "tfetch2D" else 2
        uv = self.operand(ins.srcs[0], dimension)
        fc = int(ins.srcs[1][2:])
        slot = self.tex_slot(fc, 4 if cube else dimension)
        if cube:
            if len(ins.srcs) != 2 or not re.fullmatch(r"tf(?:[0-9]|[12][0-9]|3[01])", ins.srcs[1]):
                raise NotSupported("tfetchCube operands")
            allowed = {"UnnormalizedTextureCoords", "UseComputedLOD", "UseRegisterLOD",
                       "UseRegisterGradients", "FetchValidOnly", "MagFilter", "MinFilter",
                       "MipFilter", "AnisoFilter", "VolMagFilter", "VolMinFilter",
                       "OffsetX", "OffsetY", "OffsetZ", "LODBias"}
            unknown = set(ins.opts) - allowed
            if unknown:
                raise NotSupported(f"tfetchCube option {sorted(unknown)[0]}")
            for attribute in ("UnnormalizedTextureCoords", "UseComputedLOD", "UseRegisterLOD",
                              "UseRegisterGradients", "FetchValidOnly"):
                if ins.opts.get(attribute, "true") not in ("true", "false"):
                    raise NotSupported(f"{attribute}={ins.opts[attribute]}")
            if ins.opts.get("FetchValidOnly", "true") != "true":
                raise NotSupported("FetchValidOnly=false")
            for attribute in ("MagFilter", "MinFilter", "MipFilter"):
                if ins.opts.get(attribute, "keep") not in ("point", "linear", "keep", "basemap"):
                    raise NotSupported(f"{attribute}={ins.opts[attribute]}")
                if attribute != "MipFilter" and ins.opts.get(attribute) == "basemap":
                    raise NotSupported(f"{attribute}=basemap")
            if ins.opts.get("AnisoFilter", "keep") not in (
                    "keep", "disabled", "max1to1", "max2to1", "max4to1", "max8to1", "max16to1"):
                raise NotSupported(f"AnisoFilter={ins.opts['AnisoFilter']}")
        # A register LOD or register gradients would add a term we do not carry; leave those draws emulated
        # rather than sampling the wrong mip.
        for attribute in ("UseRegisterLOD", "UseRegisterGradients"):
            if ins.opts.get(attribute, "false") == "true":
                raise NotSupported(attribute)
        if ins.opts.get("MipFilter", "") == "basemap":
            raise NotSupported("MipFilter=basemap")
        if dimension == 3:
            # The SDK disassembler normally omits fetch-default volume filters (spelled "keep" in
            # its filter-name table). The runtime handles matching fetch mag/min volume filters;
            # instruction overrides need their own metadata and stay emulated until then.
            for attribute in ("VolMagFilter", "VolMinFilter"):
                if ins.opts.get(attribute, "") not in ("", "keep", "use_fetch_const", "default"):
                    raise NotSupported(f"{attribute}={ins.opts[attribute]}")
        # Retain the legacy selector so existing generated bodies stay identical. The runtime now
        # resolves the SDK's full instruction sampler tuple and gives both selector halves the same
        # effective descriptor; conflicting descriptors for one fetch constant keep the draw emulated.
        filters = {ins.opts.get("MagFilter", ""), ins.opts.get("MinFilter", "")}
        filter_mode = 1 if "point" in filters else (2 if "linear" in filters else 0)
        offsets = [float(ins.opts.get(f"Offset{axis}", "0")) for axis in "XYZ"[:dimension]]
        bias = float(ins.opts.get("LODBias", "0"))
        if cube and not all(math.isfinite(value) for value in offsets + [bias]):
            raise NotSupported("nonfinite cube offset or LOD bias")
        # Vertex shaders have no derivatives: every fetch there is an explicit level-0 sample.
        lod0 = ins.opts.get("UseComputedLOD", "true") == "false" or self.stage == "vs"
        coords = f"float{dimension}({', '.join(uv)})"
        sample_helper = "TexCube" if cube else f"Tex{dimension}D"
        if ins.opts.get("UnnormalizedTextureCoords", "false") == "true":
            if cube:
                # Face index Z is never normalized, and offsets apply before normalization.
                sample_helper = "TexCubeTexel"
            elif dimension == 3:
                # Stacked layers already use texel-space Z. Keep it raw so normalization followed by
                # denormalization cannot move a layer boundary; the helper normalizes only X/Y.
                sample_helper = "Tex3DTexel"
            else:
                # Texel-space coordinates remain filtered and mipped after normalization.
                coords = f"({coords} / TexSize({slot}u))"
        call = (f"{sample_helper}({slot}u, {coords}, {filter_mode}u, "
                f"float{dimension}({', '.join(str(v) for v in offsets)}), "
                f"{'true' if lod0 else 'false'}, {bias!r})")
        self.apply_fetch_dest(ins.dest, call)

    # --- blocks ------------------------------------------------------------------------------------
    def bool_expr(self, cond):
        neg = cond.startswith("!")
        name = cond[1:] if neg else cond
        if name == "p0":
            e = "p0"
        elif name.startswith("b"):
            self.uses_bool = True
            e = f"BoolConst({int(name[1:])}u)"
        else:
            raise NotSupported(f"condition {cond}")
        return f"(!{e})" if neg else e

    def emit_instruction(self, ins, partner):
        """Emits one instruction (with its co-issued partner when partner is the scalar half)."""
        pred_open = None
        if ins.pred:
            self.emit(f"if ({'!p0' if ins.pred == '!p0' else 'p0'}) {{")
            self.depth += 1
            pred_open = True
        if ins.op.startswith("vfetch"):
            self.emit_vfetch(ins)
        elif ins.op.startswith("tfetch") or ins.op in ("getCompTexLOD", "setTexLOD"):
            if ins.op not in ("tfetch2D", "tfetch3D", "tfetchCube"):
                raise NotSupported(ins.op)
            self.emit_tfetch(ins)
        elif ins.op in self.VECTOR_OPS:
            self.emit_alu_pair(ins, partner)
        elif ins.op in self.SCALAR_OPS:
            self.emit_alu_pair(None, ins)
        else:
            raise NotSupported(f"op {ins.op}")
        if pred_open:
            self.depth -= 1
            self.emit("}")

    def validate_blocks(self):
        """The jump-to-flag scheme only models forward jumps to a label that exists. A backward jump is a
        loop and a missing label would silently switch off the rest of the shader, so both are refused
        rather than mistranslated."""
        labels = {}
        for i, blk in enumerate(self.blocks):
            if blk["kind"] == "label":
                labels.setdefault(blk["target"], i)
        for i, blk in enumerate(self.blocks):
            if blk["kind"] in ("jmp", "cjmp"):
                target = labels.get(blk["target"])
                if target is None:
                    raise NotSupported(f"jump to missing label {blk['target']}")
                if target <= i:
                    raise NotSupported(f"backward jump to {blk['target']}")
            # A control-flow block carries no instructions of its own; anything the parser attached to one
            # would be dropped without a word.
            if blk["kind"] in ("jmp", "cjmp", "label") and blk["instrs"]:
                raise NotSupported(f"instructions inside a {blk['kind']} block")

    def translate_body(self):
        self.validate_blocks()
        self.depth = 0
        open_flags = []  # labels whose jump flag guards the current blocks
        # exece / cexece run their block and then end the shader. Where anything follows that could still
        # write an export - the XNA tail idiom "cexece b, cexece !b, exece" usually leaves nothing - the
        # end is modelled as a jump to a synthetic label past the last block.
        ends_early = any(blk["kind"] in ("exece", "cexece") and
                         any(later["kind"] in ("jmp", "cjmp") or later["instrs"]
                             for later in self.blocks[i + 1:])
                         for i, blk in enumerate(self.blocks))
        for blk in self.blocks:
            kind = blk["kind"]
            if kind == "label":
                if blk["target"] in open_flags:
                    open_flags.remove(blk["target"])
                continue
            if kind in ("jmp", "cjmp"):
                guard_depth = 0
                for f in open_flags:
                    self.emit(f"if (!jump_{f}) {{"); self.depth += 1; guard_depth += 1
                conds = []
                if blk.get("pred"):
                    conds.append(self.bool_expr(blk["pred"]))
                if kind == "cjmp":
                    conds.append(self.bool_expr(blk["cond"]))
                if conds:
                    self.emit(f"if ({' && '.join(conds)}) jump_{blk['target']} = true;")
                else:
                    self.emit(f"jump_{blk['target']} = true;")
                for _ in range(guard_depth):
                    self.depth -= 1; self.emit("}")
                if blk["target"] not in open_flags:
                    open_flags.append(blk["target"])
                if blk["target"] not in self.jump_flags:
                    self.jump_flags.append(blk["target"])
                continue
            # exec / exece / cexec / cexece, possibly predicated as a whole
            guard_depth = 0
            for f in open_flags:
                self.emit(f"if (!jump_{f}) {{"); self.depth += 1; guard_depth += 1
            conds = []
            if blk.get("pred"):
                conds.append(self.bool_expr(blk["pred"]))
            if kind in ("cexec", "cexece"):
                conds.append(self.bool_expr(blk["cond"]))
            if conds:
                self.emit(f"if ({' && '.join(conds)}) {{"); self.depth += 1; guard_depth += 1
            instrs = blk["instrs"]
            i = 0
            while i < len(instrs):
                ins = instrs[i]
                partner = None
                if i + 1 < len(instrs) and instrs[i + 1].coissued and ins.op in self.VECTOR_OPS:
                    partner = instrs[i + 1]
                    if partner.pred != ins.pred:
                        # different predicates on the two halves: emit separately (rare)
                        partner = None
                if partner is not None:
                    self.emit_instruction(ins, partner)
                    i += 2
                else:
                    if ins.coissued and ins.op in self.SCALAR_OPS:
                        self.emit_instruction(ins, None)
                    else:
                        self.emit_instruction(ins, None)
                    i += 1
            if ends_early and kind in ("exece", "cexece"):
                self.emit("jump___end = true;")
                if "__end" not in open_flags:
                    open_flags.append("__end")
                if "__end" not in self.jump_flags:
                    self.jump_flags.append("__end")
            for _ in range(guard_depth):
                self.depth -= 1; self.emit("}")

    # --- whole shaders -----------------------------------------------------------------------------
    def declarations(self):
        d = []
        regs = sorted(self.regs_used)
        max_reg = max(regs) if regs else -1
        for r in range(0, max_reg + 1):
            if self.stage == "ps" and r < self.interp_count:
                # An interpolator, unless the guest points param_gen at this register for the draw.
                d.append(f"  float4 r{r} = ParamGenOverride({r}u, i.o{r}, i.pos, i.ptcoord, i.front);")
            elif self.stage == "vs" and r == 0:
                d.append("  float4 r0 = float4(float(vertex_index), 0.0, 0.0, 0.0);")
            else:
                d.append(f"  float4 r{r} = 0.0;")
        d.append("  float ps = 0.0;")
        d.append("  bool p0 = false;")
        if self.uses_a0:
            d.append("  int a0 = 0;")
        for f in self.jump_flags:
            d.append(f"  bool jump_{f} = false;")
        for a in self.vfetch_addrs:
            d.append(f"  uint {a} = 0u;")
        return d


SOURCE_COMPONENT_BITS = {"x": 1, "y": 2, "z": 4, "w": 8}


def source_components(text):
    """(register, component mask) a source operand reads, or (None, 0) for a constant."""
    m = REG_RE.match(text)
    if not m:
        return None, 0
    if m.group(3) is not None:
        register = int(m.group(3))
    elif m.group(4) is not None:
        register = int(m.group(4))
    else:
        return None, 0
    swizzle = m.group(10)
    if not swizzle:
        return register, 0b1111
    mask = 0
    for ch in swizzle:
        mask |= SOURCE_COMPONENT_BITS.get(ch, 0)
    return register, mask or 0b1111


def dest_components(text):
    """(register, component mask) a destination writes; a '_' in the mask leaves that component alone."""
    m = REG_RE.match(text or "")
    if not m or m.group(4) is None:
        return None, 0
    swizzle = m.group(10)
    if not swizzle:
        return int(m.group(4)), 0b1111
    mask = 0
    for i, ch in enumerate(swizzle[:4]):
        if ch != "_":
            mask |= 1 << i
    return int(m.group(4)), mask


def interpolator_input_bound(blocks):
    """How many interpolators a pixel shader consumes: the highest register with a component it reads
    before writing, plus one. Components matter: these shaders routinely write r0.w on the first
    instruction and read the interpolated r0.xy right after, which a per-register rule would miss.
    (The SDK counts every register the shader touches, but over-declaring inputs here would make pairs
    with short vertex shaders unusable.)"""
    written = {}
    bound = 0
    for blk in blocks:
        for ins in blk["instrs"]:
            for src in ins.srcs:
                register, mask = source_components(src)
                if register is not None and (mask & ~written.get(register, 0)):
                    bound = max(bound, register + 1)
            register, mask = dest_components(ins.dest)
            if register is not None:
                written[register] = written.get(register, 0) | mask
    return min(bound, 16)


NO_PACK = os.environ.get("NB_NO_PACK") == "1"


def packed_constant_map(collected):
    """Maps the constant registers a shader reads onto consecutive registers, in the order the runtime
    copies its spans. Returns (map, runs); a None map means keep absolute numbering."""
    runs = constant_runs(collected.const_used, collected.const_all)
    if collected.const_all or NO_PACK:
        return None, [(0, 255)]
    mapping = {}
    packed = 0
    for first, last in runs:
        for index in range(first, last + 1):
            mapping[index] = packed
            packed += 1
    return mapping, runs


def translate_vs(text):
    """Vertex shader alone: VsOut declares the interpolators it exports, the point-sprite coordinates and
    (when the shader kills vertices) a cull distance."""
    blocks = parse_listing(text)
    collect = Translator("vs", blocks, 0)
    collect.translate_body()
    const_map, const_runs = packed_constant_map(collect)
    t = Translator("vs", blocks, 0, const_map)
    t.translate_body()
    exports = sorted(t.exports)
    written = (max(exports) + 1) if exports else 0
    # One interpolator past what the shader exports: a pixel shader's param_gen register is fed by the
    # rasterizer, not by the vertex shader, but it still occupies an input slot, and the guest points it
    # at the register right after the real interpolators. Declaring it keeps those pairs usable.
    n_interp = min(16, written + 1)
    out = []
    # The interpolator count is a compile-time define: the library raises it when a pixel shader reads
    # more registers than this shader exports (they read as zero, undefined on the guest as well).
    out.append("#ifndef NB_INTERPOLATORS")
    out.append(f"#define NB_INTERPOLATORS {n_interp}")
    out.append("#endif")
    # Signature linkage is by register slot, not just semantic: the point coordinates come before the
    # interpolators in both stages so a pixel shader that reads fewer of them still lines up.
    out.append("struct VsOut { float4 pos : SV_Position; float2 ptcoord : POINTCOORD;")
    for k in range(16):
        out.append(f"#if NB_INTERPOLATORS > {k}")
        out.append(f"  float4 o{k} : TEXCOORD{k};")
        out.append("#endif")
    if t.writes_kill:
        out.append("  float cull : SV_CullDistance0;")
    out.append("};")
    out.append("VsOut main(uint id : SV_VertexID) {")
    out.append("  uint vertex_index = VertexIndex(id);")
    out.extend(t.declarations())
    out.append("  float4 oPos = float4(0.0, 0.0, 0.0, 1.0);")
    out.append("  float4 oPts = 0.0;")
    for k in range(n_interp):
        out.append(f"  float4 o{k} = 0.0;")  # the pad register stays zero: nothing exports to it
    out.extend(t.lines)
    out.append("  VsOut o;")
    out.append(f"  o.pos = ApplyPointOffset(HostPosition(oPos), id, oPts.x, "
               f"{'true' if t.writes_point_size else 'false'});")
    out.append("  o.ptcoord = PointCoord(id);")
    for k in range(16):
        out.append(f"#if NB_INTERPOLATORS > {k}")
        out.append(f"  o.o{k} = {f'o{k}' if k < n_interp else '0.0'};")
        out.append("#endif")
    if t.writes_kill:
        # Vertex kill as the SDK does it: bits 0:30 of oPts.z set means killed. PA_CL_CLIP_CNTL.vtx_kill_or
        # (root constant alpha_test.z) selects OR (any killed vertex kills the primitive: NaN position) or
        # AND (all vertices: cull distance, which D3D culls only when negative at every vertex).
        out.append("  bool killed = (asuint(oPts.z) & 0x7FFFFFFFu) != 0u;")
        out.append("  o.cull = killed ? -1.0 : 0.0;")
        out.append("  if (killed && alpha_test.z != 0u) o.pos = asfloat(0x7FC00000u).xxxx;")
    out.append("  return o;")
    out.append("}")
    meta = {
        "interp": n_interp,
        "tex": t.tex_slots,
        "texdim": t.tex_dimensions,
        "streams": t.streams,
        "kill": 1 if t.writes_kill else 0,
        "consts": t.max_const + 1,
        "cruns": const_runs,
    }
    return "\n".join(out) + "\n", meta


def translate_ps(text):
    """Pixel shader alone: PsIn declares the interpolators it reads, the point coordinates and faceness
    (both feed param_gen, which the guest can point at any of those registers)."""
    blocks = parse_listing(text)
    n_interp = interpolator_input_bound(blocks)
    collect = Translator("ps", blocks, n_interp)
    collect.translate_body()
    const_map, const_runs = packed_constant_map(collect)
    t = Translator("ps", blocks, n_interp, const_map)
    t.translate_body()
    colors = sorted(t.color_outputs) or [0]
    out = []
    out.append("struct PsIn { float4 pos : SV_Position; float2 ptcoord : POINTCOORD;" +
               "".join(f" float4 o{k} : TEXCOORD{k};" for k in range(n_interp)) +
               " bool front : SV_IsFrontFace; };")
    if len(colors) > 1:
        out.append("struct PsOut {" + "".join(f" float4 c{k} : SV_Target{k};" for k in colors) + " };")
        sig = "PsOut main(PsIn i" + (", out float out_depth : SV_Depth" if t.writes_depth else "") + ") {"
    else:
        sig = "float4 main(PsIn i" + (", out float out_depth : SV_Depth" if t.writes_depth else "") + \
              f") : SV_Target{colors[0]} {{"
    out.append(sig)
    out.extend(t.declarations())
    # Every target up to the highest one exported, so the alpha test can read colour 0's alpha even from a
    # shader that only writes a higher slot. Unwritten exports read zero, as they do on the guest.
    for k in range(max(colors) + 1):
        out.append(f"  float4 oC{k} = 0.0;")
    if t.writes_depth:
        out.append("  float4 oDepth = 0.0;")
    out.extend(t.lines)
    out.append("  AlphaTest(oC0.w);")
    if t.writes_depth:
        out.append("  out_depth = oDepth.x;")
    if len(colors) > 1:
        out.append("  PsOut o;")
        for k in colors:
            out.append(f"  o.c{k} = oC{k};")
        out.append("  return o;")
    else:
        out.append(f"  return oC{colors[0]};")
    out.append("}")
    meta = {
        "interp": n_interp,
        "tex": t.tex_slots,
        "texdim": t.tex_dimensions,
        "writes_depth": 1 if t.writes_depth else 0,
        "kills": 1 if t.kills else 0,
        "consts": t.max_const + 1,
        "colors": (max(colors) + 1),
        "cruns": const_runs,
    }
    return "\n".join(out) + "\n", meta


def constant_runs(used, use_all, max_runs=8, merge_gap=8):
    """The constant registers a shader reads, as at most `max_runs` [first, last] spans. Runs closer than
    `merge_gap` registers are merged: a short memcpy costs about as much as the gap it would skip."""
    if use_all or not used:
        return [(0, 255)] if use_all else []
    ordered = sorted(used)
    runs = [[ordered[0], ordered[0]]]
    for index in ordered[1:]:
        if index - runs[-1][1] <= merge_gap:
            runs[-1][1] = index
        else:
            runs.append([index, index])
    while len(runs) > max_runs:
        gaps = [(runs[i + 1][0] - runs[i][1], i) for i in range(len(runs) - 1)]
        _, i = min(gaps)
        runs[i][1] = runs[i + 1][1]
        del runs[i + 1]
    return [(first, last) for first, last in runs]


def write_sidecar(path, shader_hash, meta):
    lines = [f"hash={shader_hash}", f"interp={meta['interp']}",
             "tex=" + ",".join(str(fc) for fc in meta["tex"]),
             "texdim=" + ",".join(str(dimension) for dimension in meta["texdim"]),
             f"consts={meta['consts']}",
             "cruns=" + ",".join(f"{first}-{last}" for first, last in meta["cruns"])]
    if "streams" in meta:
        lines.append("streams=" + ",".join(f"{st['vf']}:{st['stride']}" for st in meta["streams"]))
        lines.append(f"kill={meta['kill']}")
    else:
        lines.append(f"writes_depth={meta['writes_depth']}")
        lines.append(f"kills={meta['kills']}")
        lines.append(f"colors={meta['colors']}")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def translate_shader(dump_dir, out_dir, shader_hash, stage, fxc=None):
    """One guest shader -> <stage>_<hash>.hlsl + .meta. Returns (ok, message)."""
    suffix = "vert" if stage == "vs" else "frag"
    path = os.path.join(dump_dir, f"shader_{shader_hash}.ucode.{suffix}")
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    hlsl, meta = translate_vs(text) if stage == "vs" else translate_ps(text)
    os.makedirs(out_dir, exist_ok=True)
    stem = f"{stage}_{shader_hash}"
    with open(os.path.join(out_dir, stem + ".hlsl"), "w", encoding="utf-8", newline="\n") as f:
        f.write(hlsl)
    write_sidecar(os.path.join(out_dir, stem + ".meta"), shader_hash, meta)
    if fxc:
        return compile_check(fxc, out_dir, stem, stage)
    return "ok"


def compile_check(fxc, out_dir, stem, stage):
    with open(PRELUDE, encoding="utf-8") as f:
        prelude = f.read()
    src = os.path.join(out_dir, f"{stem}.hlsl")
    with open(src, encoding="utf-8") as f:
        body = f.read()
    full = os.path.join(out_dir, f"{stem}.full.hlsl")
    with open(full, "w", encoding="utf-8", newline="\n") as f:
        f.write(prelude + "\n" + body)
    proc = subprocess.run([fxc, "/nologo", "/T", "vs_5_1" if stage == "vs" else "ps_5_1", "/E", "main",
                           "/D", f"NB_PIXEL_STAGE={0 if stage == 'vs' else 1}",
                           "/D", "NB_EFFECTIVE_SAMPLERS=1",
                           "/enable_unbounded_descriptor_tables", "/Fo",
                           os.path.join(out_dir, f"{stem}.cso"), full],
                          capture_output=True, text=True)
    if proc.returncode == 0:
        return "ok"
    message = (proc.stderr or proc.stdout).strip().splitlines()
    return "fxc: " + (message[0][:160] if message else "unknown error")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump_dir", help="directory of the SDK's shader dump (shader_<hash>.ucode.vert/.frag)")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--fxc", default=None)
    ap.add_argument("--only", default=None, help="translate just this shader hash")
    args = ap.parse_args()

    shaders = []
    for name in sorted(os.listdir(args.dump_dir)):
        m = re.match(r"shader_([0-9A-Fa-f]{16})\.ucode\.(vert|frag)$", name)
        if not m:
            continue
        if args.only and m.group(1).upper() != args.only.upper():
            continue
        shaders.append((m.group(1).upper(), "vs" if m.group(2) == "vert" else "ps"))

    ok = 0
    reasons = collections.Counter()
    for shader_hash, stage in shaders:
        try:
            result = translate_shader(args.dump_dir, args.out, shader_hash, stage, args.fxc)
        except NotSupported as e:
            reasons[f"unsupported: {e}"] += 1
            print(f"{stage}_{shader_hash}: unsupported: {e}")
            continue
        except Exception as e:  # a translator bug, not a guest shader we cannot express
            reasons[f"translator error {type(e).__name__}"] += 1
            print(f"{stage}_{shader_hash}: translator error: {e}")
            continue
        if result == "ok":
            ok += 1
        else:
            reasons[result[:90]] += 1
            print(f"{stage}_{shader_hash}: {result}")

    print(f"\n{len(shaders)} shaders: {ok} ok, {len(shaders) - ok} unusable")
    for reason, n in reasons.most_common(20):
        print(f"  {n:4} {reason}")


if __name__ == "__main__":
    main()
