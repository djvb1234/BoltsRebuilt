// Checks the direct resolve's source addressing (src/gpu/vendored/src/graphics/d3d12/direct_resolve/
// nb_direct_resolve_address.hlsli, compiled here as C++) against a transcription of the SDK's render
// target dump shader (D3D12RenderTargetCache::GetOrCreateDumpPipeline, 32bpp color): every sample the
// dump stores must be found again, at the same render target pixel and sample, from the EDRAM word the
// dump stored it in.
#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace hlsl {
using uint = uint32_t;
struct uint4 {
  uint x, y, z, w;
  uint4(uint x_, uint y_, uint z_, uint w_) : x(x_), y(y_), z(z_), w(w_) {}
};
#include "../src/gpu/vendored/src/graphics/d3d12/direct_resolve/nb_direct_resolve_address.hlsli"
}  // namespace hlsl

namespace {
uint64_t checks = 0;
void Check(bool value, const char* reason, uint32_t a = 0, uint32_t b = 0) {
  ++checks;
  if (!value) {
    std::cerr << "FAIL: " << reason << " (" << a << ", " << b << ")\n";
    std::exit(1);
  }
}

struct Dump {
  uint32_t scale_x, scale_y, msaa_log2;
  uint32_t rt_base, rt_pitch;
  // The resolve's pitch, which the dump rectangle walks, may differ from the render target's.
  uint32_t dump_pitch;
  // dispatch_first_tile: dump base plus the dispatch offset, may exceed 2047 (EDRAM wrapping).
  uint32_t first_tile, width_tiles, height_tiles;
};

// One dump dispatch, thread by thread, as the generated DXBC computes it.
void CheckDispatch(const Dump& d) {
  const uint32_t tile_width = 80 * d.scale_x;
  const uint32_t tile_height = 16 * d.scale_y;
  const uint32_t source_info = d.rt_base | (d.rt_pitch << 11);
  for (uint32_t ty = 0; ty < d.height_tiles * tile_height; ++ty) {
    for (uint32_t tx = 0; tx < d.width_tiles * tile_width; ++tx) {
      uint32_t x = tx % tile_width, tile_x = tx / tile_width;
      uint32_t y = ty % tile_height, tile_y = ty / tile_height;
      uint32_t tile_relative = tile_y * d.dump_pitch + tile_x;
      uint32_t tile_unwrapped = d.first_tile + tile_relative;
      uint32_t tile_wrapped = tile_unwrapped & 2047;
      uint32_t address = tile_wrapped * (d.scale_x * d.scale_y * 80 * 16) + x;
      address = y * tile_width + address;
      uint32_t source_tile = tile_unwrapped - d.rt_base;
      uint32_t source_tile_y = source_tile / d.rt_pitch;
      uint32_t source_tile_x = source_tile % d.rt_pitch;
      uint32_t sample_x = source_tile_x * tile_width + x;
      uint32_t sample_y = source_tile_y * tile_height + y;
      uint32_t sample = 0;
      if (d.msaa_log2 >= 2) {
        sample = (sample_x & 1) | ((sample_y & 1) << 1);
        sample_x >>= 1;
        sample_y >>= 1;
      } else if (d.msaa_log2 == 1) {
        sample = sample_y & 1;
        sample_y >>= 1;
      }
      hlsl::uint4 found = hlsl::NbDirectResolveSourceLocation(address, source_info, d.scale_x,
                                                              d.scale_y, d.msaa_log2);
      Check(found.x == sample_x, "pixel x", found.x, sample_x);
      Check(found.y == sample_y, "pixel y", found.y, sample_y);
      Check(found.z == sample, "sample", found.z, sample);
    }
  }
}
}  // namespace

int main() {
  const uint32_t scales[][2] = {{1, 1}, {2, 2}, {3, 3}, {4, 2}, {2, 1}};
  for (const auto& scale : scales) {
    for (uint32_t msaa = 0; msaa <= 2; ++msaa) {
      // The game's scene bands: 1280x384 at 2x, 16 tiles wide, 48 tile rows, at 0 and 768.
      CheckDispatch({scale[0], scale[1], msaa, 0, 16, 16, 0, 16, 48});
      CheckDispatch({scale[0], scale[1], msaa, 768, 16, 16, 768, 16, 48});
      // A resolve of part of a render target, starting mid-row, with a wider resolve pitch.
      CheckDispatch({scale[0], scale[1], msaa, 100, 9, 12, 100 + 12 * 3 + 2, 5, 4});
      // A render target that wraps past the end of the EDRAM.
      CheckDispatch({scale[0], scale[1], msaa, 2040, 8, 8, 2040 + 8, 8, 3});
      // Narrow pitch, single tile rows.
      CheckDispatch({scale[0], scale[1], msaa, 1536, 1, 1, 1536 + 5, 1, 1});
    }
  }
  std::cout << "direct resolve address: " << checks << " checks passed\n";
  return 0;
}
