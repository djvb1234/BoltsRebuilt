// Bounded native texture binding plans and exact-input sampler reuse.
// This policy owns candidate copies, never texture resources or live SRV indices.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace nb::gpu {

struct NativeTextureBindingCacheStats {
  uint64_t plan_hits = 0;
  uint64_t plan_misses = 0;
  uint64_t plan_bypasses = 0;
  uint64_t resets = 0;
  uint64_t sampler_hits = 0;
  uint64_t sampler_misses = 0;
  uint64_t sampler_resolutions = 0;
};

// TextureBinding and SamplerBinding are immutable SDK binding records after a
// successful translation. The owner must change lifetime_epoch before deleting
// any Shader, even if another Shader will later occupy the same address. The
// supplied live records must belong to that Shader and have finished translation.
template <typename TextureBinding, typename SamplerBinding>
class NativeTextureBindingCache {
 public:
  static constexpr size_t kEntries = 64;
  static constexpr size_t kFetchConstants = 32;
  static constexpr size_t kMaxTextures = 255;
  static constexpr size_t kMaxSamplers = 127;

  struct SamplerConsensus {
    uint32_t value = 0;
    bool have_sampler = false;
    bool conflict = false;
  };

 private:
  struct Range {
    uint16_t start = 0;
    uint16_t count = 0;
  };
  struct SamplerMemo {
    std::array<uint32_t, 6> fetch_words{};
    int32_t anisotropic_override = 0;
    SamplerConsensus consensus;
    bool valid = false;
  };

 public:
  struct Plan {
    std::span<const TextureBinding> Textures(uint32_t fetch_constant) const {
      if (fetch_constant >= kFetchConstants) return {};
      const auto& range = texture_ranges[fetch_constant];
      return {textures.data() + range.start, range.count};
    }
    std::span<const SamplerBinding> Samplers(uint32_t fetch_constant) const {
      if (fetch_constant >= kFetchConstants) return {};
      const auto& range = sampler_ranges[fetch_constant];
      return {samplers.data() + range.start, range.count};
    }

   private:
    friend class NativeTextureBindingCache;
    uintptr_t shader = 0;
    std::array<TextureBinding, kMaxTextures> textures{};
    std::array<SamplerBinding, kMaxSamplers> samplers{};
    std::array<Range, kFetchConstants> texture_ranges{};
    std::array<Range, kFetchConstants> sampler_ranges{};
    std::array<SamplerMemo, kFetchConstants> sampler_memos{};
  };

  const NativeTextureBindingCacheStats& stats() const { return stats_; }

  Plan* Prepare(uintptr_t shader, uint64_t lifetime_epoch,
                std::span<const TextureBinding> textures,
                std::span<const SamplerBinding> samplers) {
    if (!have_epoch_ || epoch_ != lifetime_epoch) {
      for (auto& entry : entries_) entry.shader = 0;
      epoch_ = lifetime_epoch;
      have_epoch_ = true;
      ++stats_.resets;
    }
    if (!shader || textures.size() > kMaxTextures || samplers.size() > kMaxSamplers) {
      ++stats_.plan_bypasses;
      return nullptr;
    }
    auto& entry = entries_[Slot(shader)];
    if (entry.shader == shader) {
      ++stats_.plan_hits;
      return &entry;
    }
    // Validate before replacing an existing plan. Unknown fetch indices retain
    // the caller's original binding iteration rather than indexing these arrays.
    for (const auto& binding : textures) {
      if (binding.fetch_constant >= kFetchConstants) {
        ++stats_.plan_bypasses;
        return nullptr;
      }
    }
    for (const auto& binding : samplers) {
      if (binding.fetch_constant >= kFetchConstants) {
        ++stats_.plan_bypasses;
        return nullptr;
      }
    }
    ++stats_.plan_misses;
    entry.shader = 0;
    Group(textures, entry.textures, entry.texture_ranges);
    Group(samplers, entry.samplers, entry.sampler_ranges);
    for (auto& memo : entry.sampler_memos) memo.valid = false;
    entry.shader = shader;
    return &entry;
  }

  // The immutable plan contains every instruction override, so the changing
  // part of a consensus key is exactly six fetch DWORDs and the global override.
  // resolve must call the SDK for this draw and return SamplerParameters::value.
  // Live fetch registers and configuration remain stable during this synchronous
  // call, as required by the original SDK sampler-binding loop as well.
  template <typename Resolve>
  SamplerConsensus ResolveSampler(Plan& plan, uint32_t fetch_constant,
                                  const std::array<uint32_t, 6>& fetch_words,
                                  int32_t anisotropic_override, Resolve&& resolve) {
    if (fetch_constant >= kFetchConstants) return {};
    auto& memo = plan.sampler_memos[fetch_constant];
    if (memo.valid && memo.fetch_words == fetch_words &&
        memo.anisotropic_override == anisotropic_override) {
      ++stats_.sampler_hits;
      return memo.consensus;
    }
    ++stats_.sampler_misses;
    SamplerConsensus consensus;
    for (const auto& binding : plan.Samplers(fetch_constant)) {
      const uint32_t value = resolve(binding);
      ++stats_.sampler_resolutions;
      if (consensus.have_sampler && value != consensus.value) {
        consensus.conflict = true;
        break;
      }
      consensus.value = value;
      consensus.have_sampler = true;
    }
    memo.fetch_words = fetch_words;
    memo.anisotropic_override = anisotropic_override;
    memo.consensus = consensus;
    memo.valid = true;
    return consensus;
  }

 private:
  static size_t Slot(uintptr_t shader) {
    uint64_t mixed = uint64_t(shader) >> 4;
    mixed ^= mixed >> 17;
    mixed *= UINT64_C(0x9E3779B185EBCA87);
    return size_t(mixed >> 32) & (kEntries - 1);
  }

  template <typename Binding, size_t Count>
  static void Group(std::span<const Binding> source, std::array<Binding, Count>& target,
                    std::array<Range, kFetchConstants>& ranges) {
    ranges.fill({});
    for (const auto& binding : source) ++ranges[binding.fetch_constant].count;
    uint16_t next = 0;
    for (auto& range : ranges) {
      range.start = next;
      next = uint16_t(next + range.count);
    }
    std::array<uint16_t, kFetchConstants> written{};
    for (const auto& binding : source) {
      const uint32_t fetch_constant = binding.fetch_constant;
      target[size_t(ranges[fetch_constant].start) + written[fetch_constant]++] = binding;
    }
  }

  std::array<Plan, kEntries> entries_{};
  uint64_t epoch_ = 0;
  bool have_epoch_ = false;
  NativeTextureBindingCacheStats stats_;
};

}  // namespace nb::gpu
