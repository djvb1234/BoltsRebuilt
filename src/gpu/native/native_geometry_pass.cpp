// See native_geometry_pass.h.

#include "native_geometry_pass.h"

#include <d3d12sdklayers.h>
#include <d3d12shader.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <utility>

#include <rex/cvar.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/shared_memory.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>

#include "native_prelude.h"
#include "native_asset_cache.h"
#include "native_gpu_upload_pool.h"
#include "native_constant_upload_diagnostics.h"
#include "native_shader_cache.h"
#include "native_shader_constant_reflection.h"
#include "native_timing.h"
#include "native_root_parameter.h"
#include "native_pipeline_key.h"
#include "native_vertex_range_census.h"

REXCVAR_DEFINE_STRING(nb_native_vertex_range_census_file, "", "nb",
                      "Diagnostic CSV of declared vertex streams and CPU-authoritative index bounds; empty disables; no upload changes");
REXCVAR_DEFINE_UINT32(nb_native_vertex_range_census_first, 6000, "nb", "First backend frame for the vertex range census");
REXCVAR_DEFINE_UINT32(nb_native_vertex_range_census_last, 18000, "nb", "Last backend frame for the vertex range census");
REXCVAR_DEFINE_UINT32(nb_native_vertex_range_census_period, 600, "nb", "Sample one backend frame per period, relative to first; zero samples every frame");


REXCVAR_DEFINE_BOOL(nb_native_minimal_diagnostics, false, "nb",
                    "Skip fine-grained per-draw clocks and periodic draw log spam; frame, IssueDraw, generic and refusal diagnostics remain");
REXCVAR_DEFINE_BOOL(nb_native_constant_upload_diagnostics, false, "nb",
                    "Time native constant allocation, RAM assembly, mapped fill and memo operations on the CPU")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(nb_native_shader_disk_cache, true, "nb",
                    "Reuse exact native D3DCompile results from native_shader_cache beside the executable; false forces compilation for diagnostics");

REXCVAR_DEFINE_BOOL(nb_native_shader_debug, false, "nb",
                    "Compile the generated native shaders with debug info and no optimisation, so a RenderDoc capture can be stepped with the guest register names intact (capture runs only)");

REXCVAR_DEFINE_BOOL(nb_native_constant_reuse, true, "nb",
                    "Reuse exact same-frame pixel constant bytes and the last fully matched ready native pipeline; false keeps the diagnostic original path");

REXCVAR_DEFINE_BOOL(nb_native_pipeline_key_normalize, false, "nb",
                    "Normalize only unused native PSO key fields while preserving the original descriptor and guest state")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(nb_native_stage_vertex_constants, false, "nb",
                    "Build each native vertex constant packet in ordinary RAM, including late asset bindings, then copy it once to the upload pool");

REXCVAR_DEFINE_BOOL(nb_native_vertex_packet_diagnostics, false, "nb",
                    "Count exact same-frame VS packet matches in ordinary RAM without reusing upload allocations or bindings")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(nb_native_vertex_constant_reuse, false, "nb",
                    "Reuse an exact same-frame immutable VS constant slice when the complete packet has no asset-cache bindings")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(nb_native_double_pixel_packets, false, "nb",
                    "Publish uploaded pixel constant packets by changing owned RAM slots, avoiding the extra cache snapshot copy");

REXCVAR_DEFINE_BOOL(nb_native_empty_constant_layout, false, "nb",
                    "Omit unread guest float data only for validated explicitly empty generated constant layouts")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(nb_native_unused_pixel_constants, false, "nb",
                    "Skip pixel packet preparation only when both linked shaders are proven to have no b2 reads")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(nb_native_root_cbv, false, "nb",
                    "Bind the unchanged native b0 payload through an immutable upload-pool root CBV instead of 52 root constants")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(nb_native_constant_binding_packet, false, "nb",
                    "Record native root-CBV writes in one owned deferred packet while retaining the same D3D12 setters and order")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Shared memory drops the valid bit on every CPU-uploaded page when a frame closes and fires no watch
// for it, so the residency bitmap has to be dropped in step. Declared, not defined: it belongs to the
// SDK command processor, which is compiled into this plugin.
REXCVAR_DECLARE(bool, clear_memory_page_state);

namespace nb::gpu {

namespace {

using rex::graphics::d3d12::D3D12CommandProcessor;

Microsoft::WRL::ComPtr<ID3DBlob> Compile(const char* name, const std::string& source, const char* target,
                                         const D3D_SHADER_MACRO* defines = nullptr) {
  Microsoft::WRL::ComPtr<ID3DBlob> code;
  Microsoft::WRL::ComPtr<ID3DBlob> errors;
  // With nb_native_shader_debug the generated HLSL keeps its own variable names in the shader, so a
  // RenderDoc capture can be stepped with the guest's registers - r0, r5, r7 - visible by name and
  // compared instruction for instruction against the emulated path. Optimized builds rename everything,
  // which makes the two traces impossible to line up. Slow: capture runs only.
  UINT flags = D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES;
  flags |= REXCVAR_GET(nb_native_shader_debug) ? (D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION)
                                               : D3DCOMPILE_OPTIMIZATION_LEVEL3;
  shader_cache::Digest cache_key{}, compiler_identity{};
  std::filesystem::path cache_directory;
  bool cache_request = false;
  // The input to the cache is exactly the input to the compiler below. In
  // particular, absent defines differ from explicit default-valued defines.
  const pD3DCompile compiler = REXCVAR_GET(nb_native_shader_disk_cache)
                                  ? shader_cache::CompilerEntryPoint() : nullptr;
  if (compiler) {
    try {
      std::vector<shader_cache::Macro> macros;
      if (defines) {
        for (const D3D_SHADER_MACRO* macro = defines; macro->Name; ++macro)
          macros.push_back({macro->Name, macro->Definition ? macro->Definition : "", macro->Definition != nullptr});
      }
      cache_directory = shader_cache::DefaultDirectory();
      cache_request = !cache_directory.empty() && shader_cache::CompilerIdentity(compiler_identity) &&
          shader_cache::MakeKey({name, source, "main", target, macros, flags, 0}, compiler_identity, cache_key);
      std::vector<uint8_t> cached;
      if (cache_request && shader_cache::Load(cache_directory, cache_key, target, cached) &&
          SUCCEEDED(D3DCreateBlob(cached.size(), &code))) {
        std::memcpy(code->GetBufferPointer(), cached.data(), cached.size());
        return code;
      }
    } catch (...) {
      // Optional cache allocation or filesystem failures must not change draws.
      cache_request = false;
    }
  }
  // Use exactly the export whose owning image was hashed. Cache-off and
  // resolution failure retain the ordinary statically imported entry point.
  const HRESULT hr = compiler
      ? compiler(source.c_str(), source.size(), name, defines, nullptr, "main", target, flags, 0, &code, &errors)
      : D3DCompile(source.c_str(), source.size(), name, defines, nullptr, "main", target, flags, 0, &code, &errors);
  if (FAILED(hr)) {
    REXLOG_ERROR("rexgpu-nb: geometry pass '{}': {} compile failed (0x{:08X}): {}", name, target,
                 static_cast<uint32_t>(hr),
                 errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize())
                        : std::string("no diagnostics"));
    return nullptr;
  }
  if (cache_request) {
    shader_cache::Store(cache_directory, cache_key, target,
                        {static_cast<const uint8_t*>(code->GetBufferPointer()), code->GetBufferSize()});
  }
  return code;
}

D3D12_BLEND MapBlendFactor(rex::graphics::xenos::BlendFactor f, bool alpha) {
  using rex::graphics::xenos::BlendFactor;
  switch (f) {
    case BlendFactor::kZero: return D3D12_BLEND_ZERO;
    case BlendFactor::kOne: return D3D12_BLEND_ONE;
    case BlendFactor::kSrcColor: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
    case BlendFactor::kOneMinusSrcColor: return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
    case BlendFactor::kSrcAlpha: return D3D12_BLEND_SRC_ALPHA;
    case BlendFactor::kOneMinusSrcAlpha: return D3D12_BLEND_INV_SRC_ALPHA;
    case BlendFactor::kDstColor: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
    case BlendFactor::kOneMinusDstColor: return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
    case BlendFactor::kDstAlpha: return D3D12_BLEND_DEST_ALPHA;
    case BlendFactor::kOneMinusDstAlpha: return D3D12_BLEND_INV_DEST_ALPHA;
    case BlendFactor::kConstantColor: return D3D12_BLEND_BLEND_FACTOR;
    case BlendFactor::kOneMinusConstantColor: return D3D12_BLEND_INV_BLEND_FACTOR;
    case BlendFactor::kConstantAlpha: return D3D12_BLEND_BLEND_FACTOR;
    case BlendFactor::kOneMinusConstantAlpha: return D3D12_BLEND_INV_BLEND_FACTOR;
    case BlendFactor::kSrcAlphaSaturate: return D3D12_BLEND_SRC_ALPHA_SAT;
    default: return D3D12_BLEND_ONE;
  }
}

D3D12_BLEND_OP MapBlendOp(rex::graphics::xenos::BlendOp op) {
  using rex::graphics::xenos::BlendOp;
  switch (op) {
    case BlendOp::kSubtract: return D3D12_BLEND_OP_SUBTRACT;
    case BlendOp::kMin: return D3D12_BLEND_OP_MIN;
    case BlendOp::kMax: return D3D12_BLEND_OP_MAX;
    case BlendOp::kRevSubtract: return D3D12_BLEND_OP_REV_SUBTRACT;
    default: return D3D12_BLEND_OP_ADD;
  }
}

D3D12_STATIC_SAMPLER_DESC StaticSampler(UINT reg, D3D12_FILTER filter, D3D12_TEXTURE_ADDRESS_MODE address) {
  D3D12_STATIC_SAMPLER_DESC sampler = {};
  sampler.Filter = filter;
  sampler.AddressU = address;
  sampler.AddressV = address;
  sampler.AddressW = address;
  sampler.MaxAnisotropy = 1;
  sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  sampler.ShaderRegister = reg;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  return sampler;
}


// Residency is by far the most expensive thing a native draw does - 2.66 ms of a town frame against
// 0.01 ms for everything else - and consecutive draws overwhelmingly ask for ranges that were made
// resident moments earlier. This remembers what has been requested and skips the call when a range is
// already covered.
//
// Correctness rests on shared memory's global invalidation watch: it fires whenever anything in the GPU
// copy is invalidated, by a CPU write or by a resolve, and bumping the generation there drops every
// cached range. So a range can never be read after the data behind it changed, which is the whole risk
// in caching this.
//
// The state is a page bitmap rather than a list of ranges. A 64-entry linear scan cost more in
// comparisons than the calls it saved: 6,832 lookups a frame times 64 entries came to 37% hits at
// 1.85 ms, worse than 8 entries' 27% at 1.80 ms. Shared memory is 512 MB and residency is decided per
// page, so the whole state fits in 16 KB of bits and a lookup is one or two word tests. Tracking at
// 4 KB when shared memory's page is larger only under-marks, which costs a hit and never invents one.
struct ResidencyCache {
  static constexpr uint32_t kPageBytesLog2 = 12;
  static constexpr uint32_t kPages = (512u * 1024u * 1024u) >> kPageBytesLog2;
  static constexpr uint32_t kWords = kPages / 64;

