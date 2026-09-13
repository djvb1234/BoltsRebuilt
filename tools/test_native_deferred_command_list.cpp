// Original CPU-only controls for the ACTUAL vendored deferred writer/replayer.
// Compile with NB_NATIVE_DEFERRED_REPLAY_TEST and the isolated stub include
// directory first. No D3D12 device, GPU work, game process or SDK runtime link.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include <rex/graphics/flags.h>
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/deferred_command_list.h>
#include <rex/graphics/d3d12/native_static_bindings.h>
#include <rex/graphics/phase3_counters.h>

namespace rex::graphics::d3d12 {
// Only this isolated fixture may damage owned headers or logical extents.
struct NativeDeferredReplayTestAccess {
  enum class Fault { kInvalidId, kTruncatedSuffix, kOverlongPayload, kWrongHandleSize, kExtentBeyondStorage };
  static size_t End(const DeferredCommandList& list) { return list.command_stream_used_; }
  static std::vector<uintmax_t> Tail(const DeferredCommandList& list, size_t offset) {
    return {list.command_stream_.begin() + offset, list.command_stream_.end()};
  }
  // Slice only known-valid fixture records at complete command boundaries.
  static std::vector<size_t> Boundaries(const DeferredCommandList& list) {
    std::vector<size_t> result{0};
    while (result.back() != list.command_stream_used_) {
      const auto& header = *reinterpret_cast<const DeferredCommandList::CommandHeader*>(
          list.command_stream_.data() + result.back());
      result.push_back(result.back() + DeferredCommandList::kCommandHeaderSizeElements +
                       header.arguments_size_elements);
    }
    return result;
  }
  static void CopySpan(DeferredCommandList& dst, const DeferredCommandList& src,
                       size_t first, size_t last) {
    dst.command_stream_.assign(src.command_stream_.begin() + first, src.command_stream_.begin() + last);
    dst.command_stream_used_ = last - first;
    dst.pipeline_handles_resolved_for_replay_ = src.pipeline_handles_resolved_for_replay_;
  }
  static void Damage(DeferredCommandList& list, size_t offset, Fault fault) {
    auto& header = *reinterpret_cast<DeferredCommandList::CommandHeader*>(list.command_stream_.data() + offset);
    switch (fault) {
      case Fault::kInvalidId: header.command = static_cast<DeferredCommandList::Command>(-1); break;
      case Fault::kTruncatedSuffix:
        // The current header is one whole storage element, so a nonempty
        // partial header cannot be represented. Truncate its pointer payload.
        static_assert(DeferredCommandList::kCommandHeaderSizeElements == 1);
        list.command_stream_used_ = offset + DeferredCommandList::kCommandHeaderSizeElements;
        break;
      case Fault::kOverlongPayload: header.arguments_size_elements = UINT32_MAX; break;
      case Fault::kWrongHandleSize: header.arguments_size_elements = 0; break;
      case Fault::kExtentBeyondStorage: list.command_stream_used_ = list.command_stream_.size() + 1; break;
    }
  }
};
}  // namespace rex::graphics::d3d12

