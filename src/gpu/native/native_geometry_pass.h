// nb - a native geometry pass: our own vertex and pixel shaders drawing the guest's own vertex data.
//
// The vertex shader reads the guest vertex buffer straight out of the SDK's shared-memory buffer (the
// 512 MB mirror of guest physical memory, bound as a raw ByteAddressBuffer) with an in-shader byte swap,
// exactly the way the plan's native scene renderer will read meshes. Guest shader constants travel in a
// per-draw constant buffer from the SDK's upload pool, so a transcribed (or generated, see
// native_shader_library.h) guest shader can use c0..c255 and b0..b255 by name. The pipeline state mirrors
// the guest's blend, depth, cull and colour-mask registers for the bound targets, so the pass composites
// and depth-tests like the emulated draw it replaces.

#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <atomic>
#include <future>

#include <d3d12.h>
#include <wrl/client.h>

#include <rex/graphics/d3d12/command_processor.h>

#include "native_constant_reuse.h"
#include "native_vertex_packet_diagnostics.h"
#include "native_vertex_constant_reuse.h"

namespace nb::gpu {

class NativeAssetCache;
class NativeConstantUploadPool;

class NativeGeometryPass {
 public:
  // Root constants (b0, all stages), 52 dwords; the layout is shared with the HLSL side
  // (src/gpu/native/shaders/prelude.hlsl, cbuffer NbRoot), 16-byte slots throughout because a float3
  // straddling a register boundary would be moved by the compiler and silently misalign everything after.
  // With two CBVs, two tables and two root SRVs that is 62 of the 64 available dwords.
  static constexpr uint32_t kConstantDwords = 52;
  // Texture slots: the pixel shader's fetch constants take 0..15, the vertex shader's 16..19 (this
  // game's shaders use up to 14 and 4 of them respectively).
  static constexpr uint32_t kTextureSlots = 20;
  static constexpr uint32_t kVertexTextureSlotBase = 16;
  static constexpr uint32_t kMaxVertexStreams = 16;
  struct RootConstants {
    uint32_t fetch_base_bytes;    // stream 0: guest byte address of the vertex buffer (shared memory offset)
    uint32_t fetch_stride_dwords;
    uint32_t fetch_endian;        // xenos::Endian
    uint32_t primitive_mode;      // 0 triangles, 1 quads, 2 points, 3 non-indexed strip, 4 hardware indices
    uint32_t fetch1_base_bytes;   // stream 1 (second vertex fetch constant), same fields
    uint32_t fetch1_stride_dwords;
    uint32_t fetch1_endian;
    uint32_t index_offset;        // VGT_INDX_OFFSET, added to every guest vertex index
    float ndc_scale[3];
    uint32_t vertex_index_min;    // occupies ndc_scale.w on the HLSL side, as unsigned bits
    float ndc_offset[3];
    uint32_t vertex_index_max;    // occupies ndc_offset.w; viewport mapping still uses only xyz
    uint32_t texture_index[kTextureSlots];  // shader-side bindless indices by slot the pass defines
    uint32_t index_base_bytes;    // guest index buffer, or 0xFFFFFFFF when the draw is not indexed
    uint32_t index_format;        // 0 = 16-bit, 1 = 32-bit
    uint32_t index_endian;        // xenos::Endian of the index buffer
    uint32_t param_gen;           // bit 8 = SQ_PROGRAM_CNTL.param_gen, bits 0..7 = param_gen_pos,
                                  // bits 16..19 / 20..23 = draw resolution scale x / y - 1,
                                  // bit 24 = fetch offsets count guest texels on scaled textures
    uint32_t sampler_sel[4];      // 4 bits per slot: bit1 = point, bit2 = base mip only
    uint32_t alpha_test[4];       // [0] RB_COLORCONTROL alpha_func | alpha_test_enable << 3, [1] RB_ALPHA_REF bits
    uint32_t pass_params[4];      // pass-specific (the DOF pass keeps its mask channel in [0])
  };
  static_assert(sizeof(RootConstants) == kConstantDwords * 4);

  // The HLSL declarations every pass shader starts with (the embedded prelude.hlsl).
  static const char* kCommonHlsl();

