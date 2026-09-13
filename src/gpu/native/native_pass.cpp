// See native_pass.h.

#include "native_pass.h"

#include <d3dcompiler.h>

#include <cstring>

#include <rex/logging.h>

namespace nb::gpu {

namespace {

// A fullscreen triangle from SV_VertexID; uv (0,0) is the top-left of the render target.
constexpr const char kFullscreenVs[] = R"hlsl(
struct VsOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VsOut main(uint id : SV_VertexID) {
  VsOut o;
  float2 uv = float2((id << 1) & 2, id & 2);
  o.uv = uv;
  o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  return o;
}
)hlsl";

Microsoft::WRL::ComPtr<ID3DBlob> Compile(const char* name, const char* source, size_t length,
                                         const char* target) {
  Microsoft::WRL::ComPtr<ID3DBlob> code;
  Microsoft::WRL::ComPtr<ID3DBlob> errors;
  const HRESULT hr = D3DCompile(source, length, name, nullptr, nullptr, "main", target,
                                D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_WARNINGS_ARE_ERRORS |
                                    D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES,
                                0, &code,
                                &errors);
  if (FAILED(hr)) {
    REXLOG_ERROR("rexgpu-nb: native pass '{}': {} compile failed (0x{:08X}): {}", name, target,
                 static_cast<uint32_t>(hr),
                 errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize())
                        : std::string("no diagnostics"));
    return nullptr;
  }
  return code;
}

uint64_t PipelineKey(const rex::graphics::d3d12::D3D12CommandProcessor::NativeDrawContext& context) {
  uint64_t key = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    key |= (static_cast<uint64_t>(context.rtv_formats[i]) & 0xFFu) << (8 * i);
  }
  key |= (static_cast<uint64_t>(context.dsv_format) & 0xFFu) << 32;
  key |= (static_cast<uint64_t>(context.sample_count) & 0xFu) << 40;
  key |= (context.sample_mask == UINT_MAX ? 0ull : 1ull) << 44;
  return key;
}

}  // namespace

bool NativeFullscreenPass::Initialize(ID3D12Device* device, const char* name, const std::string& ps_hlsl,
                                      Options options) {
  name_ = name;
  options_ = options;
  device_ = device;
  vs_ = Compile(name, kFullscreenVs, sizeof(kFullscreenVs) - 1, "vs_5_1");
  ps_ = Compile(name, ps_hlsl.c_str(), ps_hlsl.size(), "ps_5_1");
  if (!vs_ || !ps_) {
    return false;
  }

  D3D12_ROOT_PARAMETER1 parameters[2] = {};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants.ShaderRegister = 0;
  parameters[0].Constants.RegisterSpace = 0;
  parameters[0].Constants.Num32BitValues = kConstantDwords;
  parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  // Guest textures: the same shape as the SDK's own bindless root signature (one unbounded 2D-array
  // SRV range starting at kUnboundedSRVsStart inside the view heap, descriptors written while the list
  // is recorded, hence VOLATILE), so a shader index is `bindless SRV index - kUnboundedSRVsStart`.
  D3D12_DESCRIPTOR_RANGE1 texture_range = {};
  texture_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  texture_range.NumDescriptors = UINT_MAX;
  texture_range.BaseShaderRegister = 0;
  texture_range.RegisterSpace = 1;
  texture_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
  texture_range.OffsetInDescriptorsFromTableStart =
      UINT(rex::graphics::d3d12::D3D12CommandProcessor::SystemBindlessView::kUnboundedSRVsStart);
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[1].DescriptorTable.NumDescriptorRanges = 1;
  parameters[1].DescriptorTable.pDescriptorRanges = &texture_range;
  parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_STATIC_SAMPLER_DESC sampler = {};
  sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
  sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  sampler.ShaderRegister = 0;
  sampler.RegisterSpace = 0;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {};
  desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  desc.Desc_1_1.NumParameters = options.guest_textures ? 2 : 1;
  desc.Desc_1_1.pParameters = parameters;
  desc.Desc_1_1.NumStaticSamplers = options.guest_textures ? 1 : 0;
  desc.Desc_1_1.pStaticSamplers = &sampler;
  desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
  Microsoft::WRL::ComPtr<ID3DBlob> blob;
  Microsoft::WRL::ComPtr<ID3DBlob> errors;
  if (FAILED(D3D12SerializeVersionedRootSignature(&desc, &blob, &errors))) {
    REXLOG_ERROR("rexgpu-nb: native pass '{}': root signature serialization failed: {}", name,
                 errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no diagnostics");
    return false;
  }
  if (FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                         IID_PPV_ARGS(&root_signature_)))) {
    REXLOG_ERROR("rexgpu-nb: native pass '{}': CreateRootSignature failed", name);
    return false;
  }
  initialized_ = true;
  REXLOG_INFO("rexgpu-nb: native pass '{}' ready (vs {} B, ps {} B)", name, vs_->GetBufferSize(),
              ps_->GetBufferSize());
  return true;
}

