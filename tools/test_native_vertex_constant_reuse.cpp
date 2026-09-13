// Original portable controls for exact VS packet identity and publication.
// Real upload-buffer lifetime/output controls are in a separate D3D12 probe.
#include "../src/gpu/native/native_vertex_constant_reuse.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using Reuse = nb::gpu::NativeVertexConstantReuse;
uint64_t checks = 0;
void Check(bool value, const char* why) {
  ++checks;
  if (!value) throw std::runtime_error(why);
}
void Candidate(Reuse& cache, const std::vector<uint8_t>& bytes) {
  // Independent element writes, including every unused header lane.
  for (size_t i = 0; i < bytes.size(); ++i) cache.candidate_data()[i] = bytes[i];
}
std::vector<uint8_t> Packet(size_t size, uint32_t seed) {
  std::vector<uint8_t> bytes(size);
  for (size_t i = 0; i < size; ++i) bytes[i] = uint8_t((i * 197) ^ seed ^ (seed >> 13));
  return bytes;
}
void LiteralControls() {
  Reuse cache;
  auto bytes = Packet(Reuse::kCapacity, 0x78190AE3);
  Candidate(cache, bytes);
  Check(cache.Find(0, bytes.size()) == 0, "unpublished bytes cannot hit");
  auto* first_candidate = cache.candidate_data();
  Check(cache.Publish(0, bytes.size(), 0x100000), "complete publication accepted");
  Check(cache.candidate_data() != first_candidate, "publication swaps RAM ownership");
  Candidate(cache, bytes);
  Check(cache.Find(0, bytes.size()) == 0x100000, "same-frame complete packet returns exact address");
  // All4,784 bytes must participate, including both signs of zero, NaN payload
  // bits, upper texture/stream fields and the last packed-constant byte.
  for (size_t i = 0; i < bytes.size(); ++i) {
    cache.candidate_data()[i] ^= 0x80;
    Check(cache.Find(0, bytes.size()) == 0, "changed candidate byte must miss");
    Check(first_candidate[i] == bytes[i], "candidate writes must not mutate active RAM");
    cache.candidate_data()[i] ^= 0x80;
    Check(cache.Find(0, bytes.size()) == 0x100000, "restoring candidate retains published identity");
  }
  Check(cache.Find(0, bytes.size() - 1) == 0, "different length cannot match same prefix");
  Check(cache.Find(uint64_t(1) << 32, bytes.size()) == 0, "upper frame bits participate");
  Check(!cache.Publish(1, bytes.size(), 0), "failed GPU allocation cannot publish");
  Check(!cache.Publish(1, 0, 0x200000), "empty packet cannot publish");
  Check(!cache.Publish(1, Reuse::kCapacity + 1, 0x200000), "oversize packet cannot publish");
  Check(cache.Find(0, bytes.size()) == 0x100000, "failed publication retains original mapping");
  Check(cache.Find(0, 0) == 0 && cache.Find(0, Reuse::kCapacity + 1) == 0,
        "invalid lookup size refuses before byte access");
  Check(cache.Publish(1, bytes.size(), 0x100000), "allocator may recycle a completed old-frame address");
  Candidate(cache, bytes);
  Check(cache.Find(0, bytes.size()) == 0 && cache.Find(1, bytes.size()) == 0x100000,
        "recycled address cannot revive old-frame identity");
  cache.Reset();
  Check(cache.Find(1, bytes.size()) == 0, "owner/reset clears a live exact match");
  Reuse other;
  Candidate(other, bytes);
  Check(other.Find(1, bytes.size()) == 0, "another pass cannot reuse this owner's address");
  Check(cache.Publish(1, bytes.size(), 0x300000), "republication after reset");
  Candidate(cache, bytes);
  Check(cache.Find(1, bytes.size()) == 0x300000, "reset does not corrupt candidate storage");
}

// Reference state owns bytes independently and compares elements. Publications
// model immutable upload slices; later candidate changes cannot change them.
void EventControls() {
  struct Published { uint64_t frame, address; std::vector<uint8_t> bytes; };
  Reuse cache;
  std::vector<Published> immutable;
  std::vector<uint8_t> active, bytes = Packet(704, 0x37219);
  bool valid = false;
  uint64_t frame = 7, active_frame = 0, address = 0, next_address = 0x100000000;
  uint32_t random = 0x4187382A;
  auto next = [&]() { random ^= random << 13; random ^= random >> 17; return random ^= random << 5; };
  for (uint32_t event = 0; event < 16000; ++event) {
    const uint32_t bits = next();
    if ((bits & 63) == 0) ++frame;
    if ((bits & 31) == 1) { cache.Reset(); valid = false; }
    if ((bits & 15) == 2) {
      const size_t size = 704 + (next() % 256) * 16;
      bytes = Packet(size, next());
    }
    else if ((bits & 7) == 3) bytes[next() % bytes.size()] ^= uint8_t((bits >> 8) | 1);
    Candidate(cache, bytes);
    bool equal = valid && frame == active_frame && active.size() == bytes.size();
    if (equal) for (size_t i = 0; i < bytes.size(); ++i) equal = equal && active[i] == bytes[i];
    const uint64_t found = cache.Find(frame, bytes.size());
    Check(found == (equal ? address : 0), "lookup differs from independent exact-byte oracle");
    if (found) {
      const auto slice = std::find_if(immutable.begin(), immutable.end(),
          [&](const Published& item) { return item.address == found; });
      Check(slice != immutable.end() && slice->frame == frame && slice->bytes == bytes,
            "returned address must name the complete current-frame immutable payload");
    }
    if (!found) {
      if ((bits & 63) == 4) {
        Check(!cache.Publish(frame, bytes.size(), 0), "failed allocation rejected in event stream");
        continue;
      }
      // The complete slice is copied before publication, just as production.
      immutable.push_back({frame, next_address, bytes});
      Check(cache.Publish(frame, bytes.size(), next_address), "event publication succeeds");
      active = bytes; active_frame = frame; address = next_address; valid = true;
      next_address += (bytes.size() + 255) & ~uint64_t(255);
      Candidate(cache, bytes);
      Check(cache.Find(frame, bytes.size()) == address, "new publication is findable only after rebuilding candidate");
    }
    // Alter the scratch packet after lookup/publication. It must be rebuilt on
    // the next event, and must never silently replace an already-published slice.
    if (!bytes.empty()) cache.candidate_data()[0] ^= 0x80;
  }
  Check(immutable.size() > 100, "event stream exercised many independent publications");
}
}  // namespace
int main() {
  try {
    LiteralControls(); EventControls();
    std::cout << "PASS " << checks << " vertex constant reuse checks\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
    return 1;
  }
}
