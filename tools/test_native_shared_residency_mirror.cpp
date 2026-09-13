// Original standalone controls for nb's conservative atomic residency mirror.
#include "../src/gpu/native/native_shared_residency_mirror.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using Mirror = nb::gpu::NativeSharedResidencyMirror;
using Range = Mirror::Range;
using Scope = nb::gpu::NativeSharedResidencyRequestScope;
uint64_t checks = 0;
void Check(bool value) {
  ++checks;
  if (!value) { std::cerr << "FAIL check " << checks << '\n'; std::exit(1); }
}
bool Has(const Mirror& mirror, uint32_t start, uint32_t bytes) {
  const Range range{start, bytes};
  return mirror.Contains({&range, 1});
}

void LiteralBoundsAndMasks() {
  auto m = std::make_unique<Mirror>();
  Check(!Has(*m, 0, 1));
  Check(m->PromoteValidWord(0, UINT64_MAX, uint64_t(1) << 63) == 1);
  Check(Has(*m, 63 * 4096, 4096));
  Check(!Has(*m, 62 * 4096, 1));
  Check(!Has(*m, 63 * 4096 + 4095, 2));
  Check(m->PromoteValidWord(1, 1, 3) == 1);
  Check(Has(*m, 63 * 4096 + 4095, 2));
  Check(!Has(*m, 65 * 4096, 1));
  Check(m->PromoteValidWord(1, 2, 1) == 0);  // live/request intersection
  const std::array<Range, 2> disjoint{{{63 * 4096, 1}, {64 * 4096 + 4000, 96}}};
  Check(m->Contains(disjoint));
  m->ClearPages(64, 64);
  Check(!m->Contains(disjoint));
  Check(Has(*m, 63 * 4096, 4096));
  m->ClearPages(9, 8);
  Check(Has(*m, 63 * 4096, 4096));
  Check(!Has(*m, 0, 0));
  Check(!Has(*m, UINT32_MAX, 1));
  Check(!Has(*m, Mirror::kBufferBytes, 1));
  Check(!Has(*m, Mirror::kBufferBytes - 1, 2));
  Check(!Has(*m, 0, UINT32_MAX));
  Check(m->PromoteValidWord(Mirror::kWordCount, UINT64_MAX, UINT64_MAX) == 0);
  for (uint32_t word = 0; word < Mirror::kWordCount; ++word) {
    m->PromoteValidWord(word, UINT64_MAX, UINT64_MAX);
  }
  Check(Has(*m, 0, Mirror::kBufferBytes));
  m->ClearPages(Mirror::kPageCount - 1, UINT32_MAX);
  Check(!Has(*m, Mirror::kBufferBytes - 1, 1));
  Check(Has(*m, Mirror::kBufferBytes - 4097, 1));
  m->Reset();
  Check(!Has(*m, 63 * 4096, 1));
  m->Disable();
  Check(!m->enabled());
  m->Reset();
  Check(m->PromoteValidWord(0, 1, 1) == 0);
  Check(!Has(*m, 0, 1));
}

// Independent flat-page authority model. It does not use the helper's masks to
// decide what bytes a CPU or GPU consumer must see. The mirror is just an
// optional accelerator; every reported hit is checked against this model.
struct Model {
  static constexpr uint32_t kPages = 256;
  Mirror mirror;
  std::array<bool, kPages> allocated{}, valid{}, gpu_authority{};
  std::array<uint32_t, kPages> cpu{}, gpu{};
  bool enabled = true;
  uint32_t depth = 0;
  uint64_t copies = 0;
  uint64_t hits = 0;

