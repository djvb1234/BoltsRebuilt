// nb - our command processor: the seam where every de-emulation step and every native pass plugs in.
// Milestone 1 step 0 passes everything through to the SDK's D3D12 command processor and only counts
// frames, draws, resolves and shader loads, which is the parity metric between `gpu_plugin = "xenos"`
// and `gpu_plugin = "nb"`. On top of that: the frame inventory recorder (nb_cp_records.h) and the first
// native passes, taken over through the vendored backend's TryNativeDraw seam.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>

#include <rex/graphics/d3d12/command_processor.h>
#include <renderdoc_app.h>

#include "native/native_geometry_pass.h"
#include "native/native_gpu_upload_pool.h"
#include "native/native_shader_library.h"
#include "native/native_pass.h"
#include "native/native_asset_cache.h"
#include "native/native_cpu_affinity.h"
#include "native/native_texture_binding_cache.h"

namespace nb::gpu {

class NbGraphicsSystem;
class NativeFrameTrace;

class NbCommandProcessor : public rex::graphics::d3d12::D3D12CommandProcessor {
 public:
  NbCommandProcessor(NbGraphicsSystem* graphics_system, rex::system::KernelState* kernel_state);
  ~NbCommandProcessor();

  struct FrameStats {
    uint64_t frames = 0;
    uint64_t draws = 0;
    uint64_t copies = 0;
    uint64_t shaders_loaded = 0;
    uint64_t native_draws = 0;
  };
  FrameStats stats() const { return stats_; }
  const NativeTextureBindingCacheStats& native_texture_binding_cache_stats() const {
    return texture_binding_cache_.stats();
  }
  NativeReadyPairMemoStats native_pair_lookup_memo_stats() const {
    NativeReadyPairMemoStats total;
    for (const auto& library : shader_libraries_) {
      const auto& stats = library.pair_lookup_memo_stats();
      total.hits += stats.hits;
      total.misses += stats.misses;
      total.publications += stats.publications;
      total.resets += stats.resets;
    }
    return total;
  }

 protected:
  uint64_t NativeGpuTimingFrame() const override { return stats_.frames + 1; }
  void ShutdownContext() override;
  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;

  rex::graphics::Shader* LoadShader(rex::graphics::xenos::ShaderType shader_type,
                                    uint32_t guest_address, const uint32_t* host_address,
                                    uint32_t dword_count) override;

  bool IssueDraw(rex::graphics::xenos::PrimitiveType primitive_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool major_mode_explicit) override;

  bool IssueCopy() override;

  // The native-draw seam of the vendored backend: called with the guest draw's targets bound.
  bool TryNativeDraw(const NativeDrawContext& context) override;
  // The display-aspect seam of the vendored backend: nb_ultrawide's aspect replaces the video mode's.
  void OverrideDisplayAspect(uint32_t& display_width, uint32_t& display_height) override;

 private:
  // The ultrawide presenter state of the swap being issued. It is taken from nb.exe at the top of
  // IssueSwap, before the base class can return early, and read by OverrideDisplayAspect.
  bool present_ultrawide_ = false;
  uint32_t present_aspect_w_ = 0;
  uint32_t present_aspect_h_ = 0;
  // The presenter aspect last logged by OverrideDisplayAspect, so that each change is logged once.
  uint32_t logged_aspect_w_ = 0;
  uint32_t logged_aspect_h_ = 0;
  // DOF/fog pass draws refused at a draw resolution scale because an input is not a scaled resolve.
  uint64_t dof_scale_refusals_ = 0;
  using TextureBindingCache = NativeTextureBindingCache<
      rex::graphics::d3d12::D3D12Shader::TextureBinding,
      rex::graphics::d3d12::D3D12Shader::SamplerBinding>;
  // Pixel-shader float constant c<index> (the PS constant window starts at c256 of the register file).
  float PixelConstant(uint32_t index, uint32_t component) const;
  // Loads a draw's textures into the cache before any of them are resolved; without it a native draw
  // binds whatever an earlier emulated draw happened to request.
  void RequestDrawTextures(const NativeDrawContext& context);
  // Shader-side bindless index, or UINT32_MAX if unresolved. Only generated Tex2D callers may opt in
  // to the SDK's zero descriptor for a fetch with no base or mip data pages.
  uint32_t GuestTextureIndex(const rex::graphics::d3d12::D3D12Shader& shader, uint32_t fetch_constant,
                            bool allow_empty = false, uint32_t dimension = 2,
                            const TextureBindingCache::Plan* plan = nullptr);