using rex::graphics::d3d12::DeferredCommandList;
using rex::graphics::d3d12::D3D12CommandProcessor;
using rex::graphics::d3d12::GetNativeDeferredStorageStats;
namespace {
size_t checks = 0;
void Check(bool value, const char* reason) {
  ++checks;
  if (!value) throw std::runtime_error(reason);
}
template <class T> T* Opaque(uintptr_t id) { return reinterpret_cast<T*>(id); }
struct Event {
  std::string name;
  std::vector<uint64_t> values;
  bool operator==(const Event&) const = default;
};
// A real COM-interface implementation, with independent semantic argument
// capture. Unused vtable entry points throw rather than silently accepting an
// accidental replay change. Fake resource handles are opaque and never read.
class Recorder : public ID3D12GraphicsCommandList1 {
 public:
  virtual ~Recorder() = default;
  std::vector<Event> events;
  void Begin(const char* name) { events.push_back({name, {}}); }
  template <typename T> void Add(T value) {
    if constexpr (std::is_pointer_v<T>) events.back().values.push_back(reinterpret_cast<uintptr_t>(value));
    else if constexpr (std::is_same_v<T, float>) events.back().values.push_back(std::bit_cast<uint32_t>(value));
    else events.back().values.push_back(static_cast<uint64_t>(value));
  }
  template <typename... T> void Emit(const char* name, T... values) { Begin(name); (Add(values), ...); }
  void Bytes(const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) Add(bytes[i]);
  }
  void Rect(const D3D12_RECT& r) { Add(r.left); Add(r.top); Add(r.right); Add(r.bottom); }
  void Rects(UINT count, const D3D12_RECT* rects) {
    Add(count); Add(rects != nullptr);
    for (UINT i = 0; i < count; ++i) Rect(rects[i]);
  }
  void Location(const D3D12_TEXTURE_COPY_LOCATION& p) {
    Add(p.pResource); Add(p.Type);
    if (p.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) Add(p.SubresourceIndex);
    else {
      Add(p.PlacedFootprint.Offset); Add(p.PlacedFootprint.Footprint.Format);
      Add(p.PlacedFootprint.Footprint.Width); Add(p.PlacedFootprint.Footprint.Height);
      Add(p.PlacedFootprint.Footprint.Depth); Add(p.PlacedFootprint.Footprint.RowPitch);
    }
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override { if (out) *out = nullptr; return E_NOINTERFACE; }
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void** out) override { if (out) *out = nullptr; return E_NOINTERFACE; }
  D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() override { return D3D12_COMMAND_LIST_TYPE_DIRECT; }
  HRESULT STDMETHODCALLTYPE Close() override { throw std::runtime_error("unexpected Close"); }
  HRESULT STDMETHODCALLTYPE Reset(ID3D12CommandAllocator*, ID3D12PipelineState*) override { throw std::runtime_error("unexpected Reset"); }
  void STDMETHODCALLTYPE ClearState(ID3D12PipelineState*) override { throw std::runtime_error("unexpected ClearState"); }
  void STDMETHODCALLTYPE DrawInstanced(UINT a, UINT b, UINT c, UINT d) override { Emit("draw", a,b,c,d); }
  void STDMETHODCALLTYPE DrawIndexedInstanced(UINT a, UINT b, UINT c, INT d, UINT e) override { Emit("draw_indexed", a,b,c,d,e); }
  void STDMETHODCALLTYPE Dispatch(UINT a, UINT b, UINT c) override { Emit("dispatch", a,b,c); }
  void STDMETHODCALLTYPE CopyBufferRegion(ID3D12Resource* d, UINT64 a, ID3D12Resource* s, UINT64 b, UINT64 n) override { Emit("copy_buffer", d,a,s,b,n); }
  void STDMETHODCALLTYPE CopyResource(ID3D12Resource* d, ID3D12Resource* s) override { Emit("copy_resource",d,s); }
  void STDMETHODCALLTYPE CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* d, UINT x, UINT y, UINT z,
      const D3D12_TEXTURE_COPY_LOCATION* s, const D3D12_BOX* box) override {
    Emit("copy_texture",x,y,z); Location(*d); Location(*s); Add(box != nullptr);
    if (box) { Add(box->left); Add(box->top); Add(box->front); Add(box->right); Add(box->bottom); Add(box->back); }
  }
  void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY p) override { Emit("topology",p); }
  void STDMETHODCALLTYPE RSSetViewports(UINT n, const D3D12_VIEWPORT* p) override {
    Emit("viewport",n); for (UINT i=0;i<n;++i) { Add(p[i].TopLeftX); Add(p[i].TopLeftY); Add(p[i].Width); Add(p[i].Height); Add(p[i].MinDepth); Add(p[i].MaxDepth); }
  }
  void STDMETHODCALLTYPE RSSetScissorRects(UINT n, const D3D12_RECT* p) override { Begin("scissor"); Rects(n,p); }
  void STDMETHODCALLTYPE OMSetBlendFactor(const FLOAT p[4]) override { Begin("blend"); for (unsigned i=0;i<4;++i) Add(p[i]); }
  void STDMETHODCALLTYPE OMSetStencilRef(UINT n) override { Emit("stencil",n); }
  void STDMETHODCALLTYPE SetPipelineState(ID3D12PipelineState* p) override { Emit("pipeline",p); }
  void STDMETHODCALLTYPE ResourceBarrier(UINT n, const D3D12_RESOURCE_BARRIER* p) override {
    Emit("barrier",n);
    for (UINT i=0;i<n;++i) {
      Add(p[i].Type); Add(p[i].Flags);
      if (p[i].Type==D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) { Add(p[i].Transition.pResource); Add(p[i].Transition.Subresource); Add(p[i].Transition.StateBefore); Add(p[i].Transition.StateAfter); }
      else if (p[i].Type==D3D12_RESOURCE_BARRIER_TYPE_ALIASING) { Add(p[i].Aliasing.pResourceBefore); Add(p[i].Aliasing.pResourceAfter); }
      else if (p[i].Type==D3D12_RESOURCE_BARRIER_TYPE_UAV) Add(p[i].UAV.pResource);
      else throw std::runtime_error("invalid barrier type");
    }
  }
  void STDMETHODCALLTYPE SetDescriptorHeaps(UINT n, ID3D12DescriptorHeap* const* p) override { Emit("heaps",n); for(UINT i=0;i<n;++i) Add(p[i]); }
  void STDMETHODCALLTYPE SetComputeRootSignature(ID3D12RootSignature* p) override { Emit("compute_signature",p); }
  void STDMETHODCALLTYPE SetGraphicsRootSignature(ID3D12RootSignature* p) override { Emit("graphics_signature",p); }
  void STDMETHODCALLTYPE SetComputeRootDescriptorTable(UINT i, D3D12_GPU_DESCRIPTOR_HANDLE p) override { Emit("compute_table",i,p.ptr); }
  void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(UINT i, D3D12_GPU_DESCRIPTOR_HANDLE p) override { Emit("graphics_table",i,p.ptr); }
  void STDMETHODCALLTYPE SetComputeRoot32BitConstants(UINT i,UINT n,const void* p,UINT offset) override { Emit("compute_constants",i,n,offset); Bytes(p,size_t(n)*4); }
  void STDMETHODCALLTYPE SetGraphicsRoot32BitConstants(UINT i,UINT n,const void* p,UINT offset) override { Emit("graphics_constants",i,n,offset); Bytes(p,size_t(n)*4); }
  void STDMETHODCALLTYPE SetComputeRootConstantBufferView(UINT i,D3D12_GPU_VIRTUAL_ADDRESS p) override { Emit("compute_cbv",i,p); }
  void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(UINT i,D3D12_GPU_VIRTUAL_ADDRESS p) override { Emit("graphics_cbv",i,p); }
  void STDMETHODCALLTYPE SetComputeRootShaderResourceView(UINT i,D3D12_GPU_VIRTUAL_ADDRESS p) override { Emit("compute_srv",i,p); }
  void STDMETHODCALLTYPE SetGraphicsRootShaderResourceView(UINT i,D3D12_GPU_VIRTUAL_ADDRESS p) override { Emit("graphics_srv",i,p); }
  void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(UINT i,D3D12_GPU_VIRTUAL_ADDRESS p) override { Emit("compute_uav",i,p); }
  void STDMETHODCALLTYPE SetGraphicsRootUnorderedAccessView(UINT i,D3D12_GPU_VIRTUAL_ADDRESS p) override { Emit("graphics_uav",i,p); }
  void STDMETHODCALLTYPE IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* p) override {
    Emit("index",p != nullptr); if(p) { Add(p->BufferLocation); Add(p->SizeInBytes); Add(p->Format); }
  }
  void STDMETHODCALLTYPE IASetVertexBuffers(UINT start,UINT n,const D3D12_VERTEX_BUFFER_VIEW* p) override {
    Emit("vertices",start,n); for(UINT i=0;i<n;++i) { Add(p[i].BufferLocation); Add(p[i].SizeInBytes); Add(p[i].StrideInBytes); }
  }
  void STDMETHODCALLTYPE OMSetRenderTargets(UINT n,const D3D12_CPU_DESCRIPTOR_HANDLE* p,BOOL single,const D3D12_CPU_DESCRIPTOR_HANDLE* depth) override {
    Emit("targets",n,single,depth != nullptr); for(UINT i=0;i<(n?(single?1:n):0);++i) Add(p[i].ptr); if(depth) Add(depth->ptr);
  }
  void STDMETHODCALLTYPE ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE h,D3D12_CLEAR_FLAGS f,FLOAT d,UINT8 s,UINT n,const D3D12_RECT* p) override {
    Emit("clear_depth",h.ptr,f,d,s); Rects(n,p);
  }
  void STDMETHODCALLTYPE ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE h,const FLOAT color[4],UINT n,const D3D12_RECT* p) override {
    Emit("clear_color",h.ptr); for(unsigned i=0;i<4;++i) Add(color[i]); Rects(n,p);
  }
  void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE g,D3D12_CPU_DESCRIPTOR_HANDLE c,ID3D12Resource* r,const UINT values[4],UINT n,const D3D12_RECT* p) override {
    Emit("clear_uav",g.ptr,c.ptr,r); for(unsigned i=0;i<4;++i) Add(values[i]); Rects(n,p);
  }
  void STDMETHODCALLTYPE BeginQuery(ID3D12QueryHeap* h,D3D12_QUERY_TYPE t,UINT i) override { Emit("begin_query",h,t,i); }
  void STDMETHODCALLTYPE EndQuery(ID3D12QueryHeap* h,D3D12_QUERY_TYPE t,UINT i) override { Emit("end_query",h,t,i); }
  void STDMETHODCALLTYPE ResolveQueryData(ID3D12QueryHeap* h,D3D12_QUERY_TYPE t,UINT i,UINT n,ID3D12Resource* r,UINT64 offset) override { Emit("resolve_query",h,t,i,n,r,offset); }
  void STDMETHODCALLTYPE SetMarker(UINT metadata,const void* p,UINT n) override { Emit("marker",metadata,n); Bytes(p,n); }
  void STDMETHODCALLTYPE BeginEvent(UINT metadata,const void* p,UINT n) override { Emit("begin_event",metadata,n); Bytes(p,n); }
  void STDMETHODCALLTYPE EndEvent() override { Begin("end_event"); }
  void STDMETHODCALLTYPE SetSamplePositions(UINT samples,UINT pixels,D3D12_SAMPLE_POSITION* p) override {
    Emit("sample_positions",samples,pixels,p!=nullptr); for(UINT i=0;i<samples*pixels;++i) { Add(p[i].X); Add(p[i].Y); }
  }
