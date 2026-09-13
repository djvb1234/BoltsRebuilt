// Original portable metadata/extent policy for native guest float constants.
#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace nb::gpu {

// Missing cruns is the historical full-register contract. An explicitly empty
// cruns line is a different, validated promise from the generated shader body.
template <typename Run>
class NativeConstantLayoutMetadata {
 public:
  bool SetUpperBound(std::string_view text) {
    uint32_t value = 0;
    if (have_upper_bound_ || !Unsigned(text, value) || value > 256) return Fail();
    have_upper_bound_ = true;
    upper_bound = value;
    return true;
  }
  bool SetRuns(std::string_view text) {
    if (has_packed_layout) return Fail();
    std::vector<Run> parsed;
    uint32_t total = 0;
    while (!text.empty()) {
      const size_t comma = text.find(',');
      const auto item = text.substr(0, comma);
      const size_t dash = item.find('-');
      uint32_t first = 0, last = 0;
      if (dash == std::string_view::npos || !Unsigned(item.substr(0, dash), first) ||
          !Unsigned(item.substr(dash + 1), last) || first > last || last >= 256 ||
          last - first + 1 > 256 - total) return Fail();
      const uint32_t count = last - first + 1;
      parsed.push_back(Run{uint16_t(first), uint16_t(count)});
      total += count;
      if (comma == std::string_view::npos) break;
      text.remove_prefix(comma + 1);
      if (text.empty()) return Fail();
    }
    has_packed_layout = true;
    packed_registers = total;
    runs = std::move(parsed);
    return true;
  }
  bool valid() const noexcept {
    if (!valid_) return false;
    if (has_packed_layout && have_upper_bound_) {
      if (runs.empty()) return upper_bound == 0;
      // NB_NO_PACK deliberately emits0-255 even when no float operand was
      // collected (consts=0). Nonempty runs validate their own bounds/total.
    }
    return true;
  }

  uint32_t upper_bound = 256;
  bool has_packed_layout = false;
  uint32_t packed_registers = 0;
  std::vector<Run> runs;

 private:
  static bool Unsigned(std::string_view text, uint32_t& value) noexcept {
    if (text.empty()) return false;
    for (const char c : text) if (c < '0' || c > '9') return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
  }
  bool Fail() noexcept { valid_ = false; return false; }
  bool have_upper_bound_ = false;
  bool valid_ = true;
};

// Only a validated explicit-empty layout may reinterpret packed==0. The shader
// declaration may still contain its old unused array; this changes no reads.
constexpr uint32_t NativeConstantRegisters(uint32_t packed, bool explicit_empty = false) noexcept {
  return packed ? packed : explicit_empty ? 0u : 256u;
}

enum class NativeConstantBindingMatch { kIgnored, kExact, kAmbiguous };
constexpr NativeConstantBindingMatch NativeConstantHeaderBindingMatch(
    uint32_t space, uint32_t first, uint32_t count, uint32_t target) noexcept {
  if (space != 0) return NativeConstantBindingMatch::kIgnored;
  // Unbounded/unknown CBV arrays cannot prove one named buffer's extent.
  if (count == 0) return NativeConstantBindingMatch::kAmbiguous;
  if (first > target || target - first >= count) return NativeConstantBindingMatch::kIgnored;
  return count == 1 ? NativeConstantBindingMatch::kExact : NativeConstantBindingMatch::kAmbiguous;
}
constexpr bool NativeConstantVariableFitsHeader(bool used, uint32_t start, uint32_t size,
                                                uint32_t header_bytes) noexcept {
  return !used || (size != 0 && start <= header_bytes && size <= header_bytes - start);
}

}  // namespace nb::gpu
