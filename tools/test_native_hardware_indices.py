"""Model check for nb_native_hardware_indices; synthetic index data only.

An eligible native guest DMA triangle list is drawn one of two ways. Primitive mode 0 issues
DrawInstanced, and each vertex shader invocation loads its own index (GuestIndex in prelude.hlsl).
Mode 4 binds the guest index range of shared memory as the D3D12 index buffer and issues
DrawIndexedInstanced, so SV_VertexID is the raw index the input assembler read. This models both paths
for every index format, endian and base alignment the eligibility test admits, and checks that the
16-bit endians it refuses really differ. The source pins fail when the code the model mirrors changes.

Mode 0 may read indices from the optional asset arena; that arena holds the same bytes as guest memory
(its byte-verified contract), so the model reads guest memory for both paths.

Run with: python -m unittest discover -s tools -p test_native_hardware_indices.py
"""

import pathlib
import random
import re
import unittest

import ucode2hlsl

ROOT = pathlib.Path(__file__).resolve().parents[1]
PRELUDE = ROOT / "src/gpu/native/shaders/prelude.hlsl"
SEAM = ROOT / "src/gpu/vendored/src/graphics/d3d12/command_processor.cpp"
PREPARE = ROOT / "src/gpu/nb_command_processor.cpp"
RECORD = ROOT / "src/gpu/native/native_geometry_pass.cpp"

K_NONE, K_8IN16, K_8IN32, K_16IN32 = range(4)  # xenos::Endian
# Index width in bytes -> the endians native_index_format_supported admits. Bases are width-aligned.
ADMITTED = {2: (K_NONE, K_8IN16), 4: (K_NONE, K_8IN16, K_8IN32, K_16IN32)}
REFUSED_16_BIT = (K_8IN32, K_16IN32)
# Width-aligned, nonzero guest bases: 16-bit indices start in either half of a dword.
BASES = {2: (0x100, 0x102), 4: (0x100,)}
# (VGT_INDX_OFFSET, minimum, maximum): identity, a wrapping offset, a clamp on both sides, no upper clamp.
REMAPS = ((0, 0, 0xFFFFFF), (0xFFFFFFFF, 0, 0xFFFFFF), (0x00FFFFF0, 0x20, 0x8000), (7, 0x100, 0xFFFFFFFF))


def swap_with(value, endian):
    """SwapWith4 on one lane: 8-in-16 for endians 1 and 2, then 16-in-32 for endians 2 and 3."""
    m = endian ^ (endian >> 1)
    if m & 1:
        value = ((value & 0x00FF00FF) << 8) | ((value >> 8) & 0x00FF00FF)
    if m & 2:
        value = (value >> 16) | ((value << 16) & 0xFFFFFFFF)
    return value


def load(memory, address):
    """ByteAddressBuffer.Load: the little-endian dword at a dword-aligned byte address."""
    assert address % 4 == 0
    return int.from_bytes(memory[address:address + 4], "little")


def guest_index(memory, base, width, endian, i):
    """GuestIndex: load the dword holding index i, swap it, and pick the 16-bit half at its address."""
    if width == 4:
        return swap_with(load(memory, base + i * 4), endian)
    byte_address = base + i * 2
    word = swap_with(load(memory, byte_address & ~3), endian)
    return word >> 16 if byte_address & 2 else word & 0xFFFF


def ia_index(memory, base, width, k):
    """Index k as the input assembler reads R16_UINT or R32_UINT: little-endian, zero-extended.

    BaseVertexLocation is 0 and IBStripCutValue is DISABLED, so SV_VertexID is exactly this value.
    """
    return int.from_bytes(memory[base + k * width:base + (k + 1) * width], "little")


def vertex_index(mode, vertex_id, memory, base, width, endian, remap):
    """VertexIndex for a triangle list: mode 4 swaps the raw IA index, mode 0 gathers by position."""
    offset, minimum, maximum = remap
    i = swap_with(vertex_id, endian) if mode == 4 else guest_index(memory, base, width, endian, vertex_id)
    i = (i + offset) & 0xFFFFFF
    return min(max(i, minimum), maximum)


def draw(mode, memory, base, width, endian, count, remap):
    """The guest vertex index of each host vertex, in primitive assembly order."""
    if mode == 4:
        # DrawIndexedInstanced(count, 1, 0, 0, 0)
        ids = [ia_index(memory, base, width, k) for k in range(count)]
    else:
        ids = range(count)  # DrawInstanced(count, 1, 0, 0)
    return [vertex_index(mode, vertex_id, memory, base, width, endian, remap) for vertex_id in ids]


