// nb - a native fullscreen pass: our own HLSL pixel shader drawn with D3D12 into the render targets
// the guest draw had bound. This is the smallest unit of the engine-level renderer (docs/plan.md,
// "Route H"): the pass writes straight into the host render target the emulated backend owns for the
// current EDRAM binding, so every later emulated draw composites and depth-tests against it unchanged.
//
// Usage from NbCommandProcessor::TryNativeDraw: Record() binds our root signature and a pipeline
// state matching the bound target formats and sample count, sets the root constants (and, for a
// textured pass, the SDK's bindless view heap so the shader can index the guest textures the way
// the SDK's own translated shaders do), and records a three-vertex fullscreen triangle into the
// deferred command list through the command processor's external-pipeline helpers, which keep its
// cached state consistent.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include <d3d12.h>
#include <wrl/client.h>

#include <rex/graphics/d3d12/command_processor.h>

namespace nb::gpu {

class NativeFullscreenPass {
 public:
  // Root constants, visible to the pixel shader as `cbuffer NbPass : register(b0)` (32 dwords).
  static constexpr uint32_t kConstantDwords = 32;

  struct Options {
    // Adds root parameter 1: an unbounded Texture2DArray SRV range in register space 1 bound to the
    // SDK's bindless view heap (shader index = bindless SRV index - kUnboundedSRVsStart), plus a
    // static linear-clamp sampler at s0.
    bool guest_textures = false;
  };

  // Compiles the shaders (vs_5_1 / ps_5_1 through D3DCompile) and creates the root signature. `ps_hlsl`
  // must define `float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target0`.
  bool Initialize(ID3D12Device* device, const char* name, const std::string& ps_hlsl, Options options);
  bool initialized() const { return initialized_; }

  // Records the pass for the targets described by `context`; returns false (nothing recorded) when the
  // pipeline state for those formats cannot be created. `bindless_heap` is required when the pass was
  // created with guest_textures.
  bool Record(rex::graphics::d3d12::D3D12CommandProcessor& cp,
              const rex::graphics::d3d12::D3D12CommandProcessor::NativeDrawContext& context,
              const uint32_t constants[kConstantDwords],
              const D3D12_GPU_DESCRIPTOR_HANDLE* bindless_heap = nullptr);

 private:
  ID3D12PipelineState* GetPipeline(
      const rex::graphics::d3d12::D3D12CommandProcessor::NativeDrawContext& context);

  std::string name_;
  Options options_{};
  bool initialized_ = false;
  ID3D12Device* device_ = nullptr;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_;
  Microsoft::WRL::ComPtr<ID3DBlob> vs_;
  Microsoft::WRL::ComPtr<ID3DBlob> ps_;
  std::unordered_map<uint64_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>> pipelines_;
  uint32_t failures_ = 0;
};

}  // namespace nb::gpu