  uint64_t pages[kWords] = {};

  void Reset() { std::memset(pages, 0, sizeof(pages)); }

  // False for a range that runs past the end of shared memory, so a bogus fetch base can never index
  // outside the bitmap; the caller then treats it as a miss and RequestRanges rejects it properly.
  static bool PageSpan(uint32_t start, uint32_t length, uint32_t& first, uint32_t& last) {
    if (length == 0) return false;
    const uint64_t end = static_cast<uint64_t>(start) + length - 1;
    if (end >= (static_cast<uint64_t>(kPages) << kPageBytesLog2)) return false;
    first = start >> kPageBytesLog2;
    last = static_cast<uint32_t>(end >> kPageBytesLog2);
    return true;
  }

  bool Covers(uint32_t start, uint32_t length) const {
    uint32_t first, last;
    if (!PageSpan(start, length, first, last)) return false;
    uint32_t word = first >> 6;
    const uint32_t word_last = last >> 6;
    uint64_t mask = ~uint64_t(0) << (first & 63);
    while (word < word_last) {
      if ((pages[word] & mask) != mask) return false;
      mask = ~uint64_t(0);
      ++word;
    }
    mask &= ~uint64_t(0) >> (63 - (last & 63));
    return (pages[word] & mask) == mask;
  }

  void Add(uint32_t start, uint32_t length) {
    uint32_t first, last;
    if (!PageSpan(start, length, first, last)) return;
    MarkSpan(first, last, true);
  }

  // Takes the inclusive byte bounds shared memory reports, since an invalidation is not a length.
  void ClearAddressRange(uint32_t address_first, uint32_t address_last) {
    constexpr uint64_t kBytes = static_cast<uint64_t>(kPages) << kPageBytesLog2;
    if (address_first >= kBytes) return;
    if (address_last >= kBytes) address_last = static_cast<uint32_t>(kBytes - 1);
    if (address_first > address_last) return;
    MarkSpan(address_first >> kPageBytesLog2, address_last >> kPageBytesLog2, false);
  }

 private:
  void MarkSpan(uint32_t first, uint32_t last, bool resident) {
    uint32_t word = first >> 6;
    const uint32_t word_last = last >> 6;
    uint64_t mask = ~uint64_t(0) << (first & 63);
    while (word < word_last) {
      if (resident) {
        pages[word] |= mask;
      } else {
        pages[word] &= ~mask;
      }
      mask = ~uint64_t(0);
      ++word;
    }
    mask &= ~uint64_t(0) >> (63 - (last & 63));
    if (resident) {
      pages[word] |= mask;
    } else {
      pages[word] &= ~mask;
    }
  }
};

// Only the command processor thread touches the bitmap. Invalidations arrive on whichever thread wrote
// the memory, inside shared memory's global lock, so they are queued here instead of clearing the
// bitmap directly; the command processor folds the queue in before it reads. Dropping the whole cache
// on any invalidation was what capped the hit rate at 59%: 48 invalidations a frame, mostly the 41
// resolves, gave each generation about 53 draws to live. A resolve to a render target has nothing to
// say about where the vertex buffers are, so now only the pages it names are dropped.
ResidencyCache g_residency;
uint64_t g_residency_frame = UINT64_MAX;
rex::graphics::SharedMemory::GlobalWatchHandle g_residency_watch = nullptr;

struct PendingInvalidation {
  uint32_t address_first;
  uint32_t address_last;
};
constexpr size_t kMaxPendingInvalidations = 256;
std::mutex g_residency_pending_mutex;
PendingInvalidation g_residency_pending[kMaxPendingInvalidations];
size_t g_residency_pending_count = 0;
bool g_residency_pending_overflow = false;
// Read on every draw, so it stays out of the mutex: zero means there is nothing to fold in.
std::atomic<uint32_t> g_residency_pending_signal{0};

void ResidencyInvalidated(const std::unique_lock<std::recursive_mutex>&, void*, uint32_t address_first,
                          uint32_t address_last, bool) {
  std::lock_guard<std::mutex> lock(g_residency_pending_mutex);
  if (g_residency_pending_count < kMaxPendingInvalidations) {
    g_residency_pending[g_residency_pending_count++] = {address_first, address_last};
  } else {
    // More invalidations than the queue holds; the next drain throws the whole bitmap away, which is
    // always safe, just slower.
    g_residency_pending_overflow = true;
  }
  g_residency_pending_signal.store(1, std::memory_order_release);
}

// Returns how many spans were folded in, for the counters.
size_t DrainResidencyInvalidations() {
  if (g_residency_pending_signal.load(std::memory_order_acquire) == 0) return 0;
  PendingInvalidation drained[kMaxPendingInvalidations];
  size_t count;
  bool overflow;
  {
    std::lock_guard<std::mutex> lock(g_residency_pending_mutex);
    count = g_residency_pending_count;
    overflow = g_residency_pending_overflow;
    std::memcpy(drained, g_residency_pending, count * sizeof(drained[0]));
    g_residency_pending_count = 0;
    g_residency_pending_overflow = false;
    g_residency_pending_signal.store(0, std::memory_order_release);
  }
  if (overflow) {
    g_residency.Reset();
    return count + 1;
  }
  for (size_t i = 0; i < count; ++i) {
    g_residency.ClearAddressRange(drained[i].address_first, drained[i].address_last);
  }
  return count;
}

// The command processor thread is the only writer, so a plain static is enough.
NativeGeometryPass::Timings g_timings;
NativeGeometryPass::ConstantReuseStats g_constant_reuse_stats;
NativeGeometryPass::IndexDrawStats g_index_draw_stats;
NativeGeometryPass::PipelineKeyStats g_pipeline_key_stats;

}  // namespace

NativeGeometryPass::Timings& NativeGeometryPass::timings() { return g_timings; }
const NativeGeometryPass::ConstantReuseStats& NativeGeometryPass::constant_reuse_stats() {
  return g_constant_reuse_stats;
}
const NativeGeometryPass::IndexDrawStats& NativeGeometryPass::index_draw_stats() {
  return g_index_draw_stats;
}
const NativeGeometryPass::PipelineKeyStats& NativeGeometryPass::pipeline_key_stats() {
  return g_pipeline_key_stats;
}

const char* NativeGeometryPass::kCommonHlsl() { return kNativePreludeHlsl; }

