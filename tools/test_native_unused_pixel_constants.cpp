// Original synthetic control of the ACTUAL native b2 reflection proof and
// immutable CBV aliasing. No game data. The caller owns the machine lock.
// Does not exercise NativeGeometryPass/CP orchestration or upload-pool reclaim.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <immintrin.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include "native_shader_constant_reflection.h"

using Microsoft::WRL::ComPtr;
namespace {
constexpr size_t kRootBytes = 208, kVsBytes = 4784, kPsBytes = 4288;
constexpr size_t kSlice = 12288, kRoot = 256, kVs = 768, kPs = 5888;
constexpr size_t kReadStride = 1024;
unsigned checks = 0;
void Require(bool value, const char* why) {
  ++checks; if (!value) throw std::runtime_error(why);
}
void Check(HRESULT hr, const char* why) {
  if (FAILED(hr)) {
    std::fprintf(stderr, "%s HRESULT=%08X\n", why, unsigned(hr));
    throw std::runtime_error(why);
  }
}
[[noreturn]] void Fatal() noexcept {
  std::fputs("FAIL: GPU completion unproven; retaining GPU-owned resources by process exit\n", stderr);
  TerminateProcess(GetCurrentProcess(), 70); std::_Exit(70);
}
struct CompletionGuard {
  bool pending = false;
  ~CompletionGuard() { if (pending) Fatal(); }
};
struct Event {
  HANDLE value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  Event() { if (!value) throw std::runtime_error("event creation"); }
  ~Event() { CloseHandle(value); }
};
ComPtr<ID3DBlob> Compile(const std::string& source, const char* target) {
  ComPtr<ID3DBlob> code, errors;
  const HRESULT hr = D3DCompile(source.data(), source.size(), "synthetic_unused_b2",
      nullptr, nullptr, "main", target,
      D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES | D3DCOMPILE_OPTIMIZATION_LEVEL3,
      0, &code, &errors);
  if (errors) std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
  Check(hr, "synthetic FXC compile"); return code;
}
// Mode0 omits b2; mode1 declares but never uses it; mode2 reads it in VS;
// mode3 reads it in PS. Both stages retain b0 and VS reads b1 in all modes.
std::string Shader(bool vertex, unsigned mode) {
  std::string source = "cbuffer Root:register(b0){uint4 root_data[13];};\n";
  if (vertex) source += "cbuffer Vertex:register(b1){uint4 vertex_data[299];};\n";
  if (mode) source += "cbuffer Pixel:register(b2){uint4 pixel_data[268];};\n";
  source += "struct V{float4 p:SV_Position;nointerpolation uint4 v:TEXCOORD0;};\n";
  if (vertex) {
    source += "V main(uint id:SV_VertexID){V o;float2 p=float2((id<<1)&2,id&2);"
              "o.p=float4(p*float2(2,-2)+float2(-1,1),0,1);"
              "o.v=root_data[0]^vertex_data[0]";
    if (mode == 2) source += "^pixel_data[0]";
    source += ";return o;}";
  } else {
    source += "struct O{uint4 color:SV_Target0;float depth:SV_Depth;};"
              "O main(V i){O o;o.color=i.v^root_data[1]";
    if (mode == 3) source += "^pixel_data[0]";
    source += ";o.depth=0.25;return o;}";
  }
  return source;
}
ComPtr<ID3D12Resource> Buffer(ID3D12Device* device, size_t bytes,
                            D3D12_HEAP_TYPE heap_type, D3D12_RESOURCE_STATES state) {
  D3D12_HEAP_PROPERTIES heap{}; heap.Type = heap_type;
  heap.CreationNodeMask = heap.VisibleNodeMask = 1;
  D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = bytes; desc.Height = 1; desc.DepthOrArraySize = desc.MipLevels = 1;
  desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> result;
  Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state,
      nullptr, IID_PPV_ARGS(&result)), "buffer creation"); return result;
}
ComPtr<ID3D12Resource> Target(ID3D12Device* device, bool depth) {
  D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap.CreationNodeMask = heap.VisibleNodeMask = 1;
  D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Format = depth ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_R32G32B32A32_UINT;
  desc.Flags = depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  ComPtr<ID3D12Resource> result;
  Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
      depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET,
      nullptr, IID_PPV_ARGS(&result)), "target creation"); return result;
}
void Upload(ID3D12Resource* buffer, const std::vector<uint8_t>& bytes) {
  void* mapping = nullptr; const D3D12_RANGE none{0, 0};
  Check(buffer->Map(0, &none, &mapping), "upload map");
  std::memcpy(mapping, bytes.data(), bytes.size()); _mm_sfence();
  const D3D12_RANGE written{0, bytes.size()}; buffer->Unmap(0, &written);
}
void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
             D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
  list->ResourceBarrier(1, &b);
}
void CopyTarget(ID3D12Device* device, ID3D12GraphicsCommandList* list,
                ID3D12Resource* target, ID3D12Resource* readback, size_t offset) {
  D3D12_TEXTURE_COPY_LOCATION from{}; from.pResource = target;
  from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION to{}; to.pResource = readback;
  to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  const auto desc = target->GetDesc();
  UINT rows = 0; UINT64 row_bytes = 0;
  device->GetCopyableFootprints(&desc, 0, 1, offset, &to.PlacedFootprint,
                               &rows, &row_bytes, nullptr);
  Require(to.PlacedFootprint.Offset == offset && rows == 1 &&
          row_bytes == (desc.Format == DXGI_FORMAT_D32_FLOAT ? 4u : 16u) &&
          to.PlacedFootprint.Footprint.Width == 1 && to.PlacedFootprint.Footprint.Height == 1 &&
          to.PlacedFootprint.Footprint.Depth == 1 && to.PlacedFootprint.Footprint.RowPitch == 256,
          "actual target copy footprint remains within fixture slot");
  list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
}
uint32_t Literal(unsigned fixture, unsigned bank, size_t word) {
  uint32_t value = (fixture + 1) * (0x9E3779B9u + bank * 0x85EBCA6Bu);
  value ^= uint32_t(word * 0xC2B2AE35u); value ^= value >> 16;
  value *= 0x7FEB352Du; return value ^ (value >> 15);
}
void Put(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = uint8_t(value >> (8 * i));
}
std::vector<uint8_t> Read(ID3D12Resource* buffer, size_t bytes) {
  std::vector<uint8_t> result(bytes);
  void* mapping = nullptr; const D3D12_RANGE range{0, bytes}, none{0, 0};
  Check(buffer->Map(0, &range, &mapping), "readback map");
  std::memcpy(result.data(), mapping, bytes); buffer->Unmap(0, &none); return result;
}
struct Fixture { unsigned shader; bool enabled, force; };