  void CpuWrite(uint32_t page, uint32_t value) {
    mirror.ClearPages(page, page);  // prior to validity mutation and CPU write
    valid[page] = false;
    gpu_authority[page] = false;
    cpu[page] = value;
  }
  void GpuWrite(uint32_t page, uint32_t value) {
    Check(allocated[page]);
    mirror.ClearPages(page, page);
    gpu[page] = value;
    valid[page] = true;
    gpu_authority[page] = true;
  }
  void Frame() {
    mirror.Reset();
    valid = gpu_authority;
  }
  void Toggle(bool on) {
    mirror.Reset();
    enabled = on;
  }
  bool Request(std::span<const Range> ranges, int fail_page = -1,
               int invalidate_after_copy = -1, bool nested_probe = false) {
    Scope scope(&depth);
    std::array<bool, kPages> wanted{};
    for (const auto& range : ranges) {
      Check(range.second != 0 && uint64_t(range.first) + range.second <= kPages * 4096);
      // Enumerate intersecting byte intervals, independent of packed words.
      for (uint32_t page = 0; page < kPages; ++page) {
        if (uint64_t(range.first) < uint64_t(page + 1) * 4096 &&
            uint64_t(range.first) + range.second > uint64_t(page) * 4096) wanted[page] = true;
      }
    }
    const bool eligible = enabled && scope.outer();
    if (eligible && mirror.Contains(ranges)) {
      ++hits;
      for (uint32_t page = 0; page < kPages; ++page) if (wanted[page]) {
        Check(allocated[page] && valid[page]);
        if (!gpu_authority[page]) Check(gpu[page] == cpu[page]);
      }
      return true;
    }
    // No publication at all until every requested backing allocation succeeds.
    for (uint32_t page = 0; page < kPages; ++page) if (wanted[page]) {
      if (int(page) == fail_page) return false;
      allocated[page] = true;
    }
    for (uint32_t page = 0; page < kPages; ++page) {
      if (wanted[page] && valid[page] && eligible) {
        mirror.PromoteValidWord(page / 64, uint64_t(1) << (page % 64),
                                uint64_t(1) << (page % 64));
      }
    }
    for (uint32_t page = 0; page < kPages; ++page) if (wanted[page] && !valid[page]) {
      mirror.ClearPages(page, page);
      valid[page] = true;  // SDK marks valid before protection/copy
      gpu_authority[page] = false;
      if (nested_probe) {
        const Range one{page * 4096, 4096};
        const uint64_t old_hits = hits;
        Check(Request({&one, 1}));
        Check(hits == old_hits);  // nested scan must not become a fast hit
        Check(!Has(mirror, page * 4096, 4096));  // no premature promotion
      }
      gpu[page] = cpu[page];
      ++copies;
      if (int(page) == invalidate_after_copy) CpuWrite(page, cpu[page] + 1);
      // Deliberately no publication after copy success.
    }
    return true;
  }
};

void PublicationAndAuthority() {
  auto p = std::make_unique<Model>();
  const Range one{3 * 4096 + 7, 9};
  p->cpu[3] = 0x1234;
  Check(p->Request({&one, 1}));
  Check(p->copies == 1 && p->hits == 0 && !Has(p->mirror, one.first, one.second));
  Check(p->Request({&one, 1}));
  Check(p->copies == 1 && p->hits == 0 && Has(p->mirror, one.first, one.second));
  Check(p->Request({&one, 1}));
  Check(p->hits == 1 && p->gpu[3] == 0x1234);
  p->CpuWrite(3, 0x4567);
  Check(!Has(p->mirror, one.first, one.second));
  Check(p->Request({&one, 1}, -1, -1, true));
  Check(p->gpu[3] == 0x4567 && !Has(p->mirror, one.first, one.second));
  Check(p->depth == 0);
  Check(p->Request({&one, 1}));
  p->GpuWrite(3, 0xABCDEF);
  Check(!Has(p->mirror, one.first, one.second));
  const auto copies = p->copies;
  Check(p->Request({&one, 1}));
  Check(p->gpu[3] == 0xABCDEF && p->copies == copies);
  p->Frame();
  Check(!Has(p->mirror, one.first, one.second));
  Check(p->Request({&one, 1}));
  Check(p->gpu[3] == 0xABCDEF && p->copies == copies);
  p->CpuWrite(3, 77);
  Check(p->Request({&one, 1}, -1, 3));
  Check(!p->valid[3] && !Has(p->mirror, one.first, one.second));
  Check(p->Request({&one, 1}));
  Check(p->gpu[3] == 78);
  p->mirror.Reset();
  const std::array<Range, 2> two{{one, {4 * 4096, 1}}};
  Check(!p->Request(two, 4));
  Check(!Has(p->mirror, one.first, one.second));
  p->Toggle(false);
  Check(p->Request({&one, 1}));
  Check(!Has(p->mirror, one.first, one.second));
  p->Toggle(true);
  Check(p->Request({&one, 1}));
  Check(Has(p->mirror, one.first, one.second));
  p->mirror.Disable();
  p->Toggle(false);
  p->Toggle(true);
  Check(p->Request({&one, 1}));
  Check(!Has(p->mirror, one.first, one.second));
}

