// Original, portable policy for skipping the stencil-bit passes of a depth transfer whose bit no source
// sample has (nb_transfer_stencil_bit_predication).
//
// Without a pixel-shader stencil reference (never on NVIDIA under D3D12), an EDRAM ownership transfer into a
// depth render target rebuilds stencil in eight passes, one per bit: the destination stencil is cleared to 0
// over the transfer rectangles, and pass j writes bit j with REPLACE on the samples whose source stencil has
// that bit, discarding (or, with nb_transfer_stencil_coverage, leaving out of SV_Coverage) every other sample.
// A pass whose bit no source sample has therefore writes nothing at all, and still rasterizes and shades every
// destination sample or pixel of the rectangles.
//
// Before the passes of a depth-to-stencil-bit draw, a compute probe ORs the stencil of every source sample
// the draw can read into eight 64-bit predicates, one per bit, and each pass is recorded under
// SetPredication(EQUAL_ZERO) on its bit's predicate. A skipped pass is one that would have written nothing,
// so the destination is identical by construction. The probe reads a superset of what the transfer shader
// reads: whole source pixels (every sample) of every source tile in the transferred EDRAM range, ignoring the
// resolve-clear cutout. A superset can only keep a pass that could have been skipped.
#pragma once

#include <cstdint>
#include <string_view>

namespace nb::gpu {

// EDRAM tile count (xenos::kEdramTileCount), repeated so this header needs no SDK include.
inline constexpr uint32_t kNbEdramTileCount = 2048;

// Each probed draw owns one slot of eight 64-bit predicates, bit j at byte j * 8 of the slot.
inline constexpr uint32_t kNbStencilPredicateBytesPerBit = 8;
inline constexpr uint32_t kNbStencilPredicateSlotBytes = 8 * kNbStencilPredicateBytesPerBit;
// Probed draws per destination render target within one PerformTransfersAndResolveClears call. Draws beyond
// this keep their unpredicated passes.
inline constexpr uint32_t kNbStencilPredicateSlots = 256;
inline constexpr uint32_t kNbStencilPredicateBufferBytes = kNbStencilPredicateSlots * kNbStencilPredicateSlotBytes;

constexpr uint32_t NbStencilPredicateOffset(uint32_t slot, uint32_t bit) {
  return slot * kNbStencilPredicateSlotBytes + bit * kNbStencilPredicateBytesPerBit;
}

// A transfer's [start_tiles, end_tiles) is in absolute EDRAM tiles within [0, kNbEdramTileCount]. The
// transfer shader reads source tile (absolute - source_base) & (kNbEdramTileCount - 1), and the SDK's
// Transfer::GetRangeRectangles lays out a range in the same local numbering only when the range does not
// straddle the base (it asserts start >= base || end <= base; a range below the base is the tail after
// addressing wrapping). So the range is split at the source base into at most two such pieces.
struct NbTileRange {
  uint32_t start;
  uint32_t end;
};
constexpr uint32_t NbSplitTransferRangeAtSourceBase(uint32_t start_tiles, uint32_t end_tiles,
                                                    uint32_t source_base_tiles, NbTileRange pieces[2]) {
  uint32_t count = 0;
  if (start_tiles >= end_tiles || end_tiles > kNbEdramTileCount) return 0;
  if (start_tiles < source_base_tiles) {
    const uint32_t tail_end = end_tiles < source_base_tiles ? end_tiles : source_base_tiles;
    pieces[count++] = {start_tiles, tail_end};
  }
  if (end_tiles > source_base_tiles) {
    const uint32_t head_start = start_tiles > source_base_tiles ? start_tiles : source_base_tiles;
    pieces[count++] = {head_start, end_tiles};
  }
  return count;
}

// The local tile index GetRangeRectangles uses for an absolute tile of a piece (its local_offset rule).
constexpr uint32_t NbRangeLocalTile(uint32_t absolute_tile, uint32_t piece_start, uint32_t base_tiles) {
  return (piece_start < base_tiles ? kNbEdramTileCount : 0) + absolute_tile - base_tiles;
}

// Root signature: 0 = descriptor table with the source stencil SRV (t0), 1 = root raw-buffer UAV with the
// predicates (u0), 2 = these root constants (b0).
struct NbStencilProbeConstants {
  uint32_t x;  // first source pixel, scaled
  uint32_t y;
  uint32_t width;  // source pixels, scaled
  uint32_t height;
  uint32_t slot_offset;  // byte offset of the draw's eight predicates
  uint32_t sample_count;  // resource samples; ignored by the single-sampled form
};
inline constexpr uint32_t kNbStencilProbeGroupSize = 8;

// Compiled twice: with NB_STENCIL_PROBE_MS defined for multisampled sources (every resource sample is read,
// which for 2x emulated as 4x includes the two unused samples; that can only keep a pass) and without.
// The stencil SRVs are X24_TYPELESS_G8_UINT or X32_TYPELESS_G8X24_UINT, stencil in .y, as the transfer
// shader loads it. Writers only ever store 1 into a zeroed predicate, so racing stores are benign.
inline constexpr std::string_view kNbStencilProbeHlsl = R"hlsl(
cbuffer NbStencilProbe : register(b0) {
  uint4 nb_rect;
  uint nb_slot_offset;
  uint nb_sample_count;
};
#ifdef NB_STENCIL_PROBE_MS
Texture2DMS<uint2> nb_stencil : register(t0);
#else
Texture2D<uint2> nb_stencil : register(t0);
#endif
RWByteAddressBuffer nb_predicates : register(u0);
groupshared uint nb_group_bits;
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex) {
  if (index == 0) nb_group_bits = 0;
  GroupMemoryBarrierWithGroupSync();
  uint bits = 0;
  if (id.x < nb_rect.z && id.y < nb_rect.w) {
    int2 p = int2(nb_rect.xy + id.xy);
#ifdef NB_STENCIL_PROBE_MS
    for (uint s = 0; s < nb_sample_count; ++s) bits |= nb_stencil.Load(p, s).y;
#else
    bits = nb_stencil.Load(int3(p, 0)).y;
#endif
  }
  bits &= 0xFF;
  if (bits != 0) InterlockedOr(nb_group_bits, bits);
  GroupMemoryBarrierWithGroupSync();
  if (index < 8 && (nb_group_bits & (1u << index)) != 0) {
    nb_predicates.Store(nb_slot_offset + index * 8, 1u);
  }
}
)hlsl";

}  // namespace nb::gpu
