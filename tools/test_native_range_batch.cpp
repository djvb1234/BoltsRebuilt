// Original portable CPU controls for bounded shared-memory range preparation.
#include "../src/gpu/native/native_range_batch.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace {
using Range = nb::gpu::NativeRangeBatch::Range;
using Batch = nb::gpu::NativeRangeBatch;
void Check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
std::vector<Range> Copy(const Batch& batch) {
  return {batch.ranges().begin(), batch.ranges().end()};
}
uint64_t CountPages(const std::vector<Range>& ranges) {
  uint64_t pages = 0;
  for (auto [start, count] : ranges) pages += count;
  return pages;
}
// Deliberately simple per-page scanner independent of the SDK bit-scan loop.
std::vector<Range> QueuePages(const Batch& batch, const std::vector<bool>& valid,
                              bool optimize, uint64_t& duplicates_saved) {
  std::vector<Range> queued;
  for (auto [start, length] : batch.ranges()) {
    const uint32_t first = start >> 12;
    const uint32_t last = (start + length - 1) >> 12;
    uint32_t at = first;
    while (at <= last) {
      if (valid.at(at)) { ++at; continue; }
      const uint32_t run = at++;
      while (at <= last && !valid.at(at)) ++at;
      duplicates_saved += nb::gpu::AppendUploadPageRange(queued, run, at - run, optimize);
    }
  }
  return queued;
}
std::vector<bool> Pages(const std::vector<Range>& ranges, size_t count) {
  std::vector<bool> result(count);
  for (auto [start, length] : ranges) {
    Check(start <= count && length <= count - start, "queued page extent exceeds fixture");
    for (uint32_t i = 0; i < length; ++i) result[start + i] = true;
  }
  return result;
}

void ByteMergingPreservesGapsAndInput() {
  const std::vector<Range> input{{400, 20}, {140, 30}, {100, 50}, {420, 20},
                                {400, 0}, {120, 5}, {170, 5}};
  const auto original = input;
  const std::vector<Range> expected{{100, 75}, {400, 40}};
  for (bool optimize : {false, true}) {
    Batch batch;
    Check(batch.Build(input.data(), input.size(), 1024, optimize), "valid byte batch refused");
    Check(Copy(batch) == expected, "byte overlap/adjacency/containment merge differs");
    Check(input == original, "input ranges mutated");
  }
}

void BoundsAndEmptyRequests() {
  constexpr uint32_t limit = 1u << 29;
  Batch batch;
  Check(batch.Build(nullptr, 99, limit, true) && batch.ranges().empty(), "null request differs");
  const Range zero{UINT32_MAX, 0};
  Check(batch.Build(&zero, 1, limit, true) && batch.ranges().empty(), "zero length must remain harmless");
  const Range end{limit - 1, 1};
  Check(batch.Build(&end, 1, limit, true) && Copy(batch) == std::vector<Range>{end},
        "last physical byte refused");
  for (const Range invalid : {Range{limit, 1}, Range{limit - 1, 2}, Range{UINT32_MAX, 2}, Range{1, UINT32_MAX}}) {
    for (bool optimize : {false, true})
      Check(!batch.Build(&invalid, 1, limit, optimize), "out-of-bounds or wrapped input accepted");
  }
}

void StackThresholdAndLegacyStorage() {
  std::vector<Range> input;
  for (uint32_t i = 0; i < 33; ++i) input.emplace_back(i * 64, 16);
  Batch batch;
  Check(batch.Build(input.data(), 32, 4096, true) && batch.on_stack() && batch.ranges().size() == 32,
        "32 ranges did not use bounded stack storage");
  Check(batch.Build(input.data(), 33, 4096, true) && !batch.on_stack() && batch.ranges().size() == 33,
        "large range count did not retain heap fallback");
  Check(batch.Build(input.data(), 2, 4096, false) && !batch.on_stack(), "legacy mode did not retain heap storage");
  Check(batch.Build(input.data(), 1, 4096, true) && batch.on_stack() && batch.ranges().size() == 1,
        "reuse after heap fallback retained stale entries");
}

void DisjointBytesSamePageOnlyUploadOnce() {
  const Range input[]{{0x1000, 0x200}, {0x1300, 0x80}};
  Batch batch;
  Check(batch.Build(input, 2, 0x4000, true) && batch.ranges().size() == 2,
        "fixture must remain two disjoint byte ranges");
  std::vector<bool> valid(4);
  uint64_t old_saved = 0, new_saved = 0;
  auto old_uploads = QueuePages(batch, valid, false, old_saved);
  auto new_uploads = QueuePages(batch, valid, true, new_saved);
  Check(CountPages(old_uploads) == 2 && CountPages(new_uploads) == 1 && new_saved == 1 && !old_saved,
        "same-page duplicate copy was not eliminated exactly once");
  Check(Pages(old_uploads, 4) == Pages(new_uploads, 4), "optimization changed required page set");
}