  // Guest state that selects the pipeline.
  struct GuestState {
    uint32_t blendcontrol[4];  // RB_BLENDCONTROL0..3
    uint32_t depthcontrol;     // RB_DEPTHCONTROL, normalized like the SDK does (draw_util::GetNormalizedDepthControl)
    uint32_t su_mode_cntl;     // PA_SU_SC_MODE_CNTL (cull, front face)
    uint32_t color_mask;       // RB_COLOR_MASK
    uint32_t stencil_ref_mask; // RB_STENCILREFMASK (or _BF when only back faces draw): read/write masks
    int32_t depth_bias;        // D3D10-style integer polygon offset (draw_util::GetD3D10IntegerPolygonOffset)
    float depth_bias_slope;    // slope-scaled bias in subpixel units, already scaled by the resolution scale
    bool depth_clip;           // !PA_CL_CLIP_CNTL.clip_disable
    bool alpha_to_mask;        // RB_COLORCONTROL.alpha_to_mask_enable (foliage relies on it with MSAA)
  };

  // A span of guest float4 constant registers a shader reads.
  struct ConstantRun {
    uint16_t first;
    uint16_t count;
  };

  // Everything Record() needs beyond the root constants and the pipeline state.
  struct DrawArgs {
    uint32_t fetch_size_bytes[kMaxVertexStreams] = {};  // per stream, for shared-memory residency
    // Populated prefix, including zero-sized streams. Other callers keep the
    // conservative full scan until they can prove a smaller initialized prefix.
    uint32_t vertex_stream_count = kMaxVertexStreams;
    // Streams 0/1 remain in root constants for the original passes. Additional
    // streams travel in the vertex constant buffer: base, stride, endian, reserved.
    uint32_t extra_vertex_streams[kMaxVertexStreams - 2][4] = {};
    uint32_t index_size_bytes = 0;  // guest index extent before primitive expansion
    bool hardware_index_eligible = false;  // exact direct DMA triangle-list agreement, before diagnostic toggle
    bool use_hardware_indices = false;
    NativeAssetCache* asset_cache = nullptr;
    bool legacy_asset_cache = false;  // diagnostic baseline for same-run comparisons
    // Optional native-only constant pool in CPU-writable video memory. Null, or a request it cannot
    // serve, keeps the command processor's own upload pool; the bytes and their lifetime are the
    // same either way (native_gpu_upload_pool.h).
    NativeConstantUploadPool* constant_pool = nullptr;
    uint32_t host_vertex_count = 0;
    const float* vs_constants = nullptr;      // 256 float4 (null: zeros)
    const float* ps_constants = nullptr;      // 256 float4 (null: zeros)
    const uint32_t* bool_constants = nullptr; // 8 dwords (null: zeros)
    // Empty remains the whole256 unless explicit presence, the diagnostic flag,
    // and linked-bytecode reflection all permit a header-only generated packet.
    const ConstantRun* vs_constant_runs = nullptr;
    size_t vs_constant_run_count = 0;
    const ConstantRun* ps_constant_runs = nullptr;
    size_t ps_constant_run_count = 0;
    // Packed counts matching the runs (legacy0 = whole256, explicit empty0 = no float reads).
    uint32_t vs_constant_count = 0;
    uint32_t ps_constant_count = 0;
    bool vs_has_packed_layout = false;  // generated cruns present; distinguishes explicit empty
    bool ps_has_packed_layout = false;
    uint32_t texture_swizzle[kTextureSlots] = {};  // fetch constant swizzles per slot (0x688 = identity)
    // Low/high legacy sampler indices for each used slot (now the same SDK-resolved descriptor).
    // Resolve both stages in one request because heap rollover invalidates all prior indices.
    // The hand-written passes use static samplers instead.
    uint32_t sampler_slot_count = 0;
    uint32_t sampler_slots[kTextureSlots] = {};
    rex::graphics::d3d12::D3D12TextureCache::SamplerParameters sampler_parameters[kTextureSlots * 2];
  };

  // Compiles the prelude plus one shader body (thread-safe, no device needed): the slow half of
  // Initialize, so a library can run it in the background. Null on failure.
  // `interpolators`, when non-zero, defines NB_INTERPOLATORS for the compile (vertex shaders only).
  // `constants` sizes the stage's constant buffer (NB_VS_CONSTANTS / NB_PS_CONSTANTS); 0 means all 256.
  // `effective_samplers` requires SDK-resolved duplicate sampler indices and cleared sampler_sel.
  static Microsoft::WRL::ComPtr<ID3DBlob> CompileShader(const char* name, const std::string& hlsl, bool vertex,
                                                        uint32_t interpolators = 0, uint32_t constants = 0,
                                                        bool effective_samplers = false,
                                                        bool direct_guest_reads = false);
  // Both forms declare the same registers; root_cbv changes only b0's binding.
  static Microsoft::WRL::ComPtr<ID3D12RootSignature> CreateRootSignature(ID3D12Device* device,
                                                                       bool root_cbv = false);

