/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

// nb: vendored (src/gpu/vendored/UPSTREAM.md). The SDK ships execute_unclipped_draw_vs_on_cpu off, and nb
// turns it on in nb.toml (docs/ultrawide.md, "The transfer fix"). A review of the vertex estimate found ways
// it could claim fewer EDRAM rows than a draw really covers. Each is guarded here, and every guard can only
// make a claim larger:
// - F2, render scale. The 1x rounding can miss a host row that a fractional bottom edge covers at scale.
// - F3, stale memory. Indices or vertices the GPU wrote (a resolve or memexport) are stale in guest memory.
// - F4, geometry the vertices do not describe: the fourth corner of a sheared rectangle, the width of lines,
//   and positions with w <= 0 or a non-finite result.
// A fallback returns the scissor-based claim, the one the SDK uses with the option off.

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdint>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/format/ucode.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw_extent_estimator.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ui/graphics_util.h>

REXCVAR_DEFINE_BOOL(execute_unclipped_draw_vs_on_cpu, false, "GPU",
                    "Execute unclipped draw vertex shader on CPU");

REXCVAR_DEFINE_BOOL(execute_unclipped_draw_vs_on_cpu_with_scissor, false, "GPU",
                    "Execute unclipped draw VS on CPU with scissor");

// DEFINE_bool(
//     execute_unclipped_draw_vs_on_cpu, true,
//     "Execute the vertex shader for draws with clipping disabled, primarily "
//     "screen-space draws (such as clears), on the CPU when possible to estimate "
//     "the extent of the EDRAM involved in the draw.\n"
//     "Enabling this may significantly improve GPU performance as otherwise up "
//     "to the entire EDRAM may be considered used in draws without clipping, "
//     "potentially resulting in spurious EDRAM range ownership transfer round "
//     "trips between host render targets.\n"
//     "Also, on hosts where certain render target formats have to be emulated in "
//     "a lossy way (for instance, 16-bit fixed-point via 16-bit floating-point), "
//     "this prevents corruption of other render targets located after the "
//     "current ones in the EDRAM by lossy range ownership transfers done for "
//     "those draws.",
//     "GPU");
// DEFINE_bool(
//     execute_unclipped_draw_vs_on_cpu_with_scissor, false,
//     "Don't restrict the usage of execute_unclipped_draw_vs_on_cpu to only "
//     "non-scissored draws (with the right and the bottom sides of the scissor "
//     "rectangle at 8192 or beyond) even though if the scissor rectangle is "
//     "present, it's usually sufficient for esimating the height of the render "
//     "target.\n"
//     "Enabling this may cause excessive processing of vertices on the CPU, as "
//     "some games draw rectangles (for their UI, for instance) without clipping, "
//     "but with a proper scissor rectangle.",
//     "GPU");