namespace {
// One constant slice for this draw. The optional native pool is tried first and the command
// processor's own pool answers whenever that pool is absent, out of budget, or has latched its
// GPU_UPLOAD fallback, so a pool problem costs a heap type and never a draw. Both pools bump-
// allocate within a page and retire a page only after its frame index completes, so the address
// returned here stays immutable for as long as the GPU can read it.
uint8_t* RequestConstantSlice(rex::graphics::d3d12::D3D12CommandProcessor& cp,
                              NativeConstantUploadPool* pool, uint64_t frame, size_t bytes,
                              ID3D12Resource** buffer_out, size_t* offset_out,
                              D3D12_GPU_VIRTUAL_ADDRESS* address_out,
                              NativeConstantPreparationStats* preparations) {
  NativeConstantTimer timer(preparations ? &preparations->allocation : nullptr);
  if (pool) {
    if (uint8_t* memory = pool->Request(frame, bytes, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT,
                                        buffer_out, offset_out, address_out)) {
      return memory;
    }
  }
  uint8_t* memory = cp.GetConstantBufferPool().Request(frame, bytes,
                                            D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT,
                                            buffer_out, offset_out, address_out);
  timer.RecordFailure(memory == nullptr);
  return memory;
}
}  // namespace

Microsoft::WRL::ComPtr<ID3DBlob> NativeGeometryPass::CompileShader(const char* name, const std::string& hlsl,
                                                                   bool vertex, uint32_t interpolators,
                                                                   uint32_t constants, bool effective_samplers,
                                                                   bool direct_guest_reads) {
  const std::string source = std::string(kNativePreludeHlsl) + hlsl;
  const std::string interpolator_count = std::to_string(interpolators);
  // A shader that reads no constants still needs one register: HLSL has no zero-length array.
  const std::string constant_count = std::to_string(constants ? std::max<uint32_t>(1, constants) : 256);
  D3D_SHADER_MACRO defines[6] = {};
  uint32_t define_count = 0;
  defines[define_count++] = {"NB_PIXEL_STAGE", vertex ? "0" : "1"};
  defines[define_count++] = {"NB_EFFECTIVE_SAMPLERS", effective_samplers ? "1" : "0"};
  defines[define_count++] = {"NB_DIRECT_GUEST_READS", direct_guest_reads ? "1" : "0"};
  if (vertex && interpolators != 0) {
    defines[define_count++] = {"NB_INTERPOLATORS", interpolator_count.c_str()};
  }
  if (constants != 0) {
    defines[define_count++] = {vertex ? "NB_VS_CONSTANTS" : "NB_PS_CONSTANTS", constant_count.c_str()};
  }
  return Compile(name, source, vertex ? "vs_5_1" : "ps_5_1", define_count ? defines : nullptr);
}

bool NativeGeometryPass::Initialize(ID3D12Device* device, const char* name, const std::string& vs_hlsl,
                                    const std::string& ps_hlsl) {
  Microsoft::WRL::ComPtr<ID3DBlob> vs = CompileShader(name, vs_hlsl, true);
  Microsoft::WRL::ComPtr<ID3DBlob> ps = ps_hlsl.empty() ? nullptr : CompileShader(name, ps_hlsl, false);
  if (!vs || (!ps && !ps_hlsl.empty())) {
    return false;
  }
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature = CreateRootSignature(device);
  if (!root_signature) {
    return false;
  }
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_cbv_signature = CreateRootSignature(device, true);
  return Initialize(device, name, vs.Get(), ps.Get(), root_signature.Get(), root_cbv_signature.Get());
}

bool NativeGeometryPass::Initialize(ID3D12Device* device, const char* name, ID3DBlob* vs, ID3DBlob* ps,
                                    ID3D12RootSignature* root_signature, ID3D12RootSignature* root_cbv_signature,
                                    bool direct_guest_reads) {
  name_ = name;
  device_ = device;
  vs_ = vs;
  ps_ = ps;  // null for a depth-only pass (the guest bound no pixel shader)
  direct_guest_reads_ = direct_guest_reads;
  vertex_packet_diagnostics_.Reset();
  vertex_constant_reuse_.Reset();
  ps_constant_reuse_.Reset();
  ready_pipeline_reuse_.Reset();
  empty_vertex_buffer_safe_ = false;
  empty_pixel_buffer_safe_ = false;
  root_signature_ = root_signature;
  root_cbv_signature_ = root_cbv_signature;
  if (!vs_ || !root_signature_) {
    return false;
  }
  empty_vertex_buffer_safe_ = ShaderConstantHeaderOnly(vs_.Get(), 1, uint32_t(kVertexConstantHeaderBytes)) &&
      ShaderConstantHeaderOnly(ps_.Get(), 1, uint32_t(kVertexConstantHeaderBytes));
  empty_pixel_buffer_safe_ = ShaderConstantHeaderOnly(vs_.Get(), 2, uint32_t(NativePixelConstantPacket::kHeaderBytes)) &&
      ShaderConstantHeaderOnly(ps_.Get(), 2, uint32_t(NativePixelConstantPacket::kHeaderBytes));
  // The optional present-PS shortcut requires an actual zero-byte b2 proof in
  // BOTH linked blobs. Any used variable, ambiguous binding or reflection failure
  // retains the original path. Keep the older depth-only proof separately so the
  // default-off experiment changes no existing no-PS behavior.
  pixel_buffer_unused_ = ShaderConstantHeaderOnly(vs_.Get(), 2, 0) &&
      ShaderConstantHeaderOnly(ps_.Get(), 2, 0);
  // Root b2 is visible to all stages. Prove it has no VS consumer before
  // aliasing it for a depth-only draw; arbitrary handwritten shaders need not
  // follow the generated prelude's stage naming convention. Reflection failure
  // retains the original separate pixel upload.
  legacy_depth_only_pixel_buffer_unused_ = false;
  if (!ps_) {
    Microsoft::WRL::ComPtr<ID3D12ShaderReflection> reflection;
    if (SUCCEEDED(D3DReflect(vs_->GetBufferPointer(), vs_->GetBufferSize(), IID_PPV_ARGS(&reflection)))) {
      D3D12_SHADER_DESC shader_desc{};
      if (SUCCEEDED(reflection->GetDesc(&shader_desc))) {
        legacy_depth_only_pixel_buffer_unused_ = true;
        for (UINT i = 0; i < shader_desc.BoundResources; ++i) {
          D3D12_SHADER_INPUT_BIND_DESC binding{};
          if (FAILED(reflection->GetResourceBindingDesc(i, &binding)) ||
              (binding.Type == D3D_SIT_CBUFFER &&
               NativeBindingCoversPixelConstants(binding.Space, binding.BindPoint, binding.BindCount))) {
            legacy_depth_only_pixel_buffer_unused_ = false;
            break;
          }
        }
      }
    }
  }
  initialized_ = true;
  REXLOG_INFO("rexgpu-nb: geometry pass '{}' ready (vs {} B, ps {} B)", name, vs_->GetBufferSize(),
              ps_ ? ps_->GetBufferSize() : 0);
  return true;
}

