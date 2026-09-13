// nb native passes: the declarations every geometry-pass shader starts with. Embedded into rexgpu-nb by
// CMake (native_prelude.h) and prepended by tools/ucode2hlsl.py for the offline fxc check; one file for
// both so the two can never drift.
//
// Register model (matches NativeGeometryPass::RootConstants, 16-byte slots, 52 dwords):
//   fetch0/fetch1  guest vertex streams: byte base, stride in dwords, xenos::Endian, (mode / unused)
//   ndc_scale/off  xyz: host viewport mapping; w: bitwise unsigned minimum/maximum guest vertex index
//   tex_index      shader-side bindless indices; slots 0..15 are the pixel shader's fetch constants in
//                  the order its sidecar lists them, 16..19 the vertex shader's
//   index_info     guest index buffer: byte base (0xFFFFFFFF = not indexed), format (0 = 16, 1 = 32),
//                  endian, and param_gen (bit 8 = SQ_PROGRAM_CNTL.param_gen, bits 0..7 = param_gen_pos,
//                  bits 16..19 / 20..23 = draw resolution scale x / y - 1, bit 24 = offsets count guest
//                  texels on scaled textures)
//   sampler_sel    legacy point/base-mip selectors; generated draws now leave these clear
//   alpha_test     x = RB_COLORCONTROL alpha_func | alpha_test_enable << 3, y = RB_ALPHA_REF bits
//   pass_params    pass-specific
// Each stage reads its own constant buffer: b1 for the vertex shader, b2 for the pixel shader. Both
// start with the guest's 256 bool constants, per-slot texture swizzles and sampler indices, then the float
// constants that stage reads, renumbered from zero by the translator (NB_VS_CONSTANTS / NB_PS_CONSTANTS
// come from the sidecar, so the buffer is exactly as large as the shader needs).

cbuffer NbRoot : register(b0) {
  uint4 fetch0;         // x base bytes, y stride dwords, z endian, w mode (0 triangles, 1 quads, 2 points, 3 strip, 4 indexed)
  uint4 fetch1;         // x base bytes, y stride dwords, z endian, w VGT_INDX_OFFSET (added to every vertex index)
  float4 ndc_scale;
  float4 ndc_offset;
  uint4 tex_index[5];
  uint4 index_info;     // x base bytes or 0xFFFFFFFF, y format, z endian, w param_gen
  uint4 sampler_sel;
  uint4 alpha_test;
  uint4 pass_params;
};
// Which stage this copy of the prelude is compiled for: the two stages read their bool constants from
// different buffers, and only the pixel stage may sample with an implicit level.
#ifndef NB_PIXEL_STAGE
#define NB_PIXEL_STAGE 0
#endif
// Generated library draws have one SDK-resolved sampler per slot and clear
// sampler_sel. Keep legacy selection for hand-written passes and standalone probes.
#ifndef NB_EFFECTIVE_SAMPLERS
#define NB_EFFECTIVE_SAMPLERS 0
#endif
// A separately cached library form may omit immutable-asset routing only when
// the owning pass refuses every draw with an asset-cache pointer.
#ifndef NB_DIRECT_GUEST_READS
#define NB_DIRECT_GUEST_READS 0
#endif
// tex_swizzle: the fetch constant's 12-bit component swizzle per texture slot (3 bits per destination
// component, 0..3 = source x..w, 4 = zero, 5 = one). The SDK's texture cache bakes the swizzle into the
// bindless descriptor, so this is only for the places that read a texture without going through Tex2D.
#ifndef NB_VS_CONSTANTS
#define NB_VS_CONSTANTS 256
#endif
#ifndef NB_PS_CONSTANTS
#define NB_PS_CONSTANTS 256
#endif
#define NB_MAX_VERTEX_STREAMS 16
// Each tex_sampler word retains low/high 16-bit indices for old generated callers. Both now select
// the same SDK-resolved sampler when the draw has one effective descriptor per fetch constant.
cbuffer NbVsConstants : register(b1) {
  uint4 bools[2]; uint4 tex_swizzle[5]; uint4 tex_sampler[5];
  uint4 extra_vertex_streams[NB_MAX_VERTEX_STREAMS - 2];  // byte base, stride dwords, endian, reserved
  uint4 asset_bindings[NB_MAX_VERTEX_STREAMS + 1];  // guest base, length, arena offset, enabled; indices last
  float4 vs_c[NB_VS_CONSTANTS];
};
cbuffer NbPsConstants : register(b2) { uint4 ps_bools[2]; uint4 ps_tex_swizzle[5]; uint4 ps_tex_sampler[5]; float4 ps_c[NB_PS_CONSTANTS]; };
ByteAddressBuffer guest_memory : register(t0, space2);
ByteAddressBuffer native_asset_memory : register(t0, space4);
Texture2DArray<float4> textures[] : register(t0, space1);
// Same descriptor heap and index origin as textures, with the SDK's cube SRV dimension.
TextureCube<float4> cube_textures[] : register(t0, space3);
// Generated fetches use the SDK's sampler descriptor heap, preserving addressing on each axis.
SamplerState texture_samplers[] : register(s0, space1);
// Retained for the hand-written passes in register space0.
SamplerState samp_linear_clamp : register(s0);
SamplerState samp_linear_wrap : register(s1);
SamplerState samp_point_clamp : register(s2);
SamplerState samp_point_wrap : register(s3);

