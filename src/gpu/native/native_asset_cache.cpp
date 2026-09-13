#include "native_asset_cache.h"
#include "native_asset_mapping_table.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstring>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include <bcrypt.h>
#include <wrl/client.h>
#include <rex/graphics/shared_memory.h>
#include <rex/graphics/d3d12/command_processor.h>

#pragma comment(lib, "bcrypt.lib")

namespace nb::gpu {
namespace {
constexpr uint32_t kPhysicalBytes = 1u << 29;
constexpr uint32_t kPageShift = 12;
constexpr uint32_t kPageCount = kPhysicalBytes >> kPageShift;
constexpr uint32_t kMaxEntries = 100000;
constexpr uint32_t kLegacyMaxMappings = 8192;
constexpr size_t kHeaderSize = 80, kRecordSize = 64;

uint32_t U32(const uint8_t* p) {
  uint32_t v; std::memcpy(&v, p, sizeof(v)); return v;
}
uint64_t U64(const uint8_t* p) {
  uint64_t v; std::memcpy(&v, p, sizeof(v)); return v;
}
bool Contains(uint64_t outer, uint64_t offset, uint64_t length) {
  return offset <= outer && length <= outer - offset;
}
// Candidate accelerator only, never identity: every candidate is memcmp'd.
uint64_t Fingerprint(std::span<const uint8_t> bytes) {
  uint64_t h = 14695981039346656037ull;
  const size_t n = std::min<size_t>(32, bytes.size());
  for (size_t i = 0; i < n; ++i) h = (h ^ bytes[i]) * 1099511628211ull;
  for (size_t i = bytes.size() - n; i < bytes.size(); ++i)
    h = (h ^ bytes[i]) * 1099511628211ull;
  return h;
}
class Sha256 {
 public:
  Sha256() {
    if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
      throw std::runtime_error("SHA256 provider unavailable");
  }
  ~Sha256() { if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0); }
  std::array<uint8_t, 32> Hash(std::span<const uint8_t> bytes) {
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (bytes.size() > ULONG_MAX ||
        BCryptCreateHash(algorithm_, &hash, nullptr, 0, nullptr, 0, 0) < 0)
      throw std::runtime_error("SHA256 initialization failed");
    std::array<uint8_t, 32> result{};
    const bool ok = BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()),
                                  static_cast<ULONG>(bytes.size()), 0) >= 0 &&
                    BCryptFinishHash(hash, result.data(), 32, 0) >= 0;
    BCryptDestroyHash(hash);
    if (!ok) throw std::runtime_error("SHA256 calculation failed");
    return result;
  }
 private:
  BCRYPT_ALG_HANDLE algorithm_ = nullptr;
};
struct Entry { uint32_t offset, length; uint64_t fingerprint; };
struct CandidateKey {
  uint32_t length;
  uint64_t fingerprint;
  bool operator==(const CandidateKey&) const = default;
};
struct CandidateHash {
  size_t operator()(const CandidateKey& k) const {
    return size_t(k.fingerprint ^ (uint64_t(k.length) * 0x9e3779b97f4a7c15ull));
  }
};
}

struct NativeAssetCache::Impl {
  rex::graphics::SharedMemory* shared = nullptr;
  rex::graphics::SharedMemory::GlobalWatchHandle watch = nullptr;
  Microsoft::WRL::ComPtr<ID3D12Resource> gpu, staging;
  std::vector<uint8_t> bytes;
  std::vector<Entry> entries;
  std::unordered_map<CandidateKey, std::vector<uint32_t>, CandidateHash> candidates;
  std::unordered_map<uint32_t, bool> lengths;
  std::unique_ptr<std::atomic<uint64_t>[]> page_versions;
  std::atomic<uint64_t> sequence{0};
  struct Mapping { uint64_t version; Binding binding; };
  using MappingTable = NativeAssetMappingTable<Mapping, 65536>;
  std::unique_ptr<MappingTable> mappings;
  std::unordered_map<uint64_t, Mapping> legacy_mappings;
  std::vector<uint8_t> snapshot;
  uint64_t frame = UINT64_MAX;
  bool legacy_mode = false;
  bool uploaded = false;
  Stats counters;

