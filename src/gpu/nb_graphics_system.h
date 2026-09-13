// nb - our GPU plugin's graphics system.
//
// Milestone 1: derives from the SDK's D3D12GraphicsSystem (compiled into this DLL from the
// SDK's BSD-3 sources) and only swaps in NbCommandProcessor, so the provider, presenter and
// overlays are exactly the xenos plugin's. The de-emulation steps live in the command
// processor and, later, in vendored backend files.

#pragma once

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

 protected:
  std::unique_ptr<rex::graphics::CommandProcessor> CreateCommandProcessor() override;
};

}  // namespace nb::gpu