// The hardware composes two swap stages: 8-in-16 for modes 1 and 2, and 16-in-32 for modes 2 and 3,
// which is how the SDK emits it (dxbc_translator_fetch.cpp, its two OpSwitch blocks). `endian ^ (endian
// >> 1)` maps 0,1,2,3 to 0,1,3,2, putting the first stage in bit 0 and the second in bit 1 - two selects
// instead of three compares and eight branch instructions, and one call swaps four words at once rather
// than paying the chain per dword. Vertex fetch was 85% memory plumbing before this: one shader measured
// 832 control-flow instructions against 90 of the guest's own arithmetic.
uint4 SwapWith4(uint4 v, uint endian) {
  uint m = endian ^ (endian >> 1u);
  uint4 s16 = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
  v = (m & 1u) != 0u ? s16 : v;
  return (m & 2u) != 0u ? ((v >> 16) | (v << 16)) : v;
}
uint SwapWith(uint v, uint endian) { return SwapWith4(uint4(v, v, v, v), endian).x; }
// These spans match current guest bytes. A dynamic vfetch may legally leave
// its declared stream; each complete load then retains the shared-memory path.
bool AssetContains(uint4 binding, uint address, uint length) {
  return binding.w != 0u && address >= binding.x && length <= binding.y &&
         address - binding.x <= binding.y - length;
}
uint4 AssetBinding(uint slot) {
  if (slot <= NB_MAX_VERTEX_STREAMS) return asset_bindings[slot];
  return uint4(0u, 0u, 0u, 0u);
}
uint RawStreamLoad(uint address, uint slot) {
#if !NB_DIRECT_GUEST_READS
  uint4 binding = AssetBinding(slot);
  if (AssetContains(binding, address, 4u))
    return native_asset_memory.Load(binding.z + (address - binding.x));
#endif
  return guest_memory.Load(address);
}
uint2 RawStreamLoad2(uint address, uint slot) {
#if !NB_DIRECT_GUEST_READS
  uint4 binding = AssetBinding(slot);
  if (AssetContains(binding, address, 8u))
    return native_asset_memory.Load2(binding.z + (address - binding.x));
#endif
  return guest_memory.Load2(address);
}
uint4 RawStreamLoadN(uint address, uint count, uint slot) {
  uint4 result = 0u;
#if !NB_DIRECT_GUEST_READS
  uint4 binding = AssetBinding(slot);
  if (AssetContains(binding, address, count * 4u)) {
    uint source = binding.z + (address - binding.x);
    result.x = native_asset_memory.Load(source);
    if (count > 1u) result.y = native_asset_memory.Load(source + 4u);
    if (count > 2u) result.z = native_asset_memory.Load(source + 8u);
    if (count > 3u) result.w = native_asset_memory.Load(source + 12u);
  } else
#endif
  {
    result.x = guest_memory.Load(address);
    if (count > 1u) result.y = guest_memory.Load(address + 4u);
    if (count > 2u) result.z = guest_memory.Load(address + 8u);
    if (count > 3u) result.w = guest_memory.Load(address + 12u);
  }
  return result;
}
uint GuestLoadE(uint byte_address, uint endian) { return SwapWith(guest_memory.Load(byte_address), endian); }
uint2 GuestLoadE2(uint byte_address, uint endian) {
  return SwapWith4(uint4(guest_memory.Load2(byte_address), 0u, 0u), endian).xy;
}
// Stream-0 helpers used by the hand-written passes.
uint GuestLoad(uint byte_address) { return GuestLoadE(byte_address, fetch0.z); }
float GuestFloat(uint byte_address) { return asfloat(GuestLoad(byte_address)); }
float2 GuestFloat2(uint byte_address) { return float2(GuestFloat(byte_address), GuestFloat(byte_address + 4)); }
float4 GuestFloat4(uint byte_address) {
  return float4(GuestFloat(byte_address), GuestFloat(byte_address + 4), GuestFloat(byte_address + 8),
                GuestFloat(byte_address + 12));
}

uint GuestIndex(uint i) {
  if (index_info.y == 1u) {
    return SwapWith(RawStreamLoad(index_info.x + i * 4u, NB_MAX_VERTEX_STREAMS), index_info.z);
  }
  uint byte_address = index_info.x + i * 2u;
  uint word = SwapWith(RawStreamLoad(byte_address & ~3u, NB_MAX_VERTEX_STREAMS), index_info.z);
  return (byte_address & 2u) ? (word >> 16) : (word & 0xFFFFu);
}
// Host vertex id -> guest vertex index. Quad/point lists and triangle strips expand to triangle lists;
// Manual indexed draws read the guest index buffer after expansion. Mode 4 receives a raw IA index.
uint VertexIndex(uint id) {
  uint i = id;
  if (fetch0.w == 1u) {
    uint quad = id / 6u;
    uint t = id - quad * 6u;
    uint corner = (t == 0u || t == 3u) ? 0u : (t == 1u ? 1u : (t == 2u || t == 4u) ? 2u : 3u);
    i = quad * 4u + corner;
  } else if (fetch0.w == 2u) {
    i = id / 6u;
  } else if (fetch0.w == 3u) {
    uint triangle_index = id / 3u;
    uint corner = id - triangle_index * 3u;
    // D3D strips keep the leading vertex and reverse the last two on odd triangles:
    // (0,1,2), (1,3,2), (2,3,4), (3,5,4), ... . This preserves winding and interpolation.
    if ((triangle_index & 1u) != 0u && corner != 0u) corner = 3u - corner;
    i = triangle_index + corner;
  }
  if (fetch0.w == 4u) {
    i = SwapWith(id, index_info.z);
  } else if (index_info.x != 0xFFFFFFFFu) {
    i = GuestIndex(i);
  }
  // Match the SDK's RemapAndConvertVertexIndices: offset, 24-bit wrap, then unsigned min/max clamp.
  // Viewport mapping uses only xyz, leaving the two w lanes available without growing the root table.
  i = (i + fetch1.w) & 0xFFFFFFu;
  return min(max(i, asuint(ndc_scale.w)), asuint(ndc_offset.w));
}
uint VertexAddress(uint vertex_index) { return fetch0.x + vertex_index * fetch0.y * 4u; }
uint4 StreamInfo(uint stream) {
  if (stream == 0u) return fetch0;
  if (stream == 1u) return fetch1;
  return stream < NB_MAX_VERTEX_STREAMS ? extra_vertex_streams[stream - 2u] : uint4(0u, 0u, 0u, 0u);
}
uint StreamAddress(uint stream, uint vertex_index) {
  uint4 f = StreamInfo(stream);
  return f.x + vertex_index * f.y * 4u;
}
uint StreamEndian(uint stream) { return StreamInfo(stream).z; }

