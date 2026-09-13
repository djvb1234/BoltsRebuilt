// Original portable controls for the count-only VS packet observer.
// Build and execution are coordinated by the root task; no SDK or GPU required.
#include "../src/gpu/native/native_vertex_packet_diagnostics.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using Observer = nb::gpu::NativeVertexPacketDiagnostics;
uint64_t checks = 0;
void Check(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}

void LiteralControls() {
  Observer observer;
  std::array<uint8_t, Observer::kCapacity> packet;
  for (size_t i = 0; i < packet.size(); ++i) packet[i] = uint8_t(i * 73 + 19);
  auto result = observer.Observe(0, packet);
  Check(result.observed && !result.hit && result.new_pass_frame && !result.equal_bytes,
        "first complete packet cannot hit");
  result = observer.Observe(0, packet);
  Check(result.hit && !result.new_pass_frame && result.equal_bytes == packet.size(),
        "complete same-frame packet hits");
  // Mutate every byte, including all header lanes, signaling-NaN bit patterns,
  // high extra-stream lanes and the final float byte. No float comparison occurs.
  for (size_t i = 0; i < packet.size(); ++i) {
    packet[i] ^= 0x80;
    Check(!observer.Observe(0, packet).hit, "changed byte at same address misses");
    Check(observer.Observe(0, packet).hit, "changed packet owns independent snapshot");
    packet[i] ^= 0x80;
    Check(!observer.Observe(0, packet).hit, "restoring old bytes differs from last packet");
  }
  Check(observer.Observe(0, packet).hit, "restored snapshot now hits");
  Check(!observer.Observe(uint64_t(1) << 32, packet).hit, "all frame bits participate");
  Check(observer.Observe(uint64_t(1) << 32, packet).hit, "high frame value can hit");
  Check(!observer.Observe(0, packet).hit, "frame regression cannot reuse old identity");
  const auto prefix = std::span<const uint8_t>(packet).first(packet.size() - 1);
  Check(!observer.Observe(0, prefix).hit, "same prefix different length misses");
  Check(observer.Observe(0, prefix).equal_bytes == prefix.size(), "equal-byte count uses full length");
  Check(!observer.Observe(0, {}).observed, "empty input invalidates");
  Check(!observer.Observe(0, prefix).hit, "empty input clears identity");
  std::vector<uint8_t> oversized(Observer::kCapacity + 1, 0);
  Check(!observer.Observe(0, oversized).observed, "oversized input invalidates without reading beyond storage");
  Check(!observer.Observe(0, prefix).hit, "oversize clears identity");
  observer.Reset();
  result = observer.Observe(0, packet);
  Check(!result.hit && result.new_pass_frame, "owner reset clears identity and observed-frame history");
  Observer separate;
  Check(!separate.Observe(0, packet).hit, "different pass cannot hit another owner");

  uint64_t bypasses = 0;
  { Observer::Attempt attempt(observer, true, true, bypasses); }
  Check(bypasses == 1 && !observer.Observe(0, packet).hit, "early return invalidates and counts once");
  { Observer::Attempt attempt(observer, false, true, bypasses); }
  Check(bypasses == 1 && !observer.Observe(0, packet).hit, "disabled attempt invalidates without counter work");
  {
    Observer::Attempt attempt(observer, true, false, bypasses);
    Check(!attempt.Complete(0, packet).observed, "asset pointer refuses observation");
  }
  Check(bypasses == 2 && !observer.Observe(0, packet).hit, "ineligible attempt invalidates");
  {
    Observer::Attempt attempt(observer, true, true, bypasses);
    Check(attempt.Complete(0, packet).hit, "successful scoped observation hits");
  }
  Check(bypasses == 2 && observer.Observe(0, packet).hit, "successful scope retains identity");
  try {
    Observer::Attempt attempt(observer, true, true, bypasses);
    throw std::runtime_error("deliberate interrupted Record");
  } catch (const std::runtime_error&) {}
  Check(bypasses == 3 && !observer.Observe(0, packet).hit, "unwinding invalidates");
}

// Independent event oracle uses a dynamic byte vector and direct element
// comparisons, rather than the production storage/memcmp implementation.
void EventControls() {
  Observer observer;
  uint64_t bypasses = 0, expected_bypasses = 0, frame = 1, previous_frame = 0;
  bool valid = false, frame_seen = false;
  std::vector<uint8_t> previous, packet(704, 0);
  uint32_t random = 0x6134A9B7;
  auto next = [&]() { random ^= random << 13; random ^= random >> 17; random ^= random << 5; return random; };
  for (uint32_t event = 0; event < 16000; ++event) {
    const uint32_t bits = next();
    if ((bits & 31) == 0) ++frame;
    if ((bits & 15) == 1) packet.resize(704 + (next() % 256) * 16, uint8_t(bits));
    if ((bits & 7) == 2) packet[next() % packet.size()] ^= uint8_t((bits >> 16) | 1);
    const bool enabled = (bits & 63) != 3;
    const bool eligible = (bits & 63) != 4;
    const bool complete = (bits & 63) != 5;
    const bool finish = (bits & 63) != 6;
    {
      Observer::Attempt attempt(observer, enabled, eligible, bypasses);
      Observer::Result result;
      if (finish) result = attempt.Complete(frame, complete ? std::span<const uint8_t>(packet)
                                                           : std::span<const uint8_t>{});
      const bool observed = enabled && eligible && complete && finish;
      bool equal = valid && previous_frame == frame && previous.size() == packet.size();
      if (equal) {
        for (size_t i = 0; i < packet.size(); ++i) equal = equal && previous[i] == packet[i];
      }
      Check(result.observed == observed, "event observation eligibility");
      Check(result.hit == (observed && equal), "event exact identity");
      Check(result.equal_bytes == (observed && equal ? packet.size() : 0), "event equal-byte accounting");
      Check(result.new_pass_frame == (observed && (!frame_seen || previous_frame != frame)),
            "event per-pass observed-frame accounting");
      if (observed) {
        previous = packet;
        previous_frame = frame;
        valid = frame_seen = true;
      } else {
        valid = false;
        expected_bypasses += enabled;
      }
    }
    Check(bypasses == expected_bypasses, "event bypass accounting after scope exit");
  }
}
}  // namespace

int main() {
  try {
    LiteralControls();
    EventControls();
    std::cout << "PASS " << checks << " vertex-packet diagnostic checks\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
    return 1;
  }
}
