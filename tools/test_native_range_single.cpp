// Original portable controls for the production single-range selector.
// No guest data, SDK linkage, OS protection changes, or GPU access.
#include "../src/gpu/native/native_range_batch.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using Range = nb::gpu::NativeSingleRangeSelection::Range;
using Kind = nb::gpu::NativeSingleRangeKind;
constexpr uint32_t kPage = 4096;
constexpr uint32_t kLimit = 130 * kPage;
uint64_t checks = 0;

void Check(bool value, const char* text) {
  ++checks;
  if (!value) throw std::runtime_error(text);
}

Kind OracleKind(const Range& range, uint32_t limit) {
  if (range.second == 0) return Kind::kEmpty;
  // Independent subtraction oracle, including starts past the bound.
  if (range.first > limit || range.second > limit - range.first) return Kind::kInvalid;
  return Kind::kReady;
}

void Selection(const Range& range, uint32_t limit) {
  const Range before = range;
  const auto selected = nb::gpu::SelectNativeSingleRange(&range, 1, limit, true);
  const Kind expected = OracleKind(range, limit);
  Check(selected.kind == expected, "selector differs from independent bounds oracle");
  Check(range == before, "selector changed input");
  Check(selected.ranges().size() == (expected == Kind::kReady ? 1u : 0u),
        "selected range count differs");
  if (expected == Kind::kReady) {
    Check(selected.ranges().data() == &selected.range && selected.ranges()[0] == range,
          "ready selection does not own the exact caller range");
  }
  for (bool optimize : {false, true}) {
    nb::gpu::NativeRangeBatch legacy;
    const bool accepted = legacy.Build(&range, 1, limit, optimize);
    Check(accepted == (expected != Kind::kInvalid), "old/new acceptance differs");
    if (accepted) {
      Check(std::equal(selected.ranges().begin(), selected.ranges().end(),
                       legacy.ranges().begin(), legacy.ranges().end()),
            "old/new prepared byte extents differ");
    }
  }
}

void BoundariesAndSelectionGates() {
  for (uint32_t limit : {0u, 1u, kLimit, uint32_t(1u << 29), UINT32_MAX}) {
    for (Range range : {Range{0, 0}, Range{UINT32_MAX, 0}, Range{0, 1},
                        Range{limit, 0}, Range{limit, 1}, Range{0, limit},
                        Range{UINT32_MAX, 1}, Range{1, UINT32_MAX},
                        Range{UINT32_MAX, UINT32_MAX}}) Selection(range, limit);
    if (limit) {
      Selection({limit - 1, 1}, limit);
      Selection({limit - 1, 2}, limit);
    }
  }
  const Range input[]{{10, 20}, {50, 30}};
  for (size_t count : {size_t(0), size_t(1), size_t(2), SIZE_MAX}) {
    Check(nb::gpu::SelectNativeSingleRange(nullptr, count, kLimit, true).kind ==
              Kind::kNotSelected, "null input was selected");
    Check(nb::gpu::SelectNativeSingleRange(input, count, kLimit, false).kind ==
              Kind::kNotSelected, "disabled selector inspected or selected input");
    if (count != 1) {
      Check(nb::gpu::SelectNativeSingleRange(input, count, kLimit, true).kind ==
                Kind::kNotSelected, "non-single count was selected");
    }
  }
  Range changed_input{123, 456};
  const auto snapshot = nb::gpu::SelectNativeSingleRange(&changed_input, 1, kLimit, true);
  changed_input = {0, 0};
  const auto copied_snapshot = snapshot;
  Check(snapshot.ranges()[0] == Range{123, 456} &&
            copied_snapshot.ranges()[0] == Range{123, 456} &&
            copied_snapshot.ranges().data() == &copied_snapshot.range,
        "selection depended on caller mutation or retained a source-object pointer");
}

// Model the unchanged downstream execution. It records allocation/lock/copy
// ordering; protection and byte copying are represented by per-page events.
// Selection alone never reads validity, grants authority, or performs uploads.
struct Model {
  std::array<bool, 130> valid{};
  std::array<bool, 130> gpu{};
  std::array<uint32_t, 130> cpu_bytes{}, host_bytes{};
  std::vector<Range> uploads;
  std::vector<uint32_t> events;

  bool Execute(std::span<const Range> ranges, bool allocation_ok, bool upload_ok) {
    uploads.clear();
    events.clear();
    if (ranges.empty()) return true;
    for ([[maybe_unused]] const auto& range : ranges) {
      events.push_back(1);  // Allocate before touching validity.
      if (!allocation_ok) return false;
    }
    events.push_back(2);  // One validity lock, no fast-path pre-scan.
    for (auto [start, length] : ranges) {
      const uint32_t first = start / kPage;
      const uint32_t last = (start + length - 1) / kPage;
      uint32_t p = first;
      while (p <= last) {
        if (valid.at(p)) { ++p; continue; }
        const uint32_t begin = p++;
        while (p <= last && !valid.at(p)) ++p;
        nb::gpu::AppendUploadPageRange(uploads, begin, p - begin, true);
      }
    }
    events.push_back(3);  // Unlock before the unchanged upload path.
    if (uploads.empty()) return true;
    events.push_back(4);  // Upload pool allocation.
    if (!upload_ok) return false;
    for (auto [first, count] : uploads) {
      events.push_back(5);  // MakeRangeValid and physical write protection.
      for (uint32_t p = first; p < first + count; ++p) {
        valid.at(p) = true;
        gpu.at(p) = false;
        host_bytes.at(p) = cpu_bytes.at(p);
      }
      events.push_back(6);  // Copy bytes, then append GPU copy command.
    }
    return true;
  }
};

