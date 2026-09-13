// Original CPU controls. The independent memory oracle models SDK page
// authority; this executable does not use Windows, D3D12 or guest assets.
#include "../src/gpu/native/native_upload_shadow.h"
#include "../src/gpu/native/native_upload_shadow_compare.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
#include <vector>

namespace {
using nb::gpu::NativeUploadShadow;
constexpr uint32_t kPage = 4096;
uint64_t checks = 0;
void Check(bool value, const char* reason) {
  ++checks;
  if (!value) throw std::runtime_error(reason);
}
std::vector<uint8_t> Data(size_t size, unsigned seed) {
  std::vector<uint8_t> out(size);
  for (size_t i = 0; i < size; ++i) out[i] = uint8_t((i * 73 + seed * 29) ^ (i >> 5));
  return out;
}
void ExactBytes() {
  NativeUploadShadow shadow;
  Check(shadow.Initialize(kPage, 2 * kPage), "initialize");
  auto bytes = Data(kPage, 4);
  Check(!shadow.Matches(0, bytes), "unpublished bytes never match");
  Check(shadow.Publish(0, bytes, shadow.epoch()), "first publication");
  for (size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] ^= 0xA5;
    Check(!shadow.Matches(0, bytes), "every single-byte mutation is detected");
    bytes[i] ^= 0xA5;
    Check(shadow.Matches(0, bytes), "byte-exact restoration matches");
  }
}
void BoundsAndPageSizes() {
  auto bytes = Data(kPage, 1);
  for (uint32_t page : {4096u, 8192u, 16384u, 32768u, 65536u}) {
    NativeUploadShadow shadow;
    Check(shadow.Initialize(page, 2 * page), "supported full host page");
    auto page_bytes = Data(page, page);
    const uint32_t last = NativeUploadShadow::kPhysicalBytes - page;
    Check(shadow.Publish(last, page_bytes, shadow.epoch()), "last physical page publishes");
    Check(shadow.Matches(last, page_bytes), "last physical page matches");
    Check(!shadow.Publish(last + 1, page_bytes, shadow.epoch()), "unaligned/end overrun refuses");
    Check(!shadow.Publish(NativeUploadShadow::kPhysicalBytes, page_bytes, shadow.epoch()), "outside physical memory");
    Check(!shadow.Matches(last, std::span<const uint8_t>(page_bytes).first(page - 1)), "partial-page compare refuses");
  }
  for (uint32_t page : {0u, 1u, 2048u, 4097u, 131072u}) {
    NativeUploadShadow shadow;
    Check(!shadow.Initialize(page, 1u << 20), "unsupported page size refuses");
    Check(!shadow.Publish(0, bytes, shadow.epoch()), "failed initialization cannot publish");
  }
  NativeUploadShadow shadow;
  Check(!shadow.Initialize(kPage, kPage - 1), "too-small capacity refuses");
  Check(!shadow.Initialize(kPage, kPage + 1), "unaligned capacity refuses");
}
void TokensAndPartialGpuWrites() {
  NativeUploadShadow shadow;
  auto bytes = Data(kPage, 2);
  Check(shadow.Initialize(kPage, 3 * kPage), "token cache initialized");
  Check(shadow.Publish(kPage, bytes, shadow.epoch()), "baseline queued bytes");
  const uint64_t pending = shadow.epoch();
  shadow.Invalidate(kPage + 57, kPage + 57);
  Check(!shadow.Matches(kPage, bytes), "one-byte GPU write invalidates full page");
  Check(!shadow.Publish(kPage, bytes, pending), "GPU write rejects pending publication");
  Check(shadow.Publish(kPage, bytes, shadow.epoch()), "actual restoring upload can publish");
  const uint64_t unrelated = shadow.epoch();
  shadow.Invalidate(10 * kPage, 10 * kPage);
  Check(shadow.Matches(kPage, bytes), "unrelated invalidation preserves known bytes");
  Check(!shadow.Publish(kPage, bytes, unrelated), "unrelated race conservatively rejects pending token");
  const uint64_t before_reset = shadow.epoch();
  shadow.Reset();
  Check(!shadow.Matches(kPage, bytes), "full reset forgets identity");
  Check(!shadow.Publish(kPage, bytes, before_reset), "reset rejects pending publication");
  const uint64_t before_reinitialize = shadow.epoch();
  Check(shadow.Initialize(kPage, 2 * kPage), "resource reinitialize");
  Check(!shadow.Publish(kPage, bytes, before_reinitialize), "reinitialize does not recycle old token");
  Check(shadow.Publish(kPage, bytes, shadow.epoch()), "new resource publication");
  shadow.Invalidate(0, UINT32_MAX);
  Check(!shadow.Matches(kPage, bytes), "clamped whole-resource invalidation");
}
void FifoOracle() {
  NativeUploadShadow shadow;
  Check(shadow.Initialize(kPage, 3 * kPage), "FIFO initialized");
  struct Entry { std::vector<uint8_t> bytes; bool valid; };
  std::map<uint32_t, Entry> oracle;
  std::deque<uint32_t> insertion_order;
  std::mt19937 rng(0x83AB19);
  for (unsigned event = 0; event < 10000; ++event) {
    const uint32_t address = (rng() % 9) * kPage;
    auto bytes = Data(kPage, rng() % 5);
    switch (rng() % 8) {
      case 0: {
        shadow.Reset(); oracle.clear(); insertion_order.clear(); break;
      }
      case 1: {
        shadow.Invalidate(address + 31, address + 31);
        if (oracle.count(address)) oracle.at(address).valid = false;
        break;
      }
      case 2:
      case 3: {
        const auto found = oracle.find(address);
        const bool expected = found != oracle.end() && found->second.valid && found->second.bytes == bytes;
        Check(shadow.Matches(address, bytes) == expected, "independent FIFO lookup oracle");
        break;
      }
      default: {
        if (!oracle.count(address)) {
          if (insertion_order.size() == 3) {
            oracle.erase(insertion_order.front()); insertion_order.pop_front();
          }
          insertion_order.push_back(address);
        }
        oracle[address] = {bytes, true};
        Check(shadow.Publish(address, bytes, shadow.epoch()), "FIFO publication");
        for (const auto& [at, entry] : oracle) {
          Check(shadow.Matches(at, entry.bytes) == entry.valid, "all surviving FIFO identities");
        }
      }
    }
  }
}

