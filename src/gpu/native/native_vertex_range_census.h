// Original nb diagnostics. Observe existing resident data; never request,
// invalidate, redirect or narrow a GPU upload. Empty output path disables this.
#pragma once

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <vector>

#include <rex/graphics/shared_memory.h>
#include "native_geometry_pass.h"

namespace nb::gpu {

inline uint32_t CensusSwap(uint32_t word, uint32_t endian) {
  const uint32_t stages = endian ^ (endian >> 1);
  if (stages & 1) word = ((word & 0x00FF00FFu) << 8) | ((word >> 8) & 0x00FF00FFu);
  if (stages & 2) word = (word >> 16) | (word << 16);
  return word;
}

// Pure arithmetic matching prelude.hlsl's VertexIndex, including unsigned
// offset wrap and the guest's 24-bit mask before the min/max clamp.
inline uint32_t CensusRemap(uint32_t raw, uint32_t offset, uint32_t lo, uint32_t hi) {
  return std::min(std::max((raw + offset) & 0xFFFFFFu, lo), hi);
}

class NativeVertexRangeCensus {
 public:
  ~NativeVertexRangeCensus() { if (file_) std::fclose(file_); }

  void Record(const std::string& path, uint64_t frame, uint64_t first, uint64_t last,
              uint32_t period, const std::string& shader,
              rex::graphics::SharedMemory& memory,
              const NativeGeometryPass::RootConstants& root,
              const NativeGeometryPass::DrawArgs& args) {
    if (path.empty() || frame < first || frame > last ||
        (period && (frame - first) % period)) return;
    if (!file_) {
      if (open_attempted_) return;
      open_attempted_ = true;
      std::error_code error;
      const std::filesystem::path output(path);
      if (output.has_parent_path()) std::filesystem::create_directories(output.parent_path(), error);
      if (error) return;
      file_ = std::fopen(path.c_str(), "wx");
      if (!file_) return;  // Never overwrite evidence from another run.
      std::setvbuf(file_, nullptr, _IOFBF, 1u << 20);
      std::fputs("frame,pair,stream,base,declared_bytes,stride_bytes,indexed,primitive_mode,host_vertices,index_bytes,index_status,index_min,index_max,clamp_min,clamp_max,index_offset\n", file_);
    }
    uint32_t low = UINT32_MAX, high = 0;
    const char* status = "ok";
    auto observe = [&](uint32_t raw) {
      const uint32_t index = CensusRemap(raw, root.index_offset, root.vertex_index_min, root.vertex_index_max);
      low = std::min(low, index);
      high = std::max(high, index);
    };
    const bool indexed = root.index_base_bytes != UINT32_MAX;
    if (!args.host_vertex_count) {
      status = "empty";
    } else if (indexed) {
      const uint32_t size = args.index_size_bytes;
      const uint32_t unit = root.index_format ? 4 : 2;
      const uint64_t end = uint64_t(root.index_base_bytes) + size;
      if (!size || size % unit || root.index_base_bytes % unit || size > (1u << 20) ||
          end > rex::graphics::SharedMemory::kBufferSize || root.index_endian > 3) {
        status = "unsupported_index_extent";
      } else {
        // Manual 16-bit loads swap a complete dword before extracting a half.
        // Include that padding, just as the shader does. The snapshot API
        // refuses invalid, unrequested or GPU-written pages under the watch lock.
        const uint32_t aligned_start = root.index_base_bytes & ~3u;
        const uint32_t snapshot_size = uint32_t((end + 3) & ~uint64_t(3)) - aligned_start;
        snapshot_.resize(snapshot_size);
        if (!memory.CopyCpuAuthoritativeRange(aligned_start, snapshot_size, snapshot_.data())) {
          status = "not_cpu_authoritative";
        } else {
          for (uint32_t i = 0; i < size; i += unit) {
            const uint32_t relative = root.index_base_bytes - aligned_start + i;
            uint32_t raw = 0;
            if (root.primitive_mode == 4) {
              std::memcpy(&raw, snapshot_.data() + relative, unit);
              raw = CensusSwap(raw, root.index_endian);
            } else {
              std::memcpy(&raw, snapshot_.data() + (relative & ~3u), 4);
              raw = CensusSwap(raw, root.index_endian);
              if (unit == 2) raw = (relative & 2) ? raw >> 16 : raw & 0xFFFFu;
            }
            observe(raw);
          }
        }
      }
    } else {
      uint32_t count = args.host_vertex_count;
      switch (root.primitive_mode) {
        case 0: break;
        case 1: count = (count / 6) * 4; break;
        case 2: count /= 6; break;
        case 3: count = count / 3 + 2; break;
        default: count = 0; status = "unsupported_primitive"; break;
      }
      if (count && count <= (1u << 20)) {
        // A sparse diagnostic, not a production per-draw index scan. Enumerate
        // to handle offset wrapping without assuming monotonic remapped indices.
        for (uint32_t i = 0; i < count; ++i) observe(i);
      } else if (count) {
        status = "oversized_nonindexed";
      } else if (status[0] == 'o') {
        status = "empty";
      }
    }
    if (low == UINT32_MAX) { low = 0; high = UINT32_MAX; }
    const uint32_t streams = std::min(args.vertex_stream_count, NativeGeometryPass::kMaxVertexStreams);
    for (uint32_t i = 0; i < streams; ++i) {
      if (!args.fetch_size_bytes[i]) continue;
      const uint32_t base = i == 0 ? root.fetch_base_bytes : i == 1 ? root.fetch1_base_bytes : args.extra_vertex_streams[i - 2][0];
      const uint32_t stride = (i == 0 ? root.fetch_stride_dwords : i == 1 ? root.fetch1_stride_dwords : args.extra_vertex_streams[i - 2][1]) * 4;
      std::fprintf(file_, "%llu,%s,%u,%u,%u,%u,%u,%u,%u,%u,%s,%u,%u,%u,%u,%u\n",
                   static_cast<unsigned long long>(frame), shader.c_str(), i, base, args.fetch_size_bytes[i],
                   stride, unsigned(indexed), root.primitive_mode, args.host_vertex_count, args.index_size_bytes,
                   status, low, high, root.vertex_index_min, root.vertex_index_max, root.index_offset);
    }
    if (last_frame_ != frame) { std::fflush(file_); last_frame_ = frame; }
  }

 private:
  FILE* file_ = nullptr;
  bool open_attempted_ = false;
  uint64_t last_frame_ = UINT64_MAX;
  std::vector<uint8_t> snapshot_;
};

}  // namespace nb::gpu