#define UNUSED_BODY throw std::runtime_error("unexpected unused COM method")
  void STDMETHODCALLTYPE CopyTiles(ID3D12Resource*,const D3D12_TILED_RESOURCE_COORDINATE*,const D3D12_TILE_REGION_SIZE*,ID3D12Resource*,UINT64,D3D12_TILE_COPY_FLAGS) override { UNUSED_BODY; }
  void STDMETHODCALLTYPE ResolveSubresource(ID3D12Resource*,UINT,ID3D12Resource*,UINT,DXGI_FORMAT) override { UNUSED_BODY; }
  void STDMETHODCALLTYPE ExecuteBundle(ID3D12GraphicsCommandList*) override { UNUSED_BODY; }
  void STDMETHODCALLTYPE SetComputeRoot32BitConstant(UINT,UINT,UINT) override { UNUSED_BODY; }
  void STDMETHODCALLTYPE SetGraphicsRoot32BitConstant(UINT,UINT,UINT) override { UNUSED_BODY; }
  void STDMETHODCALLTYPE SOSetTargets(UINT,UINT,const D3D12_STREAM_OUTPUT_BUFFER_VIEW*) override { UNUSED_BODY; }
  void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE,D3D12_CPU_DESCRIPTOR_HANDLE,ID3D12Resource*,const FLOAT[4],UINT,const D3D12_RECT*) override { UNUSED_BODY; }
  void STDMETHODCALLTYPE DiscardResource(ID3D12Resource*,const D3D12_DISCARD_REGION*) override { UNUSED_BODY; }
  void STDMETHODCALLTYPE SetPredication(ID3D12Resource*,UINT64,D3D12_PREDICATION_OP) override { UNUSED_BODY; }
  void STDMETHODCALLTYPE ExecuteIndirect(ID3D12CommandSignature*,UINT,ID3D12Resource*,UINT64,ID3D12Resource*,UINT64) override { UNUSED_BODY; }
  void STDMETHODCALLTYPE AtomicCopyBufferUINT(ID3D12Resource*,UINT64,ID3D12Resource*,UINT64,UINT,ID3D12Resource* const*,const D3D12_SUBRESOURCE_RANGE_UINT64*) override { UNUSED_BODY; }
  void STDMETHODCALLTYPE AtomicCopyBufferUINT64(ID3D12Resource*,UINT64,ID3D12Resource*,UINT64,UINT,ID3D12Resource* const*,const D3D12_SUBRESOURCE_RANGE_UINT64*) override { UNUSED_BODY; }
  void STDMETHODCALLTYPE OMSetDepthBounds(FLOAT,FLOAT) override { UNUSED_BODY; }
  void STDMETHODCALLTYPE ResolveSubresourceRegion(ID3D12Resource*,UINT,UINT,UINT,ID3D12Resource*,UINT,D3D12_RECT*,DXGI_FORMAT,D3D12_RESOLVE_MODE) override { UNUSED_BODY; }
  void STDMETHODCALLTYPE SetViewInstanceMask(UINT) override { UNUSED_BODY; }
#undef UNUSED_BODY
};

void Equal(const Recorder& actual, const Recorder& expected, const char* label) {
  if (actual.events != expected.events) {
    size_t i=0;
    while (i<actual.events.size() && i<expected.events.size() && actual.events[i]==expected.events[i]) ++i;
    std::cerr << label << " first mismatch=" << i << " actual_events=" << actual.events.size()
              << " expected_events=" << expected.events.size() << '\n';
    throw std::runtime_error("semantic replay mismatch");
  }
  ++checks;
}

