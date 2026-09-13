#include "offline_texture_cache.h"

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/shared_memory.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_upload_buffer_pool.h>

REXCVAR_DEFINE_BOOL(nb_native_texture_cache, true, "nb",
    "Reuse exact offline BC texture upload bytes when a texture pack is configured");
REXCVAR_DEFINE_STRING(nb_native_texture_pack, "", "nb",
    "Optional NBTEX001 pack from build_native_texture_pack.py; empty disables offline texture uploads");

namespace nb::gpu {
namespace {

constexpr size_t kMaxPackBytes = 128u * 1024u * 1024u;
constexpr size_t kMaxUploadBytes = 2u * 1024u * 1024u;  // SDK default pool page.
using Key = std::array<uint32_t, 7>;  // width,height,format,endian,tiled,pitch,guest bytes.
struct KeyHasher {
  size_t operator()(const Key& key) const { return XXH3_64bits(key.data(), sizeof(key)); }
};
struct Entry {
  std::vector<uint8_t> guest, blocks;
  uint64_t guest_hash;
};

uint32_t Read32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
         (uint32_t(p[3]) << 24);
}

uint32_t Crc32(const uint8_t* data, size_t size) {
  uint32_t value = 0xFFFFFFFFu;
  for (size_t i = 0; i < size; ++i) {
    value ^= data[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      value = (value >> 1) ^ (0xEDB88320u & (0u - (value & 1u)));
  }
  return ~value;
}

bool ValidKey(const Key& k, uint32_t block_bytes) {
  if (!k[0] || !k[1] || k[0] > 8192 || k[1] > 8192 || k[0] % 4 || k[1] % 4 ||
      (k[2] != 18 && k[2] != 19 && k[2] != 20) || k[3] > 3 || k[4] > 1 ||
      !k[5] || k[5] % 32 || k[5] > 16352 || !k[6] || k[6] > kMaxUploadBytes)
    return false;
  uint64_t tight = uint64_t(k[0] / 4) * (k[1] / 4) * (k[2] == 18 ? 8 : 16);
  uint64_t pitch = ((uint64_t(k[0] / 4) * (k[2] == 18 ? 8 : 16)) + 255) & ~uint64_t(255);
  return tight == block_bytes && pitch * (k[1] / 4) <= kMaxUploadBytes;
}

class Cache {
 public:
  std::unordered_map<Key, std::vector<Entry>, KeyHasher> entries;
  std::vector<uint8_t> snapshot;
  OfflineTextureCacheStats stats;
  std::string path;

  void Configure(const std::string& requested_path) {
    if (path == requested_path) return;
    path = requested_path;
    entries.clear();
    snapshot.clear();
    if (path.empty()) return;
    try {
      if (!Load()) {
        entries.clear();
        REXLOG_WARN("rexgpu-nb: offline texture pack rejected: {}", path);
      }
    } catch (const std::exception& error) {
      entries.clear();
      REXLOG_WARN("rexgpu-nb: offline texture pack unavailable: {} ({})", path, error.what());
    }
  }

 private:
  bool Load() {
    std::ifstream input(std::filesystem::u8path(path), std::ios::binary | std::ios::ate);
    if (!input) return false;
    const auto length = input.tellg();
    if (length < 16 || uint64_t(length) > kMaxPackBytes) return false;
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) return false;
    if (std::memcmp(bytes.data(), "NBTEX001", 8) != 0) return false;
    uint32_t count = Read32(bytes.data() + 8);
    if (!count || count > 4096 || Crc32(bytes.data() + 16, bytes.size() - 16) != Read32(bytes.data() + 12))
      return false;
    size_t offset = 16;
    for (uint32_t i = 0; i < count; ++i) {
      if (bytes.size() - offset < 32) return false;
      Key key;
      for (size_t word = 0; word < 7; ++word) key[word] = Read32(bytes.data() + offset + word * 4);
      uint32_t block_bytes = Read32(bytes.data() + offset + 28);
      offset += 32;
      if (!ValidKey(key, block_bytes) || uint64_t(key[6]) + block_bytes > bytes.size() - offset)
        return false;
      Entry entry;
      entry.guest.assign(bytes.data() + offset, bytes.data() + offset + key[6]);
      offset += key[6];
      entry.blocks.assign(bytes.data() + offset, bytes.data() + offset + block_bytes);
      offset += block_bytes;
      entry.guest_hash = XXH3_64bits(entry.guest.data(), entry.guest.size());
      // One exact guest/layout identity must not imply two different uploads.
      auto& bucket = entries[key];
      for (const auto& existing : bucket) {
        if (existing.guest == entry.guest && existing.blocks != entry.blocks) return false;
      }
      bucket.push_back(std::move(entry));
    }
    if (offset != bytes.size()) return false;
    REXLOG_INFO("rexgpu-nb: offline texture pack loaded: {} entries from {}", count, path);
    return true;
  }
};

Cache& GetCache() { static Cache cache; return cache; }

}  // namespace

