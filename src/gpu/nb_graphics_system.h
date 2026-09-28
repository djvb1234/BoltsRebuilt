// nb - our GPU plugin's graphics system.
//
// Milestone 1: derives from the SDK's D3D12GraphicsSystem (compiled into this DLL from the
// SDK's BSD-3 sources) and only swaps in NbCommandProcessor, so the provider, presenter and
// overlays are exactly the xenos plugin's. The de-emulation steps live in the command
// processor and, later, in vendored backend files.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/d3d12/graphics_system.h>

namespace nb::gpu {

class NbGraphicsSystem : public rex::graphics::d3d12::D3D12GraphicsSystem {
 public:
  NbGraphicsSystem() = default;
  ~NbGraphicsSystem() override = default;

  std::string name() const override;

  // The SDK asks for a blocking load from inside a UI-thread task, before the
  // guest's main thread is resumed, so the window stops responding for the
  // whole load. nb_shader_storage_background_load makes it non-blocking. The
  // load still runs first on the command processor thread, ahead of any guest
  // command, and every submission still waits for its pipelines.
  void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id,
                               bool blocking) override;

 protected:
  std::unique_ptr<rex::graphics::CommandProcessor> CreateCommandProcessor() override;
};

}  // namespace nb::gpu