  static void Invalidated(const std::unique_lock<std::recursive_mutex>&,
                          void* context, uint32_t first, uint32_t last, bool) {
    auto& self = *static_cast<Impl*>(context);
    if (first >= kPhysicalBytes || last < first) return;
    last = std::min(last, kPhysicalBytes - 1);
    const uint64_t version = self.sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    for (uint32_t page = first >> kPageShift; page <= (last >> kPageShift); ++page)
      self.page_versions[page].store(version, std::memory_order_release);
  }
  uint64_t Version(uint32_t base, uint32_t length) const {
    uint64_t version = 0;
    for (uint32_t page = base >> kPageShift;
         page <= ((base + length - 1) >> kPageShift); ++page)
      version = std::max(version, page_versions[page].load(std::memory_order_acquire));
    return version;
  }
};

NativeAssetCache::NativeAssetCache() : impl_(std::make_unique<Impl>()) {}
NativeAssetCache::~NativeAssetCache() {
  if (impl_->watch && impl_->shared) impl_->shared->UnregisterGlobalWatch(impl_->watch);
}

bool NativeAssetCache::Initialize(ID3D12Device* device, rex::graphics::SharedMemory& shared,
                                  const std::filesystem::path& pack, std::string* error) {
  if (impl_->gpu) {
    if (error) *error = "cache is already initialized; GPU-idle Shutdown is required";
    return false;
  }
  Shutdown();
  try {
    if (!device) throw std::runtime_error("no D3D12 device");
    const uint64_t file_size = std::filesystem::file_size(pack);
    constexpr uint64_t max_file = uint64_t(kPhysicalBytes) + uint64_t(kMaxEntries) * kRecordSize + 256;
    if (file_size < kHeaderSize || file_size > max_file)
      throw std::runtime_error("asset pack size is outside the bounded format");
    std::ifstream input(pack, std::ios::binary);
    std::array<uint8_t, kHeaderSize> header{};
    if (!input.read(reinterpret_cast<char*>(header.data()), header.size()))
      throw std::runtime_error("asset pack header read failed");
    if (std::memcmp(header.data(), "NBGEOM1\0", 8) || U32(header.data() + 8) != 1 ||
        U32(header.data() + 12) != kHeaderSize || U32(header.data() + 20) != kRecordSize)
      throw std::runtime_error("unsupported asset pack format");
    const uint32_t count = U32(header.data() + 16);
    const uint64_t table_offset = U64(header.data() + 24);
    const uint64_t data_offset = U64(header.data() + 32);
    const uint64_t data_size = U64(header.data() + 40);
    const uint64_t table_size = uint64_t(count) * kRecordSize;
    if (!count || count > kMaxEntries || table_offset != kHeaderSize ||
        !Contains(file_size, table_offset, table_size) || (data_offset & 255) ||
        data_offset != ((table_offset + table_size + 255) & ~uint64_t(255)) ||
        !data_size || data_size > kPhysicalBytes || !Contains(file_size, data_offset, data_size) ||
        data_offset + data_size != file_size)
      throw std::runtime_error("asset pack table/data bounds are invalid");
    std::vector<uint8_t> table(static_cast<size_t>(table_size));
    if (!input.read(reinterpret_cast<char*>(table.data()), table.size()))
      throw std::runtime_error("asset pack table read failed");
    Sha256 sha;
    if (std::memcmp(sha.Hash(table).data(), header.data() + 48, 32))
      throw std::runtime_error("asset pack table checksum mismatch");
    auto& p = *impl_;
    p.bytes.resize(static_cast<size_t>(data_size));
    input.seekg(static_cast<std::streamoff>(data_offset));
    if (!input.read(reinterpret_cast<char*>(p.bytes.data()), p.bytes.size()))
      throw std::runtime_error("asset pack data read failed");
    uint64_t end = 0;
    for (uint32_t i = 0; i < count; ++i) {
      const uint8_t* row = table.data() + size_t(i) * kRecordSize;
      const uint64_t offset = U64(row);
      const uint32_t length = U32(row + 8), kinds = U32(row + 12);
      if (!length || !kinds || (kinds & ~3u) || U64(row + 56) ||
          offset != ((end + 3) & ~uint64_t(3)) || !Contains(data_size, offset, length))
        throw std::runtime_error("asset record bounds/order/flags are invalid");
      for (uint64_t pad = end; pad < offset; ++pad)
        if (p.bytes[static_cast<size_t>(pad)]) throw std::runtime_error("nonzero asset padding");
      std::span<const uint8_t> raw(p.bytes.data() + offset, length);
      if (std::memcmp(sha.Hash(raw).data(), row + 24, 32) || Fingerprint(raw) != U64(row + 16))
        throw std::runtime_error("asset bytes checksum mismatch");
      p.entries.push_back({static_cast<uint32_t>(offset), length, U64(row + 16)});
      p.candidates[{length, U64(row + 16)}].push_back(i);
      p.lengths[length] = true;
      end = offset + length;
    }
    if (end != data_size) throw std::runtime_error("unclaimed asset bytes");
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = (data_size + 3) & ~uint64_t(3);
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&p.gpu))))
      throw std::runtime_error("immutable default arena allocation failed");
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&p.staging))))
      throw std::runtime_error("immutable staging arena allocation failed");
    void* mapped = nullptr;
    const D3D12_RANGE no_reads{0, 0};
    if (FAILED(p.staging->Map(0, &no_reads, &mapped)))
      throw std::runtime_error("immutable arena staging map failed");
    std::memcpy(mapped, p.bytes.data(), p.bytes.size());
    if (desc.Width > data_size)
      std::memset(static_cast<uint8_t*>(mapped) + data_size, 0, size_t(desc.Width - data_size));
    p.staging->Unmap(0, nullptr);
    p.gpu->SetName(L"nb local immutable geometry assets");
    p.staging->SetName(L"nb local immutable geometry staging");
    p.page_versions = std::make_unique<std::atomic<uint64_t>[]>(kPageCount);
    for (uint32_t i = 0; i < kPageCount; ++i) p.page_versions[i].store(0);
    p.mappings = std::make_unique<Impl::MappingTable>();
    p.shared = &shared;
    p.watch = shared.RegisterGlobalWatch(Impl::Invalidated, &p);
    if (!p.watch) throw std::runtime_error("asset invalidation watch registration failed");
    p.counters.assets = count;
    p.counters.arena_bytes = data_size;
    return true;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    Shutdown();
    return false;
  }
}

