// CPU-only controls for the production native binding decision state.
// From a Visual Studio developer shell, with the machine lock held by the caller:
// cl /nologo /EHsc /std:c++20 /W4 /WX tools\test_native_static_bindings.cpp /Fe:<scratch>\test_native_static_bindings.exe
#include "../src/gpu/vendored/include/rex/graphics/d3d12/native_static_bindings.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

using Cache = rex::graphics::d3d12::NativeStaticBindings;
using Values = std::array<uint64_t, 4>;

namespace {
unsigned checks = 0;
void Check(bool condition, const char* label) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", label);
    std::exit(1);
  }
}

void LiteralTransitions() {
  const Values values = {100, 200, 300, 400};
  Cache cache;
  Check(cache.Update(1, values, false, true) == 13, "cold draw without sampler");
  Check(cache.Update(1, values, false, true) == 0, "same draw reuses three roots");
  Check(cache.stats().rebinds == 3 && cache.stats().hits == 3, "binding counters, not draw counters");
  Check(cache.Update(1, values, true, true) == 2, "first sampler use binds only root 5");
  Check(cache.Update(1, values, false, true) == 0, "unused sampler does not invalidate other roots");
  Check(cache.Update(1, values, true, true) == 0, "later sampler reuse");

  Values changed = values;
  changed[0] += 1;
  Check(cache.Update(1, changed, true, true) == 1, "texture handle changes alone");
  changed[1] += 1;
  Check(cache.Update(1, changed, false, true) == 0, "unused changed sampler is not written");
  Check(cache.Update(1, changed, true, true) == 2, "changed sampler bound on next use");
  changed[2] += 1;
  Check(cache.Update(1, changed, true, true) == 4, "guest buffer address changes alone");
  changed[3] += 1;
  Check(cache.Update(1, changed, true, true) == 8, "asset buffer address changes alone");
  Check(cache.Update(2, changed, true, true) == 15, "another geometry signature");
  Check(cache.Update(1, changed, true, true) == 15, "signature ABA must rebind all");

  // An intervening handwritten pass can overwrite roots with the SAME signature.
  cache.InvalidateAll();
  Check(cache.Update(1, changed, true, true) == 15, "same-signature external root writes");
  cache.InvalidateAll();
  Check(cache.Update(1, changed, true, true) == 15, "emulated transition");
  cache.InvalidateAll();
  Check(cache.Update(1, changed, true, true) == 15, "raw list access without signature change");

  cache.InvalidateDescriptorTables();
  Check(cache.Update(1, changed, true, true) == 3, "heap replacement preserves raw SRVs");
  cache.InvalidateDescriptorTables();  // A -> B.
  cache.InvalidateDescriptorTables();  // B -> recycled A, without a native draw on B.
  Check(cache.Update(1, changed, true, true) == 3, "heap ABA cannot be inferred from equal handles");
  cache.InvalidateDescriptorTables();
  Check(cache.Update(1, changed, false, true) == 1, "heap reset with unused sampler");
  Check(cache.Update(1, changed, true, true) == 2, "sampler stays invalid until used");

  cache.InvalidateAll();  // A new command list has no root argument state.
  Check(cache.Update(1, changed, false, true) == 13, "new command list with same addresses");
  Check(cache.Update(1, changed, true, true) == 2, "new list delayed sampler use");

  changed[0] += uint64_t(1) << 40;
  Check(cache.Update(1, changed, true, true) == 1, "full 64-bit descriptor handle comparison");
  changed[2] += uint64_t(1) << 40;
  Check(cache.Update(1, changed, true, true) == 4, "full 64-bit GPU virtual address comparison");

  Cache diagnostic;
  Check(diagnostic.Update(7, values, true, true) == 15, "diagnostic initial bind");
  Check(diagnostic.Update(7, values, true, false) == 15, "diagnostic disabled bind");
  Check(diagnostic.Update(7, values, true, false) == 15, "disabled mode never caches");
  Check(diagnostic.Update(7, values, true, true) == 15, "reenabled mode starts invalid");
  Check(diagnostic.Update(7, values, true, true) == 0, "reenabled mode caches subsequent draw");
  Check(diagnostic.stats().rebinds == 16 && diagnostic.stats().hits == 4,
        "diagnostic mode counter attribution");
}