void ScopeAndFlatOracle() {
  uint32_t depth = 0;
  try {
    Scope outer(&depth);
    Check(outer.outer());
    { Scope nested(&depth); Check(!nested.outer() && !outer.outer()); }
    Check(outer.outer());
    throw 7;
  } catch (int) {}
  Check(depth == 0);
  { Scope foreign(nullptr); Check(!foreign.outer()); }
  auto m = std::make_unique<Model>();
  uint32_t rng = 0xA34C517D;
  for (uint32_t event = 0; event < 12000; ++event) {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    const uint32_t page = rng % Model::kPages;
    switch ((rng >> 16) % 9) {
      case 0: m->CpuWrite(page, rng); break;
      case 1: if (m->allocated[page]) m->GpuWrite(page, rng); break;
      case 2: m->Frame(); break;
      case 3: m->Toggle(!m->enabled); break;
      default: {
        const uint32_t bytes = std::min(uint32_t(8193), Model::kPages * 4096 - page * 4096);
        const Range range{page * 4096, bytes};
        Check(m->Request({&range, 1}));
        Check(m->Request({&range, 1}));
        Check(m->Request({&range, 1}));
        for (uint32_t j = page; j <= (range.first + bytes - 1) / 4096; ++j) {
          Check(m->valid[j] && m->allocated[j]);
          if (!m->gpu_authority[j]) Check(m->gpu[j] == m->cpu[j]);
        }
        break;
      }
    }
  }
}

void ConcurrentMonotonicClears() {
  auto m = std::make_unique<Mirror>();
  for (uint32_t word = 0; word < 65; ++word) m->PromoteValidWord(word, UINT64_MAX, UINT64_MAX);
  std::atomic<uint32_t> cleared{0};
  std::atomic<bool> start{false};
  std::thread invalidator([&] {
    while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
    for (uint32_t page = 1; page <= 4096; ++page) {
      m->ClearPages(page, page);
      cleared.store(page, std::memory_order_release);
    }
  });
  start.store(true, std::memory_order_release);
  uint32_t last = 0;
  while (last < 4096) {
    last = cleared.load(std::memory_order_acquire);
    if (last) {
      Check(!Has(*m, last * 4096, 1));
      Check(!Has(*m, 0, 4097 * 4096));
    }
    Check(Has(*m, 0, 4096));
    Check(Has(*m, 4097 * 4096, 4096));
  }
  invalidator.join();
  for (uint32_t page = 1; page <= 4096; ++page) Check(!Has(*m, page * 4096, 4096));
  m->Reset();
  Check(!Has(*m, 0, 1));
}

int main() {
  LiteralBoundsAndMasks();
  PublicationAndAuthority();
  ScopeAndFlatOracle();
  ConcurrentMonotonicClears();
  std::cout << "PASS " << checks << " checks in 4 groups (concurrent count varies)\n";
}
