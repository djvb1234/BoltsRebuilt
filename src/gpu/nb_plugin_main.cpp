/**
 * @file        gpu/nb_plugin_main.cpp
 * @brief       rexgpu-nb plugin entry points.
 *
 * Derived from the RexGlue SDK's src/graphics/plugin_main.cpp:
 *   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>, BSD 3-Clause License
 *   (see the SDK's LICENSE file). Modifications for nb: plugin name, D3D12 only.
 *
 * Milestone 1 step 0: hand the host the SDK's own D3D12GraphicsSystem so `gpu_plugin = "nb"`
 * renders identically to `gpu_plugin = "xenos"`. The NbGraphicsSystem subclass replaces this
 * once the de-emulation steps begin.
 */

#include <string_view>

#include <rex/logging.h>
#include <rex/system/gpu_plugin.h>

#include "nb_graphics_system.h"

extern "C" REX_GPU_PLUGIN_EXPORT uint32_t rex_gpu_abi_version(void) {
  return rex::system::kGpuPluginAbiVersion;
}

extern "C" REX_GPU_PLUGIN_EXPORT rex::system::IGraphicsSystem* rex_gpu_create(
    uint32_t abi_version, const rex::system::GpuCreateInfo* info) {
  if (abi_version != rex::system::kGpuPluginAbiVersion) {
    REXLOG_ERROR("rexgpu-nb: host requested ABI {}, plugin is ABI {}", abi_version,
                 rex::system::kGpuPluginAbiVersion);
    return nullptr;
  }
  if (!info || info->struct_size < sizeof(rex::system::GpuCreateInfo)) {
    REXLOG_ERROR("rexgpu-nb: invalid GpuCreateInfo");
    return nullptr;
  }

  std::string_view backend = info->backend ? info->backend : "any";
  if (backend != "any" && backend != "d3d12") {
    REXLOG_ERROR("rexgpu-nb: requested backend '{}' is not compiled into this plugin", backend);
    return nullptr;
  }

  REXLOG_INFO("rexgpu-nb: creating NbGraphicsSystem (milestone 1 step 0: xenos clone with counters)");
  return new nb::gpu::NbGraphicsSystem();
}