Microsoft::WRL::ComPtr<ID3D12RootSignature> NativeGeometryPass::CreateRootSignature(ID3D12Device* device,
                                                                                bool root_cbv) {
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature;

  // The original form uses 62 root dwords. Replacing only b0 with a CBV uses 12.
  D3D12_ROOT_PARAMETER1 parameters[7] = {};
  parameters[0] = NativeRootParameter(root_cbv, kConstantDwords);
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  parameters[1].Descriptor.ShaderRegister = 1;  // vertex stage constants
  parameters[1].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;
  parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  parameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  parameters[4].Descriptor.ShaderRegister = 2;  // pixel stage constants
  parameters[4].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;
  parameters[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_DESCRIPTOR_RANGE1 texture_ranges[2] = {};
  auto& texture_range = texture_ranges[0];
  texture_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  texture_range.NumDescriptors = UINT_MAX;
  texture_range.BaseShaderRegister = 0;
  texture_range.RegisterSpace = 1;
  texture_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
  texture_range.OffsetInDescriptorsFromTableStart =
      UINT(D3D12CommandProcessor::SystemBindlessView::kUnboundedSRVsStart);
  // Separate HLSL types alias the SDK's existing SRVs in the same heap.
  texture_ranges[1] = texture_range;
  texture_ranges[1].RegisterSpace = 3;
  // Vertex shaders fetch textures too (the grass family does), so the table and the shared memory are
  // visible to both stages.
  parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[2].DescriptorTable.NumDescriptorRanges = 2;
  parameters[2].DescriptorTable.pDescriptorRanges = texture_ranges;
  parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  parameters[3].Descriptor.ShaderRegister = 0;
  parameters[3].Descriptor.RegisterSpace = 2;
  parameters[3].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
  parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_DESCRIPTOR_RANGE1 sampler_range = {};
  sampler_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
  sampler_range.NumDescriptors = UINT_MAX;
  sampler_range.RegisterSpace = 1;
  sampler_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
  parameters[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[5].DescriptorTable.NumDescriptorRanges = 1;
  parameters[5].DescriptorTable.pDescriptorRanges = &sampler_range;
  parameters[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  parameters[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  parameters[6].Descriptor.ShaderRegister = 0;
  parameters[6].Descriptor.RegisterSpace = 4;
  parameters[6].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
  parameters[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
  // The hand-written passes retain their named static samplers in space0.
  const D3D12_STATIC_SAMPLER_DESC samplers[4] = {
      StaticSampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP),
      StaticSampler(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP),
      StaticSampler(2, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP),
      StaticSampler(3, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_WRAP),
  };
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {};
  desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  desc.Desc_1_1.NumParameters = 7;
  desc.Desc_1_1.pParameters = parameters;
  desc.Desc_1_1.NumStaticSamplers = 4;
  desc.Desc_1_1.pStaticSamplers = samplers;
  desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
  Microsoft::WRL::ComPtr<ID3DBlob> blob;
  Microsoft::WRL::ComPtr<ID3DBlob> errors;
  if (FAILED(D3D12SerializeVersionedRootSignature(&desc, &blob, &errors))) {
    REXLOG_ERROR("rexgpu-nb: native geometry root signature serialization failed: {}",
                 errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no diagnostics");
    return nullptr;
  }
  if (FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                         IID_PPV_ARGS(&root_signature)))) {
    REXLOG_ERROR("rexgpu-nb: native geometry CreateRootSignature failed");
    return nullptr;
  }
  return root_signature;
}

ID3D12PipelineState* NativeGeometryPass::GetPipeline(const D3D12CommandProcessor::NativeDrawContext& context,
                                                     const GuestState& state, bool root_cbv) {
  PipelineKey key = {};
  uint32_t* w = key.words;
  for (uint32_t i = 0; i < 4; ++i) w[i] = uint32_t(context.rtv_formats[i]);
  w[4] = uint32_t(context.dsv_format);
  w[5] = context.sample_count;
  for (uint32_t i = 0; i < 4; ++i) w[6 + i] = state.blendcontrol[i];
  w[10] = state.depthcontrol;
  w[11] = state.su_mode_cntl;
  w[12] = state.color_mask;
  w[13] = state.stencil_ref_mask;
  w[14] = uint32_t(state.depth_bias);
  std::memcpy(&w[15], &state.depth_bias_slope, sizeof(float));
  w[16] = (state.depth_clip ? 1u : 0u) | (state.alpha_to_mask ? 2u : 0u) | (root_cbv ? 4u : 0u);
  w[17] = context.sample_mask;
  const bool normalize_key = REXCVAR_GET(nb_native_pipeline_key_normalize);
  if (normalize_key) {
    static_assert(DXGI_FORMAT_UNKNOWN == 0);
    ++g_pipeline_key_stats.normalized_lookups;
    g_pipeline_key_stats.changed_key_draws += NormalizeNativePipelineKey(key.words);
  }
  // Original state/context still feed every descriptor field and the cull
  // refusal below. A raw key equal to a normalized key denotes the same
  // descriptor, so ready/pending entries stay valid across diagnostic toggles.
  // Fold in anything the workers finished before answering from the cache.
  if (!pending_pipelines_.empty()) {
    ReapPipelines();
  }
  const bool reuse = REXCVAR_GET(nb_native_constant_reuse);
  if (reuse) {
    if (const uintptr_t previous = ready_pipeline_reuse_.Find(key.words)) {
      ++g_constant_reuse_stats.pipeline_hits;
      if (normalize_key) ++g_pipeline_key_stats.ready_memo_hits;
      return reinterpret_cast<ID3D12PipelineState*>(previous);
    }
    ++g_constant_reuse_stats.pipeline_misses;
  } else {
    ready_pipeline_reuse_.Reset();
  }
  auto it = pipelines_.find(key);
  if (it != pipelines_.end()) {
    if (normalize_key) {
      if (it->second) ++g_pipeline_key_stats.ready_map_hits;
      else ++g_pipeline_key_stats.known_bad_hits;
    }
    if (reuse && it->second) {
      ready_pipeline_reuse_.Publish(key.words, reinterpret_cast<uintptr_t>(it->second.Get()));
    }
    return it->second.Get();
  }
  if (pending_pipelines_.find(key) != pending_pipelines_.end()) {
    if (normalize_key) ++g_pipeline_key_stats.pending_hits;
    return nullptr;  // already being built; this draw stays emulated
  }
  char key_text[224];
  std::snprintf(key_text, sizeof key_text, "%d,%d,%d,%d|%d|%u|%08X,%08X,%08X,%08X|%08X|%08X|%08X|%08X|%d,%g,%d,%d|root_cbv=%d",
                int(context.rtv_formats[0]), int(context.rtv_formats[1]), int(context.rtv_formats[2]),
                int(context.rtv_formats[3]), int(context.dsv_format), context.sample_count, state.blendcontrol[0],
                state.blendcontrol[1], state.blendcontrol[2], state.blendcontrol[3], state.depthcontrol,
                state.su_mode_cntl, state.color_mask, state.stencil_ref_mask, state.depth_bias,
                double(state.depth_bias_slope), state.depth_clip ? 1 : 0, state.alpha_to_mask ? 1 : 0,
                root_cbv ? 1 : 0);

  rex::graphics::reg::RB_DEPTHCONTROL depth;
  depth.value = state.depthcontrol;
  rex::graphics::reg::PA_SU_SC_MODE_CNTL su;
  su.value = state.su_mode_cntl;

  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
  desc.pRootSignature = root_cbv ? root_cbv_signature_.Get() : root_signature_.Get();
  desc.VS = {vs_->GetBufferPointer(), vs_->GetBufferSize()};
  if (ps_) {
    desc.PS = {ps_->GetBufferPointer(), ps_->GetBufferSize()};
  }
  desc.BlendState.IndependentBlendEnable = TRUE;
  // Alpha to coverage on MSAA targets, as the guest's RB_COLORCONTROL asks (the SDK does the same).
  desc.BlendState.AlphaToCoverageEnable = (state.alpha_to_mask && context.sample_count > 1) ? TRUE : FALSE;
  bool any_blend = false;
  for (uint32_t i = 0; i < 4; ++i) {
    auto& rt = desc.BlendState.RenderTarget[i];
    if (context.rtv_formats[i] == DXGI_FORMAT_UNKNOWN) {
      // An unbound slot must carry a plain, disabled blend state (blending on an UNKNOWN-format slot
      // is rejected with E_INVALIDARG).
      rt.BlendEnable = FALSE;
      rt.SrcBlend = rt.SrcBlendAlpha = D3D12_BLEND_ONE;
      rt.DestBlend = rt.DestBlendAlpha = D3D12_BLEND_ZERO;
      rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
      rt.RenderTargetWriteMask = 0;
      continue;
    }
    rex::graphics::reg::RB_BLENDCONTROL blend;
    blend.value = state.blendcontrol[i];
    // "one + zero, add" is no blending.
    const bool blend_enabled = !(blend.color_srcblend == rex::graphics::xenos::BlendFactor::kOne &&
                                 blend.color_destblend == rex::graphics::xenos::BlendFactor::kZero &&
                                 blend.color_comb_fcn == rex::graphics::xenos::BlendOp::kAdd &&
                                 blend.alpha_srcblend == rex::graphics::xenos::BlendFactor::kOne &&
                                 blend.alpha_destblend == rex::graphics::xenos::BlendFactor::kZero &&
                                 blend.alpha_comb_fcn == rex::graphics::xenos::BlendOp::kAdd);
    any_blend |= blend_enabled;
    rt.BlendEnable = blend_enabled ? TRUE : FALSE;
    rt.SrcBlend = MapBlendFactor(blend.color_srcblend, false);
    rt.DestBlend = MapBlendFactor(blend.color_destblend, false);
    rt.BlendOp = MapBlendOp(blend.color_comb_fcn);
    rt.SrcBlendAlpha = MapBlendFactor(blend.alpha_srcblend, true);
    rt.DestBlendAlpha = MapBlendFactor(blend.alpha_destblend, true);
    rt.BlendOpAlpha = MapBlendOp(blend.alpha_comb_fcn);
    // RB_COLOR_MASK: four bits per target, red/green/blue/alpha, the D3D12 write-enable bit order.
    rt.RenderTargetWriteMask = UINT8((state.color_mask >> (4 * i)) & 0xF);
  }
  desc.SampleMask = context.sample_mask;
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  // Culling like the SDK's pipeline cache: face == 0 means counter-clockwise front faces; both faces
  // culled is "draw nothing", which GetPipeline reports as no pipeline (the caller then leaves the draw
  // to the emulated path, which drops it the same way).
  if (su.cull_front && su.cull_back) {
    pipelines_.emplace(key, nullptr);
    return nullptr;
  }
  desc.RasterizerState.CullMode = su.cull_front ? D3D12_CULL_MODE_FRONT
                                  : su.cull_back ? D3D12_CULL_MODE_BACK
                                                 : D3D12_CULL_MODE_NONE;
  desc.RasterizerState.FrontCounterClockwise = su.face == 0 ? TRUE : FALSE;
  // Polygon offset like the SDK's pipeline cache (shadow casters depend on it).
  desc.RasterizerState.DepthBias = state.depth_bias;
  desc.RasterizerState.DepthBiasClamp = 0.0f;
  desc.RasterizerState.SlopeScaledDepthBias = state.depth_bias_slope;
  desc.RasterizerState.DepthClipEnable = state.depth_clip ? TRUE : FALSE;
  desc.DepthStencilState.DepthEnable = depth.z_enable ? TRUE : FALSE;
  desc.DepthStencilState.DepthWriteMask = depth.z_write_enable ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
  desc.DepthStencilState.DepthFunc = static_cast<D3D12_COMPARISON_FUNC>(uint32_t(depth.zfunc) + 1);
  desc.DepthStencilState.StencilEnable = FALSE;
  if (depth.stencil_enable) {
    // Stencil ops and functions are the Xenos enums plus one, as in the SDK. Per-face masks do not exist in
    // D3D12; the caller picked the front or back RB_STENCILREFMASK. The stencil reference itself is command
    // list state the SDK already set for this draw.
    rex::graphics::reg::RB_STENCILREFMASK ref_mask;
    ref_mask.value = state.stencil_ref_mask;
    auto& ds = desc.DepthStencilState;
    ds.StencilEnable = TRUE;
    ds.StencilReadMask = UINT8(ref_mask.stencilmask);
    ds.StencilWriteMask = UINT8(ref_mask.stencilwritemask);
    ds.FrontFace.StencilFailOp = D3D12_STENCIL_OP(uint32_t(D3D12_STENCIL_OP_KEEP) + uint32_t(depth.stencilfail));
    ds.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP(uint32_t(D3D12_STENCIL_OP_KEEP) + uint32_t(depth.stencilzfail));
    ds.FrontFace.StencilPassOp = D3D12_STENCIL_OP(uint32_t(D3D12_STENCIL_OP_KEEP) + uint32_t(depth.stencilzpass));
    ds.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC(uint32_t(D3D12_COMPARISON_FUNC_NEVER) + uint32_t(depth.stencilfunc));
    if (depth.backface_enable) {
      ds.BackFace.StencilFailOp = D3D12_STENCIL_OP(uint32_t(D3D12_STENCIL_OP_KEEP) + uint32_t(depth.stencilfail_bf));
      ds.BackFace.StencilDepthFailOp =
          D3D12_STENCIL_OP(uint32_t(D3D12_STENCIL_OP_KEEP) + uint32_t(depth.stencilzfail_bf));
      ds.BackFace.StencilPassOp = D3D12_STENCIL_OP(uint32_t(D3D12_STENCIL_OP_KEEP) + uint32_t(depth.stencilzpass_bf));
      ds.BackFace.StencilFunc =
          D3D12_COMPARISON_FUNC(uint32_t(D3D12_COMPARISON_FUNC_NEVER) + uint32_t(depth.stencilfunc_bf));
    } else {
      ds.BackFace = ds.FrontFace;
    }
  }
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    desc.RTVFormats[i] = context.rtv_formats[i];
    if (context.rtv_formats[i] != DXGI_FORMAT_UNKNOWN) desc.NumRenderTargets = i + 1;
  }
  desc.DSVFormat = context.dsv_format;
  if (desc.DSVFormat == DXGI_FORMAT_UNKNOWN) {
    desc.DepthStencilState.DepthEnable = FALSE;
    desc.DepthStencilState.StencilEnable = FALSE;
  }
  desc.SampleDesc.Count = context.sample_count;

  // Everything the driver needs is by value in `desc` now, bar the root signature and the two shader
  // blobs, which this object owns for its whole life. Hand it to a worker: at about 300 ms apiece these
  // cost more than a frame each, and doing them here froze a play session for half its length.
  if (pending_pipelines_.size() >= MaxPipelinesInFlight()) {
    return nullptr;  // stays emulated until a slot frees up
  }
  const bool cull_front = su.cull_front != 0;
  const bool cull_back = su.cull_back != 0;
  pending_pipelines_.emplace(
      key, std::async(std::launch::async, [this, desc, key_string = std::string(key_text), any_blend,
                                           cull_front, cull_back]() {
        Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
        const HRESULT pso_hr = device_->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
        if (FAILED(pso_hr)) {
          if (failures_.fetch_add(1, std::memory_order_relaxed) < 8) {
            REXLOG_ERROR("rexgpu-nb: geometry pass '{}': pipeline creation failed (0x{:08X}) for key {}",
                         name_, static_cast<uint32_t>(pso_hr), key_string);
            // Bisect without the debug layer (not installed on this machine): retry with parts of the
            // state reverted to the plain form and report the first variant the runtime accepts.
            struct Variant {
              const char* name;
              void (*apply)(D3D12_GRAPHICS_PIPELINE_STATE_DESC&);
            };
            const Variant variants[] = {
                {"blend: RT0 only",
                 [](D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) { d.BlendState.IndependentBlendEnable = FALSE; }},
                {"+ write mask all", [](D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) {
                   for (auto& rt : d.BlendState.RenderTarget) rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
                 }},
                {"+ cull none", [](D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) {
                   d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                   d.RasterizerState.FrontCounterClockwise = FALSE;
                 }},
                {"+ depth off", [](D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) {
                   d.DepthStencilState.DepthEnable = FALSE;
                   d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
                 }},
                {"+ no pixel shader", [](D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) { d.PS = {}; }},
            };
            D3D12_GRAPHICS_PIPELINE_STATE_DESC trial = desc;
            for (const Variant& variant : variants) {
              variant.apply(trial);
              Microsoft::WRL::ComPtr<ID3D12PipelineState> probe;
              const HRESULT trial_hr = device_->CreateGraphicsPipelineState(&trial, IID_PPV_ARGS(&probe));
              REXLOG_ERROR("rexgpu-nb:   variant '{}': 0x{:08X}", variant.name, static_cast<uint32_t>(trial_hr));
              if (SUCCEEDED(trial_hr)) break;
            }
            // With the debug layer on (--d3d12_debug=true) the validation message says which rule broke.
            Microsoft::WRL::ComPtr<ID3D12InfoQueue> info_queue;
            if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&info_queue)))) {
              const UINT64 count = info_queue->GetNumStoredMessages();
              for (UINT64 i = count > 4 ? count - 4 : 0; i < count; ++i) {
                SIZE_T length = 0;
                info_queue->GetMessage(i, nullptr, &length);
                std::string storage(length, '\0');
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                if (length && SUCCEEDED(info_queue->GetMessage(i, message, &length))) {
                  REXLOG_ERROR("rexgpu-nb:   d3d12: {}", message->pDescription ? message->pDescription : "?");
                }
              }
            }
          }
          // Null caches as "known bad, never retry", which is what the synchronous path did.
          return Microsoft::WRL::ComPtr<ID3D12PipelineState>();
        }
        REXLOG_INFO("rexgpu-nb: geometry pass '{}': pipeline for {} (blend {}, cull {})", name_, key_string,
                    any_blend ? "on" : "off", cull_front ? "front" : cull_back ? "back" : "none");
        return pipeline;
      }));
  if (normalize_key) ++g_pipeline_key_stats.create_requests;
  return nullptr;
}