  // The native-only constant pool, created on first use, or null while the experiment is off or the
  // device is not ready. Its slices are tagged with the command processor's frame index and it is
  // reclaimed at every swap with GetCompletedFrame(), matching the SDK's own constant pool.
  NativeConstantUploadPool* ConstantUploadPool();

  bool TryDemoPass(const NativeDrawContext& context);
  bool TryBloomCompositePass(const NativeDrawContext& context);
  bool TryBloomBlurPass(const NativeDrawContext& context);
  bool TrySpritePass(const NativeDrawContext& context);
  bool TryDofFogPass(const NativeDrawContext& context);
  // The generic pass: any (vertex, pixel) pair with a generated shader in the native shader library.
  bool TryGenericPass(const NativeDrawContext& context);
  bool TryGenericPassImpl(const NativeDrawContext& context);
  // Fills the geometry-pass inputs common to every guest-geometry pass: the vertex streams (the named
  // ones, or the guest vertex shader's first binding when `streams` is null), the index buffer, the
  // primitive expansion, the viewport mapping, the pipeline-selecting state, the alpha test and the
  // guest constants.
  bool PrepareGeometry(const NativeDrawContext& context, NativeGeometryPass::RootConstants& root,
                       NativeGeometryPass::GuestState& state, NativeGeometryPass::DrawArgs& args,
                       const std::vector<NativeShaderLibrary::Stream>* streams);
  void LogNativeDraw(const char* pass, const NativeDrawContext& context);
  // Opens and closes a RenderDoc capture around the guest frames named by nb_rdc_frames.
  void UpdateRenderDocCapture(uint64_t frame);

  FrameStats stats_{};
  NativeCpuAffinity cpu_affinity_;
  TextureBindingCache texture_binding_cache_;
  // Per-frame counters, reset at each swap; reported at a coarse cadence so the log stays
  // readable (frame 1, 2, 60, then every 600 frames).
  uint32_t draws_this_frame_ = 0;
  uint32_t copies_this_frame_ = 0;
  uint32_t shaders_this_frame_ = 0;
  uint32_t native_this_frame_ = 0;
  // CPU time (nanoseconds) since the last periodic frame log: the whole SDK IssueDraw (including our
  // seam) and the generic pass alone, to tell a CPU-bound native path from a GPU-bound one.
  uint64_t issue_draw_ns_ = 0;
  uint64_t generic_pass_ns_ = 0;
  uint64_t shader_load_ns_ = 0;
  uint64_t issue_copy_ns_ = 0;
  uint64_t issue_swap_ns_ = 0;
  NativeAssetCache native_asset_cache_;
  bool native_asset_cache_attempted_ = false;
  uint64_t frames_since_log_ = 0;
  uint64_t frame_wall_ns_ = 0;
  uint64_t interval_timed_frames_ = 0;
  uint64_t interval_draws_ = 0;
  uint64_t interval_native_draws_ = 0;
  uint64_t interval_snapshot_bytes_ = 0;
  std::chrono::steady_clock::time_point previous_swap_time_{};
  int asset_cache_mode_ = 0;
  int64_t perf_default_options_ = 0;
  // Frames the screenshot harness asked for (nb_screenshot_frames), parsed on the first swap.
  std::unordered_set<uint64_t> screenshot_frames_;
  bool screenshot_frames_parsed_ = false;
  // Frames to hand RenderDoc through its in-application API (nb_rdc_frames). Bracketing a whole guest
  // frame is the only reliable way to catch this game's passes: they are submitted from this thread
  // across several command lists, and a Present-to-Present capture usually lands between them.
  std::unordered_set<uint64_t> rdc_frames_;
  bool rdc_frames_parsed_ = false;
  bool rdc_capturing_ = false;
  RENDERDOC_API_1_0_1* rdc_api_ = nullptr;
  // Why draws with a pixel shader did not go through the generic pass, since the last periodic log.
  enum GenericRefusal : uint32_t {
    kRefusedNotInLibrary,  // the guest shader was not in the dump the library was generated from
    kRefusedDisabled,      // nb_native_shader_filter / _exclude
    kRefusedNotReady,      // compile still running
    kRefusedUnusable,      // compile or pipeline failed, or the stages do not fit together
    kRefusedPrepare,       // PrepareGeometry said no (streams, index buffer, primitive type)
    kRefusedTexture,       // a texture slot has no bound texture
    kRefusedSampler,       // no unambiguous SDK sampler for a native texture slot
    kRefusedRecord,        // pipeline or shared-memory residency
    kRefusedCount
  };
  uint64_t generic_refusals_[kRefusedCount] = {};
  std::unique_ptr<NativeFrameTrace> frame_trace_;
  // These counters reset at periodic log boundaries; refresh this baseline
  // after those resets so trace rows contain this frame's refusal counts.
  uint64_t frame_trace_previous_refusals_[kRefusedCount]{};
  uint64_t native_empty_texture_draws_ = 0;  // successful generic draws, since the periodic log
  uint64_t native_triangle_strip_draws_ = 0;
  uint64_t native_stacked_texture_draws_ = 0;
  uint64_t native_cube_texture_draws_ = 0;
  uint64_t native_extra_stream_draws_ = 0;