// Reference calls go directly to an independent semantic recorder, before
// source arrays are poisoned. They do not interpret the deferred wire format.
void Scenario(DeferredCommandList& out, Recorder& expected, D3D12CommandProcessor& cp,
              uint32_t seed, size_t count, bool list1) {
  auto* r0=Opaque<ID3D12Resource>(0x1000), *r1=Opaque<ID3D12Resource>(0x2000);
  auto* p0=Opaque<ID3D12PipelineState>(0x3000), *p1=Opaque<ID3D12PipelineState>(0x4000);
  auto* root=Opaque<ID3D12RootSignature>(0x5000);
  auto* q=Opaque<ID3D12QueryHeap>(0x6000);
  auto* h0=Opaque<ID3D12DescriptorHeap>(0x7000), *h1=Opaque<ID3D12DescriptorHeap>(0x8000);
  D3D12_RECT rects[2]={{LONG(seed),-2,61,37},{7,8,90,101}};
  FLOAT color[4]={-0.0f,std::bit_cast<float>(0x7FC12345u),0.25f,1.0f};
  UINT values[4]={seed,0xFFFFFFFFu,0xA5A55A5Au,0};
  D3D12_CPU_DESCRIPTOR_HANDLE handles[3]={{0x12340},{0x22340},{0x32340}};
  const D3D12_GPU_DESCRIPTOR_HANDLE gpu{0x123456789000ull};
  const D3D12_CLEAR_FLAGS clear=D3D12_CLEAR_FLAG_DEPTH|D3D12_CLEAR_FLAG_STENCIL;
  out.D3DClearDepthStencilView(handles[0],clear,0.75f,0xEF,2,rects);
  expected.ClearDepthStencilView(handles[0],clear,0.75f,0xEF,2,rects);
  auto* allocated=out.ClearDepthStencilViewAllocatedRects(handles[1],D3D12_CLEAR_FLAG_STENCIL,0.0f,0,2);
  allocated[0]=rects[1]; allocated[1]=rects[0];
  const D3D12_RECT reversed[2]={rects[1],rects[0]};
  expected.ClearDepthStencilView(handles[1],D3D12_CLEAR_FLAG_STENCIL,0.0f,0,2,reversed);
  out.D3DClearDepthStencilView(handles[0],clear,0.0f,3,0,nullptr);
  expected.ClearDepthStencilView(handles[0],clear,0.0f,3,0,nullptr);
  out.D3DClearRenderTargetView(handles[1],color,2,rects); expected.ClearRenderTargetView(handles[1],color,2,rects);
  out.D3DClearRenderTargetView(handles[1],color,0,nullptr); expected.ClearRenderTargetView(handles[1],color,0,nullptr);
  out.D3DClearUnorderedAccessViewUint(gpu,handles[2],r0,values,2,rects); expected.ClearUnorderedAccessViewUint(gpu,handles[2],r0,values,2,rects);
  out.D3DClearUnorderedAccessViewUint(gpu,handles[2],r0,values,0,nullptr); expected.ClearUnorderedAccessViewUint(gpu,handles[2],r0,values,0,nullptr);
  out.D3DCopyBufferRegion(r0,0x123456789ull,r1,2048,4096); expected.CopyBufferRegion(r0,0x123456789ull,r1,2048,4096);
  out.D3DCopyResource(r0,r1); expected.CopyResource(r0,r1);
  D3D12_TEXTURE_COPY_LOCATION dst{},src{};
  dst.pResource=r0; dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex=7;
  src.pResource=r1; src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src.PlacedFootprint.Offset=512; src.PlacedFootprint.Footprint={DXGI_FORMAT_BC3_UNORM,17,9,1,256};
  D3D12_BOX box{1,2,0,13,8,1};
  out.CopyTexture(dst,src); expected.CopyTextureRegion(&dst,0,0,0,&src,nullptr);
  out.D3DCopyTextureRegion(&dst,3,4,5,&src,&box); expected.CopyTextureRegion(&dst,3,4,5,&src,&box);
  out.D3DCopyTextureRegion(&dst,0,0,0,&src,nullptr); expected.CopyTextureRegion(&dst,0,0,0,&src,nullptr);
  out.D3DBeginQuery(q,D3D12_QUERY_TYPE_TIMESTAMP,9); expected.BeginQuery(q,D3D12_QUERY_TYPE_TIMESTAMP,9);
  out.D3DEndQuery(q,D3D12_QUERY_TYPE_TIMESTAMP,10); expected.EndQuery(q,D3D12_QUERY_TYPE_TIMESTAMP,10);
  out.D3DResolveQueryData(q,D3D12_QUERY_TYPE_TIMESTAMP,9,2,r0,0x1100); expected.ResolveQueryData(q,D3D12_QUERY_TYPE_TIMESTAMP,9,2,r0,0x1100);
  D3D12_INDEX_BUFFER_VIEW ib{0x13579000,82,DXGI_FORMAT_R16_UINT};
  out.D3DIASetIndexBuffer(&ib); expected.IASetIndexBuffer(&ib);
  out.D3DIASetIndexBuffer(nullptr); expected.IASetIndexBuffer(nullptr);
  out.D3DIASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP); expected.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  D3D12_VERTEX_BUFFER_VIEW vb[2]={{0x100000,135,17},{0x400000,91,13}};
  out.D3DIASetVertexBuffers(4,2,vb); expected.IASetVertexBuffers(4,2,vb);
  out.D3DIASetVertexBuffers(0,0,nullptr);  // no command
  out.D3DOMSetBlendFactor(color); expected.OMSetBlendFactor(color);
  out.D3DOMSetRenderTargets(3,handles,FALSE,&handles[2]); expected.OMSetRenderTargets(3,handles,FALSE,&handles[2]);
  out.D3DOMSetRenderTargets(3,handles,TRUE,nullptr); expected.OMSetRenderTargets(3,handles,TRUE,nullptr);
  out.D3DOMSetRenderTargets(0,nullptr,FALSE,nullptr); expected.OMSetRenderTargets(0,nullptr,FALSE,nullptr);
  out.D3DOMSetStencilRef(0x19); expected.OMSetStencilRef(0x19);
  D3D12_RESOURCE_BARRIER barriers[3]{};
  barriers[0].Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barriers[0].Transition={r0,7,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
  barriers[1].Type=D3D12_RESOURCE_BARRIER_TYPE_ALIASING; barriers[1].Aliasing={r0,r1};
  barriers[2].Type=D3D12_RESOURCE_BARRIER_TYPE_UAV; barriers[2].UAV.pResource=nullptr;
  out.D3DResourceBarrier(3,barriers); expected.ResourceBarrier(3,barriers);
  out.D3DResourceBarrier(0,nullptr);
  out.RSSetScissorRect(rects[1]); expected.RSSetScissorRects(1,&rects[1]);
  D3D12_VIEWPORT viewport{-0.0f,4.5f,17.0f,19.0f,0.125f,0.875f};
  out.RSSetViewport(viewport); expected.RSSetViewports(1,&viewport);
  out.D3DSetComputeRootSignature(root); expected.SetComputeRootSignature(root);
  out.D3DSetGraphicsRootSignature(nullptr); expected.SetGraphicsRootSignature(nullptr);
  out.D3DSetComputeRootDescriptorTable(1,gpu); expected.SetComputeRootDescriptorTable(1,gpu);
  out.D3DSetGraphicsRootDescriptorTable(5,gpu); expected.SetGraphicsRootDescriptorTable(5,gpu);
  out.D3DSetComputeRootConstantBufferView(3,0xABCDEF00); expected.SetComputeRootConstantBufferView(3,0xABCDEF00);
  out.D3DSetGraphicsRootConstantBufferView(4,0x12340000); expected.SetGraphicsRootConstantBufferView(4,0x12340000);
  out.D3DSetComputeRootShaderResourceView(6,0x98760000); expected.SetComputeRootShaderResourceView(6,0x98760000);
  out.D3DSetGraphicsRootShaderResourceView(7,0xFEDC0000); expected.SetGraphicsRootShaderResourceView(7,0xFEDC0000);
  out.D3DSetComputeRootUnorderedAccessView(8,0x1234560000); expected.SetComputeRootUnorderedAccessView(8,0x1234560000);
  out.D3DSetGraphicsRootUnorderedAccessView(9,0x9876540000); expected.SetGraphicsRootUnorderedAccessView(9,0x9876540000);
  out.D3DSetComputeRoot32BitConstants(0,0,nullptr,0); out.D3DSetGraphicsRoot32BitConstants(0,0,nullptr,0);
  for(size_t i=0;i<count;++i) {
    UINT constants[5]={UINT(i)^seed,0x80000000u,0x7FC00001u,0xFFFFFFFFu,UINT(i*73)};
    out.D3DSetGraphicsRoot32BitConstants(0,5,constants,7); expected.SetGraphicsRoot32BitConstants(0,5,constants,7);
    out.D3DSetComputeRoot32BitConstants(1,3,constants+1,2); expected.SetComputeRoot32BitConstants(1,3,constants+1,2);
    std::memset(constants,0xD3,sizeof(constants));
  }
  ID3D12DescriptorHeap* heaps[2]={h0,h1};
  out.SetDescriptorHeaps(h0,h1); expected.SetDescriptorHeaps(2,heaps);
  out.SetDescriptorHeaps(nullptr,h1); expected.SetDescriptorHeaps(1,heaps+1);
  out.SetDescriptorHeaps(nullptr,nullptr); expected.SetDescriptorHeaps(0,nullptr);
  // Initially no pipeline: all dispatch/draw variants must be skipped.
  out.D3DDispatch(1,1,1); out.D3DDrawInstanced(3,1,0,0); out.D3DDrawIndexedInstanced(3,1,0,0,0);
  out.D3DSetPipelineState(p0); expected.SetPipelineState(p0);
  out.D3DDispatch(2,3,4); expected.Dispatch(2,3,4);
  out.D3DDrawInstanced(51,2,7,3); expected.DrawInstanced(51,2,7,3);
  out.D3DDrawIndexedInstanced(91,3,5,-17,11); expected.DrawIndexedInstanced(91,3,5,-17,11);
  out.D3DSetPipelineState(nullptr); out.D3DDispatch(8,8,8); // null suppresses work
  void* good=Opaque<void>(0x9000), *missing=Opaque<void>(0xA000);
  cp.pipelines[good]=p0;
  out.SetPipelineStateHandle(good);
  cp.pipelines[good]=p1; // pointer must resolve at replay time, not record time
  expected.SetPipelineState(p1);
  out.D3DDrawInstanced(6,1,2,0); expected.DrawInstanced(6,1,2,0);
  out.SetPipelineStateHandle(missing); out.D3DDrawInstanced(9,1,0,0);
  out.D3DSetPipelineState(p0); expected.SetPipelineState(p0);
  out.D3DDrawIndexedInstanced(3,1,0,0,0); expected.DrawIndexedInstanced(3,1,0,0,0);
  D3D12_SAMPLE_POSITION positions[4]={{-7,7},{5,-5},{-3,3},{1,-1}};
  out.D3DSetSamplePositions(2,2,positions);
  if(list1) expected.SetSamplePositions(2,2,positions);
  out.D3DSetSamplePositions(0,0,positions);
  if(list1) expected.SetSamplePositions(0,0,nullptr);
  char label[]="synthetic\xE2\x98\x83";
  out.BeginDebugMarker(label); expected.BeginEvent(1,label,UINT(sizeof(label)));
  out.InsertDebugMarker(label); expected.SetMarker(1,label,UINT(sizeof(label)));
  out.EndDebugMarker(); expected.EndEvent();
  std::memset(rects,0xD3,sizeof(rects)); std::memset(color,0xD3,sizeof(color));
  std::memset(values,0xD3,sizeof(values)); std::memset(handles,0xD3,sizeof(handles));
  std::memset(&dst,0xD3,sizeof(dst)); std::memset(&src,0xD3,sizeof(src)); std::memset(&box,0xD3,sizeof(box));
  std::memset(&ib,0xD3,sizeof(ib)); std::memset(vb,0xD3,sizeof(vb)); std::memset(barriers,0xD3,sizeof(barriers));
  std::memset(&viewport,0xD3,sizeof(viewport)); std::memset(positions,0xD3,sizeof(positions)); std::memset(label,0xD3,sizeof(label));
}

void Replay(DeferredCommandList& stream, const Recorder& expected, bool list1, const char* label) {
  Recorder actual;
  stream.Execute(&actual,list1?&actual:nullptr);
  Equal(actual,expected,label);
}

