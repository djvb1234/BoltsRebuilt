// nb: direct host render target resolve, source addressing (src/gpu/vendored/UPSTREAM.md).
//
// The SDK's resolve dumps every render target sample the resolve covers into the EDRAM buffer
// (D3D12RenderTargetCache::GetOrCreateDumpPipeline) and then runs the resolve copy shader over that
// buffer. The direct resolve runs the same copy shader but answers each of its EDRAM buffer reads from
// the render target itself. This file is the inverse of the dump's addressing: given the 32-bit EDRAM
// word index the copy shader would read, it returns the render target pixel and guest sample the dump
// would have stored there.
//
// Only 32bpp color render targets are handled (the dump stores one 32-bit word per sample there). The
// function is written in the subset of HLSL that also compiles as C++ with the definitions in
// tools/test_direct_resolve_address.cpp, which checks it against a transcription of the dump shader.

#ifndef NB_DIRECT_RESOLVE_ADDRESS_HLSLI_
#define NB_DIRECT_RESOLVE_ADDRESS_HLSLI_

// source_info: render target base tile in bits 0-10, render target pitch in tiles (at 32bpp) in bits
// 11-20. msaa_samples_log2: 0 for 1x, 1 for 2x, 2 for 4x (xenos::MsaaSamples).
// Returns the render target pixel in x and y, and the guest sample index within the pixel in z, which
// for 2x MSAA is 0 for the top and 1 for the bottom sample, and for 4x MSAA is bit 0 horizontal and
// bit 1 vertical, as in the dump.
uint4 NbDirectResolveSourceLocation(uint address_ints, uint source_info, uint scale_x, uint scale_y,
                                    uint msaa_samples_log2) {
  uint tile_width = 80u * scale_x;
  uint tile_height = 16u * scale_y;
  uint tile_samples = tile_width * tile_height;
  // EDRAM addressing is periodic over 2048 tiles; the copy shader already wraps its addresses.
  uint edram_tile = (address_ints / tile_samples) & 2047u;
  uint sample_in_tile = address_ints % tile_samples;
  uint sample_y_in_tile = sample_in_tile / tile_width;
  uint sample_x_in_tile = sample_in_tile - sample_y_in_tile * tile_width;
  uint source_base_tiles = source_info & 2047u;
  uint source_pitch_tiles = (source_info >> 11u) & 1023u;
  // The dump subtracts the render target base from the unwrapped tile index, which is never below it.
  uint source_tile = (edram_tile - source_base_tiles) & 2047u;
  uint source_tile_y = source_tile / source_pitch_tiles;
  uint source_tile_x = source_tile - source_tile_y * source_pitch_tiles;
  uint sample_x = source_tile_x * tile_width + sample_x_in_tile;
  uint sample_y = source_tile_y * tile_height + sample_y_in_tile;
  uint sample_index = 0u;
  if (msaa_samples_log2 >= 2u) {
    sample_index = (sample_x & 1u) | ((sample_y & 1u) << 1u);
    sample_x >>= 1u;
    sample_y >>= 1u;
  } else if (msaa_samples_log2 == 1u) {
    sample_index = sample_y & 1u;
    sample_y >>= 1u;
  }
  return uint4(sample_x, sample_y, sample_index, 0u);
}

#endif  // NB_DIRECT_RESOLVE_ADDRESS_HLSLI_