// --- guest arithmetic ---------------------------------------------------------------------------
// Xenos is a Shader Model 3 part and multiplies by the SM3 rule: a product with a zero or denormal
// multiplicand is +0 regardless of the other operand, so an infinity the guest produced (rcp of zero,
// say) multiplies away to nothing instead of poisoning the frame with a NaN. A NaN colour is a black
// surface, which is exactly how this showed up.
float MulLegacy(float a, float b) { return (min(abs(a), abs(b)) == 0.0) ? 0.0 : (a * b); }
float2 MulLegacy(float2 a, float2 b) { return float2(MulLegacy(a.x, b.x), MulLegacy(a.y, b.y)); }
float3 MulLegacy(float3 a, float3 b) {
  return float3(MulLegacy(a.x, b.x), MulLegacy(a.y, b.y), MulLegacy(a.z, b.z));
}
float4 MulLegacy(float4 a, float4 b) {
  return float4(MulLegacy(a.x, b.x), MulLegacy(a.y, b.y), MulLegacy(a.z, b.z), MulLegacy(a.w, b.w));
}
// Summed left to right rather than through dot(), so no driver fuses a guarded term back into an FMA.
float DotLegacy2(float2 a, float2 b) { return MulLegacy(a.x, b.x) + MulLegacy(a.y, b.y); }
float DotLegacy3(float3 a, float3 b) {
  return (MulLegacy(a.x, b.x) + MulLegacy(a.y, b.y)) + MulLegacy(a.z, b.z);
}
float DotLegacy4(float4 a, float4 b) {
  return ((MulLegacy(a.x, b.x) + MulLegacy(a.y, b.y)) + MulLegacy(a.z, b.z)) + MulLegacy(a.w, b.w);
}
// max/min are selects on this hardware, so a NaN on the right wins; HLSL's max/min drop it instead.
float MaxLegacy(float a, float b) { return (a >= b) ? a : b; }
float MinLegacy(float a, float b) { return (a < b) ? a : b; }
float4 MaxLegacy(float4 a, float4 b) {
  return float4(MaxLegacy(a.x, b.x), MaxLegacy(a.y, b.y), MaxLegacy(a.z, b.z), MaxLegacy(a.w, b.w));
}
float4 MinLegacy(float4 a, float4 b) {
  return float4(MinLegacy(a.x, b.x), MinLegacy(a.y, b.y), MinLegacy(a.z, b.z), MinLegacy(a.w, b.w));
}