void Run(bool warp) {
  std::array<ComPtr<ID3DBlob>, 4> vs, ps;
  std::array<bool, 4> proven{};
  for (unsigned mode = 0; mode < 4; ++mode) {
    vs[mode] = Compile(Shader(true, mode), "vs_5_1");
    ps[mode] = Compile(Shader(false, mode), "ps_5_1");
    const bool vs_ok = nb::gpu::ShaderConstantHeaderOnly(vs[mode].Get(), 2, 0);
    const bool ps_ok = nb::gpu::ShaderConstantHeaderOnly(ps[mode].Get(), 2, 0);
    Require(vs_ok == (mode != 2) && ps_ok == (mode != 3), "actual per-stage zero-byte b2 reflection");
    proven[mode] = vs_ok && ps_ok;
  }
  ComPtr<ID3DBlob> malformed;
  Check(D3DCreateBlob(32, &malformed), "malformed reflection fixture allocation");
  std::memset(malformed->GetBufferPointer(), 0xA7, 32);
  Require(!nb::gpu::ShaderConstantHeaderOnly(malformed.Get(), 2, 0), "malformed blob cannot prove no reads");
  Require(nb::gpu::ShaderConstantHeaderOnly(nullptr, 2, 0), "absent linked stage is harmless");
  auto array = Compile("struct P{uint4 x;};ConstantBuffer<P> a[2]:register(b1);"
      "uint4 main(float4 p:SV_Position):SV_Target{return a[(uint)p.x&1u].x;}", "ps_5_1");
  Require(!nb::gpu::ShaderConstantHeaderOnly(array.Get(), 2, 0), "actual overlapping CBV-array binding refuses");
  using Match = nb::gpu::NativeConstantBindingMatch;
  Require(nb::gpu::NativeConstantHeaderBindingMatch(0, 0, 0, 2) == Match::kAmbiguous,
          "unbounded binding policy refuses");
  Require(nb::gpu::NativeConstantHeaderBindingMatch(0, 1, 2, 2) == Match::kAmbiguous,
          "overlapping binding policy refuses");
  Require(nb::gpu::NativeConstantHeaderBindingMatch(1, 2, 1, 2) == Match::kIgnored,
          "different space does not cover b2 space0");

  std::vector<Fixture> fixtures;
  for (unsigned frame = 0; frame < 2; ++frame) for (unsigned mode = 0; mode < 4; ++mode)
    for (bool enabled : {true, false, true, false}) fixtures.push_back({mode, enabled, false});
  fixtures.push_back({2, true, true}); fixtures.push_back({3, true, true});
  std::vector<uint8_t> packets(fixtures.size() * kSlice, 0xD7);
  constexpr std::array<std::array<size_t, 3>, 3> parts{{
      {kRoot, kRootBytes, 1}, {kVs, kVsBytes, 2}, {kPs, kPsBytes, 3}}};
  for (unsigned f = 0; f < fixtures.size(); ++f) {
    for (const auto& part : parts)
      for (size_t word = 0; word < part[1] / 4; ++word)
        Put(packets, f * kSlice + part[0] + word * 4, Literal(f, unsigned(part[2]), word));
  }
  ComPtr<ID3D12Debug> debug; const bool debug_ok = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
  if (debug_ok) debug->EnableDebugLayer();
  ComPtr<IDXGIFactory4> factory; Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
  ComPtr<ID3D12Device> device;
  if (warp) {
    ComPtr<IDXGIAdapter> adapter; Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "WARP");
    Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "WARP device");
  } else for (UINT i = 0; ; ++i) {
    ComPtr<IDXGIAdapter1> adapter; const HRESULT hr = factory->EnumAdapters1(i, &adapter);
    if (hr == DXGI_ERROR_NOT_FOUND) break;
    Check(hr, "adapter enumeration"); DXGI_ADAPTER_DESC1 desc{}; Check(adapter->GetDesc1(&desc), "adapter description");
    if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
    if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
      std::printf("adapter_vendor=%04X device=%04X\n", desc.VendorId, desc.DeviceId); break;
    }
  }
  Require(bool(device), "hardware device unavailable; explicit --warp only");
  std::printf("debug_enabled=%u; no debug-clean claim when unavailable\n", unsigned(debug_ok));
  ComPtr<ID3D12InfoQueue> info; if (debug_ok) device.As(&info);
  ComPtr<ID3D12CommandQueue> queue; D3D12_COMMAND_QUEUE_DESC q{};
  Check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "queue");
  ComPtr<ID3D12CommandAllocator> allocator;
  Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
  ComPtr<ID3D12GraphicsCommandList> list;
  Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "list");
  std::array<D3D12_ROOT_PARAMETER1, 3> roots{};
  for (unsigned i = 0; i < roots.size(); ++i) {
    roots[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; roots[i].Descriptor.ShaderRegister = i;
    roots[i].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;
    roots[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC rd{}; rd.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  rd.Desc_1_1.NumParameters = UINT(roots.size()); rd.Desc_1_1.pParameters = roots.data();
  ComPtr<ID3DBlob> serialized, errors;
  Check(D3D12SerializeVersionedRootSignature(&rd, &serialized, &errors), "root serialization");
  ComPtr<ID3D12RootSignature> root;
  Check(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root)), "root");
  std::array<ComPtr<ID3D12PipelineState>, 4> pipelines;
  for (unsigned mode = 0; mode < 4; ++mode) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{}; p.pRootSignature = root.Get();
    p.VS = {vs[mode]->GetBufferPointer(), vs[mode]->GetBufferSize()};
    p.PS = {ps[mode]->GetBufferPointer(), ps[mode]->GetBufferSize()};
    p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    p.RasterizerState.DepthClipEnable = TRUE;
    auto& blend = p.BlendState.RenderTarget[0]; blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD; blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    p.DepthStencilState.DepthEnable = TRUE; p.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    p.DepthStencilState.FrontFace = p.DepthStencilState.BackFace =
        {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    p.SampleMask = UINT_MAX; p.SampleDesc.Count = 1; p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    p.NumRenderTargets = 1; p.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_UINT; p.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    Check(device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&pipelines[mode])), "pipeline");
  }
  auto upload = Buffer(device.Get(), packets.size(), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  Upload(upload.Get(), packets);
  auto packet_readback = Buffer(device.Get(), packets.size(), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  std::vector<uint8_t> guard(fixtures.size() * kReadStride, 0xA7);
  auto guard_upload = Buffer(device.Get(), guard.size(), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  Upload(guard_upload.Get(), guard);
  auto output = Buffer(device.Get(), guard.size(), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  auto color = Target(device.Get(), false), depth = Target(device.Get(), true);
  ComPtr<ID3D12DescriptorHeap> rtvs, dsvs;
  D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
  Check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvs)), "RTV heap");
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; Check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsvs)), "DSV heap");
  const auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart(), dsv = dsvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(color.Get(), nullptr, rtv); device->CreateDepthStencilView(depth.Get(), nullptr, dsv);
  ComPtr<ID3D12Fence> fence; Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
  Event done;
  list->CopyBufferRegion(output.Get(), 0, guard_upload.Get(), 0, guard.size());
  list->SetGraphicsRootSignature(root.Get()); list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  D3D12_VIEWPORT viewport{0, 0, 1, 1, 0, 1}; D3D12_RECT scissor{0, 0, 1, 1};
  list->RSSetViewports(1, &viewport); list->RSSetScissorRects(1, &scissor);
  list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
  unsigned aliases = 0;
  for (unsigned f = 0; f < fixtures.size(); ++f) {
    const auto test = fixtures[f]; const bool alias = test.force || (test.enabled && proven[test.shader]);
    aliases += alias && !test.force;
    const auto base = upload->GetGPUVirtualAddress() + f * kSlice;
    list->SetPipelineState(pipelines[test.shader].Get());
    list->SetGraphicsRootConstantBufferView(0, base + kRoot);
    list->SetGraphicsRootConstantBufferView(1, base + kVs);
    list->SetGraphicsRootConstantBufferView(2, base + (alias ? kVs : kPs));
    list->DrawInstanced(3, 1, 0, 0);
    Barrier(list.Get(), color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(list.Get(), depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    CopyTarget(device.Get(), list.Get(), color.Get(), output.Get(), f * kReadStride);
    CopyTarget(device.Get(), list.Get(), depth.Get(), output.Get(), f * kReadStride + 512);
    Barrier(list.Get(), color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Barrier(list.Get(), depth.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
  }
  list->CopyBufferRegion(packet_readback.Get(), 0, upload.Get(), 0, packets.size());
  Check(list->Close(), "list close");
  CompletionGuard completion; completion.pending = true;
  ID3D12CommandList* lists[] = {list.Get()}; queue->ExecuteCommandLists(1, lists);
  if (FAILED(queue->Signal(fence.Get(), 1)) || FAILED(fence->SetEventOnCompletion(1, done.value)) ||
      WaitForSingleObject(done.value, 30000) != WAIT_OBJECT_0 || fence->GetCompletedValue() != 1 ||
      FAILED(device->GetDeviceRemovedReason())) Fatal();
  completion.pending = false;
  Require(Read(packet_readback.Get(), packets.size()) == packets, "all immutable packet bytes and guards remain exact");
  const auto actual = Read(output.Get(), guard.size());
  unsigned valid = 0, negatives = 0;
  for (unsigned f = 0; f < fixtures.size(); ++f) {
    const auto test = fixtures[f]; bool wrong_color = false;
    for (unsigned lane = 0; lane < 4; ++lane) {
      uint32_t expected = Literal(f, 1, lane) ^ Literal(f, 1, lane + 4) ^ Literal(f, 2, lane);
      if (test.shader >= 2) expected ^= Literal(f, 3, lane);
      uint32_t got = 0;
      for (unsigned b = 0; b < 4; ++b) got |= uint32_t(actual[f * kReadStride + lane * 4 + b]) << (8 * b);
      wrong_color |= got != expected;
      // Negative controls must match their separately predicted WRONG source,
      // too; arbitrary corruption cannot make a negative test pass.
      if (test.force) expected ^= Literal(f, 3, lane) ^ Literal(f, 2, lane);
      Put(guard, f * kReadStride + lane * 4, expected);
    }
    Put(guard, f * kReadStride + 512, 0x3E800000u); // exact0.25f depth
    Require(wrong_color == test.force, "literal color oracle and forced-alias negative discriminate");
    if (test.force) ++negatives; else ++valid;
  }
  Require(actual == guard, "all color/depth literal bytes and untouched output padding match");
  if (info) for (UINT64 i = 0; i < info->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
    SIZE_T size = 0; Check(info->GetMessage(i, nullptr, &size), "debug size");
    std::vector<uint8_t> bytes(size); auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
    Check(info->GetMessage(i, message, &size), "debug message");
    if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
      std::fprintf(stderr, "%s\n", message->pDescription); Require(false, "D3D12 debug error");
    }
  }
  std::printf("PASS valid=%u negative=%u admitted_aliases=%u reflection_modes=4 checks=%u literal_guard_bytes=%zu\n",
              valid, negatives, aliases, checks, packets.size() + guard.size());
}
}  // namespace
int main(int argc, char** argv) {
  try {
    Require(argc == 1 || (argc == 2 && std::string(argv[1]) == "--warp"), "usage: test_native_unused_pixel_constants [--warp]");
    Run(argc == 2); return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1;
  }
}