namespace rex::graphics {

namespace {

std::atomic<DrawExtentGpuWrittenQuery> g_gpu_written_query{nullptr};
std::atomic<void*> g_gpu_written_context{nullptr};

bool IsRangeGpuWritten(uint32_t start, uint32_t length) {
  const DrawExtentGpuWrittenQuery query = g_gpu_written_query.load(std::memory_order_acquire);
  return query && length && query(g_gpu_written_context.load(std::memory_order_acquire), start, length);
}

// Why an estimate fell back to the scissor. The first occurrence of each reason is logged with its vertex
// shader, so a scene that loses the estimate can be found.
enum class Fallback : uint32_t { kGpuWrittenData, kLines, kRectangleRestart, kBadPosition, kCount };
constexpr const char* kFallbackText[] = {
    "its indices or vertices are GPU-written, so stale in guest memory",
    "it draws lines, whose width the vertices do not describe",
    "a rectangle list restarts its primitive",
    "a position has w <= 0 or is not finite",
};
std::atomic<bool> g_fallback_logged[size_t(Fallback::kCount)];

uint32_t FallBack(Fallback reason, const Shader& vertex_shader) {
  if (!g_fallback_logged[size_t(reason)].exchange(true, std::memory_order_relaxed)) {
    REXGPU_INFO("Draw extent estimate: first fallback to the scissor because {} (vertex shader {:016X})",
                kFallbackText[size_t(reason)], vertex_shader.ucode_data_hash());
  }
  return xenos::kTexture2DCubeMaxWidthHeight;
}

}  // namespace

void SetDrawExtentGpuWrittenQuery(DrawExtentGpuWrittenQuery query, void* context) {
  // A reader loads the query, then the context: publish the context first and retract the query first.
  if (query) {
    g_gpu_written_context.store(context, std::memory_order_release);
    g_gpu_written_query.store(query, std::memory_order_release);
  } else {
    g_gpu_written_query.store(nullptr, std::memory_order_release);
    g_gpu_written_context.store(nullptr, std::memory_order_release);
  }
}

void DrawExtentEstimator::PositionYExportSink::Export(ucode::ExportRegister export_register,
                                                      const float* value, uint32_t value_mask) {
  if (export_register == ucode::ExportRegister::kVSPosition) {
    if (value_mask & 0b0010) {
      position_y_ = value[1];
    }
    if (value_mask & 0b1000) {
      position_w_ = value[3];
    }
  } else if (export_register == ucode::ExportRegister::kVSPointSizeEdgeFlagKillVertex) {
    if (value_mask & 0b0001) {
      point_size_ = value[0];
    }
    if (value_mask & 0b0100) {
      vertex_kill_ = rex::memory::Reinterpret<uint32_t>(value[2]);
    }
  }
}

uint32_t DrawExtentEstimator::EstimateVertexMaxY(const Shader& vertex_shader,
                                                 uint32_t draw_resolution_scale_y) {
  SCOPE_profile_cpu_f("gpu");

  const RegisterFile& regs = register_file_;

  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  if (!vgt_draw_initiator.num_indices) {
    return 0;
  }
  if (vgt_draw_initiator.source_select != xenos::SourceSelect::kDMA &&
      vgt_draw_initiator.source_select != xenos::SourceSelect::kAutoIndex) {
    // TODO(Triang3l): Support immediate indices.
    return xenos::kTexture2DCubeMaxWidthHeight;
  }

  // Not reproducing tessellation.
  if (xenos::IsMajorModeExplicit(vgt_draw_initiator.major_mode, vgt_draw_initiator.prim_type) &&
      regs.Get<reg::VGT_OUTPUT_PATH_CNTL>().path_select ==
          xenos::VGTOutputPath::kTessellationEnable) {
    return xenos::kTexture2DCubeMaxWidthHeight;
  }

  assert_true(vertex_shader.type() == xenos::ShaderType::kVertex);
  assert_true(vertex_shader.is_ucode_analyzed());
  if (!ShaderInterpreter::CanInterpretShader(vertex_shader)) {
    return xenos::kTexture2DCubeMaxWidthHeight;
  }

  // nb (F4): a line's width is not in its vertices.
  const xenos::PrimitiveType prim_type = vgt_draw_initiator.prim_type;
  if (prim_type == xenos::PrimitiveType::kLineList || prim_type == xenos::PrimitiveType::kLineStrip ||
      prim_type == xenos::PrimitiveType::kLineLoop) {
    return FallBack(Fallback::kLines, vertex_shader);
  }

  auto vgt_dma_size = regs.Get<reg::VGT_DMA_SIZE>();
  union {
    const void* index_buffer;
    const uint16_t* index_buffer_16;
    const uint32_t* index_buffer_32;
  };
  xenos::Endian index_endian = vgt_dma_size.swap_mode;
  if (vgt_draw_initiator.source_select == xenos::SourceSelect::kDMA) {
    uint32_t index_buffer_base = regs[XE_GPU_REG_VGT_DMA_BASE];
    uint32_t index_size_bytes;
    if (vgt_draw_initiator.index_size == xenos::IndexFormat::kInt16) {
      // Handle the index endianness to same way as the PrimitiveProcessor.
      if (index_endian == xenos::Endian::k8in32) {
        index_endian = xenos::Endian::k8in16;
      } else if (index_endian == xenos::Endian::k16in32) {
        index_endian = xenos::Endian::kNone;
      }
      index_buffer_base &= ~uint32_t(sizeof(uint16_t) - 1);
      index_size_bytes = sizeof(uint16_t);
    } else {
      assert_true(vgt_draw_initiator.index_size == xenos::IndexFormat::kInt32);
      index_buffer_base &= ~uint32_t(sizeof(uint32_t) - 1);
      index_size_bytes = sizeof(uint32_t);
    }
    // nb (F3): the loop below reads these indices from guest memory.
    const uint32_t indices_read =
        std::min(uint32_t(vgt_draw_initiator.num_indices), uint32_t(vgt_dma_size.num_words));
    if (IsRangeGpuWritten(index_buffer_base, indices_read * index_size_bytes)) {
      return FallBack(Fallback::kGpuWrittenData, vertex_shader);
    }
    index_buffer = memory_.TranslatePhysical(index_buffer_base);
  }
  // nb (F3): the interpreter reads vertices from anywhere in each fetch constant's buffer.
  for (const Shader::VertexBinding& binding : vertex_shader.vertex_bindings()) {
    const xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(binding.fetch_constant);
    if (IsRangeGpuWritten(uint32_t(fetch.address) << 2, uint32_t(fetch.size) << 2)) {
      return FallBack(Fallback::kGpuWrittenData, vertex_shader);
    }
  }
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  uint32_t reset_index = regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx;
  uint32_t index_offset = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
  uint32_t min_index = regs.Get<reg::VGT_MIN_VTX_INDX>().min_indx;
  uint32_t max_index = regs.Get<reg::VGT_MAX_VTX_INDX>().max_indx;

  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  float viewport_y_scale =
      pa_cl_vte_cntl.vport_y_scale_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE) : 1.0f;
  float viewport_y_offset =
      pa_cl_vte_cntl.vport_y_offset_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET) : 0.0f;

  int32_t point_vertex_min_diameter_float = 0;
  int32_t point_vertex_max_diameter_float = 0;
  float point_constant_radius_y = 0.0f;
  if (prim_type == xenos::PrimitiveType::kPointList) {
    auto pa_su_point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    point_vertex_min_diameter_float =
        rex::memory::Reinterpret<int32_t>(float(pa_su_point_minmax.min_size) * (2.0f / 16.0f));
    point_vertex_max_diameter_float =
        rex::memory::Reinterpret<int32_t>(float(pa_su_point_minmax.max_size) * (2.0f / 16.0f));
    point_constant_radius_y = float(regs.Get<reg::PA_SU_POINT_SIZE>().height) * (1.0f / 16.0f);
  }

  float max_y = -FLT_MAX;

  // nb (F4): a rectangle list gives three corners per rectangle, and the GPU makes the fourth as
  // a + b - c for whichever edge (a, b) is the diagonal. Each rectangle's three Y values are kept and the
  // highest of the three candidates is counted too. That adds nothing for an axis-aligned rectangle and
  // over-claims only for a sheared one.
  const bool rectangle_list = prim_type == xenos::PrimitiveType::kRectangleList;
  float rectangle_y[3] = {};
  uint32_t rectangle_corners = 0;

  shader_interpreter_.SetShader(vertex_shader);

  PositionYExportSink position_y_export_sink;
  shader_interpreter_.SetExportSink(&position_y_export_sink);
  auto fall_back_in_loop = [&](Fallback reason) {
    shader_interpreter_.SetExportSink(nullptr);
    return FallBack(reason, vertex_shader);
  };
  for (uint32_t i = 0; i < vgt_draw_initiator.num_indices; ++i) {
    const uint32_t rectangle_corner = i % 3;
    if (rectangle_corner == 0) {
      rectangle_corners = 0;
    }
    uint32_t vertex_index;
    if (vgt_draw_initiator.source_select == xenos::SourceSelect::kDMA) {
      if (i < vgt_dma_size.num_words) {
        if (vgt_draw_initiator.index_size == xenos::IndexFormat::kInt16) {
          vertex_index = index_buffer_16[i];
        } else {
          vertex_index = index_buffer_32[i];
        }
        // The Xenos only uses 24 bits of the index (reset_indx is 24-bit).
        vertex_index = xenos::GpuSwap(vertex_index, index_endian) & 0xFFFFFF;
      } else {
        vertex_index = 0;
      }
      if (pa_su_sc_mode_cntl.multi_prim_ib_ena && vertex_index == reset_index) {
        // nb (F4): a restart shifts the rectangles against i % 3.
        if (rectangle_list) {
          return fall_back_in_loop(Fallback::kRectangleRestart);
        }
        continue;
      }
    } else {
      assert_true(vgt_draw_initiator.source_select == xenos::SourceSelect::kAutoIndex);
      vertex_index = i;
    }
    vertex_index =
        std::min(max_index, std::max(min_index, (vertex_index + index_offset) & 0xFFFFFF));

    position_y_export_sink.Reset();

    shader_interpreter_.temp_registers()[0] = float(vertex_index);
    shader_interpreter_.Execute();

    if (position_y_export_sink.vertex_kill().has_value() &&
        (position_y_export_sink.vertex_kill().value() & ~(UINT32_C(1) << 31))) {
      continue;
    }
    if (!position_y_export_sink.position_y().has_value()) {
      continue;
    }
    float vertex_y = position_y_export_sink.position_y().value();
    if (!pa_cl_vte_cntl.vtx_xy_fmt) {
      if (!position_y_export_sink.position_w().has_value()) {
        continue;
      }
      const float vertex_w = position_y_export_sink.position_w().value();
      // nb (F4): with clipping off, a vertex at or behind the eye does not land where the divide puts it.
      if (!(vertex_w > 0.0f)) {
        return fall_back_in_loop(Fallback::kBadPosition);
      }
      vertex_y /= vertex_w;
    }

    vertex_y = vertex_y * viewport_y_scale + viewport_y_offset;
    // nb (F4): the max below would silently drop a NaN.
    if (!std::isfinite(vertex_y)) {
      return fall_back_in_loop(Fallback::kBadPosition);
    }

    if (prim_type == xenos::PrimitiveType::kPointList) {
      float point_radius_y;
      if (position_y_export_sink.point_size().has_value()) {
        // Vertex-specified diameter. Clamped effectively as a signed integer in
        // the hardware, -NaN, -Infinity ... -0 to the minimum, +Infinity, +NaN
        // to the maximum.
        point_radius_y = 0.5f * rex::memory::Reinterpret<float>(std::min(
                                    point_vertex_max_diameter_float,
                                    std::max(point_vertex_min_diameter_float,
                                             rex::memory::Reinterpret<int32_t>(
                                                 position_y_export_sink.point_size().value()))));
      } else {
        // Constant radius.
        point_radius_y = point_constant_radius_y;
      }
      vertex_y += point_radius_y;
    }

    // std::max is `a < b ? b : a`, thus in case of NaN, the first argument is
    // always returned - max_y, which is initialized to a normalized value.
    max_y = std::max(max_y, vertex_y);

    if (rectangle_list) {
      rectangle_y[rectangle_corner] = vertex_y;
      rectangle_corners |= UINT32_C(1) << rectangle_corner;
      if (rectangle_corners == 0b111) {
        max_y = std::max({max_y, rectangle_y[1] + rectangle_y[2] - rectangle_y[0],
                          rectangle_y[0] + rectangle_y[2] - rectangle_y[1],
                          rectangle_y[0] + rectangle_y[1] - rectangle_y[2]});
      }
    }
  }
  shader_interpreter_.SetExportSink(nullptr);

  int32_t max_y_24p8 = ui::FloatToD3D11Fixed16p8(max_y);
  // 16p8 range is -32768 to 32767+255/256, but it's stored as uint32_t here,
  // as 24p8, so overflowing up to -8388608 to 8388608+255/256 is safe. The
  // range of the window offset plus the half-pixel offset is -16384 to 16384.5,
  // so it's safe to add both - adding it will neither move the 16p8 clamping
  // bounds -32768 and 32767+255/256 into the 0...8192 screen space range, nor
  // cause 24p8 overflow.
  if (regs.Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero) {
    max_y_24p8 += 128;
  }
  if (pa_su_sc_mode_cntl.vtx_window_offset_enable) {
    max_y_24p8 += regs.Get<reg::PA_SC_WINDOW_OFFSET>().window_y_offset * 256;
  }
  // Top-left rule - .5 exclusive without MSAA, 1. exclusive with MSAA.
  // nb (F2): at a render scale the host has several rows per guest row. The ones centred early in a guest row
  // lie inside a fractional edge that the 1x rule rounds away. The ceiling (the MSAA rule) counts every guest
  // row the edge enters, whatever the host's row layout. On an integer edge, where Direct3D 9's own
  // rectangles end, it claims nothing extra.
  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  const bool round_up =
      rb_surface_info.msaa_samples != xenos::MsaaSamples::k1X || draw_resolution_scale_y > 1;
  return (uint32_t(std::max(int32_t(0), max_y_24p8)) + (round_up ? 255 : 127)) >> 8;
}