// Vertex formats, decoded like the SDK's shader interpreter. `signed_` and `normalized` come from the
// fetch instruction (Signed=true; NumFormat=integer means not normalized). Components the format does
// not carry read 0; the fetch instruction's own dest swizzle supplies any 1 the compiler wanted.
float NormPacked(int raw, uint width, bool signed_, bool normalized) {
  if (!normalized) return float(raw);
  if (signed_) return max(-1.0, float(raw) / float((1u << (width - 1u)) - 1u));
  return float(uint(raw)) / float((1u << width) - 1u);
}
// Sign extension by a pair of shifts rather than a compare and a subtract: `offset` and `width` are
// literals at every call site, so this folds to a single bitfield extract, where the compare form left a
// data-dependent branch around every component of every packed attribute.
int PackedField(uint v, uint offset, uint width, bool signed_) {
  if (signed_) return int(v << (32u - offset - width)) >> int(32u - width);
  return int((v >> offset) & ((width == 32u) ? 0xFFFFFFFFu : ((1u << width) - 1u)));
}
float4 FetchF32(uint addr, uint endian, uint count, uint slot) {
  uint4 w = RawStreamLoadN(addr, count, slot);
  return asfloat(SwapWith4(w, endian));
}
// Xenos 16-bit float shares IEEE half's 1/5/10 layout and +112 exponent rebias but has no Inf or NaN -
// exponent 31 is just more finite range, up to 131008 - and flushes denormals to signed zero. f16tof32
// turns those 2048 exponent-31 patterns into Inf/NaN, which then poisons every multiply downstream.
float XenosHalf(uint h) {
  // Selected, not branched: an exponent field of 0 already yields signed zero, which is exactly what the
  // denormal case wants, and this ran once per half - four times for every FetchF16x4.
  uint e = (h >> 10u) & 0x1Fu;
  uint body = ((e + 112u) << 23u) | ((h & 0x3FFu) << 13u);
  return asfloat(((h & 0x8000u) << 16u) | ((e != 0u) ? body : 0u));
}
float4 FetchF16x2(uint addr, uint endian, uint slot) {
  uint v = SwapWith(RawStreamLoad(addr, slot), endian);
  return float4(XenosHalf(v & 0xFFFFu), XenosHalf(v >> 16), 0.0, 0.0);
}
float4 FetchF16x4(uint addr, uint endian, uint slot) {
  uint2 v = SwapWith4(uint4(RawStreamLoad2(addr, slot), 0u, 0u), endian).xy;
  return float4(XenosHalf(v.x & 0xFFFFu), XenosHalf(v.x >> 16), XenosHalf(v.y & 0xFFFFu), XenosHalf(v.y >> 16));
}
float4 Fetch2_10_10_10(uint addr, uint endian, bool signed_, bool normalized, uint slot) {
  uint v = SwapWith(RawStreamLoad(addr, slot), endian);
  return float4(NormPacked(PackedField(v, 0u, 10u, signed_), 10u, signed_, normalized),
                NormPacked(PackedField(v, 10u, 10u, signed_), 10u, signed_, normalized),
                NormPacked(PackedField(v, 20u, 10u, signed_), 10u, signed_, normalized),
                NormPacked(PackedField(v, 30u, 2u, signed_), 2u, signed_, normalized));
}
float4 Fetch8_8_8_8(uint addr, uint endian, bool signed_, bool normalized, uint slot) {
  uint v = SwapWith(RawStreamLoad(addr, slot), endian);
  return float4(NormPacked(PackedField(v, 0u, 8u, signed_), 8u, signed_, normalized),
                NormPacked(PackedField(v, 8u, 8u, signed_), 8u, signed_, normalized),
                NormPacked(PackedField(v, 16u, 8u, signed_), 8u, signed_, normalized),
                NormPacked(PackedField(v, 24u, 8u, signed_), 8u, signed_, normalized));
}
float4 Fetch16_16(uint addr, uint endian, bool signed_, bool normalized, uint slot) {
  uint v = SwapWith(RawStreamLoad(addr, slot), endian);
  return float4(NormPacked(PackedField(v, 0u, 16u, signed_), 16u, signed_, normalized),
                NormPacked(PackedField(v, 16u, 16u, signed_), 16u, signed_, normalized), 0.0, 0.0);
}
float4 Fetch16_16_16_16(uint addr, uint endian, bool signed_, bool normalized, uint slot) {
  uint2 v = SwapWith4(uint4(RawStreamLoad2(addr, slot), 0u, 0u), endian).xy;
  return float4(NormPacked(PackedField(v.x, 0u, 16u, signed_), 16u, signed_, normalized),
                NormPacked(PackedField(v.x, 16u, 16u, signed_), 16u, signed_, normalized),
                NormPacked(PackedField(v.y, 0u, 16u, signed_), 16u, signed_, normalized),
                NormPacked(PackedField(v.y, 16u, 16u, signed_), 16u, signed_, normalized));
}
float4 Fetch11_11_10(uint addr, uint endian, bool signed_, bool normalized, uint slot) {
  uint v = SwapWith(RawStreamLoad(addr, slot), endian);
  return float4(NormPacked(PackedField(v, 0u, 10u, signed_), 10u, signed_, normalized),
                NormPacked(PackedField(v, 10u, 11u, signed_), 11u, signed_, normalized),
                NormPacked(PackedField(v, 21u, 11u, signed_), 11u, signed_, normalized), 0.0);
}
float4 Fetch10_11_11(uint addr, uint endian, bool signed_, bool normalized, uint slot) {
  uint v = SwapWith(RawStreamLoad(addr, slot), endian);
  return float4(NormPacked(PackedField(v, 0u, 11u, signed_), 11u, signed_, normalized),
                NormPacked(PackedField(v, 11u, 11u, signed_), 11u, signed_, normalized),
                NormPacked(PackedField(v, 22u, 10u, signed_), 10u, signed_, normalized), 0.0);
}
float4 Fetch32x4Int(uint addr, uint endian, uint count, bool signed_, bool normalized, uint slot) {
  // Loaded first and swapped once, so the loads can merge into a single vector fetch; the loop form
  // paid the endian chain separately on every component.
  uint4 w = RawStreamLoadN(addr, count, slot);
  w = SwapWith4(w, endian);
  float4 f = signed_ ? float4(int4(w)) : float4(w);
  if (normalized) f = signed_ ? max(-1.0, f / 2147483647.0) : f / 4294967295.0;
  // Components past `count` stay zero, as the loop's untouched lanes did.
  if (count < 4u) f.w = 0.0;
  if (count < 3u) f.z = 0.0;
  if (count < 2u) f.y = 0.0;
  return f;
}

