#include "nb_graphics_system.h"

#include "nb_command_processor.h"

namespace nb::gpu {

std::string NbGraphicsSystem::name() const {
  return "nb-d3d12";
}

std::unique_ptr<rex::graphics::CommandProcessor> NbGraphicsSystem::CreateCommandProcessor() {
  return std::unique_ptr<rex::graphics::CommandProcessor>(
      new NbCommandProcessor(this, kernel_state()));
}

}  // namespace nb::gpu