uint32_t NativeGeometryPass::MaxPipelinesInFlight() {
  // Same shape as the shader library's compile cap: leave the command processor and the guest render
  // thread room to run while these grind.
  const uint32_t threads = std::thread::hardware_concurrency();
  return threads > 4 ? threads / 2 : 2;
}

void NativeGeometryPass::ReapPipelines() {
  for (auto it = pending_pipelines_.begin(); it != pending_pipelines_.end();) {
    if (it->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
      ++it;
      continue;
    }
    pipelines_.emplace(it->first, it->second.get());
    it = pending_pipelines_.erase(it);
  }
}

bool NativeGeometryPass::Record(D3D12CommandProcessor& cp, const D3D12CommandProcessor::NativeDrawContext& context,
                                const GuestState& guest_state, const RootConstants& constants, const DrawArgs& args) {
  const bool observe_vertex_packet = REXCVAR_GET(nb_native_vertex_packet_diagnostics);
  const bool vertex_packet_eligible = args.asset_cache == nullptr;
  const bool vertex_reuse_requested = REXCVAR_GET(nb_native_vertex_constant_reuse);
  const bool reuse_vertex_constants = vertex_reuse_requested && vertex_packet_eligible;
  if (!reuse_vertex_constants) vertex_constant_reuse_.Reset();
  if (vertex_reuse_requested && !vertex_packet_eligible) ++g_constant_reuse_stats.vs_constant_bypasses;
  NativeVertexPacketDiagnostics::Attempt vertex_packet_attempt(
      vertex_packet_diagnostics_, observe_vertex_packet, vertex_packet_eligible,
      g_constant_reuse_stats.vs_packet_bypasses);
  // Compile-time elimination is valid only when the original vertex packet
  // retains all-zero asset bindings. Guard before any resource or command work.
  if (direct_guest_reads_ && args.asset_cache) {
    ++g_constant_reuse_stats.direct_guest_refusals;
    return false;
  }
  if (!initialized_ || args.host_vertex_count == 0) {
    return false;
  }
  // Snapshot once for every stage/packet path. Missing metadata and handwritten
  // zero counts keep the full256 contract; only explicit generated emptiness
  // can omit floats. The compiled shader is unchanged and has no such reads.
  const bool empty_layouts = REXCVAR_GET(nb_native_empty_constant_layout);
  const bool request_empty_vs = empty_layouts && args.vs_has_packed_layout && !args.vs_constant_count;
  const bool request_empty_ps = empty_layouts && args.ps_has_packed_layout && !args.ps_constant_count;
  if ((request_empty_vs && args.vs_constant_run_count) || (request_empty_ps && args.ps_constant_run_count)) return false;
  const bool empty_vs = request_empty_vs && empty_vertex_buffer_safe_;
  const bool empty_ps = request_empty_ps && empty_pixel_buffer_safe_;
  g_constant_reuse_stats.empty_layout_bypasses +=
      (request_empty_vs && !empty_vs) + (request_empty_ps && !empty_ps);
  const bool detailed = !REXCVAR_GET(nb_native_minimal_diagnostics);
  auto* preparations = REXCVAR_GET(nb_native_constant_upload_diagnostics)
      ? &GetNativeConstantPreparationStats() : nullptr;
  // Snapshot once: pipeline identity, upload and root writes must agree even if
  // a diagnostic switch changes between draws. Both signatures outlive PSO workers.
  const bool root_cbv = REXCVAR_GET(nb_native_root_cbv);
  const bool constant_binding_packet = root_cbv && REXCVAR_GET(nb_native_constant_binding_packet);
  ID3D12RootSignature* draw_root_signature = root_cbv ? root_cbv_signature_.Get() : root_signature_.Get();
  if (!draw_root_signature) {
    return false;
  }
  auto stage_start = NativeTimingStart(detailed);
  ID3D12PipelineState* pipeline = GetPipeline(context, guest_state, root_cbv);
  g_timings.pipeline_ns += NativeTimingElapsed(detailed, stage_start);
  if (!pipeline) {
    return false;
  }
  stage_start = NativeTimingStart(detailed);
  uint32_t texture_samplers[kTextureSlots] = {};
  if (args.sampler_slot_count) {
    uint32_t indices[kTextureSlots];
    if (!cp.RequestSamplerBindlessIndices(args.sampler_parameters, args.sampler_slot_count, indices)) {
      return false;
    }
    for (uint32_t i = 0; i < args.sampler_slot_count; ++i) {
      // Both legacy selectors use this slot's effective sampler. Resolve it once, then
      // duplicate the index; the SDK's 2000-descriptor heap fits in each 16-bit half.
      texture_samplers[args.sampler_slots[i]] = indices[i] | (indices[i] << 16);
    }
  }
  // One constant buffer per stage from the upload pool: bools, texture swizzles and sampler indices
  // both stages read, then that stage's float constants (prelude.hlsl NbVsConstants / NbPsConstants).
  // This is write-combined memory, where a single contiguous write is worth many times its size in
  // scattered ones, so each buffer is exactly as large as the shader's packed constants.
  constexpr size_t kBoolBytes = 8 * 4;
  constexpr size_t kSwizzleBytes = kTextureSlots * 4;  // one dword a slot, padded to the uint4 array
  constexpr size_t kSamplerBytes = kTextureSlots * 4;
  constexpr size_t kSharedBytes = kBoolBytes + kSwizzleBytes + kSamplerBytes;
  static_assert(kSharedBytes == NativePixelConstantPacket::kHeaderBytes);
  static_assert(kSwizzleBytes == NativePixelConstantPacket::kSlotBytes);
  static_assert(kSamplerBytes == NativePixelConstantPacket::kSlotBytes);
  constexpr size_t kExtraStreamBytes = sizeof(args.extra_vertex_streams);
  constexpr size_t kAssetBindingBytes = sizeof(NativeAssetCache::Binding) * (kMaxVertexStreams + 1);
  static_assert(sizeof(NativeAssetCache::Binding) == 16);
  static_assert(kSharedBytes + kExtraStreamBytes + kAssetBindingBytes == kVertexConstantHeaderBytes);
  uint8_t* asset_bindings_upload = nullptr;
  uint8_t* staged_vertex_upload = nullptr;
  size_t staged_vertex_bytes = 0;
  const uint8_t* complete_vertex_packet = nullptr;
  size_t complete_vertex_packet_bytes = 0;
  D3D12_GPU_VIRTUAL_ADDRESS cb_address[2] = {};
  const bool reuse_constants = REXCVAR_GET(nb_native_constant_reuse);
  const bool alias_unused_pixel = reuse_constants && ps_ && pixel_buffer_unused_ &&
      REXCVAR_GET(nb_native_unused_pixel_constants);
  const bool stage_vertex_constants = REXCVAR_GET(nb_native_stage_vertex_constants) ||
                                      (observe_vertex_packet && vertex_packet_eligible);
  const bool double_pixel_packets = REXCVAR_GET(nb_native_double_pixel_packets);
  if (!reuse_constants) ps_constant_reuse_.Reset();
  // All comparisons are against ordinary RAM. Mapped upload pages are write-
  // combined and immutable after publication; never read them for cache checks.
  // These are logical RAM packet builds/bytes skipped, not measured upload or
  // pool savings: the original exact PS memo may already have reused an address.
  if (alias_unused_pixel) {
    ++g_constant_reuse_stats.unused_ps_packet_builds_skipped;
    g_constant_reuse_stats.unused_ps_packet_bytes_skipped += kSharedBytes +
        size_t(NativeConstantRegisters(args.ps_constant_count, empty_ps)) * 16;
  }
  NativePixelConstantPacket pixel_packet;
  const bool have_pixel_packet = [&] {
    NativeConstantTimer timer(preparations && reuse_constants && ps_ && !alias_unused_pixel
        ? &preparations->ram_fill : nullptr);
    return reuse_constants && ps_ && !alias_unused_pixel && (double_pixel_packets
      ? ps_constant_reuse_.BuildCandidate(args.bool_constants, args.texture_swizzle, texture_samplers,
          args.ps_constants, args.ps_constant_runs, args.ps_constant_run_count, args.ps_constant_count, empty_ps)
      : pixel_packet.Build(args.bool_constants, args.texture_swizzle, texture_samplers,
          args.ps_constants, args.ps_constant_runs, args.ps_constant_run_count, args.ps_constant_count, empty_ps));
  }();
  const auto pixel_bytes = have_pixel_packet
      ? (double_pixel_packets ? ps_constant_reuse_.candidate_bytes() : pixel_packet.bytes())
      : std::span<const uint8_t>{};
  auto upload_stage = [&](const float* source, const ConstantRun* runs, size_t run_count, uint32_t packed,
                          bool explicit_empty, bool vertex, D3D12_GPU_VIRTUAL_ADDRESS& address_out) {
    const size_t float_bytes = size_t(NativeConstantRegisters(packed, explicit_empty)) * 16;
    const size_t header_bytes = kSharedBytes + (vertex ? kExtraStreamBytes + kAssetBindingBytes : 0);
    const size_t packet_bytes = header_bytes + float_bytes;
    if (explicit_empty) {
      if (vertex) ++g_constant_reuse_stats.empty_vs_packets;
      else ++g_constant_reuse_stats.empty_ps_packets;
      g_constant_reuse_stats.empty_constant_bytes_avoided += 256 * 16;
      g_constant_reuse_stats.empty_constant_reserved_bytes_avoided +=
          ((header_bytes + 256 * 16 + 255) & ~size_t(255)) -
          ((packet_bytes + 255) & ~size_t(255));
    }
    // RAM staging and exact reuse require proof that every byte will be filled.
    // Keep the original direct upload for any layout outside this proof.
    bool complete_vertex_layout = false;
    if (vertex && (stage_vertex_constants || reuse_vertex_constants)) {
      const size_t registers = NativeConstantRegisters(packed, explicit_empty);
      complete_vertex_layout = registers <= 256;
      if (complete_vertex_layout && source && runs && run_count) {
        size_t total = 0;
        for (size_t i = 0; i < run_count; ++i) {
          const size_t first = runs[i].first, count = runs[i].count;
          if (first > 256 || count > 256 - first || count > registers - total) {
            complete_vertex_layout = false;
            break;
          }
          total += count;
        }
        complete_vertex_layout = complete_vertex_layout && total == registers;
      }
    }
    const bool reuse_vertex_packet = vertex && reuse_vertex_constants && complete_vertex_layout;
    if (vertex && reuse_vertex_constants && !complete_vertex_layout) {
      vertex_constant_reuse_.Reset();
      ++g_constant_reuse_stats.vs_constant_bypasses;
    }
    if (!vertex && have_pixel_packet) {
      {
        NativeConstantTimer timer(preparations ? &preparations->memo : nullptr);
        address_out = ps_constant_reuse_.Find(cp.GetCurrentFrame(), pixel_bytes);
      }
      if (address_out) {
        ++g_constant_reuse_stats.ps_hits;
        g_constant_reuse_stats.ps_bytes_avoided += header_bytes + float_bytes;
        g_constant_reuse_stats.ps_reserved_bytes_avoided += (header_bytes + float_bytes + 255) & ~size_t(255);
        return true;
      }
      ++g_constant_reuse_stats.ps_misses;
    } else if (!vertex && reuse_constants) {
      ++g_constant_reuse_stats.ps_bypasses;
      ps_constant_reuse_.Reset();
    }
    ID3D12Resource* buffer = nullptr;
    size_t offset = 0;
    // Tagged with the frame, not the submission: the command processor reclaims this pool with
    // Reclaim(frame_completed_), so a page stamped with a submission index is never old enough to be
    // freed and the pool grows by every draw's constants for the whole run.
    uint8_t* memory = reuse_vertex_packet ? vertex_constant_reuse_.candidate_data()
        : RequestConstantSlice(cp, args.constant_pool, cp.GetCurrentFrame(), packet_bytes,
                               &buffer, &offset, &address_out, preparations);
    if (!memory) {
      return false;
    }
    if (!vertex && have_pixel_packet) {
      const auto packet = pixel_bytes;
      {
        NativeConstantTimer timer(preparations ? &preparations->mapped_fill : nullptr);
        std::memcpy(memory, packet.data(), packet.size());
      }
      // Request tags with GetCurrentFrame. Reclaim happens only at frame open;
      // ClearCache happens after frame_current_ increments on frame close.
      // A different frame therefore cannot reuse a reclaimed upload address.
      NativeConstantTimer memo_timer(preparations ? &preparations->memo : nullptr);
      if (double_pixel_packets) {
        if (ps_constant_reuse_.PublishCandidate(cp.GetCurrentFrame(), address_out)) {
          ++g_constant_reuse_stats.ps_packet_publications;
          g_constant_reuse_stats.ps_packet_copy_bytes_avoided += packet.size();
        }
      } else {
        ps_constant_reuse_.Publish(cp.GetCurrentFrame(), packet, address_out);
      }
      return true;
    }
    if (vertex && stage_vertex_constants && !reuse_vertex_packet) {
      // Only redirect layouts that fill every byte of the bounded scratch
      // packet. In particular, an incomplete run list must not copy old RAM
      // contents into a region the original path left untouched. Retain the
      // existing direct path for any layout we cannot prove here.
      if (complete_vertex_layout) {
        staged_vertex_upload = memory;
        staged_vertex_bytes = header_bytes + float_bytes;
        memory = vertex_constant_stage_.data();
      } else {
        ++g_constant_reuse_stats.vs_stage_bypasses;
      }
    }
    {
      // One scope per fill group, not per register run. A staged/reuse candidate
      // writes ordinary RAM; the direct path writes the newly allocated mapping.
      NativeConstantTimer timer(preparations
          ? (vertex && (reuse_vertex_packet || staged_vertex_upload) ? &preparations->ram_fill
                                                         : &preparations->mapped_fill) : nullptr);
      if (args.bool_constants) std::memcpy(memory, args.bool_constants, kBoolBytes);
      else std::memset(memory, 0, kBoolBytes);
      std::memcpy(memory + kBoolBytes, args.texture_swizzle, kSwizzleBytes);
      std::memcpy(memory + kBoolBytes + kSwizzleBytes, texture_samplers, kSamplerBytes);
      if (vertex) {
        std::memcpy(memory + kSharedBytes, args.extra_vertex_streams, kExtraStreamBytes);
        asset_bindings_upload = memory + kSharedBytes + kExtraStreamBytes;
        std::memset(asset_bindings_upload, 0, kAssetBindingBytes);
      }
      uint8_t* constants = memory + header_bytes;
      if (!float_bytes) {
        // Header-only packets never access the guest float source or run list.
      } else if (!source) {
        std::memset(constants, 0, float_bytes);
      } else if (!runs || run_count == 0) {
        std::memcpy(constants, source, float_bytes);
      } else {
        // The translated shader reads these spans renumbered from zero, back to back.
        size_t written = 0;
        for (size_t i = 0; i < run_count; ++i) {
          const size_t bytes = size_t(runs[i].count) * 16;
          std::memcpy(constants + written, reinterpret_cast<const uint8_t*>(source) + size_t(runs[i].first) * 16,
                      bytes);
          written += bytes;
        }
      }
    }
    if (vertex && (reuse_vertex_packet || staged_vertex_upload)) {
      complete_vertex_packet = memory;
      complete_vertex_packet_bytes = packet_bytes;
    }
    if (reuse_vertex_packet) {
      static_assert(kVertexConstantHeaderBytes + 256 * 16 == NativeVertexConstantReuse::kCapacity);
      const uint64_t packet_frame = cp.GetCurrentFrame();
      {
        NativeConstantTimer timer(preparations ? &preparations->memo : nullptr);
        address_out = vertex_constant_reuse_.Find(packet_frame, packet_bytes);
      }
      if (address_out) {
        ++g_constant_reuse_stats.vs_constant_hits;
        g_constant_reuse_stats.vs_constant_bytes_avoided += packet_bytes;
        g_constant_reuse_stats.vs_constant_reserved_bytes_avoided += (packet_bytes + 255) & ~size_t(255);
        return true;
      }
      ++g_constant_reuse_stats.vs_constant_misses;
      uint8_t* upload = RequestConstantSlice(cp, args.constant_pool, packet_frame, packet_bytes,
                                             &buffer, &offset, &address_out, preparations);
      if (!upload) {
        vertex_constant_reuse_.Reset();
        return false;
      }
      // Publish only after this NEW current-frame slice contains the complete
      // candidate. The ownership swap avoids a second RAM snapshot copy.
      {
        NativeConstantTimer timer(preparations ? &preparations->mapped_fill : nullptr);
        std::memcpy(upload, memory, packet_bytes);
      }
      NativeConstantTimer memo_timer(preparations ? &preparations->memo : nullptr);
      if (!vertex_constant_reuse_.Publish(packet_frame, packet_bytes, address_out)) {
        vertex_constant_reuse_.Reset();
        return false;
      }
    }
    return true;
  };
  if (!upload_stage(args.vs_constants, args.vs_constant_runs, args.vs_constant_run_count,
                    args.vs_constant_count, empty_vs, true, cb_address[0])) {
    return false;
  }
  cb_address[1] = alias_unused_pixel ? cb_address[0] :
      NativeDepthOnlyPixelAddress(reuse_constants, legacy_depth_only_pixel_buffer_unused_, cb_address[0]);
  if (cb_address[1]) {
    if (alias_unused_pixel) {
      // b1's successfully written slice has the same existing frame/fence
      // lifetime. No shader reads b2, so its arbitrary aliased contents are unused.
      ++g_constant_reuse_stats.unused_ps_aliases;
    } else {
      ++g_constant_reuse_stats.depth_only_aliases;
      const size_t skipped_bytes = kSharedBytes + size_t(NativeConstantRegisters(args.ps_constant_count, empty_ps)) * 16;
      g_constant_reuse_stats.ps_bytes_avoided += skipped_bytes;
      g_constant_reuse_stats.ps_reserved_bytes_avoided += (skipped_bytes + 255) & ~size_t(255);
    }
  } else if (!upload_stage(args.ps_constants, args.ps_constant_runs, args.ps_constant_run_count,
                           args.ps_constant_count, empty_ps, false, cb_address[1])) {
    return false;
  }

  g_timings.constants_ns += NativeTimingElapsed(detailed, stage_start);
  stage_start = NativeTimingStart(detailed);

  // The guest vertex (and index) ranges must be resident in shared memory before the buffer is read.
  // Asked for together: this is by far the most expensive thing a native draw does - 2.66 ms of a town
  // frame's 3.76 ms, with everything else at 0.01 ms - and three separate calls walk the page bitmap
  // three times over.
  auto& shared_memory = cp.shared_memory();
  if (g_residency_watch == nullptr) {
    g_residency_watch = shared_memory.RegisterGlobalWatch(ResidencyInvalidated, nullptr);
  }
  // Shared memory drops the valid bit on every page that was valid only because the CPU uploaded it
  // when a frame closes (clear_memory_page_state, on by default) and that sweep fires no watch, so the
  // bitmap has to go with it or a vertex buffer the CPU has since rewritten survives into the next
  // frame. An empty bitmap can never over-claim, so doing this at the first draw of a frame is safe
  // whichever side of the sweep it lands on.
  const uint64_t frame_index = cp.GetCurrentFrame();
  if (frame_index != g_residency_frame && REXCVAR_GET(clear_memory_page_state)) {
    g_residency.Reset();
    g_residency_frame = frame_index;
    ++g_timings.residency_resets;
  }
  g_timings.residency_resets += DrainResidencyInvalidations();
  std::pair<uint32_t, uint32_t> wanted[kMaxVertexStreams + 1];
  size_t wanted_count = 0;
  const uint32_t stream_count = std::min(args.vertex_stream_count, kMaxVertexStreams);
  for (uint32_t i = 0; i < stream_count; ++i) {
    if (args.fetch_size_bytes[i]) {
      const uint32_t base = i == 0 ? constants.fetch_base_bytes :
                            i == 1 ? constants.fetch1_base_bytes : args.extra_vertex_streams[i - 2][0];
      wanted[wanted_count++] = {base, args.fetch_size_bytes[i]};
    }
  }
  if (constants.index_base_bytes != 0xFFFFFFFFu && args.index_size_bytes) {
    wanted[wanted_count++] = {constants.index_base_bytes, args.index_size_bytes};
  }
  std::pair<uint32_t, uint32_t> ranges[kMaxVertexStreams + 1];
  size_t range_count = 0;
  for (size_t i = 0; i < wanted_count; ++i) {
    if (g_residency.Covers(wanted[i].first, wanted[i].second)) {
      ++g_timings.residency_hits;
    } else {
      ++g_timings.residency_misses;
      ranges[range_count++] = wanted[i];
    }
  }
  if (range_count != 0) {
    if (!shared_memory.RequestRanges(ranges, range_count)) {
      return false;
    }
    // Only cache after the request succeeded, and fold in anything that was invalidated during the
    // call, so a write that landed mid-request is not papered over by what we are about to remember.
    for (size_t i = 0; i < range_count; ++i) g_residency.Add(ranges[i].first, ranges[i].second);
    g_timings.residency_resets += DrainResidencyInvalidations();
  }
  g_timings.residency_ns += NativeTimingElapsed(detailed, stage_start);
  stage_start = NativeTimingStart(detailed);
  D3D12_GPU_VIRTUAL_ADDRESS asset_address = shared_memory.GetGPUAddress();
  if (args.asset_cache && args.asset_cache->initialized() && args.asset_cache->EnsureUploaded(cp)) {
    NativeAssetCache::Binding bindings[kMaxVertexStreams + 1] = {};
    for (uint32_t i = 0; i < kMaxVertexStreams; ++i) {
      if (!args.fetch_size_bytes[i]) continue;
      const uint32_t base = i == 0 ? constants.fetch_base_bytes :
                            i == 1 ? constants.fetch1_base_bytes : args.extra_vertex_streams[i - 2][0];
      bindings[i] = args.asset_cache->Resolve(base, args.fetch_size_bytes[i], frame_index, args.legacy_asset_cache);
    }
    if (constants.index_base_bytes != 0xFFFFFFFFu && args.index_size_bytes) {
      bindings[kMaxVertexStreams] = args.asset_cache->Resolve(
          constants.index_base_bytes, args.index_size_bytes, frame_index, args.legacy_asset_cache);
    }
    {
      NativeConstantTimer timer(preparations
          ? (staged_vertex_upload ? &preparations->ram_fill : &preparations->mapped_fill) : nullptr);
      std::memcpy(asset_bindings_upload, bindings, sizeof(bindings));
    }
    asset_address = args.asset_cache->gpu_address();
  }
  g_timings.asset_match_ns += NativeTimingElapsed(detailed, stage_start);
  if (staged_vertex_upload) {
    const auto copy_start = NativeTimingStart(detailed);
    // The original field copies already captured the guest constants, and the
    // optional asset resolution patched this same RAM packet above. Publish
    // only its payload, leaving upload-pool alignment padding untouched.
    {
      NativeConstantTimer timer(preparations ? &preparations->mapped_fill : nullptr);
      std::memcpy(staged_vertex_upload, vertex_constant_stage_.data(), staged_vertex_bytes);
    }
    ++g_constant_reuse_stats.vs_staged_uploads;
    g_constant_reuse_stats.vs_staged_bytes += staged_vertex_bytes;
    g_timings.constants_ns += NativeTimingElapsed(detailed, copy_start);
  }
  D3D12_GPU_VIRTUAL_ADDRESS root_cb_address = 0;
  if (root_cbv) {
    const auto copy_start = NativeTimingStart(detailed);
    ID3D12Resource* buffer = nullptr;
    size_t offset = 0;
    uint8_t* memory = RequestConstantSlice(cp, args.constant_pool, cp.GetCurrentFrame(),
                                           sizeof(constants), &buffer, &offset, &root_cb_address, preparations);
    if (!memory) {
      return false;
    }
    // The pool keeps this frame's allocation alive until GPU completion. Write
    // every b0 byte once before recording its immutable DATA_STATIC binding.
    {
      NativeConstantTimer timer(preparations ? &preparations->mapped_fill : nullptr);
      std::memcpy(memory, &constants, sizeof(constants));
    }
    g_timings.constants_ns += NativeTimingElapsed(detailed, copy_start);
  }
  const auto use_start = NativeTimingStart(detailed);
  shared_memory.UseForReading();
  g_timings.use_reading_ns += NativeTimingElapsed(detailed, use_start);
  stage_start = NativeTimingStart(detailed);

  // CP helpers own static roots 2/5/3/6 and the PS CBV at root 4. The optional
  // compound packet retains root0/root1/root4 call order and pixel memo policy.
  auto& list = cp.SetNativeGeometryStaticBindings(
      draw_root_signature, cp.GetViewBindlessHeapGPUStart(),
      cp.GetSamplerBindlessHeapGPUStart(), args.sampler_slot_count != 0,
      shared_memory.GetGPUAddress(), asset_address);
  cp.SetExternalPipeline(pipeline);
  if (constant_binding_packet) {
    cp.SetNativeGeometryConstantBuffers(root_cb_address, cb_address[0], cb_address[1]);
  } else {
    if (root_cbv) {
      list.D3DSetGraphicsRootConstantBufferView(0, root_cb_address);
    } else {
      list.D3DSetGraphicsRoot32BitConstants(0, kConstantDwords, &constants, 0);
    }
    list.D3DSetGraphicsRootConstantBufferView(1, cb_address[0]);
    cp.SetNativePixelConstantBuffer(cb_address[1]);
  }
  cp.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  cp.SubmitBarriers();
  const uint32_t gpu_draw_slot = cp.BeginNativeGpuPass("native_geometry", args.host_vertex_count);
  if (args.use_hardware_indices) {
    // Shared memory is already resident and in INDEX_BUFFER | shader-read state.
    // Keep IA BaseVertex zero: the shader swaps the raw index before applying
    // the guest offset, 24-bit mask and unsigned clamp. Asset bindings do not
    // redirect this IB to the optional immutable vertex arena.
    D3D12_INDEX_BUFFER_VIEW indices;
    indices.BufferLocation = shared_memory.GetGPUAddress() + constants.index_base_bytes;
    indices.SizeInBytes = args.index_size_bytes;
    indices.Format = constants.index_format ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    list.D3DIASetIndexBuffer(&indices);
    list.D3DDrawIndexedInstanced(args.host_vertex_count, 1, 0, 0, 0);
    ++g_index_draw_stats.hardware_draws;
    g_index_draw_stats.hardware_indices += args.host_vertex_count;
  } else {
    list.D3DDrawInstanced(args.host_vertex_count, 1, 0, 0);
    if (args.hardware_index_eligible) {
      ++g_index_draw_stats.manual_eligible_draws;
      g_index_draw_stats.manual_eligible_indices += args.host_vertex_count;
    }
  }
  cp.EndNativeGpuPass(gpu_draw_slot);
  if (root_cbv) {
    ++g_constant_reuse_stats.root_cbv_draws;
    g_constant_reuse_stats.root_cbv_bytes += sizeof(constants);
  }
  if (direct_guest_reads_) ++g_constant_reuse_stats.direct_guest_draws;
  // Compare only the complete ordinary-RAM packet after a successful draw was
  // recorded. The diagnostic never changes its upload address or root binding.
  if (observe_vertex_packet && vertex_packet_eligible && complete_vertex_packet) {
    static_assert(kVertexConstantHeaderBytes + 256 * 16 == NativeVertexPacketDiagnostics::kCapacity);
    const auto observation = vertex_packet_attempt.Complete(
        cp.GetCurrentFrame(), {complete_vertex_packet, complete_vertex_packet_bytes});
    g_constant_reuse_stats.vs_packet_observations += observation.observed;
    g_constant_reuse_stats.vs_packet_hits += observation.hit;
    g_constant_reuse_stats.vs_packet_equal_bytes += observation.equal_bytes;
    g_constant_reuse_stats.vs_packet_pass_frames += observation.new_pass_frame;
  }
  g_timings.record_ns += NativeTimingElapsed(detailed, stage_start);
  const std::string& census_path = REXCVAR_GET(nb_native_vertex_range_census_file);
  if (!census_path.empty()) {
    static NativeVertexRangeCensus census;
    census.Record(census_path, cp.GetCurrentFrame(), REXCVAR_GET(nb_native_vertex_range_census_first),
                  REXCVAR_GET(nb_native_vertex_range_census_last), REXCVAR_GET(nb_native_vertex_range_census_period),
                  name_, shared_memory, constants, args);
  }
  return true;
}

}  // namespace nb::gpu