void CompareExecution(const Range& range, const Model& initial,
                      bool allocation_ok = true, bool upload_ok = true) {
  const auto selected = nb::gpu::SelectNativeSingleRange(&range, 1, kLimit, true);
  Check(selected.kind != Kind::kInvalid && selected.kind != Kind::kNotSelected,
        "execution fixture was not a bounded single range");
  nb::gpu::NativeRangeBatch batch;
  Check(batch.Build(&range, 1, kLimit, true), "legacy fixture rejected");
  Model fast = initial, old = initial;
  const bool actual = fast.Execute(selected.ranges(), allocation_ok, upload_ok);
  Check(actual == old.Execute(batch.ranges(), allocation_ok, upload_ok),
        "old/new execution result differs");
  Check(fast.events == old.events && fast.uploads == old.uploads &&
            fast.valid == old.valid && fast.gpu == old.gpu &&
            fast.host_bytes == old.host_bytes, "old/new execution state or order differs");

  // A separate literal per-page oracle derives the requested invalid set and
  // expected bytes directly from the original input, without prepared ranges.
  std::array<bool, 130> expected_pages{};
  if (range.second && allocation_ok) {
    for (uint32_t p = 0; p < expected_pages.size(); ++p) {
      const uint64_t page_end = uint64_t(p + 1) * kPage;
      const uint64_t end = uint64_t(range.first) + range.second;
      expected_pages[p] = page_end > range.first && uint64_t(p) * kPage < end &&
                          !initial.valid[p];
    }
  }
  std::array<bool, 130> actual_pages{};
  for (auto [first, count] : fast.uploads)
    for (uint32_t p = first; p < first + count; ++p) actual_pages.at(p) = true;
  Check(actual_pages == expected_pages, "upload set differs from direct byte-overlap oracle");
  for (uint32_t p = 0; p < expected_pages.size(); ++p) {
    const bool copied = expected_pages[p] && upload_ok;
    Check(fast.host_bytes[p] == (copied ? initial.cpu_bytes[p] : initial.host_bytes[p]),
          "CPU copy omitted data or replaced GPU-authoritative bytes");
    Check(fast.valid[p] == (copied || initial.valid[p]) &&
              fast.gpu[p] == (!copied && initial.gpu[p]), "authority changed outside a successful copy");
  }
  if (!range.second) Check(fast.events.empty(), "empty request allocated or scanned");
  if (range.second && !allocation_ok)
    Check(fast.events == std::vector<uint32_t>{1}, "allocation failure touched validity");
}

void CoherencyFailuresAndBoundaries() {
  Model memory;
  for (uint32_t p = 0; p < memory.valid.size(); ++p) {
    memory.valid[p] = p % 3 != 0;
    memory.gpu[p] = p % 5 == 0 && memory.valid[p];
    memory.cpu_bytes[p] = 0x10000000u + p;
    memory.host_bytes[p] = 0xA0000000u + p;
  }
  const Range ranges[]{{UINT32_MAX, 0}, {0, 1}, {kPage - 1, 2},
      {63 * kPage + 7, 2 * kPage}, {64 * kPage, 64 * kPage},
      {kLimit - 1, 1}, {0, kLimit}};
  for (const auto& range : ranges) {
    for (bool allocate : {false, true})
      for (bool upload : {false, true}) CompareExecution(range, memory, allocate, upload);
  }
  memory.valid.fill(true);
  CompareExecution({0, kLimit}, memory);  // All valid: no upload.
  memory.valid[64] = memory.gpu[64] = false;  // Same-frame CPU write.
  memory.cpu_bytes[64] ^= 0xFFFFFFFFu;
  CompareExecution({63 * kPage, 3 * kPage}, memory);
  memory.valid[65] = memory.gpu[65] = true;  // GPU write stays authoritative.
  memory.host_bytes[65] = 0xDEADBEEFu;
  CompareExecution({63 * kPage, 3 * kPage}, memory);
  memory.valid = memory.gpu;  // clear_memory_page_state=true at frame close.
  CompareExecution({0, kLimit}, memory);
  memory.valid.fill(false); memory.gpu.fill(false);  // Full invalidation.
  CompareExecution({0, kLimit}, memory);
}

void RandomizedSelectionAndLiveState() {
  uint32_t state = 0x193AC731;
  auto next = [&]() { state ^= state << 13; state ^= state >> 17; return state ^= state << 5; };
  for (uint32_t i = 0; i < 20000; ++i) {
    Selection({next(), next()}, next());
    const uint32_t start = next() % kLimit;
    const Range bounded{start, next() % (kLimit - start + 1)};
    Selection(bounded, kLimit);
    if (i % 10) continue;
    Model memory;
    for (uint32_t p = 0; p < memory.valid.size(); ++p) {
      memory.valid[p] = (next() & 1) != 0;
      memory.gpu[p] = memory.valid[p] && (next() & 1);
      memory.cpu_bytes[p] = next(); memory.host_bytes[p] = next();
    }
    CompareExecution(bounded, memory, (next() & 7) != 0, (next() & 7) != 0);
  }
}
}  // namespace

int main() {
  try {
    BoundariesAndSelectionGates();
    CoherencyFailuresAndBoundaries();
    RandomizedSelectionAndLiveState();
    std::cout << "PASS: " << checks << " single-range selector/control checks; "
                 "40000 randomized selections, 2000 live-state model cases\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