uint32_t DrawExtentEstimator::EstimateMaxY(bool try_to_estimate_vertex_max_y,
                                           const Shader& vertex_shader,
                                           uint32_t draw_resolution_scale_y) {
  SCOPE_profile_cpu_f("gpu");

  const RegisterFile& regs = register_file_;

  auto pa_sc_window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
  int32_t window_y_offset = pa_sc_window_offset.window_y_offset;

  // Scissor.
  auto pa_sc_window_scissor_br = regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
  int32_t scissor_bottom = int32_t(pa_sc_window_scissor_br.br_y);
  bool scissor_window_offset = !regs.Get<reg::PA_SC_WINDOW_SCISSOR_TL>().window_offset_disable;
  if (scissor_window_offset) {
    scissor_bottom += window_y_offset;
  }
  auto pa_sc_screen_scissor_br = regs.Get<reg::PA_SC_SCREEN_SCISSOR_BR>();
  scissor_bottom = std::min(scissor_bottom, int32_t(pa_sc_screen_scissor_br.br_y));
  uint32_t max_y = uint32_t(std::max(scissor_bottom, int32_t(0)));

  if (regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable) {
    // Actual extent from the vertices.
    if (try_to_estimate_vertex_max_y && REXCVAR_GET(execute_unclipped_draw_vs_on_cpu)) {
      bool estimate_vertex_max_y;
      if (REXCVAR_GET(execute_unclipped_draw_vs_on_cpu_with_scissor)) {
        estimate_vertex_max_y = true;
      } else {
        estimate_vertex_max_y = false;
        if (scissor_bottom >= xenos::kTexture2DCubeMaxWidthHeight) {
          // Handle just the usual special 8192x8192 case in Direct3D 9 - 8192
          // may be a normal render target height (80x8192 is well within the
          // EDRAM size, for instance), no need to process the vertices on the
          // CPU in this case.
          int32_t scissor_right = int32_t(pa_sc_window_scissor_br.br_x);
          if (scissor_window_offset) {
            scissor_right += pa_sc_window_offset.window_x_offset;
          }
          scissor_right = std::min(scissor_right, int32_t(pa_sc_screen_scissor_br.br_x));
          if (scissor_right >= xenos::kTexture2DCubeMaxWidthHeight) {
            estimate_vertex_max_y = true;
          }
        }
      }
      if (estimate_vertex_max_y) {
        max_y = std::min(max_y, EstimateVertexMaxY(vertex_shader, draw_resolution_scale_y));
      }
    }
  } else {
    // Viewport. Though the Xenos itself doesn't have an implicit viewport
    // scissor (it's set by Direct3D 9 when a viewport is used), on hosts, it
    // usually exists and can't be disabled.
    auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
    float viewport_bottom = 0.0f;
    // First calculate all the integer.0 or integer.5 offsetting exactly at full
    // precision.
    if (regs.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable) {
      viewport_bottom += float(window_y_offset);
    }
    if (regs.Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero) {
      viewport_bottom += 0.5f;
    }
    // Then apply the floating-point viewport offset.
    if (pa_cl_vte_cntl.vport_y_offset_ena) {
      viewport_bottom += regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET);
    }
    viewport_bottom += pa_cl_vte_cntl.vport_y_scale_ena
                           ? std::abs(regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE))
                           : 1.0f;
    // Using floor, or, rather, truncation (because maxing with zero anyway)
    // similar to how viewport scissoring behaves on real AMD, Intel and Nvidia
    // GPUs on Direct3D 12 (but not WARP), also like in
    // draw_util::GetHostViewportInfo.
    // max(0.0f, viewport_bottom) to drop NaN and < 0 - max picks the first
    // argument in the !(a < b) case (always for NaN), min as float (max_y is
    // well below 2^24) to safely drop very large values.
    max_y = uint32_t(std::min(float(max_y), std::max(0.0f, viewport_bottom)));
  }

  return max_y;
}

}  // namespace rex::graphics