bool NativeAssetCache::EnsureUploaded(rex::graphics::d3d12::D3D12CommandProcessor& cp) {
  auto& p = *impl_;
  if (!p.gpu) return false;
  if (!p.uploaded) {
    cp.SubmitBarriers();
    cp.GetDeferredCommandList().D3DCopyBufferRegion(p.gpu.Get(), 0, p.staging.Get(), 0,
                                                  p.gpu->GetDesc().Width);
    cp.PushTransitionBarrier(p.gpu.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cp.SubmitBarriers();
    p.uploaded = true;
  }
  return true;
}

NativeAssetCache::Binding NativeAssetCache::Resolve(uint32_t base, uint32_t length,
                                                   uint64_t frame, bool legacy) {
  auto& p = *impl_;
  ++p.counters.resolves;
  if (!p.gpu || !length || (base & 3) || base >= kPhysicalBytes || length > kPhysicalBytes - base ||
      !p.lengths.contains(length)) {
    ++p.counters.misses;
    return {};
  }
  if (p.frame != frame || p.legacy_mode != legacy) {
    if (p.mappings->Clear()) ++p.counters.mapping_generation_wraps;
    // Some unordered_map implementations clear their retained bucket array
    // even when no nodes remain. Do not pay that on optimized-only frames.
    if (!p.legacy_mappings.empty()) p.legacy_mappings.clear();
    if (p.frame != frame) ++p.counters.frame_resets;
    if (p.legacy_mode != legacy) ++p.counters.mode_switches;
    p.frame = frame;
    p.legacy_mode = legacy;
  }
  const uint64_t key = (uint64_t(base) << 32) | length;
  const uint64_t before = p.Version(base, length);
  Impl::MappingTable::Lookup lookup;
  const Impl::Mapping* prior = nullptr;
  if (legacy) {
    auto found = p.legacy_mappings.find(key);
    if (found != p.legacy_mappings.end()) prior = &found->second;
  } else {
    lookup = p.mappings->Find(key);
    p.counters.mapping_collision_steps += lookup.probes - 1;
    p.counters.mapping_max_probe = std::max<uint64_t>(p.counters.mapping_max_probe, lookup.probes);
    prior = lookup.value;
  }
  if (prior && prior->version == before) {
    if (prior->binding.enabled) { ++p.counters.hits; ++p.counters.cached_hits; }
    else ++p.counters.misses;
    return prior->binding;
  }
  try {
    // Shrinking a vector then growing it zero-initializes the same bytes before
    // memcpy overwrites them. Optimized mode retains its largest initialized
    // extent; the explicit span below still limits matching to this request.
    if (length > p.snapshot.size()) {
      ++p.counters.snapshot_growths;
      p.counters.snapshot_growth_bytes += length - p.snapshot.size();
    }
    if (legacy || length > p.snapshot.size()) p.snapshot.resize(length);
    ++p.counters.snapshots;
    if (!p.shared->CopyCpuAuthoritativeRange(base, length, p.snapshot.data())) {
      ++p.counters.authority_refusals;
      return {};
    }
    p.counters.snapshot_bytes += length;
    Binding result;
    const CandidateKey candidate_key{length, Fingerprint({p.snapshot.data(), length})};
    if (auto found = p.candidates.find(candidate_key); found != p.candidates.end()) {
      for (uint32_t index : found->second) {
        const auto& entry = p.entries[index];
        if (!std::memcmp(p.snapshot.data(), p.bytes.data() + entry.offset, length)) {
          result = {base, length, entry.offset, 1};
          break;
        }
      }
    }
    // A CPU write or GPU resolve during the snapshot/comparison discards it.
    if (before != p.Version(base, length)) {
      ++p.counters.invalidation_races;
      return {};
    }
    if (legacy) {
      if (p.legacy_mappings.size() >= kLegacyMaxMappings) {
        p.legacy_mappings.clear();
        ++p.counters.legacy_capacity_resets;
      }
      p.legacy_mappings[key] = {before, result};
    } else {
      if (!p.mappings->Store(lookup, {before, result})) ++p.counters.mapping_probe_refusals;
      p.counters.mapping_high_water = std::max<uint64_t>(p.counters.mapping_high_water,
                                                       p.mappings->size());
    }
    if (result.enabled) ++p.counters.hits;
    else ++p.counters.misses;
    return result;
  } catch (const std::bad_alloc&) {
    ++p.counters.misses;
    return {};
  }
}

D3D12_GPU_VIRTUAL_ADDRESS NativeAssetCache::gpu_address() const {
  return impl_->gpu && impl_->uploaded ? impl_->gpu->GetGPUVirtualAddress() : 0;
}
bool NativeAssetCache::initialized() const { return bool(impl_->gpu); }
const NativeAssetCache::Stats& NativeAssetCache::stats() const { return impl_->counters; }
void NativeAssetCache::Shutdown() {
  if (impl_->watch && impl_->shared) impl_->shared->UnregisterGlobalWatch(impl_->watch);
  impl_->watch = nullptr;
  impl_ = std::make_unique<Impl>();
}

}  // namespace nb::gpu