void OverlapContainmentAdjacencyAndGaps() {
  std::vector<Range> output;
  uint64_t saved = 0;
  for (Range next : {Range{1, 4}, Range{2, 2}, Range{4, 3}, Range{7, 2}, Range{10, 2}})
    saved += nb::gpu::AppendUploadPageRange(output, next.first, next.second, true);
  Check(output == std::vector<Range>{{1, 8}, {10, 2}} && saved == 3,
        "page union lost a gap or miscounted duplicate pages");
  Check(nb::gpu::AppendUploadPageRange(output, 99, 0, true) == 0 && output.size() == 2,
        "empty page range changed output");
}

void CurrentValidityAndSubsequentInvalidation() {
  const Range input[]{{0x1000, 0x2800}, {0x3900, 0x100}};
  Batch batch;
  Check(batch.Build(input, 2, 0x6000, true), "validity fixture refused");
  std::vector<bool> valid(6, true);
  valid[1] = false;
  valid[3] = false;
  uint64_t saved = 0;
  auto first = QueuePages(batch, valid, true, saved);
  Check(first == std::vector<Range>{{1, 1}, {3, 1}}, "valid pages were copied or invalid pages skipped");
  valid[1] = valid[3] = true;
  saved = 0;
  Check(QueuePages(batch, valid, true, saved).empty(), "already valid pages copied again");
  valid[2] = false;  // A same-frame write must be considered on the next request.
  Check(QueuePages(batch, valid, true, saved) == std::vector<Range>{{2, 1}},
        "new invalidation was masked by prior range preparation");
  std::fill(valid.begin(), valid.end(), false);  // Frame-close reset remains authoritative.
  Check(QueuePages(batch, valid, true, saved) == std::vector<Range>{{1, 3}},
        "frame reset did not restore all requested upload pages");
}

void LastPageAndBoundaryGap() {
  constexpr uint32_t limit = 1u << 29;
  const Range input[]{{limit - 4096, 10}, {limit - 100, 100}, {limit - 8192, 1}};
  Batch batch;
  Check(batch.Build(input, 3, limit, true), "end-of-memory fixture refused");
  std::vector<bool> valid(limit >> 12);
  uint64_t saved = 0;
  auto uploads = QueuePages(batch, valid, true, saved);
  Check(uploads == std::vector<Range>{{(limit >> 12) - 2, 2}} && saved == 1,
        "last-page union overflowed or omitted requested data");
}

void RandomizedPageSetEquivalence() {
  uint32_t random = 0xBADC0DE;
  auto next = [&]() { random = random * 1664525u + 1013904223u; return random; };
  constexpr uint32_t limit = 64 * 4096;
  for (uint32_t trial = 0; trial < 2000; ++trial) {
    const size_t count = next() % 65;
    std::vector<Range> input;
    for (size_t i = 0; i < count; ++i) {
      const uint32_t start = next() % limit;
      const uint32_t length = next() % (std::min(8192u, limit - start) + 1);
      input.emplace_back(start, length);
    }
    std::vector<bool> valid(64), expected(64);
    for (size_t i = 0; i < valid.size(); ++i) valid[i] = (next() & 3) == 0;
    for (auto [start, length] : input) {
      if (!length) continue;
      for (uint32_t page = start >> 12; page <= ((start + length - 1) >> 12); ++page)
        if (!valid[page]) expected[page] = true;
    }
    Batch old_batch, new_batch;
    Check(old_batch.Build(input.data(), input.size(), limit, false) &&
          new_batch.Build(input.data(), input.size(), limit, true), "random valid input refused");
    Check(Copy(old_batch) == Copy(new_batch), "old/new byte-range semantics differ");
    uint64_t old_saved = 0, new_saved = 0;
    auto old_uploads = QueuePages(old_batch, valid, false, old_saved);
    auto new_uploads = QueuePages(new_batch, valid, true, new_saved);
    Check(Pages(old_uploads, 64) == expected && Pages(new_uploads, 64) == expected,
          "old/new uploader changed the independently derived page set");
    Check(CountPages(new_uploads) == uint64_t(std::count(expected.begin(), expected.end(), true)),
          "optimized uploader retained a duplicate page");
    Check(CountPages(old_uploads) - CountPages(new_uploads) == new_saved,
          "duplicate-page diagnostic differs from actual eliminated copies");
  }
}
}

int main() {
  try {
    ByteMergingPreservesGapsAndInput();
    BoundsAndEmptyRequests();
    StackThresholdAndLegacyStorage();
    DisjointBytesSamePageOnlyUploadOnce();
    OverlapContainmentAdjacencyAndGaps();
    CurrentValidityAndSubsequentInvalidation();
    LastPageAndBoundaryGap();
    RandomizedPageSetEquivalence();
    std::cout << "PASS: 8 native range batch controls (including 2000 randomized page-set cases)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