void RecordingOracle() {
  // Model D3D12 root argument state independently: command-list/root changes
  // discard all arguments, descriptor heap changes discard only table arguments,
  // and each returned write changes one actual GPU argument. Every requested
  // argument must be valid and correct after recording, including disabled mode.
  Cache cache;
  Values gpu_values{};
  uintptr_t gpu_signature = 0;
  uint32_t gpu_valid = 0;
  uint32_t random = 0x725819A3u;
  auto next = [&]() {
    random ^= random << 13;
    random ^= random >> 17;
    random ^= random << 5;
    return random;
  };
  for (uint32_t event = 0; event < 20000; ++event) {
    const uint32_t operation = next() % 10;
    if (operation == 0) {
      gpu_signature = 0;  // New command list.
      gpu_valid = 0;
      cache.InvalidateAll();
      continue;
    }
    if (operation == 1) {
      gpu_valid = 0;  // External/emulated root writes, possibly same signature.
      cache.InvalidateAll();
      continue;
    }
    if (operation == 2) {
      gpu_valid &= ~3u;  // Heap changes including A -> B -> A with equal handles.
      cache.InvalidateDescriptorTables();
      cache.InvalidateDescriptorTables();
      continue;
    }
    const uintptr_t signature = 1u + next() % 3;
    const bool use_sampler = next() % 4 != 0;
    const bool enabled = next() % 5 != 0;
    Values wanted = {uint64_t(100 + next() % 3), uint64_t(200 + next() % 3),
                     uint64_t(300 + next() % 3), uint64_t(400 + next() % 3)};
    if (gpu_signature != signature) {
      gpu_signature = signature;
      gpu_valid = 0;
    }
    const uint32_t writes = cache.Update(signature, wanted, use_sampler, enabled);
    const uint32_t requested = use_sampler ? 15u : 13u;
    Check((writes & ~requested) == 0, "never writes an unused argument");
    if (!enabled) Check(writes == requested, "disabled mode emits every requested argument");
    for (uint32_t i = 0; i < 4; ++i) {
      const uint32_t bit = 1u << i;
      if (writes & bit) {
        gpu_values[i] = wanted[i];
        gpu_valid |= bit;
      }
      if (requested & bit) {
        Check((gpu_valid & bit) && gpu_values[i] == wanted[i],
              "recorded native draw sees the requested root argument");
      }
    }
  }
}

void PixelLiteralTransitions() {
  Cache cache;
  constexpr uintptr_t a = 11, b = 12;
  constexpr uint64_t first = 0x123400001000ULL;
  constexpr uint64_t second = 0x123400002000ULL;
  Check(cache.NeedsPixelConstantBufferWrite(a, first, true), "cold pixel bind");
  Check(cache.NeedsPixelConstantBufferWrite(a, first, true),
        "query alone must not publish an unrecorded pixel bind");
  Check(cache.stats().pixel_rebinds == 0 && cache.stats().pixel_hits == 0,
        "unrecorded pixel queries do not count as writes");
  cache.DidWritePixelConstantBuffer(first, true);
  Check(!cache.NeedsPixelConstantBufferWrite(a, first, true), "pixel reuse after recorded write");
  Check(cache.NeedsPixelConstantBufferWrite(a, second, true), "changed immutable allocation");
  Check(cache.NeedsPixelConstantBufferWrite(a, second, true), "failed append does not publish target address");
  Check(!cache.NeedsPixelConstantBufferWrite(a, first, true),
        "failed different-address append preserves previously recorded binding");
  cache.DidWritePixelConstantBuffer(second, true);
  Check(!cache.NeedsPixelConstantBufferWrite(a, second, true), "new address published after write");
  Check(cache.NeedsPixelConstantBufferWrite(a, second ^ (uint64_t(1) << 63), true),
        "pixel comparison includes highest address bit");

  // Static-root policy is independent: changing unrelated values or disabling
  // their reuse must not erase a successfully recorded root-CBV identity.
  cache.Update(a, {1, 2, 3, 4}, true, false);
  Check(!cache.NeedsPixelConstantBufferWrite(a, second, true), "static reuse disabled, pixel reuse enabled");
  cache.Update(a, {5, 6, 7, 8}, false, true);
  Check(!cache.NeedsPixelConstantBufferWrite(a, second, true), "other native root changes preserve pixel CBV");
  cache.InvalidateDescriptorTables();
  Check(!cache.NeedsPixelConstantBufferWrite(a, second, true), "descriptor heap change preserves root CBV");

  cache.Update(b, {5, 6, 7, 8}, true, true);  // Includes root-0 CBV variant.
  Check(cache.NeedsPixelConstantBufferWrite(b, second, true), "signature change invalidates pixel binding");
  cache.DidWritePixelConstantBuffer(second, true);
  Check(cache.NeedsPixelConstantBufferWrite(a, second, true), "signature ABA invalidates pixel binding");
  cache.DidWritePixelConstantBuffer(second, true);
  for (unsigned reason = 0; reason < 4; ++reason) {
    // Raw list, external same-signature pass, emulated pass, new submission.
    cache.InvalidateAll();
    Check(cache.NeedsPixelConstantBufferWrite(a, second, true), "full invalidation forces a pixel write");
    cache.DidWritePixelConstantBuffer(second, true);
  }

  Cache disabled;
  disabled.DidWritePixelConstantBuffer(first, false);
  Check(disabled.NeedsPixelConstantBufferWrite(a, first, false), "disabled mode always writes");
  disabled.DidWritePixelConstantBuffer(first, false);
  Check(disabled.NeedsPixelConstantBufferWrite(a, first, true), "enable after disabled write starts invalid");
  disabled.DidWritePixelConstantBuffer(first, true);
  Check(!disabled.NeedsPixelConstantBufferWrite(a, first, true), "enabled pixel hit");
  Check(disabled.NeedsPixelConstantBufferWrite(a, first, false), "disable clears existing pixel identity");
  Check(disabled.NeedsPixelConstantBufferWrite(a, first, true),
        "disabled query without publication still clears pixel identity");
  disabled.DidWritePixelConstantBuffer(first, true);
  Check(disabled.stats().pixel_rebinds == 4 && disabled.stats().pixel_hits == 1,
        "pixel stats count completed writes and omitted writes separately");
  Check(disabled.stats().rebinds == 0 && disabled.stats().hits == 0,
        "pixel statistics do not change static-root statistics");
}

