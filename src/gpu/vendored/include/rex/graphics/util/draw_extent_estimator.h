#pragma once
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

// nb: vendored from the SDK for guards on the vertex estimate (src/gpu/vendored/UPSTREAM.md). The SDK's
// RenderTargetCache, whose header is not vendored, holds this class by value. So only functions were added
// here, and the class layout is still the SDK's.

#include <cstdint>
#include <optional>

#include <rex/graphics/pipeline/shader/interpreter.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/register_file.h>
#include <rex/memory.h>

namespace rex::graphics {

// nb: whether the GPU holds newer data than guest memory anywhere in a guest physical range. That is the
// case when a resolve or a memexport wrote it and no CPU write has invalidated it since. The vertex estimate
// reads indices and vertices from guest memory, so it falls back to the scissor on such a range. The D3D12
// render target cache registers its shared memory here. With nothing registered, no range counts as
// GPU-written.
using DrawExtentGpuWrittenQuery = bool (*)(void* context, uint32_t start, uint32_t length);
void SetDrawExtentGpuWrittenQuery(DrawExtentGpuWrittenQuery query, void* context);

class DrawExtentEstimator {
 public:
  DrawExtentEstimator(const RegisterFile& register_file, const memory::Memory& memory)
      : register_file_(register_file),
        memory_(memory),
        shader_interpreter_(register_file, memory) {}

  // The shader must have its ucode analyzed.
  // nb: draw_resolution_scale_y is host rows per guest row. At 1 the rounding is the SDK's.
  uint32_t EstimateVertexMaxY(const Shader& vertex_shader, uint32_t draw_resolution_scale_y = 1);
  uint32_t EstimateMaxY(bool try_to_estimate_vertex_max_y, const Shader& vertex_shader,
                        uint32_t draw_resolution_scale_y = 1);

 private:
  class PositionYExportSink : public ShaderInterpreter::ExportSink {
   public:
    void Export(ucode::ExportRegister export_register, const float* value,
                uint32_t value_mask) override;

    void Reset() {
      position_y_.reset();
      position_w_.reset();
      point_size_.reset();
      vertex_kill_.reset();
    }

    const std::optional<float>& position_y() const { return position_y_; }
    const std::optional<float>& position_w() const { return position_w_; }
    const std::optional<float>& point_size() const { return point_size_; }
    const std::optional<uint32_t>& vertex_kill() const { return vertex_kill_; }

   private:
    std::optional<float> position_y_;
    std::optional<float> position_w_;
    std::optional<float> point_size_;
    std::optional<uint32_t> vertex_kill_;
  };

  const RegisterFile& register_file_;
  const memory::Memory& memory_;

  ShaderInterpreter shader_interpreter_;
};

}  // namespace rex::graphics
