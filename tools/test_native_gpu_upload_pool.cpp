// Original CPU-only fault/lifetime controls for the ACTUAL production pool and
// SDK GraphicsUploadBufferPool. No D3D12 device creation, GPU or runtime DLL.
// Build WITHOUT NDEBUG: the failed-retirement regression must retain SDK asserts.
#ifdef NDEBUG
#error Compile these controls with assertions enabled (/UNDEBUG or -UNDEBUG).
#endif
#ifndef NB_NATIVE_GPU_UPLOAD_POOL_TEST
#error Compile with NB_NATIVE_GPU_UPLOAD_POOL_TEST and the isolated logging shim.
#endif
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
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>
#include <vector>
#include "native/native_gpu_upload_pool.h"

namespace allocation_fault {
// Single-threaded executable; injection is armed only around one production call.
// Disarm before throwing, so exception handling and fallback can allocate normally.
thread_local long countdown = -1;
thread_local size_t throws = 0;
void Before() {
  if (countdown < 0) return;
  if (countdown-- == 0) { countdown = -1; ++throws; throw std::bad_alloc(); }
}
struct Scope {
  explicit Scope(long successes) { countdown = successes; }
  ~Scope() { countdown = -1; }
};
}
void* operator new(size_t n) {
  allocation_fault::Before();
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

namespace {
using Pool = nb::gpu::NativeConstantUploadPool;
constexpr size_t kPage = 65536;
size_t checks = 0;
void Check(bool ok, const char* reason) {
  ++checks;
  if (!ok) { std::fprintf(stderr, "FAIL after %zu checks: %s\n", checks, reason); std::exit(1); }
}
[[noreturn]] void Unexpected() {
  std::fputs("FAIL unexpected COM vtable call\n", stderr); std::abort();
}
#define UNUSED_HR(name, ...) HRESULT STDMETHODCALLTYPE name(__VA_ARGS__) override { Unexpected(); }
#define UNUSED_VOID(name, ...) void STDMETHODCALLTYPE name(__VA_ARGS__) override { Unexpected(); }
#define OBJECT_UNUSED \
  UNUSED_HR(GetPrivateData, REFGUID, UINT*, void*) \
  UNUSED_HR(SetPrivateData, REFGUID, UINT, const void*) \
  UNUSED_HR(SetPrivateDataInterface, REFGUID, const IUnknown*) \
  UNUSED_HR(SetName, LPCWSTR)

// Real ID3D12Resource vtable; fixture storage outlives all references. Release
// to zero marks destruction without deleting the fixture's observable record.
struct MockResource final : ID3D12Resource {
  ULONG refs = 0;
  unsigned releases = 0, destroyed = 0, maps = 0;
  bool fail_map = false, null_map = false;
  D3D12_HEAP_TYPE heap_type = D3D12_HEAP_TYPE_UPLOAD;
  D3D12_RESOURCE_DESC desc{};
  D3D12_GPU_VIRTUAL_ADDRESS address = 0;
  std::vector<uint8_t> bytes;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (id != __uuidof(IUnknown) && id != __uuidof(ID3D12Resource)) return E_NOINTERFACE;
    *out = static_cast<ID3D12Resource*>(this); AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { Check(refs != 0, "AddRef live resource"); return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override {
    Check(refs != 0, "no double resource release"); ++releases;
    if (--refs == 0) ++destroyed;
    return refs;
  }
  OBJECT_UNUSED
  UNUSED_HR(GetDevice, REFIID, void**)
  HRESULT STDMETHODCALLTYPE Map(UINT subresource, const D3D12_RANGE* reads, void** out) override {
    Check(refs && subresource == 0 && out && reads && reads->Begin == 0 && reads->End == 0,
          "write-only whole-buffer Map contract");
    ++maps; *out = nullptr;
    if (fail_map) return E_OUTOFMEMORY;
    if (!null_map) *out = bytes.data();
    return S_OK;
  }
  UNUSED_VOID(Unmap, UINT, const D3D12_RANGE*)
  D3D12_RESOURCE_DESC STDMETHODCALLTYPE GetDesc() override { return desc; }
  D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE GetGPUVirtualAddress() override {
    Check(refs != 0, "GPU VA obtained from live resource"); return address;
  }
  UNUSED_HR(WriteToSubresource, UINT, const D3D12_BOX*, const void*, UINT, UINT)
  UNUSED_HR(ReadFromSubresource, void*, UINT, UINT, UINT, const D3D12_BOX*)
  HRESULT STDMETHODCALLTYPE GetHeapProperties(D3D12_HEAP_PROPERTIES* heap, D3D12_HEAP_FLAGS* flags) override {
    if (heap) { *heap = {}; heap->Type = heap_type; }
    if (flags) *flags = D3D12_HEAP_FLAG_NONE;
    return S_OK;
  }
};

// All ID3D12Device entries are implemented. Unused calls terminate rather than
// silently accepting an accidental production dependency or a wrong vtable ABI.
struct MockDevice final : ID3D12Device {
  ULONG refs = 1;
  bool supported = true, fail_feature = false;
  bool fail_gpu_create = false, fail_upload_create = false;
  bool fail_gpu_map = false, fail_upload_map = false;
  bool null_gpu_map = false, null_upload_map = false;
  unsigned feature_calls = 0, gpu_attempts = 0, upload_attempts = 0;
  size_t created = 0;
  std::array<MockResource, 24> resources;
  ~MockDevice() {
    Check(refs == 1, "pool device references balanced");
    for (size_t i = 0; i < created; ++i) Check(resources[i].refs == 0, "fixture drained every retained resource");
  }
  // Deliberate-retain controls release only after asserting that pool shutdown
  // and destruction did NOT release the outstanding reference. This is fixture
  // cleanup, not an assertion that unproven real GPU resources may be released.
  void DrainRetained() {
    for (size_t i = 0; i < created; ++i)
      while (resources[i].refs) resources[i].Release();
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (id != __uuidof(IUnknown) && id != __uuidof(ID3D12Device)) return E_NOINTERFACE;
    *out = static_cast<ID3D12Device*>(this); AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override { Check(refs > 1, "fixture owns device reference"); return --refs; }
  OBJECT_UNUSED
  UINT STDMETHODCALLTYPE GetNodeCount() override { Unexpected(); }
  UNUSED_HR(CreateCommandQueue, const D3D12_COMMAND_QUEUE_DESC*, REFIID, void**)
  UNUSED_HR(CreateCommandAllocator, D3D12_COMMAND_LIST_TYPE, REFIID, void**)
  UNUSED_HR(CreateGraphicsPipelineState, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**)
  UNUSED_HR(CreateComputePipelineState, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**)
  UNUSED_HR(CreateCommandList, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*, ID3D12PipelineState*, REFIID, void**)
  HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D12_FEATURE feature, void* data, UINT size) override {
    Check(feature == D3D12_FEATURE_D3D12_OPTIONS16 && size == sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS16),
          "feature check uses OPTIONS16");
    ++feature_calls;
    if (fail_feature) return E_INVALIDARG;
    static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS16*>(data)->GPUUploadHeapSupported = supported;
    return S_OK;
  }
  UNUSED_HR(CreateDescriptorHeap, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**)
  UINT STDMETHODCALLTYPE GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE) override { Unexpected(); }
  UNUSED_HR(CreateRootSignature, UINT, const void*, SIZE_T, REFIID, void**)
  UNUSED_VOID(CreateConstantBufferView, const D3D12_CONSTANT_BUFFER_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE)
  UNUSED_VOID(CreateShaderResourceView, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE)
  UNUSED_VOID(CreateUnorderedAccessView, ID3D12Resource*, ID3D12Resource*, const D3D12_UNORDERED_ACCESS_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE)
  UNUSED_VOID(CreateRenderTargetView, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE)
  UNUSED_VOID(CreateDepthStencilView, ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE)
  UNUSED_VOID(CreateSampler, const D3D12_SAMPLER_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE)
  UNUSED_VOID(CopyDescriptors, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE)
  UNUSED_VOID(CopyDescriptorsSimple, UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE)
  D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo(UINT, UINT, const D3D12_RESOURCE_DESC*) override { Unexpected(); }
  D3D12_HEAP_PROPERTIES STDMETHODCALLTYPE GetCustomHeapProperties(UINT, D3D12_HEAP_TYPE) override { Unexpected(); }
  HRESULT STDMETHODCALLTYPE CreateCommittedResource(const D3D12_HEAP_PROPERTIES* heap,
      D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state,
      const D3D12_CLEAR_VALUE* clear, REFIID id, void** out) override {
    Check(out && heap && desc && id == __uuidof(ID3D12Resource), "resource creation interface");
    *out = nullptr;
    const bool gpu = heap->Type == D3D12_HEAP_TYPE_GPU_UPLOAD;
    Check(gpu || heap->Type == D3D12_HEAP_TYPE_UPLOAD, "only CPU-writable read heaps");
    Check(heap->CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_UNKNOWN &&
          heap->MemoryPoolPreference == D3D12_MEMORY_POOL_UNKNOWN &&
          heap->CreationNodeMask == 1 && heap->VisibleNodeMask == 1, "heap properties unchanged");
    Check(flags == D3D12_HEAP_FLAG_NONE && state == D3D12_RESOURCE_STATE_GENERIC_READ && !clear,
          "constant page immutable read state");
    Check(desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && desc->Alignment == 0 &&
          desc->Width == kPage && desc->Height == 1 && desc->DepthOrArraySize == 1 &&
          desc->MipLevels == 1 && desc->Format == DXGI_FORMAT_UNKNOWN &&
          desc->SampleDesc.Count == 1 && desc->SampleDesc.Quality == 0 &&
          desc->Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR && desc->Flags == D3D12_RESOURCE_FLAG_NONE,
          "literal page descriptor");
    if (gpu) ++gpu_attempts; else ++upload_attempts;
    if (gpu ? fail_gpu_create : fail_upload_create) return E_OUTOFMEMORY;
    Check(created < resources.size(), "bounded fixture resource arena");
    auto& resource = resources[created];
    resource.bytes.assign(kPage, 0xCD);
    resource.refs = 1; resource.desc = *desc; resource.heap_type = heap->Type;
    resource.address = 0x123400000000ull + UINT64(created + 1) * 0x100000ull;
    resource.fail_map = gpu ? fail_gpu_map : fail_upload_map;
    resource.null_map = gpu ? null_gpu_map : null_upload_map;
    ++created; *out = static_cast<ID3D12Resource*>(&resource); return S_OK;
  }
  UNUSED_HR(CreateHeap, const D3D12_HEAP_DESC*, REFIID, void**)
  UNUSED_HR(CreatePlacedResource, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**)
  UNUSED_HR(CreateReservedResource, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**)
  UNUSED_HR(CreateSharedHandle, ID3D12DeviceChild*, const SECURITY_ATTRIBUTES*, DWORD, LPCWSTR, HANDLE*)
  UNUSED_HR(OpenSharedHandle, HANDLE, REFIID, void**)
  UNUSED_HR(OpenSharedHandleByName, LPCWSTR, DWORD, HANDLE*)
  UNUSED_HR(MakeResident, UINT, ID3D12Pageable* const*)
  UNUSED_HR(Evict, UINT, ID3D12Pageable* const*)
  UNUSED_HR(CreateFence, UINT64, D3D12_FENCE_FLAGS, REFIID, void**)
  HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override { Unexpected(); }
  UNUSED_VOID(GetCopyableFootprints, const D3D12_RESOURCE_DESC*, UINT, UINT, UINT64, D3D12_PLACED_SUBRESOURCE_FOOTPRINT*, UINT*, UINT64*, UINT64*)
  UNUSED_HR(CreateQueryHeap, const D3D12_QUERY_HEAP_DESC*, REFIID, void**)
  UNUSED_HR(SetStablePowerState, BOOL)
  UNUSED_HR(CreateCommandSignature, const D3D12_COMMAND_SIGNATURE_DESC*, ID3D12RootSignature*, REFIID, void**)
  UNUSED_VOID(GetResourceTiling, ID3D12Resource*, UINT*, D3D12_PACKED_MIP_INFO*, D3D12_TILE_SHAPE*, UINT*, UINT, D3D12_SUBRESOURCE_TILING*)
  LUID STDMETHODCALLTYPE GetAdapterLuid() override { Unexpected(); }
};
#undef OBJECT_UNUSED
#undef UNUSED_VOID
#undef UNUSED_HR
static_assert(!std::is_abstract_v<MockResource> && !std::is_abstract_v<MockDevice>);
static_assert(noexcept(Pool::TryCreate(nullptr)));
static_assert(noexcept(std::declval<Pool&>().Request(1, 208, 256, nullptr, nullptr, nullptr)));
static_assert(noexcept(std::declval<Pool&>().Shutdown(false)));

struct Slice { MockResource* resource; uint8_t* memory; size_t offset; uint64_t address; };
Slice Allocate(Pool& pool, uint64_t frame, size_t size) {
  ID3D12Resource* resource = nullptr; size_t offset = SIZE_MAX; uint64_t address = 0;
  uint8_t* memory = pool.Request(frame, size, 256, &resource, &offset, &address);
  Check(memory && resource, "allocation served");
  auto* mock = static_cast<MockResource*>(resource);
  Check(offset % 256 == 0 && offset + size <= mock->bytes.size(), "slice alignment and extent");
  Check(memory == mock->bytes.data() + offset && address == mock->address + offset,
        "resource, CPU address and full 64-bit GPU VA agree");
  return {mock, memory, offset, address};
}
void Refused(Pool& pool, uint64_t frame = 9, size_t size = 208) {
  // Sentinels are NEVER dereferenced and must remain untouched on every refusal.
  auto* resource = reinterpret_cast<ID3D12Resource*>(uintptr_t(0x1234));
  size_t offset = 0x5678; uint64_t address = 0xFEDCBA9876543210ull;
  Check(pool.Request(frame, size, 256, &resource, &offset, &address) == nullptr, "request refuses safely");
  Check(resource == reinterpret_cast<ID3D12Resource*>(uintptr_t(0x1234)) && offset == 0x5678 &&
        address == 0xFEDCBA9876543210ull, "refusal preserves all output arguments");
}
std::unique_ptr<Pool> Create(MockDevice& device, uint64_t budget = 4 * kPage) {
  auto pool = Pool::TryCreate(&device, kPage, budget);
  Check(pool != nullptr, "factory succeeds"); return pool;
}
void LiteralBytes(const Slice& slice, size_t size, uint8_t seed) {
  for (size_t i = 0; i < size; ++i) slice.memory[i] = uint8_t(seed + i * 37);
}
void VerifyBytes(const Slice& slice, size_t size, uint8_t seed) {
  for (size_t i = 0; i < size; ++i)
    Check(slice.memory[i] == uint8_t(seed + i * 37), "published slice remains byte-identical");
}

void ReclaimAndAlignment() {
  MockDevice device; auto pool = Create(device);
  auto a = Allocate(*pool, 2, kPage); LiteralBytes(a, kPage, 7);
  auto b = Allocate(*pool, 3, kPage); LiteralBytes(b, kPage, 17);
  pool->ReclaimCompleted(1);
  auto c = Allocate(*pool, 3, kPage); LiteralBytes(c, kPage, 27);
  Check(a.resource != b.resource && b.resource != c.resource && a.resource != c.resource,
        "uncompleted submitted pages cannot be recycled");
  VerifyBytes(a, kPage, 7); VerifyBytes(b, kPage, 17);
  pool->ReclaimCompleted(2);
  auto d = Allocate(*pool, 4, 208);
  Check(d.resource == a.resource && d.offset == 0 && device.created == 3,
        "exactly completed page reclaims through actual SDK lists");
  LiteralBytes(d, 208, 37);
  auto e = Allocate(*pool, 4, 688);
  Check(e.resource == a.resource && e.offset == 256, "dense immutable same-frame slices");
  VerifyBytes(d, 208, 37);
  for (size_t i = 208; i < 256; ++i) Check(a.memory[i] == uint8_t(7 + i * 37), "allocator never writes padding");
  VerifyBytes(b, kPage, 17); VerifyBytes(c, kPage, 27);
  pool->Shutdown(true); pool->Shutdown(true);
  Check(pool->stats().gpu_bytes == 0 && pool->stats().retained_resources == 0,
        "proven shutdown releases video-memory budget without retention");
  for (size_t i = 0; i < device.created; ++i) Check(device.resources[i].destroyed == 1, "proven shutdown releases once");
  Refused(*pool); pool.reset();
}

void SupportAndBudget() {
  for (int mode = 0; mode != 3; ++mode) {
    MockDevice device; device.supported = mode != 0; device.fail_feature = mode == 1;
    auto pool = Create(device, kPage);
    for (int i = 0; i != 3; ++i) Allocate(*pool, 1, kPage);
    const auto stats = pool->stats();
    Check(stats.requests == 3 && stats.requested_bytes == 3 * kPage, "served-slice counters");
    if (mode == 2) {
      Check(device.gpu_attempts == 1 && device.upload_attempts == 2 && stats.gpu_bytes == kPage &&
            stats.gpu_pages == 1 && stats.upload_pages == 2 && stats.budget_stops == 2,
            "one-page video-memory cap produces a mixed pool");
    } else {
      Check(device.gpu_attempts == 0 && device.upload_attempts == 3 && !pool->gpu_upload_supported(),
            "unsupported or failed feature query uses ordinary upload");
    }
    pool->Shutdown(true);
  }
}

void GpuFailureLatch() {
  for (int mode = 0; mode != 3; ++mode) {
    MockDevice device;
    device.fail_gpu_create = mode == 0; device.fail_gpu_map = mode == 1; device.null_gpu_map = mode == 2;
    auto pool = Create(device);
    Allocate(*pool, 1, kPage);
    device.fail_gpu_create = device.fail_gpu_map = device.null_gpu_map = false;
    Allocate(*pool, 1, kPage);
    Check(device.gpu_attempts == 1 && device.upload_attempts == 2 && pool->stats().creation_failures == 1,
          "GPU create/map/null-map failure latches ordinary pages despite later recovery");
    if (mode) Check(device.resources[0].refs == 0 && device.resources[0].destroyed == 1,
                    "unpublished failed GPU page releases immediately");
    pool->Shutdown(true);
  }
}

void FailedRetirement() {
  for (int mode = 0; mode != 4; ++mode) {
    MockDevice device; device.supported = false; auto pool = Create(device);
    auto first = Allocate(*pool, 7, kPage); LiteralBytes(first, kPage, 91);
    device.fail_upload_create = mode == 0;
    device.fail_upload_map = mode == 1;
    device.null_upload_map = mode == 2;
    const auto throw_before = allocation_fault::throws;
    if (mode == 3) { allocation_fault::Scope fault(0); Refused(*pool, 7); }
    else Refused(*pool, 7);
    if (mode == 3) Check(allocation_fault::throws == throw_before + 1, "OwnedPage host allocation fault was exercised");
    Check(first.resource->refs == 1, "failed replacement preserves published page ownership");
    VerifyBytes(first, kPage, 91);
    device.fail_upload_create = device.fail_upload_map = device.null_upload_map = false;
    const auto attempts = device.upload_attempts;
    // With assertions enabled this next call catches the old SDK nonzero-used /
    // null-open-page failure state. It must be bypassed, not accidentally retried.
    Refused(*pool, 8); pool->ReclaimCompleted(7); Refused(*pool, 8);
    Check(device.upload_attempts == attempts, "all optional allocation remains disabled after refusal");
    if (mode == 1 || mode == 2) Check(device.resources[1].destroyed == 1, "unpublished ordinary failed-map page released");
    pool->Shutdown(true);
    Check(first.resource->destroyed == 1, "published page releases after proven completion");
  }
}

void FactoryAllocationFailures() {
  for (long position : {0L, 1L}) {
    MockDevice device;
    const auto before = allocation_fault::throws;
    std::unique_ptr<Pool> pool;
    { allocation_fault::Scope fault(position); pool = Pool::TryCreate(&device, kPage, kPage); }
    Check(!pool && allocation_fault::throws == before + 1,
          "factory contains pool-object and shared-budget allocation failures");
    Check(device.refs == 1 && device.created == 0, "failed constructor balances device and publishes nothing");
  }
}

void TerminalShutdown() {
  for (int mode = 0; mode != 3; ++mode) {
    MockDevice device; auto pool = Create(device, kPage);
    auto gpu = Allocate(*pool, 1, kPage);
    auto ordinary = Allocate(*pool, 2, 208); // GPU page is submitted, ordinary page is still open.
    LiteralBytes(gpu, kPage, 3); LiteralBytes(ordinary, 208, 43);
    if (mode != 2) {
      pool->Shutdown(false);
      Refused(*pool);
      pool->ReclaimCompleted(UINT64_MAX);
      pool->Shutdown(mode == 1); // false -> true must not retroactively reclaim detached refs.
      Check(pool->stats().gpu_bytes == kPage && pool->stats().retained_resources == 2,
            "retained video-memory budget and both resource references stay accounted");
    }
    pool.reset(); // also exercises direct destructor without any completion proof.
    Check(gpu.resource->refs == 1 && ordinary.resource->refs == 1 &&
          gpu.resource->releases == 0 && ordinary.resource->releases == 0,
          "unproven shutdown/destructor deliberately retains all published resource refs");
    VerifyBytes(gpu, kPage, 3); VerifyBytes(ordinary, 208, 43);
    device.DrainRetained();
  }
  // Detach must not leak a resource which never became a published pool page.
  MockDevice device; device.fail_gpu_map = true; auto pool = Create(device);
  auto ordinary = Allocate(*pool, 1, 208);
  Check(device.resources[0].destroyed == 1, "failed unused page already released before unproven shutdown");
  pool->Shutdown(false); pool.reset();
  Check(device.resources[0].refs == 0 && ordinary.resource->refs == 1, "only published page is retained");
  device.DrainRetained();
}

void InvalidRequestsLatch() {
  for (size_t size : {size_t(0), kPage + 1}) {
    MockDevice device; auto pool = Create(device);
    Refused(*pool, 1, size); Refused(*pool, 1);
    Check(device.created == 0 && device.gpu_attempts == 0, "unservable input latches before device access");
    pool->Shutdown(true);
  }
  // The constructor remains public: a null-device instance cannot make requests.
  Pool pool(nullptr, kPage, kPage);
  Refused(pool); Refused(pool); pool.Shutdown(true);
}
}

int main() {
  ReclaimAndAlignment();
  SupportAndBudget();
  GpuFailureLatch();
  FailedRetirement();
  FactoryAllocationFailures();
  TerminalShutdown();
  InvalidRequestsLatch();
  std::printf("PASS %zu checks; actual pool + SDK allocator, 7 control groups; no GPU/runtime\n", checks);
}