ID3D12PipelineState* NativeFullscreenPass::GetPipeline(
    const rex::graphics::d3d12::D3D12CommandProcessor::NativeDrawContext& context) {
  const uint64_t key = PipelineKey(context);
  auto it = pipelines_.find(key);
  if (it != pipelines_.end()) {
    return it->second.Get();
  }

  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
  desc.pRootSignature = root_signature_.Get();
  desc.VS = {vs_->GetBufferPointer(), vs_->GetBufferSize()};
  desc.PS = {ps_->GetBufferPointer(), ps_->GetBufferSize()};
  desc.BlendState.AlphaToCoverageEnable = FALSE;
  desc.BlendState.IndependentBlendEnable = FALSE;
  for (auto& rt : desc.BlendState.RenderTarget) {
    rt.BlendEnable = FALSE;
    rt.LogicOpEnable = FALSE;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  }
  desc.SampleMask = context.sample_mask;
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.RasterizerState.DepthClipEnable = TRUE;
  // The pass writes colour only; the guest's depth target stays bound but untouched.
  desc.DepthStencilState.DepthEnable = FALSE;
  desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
  desc.DepthStencilState.StencilEnable = FALSE;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    desc.RTVFormats[i] = context.rtv_formats[i];
    if (context.rtv_formats[i] != DXGI_FORMAT_UNKNOWN) {
      desc.NumRenderTargets = i + 1;
    }
  }
  desc.DSVFormat = context.dsv_format;
  desc.SampleDesc.Count = context.sample_count;
  desc.SampleDesc.Quality = 0;

  Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
  if (FAILED(device_->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)))) {
    if (failures_++ < 8) {
      REXLOG_ERROR("rexgpu-nb: native pass '{}': pipeline creation failed for rtv [{},{},{},{}] dsv {} samples {}",
                   name_, int(context.rtv_formats[0]), int(context.rtv_formats[1]), int(context.rtv_formats[2]),
                   int(context.rtv_formats[3]), int(context.dsv_format), context.sample_count);
    }
    pipelines_.emplace(key, nullptr);
    return nullptr;
  }
  REXLOG_INFO("rexgpu-nb: native pass '{}': pipeline for rtv [{},{},{},{}] dsv {} samples {}", name_,
              int(context.rtv_formats[0]), int(context.rtv_formats[1]), int(context.rtv_formats[2]),
              int(context.rtv_formats[3]), int(context.dsv_format), context.sample_count);
  ID3D12PipelineState* result = pipeline.Get();
  pipelines_.emplace(key, std::move(pipeline));
  return result;
}

bool NativeFullscreenPass::Record(rex::graphics::d3d12::D3D12CommandProcessor& cp,
                                  const rex::graphics::d3d12::D3D12CommandProcessor::NativeDrawContext& context,
                                  const uint32_t constants[kConstantDwords],
                                  const D3D12_GPU_DESCRIPTOR_HANDLE* bindless_heap) {
  if (!initialized_ || (options_.guest_textures && !bindless_heap)) {
    return false;
  }
  ID3D12PipelineState* pipeline = GetPipeline(context);
  if (!pipeline) {
    return false;
  }
  cp.SetExternalGraphicsRootSignature(root_signature_.Get());
  cp.SetExternalPipeline(pipeline);
  cp.GetDeferredCommandList().D3DSetGraphicsRoot32BitConstants(0, kConstantDwords, constants, 0);
  if (options_.guest_textures) {
    cp.GetDeferredCommandList().D3DSetGraphicsRootDescriptorTable(1, *bindless_heap);
  }
  cp.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  cp.SubmitBarriers();
  cp.GetDeferredCommandList().D3DDrawInstanced(3, 1, 0, 0);
  return true;
}

}  // namespace nb::gpu