// This oracle stores the logical bytes of a hypothetical unoptimized GPU buffer.
// CPU changes do not modify that buffer until a requested invalid page uploads.
struct MemoryModel {
  static constexpr unsigned kPages = 40;
  NativeUploadShadow shadow;
  std::vector<uint8_t> cpu = Data(kPages * kPage, 7);
  std::vector<uint8_t> actual = std::vector<uint8_t>(kPages * kPage, 0xC3);
  std::vector<uint8_t> oracle = actual;
  std::array<bool, kPages> valid{}, gpu_written{};
  uint64_t skipped = 0, copied = 0;
  MemoryModel() { Check(shadow.Initialize(kPage, 11 * kPage), "memory model initialized"); }
  void CpuWrite(unsigned page, unsigned offset, uint8_t value, bool watched = true) {
    cpu[page * kPage + offset] = value;
    if (watched) valid[page] = gpu_written[page] = false;
  }
  void GpuWrite(unsigned page, unsigned offset, uint8_t value) {
    actual[page * kPage + offset] = oracle[page * kPage + offset] = value;
    valid[page] = gpu_written[page] = true;
    shadow.Invalidate(page * kPage + offset, page * kPage + offset);
  }
  void FrameClose() { valid = gpu_written; }
  bool Request(unsigned first, unsigned count, unsigned pool_pages = 17,
               unsigned fail_allocation = UINT32_MAX, bool use_shadow = true) {
    unsigned allocation = 0;
    for (unsigned page = first; page < first + count;) {
      if (valid[page]) { ++page; continue; }
      unsigned end = page;
      while (end < first + count && !valid[end] && end - page < pool_pages) ++end;
      if (allocation++ == fail_allocation) return false; // before marking anything valid
      for (unsigned mark = page; mark < end; ++mark) valid[mark] = true;
      for (; page < end; ++page) {
        const auto captured = std::span<const uint8_t>(cpu).subspan(page * kPage, kPage);
        const uint64_t token = shadow.epoch();
        // Independent reference always copies every requested invalid page.
        std::copy(captured.begin(), captured.end(), oracle.begin() + page * kPage);
        if (use_shadow && shadow.Matches(page * kPage, captured)) {
          ++skipped;
        } else {
          ++copied;
          std::copy(captured.begin(), captured.end(), actual.begin() + page * kPage);
          if (use_shadow)
            Check(shadow.Publish(page * kPage, captured, token), "model queued copy publication");
        }
      }
    }
    return true;
  }
  void Verify() { Check(actual == oracle, "queued GPU bytes equal uncached memory oracle"); }
};
void AuthorityAndFailureOracle() {
  MemoryModel model;
  Check(!model.Request(0, 40, 17, 1), "second allocation failure modeled");
  for (unsigned page = 0; page < 40; ++page)
    Check(model.valid[page] == (page < 17), "uncopied failed-allocation pages stay invalid");
  model.Verify();
  Check(model.Request(0, 40), "retry missing pages"); model.Verify();
  // All aliases map to the same physical page; these are memory-policy controls,
  // not tests of Windows page protection or the SDK's actual A/C/E heaps.
  for (unsigned alias = 0; alias < 3; ++alias) {
    model.CpuWrite(35, 100 + alias, uint8_t(0xF0 + alias));
    Check(model.Request(35, 1), "A/C/E alias rewrite reuploads"); model.Verify();
  }
  model.FrameClose();
  Check(model.Request(35, 1), "frame clear still fully revalidates");
  Check(model.skipped != 0, "unchanged page can skip physical copy"); model.Verify();
  const uint8_t original_cpu = model.cpu[35 * kPage + 80];
  model.GpuWrite(35, 80, uint8_t(original_cpu ^ 0xFF));
  const auto copied_before = model.copied;
  model.CpuWrite(35, 80, original_cpu); // equal to OLD shadow but not current GPU
  Check(model.Request(35, 1), "CPU restoration after partial GPU write");
  Check(model.copied == copied_before + 1, "GPU invalidation forces upload despite old-byte equality");
  model.Verify();
  model.CpuWrite(35, 99, uint8_t(model.cpu[35 * kPage + 99] ^ 0xA5), false);
  model.FrameClose();
  Check(model.Request(35, 1), "untracked CPU change detected after frame clear"); model.Verify();
  // A readonly->writable change clears ordinary authority before the new write.
  model.valid[35] = model.gpu_written[35] = false;
  model.CpuWrite(35, 104, 0x43);
  Check(model.Request(35, 1), "readonly-to-writable revalidation"); model.Verify();
  model.shadow.Reset(); // mode off/on or a new resource lifetime
  model.FrameClose();
  Check(model.Request(0, 40), "off-on identity reset"); model.Verify();
}
void RandomMemoryOracle() {
  MemoryModel model;
  std::mt19937 rng(0x7012048);
  for (unsigned event = 0; event < 5000; ++event) {
    const unsigned page = rng() % MemoryModel::kPages;
    const unsigned offset = rng() % kPage;
    switch (rng() % 6) {
      case 0: model.CpuWrite(page, offset, uint8_t(rng())); break;
      case 1: model.GpuWrite(page, offset, uint8_t(rng())); break;
      case 2: model.FrameClose(); break;
      case 3: model.shadow.Reset(); break;
      default: {
        const unsigned count = 1 + rng() % (MemoryModel::kPages - page);
        Check(model.Request(page, count, 1 + rng() % 19), "random request"); break;
      }
    }
    model.Verify();
  }
}
void ClassifiedHitRetryOracle() {
  for (bool gpu_change : {false, true}) {
    MemoryModel model;
    constexpr unsigned page = 5, offset = 87;
    Check(model.Request(page, 1), "prime exact GPU-byte identity");
    model.FrameClose();
    // Model original allocation -> MakeRangeValid -> capture/classification.
    model.valid[page] = true;
    const auto captured = std::span<const uint8_t>(model.cpu).subspan(page * kPage, kPage);
    const uint64_t request_epoch = model.shadow.epoch();
    Check(model.shadow.Matches(page * kPage, captured), "hit classified before racing event");
    const uint8_t replacement = uint8_t(model.cpu[page * kPage + offset] ^ 0xA7);
    if (gpu_change) {
      model.GpuWrite(page, offset, replacement);
    } else {
      model.shadow.Reset();
      model.valid.fill(false); model.gpu_written.fill(false);
      model.CpuWrite(page, offset, replacement);
    }
    Check(model.shadow.epoch() != request_epoch, "final epoch observes invalidated classified hit");
    model.shadow.Invalidate(page * kPage, (page + 1) * kPage - 1);
    const uint64_t copied_before = model.copied;
    // The retry's independent live-validity scan bypasses the shadow. It must
    // never force a CPU copy solely because the old classification was a hit.
    Check(model.Request(page, 1, 17, UINT32_MAX, false), "ordinary authority-aware retry");
    Check(model.copied - copied_before == (gpu_change ? 0u : 1u),
          "retry copies CPU-invalid page and keeps GPU-authoritative page");
    Check(model.actual[page * kPage + offset] == replacement, "latest authoritative bytes preserved");
    model.Verify();
  }
}
void DirectClassification() {
  for (uint32_t page_bytes : {4096u, 8192u, 16384u, 32768u, 65536u}) {
    NativeUploadShadow shadow;
    Check(shadow.Initialize(page_bytes, 65536), "direct classifier initialized");
    auto source = Data(65536, 17);
    std::vector<uint8_t> scratch(65536, 0xCD);
    const auto initial = nb::gpu::ClassifyUploadPages(shadow, 0, source, scratch.data(), true, false);
    const uint32_t pages = 65536 / page_bytes;
    Check(initial.changed_mask == ((1u << pages) - 1), "cold mask captures every page");
    Check(initial.snapshot_bytes == 65536 && scratch == source, "cold exact snapshot");
    for (uint32_t p = 0; p < pages; ++p)
      Check(shadow.Publish(p * page_bytes, std::span<const uint8_t>(scratch).subspan(p * page_bytes, page_bytes), shadow.epoch()), "publish captured page");
    std::fill(scratch.begin(), scratch.end(), 0xCD);
    auto hit = nb::gpu::ClassifyUploadPages(shadow, 0, source, scratch.data(), true, false);
    Check(!hit.changed_mask && !hit.snapshot_bytes && hit.matched_pages == pages, "hits do not snapshot");
    Check(std::all_of(scratch.begin(), scratch.end(), [](uint8_t b) { return b == 0xCD; }), "hit scratch untouched");
    for (uint32_t p = 0; p < pages; p += 2) source[p * page_bytes + 13] ^= 1;
    const auto mixed = nb::gpu::ClassifyUploadPages(shadow, 0, source, scratch.data(), true, false);
    for (uint32_t p = 0; p < pages; ++p) {
      Check(bool(mixed.changed_mask & (1u << p)) == !(p & 1), "mixed changed mask");
      if (!(p & 1)) Check(std::equal(source.begin() + p * page_bytes, source.begin() + (p + 1) * page_bytes, scratch.begin() + p * page_bytes), "changed page captured exactly");
    }
    auto old_scratch = source;
    const auto old = nb::gpu::ClassifyUploadPages(shadow, 0, old_scratch, old_scratch.data(), false, false);
    Check(old.changed_mask == mixed.changed_mask, "old and direct classification agree");
    const uint64_t captured_epoch = shadow.epoch();
    shadow.Reset();
    Check(!shadow.Publish(0, std::span<const uint8_t>(scratch).first(page_bytes), captured_epoch), "direct capture reset race rejects publication");
    Check(!mixed.compare_ns && !mixed.snapshot_ns, "disabled clocks stay zero");
  }
}
}  // namespace
int main() {
  try {
    ExactBytes(); BoundsAndPageSizes(); TokensAndPartialGpuWrites(); FifoOracle();
    AuthorityAndFailureOracle(); RandomMemoryOracle(); ClassifiedHitRetryOracle(); DirectClassification();
    std::cout << "PASS: 8 upload-shadow controls, " << checks << " checks\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n'; return 1;
  }
}