// Existing handwritten shaders use the same decoders with shared memory only.
float4 FetchF32(uint a, uint e, uint c) { return FetchF32(a, e, c, NB_MAX_VERTEX_STREAMS + 1u); }
float4 FetchF16x2(uint a, uint e) { return FetchF16x2(a, e, NB_MAX_VERTEX_STREAMS + 1u); }
float4 FetchF16x4(uint a, uint e) { return FetchF16x4(a, e, NB_MAX_VERTEX_STREAMS + 1u); }
float4 Fetch2_10_10_10(uint a, uint e, bool s, bool n) { return Fetch2_10_10_10(a, e, s, n, NB_MAX_VERTEX_STREAMS + 1u); }
float4 Fetch8_8_8_8(uint a, uint e, bool s, bool n) { return Fetch8_8_8_8(a, e, s, n, NB_MAX_VERTEX_STREAMS + 1u); }
float4 Fetch16_16(uint a, uint e, bool s, bool n) { return Fetch16_16(a, e, s, n, NB_MAX_VERTEX_STREAMS + 1u); }
float4 Fetch16_16_16_16(uint a, uint e, bool s, bool n) { return Fetch16_16_16_16(a, e, s, n, NB_MAX_VERTEX_STREAMS + 1u); }
float4 Fetch11_11_10(uint a, uint e, bool s, bool n) { return Fetch11_11_10(a, e, s, n, NB_MAX_VERTEX_STREAMS + 1u); }
float4 Fetch10_11_11(uint a, uint e, bool s, bool n) { return Fetch10_11_11(a, e, s, n, NB_MAX_VERTEX_STREAMS + 1u); }
float4 Fetch32x4Int(uint a, uint e, uint c, bool s, bool n) { return Fetch32x4Int(a, e, c, s, n, NB_MAX_VERTEX_STREAMS + 1u); }