def guest_memory(base, index_bytes, seed):
    """Random bytes around the index buffer; the last 16-bit index's dword load reads past its end."""
    rng = random.Random(seed)
    return rng.randbytes(base) + index_bytes + rng.randbytes(8)


def edge_index_bytes(width, count, seed):
    """Random indices, plus all-zero, all-one (0xFFFF, 0xFFFFFFFF) and single-byte values."""
    rng = random.Random(seed)
    edges = [0, (1 << (8 * width)) - 1, 0xFF, 0xFF << (8 * width - 8)]
    if width == 4:
        edges += [0xFFFF, 0xFFFF0000, 0x00FFFFFF, 0x01000000]
    values = edges + [rng.getrandbits(8 * width) for _ in range(count - len(edges))]
    rng.shuffle(values)
    return b"".join(value.to_bytes(width, "little") for value in values)


def first_difference(a, b):
    """(position, a value, b value) of the first mismatch, or None; cheaper than a difflib report."""
    if len(a) != len(b):
        return ("length", len(a), len(b))
    return next(((k, x, y) for k, (x, y) in enumerate(zip(a, b)) if x != y), None)


def code(path):
    """Source with // comments removed and whitespace collapsed, so comment edits do not trip the pins."""
    return " ".join(re.sub(r"//[^\n]*", "", path.read_text(encoding="utf-8")).split())


def body(source, signature):
    """The text inside the braces that follow signature in normalized source."""
    start = source.index(signature + " {") + len(signature) + 2
    depth = 1
    for end in range(start, len(source)):
        depth += {"{": 1, "}": -1}.get(source[end], 0)
        if depth == 0:
            return source[start:end].strip()
    raise AssertionError(f"unbalanced braces after {signature}")


def expression(source, lhs):
    """The right-hand side of the first `lhs = ...;` in normalized source."""
    start = source.index(lhs + " = ") + len(lhs) + 3
    return source[start:source.index(";", start)]


class SwapTests(unittest.TestCase):
    def test_swap_with_matches_sdk_gpu_swap(self):
        rng = random.Random(1)
        for value in [0, 0xFFFFFFFF, 0x12345678, 0x0000FFFF] + [rng.getrandbits(32) for _ in range(500)]:
            b = value.to_bytes(4, "little")
            expected = {
                K_NONE: value,
                K_8IN16: int.from_bytes(bytes((b[1], b[0], b[3], b[2])), "little"),
                K_8IN32: int.from_bytes(b, "big"),
                K_16IN32: (value >> 16) | ((value & 0xFFFF) << 16),
            }
            for endian, swapped in expected.items():
                self.assertEqual(swap_with(value, endian), swapped, (hex(value), endian))


class IndexReadTests(unittest.TestCase):
    def test_every_16_bit_index_matches(self):
        # Every 16-bit value at each admitted endian, starting at either half of a dword.
        table = b"".join(value.to_bytes(2, "little") for value in range(0x10000))
        for base in BASES[2]:
            memory = guest_memory(base, table, base)
            for endian in ADMITTED[2]:
                with self.subTest(base=hex(base), endian=endian):
                    gathered = [guest_index(memory, base, 2, endian, k) for k in range(0x10000)]
                    assembled = [swap_with(ia_index(memory, base, 2, k), endian) for k in range(0x10000)]
                    self.assertIsNone(first_difference(assembled, gathered))
                    self.assertEqual(sorted(gathered), list(range(0x10000)))

    def test_32_bit_indices_match_at_every_endian(self):
        for base in BASES[4]:
            memory = guest_memory(base, edge_index_bytes(4, 4096, 2), base)
            for endian in ADMITTED[4]:
                with self.subTest(base=hex(base), endian=endian):
                    gathered = [guest_index(memory, base, 4, endian, k) for k in range(4096)]
                    assembled = [swap_with(ia_index(memory, base, 4, k), endian) for k in range(4096)]
                    self.assertIsNone(first_difference(assembled, gathered))
                    self.assertIn(0xFFFFFFFF, gathered)

    def test_remapped_draws_match(self):
        count = 3 * 700 + 2  # with a trailing partial triangle; both draws submit the same vertex count
        for width, endians in ADMITTED.items():
            for base in BASES[width]:
                memory = guest_memory(base, edge_index_bytes(width, count, width + base), base)
                for endian in endians:
                    for remap in REMAPS:
                        with self.subTest(width=width, base=hex(base), endian=endian, remap=remap):
                            hardware = draw(4, memory, base, width, endian, count, remap)
                            manual = draw(0, memory, base, width, endian, count, remap)
                            self.assertIsNone(first_difference(hardware, manual))

    def test_refused_16_bit_endians_differ_in_both_halves(self):
        # Cross-half swaps move a zero-extended IA index into the upper half, while the gather selects
        # the other half of the swapped dword. Every nonzero index then differs, in either dword half.
        count = 3 * 700
        table = edge_index_bytes(2, count, 3)
        for base in BASES[2]:
            memory = guest_memory(base, table, base)
            for endian in REFUSED_16_BIT:
                with self.subTest(base=hex(base), endian=endian):
                    for parity in (0, 1):
                        positions = [k for k in range(parity, count, 2) if ia_index(memory, base, 2, k)]
                        self.assertGreater(len(positions), 600)
                        for k in positions:
                            self.assertNotEqual(swap_with(ia_index(memory, base, 2, k), endian),
                                                guest_index(memory, base, 2, endian, k), k)
                    hardware = draw(4, memory, base, 2, endian, count, REMAPS[0])
                    manual = draw(0, memory, base, 2, endian, count, REMAPS[0])
                    self.assertIsNotNone(first_difference(hardware, manual))