  // Compiles both stages and creates both root signatures (the hand-written passes).
  bool Initialize(ID3D12Device* device, const char* name, const std::string& vs_hlsl, const std::string& ps_hlsl);
  // Takes compiled shaders and retained shared signatures. `ps` may be null for depth-only.
  // A missing optional CBV signature refuses only draws requesting that variant.
  bool Initialize(ID3D12Device* device, const char* name, ID3DBlob* vs, ID3DBlob* ps,
                  ID3D12RootSignature* root_signature, ID3D12RootSignature* root_cbv_signature = nullptr,
                  bool direct_guest_reads = false);
  bool initialized() const { return initialized_; }

  bool Record(rex::graphics::d3d12::D3D12CommandProcessor& cp,
              const rex::graphics::d3d12::D3D12CommandProcessor::NativeDrawContext& context,
              const GuestState& guest_state, const RootConstants& constants, const DrawArgs& args);

  // Accumulate nanoseconds before converting to milliseconds, avoiding per-draw truncation.
  struct Timings {
    uint64_t pipeline_ns = 0;   // pipeline lookup and creation
    uint64_t constants_ns = 0;  // upload pool request and the guest constant copy
    uint64_t residency_ns = 0;  // shared-memory residency requests
    uint64_t asset_match_ns = 0;  // immutable asset matching, separate from residency
    uint64_t record_ns = 0;     // root constant writes and the draw call
    uint64_t textures_ns = 0;   // resolving each fetch constant to a bindless index, in PrepareGeometry
    uint64_t texture_request_ns = 0;  // SDK RequestTextures, including any uploads
    uint64_t use_reading_ns = 0;  // shared-memory UseForReading
    uint64_t residency_hits = 0;    // ranges already resident, so no call was made
    uint64_t residency_misses = 0;  // ranges that had to be requested
    uint64_t residency_resets = 0;  // cache drops caused by a shared-memory invalidation
  };
  static Timings& timings();

  // Lifetime totals on the command processor thread; independent of the
  // periodic timing reset so benchmark callers can take interval differences.
  struct ConstantReuseStats {
    uint64_t ps_hits = 0, ps_misses = 0, ps_bypasses = 0;
    uint64_t ps_bytes_avoided = 0, ps_reserved_bytes_avoided = 0;  // upload writes / aligned pool space
    uint64_t depth_only_aliases = 0;
    // Present-PS experiment: preparation attempts and logical RAM bytes, not
    // actual mapped-write savings or the older no-PS alias count.
    uint64_t unused_ps_packet_builds_skipped = 0, unused_ps_packet_bytes_skipped = 0;
    uint64_t unused_ps_aliases = 0;
    uint64_t pipeline_hits = 0, pipeline_misses = 0;
    uint64_t vs_staged_uploads = 0, vs_staged_bytes = 0;
    uint64_t vs_stage_bypasses = 0;  // enabled, but layout cannot prove a complete bounded packet
    uint64_t ps_packet_publications = 0, ps_packet_copy_bytes_avoided = 0;
    uint64_t root_cbv_draws = 0, root_cbv_bytes = 0;  // successful draws / immutable b0 payload bytes
    uint64_t direct_guest_draws = 0, direct_guest_refusals = 0;
    uint64_t vs_packet_observations = 0, vs_packet_hits = 0, vs_packet_equal_bytes = 0;
    uint64_t vs_packet_bypasses = 0;  // enabled Record attempts without a complete eligible observation
    uint64_t vs_packet_pass_frames = 0;  // observed frame transitions per pass, not global rendered frames
    uint64_t vs_constant_hits = 0, vs_constant_misses = 0, vs_constant_bypasses = 0;
    uint64_t vs_constant_bytes_avoided = 0, vs_constant_reserved_bytes_avoided = 0;
    // Logical packet preparation counts/extent reductions, including reuse hits;
    // these are not additive measurements of actual mapped writes avoided.
    uint64_t empty_vs_packets = 0, empty_ps_packets = 0;
    uint64_t empty_constant_bytes_avoided = 0, empty_constant_reserved_bytes_avoided = 0;
    uint64_t empty_layout_bypasses = 0;  // explicit empty, but linked-bytecode extent not proven
  };
  static const ConstantReuseStats& constant_reuse_stats();

