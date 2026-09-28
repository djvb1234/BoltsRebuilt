// Original portable controls for nb_transfer_stencil_bit_predication: the split of a transfer's EDRAM range
// at the source base, checked against the transfer shader's source tile addressing and the local numbering
// of the SDK's Transfer::GetRangeRectangles (both transcribed here), and the predicate slot layout.
// No SDK, Windows, D3D12 device or GPU is required.
#include "../src/gpu/native/native_transfer_stencil_predication.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
uint64_t checks = 0;
void Check(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}

using nb::gpu::kNbEdramTileCount;

// render_target_cache.cpp transfer pixel shader: r1.w = (dest local tile + dest base - source base) & 2047,
// where the dest local tile is the tile's offset from the destination base (the destination's own wrapping
// rule), so the result depends only on the absolute tile.
uint32_t ShaderSourceLocalTile(uint32_t absolute_tile, uint32_t dest_base, uint32_t source_base) {
  const uint32_t dest_local = (absolute_tile + kNbEdramTileCount - dest_base) & (kNbEdramTileCount - 1);
  return uint32_t(int32_t(dest_local) + int32_t(dest_base) - int32_t(source_base)) & (kNbEdramTileCount - 1);
}

// The asserts GetRangeRectangles makes on each range it is given.
bool RangeAcceptedByGetRangeRectangles(uint32_t start, uint32_t end, uint32_t base) {
  return start < kNbEdramTileCount && end <= kNbEdramTileCount && start <= end &&
         (start >= base || end <= base);
}

void CheckRange(uint32_t start, uint32_t end, uint32_t source_base, uint32_t dest_base) {
  nb::gpu::NbTileRange pieces[2] = {};
  const uint32_t count = nb::gpu::NbSplitTransferRangeAtSourceBase(start, end, source_base, pieces);
  Check(count >= 1 && count <= 2, "a non-empty range gives one or two pieces");
  uint32_t covered = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const auto& piece = pieces[i];
    Check(piece.start < piece.end, "pieces are non-empty");
    Check(RangeAcceptedByGetRangeRectangles(piece.start, piece.end, source_base),
          "every piece satisfies GetRangeRectangles's asserts");
    Check(piece.start >= start && piece.end <= end, "pieces stay inside the range");
    if (i) Check(pieces[i - 1].end == piece.start, "pieces are contiguous and ordered");
    covered += piece.end - piece.start;
    for (uint32_t tile = piece.start; tile < piece.end; ++tile) {
      const uint32_t local = nb::gpu::NbRangeLocalTile(tile, piece.start, source_base);
      Check(local < kNbEdramTileCount, "local tiles stay inside the source's EDRAM span");
      Check(local == ShaderSourceLocalTile(tile, dest_base, source_base),
            "the probed local tile is the one the transfer shader reads");
    }
  }
  Check(pieces[0].start == start && covered == end - start, "pieces cover the whole range");
}
}  // namespace

int main() {
  try {
    nb::gpu::NbTileRange pieces[2];
    Check(nb::gpu::NbSplitTransferRangeAtSourceBase(5, 5, 0, pieces) == 0, "an empty range has no piece");
    Check(nb::gpu::NbSplitTransferRangeAtSourceBase(6, 5, 0, pieces) == 0, "an inverted range has no piece");
    Check(nb::gpu::NbSplitTransferRangeAtSourceBase(0, kNbEdramTileCount + 1, 0, pieces) == 0,
          "a range past the EDRAM has no piece");

    const uint32_t bases[] = {0, 1, 80, 160, 300, 1023, 1024, 1500, 2000, 2047};
    const uint32_t points[] = {0, 1, 79, 80, 81, 159, 160, 299, 300, 301, 1023, 1024, 1499, 1500,
                               1999, 2000, 2046, 2047, 2048};
    for (uint32_t source_base : bases) {
      for (uint32_t dest_base : bases) {
        for (uint32_t start : points) {
          for (uint32_t end : points) {
            if (start < end && start < kNbEdramTileCount) CheckRange(start, end, source_base, dest_base);
          }
        }
      }
    }

    Check(nb::gpu::NbStencilPredicateOffset(0, 0) == 0, "slot 0 bit 0 is at the start");
    for (uint32_t slot = 0; slot < nb::gpu::kNbStencilPredicateSlots; ++slot) {
      for (uint32_t bit = 0; bit < 8; ++bit) {
        const uint32_t offset = nb::gpu::NbStencilPredicateOffset(slot, bit);
        Check(offset % 8 == 0, "SetPredication offsets are 8-byte aligned");
        Check(offset + 8 <= nb::gpu::kNbStencilPredicateBufferBytes, "predicates fit the buffer");
        Check(offset == (slot * 8 + bit) * 8, "predicates are dense and distinct");
      }
    }
    Check(sizeof(nb::gpu::NbStencilProbeConstants) == 6 * sizeof(uint32_t),
          "the root constants match the shader's cbuffer");
    Check(nb::gpu::kNbStencilProbeHlsl.find("numthreads(8, 8, 1)") != std::string_view::npos &&
              nb::gpu::kNbStencilProbeGroupSize == 8,
          "the dispatch group size matches the shader");
    Check(nb::gpu::kNbStencilProbeHlsl.find(".y") != std::string_view::npos,
          "stencil is read from .y as the transfer shader does");
  } catch (const std::exception& error) {
    std::cerr << "FAILED after " << checks << " checks: " << error.what() << '\n';
    return 1;
  }
  std::cout << "native transfer stencil predication: " << checks << " checks passed\n";
  return 0;
}
