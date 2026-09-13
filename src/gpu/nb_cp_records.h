// nb - CP-side draw and resolve records: the frame inventory (docs/plan.md, Phase 3 "inventory the
// frame first" and Phase 5 step 3) and the CP half of the draw-to-callstack join.
//
// One record per IssueDraw / IssueCopy on the command-processor thread, buffered per frame and written
// as JSON lines at the swap. Everything the native pass needs to recognise a draw is here: the ucode
// hashes of both shaders, the vertex fetch constants the vertex shader really binds (from its
// translated bindings, not a scan of all 96 slots), the texture fetch constants the pixel shader
// samples, the render-target state, depth and blend control, viewport and scissor.
//
// Origin: Kimi swarm 3 task X (nb_cp_records), reworked: exact fetch bindings, texture fetches,
// viewport/scissor/depth/blend state, periodic capture instead of every frame. The CP is one thread,
// so there is no locking; the recorder is inert unless nb_cp_records_enable is set.

#pragma once

#include <cstdint>
#include <cstdio>

#include <rex/graphics/register_file.h>
#include <rex/graphics/xenos.h>

namespace rex::graphics {
class Shader;
}

namespace nb::gpu {

struct NbCpRecord {
  uint64_t ts_ns = 0;
  uint64_t frame = 0;      // 1-based CP frame index (the frame the draw belongs to)
  uint32_t sequence = 0;   // per-frame draw counter (draws only)
  uint32_t copies_before = 0;  // IssueCopy events earlier in this frame
  uint32_t kind = 0;       // 0 = draw, 1 = copy, 2 = frame_stats
  uint32_t prim_type = 0;  // xenos::PrimitiveType
  uint32_t index_count = 0;
  uint32_t ib_guest_base = 0;  // 0 for non-indexed draws
  uint32_t ib_count = 0;
  uint32_t ib_format = 0;  // xenos::IndexFormat (0 = 16-bit, 1 = 32-bit)
  uint32_t major_mode_explicit = 0;
  uint64_t vs_ucode_hash = 0;
  uint64_t ps_ucode_hash = 0;
  // Render-target and raster state.
  uint32_t rb_surface_pitch = 0;
  uint32_t rb_surface_msaa = 0;
  uint32_t rb_color_base[4] = {};    // EDRAM tiles (12-bit)
  uint32_t rb_color_format[4] = {};  // xenos::ColorRenderTargetFormat
  uint32_t rb_depth_base = 0;
  uint32_t rb_depth_format = 0;
  uint32_t rb_edram_mode = 0;   // RB_MODECONTROL.edram_mode (4 = color+depth, 5 = copy)
  uint32_t rb_depthcontrol = 0;
  uint32_t rb_blendcontrol0 = 0;
  uint32_t rb_colorcontrol = 0;
  uint32_t pa_su_sc_mode_cntl = 0;
  uint32_t pa_cl_vte_cntl = 0;
  float vport_xscale = 0, vport_xoffset = 0, vport_yscale = 0, vport_yoffset = 0;
  uint32_t scissor_tl = 0, scissor_br = 0;  // raw PA_SC_WINDOW_SCISSOR_TL/BR
  // Vertex fetch constants bound by the vertex shader (up to 8 bindings).
  struct VFetch {
    uint32_t fetch_constant, base, size_bytes, stride_words;
  };
  uint32_t vfetch_count = 0;
  VFetch vfetch[8] = {};
  // Texture fetch constants sampled by the pixel shader (up to 8 bindings).
  struct TFetch {
    uint32_t fetch_constant, base, width, height, format, dimension, mip_address;
  };
  uint32_t tfetch_count = 0;
  TFetch tfetch[8] = {};
};

class NbCpRecorder {
 public:
  static NbCpRecorder& Get() {
    static NbCpRecorder instance;
    return instance;
  }

  // True when frame `frame` (1-based) is one the recorder captures; cheap, so the command processor
  // can skip the record calls entirely on other frames.
  bool WantsFrame(uint64_t frame) const;

  // IndexBufferInfo is a protected nested type of CommandProcessor, so the caller unpacks it.
  void RecordDraw(uint64_t frame, rex::graphics::xenos::PrimitiveType prim_type, uint32_t index_count,
                  uint32_t ib_guest_base, uint32_t ib_count, uint32_t ib_format, bool major_mode_explicit,
                  const rex::graphics::RegisterFile* regs, const rex::graphics::Shader* vs,
                  const rex::graphics::Shader* ps);

  void RecordCopy(uint64_t frame, const rex::graphics::RegisterFile* regs, const rex::graphics::Shader* vs,
                  const rex::graphics::Shader* ps);

  // Called after the swap of frame `frame`: writes the buffered records and resets the counters.
  void EndFrameAndFlush(uint64_t frame);

  uint64_t dropped() const { return dropped_; }

 private:
  NbCpRecorder() = default;
  ~NbCpRecorder();

  NbCpRecord* NextSlot();
  void FillState(NbCpRecord* rec, const rex::graphics::RegisterFile* regs, const rex::graphics::Shader* vs,
                 const rex::graphics::Shader* ps);
  bool OpenFile();
  void WriteRecord(const NbCpRecord& rec);

  static constexpr size_t kFrameBufferMax = 65536;
  NbCpRecord* buffer_ = nullptr;
  size_t buffered_ = 0;
  uint32_t sequence_ = 0;
  uint32_t copies_ = 0;
  uint64_t dropped_ = 0;
  FILE* file_ = nullptr;
  bool open_attempted_ = false;
};

}  // namespace nb::gpu
