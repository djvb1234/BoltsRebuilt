// Original CPU controls for the sampler parameter memo used by the vendored
// D3D12CommandProcessor::UpdateBindings. A model of one shader stage (texture
// fetch constants, shaders whose sampler bindings are fixed at creation, the
// anisotropic_override cvar, shader addresses reused after a cache clear) runs
// the original loop and the memoized loop in lockstep; the stored parameters and
// every "descriptors out of date" decision must agree exactly. The SDK
// integration (the generation bump sites) is reviewed separately.
#include "../src/gpu/native/native_sampler_parameter_memo.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <random>
#include <vector>

namespace {
constinit nb::gpu::NativeSamplerParameterMemo constant_initialized_memo;

uint64_t checks = 0;
void Check(bool condition, const char* why) {
  ++checks;
  if (!condition) {
    std::cerr << "FAIL: " << why << '\n';
    std::exit(1);
  }
}

struct Binding {
  uint32_t fetch_constant;
  uint32_t filter_override;  // 0 = use the fetch constant, as kUseFetchConst
};
struct Shader {
  uint64_t ucode_hash;
  std::vector<Binding> bindings;
};

struct World {
  uint32_t fetch[32] = {};
  int32_t aniso = 3;
  uint64_t generation = 1;
  void WriteFetch(uint32_t index, uint32_t value) {
    fetch[index] = value;
    ++generation;  // as WriteRegister / WriteRegistersFromMem
  }
};

// Pure in (binding, fetch constant, cvar), like GetSamplerParameters.
uint32_t Parameters(const World& world, const Binding& binding) {
  uint32_t value = world.fetch[binding.fetch_constant];
  uint32_t filter = binding.filter_override ? binding.filter_override : (value & 7);
  uint32_t aniso = (world.aniso > -1 && world.aniso < 6 && (value & 8)) ? uint32_t(world.aniso) : 0;
  return (value & ~uint32_t(7)) ^ (filter << 1) ^ (aniso << 20);
}

struct Stage {
  std::vector<uint32_t> current;
  bool out_of_date = false;
  nb::gpu::NativeSamplerParameterMemo memo;

  void Update(const World& world, const Shader* shader, bool memo_enabled) {
    out_of_date = false;
    if (shader->bindings.empty()) return;
    if (memo_enabled && memo.Matches(this, shader, shader->ucode_hash, world.generation, world.aniso)) {
      return;
    }
    if (current.size() < shader->bindings.size()) current.resize(shader->bindings.size());
    for (size_t i = 0; i < shader->bindings.size(); ++i) {
      uint32_t parameters = Parameters(world, shader->bindings[i]);
      if (current[i] != parameters) {
        out_of_date = true;
        current[i] = parameters;
      }
    }
    memo.Publish(this, shader, shader->ucode_hash, world.generation, world.aniso);
  }
};

void DirectControls() {
  nb::gpu::NativeSamplerParameterMemo memo;
  int owner = 0, a = 0, b = 0;
  Check(!memo.Matches(&owner, &a, 1, 1, 3), "empty memo matches");
  memo.Publish(&owner, &a, 1, 5, 3);
  Check(memo.Matches(&owner, &a, 1, 5, 3), "same evaluation does not match");
  Check(!memo.Matches(&owner, &b, 1, 5, 3), "other shader matches");
  Check(!memo.Matches(&owner, &a, 2, 5, 3), "reused address with other ucode matches");
  Check(!memo.Matches(&owner, &a, 1, 6, 3), "fetch write ignored");
  Check(!memo.Matches(&owner, &a, 1, 5, 4), "cvar change ignored");
  Check(!memo.Matches(&b, &a, 1, 5, 3), "other owner matches");
  memo.Invalidate();
  Check(!memo.Matches(&owner, &a, 1, 5, 3), "invalidated memo matches");
}

void Lockstep(uint32_t seed) {
  std::mt19937 rng(seed);
  World world;
  for (uint32_t& value : world.fetch) value = rng();
  // Shaders live in fixed slots, so a "cache clear" that recreates one reuses
  // its address with new bindings, as an allocator might.
  std::vector<std::unique_ptr<Shader>> shaders(6);
  auto make = [&](size_t slot) {
    auto shader = std::make_unique<Shader>();
    shader->ucode_hash = (uint64_t(rng()) << 32) | rng();
    size_t count = rng() % 5;
    for (size_t i = 0; i < count; ++i) {
      shader->bindings.push_back({uint32_t(rng() % 32), uint32_t(rng() % 3 == 0 ? 1 + rng() % 5 : 0)});
    }
    if (shaders[slot]) {
      *shaders[slot] = std::move(*shader);  // same address, different shader
    } else {
      shaders[slot] = std::move(shader);
    }
  };
  for (size_t i = 0; i < shaders.size(); ++i) make(i);

  Stage plain, memoized;
  uint64_t hits = 0;
  for (int step = 0; step < 20000; ++step) {
    uint32_t action = rng() % 100;
    if (action < 20) {
      // Rewrites often store the same value: still a write, still a bump.
      uint32_t index = rng() % 32;
      world.WriteFetch(index, rng() % 2 ? world.fetch[index] : uint32_t(rng()));
    } else if (action < 22) {
      world.aniso = int32_t(rng() % 8) - 1;
    } else if (action < 23) {
      make(rng() % shaders.size());
      ++world.generation;  // ClearCaches bumps the generation
      memoized.memo.Invalidate();
    } else if (action < 24) {
      // Address reuse without the clear's bump, guarded by the ucode hash only.
      make(rng() % shaders.size());
    }
    const Shader* shader = shaders[rng() % 3 ? rng() % 2 : rng() % shaders.size()].get();
    bool memo_enabled = rng() % 16 != 0;  // hot reload flips it now and then
    plain.Update(world, shader, false);
    hits += (memo_enabled && !shader->bindings.empty() &&
             memoized.memo.Matches(&memoized, shader, shader->ucode_hash, world.generation,
                                   world.aniso))
                ? 1
                : 0;
    memoized.Update(world, shader, memo_enabled);
    Check(plain.out_of_date == memoized.out_of_date, "out-of-date decision differs");
    for (size_t i = 0; i < shader->bindings.size(); ++i) {
      Check(plain.current[i] == memoized.current[i], "stored parameters differ");
    }
  }
  Check(hits > 1000, "memo never hits");
}
}  // namespace

int main() {
  (void)constant_initialized_memo;
  DirectControls();
  for (uint32_t seed = 1; seed <= 50; ++seed) Lockstep(seed);
  std::cout << "native sampler parameter memo: " << checks << " checks passed\n";
  return 0;
}
