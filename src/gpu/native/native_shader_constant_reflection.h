// Original conservative reflection proof shared by native passes and controls.
#pragma once

#include <cstdint>
#include <d3dcompiler.h>
#include <d3d12shader.h>
#include <wrl/client.h>

#include "native_constant_layout.h"

namespace nb::gpu {

// Inspect actual linked bytecode, not metadata promises or an unused declared
// array size. Both root CBVs are visible to both linked stages. header_bytes=0
// proves that no variable in the selected binding is used at all.
inline bool ShaderConstantHeaderOnly(ID3DBlob* blob, uint32_t target,
                                     uint32_t header_bytes) {
  if (!blob) return true;
  Microsoft::WRL::ComPtr<ID3D12ShaderReflection> reflection;
  if (FAILED(D3DReflect(blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&reflection))) ||
      !reflection) return false;
  D3D12_SHADER_DESC shader_desc{};
  if (FAILED(reflection->GetDesc(&shader_desc))) return false;
  bool found = false;
  for (UINT i = 0; i < shader_desc.BoundResources; ++i) {
    D3D12_SHADER_INPUT_BIND_DESC binding{};
    if (FAILED(reflection->GetResourceBindingDesc(i, &binding))) return false;
    if (binding.Type != D3D_SIT_CBUFFER) continue;
    const auto match = NativeConstantHeaderBindingMatch(
        binding.Space, binding.BindPoint, binding.BindCount, target);
    if (match == NativeConstantBindingMatch::kIgnored) continue;
    if (match == NativeConstantBindingMatch::kAmbiguous || found || !binding.Name) return false;
    found = true;
    auto* buffer = reflection->GetConstantBufferByName(binding.Name);
    if (!buffer) return false;
    D3D12_SHADER_BUFFER_DESC buffer_desc{};
    if (FAILED(buffer->GetDesc(&buffer_desc)) || buffer_desc.Type != D3D_CT_CBUFFER) return false;
    for (UINT j = 0; j < buffer_desc.Variables; ++j) {
      auto* variable = buffer->GetVariableByIndex(j);
      D3D12_SHADER_VARIABLE_DESC desc{};
      if (!variable || FAILED(variable->GetDesc(&desc)) ||
          !NativeConstantVariableFitsHeader((desc.uFlags & D3D_SVF_USED) != 0,
                                           desc.StartOffset, desc.Size, header_bytes)) return false;
    }
  }
  return true;
}

}  // namespace nb::gpu
