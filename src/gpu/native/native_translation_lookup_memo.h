// Original exact lookup memo for the translations owned by one live Shader.
// It caches object identity, never translation readiness or validity.
#pragma once

#include <cstdint>

namespace nb::gpu {

struct NativeTranslationLookupMemoStats {
  uint64_t hits = 0;
  uint64_t misses = 0;
  uint64_t publications = 0;
  uint64_t invalidations = 0;
};

template <typename Translation>
class NativeTranslationLookupMemo {
 public:
  NativeTranslationLookupMemo() = default;
  NativeTranslationLookupMemo(const NativeTranslationLookupMemo&) = delete;
  NativeTranslationLookupMemo& operator=(const NativeTranslationLookupMemo&) = delete;
  NativeTranslationLookupMemo(NativeTranslationLookupMemo&&) = delete;
  NativeTranslationLookupMemo& operator=(NativeTranslationLookupMemo&&) = delete;

  Translation* Find(uint64_t modification, NativeTranslationLookupMemoStats& stats) const {
    if (translation_ && modification_ == modification) {
      ++stats.hits;
      return translation_;
    }
    ++stats.misses;
    return nullptr;
  }
  void Publish(uint64_t modification, Translation* translation,
               NativeTranslationLookupMemoStats& stats) {
    if (!translation) return;
    modification_ = modification;
    translation_ = translation;
    ++stats.publications;
  }
  // Call before deleting the map entry, even while lookup optimization is off.
  bool Invalidate(uint64_t modification) {
    if (!translation_ || modification_ != modification) return false;
    Reset();
    return true;
  }
  void Reset() { translation_ = nullptr; }

 private:
  uint64_t modification_ = 0;
  Translation* translation_ = nullptr;
};

// Per-calling-thread totals: startup translation workers own separate totals.
// Reading this on the command thread describes only its own runtime lookups.
const NativeTranslationLookupMemoStats& GetNativeTranslationLookupMemoStats();

}  // namespace nb::gpu