void PixelRecordingOracle() {
  // Independent GPU command-state oracle. Decisions from the production helper
  // drive command writes; each completed draw must see exactly the requested
  // address. There is no byte-content model because live upload slices are
  // immutable and root 4 identifies them by address. The model makes unrelated
  // roots, signature switches, list resets and external writes explicit.
  Cache cache;
  uintptr_t gpu_signature = 0;
  uint64_t gpu_address = 0;
  bool gpu_valid = false;
  uint64_t recorded = 0, omitted = 0;
  uint32_t random = 0x31795BDA;
  auto next = [&]() {
    random ^= random << 13; random ^= random >> 17; return random ^= random << 5;
  };
  for (uint32_t event = 0; event < 20000; ++event) {
    const uint32_t operation = next() % 12;
    if (operation <= 1) {
      gpu_valid = false;  // Raw/emulated access or a new command list.
      if (!operation) gpu_signature = 0;
      cache.InvalidateAll();
      continue;
    }
    if (operation == 2) {
      cache.InvalidateDescriptorTables();  // Hardware root CBV stays valid.
      continue;
    }
    if (operation == 3) {
      gpu_address ^= 0x100;  // An intervening pass writes root 4, same signature.
      gpu_valid = true;
      cache.InvalidateAll();
      continue;
    }
    const uintptr_t signature = next() % 5 == 0 ? 1u + next() % 3
                                               : (gpu_signature ? gpu_signature : 1u);
    if (gpu_signature != signature) {
      gpu_signature = signature;
      gpu_valid = false;
    }
    const Values unrelated{next(), next(), next(), next()};
    cache.Update(signature, unrelated, (next() & 1) != 0, next() % 3 != 0);
    uint64_t wanted = gpu_address;
    if (!gpu_valid || next() % 3 == 0) {
      wanted = (uint64_t(next()) << 32) | (uint64_t(next()) & ~uint64_t(255));
    }
    const bool enabled = next() % 4 != 0;
    const bool write = cache.NeedsPixelConstantBufferWrite(signature, wanted, enabled);
    if (!enabled) Check(write, "disabled pixel path cannot omit a command");
    if (write && operation == 4) {
      // Failed/abandoned append: no publication or draw. The next actual draw
      // still has to observe the last completed GPU command state.
      Check(cache.NeedsPixelConstantBufferWrite(signature, wanted, enabled),
            "an unrecorded pixel command was treated as recorded");
      continue;
    }
    if (write) {
      gpu_address = wanted;
      gpu_valid = true;
      ++recorded;
      cache.DidWritePixelConstantBuffer(wanted, enabled);
    } else {
      ++omitted;
    }
    Check(gpu_valid && gpu_address == wanted, "draw sees stale pixel address or invalid root argument");
    Check(cache.stats().pixel_rebinds == recorded && cache.stats().pixel_hits == omitted,
          "pixel recording counters differ from independent command trace");
  }
  Check(recorded > 1000 && omitted > 1000, "random pixel control did not exercise writes and hits");
}
}  // namespace

int main() {
  LiteralTransitions();
  RecordingOracle();
  PixelLiteralTransitions();
  PixelRecordingOracle();
  std::printf("PASS: %u checks; static and pixel literal controls, "
              "20000 static + 20000 pixel command-state events\n", checks);
  return 0;
}
