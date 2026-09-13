// Original optional upload data cache for verified single-base BC textures.
#pragma once

#include <cstdint>
#include <d3d12.h>

namespace rex::graphics {
class SharedMemory;
namespace d3d12 { class D3D12CommandProcessor; }
}

namespace nb::gpu {

struct OfflineTextureRequest {
  uint32_t width, height, format, endian, tiled, pitch_texels;
  uint32_t guest_address, guest_bytes;
};

struct OfflineTextureUpload {
  ID3D12Resource* buffer = nullptr;  // SDK upload pool owns submission lifetime.
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
};

struct OfflineTextureCacheStats {
  uint64_t candidates = 0, matches = 0, cpu_authority_refusals = 0;
  uint64_t byte_misses = 0, prepared_uploads = 0, upload_bytes = 0;
};

// Called only on the command processor thread, after SDK RequestRanges.
// A miss leaves the original SDK texture upload path available. This only
// prepares immutable upload bytes; the SDK caller records the copy/barriers.
bool PrepareOfflineTextureUpload(rex::graphics::SharedMemory& shared_memory,
    rex::graphics::d3d12::D3D12CommandProcessor& command_processor,
    const OfflineTextureRequest& request, OfflineTextureUpload& upload);
OfflineTextureCacheStats GetOfflineTextureCacheStats();

}  // namespace nb::gpu