  struct IndexDrawStats {
    uint64_t hardware_draws = 0, manual_eligible_draws = 0;
    uint64_t hardware_indices = 0, manual_eligible_indices = 0;
  };
  static const IndexDrawStats& index_draw_stats();  // cumulative, command processor thread only

  // Cumulative CP-thread lookup outcomes while key normalization is enabled.
  // These count Record/GetPipeline attempts, including ordinary fallback draws.
  struct PipelineKeyStats {
    uint64_t normalized_lookups = 0, changed_key_draws = 0;
    uint64_t ready_memo_hits = 0, ready_map_hits = 0;
    uint64_t pending_hits = 0, known_bad_hits = 0;
    uint64_t create_requests = 0;  // async creation successfully queued, not worker completions
  };
  static const PipelineKeyStats& pipeline_key_stats();

 private:
  ID3D12PipelineState* GetPipeline(
      const rex::graphics::d3d12::D3D12CommandProcessor::NativeDrawContext& context,
      const GuestState& state, bool root_cbv);

  std::string name_;
  bool initialized_ = false;
  bool direct_guest_reads_ = false;  // specialized bytecode; refuses non-null DrawArgs::asset_cache
  ID3D12Device* device_ = nullptr;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_cbv_signature_;
  Microsoft::WRL::ComPtr<ID3DBlob> vs_;
  Microsoft::WRL::ComPtr<ID3DBlob> ps_;
  NativePixelConstantReuse ps_constant_reuse_;
  NativeReadyPipelineReuse ready_pipeline_reuse_;
  static constexpr size_t kVertexConstantHeaderBytes = 688;
  // Record runs on the command processor thread. Rebuild every selected draw;
  // keep the scratch packet off its stack and never compare mapped upload RAM.
  std::array<uint8_t, kVertexConstantHeaderBytes + 256 * 16> vertex_constant_stage_;
  NativeVertexPacketDiagnostics vertex_packet_diagnostics_;
  NativeVertexConstantReuse vertex_constant_reuse_;
  bool pixel_buffer_unused_ = false;  // both linked blobs proven to have no used b2 byte
  bool legacy_depth_only_pixel_buffer_unused_ = false;  // original no-PS proof, unchanged
  bool empty_vertex_buffer_safe_ = false;  // both blobs have no used b1 variable beyond688
  bool empty_pixel_buffer_safe_ = false;   // both blobs have no used b2 variable beyond192
  // Pipeline cache key: target formats and GuestState as plain words. Optional
  // normalization removes only inputs unused by the original descriptor builder.
  struct PipelineKey {
    uint32_t words[20];
    bool operator==(const PipelineKey& other) const { return std::memcmp(words, other.words, sizeof words) == 0; }
  };
  struct PipelineKeyHash {
    size_t operator()(const PipelineKey& key) const {
      uint64_t h = 0x9E3779B97F4A7C15ull;
      for (uint32_t word : key.words) {
        h ^= word;
        h *= 0x100000001B3ull;
      }
      return static_cast<size_t>(h);
    }
  };
  std::unordered_map<PipelineKey, Microsoft::WRL::ComPtr<ID3D12PipelineState>, PipelineKeyHash> pipelines_;

  // Bumped from workers when a creation fails, to cap the diagnostic spew. Declared before the pending
  // map on purpose: that map's destructor blocks on the workers, so anything they touch has to outlive
  // it, and members die in reverse declaration order.
  std::atomic<uint32_t> failures_{0};

  // Creating one of these pipelines costs about 300 ms: the generated shaders run to tens of kilobytes
  // of DXBC and the driver optimises the whole thing. Doing that on the command processor thread froze
  // a play session for 62 of its 131 seconds across 784 pipelines. They are built on workers instead,
  // and the draw stays emulated until the result lands - the same shape as the shader library compile
  // queue. CreateGraphicsPipelineState is free-threaded, and the only pointers in the copied desc are
  // the root signature and the two shader blobs, all owned by this object for its whole life.
  std::unordered_map<PipelineKey, std::future<Microsoft::WRL::ComPtr<ID3D12PipelineState>>, PipelineKeyHash>
      pending_pipelines_;
  // Moves finished creations into pipelines_. Command processor thread only.
  void ReapPipelines();
  static uint32_t MaxPipelinesInFlight();
};

}  // namespace nb::gpu