  // Window key events only enqueue toggle parity; cvars change on the CP thread
  // at a swap boundary, even when a complete key tap falls between two swaps.
  void PollDebugKeys();
  static constexpr uint32_t kNativeTogglePending = 1u;
  static constexpr uint32_t kAssetTogglePending = 2u;
  std::atomic<uint32_t> pending_debug_keys_{0};
  std::string native_toggle_bind_name_;
  std::string asset_toggle_bind_name_;

  // Classify unresolved slots separately from the empty fetches the generic pass can handle.
  enum TextureFailure : uint32_t {
    kTextureNoDescriptor,   // the cache has no bindless SRV for this binding at all
    kTextureUnexpectedNull, // no usable non-null view, and not an accepted empty fetch
    kTextureMixedSigns,     // needs both signed and unsigned views
    kTextureNoBinding,      // the shader declares no 2D binding for this fetch constant
    kTextureFailureCount
  };
  uint64_t texture_failures_[kTextureFailureCount] = {};
  // Which fetch dimensions the shader did declare when no 2D binding matched, indexed by
  // FetchOpDimension, so a cube or 3D fetch shows up as itself rather than as a mystery.
  uint64_t texture_failure_dimensions_[4] = {};

  // Pass 0, the proof: replaces the draws of one pixel shader (nb_native_demo_ps) with a pattern.
  NativeFullscreenPass demo_pass_;
  bool demo_pass_init_attempted_ = false;
  uint64_t demo_ps_hash_ = 0;
  bool demo_ps_hash_parsed_ = false;
  // Pass 1, the bloom composite (pixel shader D6B0AF19A166B630): our HLSL, the game's textures and
  // constants, drawn into the game's frame target.
  NativeFullscreenPass bloom_pass_;
  bool bloom_pass_init_attempted_ = false;
  // Pass 2, the bloom blur chain (pixel shader EDE50E25A39F4768): the 9-tap kernel that builds the
  // 320x180 .. 40x22 mips the composite sums.
  NativeFullscreenPass blur_pass_;
  bool blur_pass_init_attempted_ = false;
  // Pass 3, the sprite shader pair (vs E91536F60A69ABF3 / ps 54771EC87D07B28C): the first pass that draws
  // the guest's own vertices (2D quads through the game's transform constants).
  NativeGeometryPass sprite_pass_;
  bool sprite_pass_init_attempted_ = false;
  // Pass 4, the depth-of-field / fog composite (vs 67EBCAD85D59CEBF / ps 9DD9939DE449D096): reads the depth
  // buffer as depth and as raw 8888 (stencil byte = fog mask), blends toward the 320x180 blur by a
  // linearized-depth circle of confusion, fogs and desaturates, and writes the depth back.
  NativeGeometryPass dof_pass_;
  bool dof_pass_init_attempted_ = false;
  // Pass 5, the generic pass: generated shaders keyed by (vs, ps) ucode hash (nb_native_generic).
  // Independent lifetime/PSO caches for effective-sampler and no-asset-routing
  // specializations. Live geometry matching always selects an asset-capable form.
  NativeShaderLibrary shader_libraries_[4];
  bool shader_libraries_loaded_[4] = {};
  // Optional per-draw constant slices in CPU-writable video memory. Retained once created so a
  // benchmark window that turns the experiment off and on again does not rebuild its pages; the
  // pages themselves are released in ShutdownContext, after the native resources are idle.
  std::unique_ptr<NativeConstantUploadPool> constant_upload_pool_;
  bool constant_upload_pool_attempted_ = false;
  std::chrono::steady_clock::time_point start_time_ = std::chrono::steady_clock::now();
};

}  // namespace nb::gpu