void CompoundConstantBufferControls() {
  D3D12CommandProcessor cp;
  DeferredCommandList separate(cp,8), compound(cp,8);
  // Mock-only opaque addresses deliberately retain every bit, including zero,
  // high-bit differences and values that would not be GPU allocations.
  constexpr std::array<uint64_t,8> addresses = {
      0, 256, 0x100000100ull, 0x8000000000000100ull,
      0xFFFFFFFFFFFFFF00ull, UINT64_MAX, 0x123456789ABCDE00ull, 0xFEDCBA9876543200ull};
  for (bool retained : {false,true}) {
    for (size_t count : {size_t(257),size_t(1),size_t(0),size_t(65),size_t(2)}) {
      nb_native_deferred_storage=false; separate.Reset();
      nb_native_deferred_storage=retained; compound.Reset();
      Recorder expected;
      // A skipped pixel write must preserve a preceding pixel root binding.
      separate.D3DSetGraphicsRootConstantBufferView(4,0x1122334455667700ull);
      compound.D3DSetGraphicsRootConstantBufferView(4,0x1122334455667700ull);
      expected.SetGraphicsRootConstantBufferView(4,0x1122334455667700ull);
      for (size_t i=0;i<count;++i) {
        std::array<uint64_t,3> source={addresses[i%8],addresses[(i+3)%8],addresses[(i+5)%8]};
        bool write_pixel=(i%3)!=0;
        const UINT marker=UINT(i)^0xA5730000u;
        separate.D3DOMSetStencilRef(marker); compound.D3DOMSetStencilRef(marker);
        expected.OMSetStencilRef(marker);
        separate.D3DSetGraphicsRootConstantBufferView(0,source[0]);
        separate.D3DSetGraphicsRootConstantBufferView(1,source[1]);
        if (write_pixel) separate.D3DSetGraphicsRootConstantBufferView(4,source[2]);
        compound.D3DSetNativeGraphicsConstantBufferViews(source[0],source[1],source[2],write_pixel);
        expected.SetGraphicsRootConstantBufferView(0,source[0]);
        expected.SetGraphicsRootConstantBufferView(1,source[1]);
        if (write_pixel) expected.SetGraphicsRootConstantBufferView(4,source[2]);
        // No packet may borrow caller addresses or the selection flag. Mutate
        // them immediately and force subsequent writes to grow/reuse storage.
        source.fill(0xD3D3D3D3D3D3D3D3ull); write_pixel=!write_pixel;
        // Mid-submission mode changes must not alter either stream's snapshot.
        nb_native_deferred_storage=!retained;
        const UINT poison_marker=UINT(source[0])^UINT(write_pixel);
        separate.D3DOMSetStencilRef(poison_marker); compound.D3DOMSetStencilRef(poison_marker);
        expected.OMSetStencilRef(0xD3D3D3D3u^UINT((i%3)==0));
      }
      Replay(separate,expected,false,"separate CBVs match independent API trace");
      Replay(compound,expected,false,"compound CBVs preserve full API order/pixel skip");
      Replay(compound,expected,true,"compound CBVs unchanged with list1");
      Replay(compound,expected,false,"compound repeated replay owns immutable values");
      DeferredCommandList copy(compound);
      nb_native_deferred_storage=retained; compound.Reset();
      Replay(compound,Recorder{},false,"compound reset suppresses old logical tail");
      Replay(copy,expected,false,"compound copy survives source reset");
      DeferredCommandList moved(std::move(copy));
      Replay(moved,expected,false,"compound move preserves addresses and extent");
      Replay(copy,Recorder{},false,"compound moved-from stream is empty");
      // Reuse a moved-from list after the retained-storage policy changes.
      nb_native_deferred_storage=!retained; copy.Reset();
      copy.D3DSetNativeGraphicsConstantBufferViews(0,0,0,true);
      Recorder zero; zero.SetGraphicsRootConstantBufferView(0,0);
      zero.SetGraphicsRootConstantBufferView(1,0); zero.SetGraphicsRootConstantBufferView(4,0);
      Replay(copy,zero,false,"requested null pixel view is written, not skipped");
    }
  }
  // Independent literal and negative traces catch swapped root slots and an
  // unwanted pixel write even when the total high-water storage is unchanged.
  nb_native_deferred_storage=true; compound.Reset();
  compound.D3DSetNativeGraphicsConstantBufferViews(0x100000100ull,0x200000200ull,UINT64_MAX,false);
  Recorder actual; compound.Execute(&actual,nullptr);
  const std::vector<Event> literal={
      {"graphics_cbv",{0,0x100000100ull}}, {"graphics_cbv",{1,0x200000200ull}}};
  Check(actual.events==literal,"literal compound root0/root1 trace with pixel omitted");
  auto wrong=literal; std::swap(wrong[0],wrong[1]);
  Check(actual.events!=wrong,"oracle rejects reversed native CBV call order");
  wrong=literal; wrong.push_back({"graphics_cbv",{4,UINT64_MAX}});
  Check(actual.events!=wrong,"oracle rejects unrequested pixel write");
  wrong=literal; wrong[0].values[1]^=uint64_t(1)<<32;
  Check(actual.events!=wrong,"oracle rejects truncated or changed high address bits");
}

void CompoundPixelMemoControls() {
  struct Step {uintptr_t signature;uint64_t pixel;bool enabled,invalidate,write;};
  constexpr std::array<Step,9> steps = {{
      {0x1000,0x100000100ull,true,false,true},
      {0x1000,0x100000100ull,true,false,false},
      {0x1000,0x200000100ull,true,false,true}, // Same low bits, new allocation.
      {0x1000,0x200000100ull,false,false,true},
      {0x1000,0x200000100ull,true,false,true}, // Disabled publication is invalid.
      {0x1000,0x200000100ull,true,false,false},
      {0x2000,0x200000100ull,true,false,true}, // New root signature.
      {0x2000,0x200000100ull,true,true,true},
      {0x2000,0x200000100ull,true,false,false}}};
  D3D12CommandProcessor cp;
  DeferredCommandList stream(cp,8);
  nb_native_deferred_storage=true;stream.Reset();
  rex::graphics::d3d12::NativeStaticBindings memo;
  Recorder expected;
  for (size_t i=0;i<steps.size();++i) {
    const auto& step=steps[i];if(step.invalidate)memo.InvalidateAll();
    const bool write=memo.NeedsPixelConstantBufferWrite(step.signature,step.pixel,step.enabled);
    Check(write==step.write,"actual pixel memo matches literal compound write policy");
    const uint64_t root0=0xAA00000000ull+i*256,vertex=0xBB00000000ull+i*256;
    stream.D3DOMSetStencilRef(UINT(i));expected.OMSetStencilRef(UINT(i));
    stream.D3DSetNativeGraphicsConstantBufferViews(root0,vertex,step.pixel,write);
    if(write)memo.DidWritePixelConstantBuffer(step.pixel,step.enabled);
    expected.SetGraphicsRootConstantBufferView(0,root0);
    expected.SetGraphicsRootConstantBufferView(1,vertex);
    if(step.write)expected.SetGraphicsRootConstantBufferView(4,step.pixel);
    stream.D3DIASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    expected.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  }
  Replay(stream,expected,false,"actual pixel memo drives compound CBVs before topology");
  Check(memo.stats().pixel_hits==3 && memo.stats().pixel_rebinds==6,
      "literal pixel memo hit/publication totals");
}

// Benchmark sink only. Semantic controls above still capture the full real API
// trace. The timed override has no vectors, heap allocations or D3D12 driver.
class CbvCounter final : public Recorder {
 public:
  uint64_t calls=0;
  uint64_t checksum=0x91D372E4A8650BCFull;
  void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(
      UINT root, D3D12_GPU_VIRTUAL_ADDRESS address) override {
    ++calls;
    checksum=std::rotl(checksum,7)^address^(uint64_t(root)*0x9E3779B97F4A7C15ull);
  }
};