OfflineTextureCacheStats GetOfflineTextureCacheStats() { return GetCache().stats; }

bool PrepareOfflineTextureUpload(rex::graphics::SharedMemory& shared_memory,
    rex::graphics::d3d12::D3D12CommandProcessor& cp,
    const OfflineTextureRequest& request, OfflineTextureUpload& upload) {
  if (!REXCVAR_GET(nb_native_texture_cache)) return false;
  Cache& cache = GetCache();
  cache.Configure(REXCVAR_GET(nb_native_texture_pack));
  Key key = {request.width, request.height, request.format, request.endian,
             request.tiled, request.pitch_texels, request.guest_bytes};
  auto found = cache.entries.find(key);
  if (found == cache.entries.end()) return false;
  ++cache.stats.candidates;
  try {
    cache.snapshot.resize(request.guest_bytes);
  } catch (const std::bad_alloc&) {
    return false;
  }
  if (!shared_memory.CopyCpuAuthoritativeRange(request.guest_address, request.guest_bytes,
                                               cache.snapshot.data())) {
    ++cache.stats.cpu_authority_refusals;
    return false;
  }
  const uint64_t hash = XXH3_64bits(cache.snapshot.data(), cache.snapshot.size());
  const Entry* matched = nullptr;
  for (const auto& entry : found->second) {
    if (entry.guest_hash == hash && entry.guest == cache.snapshot) { matched = &entry; break; }
  }
  if (!matched) { ++cache.stats.byte_misses; return false; }
  ++cache.stats.matches;

  const uint32_t block_bytes = request.format == 18 ? 8 : 16;
  const uint32_t row_bytes = request.width / 4 * block_bytes;
  const uint32_t row_pitch = (row_bytes + 255u) & ~255u;
  const uint32_t rows = request.height / 4;
  const size_t upload_bytes = size_t(row_pitch) * rows;
  if (upload_bytes > kMaxUploadBytes) return false;
  size_t offset = 0;
  ID3D12Resource* buffer = nullptr;
  // This SDK pool is reclaimed by completed FRAME, so use that same timeline.
  uint8_t* mapping = cp.GetConstantBufferPool().Request(cp.GetCurrentFrame(), upload_bytes,
      D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, &buffer, &offset, nullptr);
  if (!mapping) return false;
  std::memset(mapping, 0, upload_bytes);
  for (uint32_t row = 0; row < rows; ++row)
    std::memcpy(mapping + size_t(row) * row_pitch,
                matched->blocks.data() + size_t(row) * row_bytes, row_bytes);
  upload.buffer = buffer;
  upload.footprint.Offset = offset;
  upload.footprint.Footprint.Format = request.format == 18 ? DXGI_FORMAT_BC1_UNORM :
      request.format == 19 ? DXGI_FORMAT_BC2_UNORM : DXGI_FORMAT_BC3_UNORM;
  upload.footprint.Footprint.Width = request.width;
  upload.footprint.Footprint.Height = request.height;
  upload.footprint.Footprint.Depth = 1;
  upload.footprint.Footprint.RowPitch = row_pitch;
  ++cache.stats.prepared_uploads;
  cache.stats.upload_bytes += matched->blocks.size();
  if (cache.stats.prepared_uploads <= 8 || !(cache.stats.prepared_uploads % 64))
    REXLOG_INFO("rexgpu-nb: offline texture upload #{}: {}x{} BC{} ({} bytes, guest {:08X})",
        cache.stats.prepared_uploads, request.width, request.height, request.format - 17,
        matched->blocks.size(), request.guest_address);
  return true;
}

}  // namespace nb::gpu