class SourcePinTests(unittest.TestCase):
    """The code the model mirrors. A failure here means the model must be revisited, not just updated."""

    def test_shader_index_paths(self):
        prelude = code(PRELUDE)
        self.assertEqual(body(prelude, "uint4 SwapWith4(uint4 v, uint endian)"),
                         "uint m = endian ^ (endian >> 1u); "
                         "uint4 s16 = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu); "
                         "v = (m & 1u) != 0u ? s16 : v; "
                         "return (m & 2u) != 0u ? ((v >> 16) | (v << 16)) : v;")
        self.assertEqual(body(prelude, "uint SwapWith(uint v, uint endian)"),
                         "return SwapWith4(uint4(v, v, v, v), endian).x;")
        self.assertEqual(body(prelude, "uint GuestIndex(uint i)"),
                         "if (index_info.y == 1u) { return SwapWith(RawStreamLoad(index_info.x + i * 4u, "
                         "NB_MAX_VERTEX_STREAMS), index_info.z); } "
                         "uint byte_address = index_info.x + i * 2u; "
                         "uint word = SwapWith(RawStreamLoad(byte_address & ~3u, NB_MAX_VERTEX_STREAMS), "
                         "index_info.z); "
                         "return (byte_address & 2u) ? (word >> 16) : (word & 0xFFFFu);")
        vertex = body(prelude, "uint VertexIndex(uint id)")
        tail = ("if (fetch0.w == 4u) { i = SwapWith(id, index_info.z); } "
                "else if (index_info.x != 0xFFFFFFFFu) { i = GuestIndex(i); } "
                "i = (i + fetch1.w) & 0xFFFFFFu; "
                "return min(max(i, asuint(ndc_scale.w)), asuint(ndc_offset.w));")
        self.assertTrue(vertex.startswith("uint i = id; "), vertex)
        self.assertTrue(vertex.endswith(tail), vertex)
        # Only the quad, point and strip modes expand the id before the tail.
        self.assertEqual(set(re.findall(r"fetch0\.w == (\d+)u", vertex[:-len(tail)])), {"1", "2", "3"})
        self.assertNotRegex(vertex, r"\bid\s*[-+*/|&^]?=(?!=)")
        for helper in ("float2 PointCoord(uint id)",
                       "float4 ApplyPointOffset(float4 clip_pos, uint id, float vertex_diameter, "
                       "bool has_vertex_diameter)"):
            self.assertTrue(body(prelude, helper).startswith("if (fetch0.w != 2u) return "), helper)

    def test_generated_vertex_shader_reads_the_id_only_through_these_helpers(self):
        source, _ = ucode2hlsl.translate_vs(
            "vfetch_full r1, r0.x, vf95, DataFormat=FMT_32_32_32_FLOAT, Stride=3\n"
            "vfetch_full r2, r0.x, vf94, DataFormat=FMT_8_8_8_8, Stride=1\n"
            "mul oPos, r1, c0\nadd o0, r2, c1")
        uses = [line.strip() for line in source.splitlines() if re.search(r"\bid\b", line)]
        self.assertEqual(uses, ["VsOut main(uint id : SV_VertexID) {",
                                "uint vertex_index = VertexIndex(id);",
                                "o.pos = ApplyPointOffset(HostPosition(oPos), id, oPts.x, false);",
                                "o.ptcoord = PointCoord(id);"])

    def test_eligibility(self):
        seam = code(SEAM)
        self.assertIn("if (host_render_targets_used && !memexport_used) {", seam)
        self.assertEqual(expression(seam, "const bool native_index_format_supported"),
                         "(native_context.index_format == xenos::IndexFormat::kInt16 && "
                         "(native_context.index_endian == xenos::Endian::kNone || "
                         "native_context.index_endian == xenos::Endian::k8in16)) || "
                         "(native_context.index_format == xenos::IndexFormat::kInt32 && "
                         "uint32_t(native_context.index_endian) <= uint32_t(xenos::Endian::k16in32))",
                         "the admitted index formats changed; revisit ADMITTED and the model")
        terms = set(expression(seam, "native_context.hardware_index_eligible").split(" && "))
        required = {
            "native_index_format_supported",
            "regs.Get<reg::VGT_DRAW_INITIATOR>().source_select == xenos::SourceSelect::kDMA",
            "primitive_type == xenos::PrimitiveType::kTriangleList",
            "primitive_processing_result.guest_primitive_type == primitive_type",
            "primitive_processing_result.host_primitive_type == xenos::PrimitiveType::kTriangleList",
            "primitive_processing_result.host_vertex_shader_type == Shader::HostVertexShaderType::kVertex",
            "!primitive_processing_result.IsTessellated()",
            "!primitive_processing_result.host_primitive_reset_enabled",
            "primitive_processing_result.index_buffer_type == "
            "PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA",
            "primitive_processing_result.host_draw_vertex_count == index_count",
            "index_buffer_info->count == index_count",
            "primitive_processing_result.guest_index_base == native_context.index_guest_base",
            "primitive_processing_result.host_index_format == native_context.index_format",
            "primitive_processing_result.host_shader_index_endian == native_context.index_endian",
            # The index buffer view spans exactly the draw's indices: a nonzero, width-aligned base, inside
            # the 512 MB shared-memory buffer and inside the guest's DMA range.
            "native_context.index_guest_base != 0",
            "(native_context.index_guest_base & (native_index_width - 1)) == 0",
            "native_context.index_guest_base < SharedMemory::kBufferSize",
            "native_index_bytes <= SharedMemory::kBufferSize - native_context.index_guest_base",
            "native_index_bytes <= index_buffer_info->length",
        }
        self.assertEqual(required - terms, set(), "an eligibility term the model relies on was removed")
        self.assertEqual(expression(seam, "const uint32_t native_index_width"),
                         "native_context.index_format == xenos::IndexFormat::kInt32 ? 4u : 2u")
        self.assertEqual(expression(seam, "const uint64_t native_index_bytes"),
                         "uint64_t(index_count) * native_index_width")
        self.assertEqual(expression(seam, "native_context.use_hardware_indices"),
                         "native_context.hardware_index_eligible && REXCVAR_GET(nb_native_hardware_indices)")

    def test_draw_recording(self):
        prepare = code(PREPARE)
        for statement in ("root.index_base_bytes = context.index_guest_base;",
                          "root.index_format = context.index_format == xenos::IndexFormat::kInt32 ? 1u : 0u;",
                          "root.index_endian = static_cast<uint32_t>(context.index_endian);",
                          # The view's SizeInBytes: every index of the draw at its width.
                          "const uint64_t index_bytes = "
                          "uint64_t(context.index_count) * (root.index_format ? 4u : 2u);",
                          "args.index_size_bytes = static_cast<uint32_t>(index_bytes);",
                          "case xenos::PrimitiveType::kTriangleList: "
                          "args.hardware_index_eligible = context.hardware_index_eligible; "
                          "args.use_hardware_indices = "
                          "context.hardware_index_eligible && context.use_hardware_indices; "
                          "root.primitive_mode = args.use_hardware_indices ? 4u : 0u; "
                          "args.host_vertex_count = context.index_count; break;"):
            self.assertIn(statement, prepare)
        record = code(RECORD)
        for statement in ("D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};",
                          "cp.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);",
                          "indices.BufferLocation = "
                          "shared_memory.GetGPUAddress() + constants.index_base_bytes;",
                          "indices.SizeInBytes = args.index_size_bytes;",
                          "indices.Format = "
                          "constants.index_format ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;",
                          "list.D3DDrawIndexedInstanced(args.host_vertex_count, 1, 0, 0, 0);",
                          "list.D3DDrawInstanced(args.host_vertex_count, 1, 0, 0);"):
            self.assertIn(statement, record)
        # A value-initialized desc keeps IBStripCutValue DISABLED: 0xFFFF and 0xFFFFFFFF stay indices.
        self.assertNotIn("IBStripCutValue", record)


if __name__ == "__main__":
    unittest.main()
