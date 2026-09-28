#include "nb_graphics_system.h"

#include "nb_command_processor.h"

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(nb_shader_storage_background_load, false, "nb",
                    "Load the stored shaders and pipelines on the command processor thread without holding "
                    "the window and the game's start; guest GPU commands still run only after the load")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace nb::gpu {

std::string NbGraphicsSystem::name() const {
  return "nb-d3d12";
}

std::unique_ptr<rex::graphics::CommandProcessor> NbGraphicsSystem::CreateCommandProcessor() {
  return std::unique_ptr<rex::graphics::CommandProcessor>(
      new NbCommandProcessor(this, kernel_state()));
}

void NbGraphicsSystem::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                               uint32_t title_id, bool blocking) {
  if (blocking && REXCVAR_GET(nb_shader_storage_background_load)) {
    REXLOG_INFO("rexgpu-nb: shader storage: loading on the command processor thread without blocking");
    blocking = false;
  }
  D3D12GraphicsSystem::InitializeShaderStorage(cache_root, title_id, blocking);
}

}  // namespace nb::gpu
