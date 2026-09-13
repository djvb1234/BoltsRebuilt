// nb - interchangeable bindings for the unchanged native b0 payload.
// Root CBV data lives in an immutable, frame-owned upload allocation until
// GPU completion. The register, space and stage visibility match root constants.

#pragma once

#include <cstdint>
#include <d3d12.h>

namespace nb::gpu {

inline D3D12_ROOT_PARAMETER1 NativeRootParameter(bool cbv, uint32_t dwords = 52) {
  D3D12_ROOT_PARAMETER1 parameter{};
  parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  if (cbv) {
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister = 0;
    parameter.Descriptor.RegisterSpace = 0;
    parameter.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;
  } else {
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameter.Constants.ShaderRegister = 0;
    parameter.Constants.RegisterSpace = 0;
    parameter.Constants.Num32BitValues = dwords;
  }
  return parameter;
}

}  // namespace nb::gpu
