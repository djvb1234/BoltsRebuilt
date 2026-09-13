// The plugin's view of nb.exe's ultrawide setting. src/ultrawide/nb_ultrawide.cpp exports the functions,
// and docs/ultrawide.md describes the feature. An nb.exe without the exports leaves the plugin at 16:9.
#pragma once

#include <cstdint>

namespace nb::gpu {

// Call once per swap packet, before the swap is issued, with the frontbuffer address the packet carries. It
// returns the display aspect for the frame that packet presents, and false to keep the video mode's aspect.
bool UltrawideTakePresentAspect(uint32_t frontbuffer, uint32_t* width, uint32_t* height);
// The render scale nb_ultrawide asks for when no draw resolution scale is configured. Returns false for none.
bool UltrawideDefaultScale(uint32_t* scale_x, uint32_t* scale_y);

}  // namespace nb::gpu
