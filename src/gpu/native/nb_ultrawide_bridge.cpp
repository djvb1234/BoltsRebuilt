#include "native/nb_ultrawide_bridge.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace nb::gpu {
namespace {

using TakePresentAspectFn = bool (*)(uint32_t, uint32_t*, uint32_t*);
using DefaultScaleFn = bool (*)(uint32_t*, uint32_t*);

// nb.exe exports its ultrawide state. The plugin is loaded by it, so the process module is nb.exe.
template <typename Fn>
Fn Resolve(const char* name) {
#ifdef _WIN32
  // Through void*: FARPROC and the real signature are different function types.
  return reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(nullptr), name)));
#else
  (void)name;
  return nullptr;
#endif
}

}  // namespace

bool UltrawideTakePresentAspect(uint32_t frontbuffer, uint32_t* width, uint32_t* height) {
  static const auto take = Resolve<TakePresentAspectFn>("NbUltrawideTakePresentAspect2");
  return take && take(frontbuffer, width, height);
}

bool UltrawideDefaultScale(uint32_t* scale_x, uint32_t* scale_y) {
  static const auto query = Resolve<DefaultScaleFn>("NbUltrawideDefaultScale");
  return query && query(scale_x, scale_y);
}

}  // namespace nb::gpu