uint TexIndex(uint slot) { return tex_index[slot >> 2u][slot & 3u]; }
uint TexSwizzle(uint slot) { return tex_swizzle[slot >> 2u][slot & 3u]; }
uint PsTexSwizzle(uint slot) { return ps_tex_swizzle[slot >> 2u][slot & 3u]; }
float SwizzleComponent(float4 v, uint sel) {
  return sel == 0u ? v.x : sel == 1u ? v.y : sel == 2u ? v.z : sel == 3u ? v.w : (sel == 5u ? 1.0 : 0.0);
}
float4 ApplySwizzle(float4 v, uint swz) {
  if (swz == 0x688u) return v;  // x, y, z, w: the identity (0 | 1 << 3 | 2 << 6 | 3 << 9)
  return float4(SwizzleComponent(v, swz & 7u), SwizzleComponent(v, (swz >> 3u) & 7u),
                SwizzleComponent(v, (swz >> 6u) & 7u), SwizzleComponent(v, (swz >> 9u) & 7u));
}
// The fetch constant's exponent bias, packed by the command processor into bits 12..17 of the swizzle
// word as a 6-bit signed field. The hardware multiplies every component of every sample by 2^bias and
// the SDK's translator does the same at the tail of each texture fetch (dxbc_translator_fetch.cpp,
// "Apply the result exponent bias"); the interpreter uses ldexp for it. Leaving it out made every
// texture that carries a bias come out wrong by a hue-preserving power of two - black skies, black
// roofs and signposts, and blend masks that went hard where they should have been soft, because a sky
// or a mask is stored dark in a low-precision format and scaled up here.
float TexExpScale(uint swz) {
  int bias = int((swz >> 12u) & 0x3Fu);
  if (bias >= 32) bias -= 64;  // sign-extend the 6-bit field
  return exp2(float(bias));
}
#if NB_PIXEL_STAGE
uint TexSwizzleFor(uint slot) { return PsTexSwizzle(slot); }
#else
uint TexSwizzleFor(uint slot) { return TexSwizzle(slot); }
#endif
float TexExpScaleFor(uint slot) { return TexExpScale(TexSwizzleFor(slot)); }
// Signed fetch-constant LOD bias, in 1/32 mip units. Independent from the sample-result exponent bias.
float TexLodBiasFor(uint slot) { return float(asint(TexSwizzleFor(slot) << 4u) >> 22) / 32.0; }
// filter_mode from the fetch instruction: 0 = as the fetch constant says (sampler_sel bit1 = point),
// 1 = point, 2 = linear.
// Legacy fixtures may request explicit LOD with bit2. The command processor now leaves this clear:
// SDK sampler MinLOD/MaxLOD handles base-map limits without losing minification vs. magnification.
bool BaseMipOnly(uint slot) {
#if NB_EFFECTIVE_SAMPLERS
  return false;
#else
  return ((sampler_sel[slot >> 3u] >> ((slot & 7u) * 4u)) & 4u) != 0u;
#endif
}
uint SamplerFor(uint slot, uint filter_mode) {
#if !NB_EFFECTIVE_SAMPLERS
  uint bits = (sampler_sel[slot >> 3u] >> ((slot & 7u) * 4u)) & 15u;
  bool point_filter = filter_mode == 1u || (filter_mode == 0u && (bits & 2u) != 0u);
#endif
#if NB_PIXEL_STAGE
  uint packed = ps_tex_sampler[slot >> 2u][slot & 3u];
#else
  uint packed = tex_sampler[slot >> 2u][slot & 3u];
#endif
#if NB_EFFECTIVE_SAMPLERS
  return packed & 0xFFFFu;
#else
  return (packed >> (point_filter ? 0u : 16u)) & 0xFFFFu;
#endif
}
// The draw resolution scale. index_info.w bits 16..19 and 20..23 hold scale - 1, and are zero at 1x
// (NbCommandProcessor::PrepareGeometry).
float2 DrawScale() {
  return float2(float(((index_info.w >> 16u) & 0xFu) + 1u), float(((index_info.w >> 20u) & 0xFu) + 1u));
}
// Bit 28 of the slot's swizzle word: the bound texture is a resolution-scaled resolve, holding scale x
// the guest's texels.
bool TexResolutionScaled(uint slot) { return (TexSwizzleFor(slot) & 0x10000000u) != 0u; }
float2 TexHostSize(uint slot) {
  // Index zero is the SDK's all-zero null descriptor. Its dimensions are not texture data; use a
  // defined normalization size for an empty fetch, whose result Tex2D returns before sampling.
  if (TexIndex(slot) == 0u) return float2(1.0, 1.0);
  uint w, h, e;
  textures[TexIndex(slot)].GetDimensions(w, h, e);
  return float2(w, h);
}
// The guest's texel size, which unnormalized coordinates count in. For a resolution-scaled resolve the
// host size is divided back down. The SDK's translator scales the coordinates up instead, which
// normalizes to the same value. At 1x this is exactly the host size.
float2 TexSize(uint slot) {
  float2 size = TexHostSize(slot);
  return TexResolutionScaled(slot) ? size / DrawScale() : size;
}
// index_info.w bit 24 is set only at a draw resolution scale, when draw_resolution_scaled_texture_offsets is
// off. Fetch offsets and the nudge then count guest texels on resolution-scaled textures, as the SDK's
// translator does with it off. By default they count host texels.
bool OffsetsCountGuestTexels() { return (index_info.w & 0x1000000u) != 0u; }
// The size fetch offsets and the nudge are divided by. At 1x this is exactly the host size.
float2 TexOffsetSize(uint slot) { return OffsetsCountGuestTexels() ? TexSize(slot) : TexHostSize(slot); }
float3 TexSize3D(uint slot) {
  // Only the stacked-2D subset of tfetch3D is admitted. The SDK denormalizes layer coordinates with
  // the raw six-bit depth field even when the cache ultimately exposes a single array slice.
  return float3(TexSize(slot), float((TexSwizzleFor(slot) & 63u) + 1u));
}
float4 SampleArray(uint idx, uint s, float3 coord, bool lod0, float bias, float2 grad_x, float2 grad_y) {
#if NB_PIXEL_STAGE
  if (!lod0) return textures[idx].SampleGrad(texture_samplers[s], coord, grad_x, grad_y);
#endif
  return textures[idx].SampleLevel(texture_samplers[s], coord, bias);
}
// Xenos tfetch2D with normalized coordinates; texel offsets are given in texels. lod0 selects an explicit
// level-0 fetch (UseComputedLOD=false, and every vertex-shader fetch); bias is the instruction's LODBias.
float4 Tex2D(uint slot, float2 uv, uint filter_mode, float2 texel_offset, bool lod0, float bias) {
  uint idx = TexIndex(slot);
  // The generic path admits this only for a fetch without base/mip data pages resolved to the SDK's
  // null 2D descriptor, whose component mapping forces all four channels to zero. It may belong to
  // an inactive shader branch; no resource dimensions, sampler, or exponent adjustment are needed.
  if (idx == 0u) return float4(0.0, 0.0, 0.0, 0.0);
  uint s = SamplerFor(slot, filter_mode);
  // The guest quantizes coordinates to about 1/256 of a texel and the host rounds its own way, so a
  // point-sampled tap that lands exactly on a texel boundary can fall either side of it. Nudging every
  // coordinate by a small fraction of a texel settles that the way the reference emulator does; it is
  // what keeps shadow-map and blur taps from going blocky.
  // Offsets and the nudge count host texels, the SDK's draw_resolution_scaled_texture_offsets default, or guest
  // texels with it off (TexOffsetSize).
  float2 size = TexOffsetSize(slot);
  uv += (texel_offset + 1.5 / 1024.0) / size;
  bias += TexLodBiasFor(slot);
  float2 grad_x = 0.0, grad_y = 0.0;
#if NB_PIXEL_STAGE
  // Match the SDK's coarse derivatives and bias-as-gradient-scale path, including anisotropy.
  grad_x = ddx_coarse(uv) * exp2(bias);
  grad_y = ddy_coarse(uv) * exp2(bias);
#endif
  // No ApplySwizzle here: the SDK's texture cache bakes the fetch constant's swizzle into the bindless SRV
  // descriptor itself, so the sample already comes back swizzled (applying it again swapped R and B on
  // every k_8_8_8_8 texture and tinted the whole frame teal).
  return SampleArray(idx, s, float3(uv, 0.0), lod0 || BaseMipOnly(slot), bias, grad_x, grad_y) * TexExpScaleFor(slot);
}
// tfetch3D on a 2D texture array with equal volume min/mag filters. True volumes, differing volume
// filters and instruction volume overrides stay emulated. Array slices do not shrink with mip level.
float4 Tex3DImpl(uint slot, float3 uvw, uint filter_mode, float3 texel_offset, bool lod0, float bias,
                bool normalized) {
  uint idx = TexIndex(slot);
  if (idx == 0u) return float4(0.0, 0.0, 0.0, 0.0);
  uint s = SamplerFor(slot, filter_mode);
  float3 size = TexSize3D(slot);
  // Keep texel-space Z in texels, avoiding a divide/multiply round trip near layer boundaries.
  float2 uv;
  if (!TexResolutionScaled(slot)) {
    uv = normalized ? uvw.xy + (texel_offset.xy + 1.5 / 1024.0) / size.xy
                    : (uvw.xy + texel_offset.xy + 1.5 / 1024.0) / size.xy;
  } else {
    // Unnormalized X/Y count guest texels (size.xy). Offsets and the nudge are sized as in Tex2D.
    uv = (normalized ? uvw.xy : uvw.xy / size.xy) + (texel_offset.xy + 1.5 / 1024.0) / TexOffsetSize(slot);
  }
  float layer = (normalized ? uvw.z * size.z : uvw.z) + texel_offset.z + 1.5 / 1024.0;
  bool linear_layers = (TexSwizzleFor(slot) & 64u) != 0u;
  if (linear_layers) layer -= 0.5;
  float blend = linear_layers ? frac(layer) : 0.0;
  layer = floor(layer);
  bias += TexLodBiasFor(slot);
  float2 grad_x = 0.0, grad_y = 0.0;
#if NB_PIXEL_STAGE
  grad_x = ddx_coarse(uv) * exp2(bias);
  grad_y = ddy_coarse(uv) * exp2(bias);
#endif
  // D3D array slice selection clamps the integer layer; U/V addressing still comes from the SDK
  // sampler. Compute gradients before the per-pixel second-layer branch, as the SDK does.
  float4 result = SampleArray(idx, s, float3(uv, layer), lod0, bias, grad_x, grad_y);
  if (blend != 0.0) {
    float4 next_layer = SampleArray(idx, s, float3(uv, layer + 1.0), lod0, bias, grad_x, grad_y);
    result += (next_layer - result) * blend;
  }
  return result * TexExpScaleFor(slot);
}
float4 Tex3D(uint slot, float3 uvw, uint filter_mode, float3 texel_offset, bool lod0, float bias) {
  return Tex3DImpl(slot, uvw, filter_mode, texel_offset, lod0, bias, true);
}
float4 Tex3DTexel(uint slot, float3 uvw, uint filter_mode, float3 texel_offset, bool lod0, float bias) {
  return Tex3DImpl(slot, uvw, filter_mode, texel_offset, lod0, bias, false);
}
// cube ALU takes the first already-swizzled operand (.zzxy in ordinary assembly). Its result is
// (TC, SC, twice the signed major axis, face). The SDK breaks equal magnitudes in Z, Y, X order.
float4 XenosCube(float4 source) {
  float3 direction = source.zwx;
  float3 magnitude = abs(direction);
  if (magnitude.z >= magnitude.x && magnitude.z >= magnitude.y) {
    bool negative = direction.z < 0.0;
    return float4(-direction.y, negative ? -direction.x : direction.x,
                  2.0 * direction.z, negative ? 5.0 : 4.0);
  }
  if (magnitude.y >= magnitude.x) {
    bool negative = direction.y < 0.0;
    return float4(negative ? -direction.z : direction.z, direction.x,
                  2.0 * direction.y, negative ? 3.0 : 2.0);
  }
  bool negative = direction.x < 0.0;
  return float4(-direction.y, negative ? direction.z : -direction.z,
                2.0 * direction.x, negative ? 1.0 : 0.0);
}
// tfetchCube consumes normalized face-space (SC, TC) in 1..2, plus a numeric face index, rather
// than the direction consumed by a host TextureCube. Float-to-uint matches the SDK's ftou, then
// unsigned min limits the face to 5. Z offsets have no texture-coordinate rounding epsilon.
float3 XenosCubeDirection(float2 st, float face_value) {
  st = st * 2.0 - 3.0;
  uint face = min((uint)face_value, 5u);
  bool negative = (face & 1u) != 0u;
  uint axis = face >> 1u;
  if (axis == 0u) return float3(negative ? -1.0 : 1.0, -st.y, negative ? st.x : -st.x);
  if (axis == 1u) return float3(st.x, negative ? -1.0 : 1.0, negative ? -st.y : st.y);
  return float3(negative ? -st.x : st.x, -st.y, negative ? -1.0 : 1.0);
}
float2 TexSizeCube(uint slot) {
  uint width, height, levels;
  cube_textures[TexIndex(slot)].GetDimensions(0u, width, height, levels);
  return float2(width, height);
}
float4 TexCubeImpl(uint slot, float3 coord, uint filter_mode, float3 texel_offset,
                   bool lod0, float bias, bool normalized) {
  uint idx = TexIndex(slot);
  // The runtime normalizes an approved empty cube binding to the shared zero sentinel. Heap
  // descriptor zero is typed 2D, so return before even querying it through the cube declaration.
  if (idx == 0u) return float4(0.0, 0.0, 0.0, 0.0);
  float2 size = TexSizeCube(slot);
  // Retain a defined result for a typed null cube descriptor as well.
  if (any(size == 0.0)) return float4(0.0, 0.0, 0.0, 0.0);
  uint s = SamplerFor(slot, filter_mode);
  float2 st;
  if (!TexResolutionScaled(slot)) {
    st = normalized ? coord.xy + (texel_offset.xy + 1.5 / 1024.0) / size
                    : (coord.xy + texel_offset.xy + 1.5 / 1024.0) / size;
  } else {
    // A resolution-scaled cube: unnormalized coordinates count guest texels. Offsets and the nudge are sized
    // as in Tex2D.
    float2 guest = size / DrawScale();
    float2 offset_size = OffsetsCountGuestTexels() ? guest : size;
    st = (normalized ? coord.xy : coord.xy / guest) + (texel_offset.xy + 1.5 / 1024.0) / offset_size;
  }
  float3 direction = XenosCubeDirection(st, coord.z + texel_offset.z);
  bias += TexLodBiasFor(slot);
  float4 result;
#if NB_PIXEL_STAGE
  // Derive after remapping all three direction components, including when neighboring pixels
  // select different faces. This is the SDK's computed-LOD and anisotropic sampling path.
  float3 grad_x = ddx_coarse(direction) * exp2(bias);
  float3 grad_y = ddy_coarse(direction) * exp2(bias);
  if (!lod0) result = cube_textures[idx].SampleGrad(texture_samplers[s], direction, grad_x, grad_y);
  else
#endif
  result = cube_textures[idx].SampleLevel(texture_samplers[s], direction, bias);
  // Component swizzle and signed format are encoded in the SDK SRV, just as for 2D textures.
  return result * TexExpScaleFor(slot);
}
float4 TexCube(uint slot, float3 coord, uint filter_mode, float3 texel_offset, bool lod0, float bias) {
  return TexCubeImpl(slot, coord, filter_mode, texel_offset, lod0, bias, true);
}
float4 TexCubeTexel(uint slot, float3 coord, uint filter_mode, float3 texel_offset, bool lod0, float bias) {
  return TexCubeImpl(slot, coord, filter_mode, texel_offset, lod0, bias, false);
}
#if NB_PIXEL_STAGE
bool BoolConst(uint n) { return ((ps_bools[(n >> 7u) & 1u][(n >> 5u) & 3u] >> (n & 31u)) & 1u) != 0u; }
#else
bool BoolConst(uint n) { return ((bools[(n >> 7u) & 1u][(n >> 5u) & 3u] >> (n & 31u)) & 1u) != 0u; }
#endif
// The guest can point one pixel-shader interpolator register at the rasterizer's generated parameters
// instead (SQ_PROGRAM_CNTL.param_gen with SQ_CONTEXT_MISC.param_gen_pos, both in index_info.w). Built the
// way the SDK's translated pixel shaders build it: the pixel position, its sign bits carrying faceness
// and the point flag, and for points the sprite coordinates in zw.
float4 ParamGenOverride(uint reg_index, float4 interpolated, float4 pos, float2 ptcoord, bool front_face) {
  if ((index_info.w & 0x100u) == 0u || (index_info.w & 0xFFu) != reg_index) {
    return interpolated;
  }
  // The host pixel position, rounded down, then taken back to guest pixels at a draw resolution scale.
  // The SDK's translator does the same; the fractions keep a later tfetch sampling at the higher resolution.
  float2 xy = floor(pos.xy) / DrawScale();
  if (fetch0.w == 2u) {
    // A point: always front-facing (sign bit of x clear), the point flag is the sign of y.
    return float4(abs(xy.x), -abs(xy.y), saturate(ptcoord));
  }
  // A polygon: the sign of x carries faceness, y stays positive, z's sign bit would mark a line.
  return float4(front_face ? abs(xy.x) : -abs(xy.x), abs(xy.y), 0.0, 0.0);
}
// The SDK's translated vertex shaders apply the host viewport mapping the same way.
float4 HostPosition(float4 guest_pos) {
  return float4(guest_pos.xyz * ndc_scale.xyz + ndc_offset.xyz * guest_pos.w, guest_pos.w);
}
// Point sprites (primitive mode 2): six host vertices per point, two triangles (0,1,2)(2,1,3) of the
// corners (0,0) (1,0) (0,1) (1,1), the point coordinates the pixel shader reads in param_gen.zw. Sizes
// follow the SDK: the per-vertex diameter (oPts.x) clamped to PA_SU_POINT_MINMAX, else the constant
// PA_SU_POINT_SIZE; pass_params.xy = resolution scale / viewport extent (diameter in pixels -> NDC
// radius), pass_params.zw = constant diameter, alpha_test.w = PA_SU_POINT_MINMAX raw (12.4 radii).
float2 PointCoord(uint id) {
  if (fetch0.w != 2u) return float2(0.0, 0.0);
  uint t = id - (id / 6u) * 6u;
  uint corner = (t == 0u) ? 0u : (t == 1u || t == 4u) ? 1u : (t == 2u || t == 3u) ? 2u : 3u;
  return float2(float(corner & 1u), float(corner >> 1u));
}
float4 ApplyPointOffset(float4 clip_pos, uint id, float vertex_diameter, bool has_vertex_diameter) {
  if (fetch0.w != 2u) return clip_pos;
  float2 diameter = float2(asfloat(pass_params.z), asfloat(pass_params.w));
  if (has_vertex_diameter) {
    float min_d = float(alpha_test.w & 0xFFFFu) * (2.0 / 16.0);
    float max_d = float(alpha_test.w >> 16u) * (2.0 / 16.0);
    diameter = clamp(vertex_diameter, min_d, max_d).xx;
  }
  float2 coord = PointCoord(id);
  float2 offset = (coord * 2.0 - 1.0) * float2(1.0, -1.0);
  float2 radius_ndc = diameter * float2(asfloat(pass_params.x), asfloat(pass_params.y));
  clip_pos.xy += offset * radius_ndc * clip_pos.w;
  return clip_pos;
}
// RB_COLORCONTROL alpha test on the colour-0 alpha, the way the SDK's translated pixel shaders do it
// (xenos::CompareFunction: never, less, equal, less-equal, greater, not-equal, greater-equal, always).
void AlphaTest(float a) {
  if ((alpha_test.x & 8u) == 0u) return;
  float ref_value = asfloat(alpha_test.y);
  uint f = alpha_test.x & 7u;
  // ("pass" is an HLSL keyword.)
  bool keep = (f == 1u) ? (a < ref_value) : (f == 2u) ? (a == ref_value) : (f == 3u) ? (a <= ref_value) :
              (f == 4u) ? (a > ref_value) : (f == 5u) ? (a != ref_value) : (f == 6u) ? (a >= ref_value) :
              (f == 7u);
  if (!keep) discard;
}