void CompoundConstantBufferBenchmark() {
  constexpr size_t count=65536;
  struct Input {std::array<uint64_t,3> addresses;bool pixel;};
  std::vector<Input> inputs(count);
  D3D12CommandProcessor cp;
  // Reserve outside timing. Warmups construct the high-water storage before
  // measured intervals; only the current enabled retained-storage path is timed.
  DeferredCommandList separate(cp,count*72),compound(cp,count*72);
  constexpr std::array<bool,10> order={false,true,false,true,true,false,false,true,true,false};
  for (bool mixed_pixel : {false,true}) {
    for (size_t i=0;i<count;++i) {
      inputs[i].addresses={0x100000000ull+uint64_t(i)*256,
          0xA700000000ull+uint64_t(i)*512,0x712300000000ull+uint64_t(i)*768};
      inputs[i].pixel=!mixed_pixel || (i&1)!=0;
    }
    // Independent API-order checksum is computed before any timed recording.
    uint64_t expected_calls=0,expected_checksum=0x91D372E4A8650BCFull;
    for (const auto& input : inputs) {
      for (unsigned lane=0;lane<(input.pixel?3u:2u);++lane) {
        const unsigned root=std::array<unsigned,3>{0,1,4}[lane];
        expected_checksum=std::rotl(expected_checksum,7)^input.addresses[lane]^
            (uint64_t(root)*0x9E3779B97F4A7C15ull);
        ++expected_calls;
      }
    }
    std::array<std::vector<double>,2> record_samples,replay_samples;
    for (size_t batch=0;batch<order.size();++batch) {
      const bool fused=order[batch]; auto& stream=fused?compound:separate;
      nb_native_deferred_storage=true; stream.Reset();
      const auto before=GetNativeDeferredStorageStats();
      const auto record_start=std::chrono::steady_clock::now();
      if (fused) {
        for (const auto& input : inputs)
          stream.D3DSetNativeGraphicsConstantBufferViews(input.addresses[0],
              input.addresses[1],input.addresses[2],input.pixel);
      } else {
        for (const auto& input : inputs) {
          stream.D3DSetGraphicsRootConstantBufferView(0,input.addresses[0]);
          stream.D3DSetGraphicsRootConstantBufferView(1,input.addresses[1]);
          if (input.pixel) stream.D3DSetGraphicsRootConstantBufferView(4,input.addresses[2]);
        }
      }
      const auto record_end=std::chrono::steady_clock::now();
      const auto after=GetNativeDeferredStorageStats();
      CbvCounter sink;
      const auto replay_start=std::chrono::steady_clock::now();
      stream.Execute(&sink,nullptr);
      const auto replay_end=std::chrono::steady_clock::now();
      const uint64_t command_bytes=(after.initialized_bytes-before.initialized_bytes)+
          (after.reused_bytes-before.reused_bytes);
      Check(sink.calls==expected_calls && sink.checksum==expected_checksum,
          "benchmark actual API count/order/address checksum");
      Check(sink.events.empty(),"timed CBV sink captured no allocated event trace");
      Check(after.commands-before.commands==(fused?count:expected_calls),
          "benchmark actual packet count");
      Check(command_bytes==(fused?count*40:expected_calls*24),
          "benchmark measured actual stream bytes");
      const double record_ns=std::chrono::duration<double,std::nano>(record_end-record_start).count()/count;
      const double replay_ns=std::chrono::duration<double,std::nano>(replay_end-replay_start).count()/count;
      if (batch>=2) {record_samples[unsigned(fused)].push_back(record_ns);replay_samples[unsigned(fused)].push_back(replay_ns);}
      std::cout<<"CBV_PACKET_BENCH mixed_pixel="<<mixed_pixel<<" batch="<<batch
          <<" warmup="<<(batch<2)<<" compound="<<fused<<" packets="<<after.commands-before.commands
          <<" stream_bytes="<<command_bytes<<" actual_API_calls="<<sink.calls<<" checksum="<<sink.checksum
          <<" record_ns_per_native_draw="<<record_ns<<" replay_ns_per_native_draw="<<replay_ns<<"\n";
    }
    for (unsigned fused=0;fused<2;++fused) {
      auto& r=record_samples[fused];auto& e=replay_samples[fused];
      std::sort(r.begin(),r.end());std::sort(e.begin(),e.end());
      std::cout<<"CBV_PACKET_MEDIAN mixed_pixel="<<mixed_pixel<<" compound="<<fused
          <<" record_ns="<<(r[1]+r[2])*0.5<<" replay_ns="<<(e[1]+e[2])*0.5
          <<" record_min="<<r.front()<<" record_max="<<r.back()
          <<" replay_min="<<e.front()<<" replay_max="<<e.back()<<" measured_batches="<<r.size()<<"\n";
    }
  }
  std::cout<<"PASS: actual writer/replay benchmark uses retained storage and no-allocation COM sink; "
      <<"includes command-storage diagnostics; excludes real driver/GPU/Record work; no FPS prediction\n";
}

void MalformedPreflightControls() {
  using Access = rex::graphics::d3d12::NativeDeferredReplayTestAccess;
  using Fault = Access::Fault;
  auto& phase = rex::graphics::GetPhase3Counters();
  for (bool retained : {false, true}) {
    for (Fault fault : {Fault::kInvalidId, Fault::kTruncatedSuffix, Fault::kOverlongPayload,
                       Fault::kWrongHandleSize, Fault::kExtentBeyondStorage}) {
      D3D12CommandProcessor cp;
      DeferredCommandList damaged(cp, 8);
      nb_native_deferred_storage = retained; damaged.Reset();
      cp.pipelines[Opaque<void>(0xA000)] = Opaque<ID3D12PipelineState>(0x3000);
      damaged.SetPipelineStateHandle(Opaque<void>(0xA000));
      damaged.SetPipelineStateHandle(Opaque<void>(0xB000));  // Valid null-resolving prefix.
      const size_t suffix = Access::End(damaged);
      damaged.SetPipelineStateHandle(Opaque<void>(0xA000));
      Access::Damage(damaged, suffix, fault);
      const auto malformed_tail = Access::Tail(damaged, suffix);
      const auto nulls_before = phase.execute_time_null_handle_count;
      for (unsigned attempt = 0; attempt != 2; ++attempt) {
        Check(!damaged.ResolvePipelineHandlesForReplay(), "malformed suffix rejects preflight");
        Check(!damaged.pipeline_handles_resolved_for_replay(), "malformed stream never publishes replay marker");
        Check(Access::Tail(damaged, suffix) == malformed_tail, "malformed command is never resolved before validation");
        Check(phase.execute_time_null_handle_count <= nulls_before + 1,
              "resolved prefix may persist but retry never recounts its null");
      }
      // Intentionally never Execute this stream, including when a check fails.
      // Earlier prefix resolutions need no rollback under the fatal contract.
    }
  }
}

