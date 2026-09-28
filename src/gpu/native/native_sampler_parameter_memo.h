// Original memo of the last sampler parameter evaluation of one shader stage in
// the vendored D3D12CommandProcessor::UpdateBindings. For every sampler binding
// of the stage's shader, UpdateBindings calls D3D12TextureCache::
// GetSamplerParameters and stores the result in current_samplers_<stage>_,
// marking the descriptors out of date when a value differs. That function reads
// only the binding (fixed per shader: the bindings are built once, at the first
// translation, from the ucode), the binding's texture fetch constant and the
// anisotropic_override cvar. So while the same shader is bound, no texture fetch
// constant has been written (the fetch generation is unchanged) and the cvar
// holds the same value as at the last evaluation, every call would return the
// value already stored and change nothing. The shader is matched by address and
// ucode hash, so an address reused by a different shader never matches.
// Command processor thread only.
#pragma once

#include <cstdint>

namespace nb::gpu {

class NativeSamplerParameterMemo {
 public:
  // constinit-capable, so a file-scope static needs no initialization guard.
  constexpr NativeSamplerParameterMemo() = default;

  bool Matches(const void* owner, const void* shader, uint64_t ucode_hash,
               uint64_t fetch_generation, int32_t anisotropic_override) const {
    return shader_ && owner == owner_ && shader == shader_ && ucode_hash == ucode_hash_ &&
           fetch_generation == fetch_generation_ &&
           anisotropic_override == anisotropic_override_;
  }

  // Call after every full evaluation of the stage, whatever the switch, with the
  // fetch generation and cvar value it read, so the memo never describes an
  // evaluation older than the stored parameters.
  void Publish(const void* owner, const void* shader, uint64_t ucode_hash,
               uint64_t fetch_generation, int32_t anisotropic_override) {
    owner_ = owner;
    shader_ = shader;
    ucode_hash_ = ucode_hash;
    fetch_generation_ = fetch_generation;
    anisotropic_override_ = anisotropic_override;
  }

  void Invalidate() { shader_ = nullptr; }

 private:
  const void* owner_ = nullptr;
  const void* shader_ = nullptr;
  uint64_t ucode_hash_ = 0;
  uint64_t fetch_generation_ = 0;
  int32_t anisotropic_override_ = 0;
};

}  // namespace nb::gpu
