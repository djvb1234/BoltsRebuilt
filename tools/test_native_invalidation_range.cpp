// Original portable controls for the production excess-invalidation cap.
// Models writable physical pages, their protection, GPU authority and textures.
// No SDK linkage, game data, real protection changes or GPU execution.
#include "../src/gpu/native/native_invalidation_range.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {
using Range = nb::gpu::NativeInvalidationRange;
constexpr uint32_t kPages = 256;
uint64_t checks = 0;
void Check(bool ok, const char* why) {
  ++checks;
  if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
bool Contains(Range range, uint32_t page) { return page >= range.first && page <= range.last; }

// Independent page enumeration: select members of the original range sharing
// a 64KiB block with any requested page, always including the actual write.
Range EnumeratedCap(Range requested, Range original, uint32_t page_log2) {
  const uint64_t page_bytes = uint64_t(1) << page_log2;
  const uint64_t first_block = uint64_t(requested.first) * page_bytes / 65536;
  const uint64_t last_block = uint64_t(requested.last) * page_bytes / 65536;
  Range result{UINT32_MAX, 0};
  for (uint32_t p = original.first; p <= original.last; ++p) {
    const uint64_t block = uint64_t(p) * page_bytes / 65536;
    if (Contains(requested, p) || (block >= first_block && block <= last_block)) {
      if (result.first == UINT32_MAX) result.first = p;
      result.last = p;
    }
  }
  return result;
}

void LiteralBounds() {
  struct Case { Range request, original, expected; uint32_t page_log2; };
  const Case cases[]{
      {{20, 20}, {0, 63}, {16, 31}, 12},
      {{15, 16}, {0, 63}, {0, 31}, 12},
      {{63, 64}, {0, 127}, {48, 79}, 12},
      {{20, 20}, {19, 24}, {19, 24}, 12},  // GPU neighbors are tighter.
      {{20, 20}, {18, 63}, {18, 31}, 12},
      {{20, 20}, {0, 22}, {16, 22}, 12},
      {{31, 80}, {0, 127}, {16, 95}, 12},
      {{0, 255}, {0, 255}, {0, 255}, 12},
      {{255, 255}, {192, 255}, {240, 255}, 12},
      {{20, 20}, {0, 63}, {16, 23}, 13},
      {{20, 20}, {0, 63}, {20, 23}, 14},
      {{20, 20}, {0, 63}, {20, 21}, 15},
      {{20, 20}, {0, 63}, {20, 20}, 16},
      {{20, 20}, {0, 63}, {20, 20}, 17},
      {{20, 20}, {0, 63}, {0, 63}, 10},
      {{UINT32_MAX, UINT32_MAX}, {UINT32_MAX - 63, UINT32_MAX},
       {UINT32_MAX - 15, UINT32_MAX}, 12},
  };
  for (const auto& test : cases) {
    const auto actual = nb::gpu::CapNativeInvalidationRange(
        test.request, test.original, test.page_log2, false, true);
    Check(actual == test.expected, "literal64KiB cap differs");
    Check(nb::gpu::CapNativeInvalidationRange(test.request, test.original, test.page_log2,
                                             false, false) == test.original,
          "disabled path changes the original range");
    Check(nb::gpu::CapNativeInvalidationRange(test.request, test.original, test.page_log2,
                                             true, true) == test.original,
          "exact callback changes its original range");
    Check(actual.first <= test.request.first && actual.last >= test.request.last &&
              actual.first >= test.original.first && actual.last <= test.original.last,
          "cap omitted a requested page or added an excluded neighbor");
  }
  Check(nb::gpu::CapNativeInvalidationRange({9, 8}, {0, 63}, 12, false, true) == Range{0, 63},
        "malformed request must preserve original behavior");
  Check(nb::gpu::CapNativeInvalidationRange({20, 30}, {21, 63}, 12, false, true) == Range{21, 63},
        "noncontaining original range must preserve original behavior");
  Check(nb::gpu::CapNativeInvalidationRange({20, 30}, {0, 63}, 32, false, true) == Range{0, 63},
        "invalid host page shift must preserve original behavior");
  Check(nb::gpu::NativeInvalidationPageCount({0, UINT32_MAX}) == (uint64_t(1) << 32),
        "inclusive page count must not wrap");
  Check(nb::gpu::NativeInvalidationPageMask({UINT32_MAX - 15, UINT32_MAX}, UINT32_MAX >> 6) ==
            UINT64_C(0xFFFF000000000000), "last page block mask must not overflow");
}

// Independent form of the existing SDK GPU-neighbor limit: expand one page at
// a time only within the first/last64-page blocks and stop before GPU data.
Range GpuGuard(Range requested, const std::array<bool, kPages>& gpu) {
  Range result = requested;
  while (result.first % 64 != 0 && !gpu[result.first - 1]) --result.first;
  while (result.last % 64 != 63 && !gpu[result.last + 1]) ++result.last;
  return result;
}

void MasksAndGpuNeighbors() {
  uint32_t random = 0x8142A5D1;
  auto next = [&]() { random ^= random << 13; random ^= random >> 17; return random ^= random << 5; };
  for (unsigned trial = 0; trial < 20000; ++trial) {
    const uint32_t first = next() % kPages;
    const Range requested{first, first + next() % (kPages - first)};
    std::array<bool, kPages> gpu{}, valid{};
    for (uint32_t p = 0; p < kPages; ++p) {
      valid[p] = (next() & 1) != 0;
      gpu[p] = valid[p] && next() % 11 == 0;
    }
    const Range original = GpuGuard(requested, gpu);
    const uint32_t log2 = 10 + next() % 9;
    const Range selected = nb::gpu::CapNativeInvalidationRange(requested, original, log2, false, true);
    Check(selected == EnumeratedCap(requested, original, log2), "cap differs from independent byte-block oracle");
    uint32_t expected_retained = 0, actual_retained = 0;
    for (uint32_t block = 0; block < kPages / 64; ++block) {
      uint64_t expected_mask = 0, valid_word = 0, gpu_word = 0;
      for (uint32_t bit = 0; bit < 64; ++bit) {
        const uint32_t p = block * 64 + bit;
        if (Contains(selected, p)) expected_mask |= uint64_t(1) << bit;
        if (valid[p]) valid_word |= uint64_t(1) << bit;
        if (gpu[p]) gpu_word |= uint64_t(1) << bit;
        if (Contains(original, p) && !Contains(selected, p) && valid[p] && !gpu[p]) ++expected_retained;
        if (Contains(selected, p) && !Contains(requested, p))
          Check(!gpu[p], "cap crossed the original GPU-authority exclusion");
      }
      Check(nb::gpu::NativeInvalidationPageMask(selected, block) == expected_mask,
            "page mask differs from per-page enumeration");
      actual_retained += nb::gpu::NativeRetainedCpuPages(original, selected, block, valid_word, gpu_word);
    }
    Check(actual_retained == expected_retained, "retained CPU-valid page counter differs from oracle");
  }
}

struct Oracle {
  std::array<uint32_t, kPages> cpu{}, gpu{};
  std::array<bool, kPages> gpu_authority{};
  uint32_t Expected(uint32_t page) const { return gpu_authority[page] ? gpu[page] : cpu[page]; }
};
struct Machine {
  std::array<uint32_t, kPages> shared{};
  std::array<bool, kPages> valid{}, gpu{}, protected_page{};
  std::array<bool, 16> texture_dirty{};
  std::array<uint32_t, kPages> texture_data{};
  uint64_t faults = 0, uploads = 0, texture_loads = 0;
  Machine() { texture_dirty.fill(true); }
  void Notify(Range pages) {
    for (uint32_t texture = 0; texture < 16; ++texture) {
      if (pages.first <= texture * 16 + 15 && pages.last >= texture * 16) texture_dirty[texture] = true;
    }
  }
  void Callback(Range requested, bool exact, bool narrow, bool intersect_other = false) {
    ++faults;
    const Range original = exact ? requested : GpuGuard(requested, gpu);
    const Range selected = nb::gpu::CapNativeInvalidationRange(requested, original, 12, exact, narrow);
    const auto prior_protection = protected_page;
    // The SDK runtime intersects callback return extents. An independent,
    // tighter callback may reduce unprotection further, never below requested.
    const Range physical_unwatch = intersect_other
        ? Range{std::max(selected.first, requested.first), std::min(selected.last, requested.last + 3)}
        : selected;
    Check(physical_unwatch.first <= requested.first && physical_unwatch.last >= requested.last,
          "runtime intersection must still cover the actual write");
    for (uint32_t p = selected.first; p <= selected.last; ++p) { valid[p] = false; gpu[p] = false; }
    Notify(selected);
    for (uint32_t p = physical_unwatch.first; p <= physical_unwatch.last; ++p) protected_page[p] = false;
    for (uint32_t p = 0; p < kPages; ++p) {
      if (prior_protection[p] && !protected_page[p])
        Check(!valid[p] && !gpu[p], "runtime unprotected a page still claimed valid or GPU-authoritative");
      if (!Contains(selected, p))
        Check(protected_page[p] == prior_protection[p], "omitted neighbor lost physical write protection");
    }
  }
  void CpuWrite(uint32_t page, bool narrow) {
    if (protected_page[page]) Callback({page, page}, false, narrow, (page & 1) != 0);
    Check(!valid[page] && !gpu[page], "a real CPU write escaped authority invalidation");
  }
  void GpuWrite(uint32_t page, uint32_t value) {
    shared[page] = value; valid[page] = gpu[page] = protected_page[page] = true;
    Notify({page, page});
  }
  void Draw(Range requested, const Oracle& oracle) {
    for (uint32_t p = requested.first; p <= requested.last; ++p) {
      if (!valid[p]) {
        Check(!gpu[p], "GPU-authoritative data cannot become an upload candidate");
        shared[p] = oracle.cpu[p]; valid[p] = protected_page[p] = true;
        ++uploads;
      }
      Check(shared[p] == oracle.Expected(p), "draw changed literal CPU/GPU-authoritative bytes");
    }
  }
  void Texture(uint32_t texture, const Oracle& oracle) {
    const Range pages{texture * 16, texture * 16 + 15};
    if (texture_dirty[texture]) {
      Draw(pages, oracle);
      for (uint32_t p = pages.first; p <= pages.last; ++p) texture_data[p] = shared[p];
      texture_dirty[texture] = false; ++texture_loads;
    }
    for (uint32_t p = pages.first; p <= pages.last; ++p)
      Check(texture_data[p] == oracle.Expected(p), "texture watch missed a real changed byte");
  }
};

void ProtectionAndAuthoritySequences() {
  Oracle oracle;
  Machine legacy, candidate;
  for (uint32_t p = 0; p < kPages; ++p) oracle.cpu[p] = 0xAA000000u + p;
  legacy.Draw({0, kPages - 1}, oracle); candidate.Draw({0, kPages - 1}, oracle);
  for (uint32_t t = 0; t < 16; ++t) { legacy.Texture(t, oracle); candidate.Texture(t, oracle); }
  // A real write beside an unrelated texture: both remain byte-correct, while
  // the narrower notification avoids invalidating textures on other64KiB blocks.
  oracle.cpu[20] ^= 0xFFFF;
  legacy.CpuWrite(20, false); candidate.CpuWrite(20, true);
  Check(legacy.texture_dirty[0] && !candidate.texture_dirty[0], "literal neighboring texture opportunity not exercised");
  Check(candidate.protected_page[5], "retained neighboring page must remain physically protected");
  const uint64_t candidate_faults = candidate.faults;
  legacy.CpuWrite(5, false); candidate.CpuWrite(5, true);
  oracle.cpu[5] ^= 0x11223344;
  Check(candidate.faults == candidate_faults + 1 && candidate.texture_dirty[0],
        "later write to a retained neighbor must fault and notify its texture");
  for (uint32_t t = 0; t < 16; ++t) { legacy.Texture(t, oracle); candidate.Texture(t, oracle); }

  oracle.gpu[31] = 0xDEADBEEF; oracle.gpu_authority[31] = true;
  legacy.GpuWrite(31, oracle.gpu[31]); candidate.GpuWrite(31, oracle.gpu[31]);
  legacy.CpuWrite(32, false); candidate.CpuWrite(32, true);
  oracle.cpu[32] ^= 0x87654321;
  Check(legacy.gpu[31] && candidate.gpu[31] && candidate.protected_page[31],
        "neighboring GPU data remains authoritative and protected");
  legacy.Draw({31, 32}, oracle); candidate.Draw({31, 32}, oracle);
  const Range crossing{63, 65};  // Crosses both64KiB and256KiB blocks.
  legacy.Callback(crossing, false, false, true); candidate.Callback(crossing, false, true, true);
  for (uint32_t p = crossing.first; p <= crossing.last; ++p) {
    oracle.cpu[p] ^= 0xFEDCBA98; oracle.gpu_authority[p] = false;
  }
  legacy.Draw(crossing, oracle); candidate.Draw(crossing, oracle);

  uint32_t random = 0x7942AD51;
  auto next = [&]() { random ^= random << 13; random ^= random >> 17; return random ^= random << 5; };
  for (uint32_t event = 0; event < 20000; ++event) {
    const uint32_t page = next() % kPages;
    const bool narrow = (event / 137) % 2 != 0;  // Live diagnostic toggles.
    switch (next() % 8) {
      case 0:
      case 1:
        legacy.CpuWrite(page, false); candidate.CpuWrite(page, narrow);
        oracle.cpu[page] = next(); oracle.gpu_authority[page] = false;
        break;
      case 2:
        oracle.gpu[page] = next(); oracle.gpu_authority[page] = true;
        legacy.GpuWrite(page, oracle.gpu[page]); candidate.GpuWrite(page, oracle.gpu[page]);
        break;
      case 3:  // clear_memory_page_state=true; physical protection stays intact.
        legacy.valid = legacy.gpu; candidate.valid = candidate.gpu;
        break;
      case 4:  // Exact CPU invalidation, including spans across64KiB boundaries.
        {
          const Range requested{page, std::min(kPages - 1, page + 17)};
          legacy.Callback(requested, true, false); candidate.Callback(requested, true, narrow);
          for (uint32_t p = requested.first; p <= requested.last; ++p) {
            oracle.cpu[p] = next(); oracle.gpu_authority[p] = false;
          }
        }
        break;
      case 5:
        legacy.valid.fill(false); legacy.gpu.fill(false); legacy.Notify({0, kPages - 1});
        candidate.valid.fill(false); candidate.gpu.fill(false); candidate.Notify({0, kPages - 1});
        oracle.gpu_authority.fill(false);
        break;
      case 6:
        legacy.Texture(page / 16, oracle); candidate.Texture(page / 16, oracle);
        break;
      case 7:
        {
          const Range requested{page, std::min(kPages - 1, page + 31)};
          legacy.Draw(requested, oracle); candidate.Draw(requested, oracle);
        }
        break;
    }
  }
  legacy.Draw({0, kPages - 1}, oracle); candidate.Draw({0, kPages - 1}, oracle);
  for (uint32_t t = 0; t < 16; ++t) { legacy.Texture(t, oracle); candidate.Texture(t, oracle); }
  std::printf("model legacy/candidate faults=%llu/%llu page_uploads=%llu/%llu texture_loads=%llu/%llu\n",
      static_cast<unsigned long long>(legacy.faults), static_cast<unsigned long long>(candidate.faults),
      static_cast<unsigned long long>(legacy.uploads), static_cast<unsigned long long>(candidate.uploads),
      static_cast<unsigned long long>(legacy.texture_loads), static_cast<unsigned long long>(candidate.texture_loads));
}
}  // namespace

int main() {
  LiteralBounds();
  MasksAndGpuNeighbors();
  ProtectionAndAuthoritySequences();
  std::printf("PASS %llu checks: literal cap bounds, 20000 GPU/mask cases, 20000 protection/authority events\n",
              static_cast<unsigned long long>(checks));
}