void PreflightAndSwapControls() {
  auto& phase = rex::graphics::GetPhase3Counters();
  for (bool retained : {false,true}) for (bool list1 : {false,true}) {
    D3D12CommandProcessor cp;
    DeferredCommandList original(cp,8), scratch(cp,8);
    nb_native_deferred_storage=retained; original.Reset();
    Recorder expected;
    Scenario(original,expected,cp,0x123,17,list1);
    Check(!original.pipeline_handles_resolved_for_replay(),"recorded handles require CP preflight");
    const auto initial_nulls=phase.execute_time_null_handle_count;
    Replay(original,expected,list1,"original handles vs full independent API trace");
    Check(phase.execute_time_null_handle_count==initial_nulls+1,"original missing handle counted once");
    DeferredCommandList resolved(original);
    Check(resolved.ResolvePipelineHandlesForReplay(),"full mixed stream preflight succeeds");
    Check(resolved.pipeline_handles_resolved_for_replay(),"preflight marker published");
    Check(phase.execute_time_null_handle_count==initial_nulls+2,"preflight counts same null on CP");
    Check(resolved.ResolvePipelineHandlesForReplay(),"repeated preflight is safe");
    Check(phase.execute_time_null_handle_count==initial_nulls+2,"repeated preflight does not recount null");
    // Poison both lookups after preflight: an accidental worker-side lookup
    // loses the formerly valid draw and revives the formerly suppressed one.
    cp.pipelines[Opaque<void>(0x9000)]=nullptr;
    cp.pipelines[Opaque<void>(0xA000)]=Opaque<ID3D12PipelineState>(0xDEAD0);
    Replay(resolved,expected,list1,"preflight snapshots exact ordered pipeline/API semantics");
    Check(phase.execute_time_null_handle_count==initial_nulls+2,"resolved replay never recounts handles");
    DeferredCommandList copy(resolved);
    Check(copy.pipeline_handles_resolved_for_replay(),"copy preserves resolved identity");
    DeferredCommandList moved(std::move(copy));
    Check(moved.pipeline_handles_resolved_for_replay() && !copy.pipeline_handles_resolved_for_replay(),
          "move transfers resolved marker and clears source");
    Replay(copy,Recorder{},list1,"moved source has no stale commands");
    nb_native_deferred_storage=!retained; scratch.Reset();
    scratch.D3DOMSetStencilRef(0xFEED);
    Recorder short_trace; short_trace.OMSetStencilRef(0xFEED);
    moved.SwapStorage(scratch);
    Check(scratch.pipeline_handles_resolved_for_replay() && !moved.pipeline_handles_resolved_for_replay(),
          "swap transfers marker with bytes and logical extent");
    Replay(scratch,expected,list1,"swapped long stream owns exact payload");
    Replay(moved,short_trace,list1,"swapped scratch owns only former short prefix");
    const Recorder original_expected=expected;
    const auto writes_before=GetNativeDeferredStorageStats().commands;
    scratch.D3DOMSetStencilRef(0xBEEF); expected.OMSetStencilRef(0xBEEF);
    Check(GetNativeDeferredStorageStats().commands==writes_before+(retained?1u:0u),
          "swap transfers latched storage policy rather than current flag");
    Replay(scratch,expected,list1,"appending direct command preserves prior resolved payload");
    Replay(resolved,original_expected,list1,"copy remains independent of appended swapped stream");
    // Reset the returned scratch while the other stream retains all snapshots.
    moved.Reset(); Check(!moved.pipeline_handles_resolved_for_replay(),"Reset invalidates preflight marker");
    Replay(moved,Recorder{},list1,"Reset after swap drops logical prefix");
    moved.D3DOMSetStencilRef(0xCAFE);
    Check(moved.ResolvePipelineHandlesForReplay(),"direct-only rerecord preflight");
    cp.pipelines.clear();
    moved.SetPipelineStateHandle(Opaque<void>(0xA000));
    Check(!moved.pipeline_handles_resolved_for_replay(),"new handle invalidates previous preflight");
    moved.D3DDispatch(7,8,9); moved.D3DDrawInstanced(3,1,0,0);
    moved.D3DDrawIndexedInstanced(3,1,0,0,0);
    auto* native=Opaque<ID3D12PipelineState>(0x3000);
    moved.D3DSetPipelineState(native); moved.D3DDispatch(1,2,3);
    moved.D3DSetPipelineState(nullptr); moved.D3DDrawInstanced(9,1,0,0);
    Recorder literal; literal.OMSetStencilRef(0xCAFE); literal.SetPipelineState(native); literal.Dispatch(1,2,3);
    const auto nulls_before=phase.execute_time_null_handle_count;
    Check(moved.ResolvePipelineHandlesForReplay(),"null-handle/direct PSO mixed preflight");
    Check(phase.execute_time_null_handle_count==nulls_before+1,"mixed null preflight counts once");
    cp.pipelines[Opaque<void>(0xA000)]=native;
    Replay(moved,literal,list1,"null handle suppresses all work until next valid direct PSO");
    Check(phase.execute_time_null_handle_count==nulls_before+1,"mixed resolved replay has no CP counter writes");
    Recorder wrong=literal; wrong.DrawInstanced(9,1,0,0);
    Recorder actual; moved.Execute(&actual,list1?&actual:nullptr);
    Check(actual.events!=wrong.events,"literal oracle rejects stale native PSO draw after null");
    moved.Reset(); Check(moved.ResolvePipelineHandlesForReplay(),"empty preflight succeeds");
    Replay(moved,Recorder{},list1,"empty resolved stream replays nothing");
  }
}

void ChunkReadinessControls() {
  using Access = rex::graphics::d3d12::NativeDeferredReplayTestAccess;
  using Fault = Access::Fault;
  auto& phase = rex::graphics::GetPhase3Counters();
  for (bool retained : {false,true}) {
    D3D12CommandProcessor cp;
    DeferredCommandList stream(cp,8);
    nb_native_deferred_storage=retained; stream.Reset();
    Check(stream.UsedBytes()==0 && stream.IsReadyForReplay(),"empty chunk is ready without bytes");
    auto* good=Opaque<void>(0x9000); auto* later=Opaque<void>(0xA000);
    auto* pso=Opaque<ID3D12PipelineState>(0x3000);
    cp.pipelines[good]=pso;
    stream.SetPipelineStateHandle(good); stream.D3DDrawInstanced(3,1,0,0);
    stream.SetPipelineStateHandle(later); stream.D3DDispatch(2,3,4);
    const auto bytes=Access::Tail(stream,0);
    const auto used=stream.UsedBytes();
    const auto nulls=phase.execute_time_null_handle_count;
    const auto commands=GetNativeDeferredStorageStats().commands;
    for (bool ready : {false,true,false,true}) {
      cp.pipelines[later]=ready?pso:nullptr;
      Check(stream.IsReadyForReplay()==ready,"readiness observes current pipeline result without memoizing");
      Check(Access::Tail(stream,0)==bytes && stream.UsedBytes()==used &&
            !stream.pipeline_handles_resolved_for_replay(),"readiness never mutates source or marker");
      Check(phase.execute_time_null_handle_count==nulls &&
            GetNativeDeferredStorageStats().commands==commands,"readiness writes no replay/storage counters");
    }
    // The final post-wait preflight must retain its original null suppression.
    cp.pipelines[later]=nullptr;
    Check(stream.ResolvePipelineHandlesForReplay(),"final preflight still accepts failed pipeline");
    Check(phase.execute_time_null_handle_count==nulls+1,"only final preflight counts null pipeline");
    const auto resolved_bytes=Access::Tail(stream,0);
    Check(stream.IsReadyForReplay() && stream.pipeline_handles_resolved_for_replay() &&
          Access::Tail(stream,0)==resolved_bytes,"resolved explicit null is stable logical state");
    Recorder expected; expected.SetPipelineState(pso); expected.DrawInstanced(3,1,0,0);
    Replay(stream,expected,false,"final null behavior unchanged after readiness checks");
    for (Fault fault : {Fault::kInvalidId,Fault::kTruncatedSuffix,Fault::kOverlongPayload,
                       Fault::kWrongHandleSize,Fault::kExtentBeyondStorage}) {
      stream.Reset(); stream.SetPipelineStateHandle(good);
      const size_t suffix=Access::End(stream);
      stream.SetPipelineStateHandle(good); Access::Damage(stream,suffix,fault);
      const auto malformed=Access::Tail(stream,0);
      const auto malformed_used=stream.UsedBytes();
      for (unsigned attempt=0;attempt!=2;++attempt) {
        Check(!stream.IsReadyForReplay(),"malformed suffix is never chunk-ready");
        Check(Access::Tail(stream,0)==malformed && stream.UsedBytes()==malformed_used &&
              !stream.pipeline_handles_resolved_for_replay(),"malformed readiness preserves even valid prefix");
        Check(phase.execute_time_null_handle_count==nulls+1,"malformed readiness never counts nulls");
      }
      // Never Execute a malformed fixture.
    }
    stream.Reset(); stream.SetPipelineStateHandle(nullptr);
    Check(!stream.IsReadyForReplay(),"null opaque handle conservatively remains on CP");
    stream.Reset(); Check(stream.UsedBytes()==0,"Reset clears logical chunk bytes");
  }
}

void ChunkReplayControls() {
  using Access = rex::graphics::d3d12::NativeDeferredReplayTestAccess;
  auto& phase = rex::graphics::GetPhase3Counters();
  for (bool retained : {false,true}) for (bool list1 : {false,true}) {
    D3D12CommandProcessor cp;
    DeferredCommandList full(cp,8), source(cp,8), received(cp,8);
    nb_native_deferred_storage=retained; full.Reset();
    Recorder expected;
    Scenario(full,expected,cp,0x678,3,list1);
    Replay(full,expected,list1,"unsplit full literal trace before chunking");
    Check(full.ResolvePipelineHandlesForReplay(),"resolve fixture before immutable slicing");
    const auto boundaries=Access::Boundaries(full);
    const auto nulls=phase.execute_time_null_handle_count;
    cp.pipelines.clear(); // Chunk execution cannot consult the original resolver.
    DeferredCommandList::ReplayState state;
    Recorder actual,wrong_fresh_state;
    size_t total_bytes=0;
    for (size_t i=1;i<boundaries.size();++i) {
      nb_native_deferred_storage=(i&1)?retained:!retained;
      source.Reset(); Access::CopySpan(source,full,boundaries[i-1],boundaries[i]);
      const size_t chunk_bytes=source.UsedBytes();
      Check(chunk_bytes==(boundaries[i]-boundaries[i-1])*sizeof(uintmax_t),"chunk threshold uses logical bytes");
      Check(source.IsReadyForReplay(),"resolved immutable command ready for handoff");
      source.SwapStorage(received);
      source.Reset(); source.D3DSetPipelineState(Opaque<ID3D12PipelineState>(0xDEAD));
      source.D3DDrawInstanced(999,1,0,0); // Poison CP scratch before receiving-side replay.
      Check(received.UsedBytes()==chunk_bytes,"handoff preserves independent owned chunk extent");
      received.Execute(&actual,list1?&actual:nullptr,&state);
      received.Execute(&wrong_fresh_state,list1?&wrong_fresh_state:nullptr);
      total_bytes+=chunk_bytes;
    }
    Check(total_bytes==full.UsedBytes(),"all commands replayed exactly once across chunks");
    Equal(actual,expected,"one-command chunks preserve full API trace and all null suppression");
    Check(wrong_fresh_state.events!=expected.events,"oracle detects missing PSO inheritance across chunks");
    Check(state.current_pipeline_state==Opaque<ID3D12PipelineState>(0x3000),"final logical PSO survives chunk boundary");
    Check(phase.execute_time_null_handle_count==nulls,"resolved chunks do not touch CP null counters");
    // Starting a new physical list must clear inheritance; legacy calls remain fresh.
    source.Reset(); source.D3DDispatch(4,5,6); source.D3DDrawInstanced(7,1,0,0);
    source.D3DDrawIndexedInstanced(9,1,0,0,0);
    Recorder inherited,empty,legacy;
    source.Execute(&inherited,nullptr,&state);
    Check(inherited.events.size()==3,"all leading work commands inherit the previous chunk PSO");
    state={}; source.Execute(&empty,nullptr,&state); source.Execute(&legacy,nullptr);
    Equal(empty,Recorder{},"new physical list state suppresses unbound work");
    Equal(legacy,Recorder{},"default Execute never inherits an earlier call");
  }
}

void Controls() {
  D3D12CommandProcessor cp;
  DeferredCommandList legacy(cp,8), reuse(cp,8); // exercise real vector growth
  std::set<std::string> families;
  for(unsigned pass=0;pass<24;++pass) {
    const size_t count=pass%4==0?2048:pass%4==1?1:pass%4==2?113:0;
    const bool list1=(pass&1)!=0;
    nb_native_deferred_storage=false; legacy.Reset();
    nb_native_deferred_storage=true; reuse.Reset();
    Recorder a,b;
    Scenario(legacy,a,cp,pass*97,count,list1);
    Scenario(reuse,b,cp,pass*97,count,list1);
    Equal(a,b,"independent input traces");
    Replay(legacy,a,list1,"legacy vs direct API oracle");
    Replay(reuse,b,list1,"retained storage vs direct API oracle");
    Replay(reuse,b,list1,"repeat Execute retains immutable stream");
    for(const Event& e:a.events) families.insert(e.name);
    // Copied and moved lists retain only the logical prefix, while moved-from
    // lists must execute as empty even if their old storage had a long tail.
    DeferredCommandList copy(reuse);
    Replay(copy,b,list1,"copy preserves logical extent");
    DeferredCommandList moved(std::move(copy));
    Replay(moved,b,list1,"move preserves logical extent");
    Replay(copy,Recorder{},list1,"moved-from stream is empty");
    reuse.Reset(); Replay(reuse,Recorder{},list1,"empty reset ignores poisoned prior submission");
  }
  CompoundConstantBufferControls();
  CompoundPixelMemoControls();
  MalformedPreflightControls();
  PreflightAndSwapControls();
  ChunkReadinessControls();
  ChunkReplayControls();
  const auto stats=GetNativeDeferredStorageStats();
  Check(stats.commands>0 && stats.reused_bytes>0 && stats.initialized_bytes>0,"actual growth and retained-word paths exercised");
  Check(families.size()==39,"all replay API families exercised");
  // Modes latch at Reset, including a toggle between two commands. The next
  // Reset selects the new policy without replaying old snapshots.
  nb_native_deferred_storage=true; reuse.Reset();
  reuse.D3DOMSetStencilRef(0xA1);
  nb_native_deferred_storage=false;
  reuse.D3DOMSetStencilRef(0xB2);
  Recorder pair; pair.OMSetStencilRef(0xA1); pair.OMSetStencilRef(0xB2);
  Replay(reuse,pair,false,"mid-submission flag transition");
  reuse.Reset();
  Recorder short_trace; short_trace.OMSetStencilRef(0xCC);
  reuse.D3DOMSetStencilRef(0xCC);
  Replay(reuse,short_trace,false,"enabled to legacy clears stale tail");
  nb_native_deferred_storage=true; reuse.Reset();
  reuse.D3DOMSetStencilRef(0xDD); short_trace.events.clear(); short_trace.OMSetStencilRef(0xDD);
  Replay(reuse,short_trace,false,"legacy to enabled preserves exact trace");
  // Meaningful negative oracle control: equal record count with a changed
  // active value must be detected by the independent semantic trace.
  Recorder actual; reuse.Execute(&actual,nullptr);
  short_trace.events.front().values.front()^=1;
  Check(actual.events!=short_trace.events,"semantic oracle catches one-bit argument corruption");
  std::cout<<"PASS: actual deferred replay CPU controls checks="<<checks<<" api_families="<<families.size()
           <<" enabled_commands="<<stats.commands<<" reused_bytes="<<stats.reused_bytes
           <<" initialized_bytes="<<stats.initialized_bytes<<" compound_cbv_controls=1 replay_preflight_swap_controls=1"
           <<" chunk_readiness_controls=1 chunk_replay_controls=1\n";
}
}
int main(int argc,char** argv) {
  try {
    if (argc==2 && std::string(argv[1])=="--benchmark") CompoundConstantBufferBenchmark();
    else { Check(argc==1,"usage: test_native_deferred_command_list [--benchmark]"); Controls(); }
    return 0;
  }
  catch(const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<'\n'; return 1; }
}
